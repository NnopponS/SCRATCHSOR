# Dashboard over USB

## Receive realtime data only

The current first step is to receive live readings, with recording and other
features deferred. Connect the USB data cable, close other serial monitors,
and double-click **Start Live USB.cmd**. The page opens at
`http://127.0.0.1:8766/` and shows raw sensor axes, magnitude, status, device
acquisition rate and the rate received by the PC.

```powershell
python tools/usb_dashboard.py --live-only --port COM7 --http-port 8766
```

Replace COM7 with the current USB board port. The launcher can select it
automatically when exactly one USB serial device exists. Serial uses 921600
baud. Missing, saturated and stale readings display a dash rather than zero.
COM port numbers can change when the cable moves between USB sockets.

Live-only mode opens no session database and never records or changes device
settings. It only subscribes to incoming USB events. Existing files under
`recordings/` remain untouched. Keep the launcher running; Ctrl+C stops it.

The full dashboard workflow below remains available separately; it is not
part of the receive-only mode.

## Current recording source: PC USB receiver

New batch recordings collect the incoming raw USB sensor events directly on
this PC for 3 seconds. No ESP32 record command or RAM-buffer export is used.
The ESP32 continues acquisition/streaming; the Python client controls capture,
deduplicates sequence numbers, journals received frames and saves to SQLite.
Browser closure does not stop capture. USB sequence/timestamp gaps are retained;
received sample rate can be below the board acquisition rate. Temperature is
NaN when absent from the live protocol. Complete failed saves can be retried;
partial/stalled recordings remain under recordings/partial after clearing them.
Old saved trials and legacy interrupted board captures remain readable.

Connect the ESP32 using a USB **data** cable. Your PC can stay on its normal Wi-Fi, or have no network connection at all. Wi-Fi access to the ESP32 dashboard still works separately.

## First-time firmware update

The board needs the firmware from this version of the project. Do not erase flash: existing configuration and calibration are kept.

```powershell
pio run -e esp32dev
pio run -e esp32dev -t upload --upload-port COM7
```

Replace `COM7` with the board's port. This board may need manual download mode before upload: hold **BOOT**, tap **EN/RST**, then release **BOOT**. After uploading, tap **EN/RST** to run the application. Close the bridge and any serial monitor before flashing.

The USB dashboard loads its files from the PC's `data/` folder, so it does **not** require a filesystem update. To update the dashboard served over Wi-Fi too:

```powershell
pio run -e esp32dev -t buildfs
pio run -e esp32dev -t uploadfs --upload-port COM7
```

## Open the dashboard

On this PC a Python environment with pyserial and PlatformIO is available at `.pio/usb-env/Scripts/python.exe`. Double-click **Start USB Dashboard.cmd** in the project root. It opens `http://127.0.0.1:8765/` and selects the serial port if exactly one exists.

To choose a port explicitly:

```powershell
.\"Start USB Dashboard.cmd" --port COM7
```

On another PC, install Python 3.10+ and pyserial, then run:

```powershell
python -m pip install pyserial
python tools/usb_dashboard.py --port COM7
```

Keep the launcher running; **Ctrl+C** stops it. Only one program can use the serial port at a time. Close PlatformIO's serial monitor before starting the bridge. If multiple serial devices are connected, specify the correct port rather than guessing.

The connection indicator reads **USB · live** after compatible firmware responds. Calibration, settings, recording, downloads, and maintenance controls use the same handlers as Wi-Fi. Changes still require the usual admin token, currently the board's `SCRATCHSOR-XXXX` name. The PC bridge does not store that token.

## Collect a batch session over USB

1. Open **Training data**. Resume a saved session from the selector, or use
   **New session** to create a separate dataset. No baseline/profile/model is needed.
2. Measure a real distance and enter it in **cm**, then click **Add distance**.
   Only distances you add get sections; there is no fixed range or automatic
   stepping. Entering the same value again selects its existing section.
3. Select that distance, hold the magnet still, and press **Record Trial N ·
   3 seconds**. Recording ends on the ESP32, then the PC downloads and saves
   the complete trial. Wait until it says saved before taking the next trial.
4. Repeat as often as needed. Each distance has its own Trial 1, Trial 2, etc.
   Select another section when you physically move to that measured distance.
   The graph remains live during recording. At 50 Hz, expect about 150 complete
   frames / 600 sensor rows per trial, not a guaranteed sample count.
5. Uncheck any bad trial to exclude it from exports. The original data remains
   saved and can be included again; trial numbers are not changed or reused.
6. **Download CSV** on a section exports all its included trials. **Export all
   distances (ZIP)** makes one download containing a separate CSV for every
   distance with included trials, for example `distance_2.0cm.csv` and
   `distance_2.1cm.csv`. Empty sections are omitted.

Click a saved **Trial N** chip to view exactly that capture on the graph.
The saved graph uses actual elapsed timestamps on a 0–3 second axis and can
be viewed without a live USB connection. Sensor selection still works.
**Back to live** resumes the live graph; starting a new recording also returns
it to live mode. Invalid/saturated samples appear as gaps, with their raw
values and health flags retained in CSV.

**Rename session** changes its name. **Edit distance** corrects the ground-truth
reference for every trial in that section, including both distance_cm and
ref_mm in future exports; raw magnetic readings are preserved. An existing
section's distance cannot be overwritten/merged by renaming another section.
**Rename trial** adds an optional name while keeping its original trial number.
Trial names are included as trial_name in CSV; spreadsheet formula prefixes
are escaped in this free-text column.

**Remove** actions require confirmation. Removing a trial deletes its saved
raw data; removing a distance/session also deletes its trials. Previously
exported files do not change. Removed trial numbers and internal IDs are never
reused, and deleted retained board data is not automatically imported again.
Rename/removal operations are blocked while a capture/save is pending.

Trials are committed to `recordings/sessions.sqlite3` on the PC. Saved trials
survive browser refreshes, new recordings, board restarts and USB-server
restarts. Keep this directory to retain your sessions. Exported files are
copies; downloading does not clear the session. The PC server continues
saving a started capture even if the browser tab closes.

A failed download/save blocks the next recording. **Retry saving** only reads
back the retained trial; it never repeats the record command. If a server
restart interrupts a save, reopen its session and retry. **Recover available
board data** preserves any currently retained completed recording before
clearing the failed attempt. Data that was never saved to the PC can be lost
if the board is powered off/reset; wait for the saved status before unplugging.
The previous stopped single-trial buffer is also protected before a new batch
recording starts, avoiding silent replacement of an unsaved trial.

Each distance CSV has one row per sensor per frame, including unavailable
sensors. Its columns are:

`session_id,distance_cm,trial_no,trial_name,saved_at,seq,ts,id,raw_x,raw_y,raw_z,raw_m,st,ref_mm,elapsed_ms,trial_start_ms,valid,saturated,temp_c,quality`

`distance_cm` is the manual label, `trial_no` numbers captures at that distance,
and `saved_at` is the UTC time the PC saved the trial. Bx/By/Bz/|B| are
`raw_x/raw_y/raw_z/raw_m`, in µT. `ref_mm` is the reference in mm. Filtered
values are excluded. `valid`, `saturated`, `st`, `temp_c` and `quality` let you
inspect unusable samples. Exclude unhealthy/saturated rows from training;
zero placeholders from missing sensors are not measurements. Distance CSVs
can be read directly with `pd.read_csv(path)`.

Group sensor rows into frames using `session_id,distance_cm,trial_no,seq`.
Keep whole trials together in training/test splits; neighboring frames from
one stationary trial are not independent validation. The standalone legacy
`/api/v1/export.csv` still exports only the board's retained trial and has
leading `#` metadata lines; the batch buttons use the durable PC dataset.

The Training data graph shows raw Bx/By/Bz/|B| for the selected sensor, including
while recording. Signed axes can be negative because they encode direction;
|B| is sqrt(Bx² + By² + Bz²) and cannot be negative. Calibration, profile and
classification controls have been removed from the dashboard. A future model
will be trained on the PC and integrated separately when ready.

Repeat distances and reposition between trials to collect physically independent examples.

The recorder holds at most 256 frames in acquisition order. If it fills, it
stops early and keeps the first frames. A 3-second trial at 50 Hz fits with
headroom. The MLX90393 exposes measurement results rather than a sample FIFO;
this ordered recording buffer lives in ESP32 RAM.
Sensor/configuration and baseline changes are rejected during a recording.

The 50 Hz setup uses sensor OSR=0 and DIG_FILT=0, with USB serial at 921600
baud. Existing saved settings remain until changed. Higher oversampling/filter
settings increase conversion time and can prevent the target rate. Short
conversions trade noise for response speed; see the [Melexis datasheet, timing
and noise sections](https://www.melexis.com/-/media/files/documents/datasheets/mlx90393-datasheet-melexis.pdf).
At a configured 50 Hz, continuous OLED refresh is paused because its full-screen
transfer occupies the shared 100 kHz bus for about 100 ms. The OLED shows raw
sensor availability and refreshes when that availability changes, between
recordings. This replaces its stale startup/profile screen. The browser stays
live during recording.

## Connection and performance

- The bridge listens only on `127.0.0.1`; it rejects foreign browser origins and Host headers. Other computers cannot access it through your LAN.
- Unplugging the cable reports disconnection. The bridge tries to reopen the same port; it never automatically repeats failed commands. If a command loses its response, inspect the device state before trying again because it may already have executed.
- No serial port detected: check the USB data cable, Windows Device Manager, and the board's USB-to-serial driver. A power-only cable can light the board without exposing a serial port.
- Live USB readings are sent up to 50 times/second at 921600 baud. The Training data screen shows the measured acquisition rate. Frame timestamps in the CSV establish the actual rate; a configured target alone is not proof of timing.
- Wait for the automatic stop before exporting. Batch exports read saved PC data without touching the board. The legacy single-trial download retains board frames for retries; a new recording or restart clears only that RAM buffer. Exports share the serial link and can take several seconds. Live/dashboard servicing may pause during a download; sensor acquisition continues on its own task. Wait for the download to finish before issuing another command. A stalled/missing response fails explicitly rather than returning a partial successful download.
- USB request messages are limited to 4096 encoded bytes and each board response to 1 MiB. PC session CSV/ZIP exports are not subject to that USB response limit. These exceed the current configuration and 256-frame recorder's normal needs.
- Normal boot/diagnostic logs remain readable by `tools/serial_diagnostics.py` when the bridge is stopped. Protocol records use the `@USB ` prefix.

## Developer checks

```powershell
python -m unittest discover -s tools -p 'test_*.py'
pio test -e native
pio run -e esp32dev
```

The bridge tests use a controlled serial peer and a real local HTTP server. Firmware host checks need a C++ compiler (or `python -m pip install ziglang`) and the PlatformIO ArduinoJson dependency. They compile the actual recorder method and reset handler against hardware fixtures. These checks do not establish real-board timing, reconnect, or calibration accuracy; verify those on the connected hardware.
