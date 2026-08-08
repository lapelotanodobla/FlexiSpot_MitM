# Phase 3 — Cover Entity + Closed-Loop Move-to-Height

**Date:** 2026-08-08
**Depends on:** `2026-08-08-phase2-mitm-design.md` (hardware unchanged), `2026-08-08-phase2-findings.md` (verified primitives: injection, wake, height telemetry, tap-cancels-move).

## Goal

A real Home Assistant `cover` entity (open/close/stop/position slider) and a `Target Height` (cm) number entity, both driven by one on-device closed-loop `move_to(cm)` engine. No hardware changes.

## Measured constants the design builds on (from captures/findings)

- Travel speed ≈ 2.2 cm/s during autonomous and held-key moves.
- A ~2-poll (≈60 ms) key tap moves ≈ 0.1 cm and cancels any autonomous move.
- Height frames stream continuously while moving; display resolution 0.1 cm.
- Desk coasts briefly after key release (sub-centimeter at manual speeds).
- Wake-from-asleep to first poll ≈ 500 ms via PIN 20 drive.

## MoveController (pure logic, `protocol.h`, host-tested)

State machine: `IDLE → (wake handled by glue) → COARSE → SETTLE → FINE → SETTLE → … → IDLE`.

- `move_to(target, now)`: validates target against `[min_height, max_height]`, computes direction, enters COARSE.
- **COARSE**: output the direction key continuously; when `|target − current| ≤ coast_margin` (default **0.7 cm**), release → SETTLE.
- **SETTLE**: output idle for `settle_ms` (default **1200 ms**) so coast finishes and the display catches up.
- **FINE**: if `|error| ≤ deadband` (default **0.3 cm**) → done (IDLE). Else emit one `tap_polls` (default 2) tap toward the target, → SETTLE. Max `max_taps` (default **5**) fine taps per move; exceeding it ends the move with a logged warning (good enough beats hunting).
- Tick interface: `uint8_t update(float current_height, uint32_t now_ms)` returns the key mask the glue should inject this instant; MoveController never touches hardware.

### Abort rules (all → IDLE, idle output, logged reason)

1. **Physical override**: glue reports any real-keypad key ≥ 1 frame → abort immediately. Human wins, always.
2. **Fail-safe events**: poll silence, `type 13` burst, PIN 20 fall, reboot (glue already clears key state; it also aborts the controller).
3. **Move timeout**: `move_timeout_ms` (default **30 s**) wall-clock cap.
4. **Stall**: height unchanged for `stall_ms` (default **2 s**) while COARSE is commanding motion — the desk's own anti-collision has tripped or something's wrong; do not fight it.

All constants are YAML substitutions with the defaults above.

## ESPHome entities

- **`cover` platform `desk_mitm`** (new `cover.py` + `DeskCover : cover::Cover`): traits position + stop. `position = (height − min_height) / (max_height − min_height)`, published on height change. `open` → `move_to(max_height)`, `close` → `move_to(min_height)`, `stop` → abort + existing stop-tap, `set_position(p)` → `move_to(min + p·(max−min))`. Device icon `mdi:desk`.
- **`number` platform `desk_mitm`** ("Target Height", cm, min/max from the same substitutions, step 0.1): `set` → `move_to(value)`. State reflects the last requested target.
- Existing sensor/buttons/switch unchanged. `move_to` while asleep rides the existing PIN 20 wake path (glue defers engine start until polling begins; existing 2 s wake timeout aborts the move if the controller never wakes).

## Calibration (bench step, part of the plan)

Jog to physical bottom and top with the keypad, read both heights in HA, set `min_height` / `max_height` substitutions. Until calibrated the YAML ships placeholder values 60.0 / 121.0 marked loudly as UNCALIBRATED.

## Testing

Host-side, extending `tests/test_protocol.cpp`: a `SimDesk` model (2.2 cm/s ramp while a key mask is applied, 0.4 cm coast after release, 0.1 cm quantization) driven against MoveController. Cases: long up move lands within deadband; long down move; early-release happens `coast_margin` before target; fine taps converge; tap cap respected; physical-override abort; stall abort; timeout abort; target clamping. The glue layer stays thin (entity plumbing + feeding `update()`), consistent with Phases 1–2.

## Non-goals

Preset programming from HA; speed control (hardware has none); ESP8266 build target (README gets a compatibility note only: 1 hW RX → software serial for keypad side, `logger baud 0`, drop `web_server`).
