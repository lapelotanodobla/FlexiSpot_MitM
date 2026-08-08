# Phase 1 Passive Desk Sniffer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A passive ESPHome tap that logs all desk↔keypad UART traffic and PIN 20 edges while the keypad runs in a factory-original circuit, producing captures that answer what the HS01B-1 keypad sends and how PIN 20 behaves.

**Architecture:** All 8 RJ45 lines straight through desk↔keypad; three lines branch to the ESP32 (two UART RX-only taps via a BSS138 level shifter, PIN 20 via resistor divider to an input-only GPIO). The node transmits nothing. Spec: `docs/superpowers/specs/2026-08-08-desk-sniffer-design.md`.

**Tech Stack:** ESPHome (installed, latest), generic `esp32dev` devkit, 2× female RJ45 breakouts, 4-ch BSS138 level shifter, 4.7k + 10k resistors.

**Verification model:** Hardware project — "tests" are `esphome config`/`compile` gates, multimeter checks before each connection, and observable behavior (keypad works, frames appear in logs). Each task ends verified before the next.

---

### Task 1: ESPHome sniffer config

**Files:**
- Create: `desk-sniffer.yaml`
- Uses: `secrets.yaml` (exists: `wifi_ssid`, `wifi_password`)

- [ ] **Step 1: Write `desk-sniffer.yaml`**

```yaml
esphome:
  name: desk-sniffer
  friendly_name: Flexispot Desk

esp32:
  board: esp32dev
  framework:
    type: esp-idf

logger:
  level: DEBUG

api:

ota:
  - platform: esphome

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  use_address: 192.168.40.65
  ap:
    ssid: "Desk-Sniffer Fallback"

captive_portal:

# Both buses are RX-only taps. The ESP transmits nothing in Phase 1.
uart:
  - id: uart_desk    # controller -> keypad line (RJ45 pin 6)
    rx_pin: GPIO16
    baud_rate: 9600
    debug:
      direction: RX
      dummy_receiver: true
      after:
        delimiter: [0x9D]
      sequence:
        - lambda: |-
            std::string hex;
            char buf[4];
            for (auto b : bytes) { sprintf(buf, "%02X:", b); hex += buf; }
            if (!hex.empty()) hex.pop_back();
            ESP_LOGD("DESK", "%s", hex.c_str());
  - id: uart_keypad  # keypad -> controller line (RJ45 pin 5)
    rx_pin: GPIO17
    baud_rate: 9600
    debug:
      direction: RX
      dummy_receiver: true
      after:
        delimiter: [0x9D]
      sequence:
        - lambda: |-
            std::string hex;
            char buf[4];
            for (auto b : bytes) { sprintf(buf, "%02X:", b); hex += buf; }
            if (!hex.empty()) hex.pop_back();
            ESP_LOGD("KEYPAD", "%s", hex.c_str());

binary_sensor:
  - platform: gpio
    pin:
      number: GPIO34
      mode:
        input: true
    name: "PIN 20"
    id: pin20
```

Notes for the implementer:
- `friendly_name: Flexispot Desk` matches what HA already shows. The old node's API key is lost, so HA will re-discover this as a fresh ESPHome device — delete the stale HA device entry and adopt the new one when prompted.
- `after: delimiter: [0x9D]` flushes a log line at each frame terminator; a partial tail also flushes on the default 100 ms timeout.
- GPIO34 is input-only and has no internal pulls — correct here, the divider defines the level.
- Binary sensor state changes are logged with timestamps by the logger automatically; no extra config needed.

- [ ] **Step 2: Validate the config**

Run: `esphome config desk-sniffer.yaml`
Expected: full rendered config printed, exit 0, no red errors. (`api:` with no key will auto-note a generated key — fine.)

- [ ] **Step 3: Compile**

Run: `esphome compile desk-sniffer.yaml`
Expected: ends with `Successfully compiled program` (first build downloads esp-idf toolchain, takes minutes).

- [ ] **Step 4: Commit**

```bash
git add desk-sniffer.yaml
git commit -m "feat: Phase 1 passive sniffer node (RX-only taps + PIN20 sense)"
```

---

### Task 2: Bench wiring (no ESP connected yet)

**Files:** none — hardware. Multimeter checks gate every connection.

- [ ] **Step 1: Straight-through coupling**

Wire all 8 lines of the two female RJ45 breakouts 1:1 (1→1 … 8→8). Plug desk cable into one jack, keypad into the other.

- [ ] **Step 2: Verify factory behavior through the coupler**

Power the desk. Expected: keypad display lights/shows height, up/down buttons move the desk. If not, stop and fix continuity (meter each pin 1→1 … 8→8) — failure here is crimps, not protocol.

- [ ] **Step 3: Shifter power rails**

Shifter HV ← RJ45 pin 8 (+5 V); shifter LV ← nothing yet (ESP 3V3 comes later); shifter GND ← RJ45 pin 7. Do not connect channel wires to the ESP yet.

- [ ] **Step 4: Signal branches**

- RJ45 pin 6 branch → shifter HV1
- RJ45 pin 5 branch → shifter HV2
- RJ45 pin 4 branch → 4.7k resistor → node "P20_TAP" → 10k resistor → GND (pin 7)

- [ ] **Step 5: Meter the low sides before any ESP connection**

Desk powered, keypad awake. Measure vs GND:
- P20_TAP: expect ≈ 2.8 V (4.1 V × 10/14.7) awake, ≈ 0 V asleep
- LV1/LV2 with LV rail unpowered: expect ≈ 0 V (channels are dead without LV ref — that's fine)

Any P20_TAP reading > 3.4 V: stop, recheck resistor order.

---

### Task 3: First flash and network verification (ESP on USB, RJ45 power lead disconnected)

**Files:** none — uses `desk-sniffer.yaml` from Task 1.

- [ ] **Step 1: Isolate desk power from ESP**

Disconnect the pin 8→VIN lead (or don't connect it yet). ESP on USB only. GND lead (pin 7→ESP GND) can be connected — shared ground is required and safe.

- [ ] **Step 2: Flash over USB**

Run: `esphome run desk-sniffer.yaml` and pick the serial port.
Expected: flash succeeds, boot log shows WiFi connected with IP `192.168.40.65`.

- [ ] **Step 3: Verify OTA log path**

Unplug USB, replug (still no RJ45 5 V). Run: `esphome logs desk-sniffer.yaml`
Expected: connects over the network, streams logs. From here on, USB is only needed for recovery.

---

### Task 4: Connect taps and go standalone

**Files:** none — hardware.

- [ ] **Step 1: Connect ESP with everything unpowered**

Desk unplugged, USB out. Connect: shifter LV ← ESP 3V3, LV1 → GPIO16, LV2 → GPIO17, P20_TAP → GPIO34, RJ45 pin 8 → VIN, pin 7 → ESP GND.

- [ ] **Step 2: Power up from the desk**

Plug in the desk. Expected: ESP boots (LED), joins WiFi, `esphome logs desk-sniffer.yaml` streams.

- [ ] **Step 3: Keypad still factory-functional**

Expected: display shows height, buttons move desk — identical to Task 2 Step 2. If the keypad broke, the taps are loading the bus: check shifter channel wiring (a swapped HV/LV would do this).

- [ ] **Step 4: Traffic smoke test**

Watch logs; wake the keypad and press up briefly. Expected: `[DESK]` and/or `[KEYPAD]` hex lines appear, `PIN 20` binary sensor flips. Frames should start `9B` and end `9D`. Garbage bytes → suspects: pins 5/6 swapped at the branch, or wrong baud (shouldn't be — 9600 is documented).

---

### Task 5: Capture session

**Files:**
- Create: `captures/2026-08-08-session1.log` (raw)
- Create: `captures/2026-08-08-session1.md` (annotated timeline)

- [ ] **Step 1: Record raw log**

Run: `mkdir -p captures && esphome logs desk-sniffer.yaml 2>&1 | tee captures/2026-08-08-session1.log`

- [ ] **Step 2: Scripted actions, ~5 s apart, noting wall-clock time of each**

1. Idle 60 s, keypad asleep
2. Wake keypad (touch/any button), then idle 10 s
3. Up — short tap; 4. Up — hold 2 s; 5. Down — short tap; 6. Down — hold 2 s
7. Preset 1; 8. Preset 2; 9. Preset 3 (let each move finish)
10. M button alone; 11. Program a preset (M then a preset key)
12. Let keypad time out to sleep, idle 30 s

Write each action + timestamp into `captures/2026-08-08-session1.md` as you go.

- [ ] **Step 3: Commit captures**

```bash
git add captures/
git commit -m "data: Phase 1 sniff session 1 (raw + annotated)"
```

---

### Task 6: Protocol analysis write-up

**Files:**
- Create: `docs/superpowers/specs/2026-08-08-phase1-findings.md`

- [ ] **Step 1: Decode the capture**

Cross-reference `captures/2026-08-08-session1.log` against the LoctekMotion command table (https://github.com/iMicknl/LoctekMotion_IoT — command frames `9b 06 02 <k1> <k2> <crc> 9d`, height frames from controller). Produce the findings doc answering, with quoted frames:
- (a) Which frames does the keypad emit per button? Do they match the documented `9b 06 02 …` set?
- (b) PIN 20 timeline: level at rest / edge relative to wake / behavior during presses and moves. Who drives it (does it rise before the first keypad frame or after a controller frame)?
- (c) Any undocumented frames (e.g. the `9B:05:02:00:A1:60:9D` heartbeat from issue #146 — which side sends it, when).
- (d) Consequences for Phase 2: what the MITM must forward/drive on PIN 20, and required forwarding latency (max observed inter-frame gap).

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-08-08-phase1-findings.md
git commit -m "docs: Phase 1 sniff findings"
```

Exit criterion (from spec): questions (a) and (b) answered from captures. Phase 2 design starts from the findings doc.
