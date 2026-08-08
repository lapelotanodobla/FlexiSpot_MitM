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

int main() {
  test_crc16();
  test_parser();
  test_height();
  test_build_reply();
  test_key_state();
  printf("all tests passed\n");
  return 0;
}
