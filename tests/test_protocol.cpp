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

int main() {
  test_crc16();
  test_parser();
  printf("all tests passed\n");
  return 0;
}
