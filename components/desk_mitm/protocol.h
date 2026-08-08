#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>
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

// 7-segment glyph -> digit value, ignoring the decimal-point bit (bit 7). -1 = not a digit.
inline int seg_digit(uint8_t b) {
  switch (b & 0x7F) {
    case 0x3F: return 0; case 0x06: return 1; case 0x5B: return 2;
    case 0x4F: return 3; case 0x66: return 4; case 0x6D: return 5;
    case 0x7D: return 6; case 0x07: return 7; case 0x7F: return 8;
    case 0x6F: return 9; default: return -1;
  }
}

// Decode a type-12 payload (3 display bytes) to height. NAN when not numeric
// (M-mode "S-" prompt, blanks). Middle-digit decimal point => XX.X, else XXX.
inline float decode_height(const uint8_t *p) {
  int d0 = seg_digit(p[0]), d1 = seg_digit(p[1]), d2 = seg_digit(p[2]);
  if (d0 < 0 || d1 < 0 || d2 < 0) return NAN;
  if (p[1] & 0x80) return d0 * 10.0f + d1 + d2 * 0.1f;
  return d0 * 100.0f + d1 * 10.0f + d2 * 1.0f;
}

// Build a keypad type-02 reply frame for a key bitmask.
inline std::vector<uint8_t> build_key_reply(uint8_t keys) {
  std::vector<uint8_t> f = {0x9B, 0x06, 0x02, keys, 0x00};
  uint16_t crc = crc16(f.data() + 1, 4);
  f.push_back(crc >> 8);
  f.push_back(crc & 0xFF);
  f.push_back(0x9D);
  return f;
}

// Local key-state model. Injection replaces physical keys for its bounded
// window; fail-safe callers use clear_all() (poll timeout, CRC storm, type-13,
// PIN20 fall, reboot paths are the ESPHome layer's responsibility).
class KeyState {
 public:
  void set_real(uint8_t mask) { real_ = mask; }

  // Only idle or a single documented key may be injected.
  bool inject(uint8_t mask, uint32_t duration_ms, uint32_t now_ms) {
    static constexpr uint8_t VALID[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20};
    bool ok = false;
    for (uint8_t v : VALID) ok |= (mask == v);
    if (!ok) return false;
    inj_ = mask;
    inj_deadline_ = now_ms + duration_ms;
    inj_active_ = true;
    return true;
  }

  void stop() { inj_active_ = false; }
  void clear_all() { inj_active_ = false; real_ = 0; }

  uint8_t current(uint32_t now_ms) {
    if (inj_active_ && now_ms >= inj_deadline_) inj_active_ = false;
    return inj_active_ ? inj_ : real_;
  }

  bool injection_active(uint32_t now_ms) {
    if (inj_active_ && now_ms >= inj_deadline_) inj_active_ = false;
    return inj_active_;
  }

 private:
  uint8_t real_{0}, inj_{0};
  uint32_t inj_deadline_{0};
  bool inj_active_{false};
};

}  // namespace desk_mitm
