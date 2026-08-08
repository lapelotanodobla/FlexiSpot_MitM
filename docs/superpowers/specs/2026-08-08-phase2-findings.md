# Phase 2 Findings — Active MITM Verification

**Date:** 2026-08-08. Hardware per `2026-08-08-phase2-mitm-design.md` (single-cut keypad emulation, PIN 20 split). All results from live bench verification on the HS01B-1 / CB38M2A-1 desk.

## Verification ladder results

1. **Echo build — PASS.** Keypad fully functional through the ESP (display, jogs, presets). This confirms the core Phase 1 thesis: prior "keypad goes deaf" failures were 3.3 V transmit levels; at 5 V through the shifter the controller accepts ESP-relayed frames indistinguishably from the real keypad.
2. **Emulation build — PASS.** Keypad behavior indistinguishable from stock with the ESP synthesizing every poll reply from parsed key state. Height sensor tracks the display frames live.
3. **Injection — PASS.** HA/REST preset buttons execute full moves (verified to stored heights, e.g. preset 3 → exactly 75.0 cm); up/down jogs nudge as expected.
4. **ESP-initiated wake — PASS, deterministic.** Driving desk-side PIN 20 wakes the controller reliably: measured 493 ms, 508 ms, 524 ms, ~491 ms from drive to first poll across runs. **No polling was ever observed without the PIN 20 drive** — the UART-only wake hypothesis from older projects is dead on this hardware; the PIN 20 split is required for autonomous control.
5. **Abuse pass — one real discovery** (see hazard below). Not explicitly exercised: keypad unplug mid-manual-move (fail-safe designed: 500 ms poll-silence clears keys), OTA mid-move.

## Stop semantics (settled)

- Reverting to idle replies does **not** cancel a controller-autonomous (preset) move — the controller finishes on its own, exactly as the fire-and-forget model predicted.
- **Any brief up/down key press cancels an in-flight preset move** (verified physically and via injection).
- `stop()` therefore injects a ~60 ms (2-poll) up-tap, gated on "height changed within 500 ms" so Stop on an idle desk remains a true no-op. Verified: preset 1 launched from 77.2 cm, Stop pressed ≈1 s in, desk halted at 78.3 cm.

## Bugs found on hardware (both fixed)

1. **Watchdog time-underflow flap:** `loop()` captured `millis()` before draining UARTs while frame handlers stamped `last_poll_ms_` fresher; the unsigned subtraction wrapped and fired a false 500 ms poll timeout every cycle. Symptom: polling state flapping ~10×/s. Echo mode masked it (echo replies bypass key state). Fix: watchdog timestamp taken after the drain.
2. **Boot height fetch race:** the on-boot no-injection wake can race the controller's own sleep transition and produce nothing. Fix: bounded retry (3 attempts, 30 s apart, only until a height is ever decoded) plus a manual `Refresh Height` button.

## ⚠ Known hazard: ASR/RST reset state

**Cutting ESP power during a controller-autonomous (preset) move puts the desk into the ASR ("ASr"/"A5T"/"RST" on the display) reset state**: the controller loses position certainty when its keypad (the ESP) vanishes mid-move. In this state only downward travel works and power cycling does not clear it. Recovery (~1 minute, standard Flexispot procedure): hold Down until fully lowered, keep holding ~10 s until `RST` shows, release; if needed hold Up (or Up+Down on some units) until a normal height displays. Manual moves do not trigger this — the controller stops cleanly on reply silence. Documented in the README.

## Extra observations

- The keypad wakes itself when the controller starts transmitting on (intact) pin 5 — so HA-initiated moves light up the keypad display for free.
- Wake latency consistency (±20 ms around 500 ms) suggests a fixed controller boot sequence, useful as a health signal.
- Cold-wake injection sessions end with the same `type 13` shutdown burst + PIN 20 fall as keypad sessions; the controller makes no distinction between a real keypad and the emulation.

## Status

Phase 2 complete: keypad fully functional, HA control (height / jogs / presets / working Stop / wake-on-demand / boot height) verified end to end. Phase 3 candidates: closed-loop move-to-height, cover entity, hold-to-jog, preset programming from HA.
