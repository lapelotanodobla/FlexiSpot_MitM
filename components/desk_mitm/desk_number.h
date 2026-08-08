#pragma once
#include "esphome/core/component.h"
#include "esphome/components/number/number.h"
#include "desk_mitm.h"

namespace desk_mitm {

class DeskNumber : public esphome::number::Number, public esphome::Component {
 public:
  void set_parent(DeskMitm *p) { parent_ = p; }

 protected:
  void control(float value) override {
    parent_->move_to_height(value);
    this->publish_state(value);
  }
  DeskMitm *parent_{nullptr};
};

}  // namespace desk_mitm
