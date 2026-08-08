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

struct Frame {
  uint8_t type{0};
  std::vector<uint8_t> payload;  // bytes between type and CRC
  std::vector<uint8_t> raw;      // full frame incl. 9B..9D, for verbatim forwarding
};

// Byte-driven parser. len counts type+payload+crc+terminator (everything after len).
// Emits only CRC-valid, 9D-terminated frames; anything else resyncs on next 9B.
class Parser {
 public:
  // Returns true when `out` holds a complete valid frame.
  bool feed(uint8_t b, Frame &out) {
    switch (state_) {
      case State::SYNC:
        if (b == 0x9B) { buf_.assign(1, 0x9B); state_ = State::LEN; }
        return false;
      case State::LEN:
        // Sane length: type+crc2+9D = 4 minimum, cap payload at 8 bytes.
        if (b < 4 || b > 12) { state_ = State::SYNC; return false; }
        len_ = b;
        buf_.push_back(b);
        state_ = State::BODY;
        return false;
      case State::BODY:
        buf_.push_back(b);
        if (buf_.size() < static_cast<size_t>(len_) + 2) return false;
        state_ = State::SYNC;
        return finish_(out);
    }
    return false;
  }

 private:
  bool finish_(Frame &out) {
    // buf_ = 9B len type payload... crc_hi crc_lo 9D
    if (buf_.back() != 0x9D) return false;
    size_t n = buf_.size();
    uint16_t crc = crc16(buf_.data() + 1, n - 4);  // over [len, type, payload]
    if (buf_[n - 3] != (crc >> 8) || buf_[n - 2] != (crc & 0xFF)) return false;
    out.type = buf_[2];
    out.payload.assign(buf_.begin() + 3, buf_.end() - 3);
    out.raw = buf_;
    return true;
  }

  enum class State { SYNC, LEN, BODY } state_{State::SYNC};
  uint8_t len_{0};
  std::vector<uint8_t> buf_;
};

}  // namespace desk_mitm
