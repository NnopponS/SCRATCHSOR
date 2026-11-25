# PC batch-session API (USB dashboard only)

The loopback PC server adds the following routes under `/api/v1`. They do not
require a firmware update. All have the existing local Host/Origin checks;
starting a recording also requires the device admin token. Saved trials live
in `recordings/sessions.sqlite3`, outside the served dashboard assets.

- `GET /sessions` or `/sessions/{id}`: session list, manual distance sections,
  saved trial summaries, inclusion flags, and pending recording/save status.
- `POST /sessions {name}`: create a new dataset without deleting previous sessions.
- `POST /sessions/{id}/distances {distanceCm:"2.0"}`: add/select a manual
  distance section; equivalent labels such as 2.0 and 2.00 share one section.
- `POST /sessions/{id}/record {distanceId:1}`: start one 3-second capture,
  then download/validate/save it in the background. Trial numbering is independent
  per distance. A pending/failed save blocks another capture.
- `POST /sessions/{id}/retry {}`: retry reading/saving an interrupted trial;
  the device record command is never repeated.
- `POST /sessions/{id}/recover {}`: preserve any retained completed board data
  before clearing a failed capture attempt.
- `POST /sessions/{id}/trials/{trialId} {included:false}`: reversible export
  exclusion; raw data and trial numbering are retained.
- `GET /sessions/{id}/distances/{distanceId}.csv`: all included trials for that
  distance. Batch columns prepend `session_id,distance_cm,trial_no,trial_name,saved_at` to
  the 15 raw sensor columns. `saved_at` is UTC, not the sensor's uptime timestamp.
- `GET /sessions/{id}/export.zip`: one CSV per distance with included trials.
  This reads the PC database; it does not use the board RAM export buffer.

- `GET /sessions/{id}/trials/{trialId}` returns saved trial metadata and raw
  frames with elapsedMs and four sensors' raw rx/ry/rz/rm/status/valid/sat fields.
- `POST /sessions/{id}/rename {name}` and `/remove {}` rename/remove a session.
- `POST /sessions/{id}/distances/{distanceId}/rename {distanceCm}` corrects a
  section's reference label; conflicts with another existing distance return 409.
  `/remove {}` removes that section and its trials.
- `POST /sessions/{id}/trials/{trialId}/rename {name}` updates an optional trial
  name without changing its number. `/remove {}` deletes its raw data.
  Session/distance/trial rename/removal returns 409 while a capture is pending.
  Deletion does not reuse trial numbers or internal IDs. The PC database upgrades
  existing sessions in place to retain their trial data and numbering.

## Earlier on-device fingerprint API (retained backend)

The existing `/api/v1` prefix, token authentication, Wi-Fi routes and USB
transport remain. These definitions replace the historical distance-specific
contracts below. Mutating requests require `X-SCRATCHSOR-Token` equal to the
AP Wi-Fi name/password. Errors retain `{error, detail}` and non-2xx status.

- `GET/POST /config`: add `reach:{profile,selected,setup}`. Profiles 0–3 mean
  male/right, male/left, female/right, female/left. `selected` is boolean;
  setup is at most 95 UTF-8 bytes without control characters. Signal or setup
  changes with saved data return 409 unless `resetCalibration:true` is present.
  No config changes during a capture. Baseline removal must remain enabled.
- `GET /calibration`: phase, detail, baselineReady, ready, profileId, selected,
  setup, count/max, collected/target, profiles, points and model. Points contain
  id, profileId, refMm, role (`train`/`test`), category. Model contains KNN3 type,
  ready, accuracy, trainCount/testCount[3], recall[3], confusion[actual][predicted].
- `POST /calibration/start`: `{phase:"baseline",resetCalibration:true}`
  explicitly replaces baseline/datasets, or `{phase:"collect",refMm:-25,
  role:"train",repositioned:true}` captures a physical trial. `refMm` is a finite
  number from −500 through +500, including zero; web inputs use cm × 10.
  Collection is fixed at 20 complete fresh frames. Baseline uses 100.
- `POST /calibration/fit`: require at least 8 train and 2 test per category in
  the active profile, activate KNN and return ready/model. Scores are advisory.
- `POST /calibration/delete {id:123}` removes that trial; its profile needs
  validation again. `POST /calibration/cancel` stops a running capture.
  `POST /calibration/reset` clears all four profiles/baseline; cancel first.
- `GET /calibration.csv`: all averaged fingerprints and their ground-truth
  reference/category/profile/setup/role/ID. Feature values use 9 significant
  digits for float round-trip. Setup cells are CSV-quoted; spreadsheet formula
  prefixes are escaped with an apostrophe.
- `GET /measurements` and streamed `sensors.est`: `{state,category,profile,
  profileId,selected,ready,conf,ts}`. `category` is a label or null; no numeric
  distance is present. States used: NOT_CALIBRATED, CALIBRATING, WARMING_UP,
  VALID, SENSOR_ERROR. `conf` is nearest-neighbor vote agreement, not probability.
  Sensor data older than 1000 ms reports TIMEOUT. WS is `ws://host:81/ws`.
- `POST /record`: `{active:true,refMm:50}` starts a fixed 3000 ms training
  trial labeled with a finite measured reference from −500 to +500 mm.
  No calibrated model or profile is required; at least one fresh healthy sensor
  is required. An active recorder, missing buffer or unhealthy/stale sensors
  returns 409 without clearing the prior trial. Missing/non-numeric labels or
  non-boolean `active` return 400. `{active:false}` stops early. New starts
  replace the retained trial; settings and baseline changes are blocked while
  recording. WebSocket record commands require the same `refMm` label.
- `GET /measurements.record`, record events and POST responses include
  `active,count,max,durationMs,remainingMs,refMm,startedMs`. Completion/progress
  events are sent automatically; GET recovers missed events.
- `/export.csv` is now raw-only:
  `seq,ts,id,raw_x,raw_y,raw_z,raw_m,st,ref_mm,elapsed_ms,trial_start_ms,valid,saturated,temp_c,quality`.
  `raw_x/raw_y/raw_z/raw_m` are Bx/By/Bz/|B|. Filtered `x/y/z/m` columns have
  been removed. Magnetic units are µT, reference units mm, temperature °C.
  Float columns use 9 significant digits. Leading `#` lines remain metadata.
  `/export.json` adds `meta.refMm,startedMs,durationMs` to its frames/config.
  Export after the automatic stop (or an explicit early stop); active exports
  return 409. Downloads retain data. Starting another trial or restarting clears
  the RAM recording. See [USB training collection](usb-dashboard.md).

All paths above are relative to `/api/v1`. Wi-Fi HTTP and USB invoke the same
registered handlers. USB downloads include `/calibration.csv`.

---

## Historical distance API reference (inactive calibration/measurement contract)

# SCRATCHSOR API contract (v1)

The current bench build also exposes `GET /api/v1/calibration` and a `gap`
object in `GET/POST /api/v1/config`. See [magnet gap configuration](magnet-gap.md)
for fields, fixture requirements, persistence and calibration reset behavior.

Authoritative field names: `include/protocol.h`. Base URL: `http://192.168.4.1` (AP) or device STA IP. All JSON.

## Auth

Mutating `POST`s require header `X-SCRATCHSOR-Token: <AP password>` and same-origin (or absent) `Origin`/`Referer`. GETs are open on the device's own network. STA password is never exposed.

Error body: `{"error":"<code>","detail":"<text>"}` — codes: `unauthorized`, `forbidden_origin`, `bad_request`, `busy`, `not_found`, `invalid_state`, `internal`.

## REST endpoints

| Method | Path | Description | Response |
|---|---|---|---|
| GET | `/api/v1/device` | Identity + firmware | `{"name":"SCRATCHSOR-XXXX","product":"SCRATCHSOR","fw":"0.1.0","api":"v1","uptimeMs":123,"heap":180000}` |
| GET | `/api/v1/health` | Hardware health | `{"mux":true,"muxAddr":112,"oled":true,"oledAddr":60,"controller":"SSD1306","rate":9.8,"sensors":[{"id":0,"st":"OK","ok":12,"err":0},...]}` |
| GET | `/api/v1/sensors` | Latest frame | `{"seq":12,"ts":1234,"rate":9.8,"s":[{"id":0,"x":..,"y":..,"z":..,"m":..,"rx":..,"ry":..,"rz":..,"rm":..,"st":"OK","sat":false,"q":0.9},x4]}` (x/y/z/m filtered, r* raw) |
| GET | `/api/v1/config` | Config (no secrets) | `{"deviceName":..,"apPersistent":true,"sensor":{"gain":7,"resolution":0,"osr":2,"digFilt":2,"rateHz":10,"temp":true},"filter":{"avg":true,"avgWin":4,"median":false,"medWin":5,"lp":true,"alpha":0.35,"baseline":true,"outlierSigma":4},"oled":{"refreshHz":4,"rotateSec":5,"flip":false,"controller":0}}` |
| POST | `/api/v1/config` | Merge-update config | same body subset; applied live; persisted |
| GET | `/api/v1/wifi/status` | Wi-Fi state | `{"mode":"AP_STA","sta":{"state":"CONNECTED","ssid":"..","ip":"..","rssi":-52,"error":""},"ap":{"ssid":"SCRATCHSOR-1A2B","ip":"192.168.4.1","clients":1}}` |
| GET | `/api/v1/wifi/scan` | Trigger+read scan | `{"done":true,"nets":[{"ssid":"..","rssi":-50,"open":false}]}` (`done:false` = still scanning, poll again) |
| POST | `/api/v1/wifi/connect` | Body `{"ssid":"..","pass":".."}` → bounded connect attempt | `{"ok":true,"state":"CONNECTING"}` |
| POST | `/api/v1/wifi/forget` | Disconnect + clear STA creds | `{"ok":true}` |
| POST | `/api/v1/calibration/start` | `{"phase":"baseline","samples":100}` or `{"phase":"collect","refMm":50,"samples":60}` | `{"ok":true,"phase":"BASELINE"}` |
| POST | `/api/v1/calibration/fit` | Split + fit + validate | `{"ok":true,"validated":true,"model":{"type":"DIPOLE_LINEAR","coeffs":[..],"mae":2.1,"rmse":3.0,"maxErr":6.4,"range":[10,100],"nTrain":6,"nValid":2}}` |
| POST | `/api/v1/calibration/reset` | Wipe dataset + model | `{"ok":true}` |
| GET | `/api/v1/measurements` | Estimate + recorder state | `{"est":{"state":"NOT_CALIBRATED","mm":0,"conf":0,"ts":..},"record":{"active":false,"count":0,"max":128}}` |
| POST | `/api/v1/record` | `{"active":true}` | `{"ok":true,"active":true}` |
| GET | `/api/v1/export.csv` | Recorded frames CSV | `seq,ts,id,x,y,z,m,raw_x,raw_y,raw_z,raw_m,st` rows + `# meta` header lines |
| GET | `/api/v1/export.json` | Same as JSON + config + calib version | `{"meta":{...},"frames":[...]}` |
| POST | `/api/v1/restart` | Safe restart | `{"ok":true}` |
| POST | `/api/v1/factory_reset` | `{"keepWifi":true}` | `{"ok":true}` |

Stop recording before exporting (`POST /api/v1/record` with `{"active":false}`).
Both export endpoints return HTTP 409 `recording_active` while recording. Downloads
retain all frames, including after a failed transfer, so CSV and JSON can be saved
from the same recording. Starting a new recording or restarting clears the buffer.

Factory reset accepts an empty body or a JSON object with an optional boolean
`keepWifi` (default `true`). Invalid JSON, other body shapes and non-boolean
`keepWifi` return HTTP 400 without resetting or rebooting.

## WebSocket `ws://<ip>:81/ws`

Server→client frames (field `t`): `hello`, `sensors` (~10 Hz), `status` (heartbeat ~2 s + on change), `calib`, `record`, `error`, `pong`. Shapes defined in `include/protocol.h`.

`sensors` frame:
```json
{"t":"sensors","seq":123,"ts":4567,"rate":9.8,
 "s":[{"id":0,"x":12.3,"y":-4.5,"z":98.7,"m":99.7,"st":"OK","sat":false,"q":0.92}],
 "est":{"state":"NOT_CALIBRATED","mm":0,"conf":0}}
```

Client→server: `{"t":"ping"}`, `{"t":"subscribe","ch":["sensors","status"]}`, `{"t":"record","active":true,"token":"<ap password>"}`.

## Captive portal

DNS resolves all names to AP IP while SoftAP is up. HTTP also answers OS probe paths (`/generate_204`, `/hotspot-detect.html`, `/ncsi.txt`, `/connecttest.txt`, `/redirect`) with the portal index. Direct fallback: `http://192.168.4.1`.
