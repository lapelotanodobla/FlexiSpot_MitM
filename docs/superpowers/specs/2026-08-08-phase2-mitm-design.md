# Phase 2 — Active MITM via Keypad Emulation (single-cut)

**Date:** 2026-08-08 (rev 2 after external review — see "Review outcomes" at bottom)
**Depends on:** `2026-08-08-desk-sniffer-design.md` (Phase 1 hardware), `2026-08-08-phase1-findings.md` (protocol ground truth).

## Goal

The ESP32 intercepts only the keypad→controller direction, keeps the keypad fully functional, and adds minimal HA control: height sensor + buttons (up, down, stop, preset 1/2/3), with deterministic ESP-initiated wake.

## Architecture: emulate the keypad only

**Controller→keypad (pin 5) stays physically intact** — display frames, polls, keepalives and shutdown reach the keypad with original electrics and timing; the ESP taps it read-only (as in Phase 1). Only **keypad→controller (pin 6) is cut**: the ESP receives the real keypad's replies on the keypad-side segment and is the sole talker on the desk-side segment.

- The ESP hears every `9B:04:11` poll on the pin-5 tap and answers on desk-side pin 6 immediately with `9B:06:02:<keys>:00:<crc>:9D` from local state.
- `<keys>` = injected command if an injection is active, **else** latest real-keypad key state. Injection *replaces* (never ORs with) physical keys; opposing/undocumented combinations are never emitted.
- The real keypad still hears polls directly (pin 5 intact) and replies on its own schedule; the ESP consumes those replies to update `real_keypad_keys`.
- Any keypad frame that is **not** `type 02` is forwarded to the controller verbatim (covers preset-programming and unknown exchanges).

**PIN 20 (pin 4) is also cut**: keypad-side segment keeps the existing 4.7k/10k divider → GPIO34 (sense); desk-side segment is driven at 5 V via shifter ch 4. Firmware mirrors keypad→desk and ORs in its own wake request when injecting. This makes ESP-initiated wake deterministic instead of hoping the controller wakes from UART frames alone. (The UART-only wake question can still be answered experimentally from logs, but nothing depends on it.)

### Fail-safe key-state rules

- All key state (injected and mirrored) **clears** on: parser/CRC error on either bus, no controller poll for 500 ms, `type 13` shutdown burst, PIN 20 fall, reboot, or OTA.
- Injections carry a hard deadline (~150 ms for command bursts) — expiry clears them regardless of poll arrival; a stuck injection cannot outlive its window.
- Ignore keypad bytes for 300 ms after keypad-side PIN 20 rises (wake transients); discard partial frames after shutdown.
- Reply exactly once per received poll — never on a self-generated schedule (log timestamps from Phase 1 are batched and unsuitable for deriving a clock; the poll itself is the clock).

### Injection semantics

- **Preset 1/2/3** (`04`/`08`/`10`): command payload in poll replies for ~150 ms (4–5 polls observed from the real keypad; duration-bounded, not count-exact), then idle. Move is fire-and-forget by the controller.
- **Up / Down** (`01`/`02`): stream for ~300 ms per HA button press (coarse jog; closed-loop control is Phase 3).
- **Stop**: clear injection immediately — verified to stop *manual* motion. Whether it cancels a preset move is unknown; tested on the bench in verification step 3 and the entity renamed/documented accordingly if not.
- If the controller is asleep (no polls), an injection first raises desk-side PIN 20, waits for polling to start (~350 ms observed keypad-wake latency; timeout 2 s), then proceeds. PIN 20 held until the move completes (height frames go quiet) plus 1 s.

## Hardware delta from Phase 1

Cut RJ45 lines **6 and 4** inside the coupler; lines 1, 2, 3, 5, 7, 8 stay straight through.

| Segment | Via | ESP pin |
|---|---|---|
| Keypad-side pin 6 (keypad TX) → ESP | shifter ch 1 (existing) | GPIO16 (uart_keypad RX-only) |
| Desk-side pin 5 tap (controller TX) → ESP | shifter ch 2 (existing) | GPIO17 (uart_desk RX) |
| ESP → desk-side pin 6 (controller RX) | shifter ch 3 (new) | GPIO26 (uart_desk TX) |
| ESP → desk-side pin 4 (PIN 20 drive) | shifter ch 4 (new) | GPIO27 (output) |
| Keypad-side pin 4 → ESP | 4.7k/10k divider (existing) | GPIO34 (sense) |

All four channels of the single BSS138 board are now used; no second board needed. BSS138 + 10k pullup rise time (~2–5 µs) is ~2% of a 104 µs bit at 9600 baud — adequate; verification step 1 confirms empirically before anything depends on it.

## Firmware structure

- `components/desk_mitm/` — ESPHome external component (C++): shared frame parser (`9B` → len → payload → CRC-16/MODBUS → `9D`), key-state model with the fail-safe rules above, injection queue, height decoder (7-segment, payload bytes 3–5, validated against session 1), PIN 20 mirror/wake logic.
- Entities: `height` sensor (cm), `keypad_active` (GPIO34), `controller_polling` (diagnostic), buttons up/down/stop/preset 1–3.
- `desk-mitm.yaml` — new node config (same device name/IP). `desk-sniffer.yaml` stays untouched as the known-good fallback build.
- **Echo mode** (boolean): ESP re-emits the real keypad's `type 02` replies verbatim instead of synthesizing from state — the minimal-logic first build and a permanent escape hatch.

## Verification ladder (each step gates the next)

1. **Echo build**: physical keypad fully functional through the ESP (buttons move desk, display live) → proves the 5 V TX path, the cut wiring, and PIN 20 mirroring with minimal firmware in the loop.
2. **Emulation build**: keypad still factory-perfect; height sensor live in HA while moving with the keypad; kill-switch checks (yank keypad mid-hold → desk stops within 500 ms via poll-timeout rule).
3. **Injection, keypad awake**: HA preset button moves desk; stop behavior during manual jog and during a preset move recorded.
4. **ESP wake, keypad asleep**: HA preset from cold — PIN 20 drive brings controller up, injection completes. Also log whether polling ever starts *without* the PIN 20 drive (settles the UART-only wake question for the record).
5. **Abuse pass**: reboot ESP mid-move, OTA mid-move, simultaneous HA + physical input (physical wins after injection window), sleep-boundary injections.

## Risks

- Controller may validate reply timing/content more strictly than observed → reply-on-poll from RAM, echo-mode fallback.
- Keypad may misbehave when its TX has no controller listening (it talks only to the ESP now) → it can't tell the difference electrically; RX-side unchanged.
- Worst case recovery: unplug ESP, re-bridge coupler lines 6 and 4 → factory desk (two jumpers, documented here).

## Non-goals (deferred to Phase 3)

Closed-loop move-to-height, cover/position entity, hold-to-jog from HA, preset programming from HA.

## Review outcomes (Codex second opinion, 2026-08-08)

Accepted: single-cut architecture (pin 5 intact); PIN 20 split now instead of the UART-wake experiment; fail-safe key-state clearing; injection replaces rather than ORs physical keys; duration-bounded injection; poll-as-clock (no schedule derived from batched log timestamps); stop-semantics caveat; height decoder byte-indexing bug fixed and re-validated (`tools/decode.py`, payload bytes 3–5).
Rejected: replacing BSS138 with a dedicated buffer (rise time is 2% of bit period at 9600; empirically gated in step 1); removing the wake observation entirely (kept as free logging, nothing depends on it).
