#include "desk_mitm.h"
#include "esphome/core/log.h"

namespace desk_mitm {

static const char *const TAG = "desk_mitm";
using esphome::millis;

void DeskMitm::setup() {
  pin20_sense_->setup();
  pin20_drive_->setup();
  pin20_drive_->digital_write(false);
}

void DeskMitm::loop() {
  // PIN 20 sense edges.
  bool s = pin20_sense_->digital_read();
  if (s && !sense_state_) sense_rise_ms_ = millis();
  if (!s && sense_state_) failsafe_clear_("pin20 fell");
  sense_state_ = s;

  // Drain desk-side UART (controller talking on the pin-5 tap).
  uint8_t b;
  Frame f;
  while (desk_->available()) {
    desk_->read_byte(&b);
    if (desk_parser_.feed(b, f)) handle_desk_frame_(f);
  }
  // Drain keypad-side UART.
  while (keypad_->available()) {
    keypad_->read_byte(&b);
    if (keypad_parser_.feed(b, f)) handle_keypad_frame_(f);
  }

  // Timestamp taken AFTER the drains: frame handlers stamp last_poll_ms_ with a
  // fresh millis(), so a pre-drain timestamp here would sit in their past and
  // make the unsigned comparisons below underflow into false timeouts.
  const uint32_t now = millis();

  // Poll watchdog: 500 ms of silence clears everything.
  if (polling_ && now - last_poll_ms_ > 500) {
    polling_ = false;
    failsafe_clear_("poll timeout");
  }
  // Wake timeout: controller never started polling.
  if (wake_request_ && pending_mask_ != 0 && now - wake_started_ms_ > 2000 && !polling_) {
    ESP_LOGW(TAG, "wake timeout: controller did not start polling; dropping injection");
    pending_mask_ = 0;
    wake_request_ = false;
  }
  update_pin20_();
}

void DeskMitm::handle_desk_frame_(const Frame &f) {
  const uint32_t now = millis();
  switch (f.type) {
    case 0x11:  // poll
      if (!polling_) ESP_LOGI(TAG, "controller polling started");
      polling_ = true;
      last_poll_ms_ = now;
      if (pending_mask_ != 0) {  // wake-deferred injection starts now
        keys_.inject(pending_mask_, pending_duration_, now);
        pending_mask_ = 0;
      }
      reply_to_poll_();
      break;
    case 0x12: {  // display
      if (f.payload.size() == 3) {
        float h = decode_height(f.payload.data());
        if (!std::isnan(h) && h != height_) {
          height_ = h;
          last_height_change_ms_ = now;
        }
      }
      break;
    }
    case 0x13:  // shutdown announcement
      failsafe_clear_("controller shutdown");
      polling_ = false;
      break;
    default:
      break;  // keepalive 0x15 etc: keypad hears these directly on intact pin 5
  }
}

void DeskMitm::handle_keypad_frame_(const Frame &f) {
  const uint32_t now = millis();
  // Squelch wake transients for 300 ms after PIN 20 rise.
  if (sense_state_ && now - sense_rise_ms_ < 300) return;
  if (f.type == 0x02 && f.payload.size() == 2) {
    keys_.set_real(f.payload[0]);
    last_real_reply_ = f.raw;
    last_real_reply_ms_ = now;
  } else {
    // Unknown keypad frame (programming etc.): forward verbatim.
    desk_->write_array(f.raw.data(), f.raw.size());
    ESP_LOGI(TAG, "forwarded keypad frame type=%02X len=%u", f.type, (unsigned) f.raw.size());
  }
}

void DeskMitm::reply_to_poll_() {
  const uint32_t now = millis();
  if (!emulation_) {
    // Echo mode: replay the keypad's latest reply if fresh, else synthesize idle.
    if (!last_real_reply_.empty() && now - last_real_reply_ms_ < 500) {
      desk_->write_array(last_real_reply_.data(), last_real_reply_.size());
      return;
    }
    auto idle = build_key_reply(0x00);
    desk_->write_array(idle.data(), idle.size());
    return;
  }
  auto reply = build_key_reply(keys_.current(now));
  desk_->write_array(reply.data(), reply.size());
}

void DeskMitm::inject(uint8_t mask, uint32_t duration_ms) {
  const uint32_t now = millis();
  if (!emulation_) {
    ESP_LOGW(TAG, "injection ignored: emulation mode is off");
    return;
  }
  if (polling_) {
    if (!keys_.inject(mask, duration_ms, now)) ESP_LOGW(TAG, "invalid mask %02X", mask);
    return;
  }
  ESP_LOGI(TAG, "controller asleep; raising PIN20 and deferring injection %02X", mask);
  pending_mask_ = mask;
  pending_duration_ = duration_ms;
  wake_request_ = true;
  wake_started_ms_ = now;
}

void DeskMitm::stop() { keys_.stop(); }

void DeskMitm::update_pin20_() {
  const uint32_t now = millis();
  // Hold our wake while: injection pending/active, or height still settling.
  bool moving_recently = now - last_height_change_ms_ < 1000;
  if (wake_request_ && pending_mask_ == 0 && !keys_.injection_active(now) && !moving_recently)
    wake_request_ = false;
  // Mirror keypad wake; OR in our own.
  pin20_drive_->digital_write(sense_state_ || wake_request_);
}

void DeskMitm::failsafe_clear_(const char *reason) {
  keys_.clear_all();
  pending_mask_ = 0;
  ESP_LOGD(TAG, "key state cleared (%s)", reason);
}

}  // namespace desk_mitm
