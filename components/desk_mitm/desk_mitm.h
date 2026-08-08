#pragma once
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/uart/uart.h"
#include "protocol.h"

namespace desk_mitm {

class DeskCover;
class DeskNumber;

class DeskMitm : public esphome::Component {
 public:
  void set_uarts(esphome::uart::UARTComponent *desk, esphome::uart::UARTComponent *keypad) {
    desk_ = desk;
    keypad_ = keypad;
  }
  void set_pin20(esphome::GPIOPin *sense, esphome::GPIOPin *drive) {
    pin20_sense_ = sense;
    pin20_drive_ = drive;
  }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return esphome::setup_priority::DATA; }

  // Called from YAML template buttons / switch.
  void inject(uint8_t mask, uint32_t duration_ms);
  void stop();
  // Wake the controller with no key injection, so it streams the current
  // height (display frames) and sleeps again on its own. Used at boot.
  void request_height();

  void set_move_config(float min_h, float max_h, float coast, float deadband,
                       uint32_t settle_ms, uint32_t timeout_ms, uint32_t stall_ms,
                       uint8_t max_taps) {
    move_cfg_.min_height = min_h;
    move_cfg_.max_height = max_h;
    move_cfg_.coast_margin = coast;
    move_cfg_.deadband = deadband;
    move_cfg_.settle_ms = settle_ms;
    move_cfg_.move_timeout_ms = timeout_ms;
    move_cfg_.stall_ms = stall_ms;
    move_cfg_.max_taps = max_taps;
    mover_.set_config(move_cfg_);
  }
  void set_cover(DeskCover *c) { cover_ = c; }
  void set_target_number(DeskNumber *n) { target_number_ = n; }

  void move_to_height(float cm);
  void move_to_position(float pos) {
    move_to_height(move_cfg_.min_height + pos * (move_cfg_.max_height - move_cfg_.min_height));
  }
  float position_from_height_(float h) const {
    float p = (h - move_cfg_.min_height) / (move_cfg_.max_height - move_cfg_.min_height);
    return p < 0 ? 0 : (p > 1 ? 1 : p);
  }
  void set_emulation(bool on) { emulation_ = on; }

  float get_height() const { return height_; }
  bool get_controller_polling() const { return polling_; }

 protected:
  void handle_desk_frame_(const Frame &f);
  void handle_keypad_frame_(const Frame &f);
  void reply_to_poll_();
  void update_pin20_();
  void failsafe_clear_(const char *reason);

  esphome::uart::UARTComponent *desk_{nullptr};
  esphome::uart::UARTComponent *keypad_{nullptr};
  esphome::GPIOPin *pin20_sense_{nullptr};
  esphome::GPIOPin *pin20_drive_{nullptr};

  Parser desk_parser_, keypad_parser_;
  KeyState keys_;

  bool emulation_{false};  // false = echo mode
  std::vector<uint8_t> last_real_reply_;
  uint32_t last_real_reply_ms_{0};

  float height_{NAN};
  uint32_t last_height_change_ms_{0};
  uint32_t last_poll_ms_{0};
  bool polling_{false};

  bool sense_state_{false};
  uint32_t sense_rise_ms_{0};

  bool wake_request_{false};
  uint8_t pending_mask_{0};
  uint32_t pending_duration_{0};
  uint32_t wake_started_ms_{0};

  uint8_t height_fetch_attempts_{0};
  uint32_t last_height_fetch_ms_{0};

  MoveConfig move_cfg_;
  MoveController mover_{move_cfg_};
  uint8_t mover_mask_{0};
  float pending_target_{NAN};  // move deferred until controller wakes
  DeskCover *cover_{nullptr};
  DeskNumber *target_number_{nullptr};
  void publish_cover_state_();
};

}  // namespace desk_mitm
