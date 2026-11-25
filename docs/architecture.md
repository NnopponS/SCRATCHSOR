> Updated 2026-11-09: the active calibration/measurement path is now the
> fingerprint classifier described in [calibration.md](calibration.md).
> The distance architecture below is historical. Legacy distance blobs remain
> stored but are inactive. OLED CH0; magnetometers CH2–5.

# SCRATCHSOR architecture

ESP32 Arduino firmware. Design goals: single shared I2C bus behind a mux,
lock-step acquisition isolated on one core, every subsystem degraded-safe
(no single failure bricks the device), honest data (timestamps and status
travel with every reading; estimates are never invented).

Source map: `src/main.cpp` wires everything; interfaces live in `include/`
under `hardware/`, `services/`, `networking/`, `diagnostics/`.

## 1. Module diagram

```
                        ┌───────────────────────────────────────────┐
                        │              src/main.cpp                 │
                        │   setup(): ordered bring-up  loop(): pump │
                        └───────┬───────────────────────┬───────────┘
                                │                       │
        ┌───────────────────────┼───────────┐   ┌───────┼───────────────────┐
        │ hardware layer        │           │   │ services layer          │
        │                       │           │   │                         │
        │  I2CManager (gI2c)    │           │   │  SensorService          │
        │   Wire + mutex +      │◄──────────┼───┤   4x Mlx90393           │
        │   mux channel select  │  owns bus │   │   FreeRTOS task core 0  │
        │        ▲              │           │   │   Vec3Filter x4         │
        │  MuxDriver (gMux)     │           │   │   latest()/health()     │
        │   detect/scan/map     │           │   │   ring recorder (128f)  │
        │        ▲              │           │   │        │                │
        │  OledManager (gOled)  │───────────┘   │  CalibrationService     │
        │   U8G2 on CH5         │               │   baseline/collect/fit  │
        │                     │               │   state machine         │
        └─────────────────────┘               │        │                │
                                              │  MeasurementService     │
        ┌──────────────────────────┐          │   dipole model → mm     │
        │ networking layer         │          │   (NOT_CALIBRATED until │
        │  WifiManager (gWifi)     │          │    validated)           │
        │   AP/STA/AP_STA fsm      │          │                         │
        │  CaptivePortal (gPortal) │          │  StorageService (NVS)   │
        │   DNSServer :53          │          │   cfg/wifi/ap/calib     │
        │  ScratchsorWebServer     │          │   CRC + schema          │
        │   WebServer :80 REST+FS  │          └─────────────────────────┘
        │  WsServer :81            │          
        │   sensors/status/calib   │          ┌─────────────────────────┐
        └──────────────────────────┘          │ diagnostics             │
                                              │  health report, boot    │
                                              │  table, [I]/[W]/[E] log │
                                              └─────────────────────────┘
```

## 2. Threading model

Two FreeRTOS contexts touch hardware:

| Context | Core | Priority | Touches |
|---|---|---|---|
| Acquisition task (`SensorService::run`) | core `kAcquisitionCore` = **0** | `kAcquisitionPrio` = 2, stack 8 KiB | I2C bus, mux channels 0–3, MLX90393 registers |
| Arduino `loop()` (app task) | core **1** (default) | default | Wi-Fi/DNS/HTTP/WS pumps, OLED render, calib/measure update, frame fan-out |

Rules enforced by construction:

- **I2C mutex.** `I2CManager` owns a FreeRTOS mutex; every transaction must
  take `I2CBusGuard` (RAII) or `withChannel()`. Lock timeout
  `kI2cLockTimeoutMs` = 150 ms — a stuck holder degrades to a failed
  transaction, never a deadlock [verified — `i2c_manager.h`].
- **Mux channel discipline.** The guard *locks and selects the channel*;
  channel state is never assumed to persist between calls (`currentChannel_`
  is tracked, 0xFF = none). The acquisition task only ever selects CH0–3;
  OLED render selects CH5; mux detect/scan selects none.
- **Single writer to sensors.** Only the acquisition task calls
  `Mlx90393::read`. Consumers get data via `latest()` snapshots copied under
  a short critical section (`portMUX_TYPE` spinlock) — no torn frames, and a
  reading's own timestamp is what marks it fresh, not the copy time.
- **Config changes are deferred.** `requestConfig()` sets an atomic
  `cfgPending_` flag; the task applies new sensor/filter settings at the
  next frame boundary, never mid-transaction.
- **Nothing long runs in HTTP handlers.** WebServer handlers return
  immediately; calibration capture and Wi-Fi connect run as state machines
  pumped from `loop()` / the task.
- WS broadcast serializes once into a shared static buffer; sending happens
  on the app loop only. Slow clients are reaped by the library's ping/pong.

## 3. Data flow

```
 MLX90393 x4 (CH0..3)
      │  SM → wait conversionTimeMs → RM (raw 16-bit axes + status byte)
      ▼
 Mlx90393::read ──► SensorReading {raw µT, status, saturated, seq, ts}
      │            per sensor, per frame tick (kSampleRateHz, 10 Hz default)
      ▼
 Vec3Filter (per sensor): median? → moving-avg → low-pass →
                          outlier z-gate → baseline subtract
      │            filtered µT + quality 0..1
      ▼
 SensorService: assemble SensorFrame {seq, ts, validCount, readings[4]}
      │  publish under spinlock; update rate EMA; append to recorder if on
      ▼
 ┌────────────────────────────────────────────────────────────┐
 │ consumers (all read snapshots; none touches the bus)         │
 │  • loop(): frame → OLED readings screen + measurement screen │
 │  • loop(): frame → WsServer.broadcastSensors (~10 Hz cap)    │
 │  • MeasurementService.update(): |ΔB| → CalibrationService    │
 │    .predictMm() → DistanceEstimate {state, mm, conf}         │
 │  • CalibrationService.accumulate(): captures during baseline │
 │    /collect phases                                           │
 │  • REST /api/v1/sensors, /export.csv, /export.json           │
 └────────────────────────────────────────────────────────────┘
```

Units: microtesla (µT) everywhere; `m`/`rm` = vector magnitude. `st` is the
per-sensor status string (OK, NACK, TIMEOUT, SATURATED, COMM_ERROR,
NOT_FOUND …) so consumers can distinguish "zero field" from "dead sensor".

## 4. Boot sequence

`main.cpp` setup(), in order — each stage tolerates sibling failure:

1. Serial 115200 + banner.
2. `StorageService`: load config, STA creds, load-or-create AP password
   (10 chars, esp_random + efuse MAC, persisted).
3. LittleFS mount (failure → Web UI down, everything else runs).
4. `I2CManager::begin` (GPIO21/22 @ 100 kHz), `MuxDriver::detect`
   (scan 0x70..0x77) + full channel scan for the boot map.
5. `OledManager::begin` on CH5 (probe 0x3C then 0x3D; SSD1306 default,
   override via config). Failure → headless, one `[W]` line.
6. `SensorService::begin` — per channel: probe 0x0C..0x0F, RT reset,
   apply SensorConfig. Found count logged.
7. `buildHealthReport` + `printBootReport` — the table the serial
   diagnostics tool parses.
8. `WifiManager::begin` — SoftAP `SCRATCHSOR-XXXX` (XXXX = efuse MAC tail),
   ch1, ≤4 clients; STA reconnect starts if stored creds exist.
9. `CaptivePortal::begin` — DNS wildcard → AP IP.
10. `SensorService::start` — acquisition task launched **before** HTTP so
    early clients already see live frames.
11. `CalibrationService::begin` (loads persisted points+model),
    `MeasurementService` created.
12. `WsServer::begin` (:81), `ScratchsorWebServer::begin` (:80, LittleFS
    statics + `/api/v1/*`).
13. OLED pinned to `AP_SETUP` screen showing the AP password.

Then `loop()` pumps: wifi.update → portal.update → http.update → ws.update
→ measure.update → calib.update → oled.update → frame fan-out (OLED + WS).

## 5. Failure / degradation matrix

| Component absent/failed | Boot symptom | Runtime behavior |
|---|---|---|
| Mux not found (no ACK 0x70–0x77) | `[E] PCA9548A not found` | All downstream channels unreachable: 0 sensors, no OLED. Networking + HTTP + recorder still fully up — you can fix wiring from the dashboard's health page. **Single point of failure.** |
| One/few sensors missing | `Magnetometers online: n/4` | Partial frames: missing sensors report their status, `validCount` drops; measurement/calibration continue on the healthy subset (model uses combined norm — expect degraded accuracy). |
| All sensors missing | `online: 0/4` | Acquisition task idles; health still visible; rest of system normal. |
| OLED missing | `[W] No OLED on CH5` | Headless operation. Caveat: the AP password (= admin token) is only ever shown on the OLED setup screen — a headless build can't retrieve it over network or serial (by design). Provision once with an OLED attached, or recover via `pio run -t erase` + reflash (regenerates everything). |
| LittleFS mount fail | `[E] LittleFS mount failed` | REST API + WS still serve; only static Web UI 404s. |
| STA connect fail | `sta.error` in status | AP stays up (management interface, `apPersistent` default) — device never strands you. |
| Wi-Fi entirely down | — | Acquisition + OLED readings + calibration unaffected. |
| Single-sensor comm errors | `st` ≠ OK in frame | `Mlx90393::recover()` (RT reset + re-apply config) after repeated errors; error counters surface in `/api/v1/health`. |
| NVS corrupt/absent | silent | Defaults used; never bricks (storage_service contract). |

Note on the AP password without OLED: it is generated once and persisted,
but is **only** ever shown on the OLED setup screen — never printed to
serial and never returned by the API. Recovery for a headless build:
temporarily attach an OLED and reboot (the setup screen is pinned at
boot), or `pio run -t erase` + reflash to regenerate. (`factory_reset`
needs the token itself, so it can't bootstrap a lost password.)

## 6. Cross-cutting contracts

- **Field names**: `include/protocol.h` is authoritative; `docs/api.md` is
  the human copy. Frontend in `data/` must match.
- **Units**: µT for field, mm for distance, ms for time, Hz for rates.
- **Secrets**: STA password write-only over API, never logged, never
  returned. AP password = admin token; shown only on the OLED setup screen
  and required as `X-SCRATCHSOR-Token` on mutating calls.
- **Flash wear**: NVS writes only on explicit config/calibration saves —
  nothing writes flash on the acquisition hot path.
- **Testability**: `signal_processing.h`, `calibration_math.h`, `protocol.h`
  are Arduino-free and unit-tested under `pio test -e native`; hardware
  drivers stay behind `I2CManager` so the bus can be mocked.
