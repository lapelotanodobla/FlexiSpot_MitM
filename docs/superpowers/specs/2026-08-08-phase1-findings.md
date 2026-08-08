# Phase 1 Findings — HS01B-1 / CB38M2A-1 Protocol Sniff

**Source:** `captures/2026-08-08-session1.log` (annotated: `.md` sibling, decoder: `tools/decode.py`), captured with the passive Y-tap node (`desk-sniffer.yaml`). All frames CRC-verified 9600 8N1, format `9b <len> <type> <payload…> <crc16> 9d`.

## Physical-layer corrections vs the reference docs

- **5 V logic**, not 3.3 V (idle high 4.6 V). Any device transmitting to the controller or keypad at 3.3 V sits at/below the 5 V-logic V_IH — this is the most likely root cause of the "ESP works but keypad dies" failure mode in prior passthrough builds (iMicknl/LoctekMotion_IoT#146).
- **Direction naming is reversed** vs the iMicknl HS01B-1 table on this desk: RJ45 **pin 6 carries keypad→controller**, **pin 5 carries controller→keypad**.
- The keypad **tri-states its TX when asleep** (~1 V float); controller line idles high always.

## Protocol structure: poll/response, not free-running

Frame counts over the session: controller `type 11` = 2486, keypad `type 02` = 2486 — **exactly 1:1, with the keypad's reply ~6 ms after each poll**.

While awake (~31 Hz):

| Frame | Direction | Meaning |
|---|---|---|
| `9B:04:11:7C:C3:9D` | controller→keypad | Key-state poll, ~31 Hz |
| `9B:06:02:<k1>:<k2>:<crc>:9D` | keypad→controller | Key-state reply to every poll |
| `9B:07:12:07:<d2>:<d3>:<crc>:9D` | controller→keypad | Display update (7-segment encoding, bit7 = decimal point), sent on change + periodic refresh |
| `9B:04:15:BF:C2:9D` | controller→keypad | Keepalive, ~2 Hz while awake |
| `9B:04:13:BD:42:9D` | controller→keypad | Shutdown announcement — burst of ~8 in the ~100 ms before PIN 20 falls |

## (a) Keypad command frames — confirmed, full button map

The keypad answers every poll with `9B:06:02:<keys>:00:<crc>:9D`. `<keys>` is a bitmask; observed values match the LoctekMotion command table:

| Button | Payload | Observed |
|---|---|---|
| none | `00:00` | continuous idle |
| Up | `01:00` | tap = 4 frames (~100 ms), hold = continuous |
| Down | `02:00` | same pattern |
| Preset 1 | `04:00` | single ~5-frame burst, desk then moves autonomously |
| Preset 2 | `08:00` | same |
| Preset 3 | `10:00` | same |
| M | `20:00` | display switches to `S-` programming prompt for ~4 s, then reverts |

Key facts: a **preset tap is fire-and-forget** — the desk completes the whole move while the keypad streams `00:00`. Manual up/down moves only while the key code streams; release stops the desk.

## (b) PIN 20 — answered

- **The keypad drives PIN 20.** Rising edge comes first; controller UART starts polling ~140 ms later; first clean frame ~350 ms after the edge.
- Stays high while awake; drops after **~10 s of inactivity** (button presses and motion reset the timer).
- The controller announces the impending sleep with the `type 13` burst, *then* PIN 20 falls, then both transmitters die mid-stream (trailing garbage bytes `9B:FF`, partial frames are normal at power-down and must be discarded).
- Wake transients: the keypad's first ~2 bytes after the rising edge (`00`, `FE`) are garbage — ignore bytes for ~300 ms after the edge.

## (c) Frames absent from the reference docs

`type 11` (poll), `type 13` (shutdown), `type 15` (keepalive) are not in the LoctekMotion tables. Issue #146's mystery frame `9B:05:02:00:A1:60:9D` (len 05) did not appear here — likely a variant firmware's idle reply.

## (d) Phase 2 (active MITM) requirements

1. **Level shift everything** — 5 V toward desk and keypad. 3.3 V TX is not reliably heard.
2. **Split PIN 20**: sense the keypad side (divider→GPIO), drive the desk side (shifter channel). Mirror keypad→desk by default; OR-in ESP's own wake when injecting.
3. **To inject a command with the keypad asleep**: drive desk-side PIN 20 high, wait ≥350 ms for the controller to start polling, then answer polls with the command payload for ~4-5 polls (~150 ms), then stream `00:00` idle replies. Presets are fire-and-forget after that; the move completes on its own. Keep PIN 20 high at least until the move ends (height sensor goes quiet).
4. **The controller is the poll master.** A passthrough MITM must forward polls promptly; the observed poll→reply turnaround is ~6 ms, so per-byte forwarding latency well under that is required. Frame-level store-and-forward at ~1 ms/byte UART speed fits comfortably.
5. **Height telemetry is free**: decode `type 12` display frames — payload is frame bytes 3–5 (after `9B <len> 12`), 7-segment encoded, bit7 = decimal point; 3-digit values ≥100 drop the dot. Decoder validated against session 1: continuous cm readings 75.0–121.0 with no gaps (session presets: P1=121.0, P2≈77.2, P3=75.0). Non-numeric glyphs appear only in the M-mode `S-` prompt.
6. **Hygiene**: discard partial frames after `type 13`/PIN 20 fall; ignore first ~300 ms of keypad bytes after wake.

## Exit criteria

Questions (a) and (b) from the design spec are answered from captures — Phase 1 complete. Not yet captured: preset *programming* exchange (session step 11 skipped); can be added to a later session if Phase 2 needs it.
