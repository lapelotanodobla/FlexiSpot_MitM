#include "../components/desk_mitm/protocol.h"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace desk_mitm;

static void test_crc16() {
  // Vectors straight from captures/2026-08-08-session1.log
  const uint8_t poll[] = {0x04, 0x11};
  assert(crc16(poll, 2) == 0x7CC3);
  const uint8_t idle[] = {0x06, 0x02, 0x00, 0x00};
  assert(crc16(idle, 4) == 0x6CA1);
  const uint8_t disp[] = {0x07, 0x12, 0x07, 0xEF, 0x6D};
  assert(crc16(disp, 5) == 0xA4A8);
}

static void test_parser() {
  Parser p;
  Frame f;
  // Clean poll frame parses.
  const uint8_t poll[] = {0x9B, 0x04, 0x11, 0x7C, 0xC3, 0x9D};
  int emitted = 0;
  for (uint8_t b : poll)
    if (p.feed(b, f)) emitted++;
  assert(emitted == 1 && f.type == 0x11 && f.payload.empty());

  // Garbage prefix (wake transients) is skipped; frame still parses.
  const uint8_t noisy[] = {0x00, 0xFE, 0x9B, 0x06, 0x02, 0x01, 0x00, 0xFC, 0xA0, 0x9D};
  emitted = 0;
  for (uint8_t b : noisy)
    if (p.feed(b, f)) emitted++;
  assert(emitted == 1 && f.type == 0x02);
  assert(f.payload.size() == 2 && f.payload[0] == 0x01 && f.payload[1] == 0x00);

  // Corrupted CRC (captured power-down tail had 42:FD instead of 42:9D) is dropped.
  const uint8_t bad[] = {0x9B, 0x04, 0x13, 0xBD, 0x42, 0xFD};
  emitted = 0;
  for (uint8_t b : bad)
    if (p.feed(b, f)) emitted++;
  assert(emitted == 0);

  // Parser recovers after garbage: next clean frame parses.
  const uint8_t disp[] = {0x9B, 0x07, 0x12, 0x07, 0xEF, 0x6D, 0xA4, 0xA8, 0x9D};
  emitted = 0;
  for (uint8_t b : disp)
    if (p.feed(b, f)) emitted++;
  assert(emitted == 1 && f.type == 0x12 && f.payload.size() == 3);

  // raw() returns the full frame bytes for verbatim forwarding.
  assert(f.raw.size() == 9 && f.raw[0] == 0x9B && f.raw[8] == 0x9D);
}

static void test_height() {
  // Captured vectors: 07:EF:6D = "79.5", 07:ED:3F = "75.0"
  const uint8_t h795[] = {0x07, 0xEF, 0x6D};
  assert(std::fabs(decode_height(h795) - 79.5f) < 0.01f);
  const uint8_t h750[] = {0x07, 0xED, 0x3F};
  assert(std::fabs(decode_height(h750) - 75.0f) < 0.01f);
  // 3-digit, no decimal point: 06:5B:06 = "121"
  const uint8_t h121[] = {0x06, 0x5B, 0x06};
  assert(std::fabs(decode_height(h121) - 121.0f) < 0.01f);
  // M-mode prompt "S- " is not a height.
  const uint8_t prompt[] = {0x6D, 0x40, 0x00};
  assert(std::isnan(decode_height(prompt)));
}

static void test_build_reply() {
  // Idle reply must byte-match the captured keypad frame.
  auto idle = build_key_reply(0x00);
  const uint8_t expect[] = {0x9B, 0x06, 0x02, 0x00, 0x00, 0x6C, 0xA1, 0x9D};
  assert(idle.size() == 8);
  for (int i = 0; i < 8; i++) assert(idle[i] == expect[i]);
  // Up reply matches captured 9B:06:02:01:00:FC:A0:9D.
  auto up = build_key_reply(0x01);
  assert(up[3] == 0x01 && up[5] == 0xFC && up[6] == 0xA0);
}

static void test_key_state() {
  KeyState k;
  // Default idle.
  assert(k.current(1000) == 0x00);
  // Real keys pass through.
  k.set_real(0x01);
  assert(k.current(1000) == 0x01);
  // Injection REPLACES real keys (never ORs).
  k.inject(0x04, 150, 1000);
  assert(k.current(1010) == 0x04);
  // Injection expires by deadline even with no further events.
  assert(k.current(1200) == 0x01);
  // stop() clears injection immediately.
  k.inject(0x04, 150, 2000);
  k.stop();
  assert(k.current(2010) == 0x01);
  // clear_all releases everything (fail-safe path).
  k.clear_all();
  assert(k.current(2020) == 0x00);
  // Invalid masks are rejected: multi-bit and opposing directions never emitted.
  assert(!k.inject(0x03, 150, 3000));
  assert(!k.inject(0xC0, 150, 3000));
  assert(k.current(3010) == 0x00);
  // Valid single-key masks accepted.
  assert(k.inject(0x10, 150, 4000));
  assert(k.current(4010) == 0x10);
}

// Simulated desk: 2.2 cm/s while a key is held, 0.4 cm coast after release,
// display quantized to 0.1 cm. Matches measured constants in the findings docs.
struct SimDesk {
  float h;
  float coast_left{0};
  int dir{0};
  void step(uint8_t mask, uint32_t dt_ms) {
    const float v = 2.2f * dt_ms / 1000.0f;
    if (mask == 0x01) { h += v; dir = 1; coast_left = 0.4f; }
    else if (mask == 0x02) { h -= v; dir = -1; coast_left = 0.4f; }
    else if (coast_left > 0) {
      float c = v < coast_left ? v : coast_left;
      h += dir * c;
      coast_left -= c;
    }
  }
  float display() const { return roundf(h * 10) / 10; }
};

// Drive controller against sim until it goes idle or 60 simulated seconds pass.
static void run_sim(MoveController &mc, SimDesk &sim, uint32_t &now) {
  const uint32_t deadline = now + 60000;
  while (mc.active() && now < deadline) {
    uint8_t mask = mc.update(sim.display(), now);
    sim.step(mask, 10);
    now += 10;
  }
}

static void test_move_controller_happy() {
  MoveConfig cfg;  // defaults from the spec
  {  // long up move lands within deadband
    MoveController mc(cfg);
    SimDesk sim{75.0f};
    uint32_t now = 1000;
    assert(mc.move_to(110.0f, sim.display(), now));
    run_sim(mc, sim, now);
    assert(!mc.active());
    assert(mc.end_reason() == MoveEnd::DONE);
    assert(std::fabs(sim.display() - 110.0f) <= cfg.deadband + 0.001f);
  }
  {  // long down move
    MoveController mc(cfg);
    SimDesk sim{110.0f};
    uint32_t now = 1000;
    assert(mc.move_to(75.0f, sim.display(), now));
    run_sim(mc, sim, now);
    assert(mc.end_reason() == MoveEnd::DONE);
    assert(std::fabs(sim.display() - 75.0f) <= cfg.deadband + 0.001f);
  }
  {  // early release: key must drop out BEFORE the display reaches the target
    MoveController mc(cfg);
    SimDesk sim{80.0f};
    uint32_t now = 1000;
    mc.move_to(90.0f, sim.display(), now);
    float last_commanded_h = NAN;
    while (mc.active() && now < 61000) {
      uint8_t mask = mc.update(sim.display(), now);
      if (mask != 0) last_commanded_h = sim.display();
      sim.step(mask, 10);
      now += 10;
    }
    assert(mc.end_reason() == MoveEnd::DONE);
    assert(last_commanded_h < 90.0f);  // released below target, coast covered the rest
  }
  {  // target outside range is clamped to the rail
    MoveConfig c2;
    c2.min_height = 60.0f;
    c2.max_height = 121.0f;
    MoveController mc(c2);
    SimDesk sim{119.0f};
    uint32_t now = 1000;
    assert(mc.move_to(300.0f, sim.display(), now));
    run_sim(mc, sim, now);
    assert(std::fabs(sim.display() - 121.0f) <= c2.deadband + 0.001f);
  }
  {  // NaN target rejected outright
    MoveController mc(cfg);
    assert(!mc.move_to(NAN, 75.0f, 1000));
    assert(!mc.active());
  }
}

static void test_move_controller_failures() {
  MoveConfig cfg;
  {  // abort mid-move: goes idle, outputs 0
    MoveController mc(cfg);
    SimDesk sim{75.0f};
    uint32_t now = 1000;
    mc.move_to(110.0f, sim.display(), now);
    (void) mc.update(sim.display(), now);
    mc.abort();
    assert(!mc.active());
    assert(mc.end_reason() == MoveEnd::ABORTED);
    assert(mc.update(sim.display(), now + 10) == 0);
  }
  {  // stall: height frozen while commanding motion
    MoveController mc(cfg);
    uint32_t now = 1000;
    mc.move_to(110.0f, 75.0f, now);
    while (mc.active() && now < 20000) {
      (void) mc.update(75.0f, now);  // desk never moves
      now += 10;
    }
    assert(mc.end_reason() == MoveEnd::STALL);
    assert(now - 1000 <= cfg.stall_ms + 100);
  }
  {  // timeout: motion keeps happening but never converges (stall never trips)
    MoveConfig slow = cfg;
    slow.stall_ms = 10000000;  // disable stall for this case
    MoveController mc(slow);
    uint32_t now = 1000;
    float h = 75.0f;
    bool flip = false;
    mc.move_to(110.0f, h, now);
    while (mc.active() && now < 60000) {
      (void) mc.update(h, now);
      h += flip ? 0.1f : -0.1f;  // jitter in place forever
      flip = !flip;
      now += 10;
    }
    assert(mc.end_reason() == MoveEnd::TIMEOUT);
  }
  {  // tap limit: deadband unreachably tight -> gives up after max_taps
    MoveConfig tight = cfg;
    tight.deadband = 0.001f;  // display quantizes to 0.1, can't ever satisfy
    MoveController mc(tight);
    SimDesk sim{75.0f};
    uint32_t now = 1000;
    mc.move_to(90.05f, sim.display(), now);
    run_sim(mc, sim, now);
    assert(mc.end_reason() == MoveEnd::TAP_LIMIT);
  }
  {  // NaN current at start: waits, then proceeds once height appears
    MoveController mc(cfg);
    uint32_t now = 1000;
    mc.move_to(80.0f, NAN, now);
    assert(mc.update(NAN, now + 100) == 0);  // holds still while blind
    SimDesk sim{75.0f};
    now += 500;  // height arrives 500ms in (post-wake)
    run_sim(mc, sim, now);
    assert(mc.end_reason() == MoveEnd::DONE);
  }
}

int main() {
  test_crc16();
  test_parser();
  test_height();
  test_build_reply();
  test_key_state();
  test_move_controller_happy();
  test_move_controller_failures();
  printf("all tests passed\n");
  return 0;
}
