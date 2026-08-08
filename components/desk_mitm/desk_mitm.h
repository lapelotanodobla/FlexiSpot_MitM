#pragma once
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/uart/uart.h"
#include "protocol.h"

namespace desk_mitm {

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
};

}  // namespace desk_mitm
