# SCRATCHSOR troubleshooting

Symptom → likely cause → fix. Start with the serial log (`pio device
monitor -b 115200` or `tools/serial_diagnostics.py`) — the boot report
localizes almost every hardware fault.

## I2C / sensor issues

| Symptom | Cause | Fix |
|---|---|---|
| `PCA9548A not found` | SDA/SCL swapped; mux unpowered; A0–A2 not grounded (address ≠ 0x70–0x77); RST floating low | re-check wiring table; measure VCC at mux; confirm A0–A2 low + RST high (`docs/wiring.md` §7) |
| Mux found, all 4 sensors `NOT_FOUND` | sensor 3V3 rail unpowered; **CS floating on the CJMCU-90393s** (classic); channels not reaching SDn/SCn | verify sensor VCC pins; measure CS→3V3 per board, wire CS→3V3 if open; probe one sensor on a known-good channel |
| Some sensors found, some not | individual CS floating; bad Dupont contact; dead module | swap channels to isolate module-vs-wiring; re-crimp/re-seat; the bad module's `st` will follow it |
| Sensor found but `st` = NACK/TIMEOUT intermittently | marginal pull-ups, long leads, 400 kHz too fast for your wiring | shorten leads <10–15 cm; cut bus to 100 kHz (`kI2cClockHz` in `config.h`) |
| Sensors ACK at wrong address | module built on a different MLX90393 option code (0x18 base — post-May-2023 parts) | run a raw I2C scan on the channel; if at 0x18–0x1B, widen `kMlxAddrMin/Max` — otherwise expected at 0x0C–0x0F |
| `mux map` shows a stray device on a sensor channel | two things on one channel | sensors on CH0–3, OLED on CH5; CH4 and CH6–7 should be empty |
| Readings frozen but sensor `OK` | filter chain saturated after a big step; outlier gate rejecting real signal | drop `outlierSigma` sensitivity via config; power-cycle filters via restart |
| All readings shifted vs. expectation | baseline captured with a magnet present | re-run baseline with magnets removed (`docs/calibration.md` §3) |
| Wild µT spikes when touching wires | bad ground or flaky contact | re-seat GND; suspect the breadboard rail |

## OLED issues

| Symptom | Cause | Fix |
|---|---|---|
| `No OLED on CH5 — continuing headless` | OLED off CH5; wrong address variant; dead module | OLED SDA to **SD5**, SCL to **SC5**; firmware probes 0x3C + 0x3D only — check your module's spec |
| OLED blank but detected (`OLED at 0x3C`) | SH1106 panel running SSD1306 init (draws wrong) or contrast | try `oled.controller: 2` (SH1106) in Settings — signature is a 2-px right-edge offset/clip with SSD1306 |
| 2-pixel column shift, right edge clipped | SH1106 in SSD1306 mode | `POST /api/v1/config {"oled":{"controller":2}}` |
| Display upside down | module orientation | `oled.flip: true` |
| OLED flickers / tears under load | I2C contention with acquisition | cosmetic — refresh is rate-limited to `refreshHz`; if severe, lower `kSampleRateHz` |
| AP password screen gone | screen auto-rotated | `AP_SETUP` is pinned at boot; `oled.rotateSec` cycles it away — it comes back; or re-pin via `show` logic by rebooting |

## Wi-Fi / networking issues

| Symptom | Cause | Fix |
|---|---|---|
| Can't see `SCRATCHSOR-XXXX` | STA_ONLY mode with STA down; AP disabled via `apPersistent:false` | power-cycle to force boot defaults if config got persisted badly; check `GET /api/v1/wifi/status` → `mode` |
| Captive portal doesn't pop | OS variance (Windows/Android silent) | browse `http://192.168.4.1` — always works on the AP |
| Portal page loads then dies | OS dropped the no-internet Wi-Fi | "keep connection" prompt on Android; disable cellular-assist fallback temporarily |
| AP drops when STA connects | **channel sharing** — radio follows the router's channel | expected; rejoin AP after hop (`docs/wifi.md` §4) |
| STA `FAILED` with reason string | wrong password, 5 GHz-only network (ESP32 is 2.4 GHz), wrong SSID | check `sta.error`; ensure 2.4 GHz SSID |
| `ws://…:81` connects but no sensor frames | no subscribers / `subscribe` filter; or acquisition down | send `{"t":"ping"}` → expect `pong`; check `/api/v1/health` rate |
| REST returns `unauthorized` | missing/wrong `X-SCRATCHSOR-Token` | token = AP password on the OLED setup screen |
| REST returns `forbidden_origin` | cross-origin call | call from the device-served page or strip `Origin` |

## Stability / rate issues

| Symptom | Cause | Fix |
|---|---|---|
| Random reboots under Wi-Fi load | USB cable droop / brown-out | better/shorter cable, powered hub; check serial for `Brownout` lines |
| `rate` well below configured Hz | conversion time too long for the frame budget (high OSR/DIG_FILT/resolution), or lock contention | lower `osr`/`digFilt`/`resolution` in sensor config; `conversionTimeMs` grows fast — at 40 Hz cap there's little headroom |
| Sample rate drops after config change | new settings exceed frame period | same as above — `GET /api/v1/health` `rate` shows the real number |
| I2C lockups / all-sensor timeouts | stuck bus (a sensor holding SDA low) | power-cycle is definitive; firmware `recover()` does RT reset; if recurring, check for a marginal module |
| Heap shrinking / crash after hours | leak or recorder saturating | check `heap` in `/api/v1/device` over time; recorder is bounded at 128 frames |

## Calibration issues

| Symptom | Cause | Fix |
|---|---|---|
| `fit` returns `validated:false` | not enough held-out points or model genuinely poor | ≥8 distances spread over the range; check `nValid` ≥2 |
| rmse huge (>15 mm) | fixture moved between points; inconsistent orientation; interfering metal | rebuild dataset with fixed orientation and a clean bench (`docs/calibration.md`) |
| Estimate stuck `OUT_OF_RANGE` | current field outside the fitted 10–100 mm span | it's honest — bring the magnet into range or recalibrate |
| Estimate stuck `NOT_CALIBRATED` | no validated model (expected before cal), or `calibration/reset` was called | run the calibration flow |
| Baseline took but readings look wrong | something magnetic was present during baseline | re-baseline with a verified clean bench |

## Still stuck?

1. Capture a full boot log: `python tools/serial_diagnostics.py --port COMx --log boot.log`.
2. `GET /api/v1/health` JSON alongside it.
3. Fill the validation log (`docs/validation.md`) — the FAIL lines *are* the bug report.
