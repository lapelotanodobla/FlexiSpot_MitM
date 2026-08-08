# Flexispot Desk → Home Assistant (ESPHome MITM)

ESP32 man-in-the-middle between a Flexispot standing desk's control box and its keypad, built on ESPHome. The keypad keeps working exactly as stock; Home Assistant gets a live height sensor, up/down/stop, presets, and the ability to wake the desk on its own.

**Hardware this was built against:** HS01B-1 keypad, CB38M2A-1 control box (Flexispot E5B family), generic ESP32 devkit (WROOM), 2× female RJ45 breakouts, one 4-channel BSS138-style bidirectional level shifter, one 4.7 kΩ + one 10 kΩ resistor.

## Why this exists

The excellent prior art ([LoctekMotion_IoT](https://github.com/iMicknl/LoctekMotion_IoT), [standing-desk-interceptor](https://github.com/nv1t/standing-desk-interceptor), [loctek_IOT_box](https://github.com/Dude88/loctek_IOT_box), [ha-flexispot-standing-desk](https://github.com/grssmnn/ha-flexispot-standing-desk)) was mostly built for control boxes with **two RJ45 ports**: the keypad keeps its own port, the ESP plugs into the spare one, and plain TX/RX does the trick — no man-in-the-middle needed, and the keypad problem never arises. On a **single-port** box like this one, the ESP has to share the one line with the keypad, and the passthrough recipes adapted for that case tend to hit the same wall: the ESP can control the desk, but the keypad goes dead ([LoctekMotion_IoT#146](https://github.com/iMicknl/LoctekMotion_IoT/issues/146)). By passively sniffing a factory-wired desk first (phase 1) and only then going active (phase 2), this project found the causes:

1. **The bus is 5 V logic, not 3.3 V** (UART idles at 4.6 V). An ESP32 transmitting at 3.3 V sits at/below the switching threshold of the 5 V-logic keypad and controller — commands "work" by luck, the keypad goes deaf. Everything here goes through a level shifter.
2. **The protocol is strict poll/response.** The controller polls `9B:04:11` at ~31 Hz; the keypad answers every poll with its key state. Nobody free-runs.
3. **RJ45 pin 4 ("PIN 20") is driven by the keypad** as a wake line: high = session active, controller starts polling ~140 ms after the rising edge, sleeps ~10 s after inactivity (announced by a `9B:04:13` burst).
4. On this desk the documented TX/RX naming is swapped: **pin 6 carries keypad→controller, pin 5 carries controller→keypad**.

Full protocol write-ups: [`docs/superpowers/specs/2026-08-08-phase1-findings.md`](docs/superpowers/specs/2026-08-08-phase1-findings.md). Raw + annotated captures in [`captures/`](captures/), frame decoder in [`tools/decode.py`](tools/decode.py).

## Architecture (one line cut, one line split)

The ESP emulates *only* the keypad, toward the controller. The controller→keypad line is never touched, so display data reaches the keypad with factory electrics:

```
controller ──pin 5──┬────────────────────────────► keypad   (polls, display, keepalives
                    │                                        — physically untouched)
                    └──► ESP listens (tap)

keypad ──pin 6 (keypad side)──► ESP ──pin 6 (desk side)──► controller
         (ESP consumes real key frames,   (ESP answers every poll:
          or forwards unknown frames)      real keys, or HA-injected keys)

keypad ──pin 4 (keypad side)──► ESP sense ──pin 4 (desk side)──► controller
         (divider → GPIO34)      (GPIO27 → shifter: mirrors keypad wake,
                                  ORs in ESP's own wake for HA commands)
```

## Wiring

Both RJ45 breakouts sit back-to-back as a "coupler". Six of the eight lines are bridged straight through; **only lines 6 and 4 are cut**, and line 5 gets a tap.

Pin numbering note: hold the plug clip-down, contacts toward you, pin 1 on the left. **Verify with a meter before trusting any table**: pin 8 = +5 V, pin 7 = GND on the desk side.

| RJ45 line | Coupler | Connections |
|---|---|---|
| 1 (RES) | straight | — |
| 2 (SWIM) | straight | — |
| 3 (n/c) | straight | — |
| 4 (PIN 20) | **CUT** | keypad side → 4.7k → `GPIO34` node → 10k → GND · desk side ← shifter `HV4`, `LV4` ← `GPIO27` |
| 5 (controller→keypad UART) | straight + tap | tap → shifter `HV2`, `LV2` → `GPIO17` |
| 6 (keypad→controller UART) | **CUT** | keypad side → shifter `HV1`, `LV1` → `GPIO16` · desk side ← shifter `HV3`, `LV3` ← `GPIO26` |
| 7 (GND) | straight | ESP `GND`, shifter `GND` |
| 8 (+5 V) | straight | ESP `VIN`, shifter `HV` ref |

Shifter `LV` ref ← ESP `3V3`.

```
                        ┌──────────────────────────────┐
  desk cable            │   BSS138 level shifter       │           keypad cable
 ┌─────────┐            │  HV ── pin8 (+5V)            │          ┌─────────┐
 │ 8 +5V ──┼────────────┼──────────────────────────────┼──────────┼── 8     │
 │ 7 GND ──┼────────────┼──GND                LV ──3V3─┼──────────┼── 7     │
 │         │            │                              │          │         │
 │ 5 ──────┼───┬────────┼──────────────────────────────┼──────────┼── 5     │
 │         │   └─►HV2   │  LV2─►GPIO17 (uart_desk RX)  │          │         │
 │         │            │                              │          │         │
 │ 6 ◄─────┼──HV3       │  LV3◄─GPIO26 (uart_desk TX)  │   ┌──────┼── 6     │
 │      ×CUT×           │  LV1─►GPIO16 (uart_keypad RX)│ HV1◄─────┘         │
 │         │            │                              │          │         │
 │ 4 ◄─────┼──HV4       │  LV4◄─GPIO27 (PIN20 drive)   │   ┌──────┼── 4     │
 │      ×CUT×           └──────────────────────────────┘   │      │         │
 │         │                     GPIO34 ◄──[4.7k]──────────┤      │         │
 │ 1,2,3 ──┼───────────────────────────────[10k]           │      │         │
 └─────────┘         (straight through)      └───── GND    │      └─────────┘
                                                     (divider on keypad side)
```

**Electrical notes**

- ESP32 pins are **not 5 V tolerant** — nothing from the bus may touch a GPIO without the shifter or the divider.
- The PIN 20 *sense* is a plain resistor divider (not a shifter channel) on purpose: shifter boards pull their HV side to 5 V, which would hold a sleeping desk awake.
- Power the ESP from desk 5 V (pin 8 → VIN). **Never desk 5 V and USB at the same time** — unplug the pin-8 lead before flashing over USB. OTA needs no cable.

## Repo layout

| Path | What |
|---|---|
| `desk-mitm.yaml` | The active MITM node (phase 2, current) |
| `desk-sniffer.yaml` | Passive sniffer node (phase 1, known-good fallback; wiring differs — see its spec) |
| `components/desk_mitm/` | ESPHome external component: `protocol.h` (pure logic) + glue |
| `tests/test_protocol.cpp` | Host-side unit tests; every vector is a captured frame |
| `captures/` | Raw + annotated sniff sessions |
| `tools/decode.py` | Turns a capture log into an annotated timeline |
| `docs/superpowers/specs/` | Design specs and protocol findings |
| `docs/superpowers/plans/` | Implementation plans |

## Usage

```sh
# secrets.yaml (not committed): wifi_ssid + wifi_password
esphome run desk-mitm.yaml                     # first flash over USB, then OTA
esphome logs desk-mitm.yaml                    # live logs over WiFi
c++ -std=c++17 -o build/test_protocol tests/test_protocol.cpp && ./build/test_protocol
```

Home Assistant entities (via native ESPHome API): `Desk Height` (cm, fetched automatically at boot via a wake pulse), `PIN 20` / `Controller Polling` / `Refresh Height` (diagnostics), `Emulation Mode` switch, and buttons `Desk Up (jog)`, `Desk Down (jog)`, `Desk Stop`, `Desk Preset 1/2/3`. The node also serves a local web UI + REST API at its IP (`web_server`).

**Emulation Mode off** (default at boot) = echo mode: the ESP replays the real keypad's frames verbatim — zero protocol logic in the loop, useful as a first bring-up step and a permanent escape hatch. Injection requires the switch on.

## Protocol cheat sheet

Frames: `9B <len> <type> <payload…> <crc_hi> <crc_lo> 9D` · 9600 8N1 · `len` counts everything after itself incl. the `9D` · CRC-16/MODBUS over `[len, type, payload]`, high byte first.

| Frame | Who | Meaning |
|---|---|---|
| `9B:04:11:7C:C3:9D` | controller | key-state poll, ~31 Hz while awake |
| `9B:06:02:<k>:00:<crc>:9D` | keypad | reply to every poll; `<k>`: 01 up · 02 down · 04/08/10 presets · 20 M |
| `9B:07:12:<3 digits>:<crc>:9D` | controller | display (7-segment bytes, bit 7 = decimal point) |
| `9B:04:15:BF:C2:9D` | controller | keepalive, ~2 Hz |
| `9B:04:13:BD:42:9D` | controller | shutdown burst right before PIN 20 falls |

Presets are fire-and-forget (one ~150 ms key burst, controller completes the move). Manual up/down move only while the key streams.

## Safety / fail-safes

All key state clears on: poll silence >500 ms, CRC failure, shutdown burst, PIN 20 fall, reboot. Injections carry hard deadlines. Only single documented key masks can ever be emitted. Worst case: unplug the ESP and re-bridge coupler lines 6 and 4 → factory desk.

**⚠ Known hazard — ASR/RST reset state.** If the ESP loses power *during a preset (autonomous) move*, the controller loses position certainty and enters its reset state: display shows `ASr`/`RST`, only downward travel works, and power cycling does **not** clear it. Recovery takes a minute and is the standard Flexispot procedure: hold Down until the desk is fully lowered and `RST` shows (~10 s extra), release; on some units then hold Up (or Up+Down) until a normal height displays. Manual moves are unaffected — the controller stops cleanly when replies vanish. Stop semantics note: reverting to idle replies never cancels a preset move; the firmware's Stop button injects a ~60 ms key tap instead, which does (verified).

## Credits

Protocol groundwork by [iMicknl/LoctekMotion_IoT](https://github.com/iMicknl/LoctekMotion_IoT) and the projects linked above; this repo's contribution is the measured 5 V finding, the poll/response + PIN 20 semantics, and the keypad-emulation architecture built on them.
