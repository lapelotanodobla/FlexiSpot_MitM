# Flexispot Desk — ESPHome Sniffer/MITM Design

**Date:** 2026-08-08
**Hardware:** Flexispot desk with HS01B-1 keypad / CB38M2A-1 control box, generic ESP32 devkit, 2× female RJ45 breakouts.
**Context:** Prior attempts (like [iMicknl/LoctekMotion_IoT#146](https://github.com/iMicknl/LoctekMotion_IoT/issues/146)) get ESP control of the desk working but the keypad goes dead, with only a repeating `9B:05:02:00:A1:60:9D` frame visible and no button-press frames. Root cause unknown; suspected to involve RJ45 pin 4 ("PIN 20", the screen-activation line). We start from scratch and capture ground truth before building control.

## Goals

1. **Phase 1 (this spec):** a purely passive tap that logs all traffic in both directions plus the PIN 20 line, while desk and keypad remain in a factory-original circuit.
2. Use those captures to answer: what does a real HS01B-1 keypad send when buttons are pressed, and how does PIN 20 behave?
3. **Phase 2 (later, informed by Phase 1):** active MITM passthrough with command injection.
4. **Phase 3 (later):** full HA integration (height sensor, presets, up/down) with a working keypad.

## Non-goals (Phase 1)

- No desk control from the ESP. No TX pins connected. No HA control entities.
- No decoding logic on the node itself — raw hex logging only; analysis happens on the captured logs.

## Reference protocol (from LoctekMotion_IoT docs)

- UART 9600 baud, 8N1.
- Frames: `9b <len> <type> <payload…> <crc16-lo> <crc16-hi> 9d`.
- Controller→keypad: height display frames. Keypad→controller: command frames (e.g. wake `9b 06 02 00 00 6c a1 9d`).
- Control box accepts commands only while the "screen is active"; PIN 20 HIGH for ~1 s activates it.
- HS01B-1 RJ45 pinout: 8=+5V, 7=GND, 6=desk-TX, 5=desk-RX (keypad-TX), 4=PIN 20, 3/2/1=NC/SWIM/RES.

## Topology: Y-tap (passive)

All 8 lines run straight through between the two RJ45 jacks (desk cable in one, keypad in the other — electrically a coupler). Three signal lines and power additionally branch to the ESP:

```
desk RJ45 ═══════════ 8 straight wires ═══════════ keypad RJ45
                │           │          │
              pin 6       pin 5      pin 4
                │           │          │
             GPIO16      GPIO17     GPIO34      (+ pin 8→VIN, pin 7→GND)
                └────── ESP32 only listens ──────┘
```

| RJ45 line | ESP pin | Role |
|---|---|---|
| 8 (+5V) | VIN | Powers ESP from desk |
| 7 (GND) | GND | Common ground |
| 6 (desk-TX) | GPIO16 | UART1 RX — controller→keypad traffic |
| 5 (keypad-TX) | GPIO17 | UART2 RX — keypad→controller traffic |
| 4 (PIN 20) | GPIO34 | Digital input (input-only pin), edge logging |

**Safety gate before connecting taps:** with keypad plugged straight into desk and no ESP attached, measure pins 6, 5, 4 vs pin 7 with a multimeter during idle and while pressing buttons. Any line that exceeds ~3.6 V gets a resistor divider (e.g. 10k/15k) on its tap; ESP32 GPIOs are not 5 V tolerant. (Reports say these boxes use 3.3 V logic, but verify.)

## ESPHome node (`desk-sniffer.yaml`)

- Board: generic `esp32dev`. WiFi from existing `secrets.yaml`; device currently at 192.168.40.65, known to HA as "Flexispot Desk". Keep `api:` + `ota:` so HA connectivity and OTA updates work.
- Two RX-only `uart:` buses at 9600 8N1 with `debug:` enabled, `after: delimiter: 0x9D`, hex dump `sequence`, direction-labeled `DESK>` (GPIO16 bus) and `KEYPAD>` (GPIO17 bus).
- `binary_sensor` (GPIO platform) on GPIO34 for PIN 20, logging state changes; exposed to HA for convenience.
- Logger at `DEBUG` level over WiFi (`esphome logs`); USB serial only needed for the first flash.

## Verification plan

1. Flash over USB → node joins WiFi → `esphome logs` streams over the network.
2. Keypad sanity in Y-tap circuit: display shows height, buttons move desk. If not, debug wiring/crimps before anything else — the circuit is electrically original, so failure here is physical.
3. Capture a labeled session into a log file committed to this repo (`captures/`): idle baseline, each button press (up, down, presets 1–3, M), a full preset move, preset programming — noting PIN 20 edges throughout.
4. Analysis answers, explicitly written up: (a) does the keypad emit `9b 06 02 …` command frames, and which; (b) PIN 20 behavior at rest / on wake / during presses; (c) any frames not in the LoctekMotion docs.

Exit criterion for Phase 1: questions (a) and (b) answered from captures. That data drives the Phase 2 design (how the MITM must drive/relay PIN 20 so the keypad stays alive).

## Risks

- **Voltage:** covered by the multimeter gate above.
- **UART invert/idle level:** if a bus shows garbage, first suspects are swapped pins 5/6 or inverted logic (`invert: true` on the UART pin is the test).
- **GPIO34 has no internal pull:** if PIN 20 floats when idle, readings may bounce; acceptable for logging (we care about driven states), can add external pulldown if noisy.
- **Powering from desk during flashing:** never USB and desk 5 V simultaneously unless the devkit's diode handles it; unplug RJ45 power lead when flashing over USB.
