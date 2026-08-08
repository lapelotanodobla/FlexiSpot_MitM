#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace desk_mitm {

// CRC-16/MODBUS (init 0xFFFF, poly 0xA001 reflected).
inline uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

}  // namespace desk_mitm
