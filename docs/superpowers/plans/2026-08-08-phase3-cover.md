# Phase 3 Cover + Move-to-Height Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** HA `cover` entity (open/close/stop/position) and `Target Height` number, driven by a host-tested closed-loop `MoveController` in the existing `desk_mitm` component.

**Architecture:** Per `docs/superpowers/specs/2026-08-08-phase3-cover-design.md`. `MoveController` is pure logic in `protocol.h`, ticked by the glue loop; it outputs a key mask, never touches hardware. Cover/number are thin ESPHome platforms on the component. No hardware changes.

**Tech Stack:** Existing repo conventions — pure C++ TDD via `tests/test_protocol.cpp` + `c++ -std=c++17`, ESPHome external component platforms (`cover.py`, `number.py`).

**Key constants (from findings):** travel ≈2.2 cm/s, 2-poll tap ≈0.1 cm, coast ≈0.4 cm, display step 0.1 cm.

---

### Task 1: MoveController happy path (TDD with SimDesk)

**Files:**
- Modify: `components/desk_mitm/protocol.h`
- Modify: `tests/test_protocol.cpp`

- [ ] **Step 1: Add SimDesk + happy-path tests** (append to `tests/test_protocol.cpp`, call new tests from `main`)

```cpp
// Simulated desk: 2.2 cm/s while a key is held, 0.4 cm coast after release,
// display quantized to 0.1 cm. Matches measured constants in the findings docs.
struct SimDesk {
  float h;
  float coast_left{0};
  int dir{0};
  void step(uint8_t mask, uint32_t dt_ms) {
    const float v = 2.2f * dt_ms / 1000.0f;
    if (mask == 0x01) { h += v; dir = 1; coast_left = 0.4f; }
    else if (mask == 0x02) { h -= v; dir = -1; coast_left = 0.4f; }
    else if (coast_left > 0) {
      float c = v < coast_left ? v : coast_left;
      h += dir * c;
      coast_left -= c;
    }
  }
  float display() const { return roundf(h * 10) / 10; }
};

// Drive controller against sim until it goes idle or 60 simulated seconds pass.
static void run_sim(MoveController &mc, SimDesk &sim, uint32_t &now) {
  const uint32_t deadline = now + 60000;
  while (mc.active() && now < deadline) {
    uint8_t mask = mc.update(sim.display(), now);
    sim.step(mask, 10);
    now += 10;
  }
}

static void test_move_controller_happy() {
  MoveConfig cfg;  // defaults from the spec
  {  // long up move lands within deadband
    MoveController mc(cfg);
    SimDesk sim{75.0f};
    uint32_t now = 1000;
    assert(mc.move_to(110.0f, sim.display(), now));
    run_sim(mc, sim, now);
    assert(!mc.active());
    assert(mc.end_reason() == MoveEnd::DONE);
    assert(std::fabs(sim.display() - 110.0f) <= cfg.deadband + 0.001f);
  }
  {  // long down move
    MoveController mc(cfg);
    SimDesk sim{110.0f};
    uint32_t now = 1000;
    assert(mc.move_to(75.0f, sim.display(), now));
    run_sim(mc, sim, now);
    assert(mc.end_reason() == MoveEnd::DONE);
    assert(std::fabs(sim.display() - 75.0f) <= cfg.deadband + 0.001f);
  }
  {  // early release: key must drop out BEFORE the display reaches the target
    MoveController mc(cfg);
    SimDesk sim{80.0f};
    uint32_t now = 1000;
    mc.move_to(90.0f, sim.display(), now);
    float last_commanded_h = NAN;
    while (mc.active() && now < 61000) {
      uint8_t mask = mc.update(sim.display(), now);
      if (mask != 0) last_commanded_h = sim.display();
      sim.step(mask, 10);
      now += 10;
    }
    assert(mc.end_reason() == MoveEnd::DONE);
    assert(last_commanded_h < 90.0f);  // released below target, coast covered the rest
  }
  {  // target outside range is clamped to the rail
    MoveConfig c2;
    c2.min_height = 60.0f;
    c2.max_height = 121.0f;
    MoveController mc(c2);
    SimDesk sim{119.0f};
    uint32_t now = 1000;
    assert(mc.move_to(300.0f, sim.display(), now));
    run_sim(mc, sim, now);
    assert(std::fabs(sim.display() - 121.0f) <= c2.deadband + 0.001f);
  }
  {  // NaN target rejected outright
    MoveController mc(cfg);
    assert(!mc.move_to(NAN, 75.0f, 1000));
    assert(!mc.active());
  }
}
```

- [ ] **Step 2: Run to verify failure**

Run: `c++ -std=c++17 -o build/test_protocol tests/test_protocol.cpp && ./build/test_protocol`
Expected: compile error — `MoveConfig`/`MoveController`/`MoveEnd` not defined.

- [ ] **Step 3: Implement in `protocol.h`** (before the closing `}  // namespace desk_mitm`)

```cpp
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
```

- [ ] **Step 4: Run tests** — `c++ -std=c++17 -o build/test_protocol tests/test_protocol.cpp && ./build/test_protocol` → `all tests passed`
- [ ] **Step 5: Commit** — `git add components tests && git commit -m "feat: MoveController closed-loop engine with simulated-desk tests"`

---

### Task 2: MoveController failure paths (TDD)

**Files:**
- Modify: `tests/test_protocol.cpp`

- [ ] **Step 1: Add failure-path tests** (call from `main`)

```cpp
static void test_move_controller_failures() {
  MoveConfig cfg;
  {  // abort mid-move: goes idle, outputs 0
    MoveController mc(cfg);
    SimDesk sim{75.0f};
    uint32_t now = 1000;
    mc.move_to(110.0f, sim.display(), now);
    (void) mc.update(sim.display(), now);
    mc.abort();
    assert(!mc.active());
    assert(mc.end_reason() == MoveEnd::ABORTED);
    assert(mc.update(sim.display(), now + 10) == 0);
  }
  {  // stall: height frozen while commanding motion
    MoveController mc(cfg);
    uint32_t now = 1000;
    mc.move_to(110.0f, 75.0f, now);
    while (mc.active() && now < 20000) {
      (void) mc.update(75.0f, now);  // desk never moves
      now += 10;
    }
    assert(mc.end_reason() == MoveEnd::STALL);
    assert(now - 1000 <= cfg.stall_ms + 100);
  }
  {  // timeout: motion keeps happening but never converges (stall never trips)
    MoveConfig slow = cfg;
    slow.stall_ms = 10000000;  // disable stall for this case
    MoveController mc(slow);
    uint32_t now = 1000;
    float h = 75.0f;
    bool flip = false;
    mc.move_to(110.0f, h, now);
    while (mc.active() && now < 60000) {
      (void) mc.update(h, now);
      h += flip ? 0.1f : -0.1f;  // jitter in place forever
      flip = !flip;
      now += 10;
    }
    assert(mc.end_reason() == MoveEnd::TIMEOUT);
  }
  {  // tap limit: deadband unreachably tight -> gives up after max_taps
    MoveConfig tight = cfg;
    tight.deadband = 0.001f;  // display quantizes to 0.1, can't ever satisfy
    MoveController mc(tight);
    SimDesk sim{75.0f};
    uint32_t now = 1000;
    mc.move_to(90.05f, sim.display(), now);
    run_sim(mc, sim, now);
    assert(mc.end_reason() == MoveEnd::TAP_LIMIT);
  }
  {  // NaN current at start: waits, then proceeds once height appears
    MoveController mc(cfg);
    uint32_t now = 1000;
    mc.move_to(80.0f, NAN, now);
    assert(mc.update(NAN, now + 100) == 0);  // holds still while blind
    SimDesk sim{75.0f};
    now += 500;  // height arrives 500ms in (post-wake)
    run_sim(mc, sim, now);
    assert(mc.end_reason() == MoveEnd::DONE);
  }
}
```

- [ ] **Step 2: Run** — Expected: all pass immediately if Task 1's implementation is correct; any failure here is a real bug — fix `MoveController`, not the test.
- [ ] **Step 3: Commit** — `git add tests && git commit -m "test: MoveController abort/stall/timeout/tap-limit/blind-start paths"`

---

### Task 3: Glue integration (wire MoveController into DeskMitm)

**Files:**
- Modify: `components/desk_mitm/desk_mitm.h`
- Modify: `components/desk_mitm/desk_mitm.cpp`
- Modify: `components/desk_mitm/__init__.py`

- [ ] **Step 1: `desk_mitm.h` additions**

Add includes/members/methods (forward-declare the entity classes):

```cpp
// at top, after existing includes
namespace desk_mitm {
class DeskCover;
class DeskNumber;
```

Inside `class DeskMitm`, public section:

```cpp
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
```

Protected members:

```cpp
  MoveConfig move_cfg_;
  MoveController mover_{move_cfg_};
  uint8_t mover_mask_{0};
  float pending_target_{NAN};  // move deferred until controller wakes
  DeskCover *cover_{nullptr};
  DeskNumber *target_number_{nullptr};
  void publish_cover_state_();
```

- [ ] **Step 2: `desk_mitm.cpp` changes**

Add include at top: `#include "desk_cover.h"` and `#include "desk_number.h"` (created in Task 4; to keep this task compiling standalone, guard the two publish sites with `if (cover_ != nullptr)` — the pointers stay null until Task 4 wires them).

In `loop()`, after the UART drains and before the watchdog block:

```cpp
  // Tick the move engine once per loop; poll replies read mover_mask_.
  {
    const uint32_t t = millis();
    bool was_active = mover_.active();
    mover_mask_ = mover_.update(height_, t);
    if (was_active && !mover_.active()) {
      ESP_LOGI(TAG, "move ended: %d (height %.1f)", (int) mover_.end_reason(), height_);
      publish_cover_state_();
    }
  }
```

In `handle_desk_frame_` case `0x11`, next to the pending-injection start:

```cpp
      if (!std::isnan(pending_target_)) {  // wake-deferred move starts now
        mover_.move_to(pending_target_, height_, now);
        pending_target_ = NAN;
      }
```

In `handle_desk_frame_` case `0x12`, after `last_height_change_ms_ = now;` add `publish_cover_state_();`

In `handle_keypad_frame_`, type `0x02` branch — physical override:

```cpp
    if (f.payload[0] != 0 && mover_.active()) {
      ESP_LOGI(TAG, "physical key %02X overrides move-to-height; aborting", f.payload[0]);
      mover_.abort();
    }
```

In `reply_to_poll_`, emulation branch becomes:

```cpp
  uint8_t mask = mover_.active() ? mover_mask_ : keys_.current(now);
  auto reply = build_key_reply(mask);
  desk_->write_array(reply.data(), reply.size());
```

In `failsafe_clear_`: add `mover_.abort(); pending_target_ = NAN;`
In `stop()`: add `mover_.abort();` as the first line.
In `update_pin20_`: extend the hold condition — wake also held while `mover_.active() || !std::isnan(pending_target_)`.

New methods:

```cpp
void DeskMitm::move_to_height(float cm) {
  const uint32_t now = millis();
  if (!emulation_) {
    ESP_LOGW(TAG, "move_to_height ignored: emulation mode is off");
    return;
  }
  if (polling_) {
    if (mover_.move_to(cm, height_, now)) ESP_LOGI(TAG, "move_to %.1f cm", cm);
    return;
  }
  ESP_LOGI(TAG, "controller asleep; waking for move_to %.1f cm", cm);
  pending_target_ = cm;
  wake_request_ = true;
  wake_started_ms_ = now;
}

void DeskMitm::publish_cover_state_() { /* filled in Task 4; no-op until entities exist */
}
```

- [ ] **Step 3: `__init__.py` config options** — extend `CONFIG_SCHEMA` dict:

```python
        cv.Optional("min_height", default=60.0): cv.float_,
        cv.Optional("max_height", default=121.0): cv.float_,
        cv.Optional("coast_margin", default=0.7): cv.float_,
        cv.Optional("deadband", default=0.3): cv.float_,
        cv.Optional("settle_ms", default=1200): cv.positive_int,
        cv.Optional("move_timeout_ms", default=30000): cv.positive_int,
        cv.Optional("stall_ms", default=2000): cv.positive_int,
        cv.Optional("max_taps", default=5): cv.positive_int,
```

and in `to_code`:

```python
    cg.add(
        var.set_move_config(
            config["min_height"], config["max_height"], config["coast_margin"],
            config["deadband"], config["settle_ms"], config["move_timeout_ms"],
            config["stall_ms"], config["max_taps"],
        )
    )
```

- [ ] **Step 4: Gates** — host tests still pass; `esphome compile desk-mitm.yaml` succeeds (entities not in YAML yet).
- [ ] **Step 5: Commit** — `git add components && git commit -m "feat: wire MoveController into desk_mitm glue with wake, override and fail-safe aborts"`

---

### Task 4: Cover + number platforms, YAML, compile

**Files:**
- Create: `components/desk_mitm/desk_cover.h`
- Create: `components/desk_mitm/desk_number.h`
- Create: `components/desk_mitm/cover.py`
- Create: `components/desk_mitm/number.py`
- Modify: `components/desk_mitm/desk_mitm.cpp` (fill `publish_cover_state_`)
- Modify: `desk-mitm.yaml`

- [ ] **Step 1: `desk_cover.h`**

```cpp
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
```

- [ ] **Step 2: `desk_number.h`**

```cpp
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
```

- [ ] **Step 3: fill `publish_cover_state_` in `desk_mitm.cpp`**

```cpp
void DeskMitm::publish_cover_state_() {
  if (cover_ == nullptr || std::isnan(height_)) return;
  float pos = position_from_height_(height_);
  int dir = mover_.active() ? mover_.direction(height_) : 0;
  auto op = dir > 0   ? esphome::cover::COVER_OPERATION_OPENING
            : dir < 0 ? esphome::cover::COVER_OPERATION_CLOSING
                      : esphome::cover::COVER_OPERATION_IDLE;
  if (pos != cover_->position || op != cover_->current_operation) {
    cover_->position = pos;
    cover_->current_operation = op;
    cover_->publish_state();
  }
}
```

- [ ] **Step 4: `cover.py`**

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import cover
from . import DeskMitm, desk_mitm_ns

DEPENDENCIES = ["desk_mitm"]
CONF_DESK_MITM_ID = "desk_mitm_id"

DeskCover = desk_mitm_ns.class_("DeskCover", cover.Cover, cg.Component)

CONFIG_SCHEMA = cover.cover_schema(DeskCover).extend(
    {cv.GenerateID(CONF_DESK_MITM_ID): cv.use_id(DeskMitm)}
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await cover.new_cover(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_DESK_MITM_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_cover(var))
```

- [ ] **Step 5: `number.py`**

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_STEP
from . import DeskMitm, desk_mitm_ns

DEPENDENCIES = ["desk_mitm"]
CONF_DESK_MITM_ID = "desk_mitm_id"

DeskNumber = desk_mitm_ns.class_("DeskNumber", number.Number, cg.Component)

CONFIG_SCHEMA = number.number_schema(DeskNumber).extend(
    {
        cv.GenerateID(CONF_DESK_MITM_ID): cv.use_id(DeskMitm),
        cv.Optional(CONF_MIN_VALUE, default=60.0): cv.float_,
        cv.Optional(CONF_MAX_VALUE, default=121.0): cv.float_,
        cv.Optional(CONF_STEP, default=0.1): cv.positive_float,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await number.new_number(
        config,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_DESK_MITM_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_target_number(var))
```

Note: `desk_mitm.cpp` includes `desk_cover.h`/`desk_number.h`; ESPHome compiles all files in the component dir, and the platform classes are header-only, so no extra build config is needed. If `cover.cover_schema(...)`/`number.number_schema(...)` don't exist in the installed ESPHome (API drift), check `.esphome` build cache of another project or the installed package source for the current names (`COVER_SCHEMA` etc.) and adapt — the C++ side is version-stable.

- [ ] **Step 6: `desk-mitm.yaml`** — add substitutions at the top and entities:

```yaml
substitutions:
  # UNCALIBRATED placeholders — set from the calibration step (Task 5)
  min_height: "60.0"
  max_height: "121.0"
```

Extend the `desk_mitm:` block:

```yaml
  min_height: ${min_height}
  max_height: ${max_height}
```

New top-level blocks:

```yaml
cover:
  - platform: desk_mitm
    name: "Desk"
    device_class: none
    icon: mdi:desk

number:
  - platform: desk_mitm
    name: "Target Height"
    unit_of_measurement: "cm"
    icon: mdi:human-male-height
    mode: box
    min_value: ${min_height}
    max_value: ${max_height}
    step: 0.1
```

- [ ] **Step 7: Gates** — host tests pass; `esphome config desk-mitm.yaml` valid; `esphome compile desk-mitm.yaml` succeeds.
- [ ] **Step 8: Commit** — `git add components desk-mitm.yaml && git commit -m "feat: desk cover and target-height number platforms"`

---

### Task 5: Calibration + live verification

**Files:**
- Modify: `desk-mitm.yaml` (real min/max)

- [ ] **Step 1: Flash** — `esphome run desk-mitm.yaml --device 192.168.40.65 --no-logs` → `OTA successful`.
- [ ] **Step 2: Calibrate** — user jogs (physical keypad) to the very bottom, notes height; then to the very top, notes height. Update the two substitutions, reflash. (Until then the entities work with placeholder rails — just don't trust position 0/1 extremes.)
- [ ] **Step 3: Live checks** (mix of REST/HA driving and user observation):
  - Number: set Target Height to a value ~10 cm away → desk lands within ±0.3 cm (read the sensor).
  - Cover: slider to 50% → desk goes to mid-range; open → top; close → bottom; stop mid-travel → halts.
  - Physical override: start a cover move, press a keypad key → move aborts instantly, keypad command wins.
  - Cold start: with desk asleep, set a target → wakes, moves, sleeps.
- [ ] **Step 4: Commit** — `git add desk-mitm.yaml && git commit -m "feat: calibrated desk travel range"`

---

### Task 6: Docs + close out

**Files:**
- Modify: `README.md`
- Create: `docs/superpowers/specs/2026-08-08-phase3-findings.md`

- [ ] **Step 1: README** — entity list gains the cover + number; add a short "ESP8266 compatibility" note (1 hardware RX → keypad side on software serial, `logger: baud_rate: 0`, drop `web_server`; ESP32 remains the recommendation); mention the closed-loop tunables (substitutions) in one sentence.
- [ ] **Step 2: Findings doc** — measured accuracy over several moves (target vs landed), any tuning changes to coast/deadband defaults, override/cold-start behavior, tap effectiveness.
- [ ] **Step 3: Commit + push** — `git add README.md docs && git commit -m "docs: phase 3 findings and README updates" && git push`
