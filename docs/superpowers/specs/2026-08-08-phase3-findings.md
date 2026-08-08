# Phase 3 Findings — Cover + Closed-Loop Move-to-Height

**Date:** 2026-08-08. Design: `2026-08-08-phase3-cover-design.md`. All results from live verification after calibration (travel 71.0–121.0 cm).

## Accuracy (live moves, default tunables)

| Target (cm) | Landed | Error | Notes |
|---|---|---|---|
| 79.5 | 79.5 | 0.0 | cold start: wake 492 ms → done in 4 s total |
| 85.0 | 85.1 | +0.1 | awake, up |
| 75.0 | 74.9 | −0.1 | awake, down |
| 71.0 | 71.1 | +0.1 | cover close → bottom rail |
| 83.5 | 83.6 | +0.1 | mid-range |
| 121.0 | 121.0 | 0.0 | full 37.5 cm climb (~18 s), top rail |

Max error = one display quantum (0.1 cm). **Zero fine-correction taps were needed in any move** — the default `coast_margin` 0.7 cm matches this desk's real coast almost exactly; COARSE + coast alone lands inside the 0.3 cm deadband. The FINE/tap stage exists as insurance (and is sim-verified), but on this hardware it never fires.

## Behaviors verified live

- **Physical override:** keypad Down pressed ~1.8 s into an automated 31 cm descent → `physical key 02 overrides move-to-height; aborting`, instant. Human always wins.
- **Cold-start moves:** target set while desk asleep → PIN 20 wake (~490 ms to polling), move runs, desk re-sleeps on its own.
- **Cover semantics:** position slider, open (→121.0), close (→71.1), all landing on the rails; position math verified against calibration (75.0 cm ↔ 0.246 pre-calibration, correct per formula).
- **Sleep cycles between moves** are handled transparently — back-to-back commands mix awake and cold-start paths without special-casing.

## Not exercised live (sim-verified only)

Stall abort (would require blocking the desk), move timeout, tap-limit exhaustion. All covered by `tests/test_protocol.cpp` against the SimDesk model.

## Tuning outcome

Ship defaults unchanged: `coast_margin` 0.7, `deadband` 0.3, `settle_ms` 1200, `move_timeout_ms` 30000, `stall_ms` 2000, `max_taps` 5. All exposed as component config for other desks (a heavier/lighter desk will coast differently — that's the knob to turn if landings start needing taps).

## Status

Phase 3 complete. The desk is a full HA citizen: cover with position, target-height box, presets, jogs, working stop, height telemetry, wake-on-demand, boot height fetch — all through a MITM that leaves the physical keypad factory-perfect.
