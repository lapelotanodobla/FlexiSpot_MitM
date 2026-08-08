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

int main() {
  test_crc16();
  printf("all tests passed\n");
  return 0;
}
