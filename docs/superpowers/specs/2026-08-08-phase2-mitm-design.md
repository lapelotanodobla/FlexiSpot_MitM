# Phase 2 — Active MITM via Endpoint Emulation

**Date:** 2026-08-08
**Depends on:** `2026-08-08-desk-sniffer-design.md` (Phase 1 hardware), `2026-08-08-phase1-findings.md` (protocol ground truth).

## Goal

The ESP32 sits between controller and keypad on both UART lines, keeps the keypad fully functional, and adds minimal HA control: height sensor + buttons (up, down, stop, preset 1/2/3).

## Architecture: endpoint emulation

The ESP plays both roles rather than bridging bytes:

- **Toward the controller (uart_desk), the ESP is a keypad.** It answers every `9B:04:11` poll immediately with `9B:06:02:<keys>:00:<crc>:9D`, where `<keys>` = `real_keypad_keys | injected_keys`. The controller never sees a dead or slow partner.
- **Toward the keypad (uart_keypad), the ESP is a controller.** It forwards the real controller's frames (polls, `type 12` display, `type 15` keepalive, `type 13` shutdown) verbatim as they arrive.
- `real_keypad_keys` is updated from the keypad's `type 02` replies. Any keypad frame that is **not** `type 02` is forwarded to the controller verbatim (future-proofs preset programming and unknown exchanges).

Decoupling the two conversations removes forwarding latency from the poll→reply path (observed real-keypad turnaround: ~6 ms; ours is from RAM) and makes injection a pure state change.

### Frame handling rules (from Phase 1 findings)

- Frame parser: `9B` → length byte → payload → CRC-16 (ModRTU, as in LoctekMotion docs) → `9D`. Bad CRC or malformed → drop, log at DEBUG.
- Discard partial frames after a `type 13` burst / PIN 20 fall; ignore keypad bytes for 300 ms after PIN 20 rises (wake transients `00`, `FE`).
- Height decoding from `type 12`: 7-segment map, bit 7 = decimal point; 3-digit values ≥ 100 have no dot.

### Injection semantics

- **Preset 1/2/3**: set `injected_keys` to `04`/`08`/`10` for 5 poll replies, then clear. Move is fire-and-forget by the controller.
- **Up / Down**: set `01`/`02` for 10 poll replies (~300 ms of motion) per HA button press. Coarse jog; closed-loop "move to height" is Phase 3.
- **Stop**: clear `injected_keys` immediately (idle replies stop manual motion).
- **Wake experiment** (controller asleep, i.e. no polls seen for >1 s): on injection request, transmit unsolicited idle + command frames for up to 1 s while watching for polling to resume. Outcome logged loudly. If the controller cannot be woken over UART alone, Phase 2b adds a desk-side PIN 20 driver (transistor open-drain + 10k pullup to 5 V, or a second shifter board); PIN 4 stays uncut in Phase 2a regardless.

## Hardware delta from Phase 1

Cut RJ45 lines **5 and 6** inside the coupler; lines 1–4, 7, 8 stay straight through (keypad still drives PIN 20 itself; GPIO34 divider tap still senses it).

| Segment | Shifter ch | ESP pin |
|---|---|---|
| Keypad-side pin 6 (keypad TX) → ESP | ch 1 (existing) | GPIO16 (uart_keypad RX) |
| Desk-side pin 5 (controller TX) → ESP | ch 2 (existing) | GPIO17 (uart_desk RX) |
| ESP → desk-side pin 6 (controller RX) | ch 3 (new) | GPIO26 (uart_desk TX) |
| ESP → keypad-side pin 5 (keypad RX) | ch 4 (new) | GPIO27 (uart_keypad TX) |

Note the deliberate cross-over: each UART bus pairs one keypad-side and one desk-side segment, because pin 6 is keypad→controller and pin 5 is controller→keypad (Phase 1 finding).

## Firmware structure

- `components/desk_mitm/` — ESPHome external component (C++): frame parser ×2, key-state model, injection queue, height decode, wake experiment. Exposes: `height` sensor (cm, float), `keypad_active` binary sensor (PIN 20 via existing GPIO34), `controller_polling` binary sensor (diagnostic), and methods `press(mask, n_polls)` / `stop()`.
- `desk-mitm.yaml` — new node config (same device name/IP): two full UARTs at 9600, component instantiation, HA `button:` entities calling the methods, existing `binary_sensor` for PIN 20.
- `desk-sniffer.yaml` stays in the repo untouched as the known-good fallback build.
- A `passthrough` boolean compile/runtime mode: forward both directions byte-verbatim with no emulation (verification step 1 and permanent escape hatch).

## Verification ladder (each step gates the next)

1. **Passthrough build**: keypad works identically through the ESP → proves 5 V TX path and wiring. If keypad misbehaves here, the problem is electrical, full stop.
2. **Emulation build**: keypad still factory-perfect; height sensor live in HA while moving with the keypad.
3. **Injection, keypad awake**: HA preset button moves the desk; stop works during manual jog.
4. **Wake experiment, keypad asleep**: logged verdict → Phase 2b (PIN 20 driver) or done.

## Risks

- Controller may validate reply timing or contents more strictly than observed → mitigations: reply-from-RAM (faster than real keypad), verbatim forwarding of anything unrecognized, passthrough escape hatch.
- Unknown frames during preset programming → verbatim-forward rule covers; capture a programming session in Phase 2 to extend the findings doc.
- Worst case recovery: unplug ESP, re-bridge coupler lines 5/6 → factory desk.

## Non-goals (deferred to Phase 3)

Closed-loop move-to-height, cover/position entity, HA hold-to-jog, preset programming from HA, PIN 20 desk-side driver (unless the wake experiment forces it into 2b).
