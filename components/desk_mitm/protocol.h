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

struct MoveConfig {
  float min_height{60.0f}, max_height{121.0f};
  float coast_margin{0.7f}, deadband{0.3f};
  uint32_t settle_ms{1200}, move_timeout_ms{30000}, stall_ms{2000}, tap_ms{60};
  uint8_t max_taps{5};
};

enum class MoveEnd : uint8_t { NONE, DONE, TAP_LIMIT, TIMEOUT, STALL, ABORTED };

// Closed-loop move engine. Pure logic: tick update() with the latest decoded
// height; it returns the key mask to emit right now (0 = idle). Never touches
// hardware; the glue owns wake, injection and abort triggers.
class MoveController {
 public:
  explicit MoveController(const MoveConfig &cfg) : cfg_(cfg) {}
  void set_config(const MoveConfig &cfg) { cfg_ = cfg; }

  // Starts a move. Target is clamped to [min_height, max_height]; NaN rejected.
  // `current` may be NAN (height not yet known) — COARSE waits for frames.
  bool move_to(float target, float current, uint32_t now) {
    if (std::isnan(target)) return false;
    if (target < cfg_.min_height) target = cfg_.min_height;
    if (target > cfg_.max_height) target = cfg_.max_height;
    target_ = target;
    start_ms_ = now;
    last_h_ = current;
    last_h_change_ = now;
    taps_ = 0;
    end_ = MoveEnd::NONE;
    state_ = State::COARSE;
    return true;
  }

  void abort() {
    if (state_ != State::IDLE) end_ = MoveEnd::ABORTED;
    state_ = State::IDLE;
  }

  bool active() const { return state_ != State::IDLE; }
  MoveEnd end_reason() const { return end_; }
  float target() const { return target_; }
  // Direction of the current move: +1 up, -1 down, 0 idle. For cover operation state.
  int direction(float current) const {
    if (state_ == State::IDLE || std::isnan(current)) return 0;
    return target_ > current ? 1 : -1;
  }

  uint8_t update(float h, uint32_t now) {
    if (state_ == State::IDLE) return 0;
    if (now - start_ms_ > cfg_.move_timeout_ms) return end_with_(MoveEnd::TIMEOUT);
    switch (state_) {
      case State::COARSE: {
        if (std::isnan(h)) {
          // Height unknown (e.g. just woke): hold still, wait for frames.
          // The stall guard below uses last_h_change_, armed at move start.
          if (now - last_h_change_ > cfg_.stall_ms) return end_with_(MoveEnd::STALL);
          return 0;
        }
        if (std::isnan(last_h_) || h != last_h_) { last_h_ = h; last_h_change_ = now; }
        float err = target_ - h;
        if (std::fabs(err) <= cfg_.coast_margin) {
          state_ = State::SETTLE;
          settle_until_ = now + cfg_.settle_ms;
          return 0;
        }
        // Stall only matters while we are commanding motion and it isn't happening.
        if (now - last_h_change_ > cfg_.stall_ms) return end_with_(MoveEnd::STALL);
        return err > 0 ? 0x01 : 0x02;
      }
      case State::SETTLE:
        if (now < settle_until_) return 0;
        state_ = State::FINE;
        [[fallthrough]];
      case State::FINE: {
        if (std::isnan(h)) return end_with_(MoveEnd::STALL);
        float err = target_ - h;
        if (std::fabs(err) <= cfg_.deadband) return end_with_(MoveEnd::DONE);
        if (taps_ >= cfg_.max_taps) return end_with_(MoveEnd::TAP_LIMIT);
        taps_++;
        tap_key_ = err > 0 ? 0x01 : 0x02;
        tap_until_ = now + cfg_.tap_ms;
        state_ = State::TAP;
        return tap_key_;
      }
      case State::TAP:
        if (now < tap_until_) return tap_key_;
        state_ = State::SETTLE;
        settle_until_ = now + cfg_.settle_ms;
        return 0;
      case State::IDLE:
        break;
    }
    return 0;
  }

 private:
  uint8_t end_with_(MoveEnd e) {
    end_ = e;
    state_ = State::IDLE;
    return 0;
  }

  enum class State : uint8_t { IDLE, COARSE, SETTLE, FINE, TAP } state_{State::IDLE};
  MoveConfig cfg_;
  float target_{NAN}, last_h_{NAN};
  uint32_t start_ms_{0}, last_h_change_{0}, settle_until_{0}, tap_until_{0};
  uint8_t taps_{0}, tap_key_{0};
  MoveEnd end_{MoveEnd::NONE};
};

}  // namespace desk_mitm
