# SCRATCHSOR

Portable magnetic-sensing research device: one ESP32 DevKit (30-pin, USB-C), four CJMCU-90393 (MLX90393) magnetometers behind a PCA9548A/TCA9548A I2C multiplexer, and a 0.96" I2C OLED. Firmware `0.3.0-range-v5` runs overlap inference on the ESP32 and serves the phone interface, with one-tap Connect and a scheduled live graph. The new range classifier uses all dataset 3 tail trials plus dataset 2, producing qualitative `<0` / `>8` estimates alongside v4 numeric 0–8 cm results. Whole-trial range-classification development success is 95.6%, with fresh-session validation still needed. See the [v5 model card](models/v5/README.md). USB raw streaming, phone record editing/deletion, patient-wide selection and summary/raw/ZIP exports remain available.

**Phone product:** join the Wi-Fi shown on the OLED, open `http://192.168.4.1` in normal Safari or Chrome, then tap **Connect** once. The app does not ask for the Wi-Fi password again. The two pages are **Measure** and **Records / Export**. Enter patient details, press **Measure Â· 1 second**, and keep hands still. The result stays visible. Records and every captured raw sample save in the phone browser; export CSV or ZIP to Files and share to LINE. See the [phone guide](docs/phone-product.md) for installation, storage, OLED, configuration and limitations. Existing recordings, CSVs and model versions are preserved; the earlier PC collector remains at `/legacy.html`.

## PC overlap model v4 â€” dataset 2

The active continuous fingertip-overlap model uses all 97 trials in `raw-training-data/2`,
whose labels already cover 0â€“8 cm. V4 smooths real vector magnitudes before
combining magnitude and direction networks, then predicts from the one-second
capture's median features. Nested whole-trial replay on identical folds reached
184/194 attempts within Â±0.5 cm (94.8%), versus 177/194 (91.2%) for v3.
Completed-result MAE fell from 0.188 to 0.151 cm. Rejected attempts count as misses.
These are reused-session development scores before the final all-trial refit;
fresh-session and half-centimetre accuracy remain unverified.
All original CSVs, recordings and previous models are preserved.
Press **Measure Â· 1 second** when ready and hold still; the result stays visible
until you request and complete another measurement.
Run **Start USB Dashboard.cmd**, then **Start PC Model.cmd**. The collector runs
at `http://127.0.0.1:8766/`; predictions run at `http://127.0.0.1:8780/`.
The optional PC workflow runs filtering and inference on the PC. The phone firmware embeds v4 numeric inference alongside the new v5 range classifier. See the
[v4 model card](models/v4/README.md) for the paired comparison, data-quality warnings, missing distances,
collection instructions and retraining. Half-centimetre accuracy
has not yet been established. Raw data and prior model versions are retained.

## Receive live USB data first

Connect the board with a USB data cable and double-click **Start Live USB.cmd**.
Open `http://127.0.0.1:8766/` to watch raw Bx/By/Bz/|B| and sensor status.
The launcher selects the port when exactly one USB serial device is connected;
otherwise run `"Start Live USB.cmd" --port COM7` with the board's current port.

This receive-only mode sends stream-subscription messages at 921600 baud. It
does not open the recordings database, record trials, or change device settings.
Close other serial monitors first. Ctrl+C in the launcher stops the receiver.
The page distinguishes device acquisition rate from frames received on the PC,
and shows unavailable/stale readings as dashes. See [USB setup](docs/usb-dashboard.md).

## Build & flash

Requires VS Code + PlatformIO (or `pip install platformio`).

```powershell
pio run                                  # build
pio run -e esp32dev -t upload            # flash firmware
pio run -e esp32dev -t buildfs           # build LittleFS image (web UI)
pio run -e esp32dev -t uploadfs          # flash filesystem
pio device monitor -b 921600             # application serial console
pio test -e native                       # host-side unit tests
```

## First boot

For a one-click reflash of the existing build, close serial monitors and
double-click **Flash Board.bat**. USB reset and restart are automatic. It detects a
single serial port and writes firmware, partition table and dashboard together.
With multiple ports, run `"Flash Board.bat" COM3` using your board's port.
It preserves Wi-Fi/settings and fingerprint NVS. It uses the last built images;
after source changes run `pio run` and `pio run -t buildfs` first.
The app normally starts automatically after flashing; manual BOOT/EN is only a fallback for a USB connection that cannot reset the board.

1. Device starts SoftAP `SCRATCHSOR-XXXX` (XXXX from efuse MAC).
2. The AP password matches the Wi-Fi name (for this board: `SCRATCHSOR-9BD8`); the same string is the admin token. The OLED shows credentials, explicit HTTP URL and QR together on one static screen until a browser connects, then shows patient details, four sensor circles and the held result. Filled = online; hollow = offline.
3. Connect a phone/laptop â†’ captive portal opens, or browse to `http://192.168.4.1`.
4. Dashboard â†’ Wi-Fi Settings to join your network; AP stays up as management interface unless disabled.

## USB dashboard (no ESP32 Wi-Fi connection needed)

After updating the firmware, connect a USB data cable and run **Start USB Dashboard.cmd**. Open `http://127.0.0.1:8765/` for live readings, calibration, recording, and downloads. Your PC can stay on its normal Wi-Fi. See [USB setup and troubleshooting](docs/usb-dashboard.md).

For PC model training, open **Training data**, add each distance you manually
measure in cm, then record repeated 3-second trials. Each distance gets its
own Trial 1, Trial 2, etc. Trials save automatically to the PC and remain
available after refreshes and restarts. Each section downloads one CSV with
all its included trials; **Export all distances (ZIP)** downloads those CSVs
together. Uncheck bad trials without deleting their raw data.

Sessions live in `recordings/sessions.sqlite3`; keep this folder to retain
all saved trials. Click a trial to review its saved 0â€“3 second graph; use Back
to live to resume the live view. Sessions, distance labels and trial names can
be renamed; removal requires confirmation and never reuses trial numbers.
No firmware update is needed for batch sessions: they run
in the local USB dashboard. Target acquisition remains 50 Hz, with the
board's 256-frame recorder buffering each trial. The default page shows a
realtime raw Bx/By/Bz/|B| graph; profile/calibration/classification controls
are removed.

## Wiring

See `docs/wiring.md`. Primary bus: GPIO21=SDA, GPIO22=SCL @ 100 kHz â†’ mux (0x70). Mux CH2â€“5 = MLX90393 Ã—4, CH0 = OLED. CJMCU-90393 needs CS high for I2C (see wiring notes).

## Docs

For manual batch collection, persistence and per-distance exports, see [the USB workflow](docs/usb-dashboard.md). Calibration documents describe the earlier on-device model workflow.

`docs/` â€” architecture, wiring, setup, Wi-Fi, calibration method, validation protocol, troubleshooting, API.

## Honesty notes

- This build collects raw magnetic measurements and runs research overlap inference; qualitative tails are estimates rather than exact distances.
- Training labels are manually measured. Training and development evaluation happen on the PC; the final models run on the ESP32. Fresh-session accuracy remains unverified.
- Fingerprints use a dedicated 64 KB NVS partition; Wi-Fi settings and LittleFS offsets are preserved. Upload firmware and filesystem; never erase flash to upgrade.
- Hardware-dependent tests in `PROJECT_STATUS.md` are marked `NOT TESTED`/`BLOCKED` until run on real hardware.
