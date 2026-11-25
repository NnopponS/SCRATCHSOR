# SCRATCHSOR setup guide

Firmware: PlatformIO + Arduino on `esp32dev`. Everything below matches
`platformio.ini` (env `esp32dev`, espressif32@6.7.0, LittleFS).

## 1. Prerequisites

| Need | Options | Notes |
|---|---|---|
| Editor/IDE | VS Code + PlatformIO extension **or** `pip install platformio` for CLI-only | either works; commands are identical |
| USB-UART driver | **CH9102X** driver (WCH) for this DevKit's USB-C bridge | Windows: install the WCH CH341/CH9102 package. macOS usually works out of the box; Linux: kernel `ch341` driver covers CH9102. Without it the port never appears. |
| Git | any | clone only; firmware is self-contained |
| Python | 3.8+ for `tools/serial_diagnostics.py` | needs `pyserial` (`pip install pyserial`); stdlib otherwise |

PlatformIO fetches the toolchain + libs (U8G2, ArduinoJson, WebSockets) on
first build — no manual installs.

## 2. Clone, build, flash

```powershell
git clone <repo> D:\SCRATCHSOR        # or your path
cd D:\SCRATCHSOR

pio run                                # 1. build firmware
pio run -e esp32dev -t upload          # 2. flash firmware (auto-detects port;
                                       #    add --upload-port COMx if needed)
pio run -e esp32dev -t buildfs         # 3. build LittleFS image (web UI files in data/)
pio run -e esp32dev -t uploadfs        # 4. flash filesystem image
pio device monitor -b 115200           # 5. serial console (Ctrl+] to exit)
```

Order matters: flash both firmware **and** the filesystem, or the dashboard
pages 404 (REST API still works — see failure matrix in `docs/architecture.md`).

Host-side unit tests (no hardware needed — pure C++ filter/calibration/
protocol modules):

```powershell
pio test -e native
```

## 3. First-boot walkthrough

1. **Power via USB.** Open `pio device monitor -b 115200` (or
   `tools/serial_diagnostics.py --port COMx`). Watch the boot report —
   expected lines in `docs/wiring.md` §10.
2. **Find the AP.** The device starts SoftAP `SCRATCHSOR-XXXX` (XXXX = last
   4 hex of the efuse MAC). The OLED shows the AP setup screen with the
   generated password.
3. **Connect** a phone or laptop to that SSID with the OLED password.
   The captive portal should open automatically; if not, browse to
   `http://192.168.4.1`. Details + OS quirks: `docs/wifi.md`.
4. **Verify live data.** Dashboard → sensor tiles should update ~10 Hz.
   `GET http://192.168.4.1/api/v1/health` should show mux + OLED true and
   per-sensor `st:"OK"`.
5. **Join your network (optional).** Dashboard → Wi-Fi Settings → scan →
   select SSID → password → connect. The AP remains up for management
   unless you disable persistence.
6. **Calibrate** when you're ready to trust distances — `docs/calibration.md`.

## 4. Serial diagnostics

`tools/serial_diagnostics.py` is a passive monitor + log parser (the
firmware has no serial CLI):

```powershell
python tools/serial_diagnostics.py --port COM5 --baud 115200 --log boot.log
python tools/serial_diagnostics.py --port COM5 --stats --json summary.json
```

It timestamps lines, colorizes `[I]/[W]/[E]` levels, extracts the boot
health report, and prints an error/warning/sensor-count summary on exit.

## 5. Pin-version policy

- `platformio.ini` pins `espressif32@6.7.0` and semver ranges on libs
  (`olikraus/U8G2@^2.35.19`, `bblanchon/ArduinoJson@^7.3.1`,
  `links2004/WebSockets@^2.6.1`). Treat bumping the platform or a major lib
  as a re-validation event: run `pio test -e native` plus the hardware test
  matrix in `docs/validation.md` before trusting field output.
- Firmware identity is `kFirmwareVer` in `include/config.h` (reported by
  `/api/v1/device`); bump it with any behavioral change so serial/WS logs
  are attributable.
- Don't commit `secrets.ini` (gitignored) or edit `platformio.ini` for
  personal port/board tweaks — use `pio run --upload-port` / a private
  `[env]` copy instead.

## 6. Common setup failures

| Symptom | Likely cause | Fix |
|---|---|---|
| No COM port appears | CH9102X driver missing | install WCH driver; try another cable (charge-only cables lack data) |
| `upload` fails / board not responding | port busy (monitor open) or wrong port | close monitors; `--upload-port COMx` |
| `uploadfs` fails | firmware env mismatch | run `buildfs` and `uploadfs` with `-e esp32dev` |
| Dashboard 404s but API works | filesystem never flashed | steps 3–4 above |
| Boot loop / brown-out | weak USB cable or port | shorter/better cable, powered hub |
