# SCRATCHSOR validation protocol

Purpose: a repeatable, honest record of what the device actually does on
real hardware. The rule set:

- **PASS** = you observed it working, with evidence (serial log, screenshot,
  API response) noted.
- **FAIL** = you ran it and it did not do the thing.
- **NOT TESTED** = not attempted yet — the honest default.
- **BLOCKED** = attempted but prevented by a stated external cause (driver,
  missing part, missing instrument).
- "Firmware exists for X" is never a substitute for running X.

Keep one filled log per hardware revision / firmware version. Suggested
location: `work/validation/` (scratch area).

## Test matrix A–I

### A. Build & flash (host)

| # | Step | Expected | Result |
|---|---|---|---|
| A1 | `pio run` | firmware builds clean, no warnings beyond upstream libs | |
| A2 | `pio test -e native` | all host unit tests pass | |
| A3 | `pio run -e esp32dev -t upload` | flashes at 115200/460800, verifies | |
| A4 | `pio run -e esp32dev -t buildfs && -t uploadfs` | LittleFS image written | |

### B. I2C / mux (bench, serial monitor open)

| # | Step | Expected | Result |
|---|---|---|---|
| B1 | boot banner | `[I] SCRATCHSOR firmware 0.1.0` first | |
| B2 | mux detect | `PCA9548A found at 0x70` | |
| B3 | channel map | CH0–3 sensors and CH5:0x3C (or 0x3D) for full setup; current bench: CH0 sensor + CH5 OLED, other channels empty | |
| B4 | bus health | no repeated `[E]`/timeouts during 60 s idle | |

### C. Acquisition

| # | Step | Expected | Result |
|---|---|---|---|
| C1 | sensor count | `Magnetometers online: 4/4` | |
| C2 | frame rate | `GET /api/v1/health` `rate` ≈ configured Hz ±10% | |
| C3 | field response | wave a magnet near sensor 0 → its `x/y/z/m` move, others mostly still | |
| C4 | status honesty | unplug one sensor mid-run → its `st` leaves `OK`, `validCount` drops | |
| C5 | recovery | replug it → returns to `OK` without reboot | |

### D. AP mode + portal

| # | Step | Expected | Result |
|---|---|---|---|
| D1 | SSID | `SCRATCHSOR-XXXX` visible on a phone | |
| D2 | password | OLED AP_SETUP password joins the AP | |
| D3 | portal | captive portal opens on ≥1 OS, or `http://192.168.4.1` loads | |
| D4 | DNS wildcard | any hostname resolves to the portal while on AP | |

### E. STA provisioning

| # | Step | Expected | Result |
|---|---|---|---|
| E1 | scan | `/api/v1/wifi/scan` returns your LAN SSID with rssi | |
| E2 | connect | `/api/v1/wifi/connect` → CONNECTED + ip within 15 s | |
| E3 | persistence | reboot → STA reconnects without input | |
| E4 | wrong password | FAILED with `sta.error` string; AP still reachable | |
| E5 | channel hop | AP client sees ≤ ~2 s drop when STA associates | |

### F. OLED

| # | Step | Expected | Result |
|---|---|---|---|
| F1 | boot screen | BOOT screen shows at power-on | |
| F2 | AP_SETUP | password legible | |
| F3 | rotation | screens rotate every `rotateSec` | |
| F4 | readings | READINGS screen tracks sensor values | |
| F5 | controller check | no clipped columns; if 2-px offset → SH1106 override applied and retested | |
| F6 | headless | OLED unplugged → `[W]` + everything else still runs | |

### G. Dashboard / REST / WS

| # | Step | Expected | Result |
|---|---|---|---|
| G1 | dashboard load | `/` serves UI from LittleFS on AP | |
| G2 | live tiles | sensor values update ~10 Hz | |
| G3 | WS stream | devtools shows `sensors` frames flowing on :81 | |
| G4 | auth | mutating POST without token → `{"error":"unauthorized"}` | |
| G5 | auth | with `X-SCRATCHSOR-Token: <pw>` → `ok:true` | |
| G6 | health JSON | `/api/v1/health` matches serial boot report | |

### H. Calibration

| # | Step | Expected | Result |
|---|---|---|---|
| H1 | baseline | `{"phase":"baseline"}` completes; baseline stored | |
| H2 | collect | ≥10 distances, ≥30 samples each; points listed | |
| H3 | fit | `fit` returns `validated:true`, rmse ≤5 mm, maxErr ≤15 mm, nValid ≥2 | |
| H4 | estimate | state VALID in-range, `mm` plausible vs. physical distance | |
| H5 | out-of-range | magnet beyond span → `OUT_OF_RANGE`, not a bogus number | |

### I. Recorder / export

| # | Step | Expected | Result |
|---|---|---|---|
| I1 | record | `/api/v1/record {"active":true}` → count grows | |
| I2 | CSV | `export.csv` opens clean; columns match `docs/api.md` | |
| I3 | JSON | `export.json` parses; meta has fw + calib version | |
| I4 | bounds | record past 128 frames → count caps, no crash | |

## Log template

```
SCRATCHSOR VALIDATION LOG
Date:             ____-__-__   Tester: ____________
Firmware:         kFirmwareVer ___.___.___  (git commit ____)
Hardware:         board rev ____  sensors: CJMCU-90393 x4 batch ____
                  mux: PCA9548A/TCA9548A module ____  OLED: SSD1306/SH1106
Environment:      bench / field;  Wi-Fi LAN: ____

| Test | Status (PASS/FAIL/NOT TESTED/BLOCKED) | Evidence / notes |
|------|--------|-------------------|
| A1   |        |                   |
| ...  |        |                   |

SUMMARY:  __/43 PASS   __ FAIL   __ NOT TESTED   __ BLOCKED
Open issues:
1.
Verified capabilities (observed):
-
Assumed / not yet demonstrated:
-
```

## Demonstrated vs assumed — standing notes

Until a full log exists, treat these as **assumed**, not demonstrated:

- 100 kHz bus stability with all 4 sensors + OLED over long Dupont runs.
- Conversion-time polling jitter vs. the INT-based timing the driver
  doesn't use.
- Calibration accuracy on anything but the exact fixture/orientation it
  was fitted on.
- AP+STA coexistence under sustained WS client load (>2 clients).
- Field behaviour near real-world interferers (steel furniture, speakers).
