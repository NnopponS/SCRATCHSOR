# Magnetic fingerprint calibration

This firmware predicts a reach category directly with on-device 3-neighbor KNN.
It does not estimate centimeters during normal measurement. One dataset belongs
to one person and mounting setup; moving sensors or changing magnets requires
new calibration. Mounting design can be chosen later, before collecting data.

## Connect and select a profile

Join the device Wi-Fi `SCRATCHSOR-XXXX`. Its Wi-Fi name is also its password
and browser admin token. Open `http://192.168.4.1`, or use the USB dashboard.
In Calibration select male/female, right/left hand on top, and a short identifier
for the person and mounting arrangement. Save before capturing.

OLED: profile and result plus S1–S4 status circles. Filled = online; hollow =
offline. SSD1306/SH1106 cannot show red/green; the browser indicators can.
OLED is CH0. Sensors 1–4 are CH2, CH3, CH4, CH5. All four are required.

## Baseline and physical trials

1. Remove magnets without changing sensor mounting. Capture baseline: 100
   complete fresh frames. Replacing it explicitly resets all four profiles.
2. Position the hands and measure fingertip overlap/gap with a ruler. Enter
   positive centimeters for overlap, negative for a gap, zero for touching.
   Accepted reference range: −50 through +50 cm.
3. Choose Training or Separate validation test. Reposition and independently
   measure before EVERY capture, then check the confirmation. Hold steady.
4. Each trial stores one 20-frame mean of all four sensors’ filtered,
   baseline-subtracted XYZ axes (12 values). Partial, stale or saturated frames
   do not count; interrupted windows restart. Capture times out after 30 seconds.
5. For each selected profile collect at least 8 training and 2 independent
   test trials in each of the three categories (30 trials minimum).
6. Validate this profile. Scaling uses training trials only. KNN uses standardized
   Euclidean distance and three votes; a three-way tie uses the nearest example.
   Held-out trials never train the classifier. Accuracy, per-category recall and
   confusion matrix are shown; low scores do not prevent activation.

| Profile | Below Standard | Standard | Above Standard |
|---|---:|---:|---:|
| Male, right on top | <4 | 4≤cm<6 | ≥6 |
| Female, right on top | <5 | 5≤cm<7 | ≥7 |
| Male, left on top | <0 | 0≤cm<3 | ≥3 |
| Female, left on top | <0 | 0≤cm<6 | ≥6 |

## Live use and managing data

Measurement shows only the selected profile and category. Its rolling fingerprint
uses the same 20-frame mean. After activation, healthy unfamiliar fingerprints
always receive a category; confidence is neighbor agreement, not a probability.
There is no unknown-category rejection. Missing, stale, saturated or invalid
sensors clear the result. A complete fresh window is needed to resume.

Each profile becomes ready independently. Switching to an untrained profile
shows Not calibrated; switching back restores readiness. Adding/deleting a trial
invalidates that profile’s validation. Validate it again. Setup, baseline,
sensor mapping or signal-processing changes invalidate every profile. Changing
setup/signal settings with saved data requires explicit reset authorization.

The shared limit is 128 trials across all profiles. A full dataset rejects new
captures without overwriting. Delete individual trials to make room. Download
calibration CSV before resetting. It includes signed references, labels, setup,
profile, trial ID/purpose and the exact averaged features used by the classifier.
Raw recording CSV/JSON remain separate and retain their existing sensor columns.

## Persistence and upgrades

Baseline, trials and model metadata persist with version and CRC checks in a
64 KB `reach_nvs` partition. Existing Wi-Fi/config NVS, app0 and LittleFS offsets
are unchanged. `partitions.csv` repurposes part of the previously unused second
OTA slot; this firmware has no OTA updater. Upload firmware (including its
partition table) and LittleFS with PlatformIO; do not erase all flash.
Old distance calibration blobs are retained in the original NVS but inactive.
Corrupt/incompatible fingerprint data never enables classification. NVS write
failures retain the previous dataset and report an error.

Reboot restores the saved baseline before acquisition and checks saved model
readiness by recomputing training/validation. Firmware/filesystem builds and host
fixtures do not prove physical accuracy. Validate actual independent poses and
mounting before relying on results.
