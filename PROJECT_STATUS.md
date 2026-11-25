# PC overlap model v4 — 2026-11-12

The active PC model now uses robust magnitude-before-median preprocessing,
small magnitude/direction ensembles and one-second feature aggregation.
Nested whole-trial development replay on the same 194 attempts improved
within-0.5-cm success from 177/194 (91.2%, v3) to 184/194 (94.8%, v4),
including rejections as misses. Completed-result MAE fell from 0.188 to
0.151 cm. The final model was refitted on all 97 dataset-2 trials.
These reused-session scores do not establish fresh-session or unmeasured
half-centimetre accuracy. The Measure button still takes one second and
retains its last completed result. All CSVs, recordings and prior models
remain intact. No firmware was flashed. See models/v4/README.md.

# PC-side stream capture — 2026-11-11

New batch recordings now collect incoming raw USB events in the PC Python
receiver for 3 seconds. They do not send ESP32 record commands or download its
RAM recorder. Existing session/trial data is preserved. Live frames are
deduplicated, journaled to the PC, then committed to SQLite. Interrupted/partial
PC captures can be retained/cleared; complete failed saves can be retried.
Legacy board-capture recovery remains for old interrupted recordings only.

Verified against the real stream using an isolated temporary dataset: 142
frames at 47.64 received Hz, with only GET /sensors requested and zero device
record/export commands. The real dataset (11 trials before restart) was not
used for synthetic test captures. Acquisition remains 50 Hz; USB event delivery
can be slower and sequence/timestamp gaps remain in the data. Temperature is
NaN where absent from the live stream. Nineteen session/HTTP checks passed.

# PC batch sessions — 2026-11-11

The running USB dashboard now saves manual distance sections and repeated
3-second trials on the PC in recordings/sessions.sqlite3. Each distance has
independent stable trial numbers and its own CSV; Export all downloads those
CSVs in one ZIP. Trials can be included/excluded without removing data.
Sessions, distances and trials can be renamed/removed, with deletion
confirmation. Corrected distance labels update CSV distance_cm and ref_mm;
trial names export as trial_name. Deleted numbers/IDs are not reused and
removed retained captures are not silently re-imported.

Clicking a trial loads its saved raw graph with actual elapsed timestamps on
a 0–3 second axis. Back to live resumes the sensor stream. Recorded graphs
remain viewable when USB readings are stale. Capture/saving/recovery states
block overwrites and unsafe label/deletion edits. A failed save is retried
without repeating the record command. Data is retained across refreshes and
server restarts. Existing sessions upgrade in place; a consistent backup was
made under recordings/backups before adding the trial-editing schema.

The board's retained real 2.0 cm recording was preserved as Trial 1: 149 frames,
596 valid sensor rows. Its saved graph was checked against those timestamps.
No synthetic training trials were added to the real dataset. Live acquisition
remains about 50 Hz on COM3; this update needs no firmware change.

Verification: 65 Python checks plus 35 native C++ assertions passed, including
session durability, failed-save recovery, CSV/ZIP grouping, rename/delete,
migration, stable IDs/numbers, CSV formula-prefix escaping and HTTP access
checks. Browser workflows, recorded-graph selection, raw metric binding and
all five views passed desktop/390 px checks. The final graph was visually
checked using the user's saved trial. Model accuracy remains unvalidated.

# Current update — 2026-11-11

Raw-data collection build is flashed and hash-verified on COM3. The restarted
USB dashboard runs at http://127.0.0.1:8765/#/measure, with a realtime raw
Bx/By/Bz/|B| graph, 3-second labeled recordings, and raw-only CSV downloads.
Calibration, profile selection and classification controls are removed from
the dashboard. Final PC-trained model integration is deferred.

Real-board check: 50.0169 Hz, 149 frames / 596 sensor rows in a 3-second
window, 19/20/21 ms min/median/max frame intervals, all four sensors OK, and
zero reported I2C errors. Magnitudes matched sqrt(Bx²+By²+Bz²). The synthetic
reference-zero check was cleared by restart and was not saved as training data.
The ordered RAM recorder holds 256 frames. USB runs at 921600 baud; fast
sensor settings are OSR=0 and DIG_FILT=0 on the existing 100 kHz I2C bus.

The OLED now shows raw sensor availability and updates on status changes
between trials. The user confirmed **4/4 sensors online** after the fix.
Continuous display refresh is paused at 50 Hz to avoid blocking capture.
The negative-amplitude chart bug was reproduced and fixed: selected metric
and sensor persist across navigation; raw legends match raw chart buffers.
Signed axes are retained; raw magnitude remains non-negative.

Verification: firmware and LittleFS builds passed; 46 Python checks and the
35 native C++ tests passed. Raw recording and chart regression checks passed;
all six views passed desktop and 390 px layout checks. The live raw graph was
visually checked with real sensor data. Distance-model accuracy is not tested.

The older records below describe historical workflows and hardware states.

# Current update — 2026-11-09

Implemented magnetic fingerprint classification and light dashboard. OLED CH0,
magnetometers CH2–5. See [calibration](docs/calibration.md) for the current
workflow and [API](docs/api.md) for changed contracts. Numeric distance models
are inactive. `partitions.csv` adds 64 KB fingerprint NVS while preserving
original settings/Wi-Fi NVS and filesystem offsets. No serial devices are
currently detected; hardware bring-up and real accuracy remain unverified.

Verification: firmware/LittleFS/diagnostic builds passed; 38 Python checks and
35 native C++ tests passed. Browser feature checks and all seven views passed
at desktop and 390 px mobile. Independent review findings were fixed and
rechecked. Firmware was not flashed.

The status record below is historical, including its wiring and port claims.

# SCRATCHSOR — Project Status

**Date:** 2026-10-31 · **Firmware:** 0.1.0 · **Repo:** `D:\SCRATCHSOR` (local git, no remote)

> Hardware update: an ESP32 is now connected on **COM7**. MUX, one sensor
> on CH0 and OLED on CH5 passed the 100 kHz diagnostic scan; the user
> confirmed OLED text with the main firmware. See
> [bench evidence](docs/bench-2026-10-31.md) for failures and current checks.
> Updated firmware and LittleFS have now been flashed and hash-verified on
> COM7. The 100 kHz main application reports 1/4 valid sensors, allocates the
> 128-frame recorder, and the user confirmed the new OLED screen. Host tests:
> 27 passed. Wi-Fi fixture configuration is implemented; physical gap accuracy
> requires the user's measured calibration dataset.
> Follow-up sensor fixes: STOP-separated commands plus a 150 ms shared-bus
> lock wait. Real board at 30/60 s: 1/4 valid, 10.0 Hz, successful samples
> 287→587 and **err=0** in both snapshots, with OLED active.
> The original build/test inventory below is historical; its no-hardware
> statements do not describe the current bench connection.

## Implemented features

- **PlatformIO project** (`esp32dev`, espressif32@6.7.0, Arduino 2.0.16, gnu++17, LittleFS). Libs resolved: U8g2 2.36.18, ArduinoJson 7.4.3, WebSockets 2.7.3.
- **Hardware layer**: mutex-serialized I2C manager (`I2CBusGuard`, channel re-selected per transaction), PCA9548A driver (detect, per-channel scan, channel map), MLX90393 driver (0x0C–0x0F probe, RT reset, SM/RM/RR/WR, datasheet sensitivity table + tconv lookup + temp formula, saturation detection, 3-strike auto-recovery), OLED manager (U8G2, SSD1306/SH1106 runtime selection, 7 screens, rate-limited auto-rotation).
- **Acquisition**: dedicated FreeRTOS task on core 0, ~10 Hz configurable frame pacing, per-sensor filter chains (median→moving-avg→low-pass→baseline, σ-gated outliers, Welford noise estimate → quality), measured-rate EMA, bounded 600-frame RAM recorder, deferred config updates at frame boundaries, degraded modes (0 sensors → idle poll).
- **Networking**: AP+STA+AP_STA state machine (never blocking, bounded timeouts + backoff, never-stranded fallback), per-device generated AP password (NVS-persisted, OLED-only display, doubles as admin token), captive DNS + OS-probe endpoints, `WebServer` REST (18 endpoints, `X-SCRATCHSOR-Token` + Origin/Referer checks), WebSocket :81 (non-blocking broadcast, heartbeat, stale-friendly).
- **Services**: NVS persistence (versioned blobs + CRC-16), calibration state machine (baseline → collect → fit → validate), dipole-linearized OLS fits (DIPOLE_LINEAR/QUAD), held-out-only validation (rmse ≤ 5 mm, maxErr ≤ 15 mm), honest measurement states — **no distance is ever shown without a validated model**.
- **Web UI** (~93 KB, zero external deps): dark instrument SPA, 7 views (Overview/Live Sensors/Calibration/Measurement/Wi-Fi/Settings/Export), Canvas charts, WS reconnect w/ backoff + HTTP-poll fallback, stale-data dimming, mobile-first.
- **Docs** (8 files): architecture, wiring (w/ cited CJMCU-90393 CS/pull-up research), setup, wifi, calibration, validation matrix, troubleshooting, API contract.
- **Tooling**: `tools/serial_diagnostics.py` (passive monitor/parser; `py_compile` ✓, replay-tested).

## Verified (real evidence)

| Check | Result | Evidence |
|---|---|---|
| Test A — firmware builds | **PASS** | `pio run` → SUCCESS; `firmware.bin` 963,309 B = 73.5 % of 1.31 MB app partition; RAM 50,544 B static (15.4 %) |
| Test A — LittleFS fits | **PASS** | `pio run -t buildfs` → `littlefs.bin` 1,441,792 B built; data/ ≈ 93 KB used |
| Native tests — signal processing | **PASS** | 12/12 Unity tests (zig c++ host build — see caveat) |
| Native tests — calibration math | **PASS** | 10/10 Unity tests (fit recovers y=2x+1, x², dipole model; split/metrics/domain) |
| Python tool | **PASS** (static) | `py_compile` + `--replay` on synthetic logs |

**Test runner caveat:** `pio test -e native` needs a host gcc/g++ — none installed. Tests were compiled/run instead via `ziglang` (pip) against `.pio/libdeps/native/Unity`. Same sources, same Unity asserts; `pio test` will work once a host toolchain (MinGW/MSVC) exists.

## NOT TESTED (no hardware attached)

- Tests B–I of `docs/validation.md` entirely: serial/boot logs, mux+sensor+OLED detection, acquisition rate, AP visibility/captive portal/phone connect, STA provisioning, OLED screens, browser dashboard, calibration workflow on real data.
- Flash size is healthy but leaves ~350 KB headroom — fine for updates, tight for dual-OTA.

## Untested hardware assumptions (verify before first power — docs/wiring.md §7/§9)

- CJMCU-90393 **CS pull-up state is batch-dependent** — must be verified/tied to 3V3 per board.
- CJMCU-90393 is **3.3 V-only** (no on-board regulator assumed) — never feed 5 V.
- MLX90393 **variant option codes**: firmware scans 0x0C–0x0F only; ABA-014 parts answer at 0x18–0x1B (extend `kMlxAddrMin/Max` if sensors aren't found).
- OLED controller defaults to SSD1306; override via Settings → controller 2 (SH1106) if columns look shifted by 2 px.
- AP password lives only on the OLED — a headless (no-OLED) build cannot recover the admin token without reflash/erase (documented in wifi.md).

## Outstanding defects / risks

- Conversion-time margin is an estimate (tconv × 1.2 + 1 ms); if RM-before-ready errors appear, raise the margin — watch `[W]` logs.
- Median/moving-average window sizes in FilterConfig are advisory — fixed capacities (MA≤8, Median≤9).
- `subscribe` WS filtering accepted but not enforced (all clients get all frames).
- Web UI unverified in a real browser against a live device.

## Required human actions (next milestone gate)

1. Wire per `docs/wiring.md` incl. continuity + CS checks.
2. Flash: `pio run -t upload` then `pio run -t uploadfs`.
3. Boot → confirm `Magnetometers online: 4/4` + OLED AP_SETUP screen.
4. Phone → `SCRATCHSOR-XXXX` → `http://192.168.4.1` → record Test E/F results.
5. Run calibration protocol (`docs/calibration.md`) — only then does distance estimation produce numbers.

## Blockers

- **No ESP32/sensor hardware on this machine** (COM3=Intel AMT, COM4/5=Bluetooth only). Everything hardware-tagged above waits on the bench.

## Next recommended engineering tasks

1. Hardware bring-up per `docs/validation.md` Test B–D.
2. Extend MLX address scan to 0x18–0x1B if needed.
3. Consider `/api/v1/calibration` GET endpoint exposing points+model (frontend currently session-tracks them).
4. OTA-capable partition scheme if over-the-air updates are wanted.
