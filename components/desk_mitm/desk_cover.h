#pragma once
#include "esphome/core/component.h"
#include "esphome/components/cover/cover.h"
#include "desk_mitm.h"

namespace desk_mitm {

class DeskCover : public esphome::cover::Cover, public esphome::Component {
 public:
  void set_parent(DeskMitm *p) { parent_ = p; }
  esphome::cover::CoverTraits get_traits() override {
    auto t = esphome::cover::CoverTraits();
    t.set_supports_position(true);
    t.set_supports_stop(true);
    return t;
  }

 protected:
  void control(const esphome::cover::CoverCall &call) override {
    if (call.get_stop()) parent_->stop();
    if (call.get_position().has_value()) parent_->move_to_position(*call.get_position());
  }
  DeskMitm *parent_{nullptr};
};

}  // namespace desk_mitm
