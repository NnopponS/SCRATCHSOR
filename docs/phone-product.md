# Phone product firmware 0.3.0

This prototype runs a v5 range classifier and the existing v4 numeric overlap model on the ESP32. No PC or native application is needed after installation. It uses the same magnet/sensor fixture and orientation as dataset 2. Existing PC recordings and training CSVs are not migrated, deleted or modified.

## Install and open

Build with `python tools/export_product_model.py`, `python tools/export_range_model.py`, `pio run -e esp32dev`, then `pio run -e esp32dev -t buildfs`. The exporters read the existing model artifacts and write `include/product/model_v4.h` and `include/product/range_v5.h`. Training and deployment details are in [the v5 model card](../models/v5/README.md).

The ready-built bundle is `releases/phone-v0.3.0.zip`. Extract it and run `python flash.py COM7` using the actual port. DTR/RTS automatic reset and automatic restart were verified on this board on COM7; no button presses or enclosure access are normally needed. The included manifest records image addresses, sizes and SHA-256 hashes. Rebuild the bundle after source/build changes with `python tools/package_phone_product.py`. Prior bundles remain available. The former experimental extrapolation policy is superseded by the learned v5 range classifier; its old artifacts remain available.

Close anything using the board's serial port, then run `python tools/flash_board.py COM7` with the detected port. It uses `default_reset` before upload and `hard_reset` afterwards. This flashes firmware and LittleFS together and preserves both NVS partitions. Manual BOOT/EN is only a fallback if a particular USB connection cannot reset the board; the former blanket requirement to press buttons was incorrect. Never use an erase-all command. Previous application/filesystem images and a consistent PC database backup are in `.pio/product-backup-20261112-210358/`.

1. Power the board. Join the SSID and password shown on its OLED. Accept staying connected without internet.
2. Open **normal Safari or Chrome**, not a private tab or the temporary captive-portal window. Scan the URL QR after joining Wi-Fi, or enter `http://192.168.4.1`.
3. Tap **Connect** once. There is no in-app password form. The app obtains the device name from the existing same-origin identity endpoint and uses it as the existing local API token (this firmware already sets the token equal to the AP Wi-Fi name). The protected Wi-Fi network, server token validation and origin checks remain in place. Joining Wi-Fi in the phone's system settings still uses the displayed password when the phone has not saved it.
4. Enter a patient name or identifier, gender and which hand is **on top**. Missing fields are permitted; gender and hand are needed for a category. New patient creates a separate patient group, including when two people share a name.
5. Wait for four online sensors and a supported magnet position. Press **Measure Â· 1 second**, hold still, then wait for **Saved on this phone**. The result remains visible until a later measurement finishes or the patient context changes.
6. Under **Records / Export**, tap a record to inspect its raw graph, rename that trial or save its CSV. Each patient has a checkbox selecting **all active trials for that patient**, including trials hidden by a search or filter, plus Export / Rename / Delete buttons. The global Select all checkbox selects the visible filtered list. Counts show the total selected across filters. Clear empties the selection. Filters cover hand on top, result kind, and Today / Last 7 days. Export offers a results-only summary CSV, one raw CSV, or ZIP with patient folders and a summary. With no selection, the main Export button exports the visible list; patient Export always includes that patient's active trials. Trial names are included in metadata.
7. **Delete selected** or **Delete measurement** moves only the chosen phone records to **Recently deleted**, with confirmation and Undo. Restore them individually or in a batch from Recently deleted. Deletion is recoverable and does not alter raw samples, training CSVs, PC recordings or other phones. The IndexedDB name, schema version and record identifiers stay unchanged during this upgrade.

Version 0.2.2 fixes two record-control failures: saved-record edits no longer depend on stale measurement/calibration state from a disconnected ESP32, and patient action buttons re-enable after each metadata transaction. Only the same record still awaiting its save acknowledgment is protected, and renaming the patient of a pending capture waits for that save. Record edits otherwise work offline. Controls and status labels replace the former descriptive paragraphs. Connection/sensor status is confined to Measure; Records / Export focuses on patient records.

The original PC collector is at `http://127.0.0.1:8766/legacy.html`; the optional PC model remains at port 8780. The phone interface can also be opened through the USB bridge at `/phone/index.html` for development, but that stores records in the **PC browser**, not a phone.

## Live preview performance

Version 0.2.3 separates incoming data from drawing. Canvas paint is capped at 15 FPS, rapid packets are coalesced, the three-second buffer is bounded, and the canvas backing dimensions change only when its actual size changes. Device timestamps and sequence numbers prevent duplicate or delayed packets from being replayed as current data. Invalid sensor values produce gaps rather than false zero lines. Streaming stops in Records and when the page is hidden; foreground/Measure resumes with fresh data. Idle device polling uses 500 ms rather than 200 ms, retaining 200 ms progress polling while measuring or calibrating.

Wi-Fi sensor preview is limited to at most 20 messages/s independently of the USB stream. USB streaming and ESP32 acquisition remain up to 50 Hz, and every raw frame inside the one-second capture is still retained. The display limit does not downsample saved captures or change model inputs. These changes reduce client and network work; real-phone responsiveness still depends on the Wi-Fi connection and phone.

## Storage, timestamps and sharing

Each measurement is one atomic IndexedDB entry in `scratchsor-phone-records`, store `measurements`. It includes patient identity, phone ISO time, timezone/UTC offset, model/rule/device identifiers, result or Unsure, and all captured raw XYZ frames for all four sensors. Sensor status, validity and saturation are retained even for rejected captures. Approximately 50 frames (200 sensor rows in CSV) are expected per second. No raw smoothing is applied to exported samples.

Phone time is captured when the request is made; sample calendar times are estimates from that phone timestamp plus device-relative elapsed milliseconds. They are not clock-synchronized sample times. Device uptime is never substituted for calendar time.

Records are specific to the browser and origin. Use the same browser and address; another phone, private browsing, a different IP address, clearing site data or browser eviction will not retain the same list. Export regularly. Patient form and interrupted-request metadata use localStorage; the authentication token uses sessionStorage. No patient records are written to ESP32 flash. Only configuration, classification boundaries and no-magnet calibration persist there.

The ESP32 retains one capture in RAM until the phone confirms its IndexedDB transaction completed. New measurements are blocked until that save is acknowledged. Network retry uses the same measurement identifier. Keep the device powered during interrupted saves. If the board restarts before saving, unsaved RAM samples cannot be recovered; the app offers to clear the interrupted request without deleting any saved records.

Use **Save CSV/ZIP**, then share the downloaded file from Files/Downloads to LINE or another application. Direct browser file sharing is offered only when the browser supports it in a secure context; ordinary ESP32 HTTP generally does not. No internet is needed for measurement or download. Sharing through another app may require internet afterwards.

## Model, uncertainty and configuration

The firmware embeds the v5 three-class range classifier alongside all six unchanged v4 numeric networks. Dataset 3 supplies 20 below-zero and 20 above-eight trials; dataset 2 supplies 97 in-range trials. Labels in the tail CSVs are treated as qualitative regions, not exact negative/9 cm distances. Outputs are **<0 cm**, **0–8 cm numeric**, or **>8 cm**. Tail numeric values remain null; phone/OLED show Below or Higher and mark them as estimates. CSV `result_cm` stays blank for tails and `result_range` contains `<0` or `>8`. `range_confidence` records the classifier decision score.

Features use 200 ms median windows and the median of the one-second capture. This is not a high-pass filter. Background subtraction serves magnet-presence gating. Capture requires at least 35 raw frames, 90% healthy frames, six magnet feature windows spanning at least 600 ms, fresh data, and no queue overflow or acquisition gap above 120 ms. Range acceptance requires probability at least 0.80, margin at least 0.20 and class support; tails also require agreement from at least 80% of feature windows. No magnet remains distinct from zero and Below. All raw samples, including rejected measurements, are retained.

In-range results retain v4 numeric support, 0.75 cm head-disagreement and 0.8 cm motion limits. A confident in-range result within -0.25 to 8.25 cm is bounded to 0–8 cm to accommodate endpoint regression error. Tail classification happens first. Ambiguous, unfamiliar, moving or unhealthy readings show Unsure rather than an invented value. The previous 0.2.2 negative extrapolation heuristic is no longer used.

Nested whole-trial evaluation of range classification produced **262/274 correct attempts (95.6%)**, counting rejected attempts as failures: below zero 35/40, in range 188/194, above eight 39/40. These are development scores; session and region are confounded because all in-range data is from dataset 2 and all tails from dataset 3. They do not establish numeric accuracy, new-tester accuracy, or clinical validity. The v4 numeric networks were not retrained with qualitative labels. Full details, confusion counts and limitations are in [the v5 model card](../models/v5/README.md).

Exported float32 C++ classification and acceptance matched Python on all 4,094 real feature vectors, with maximum probability difference about 0.000000606. Phone export/render and record-control checks use isolated fixtures, never production records. Hardware checks after flashing are documented in `models/v5/verification.json`; real-phone measurements and fresh-session boundary validation remain necessary.

The Measure page's **Recalibrate** shortcut opens Setup. **Recalibrate no magnet Â· 5 seconds** can be repeated after success, failure or cancellation; it shows progress and a Cancel control. Remove all magnets first; stability checks cannot tell a stationary magnet from a true background. A successful capture atomically replaces the previous reference; failed or canceled captures keep it. It requires at least 200 healthy frames, at most 10% invalid frames, fresh readings and at least 4.5 seconds of coverage, alongside the existing noise/drift checks. Calibration is tied to the numeric v4 model version; upgrading only the compatible range classifier preserves that reference. The current model requires gain 7, resolution 0, OSR 0, digital filter 0, temperature on and 50 Hz; boot uses these settings and incompatible API changes are rejected. Optional router Wi-Fi is configurable; the device's own setup Wi-Fi remains available.

Reference category boundaries (cm) are editable in Setup. Existing records retain their original rules and rule version:

| Gender | Hand on top | Below standard | Standard | Above standard |
| --- | --- | --- | --- | --- |
| Male | Right | <4 | 4 to <6 | â‰¥6 |
| Male | Left | <0 | 0 to <3 | â‰¥3 |
| Female | Right | <5 | 5 to <7 | â‰¥7 |
| Female | Left | <0 | 0 to <6 | â‰¥6 |

These are the user's supplied reference categories, not a diagnosis. Qualitative tail estimates are distinct from validated numeric measurement in those ranges.

## OLED and protocol

Before a browser connects, OLED shows one static screen containing the URL QR, Wi-Fi name, password, explicit `http://` address and joined-phone status together. It does not alternate screens. The QR decodes to exactly `http://192.168.4.1`, using mixed segments to keep the lowercase URI and full quiet zone inside a 58-pixel square. Regenerate it with `python tools/export_app_qr.py` (development dependency: `qrcode`). Afterwards it shows a shortened patient name, gender and hand markers, four live sensor circles, large category and small centimetres. Empty details show a dash. An acknowledged capture shows Saved on phone. Full patient names remain in the phone record; very long OLED names are truncated. The web interface explicitly requests a light color scheme, including browser form controls.

`POST /api/v1/phone` accepts JSON `{action, owner, ...}` and the existing `X-SCRATCHSOR-Token` header. Actions are `connect`, `poll`, `patient`, `measure`, `raw`, `saved`, `background`, `cancel_background`, and `rules`. Repeated background requests during a running capture return its status without restarting it. Status includes calibration progress/outcome, separate live/held-result reasons, range model version, numeric model version, `rangeConfidence`, and `rangeResult` (`BELOW_ZERO`, `VALUE`, `ABOVE_EIGHT`, or `UNSURE`). Device metadata includes `overlapModel`; health includes `phoneMeasuring` and `phoneCapturePending`. Only one phone owns an active/pending capture; heartbeats expire after 15 seconds but an unsaved capture retains ownership. Raw frames download in eight-frame chunks. Socket live sensor data remains at port 81 `/ws`; USB development uses `/usb/events`. Older APIs and legacy assets remain available.
