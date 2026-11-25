> Historical distance workflow. Use [fingerprint calibration](calibration.md)
> for current firmware; distance models are inactive.

# Configure a magnet-to-magnet gap experiment over Wi-Fi

Connect to `SCRATCHSOR-9BD8`, using `SCRATCHSOR-9BD8` as the password.
Open `http://192.168.4.1`. The same string is the dashboard admin token.

1. Open **Settings → ตั้งค่าระยะระหว่างแม่เหล็ก**. Select the sensor
   (current bench: Sensor 1 / CH0), surface-to-surface or center-to-center
   reference, and a short description identifying magnets, polarity and mounting.
2. Physically fix the sensor and first magnet. Constrain the second magnet
   to a repeatable one-dimensional path with unchanged orientation. Confirm
   the fixed-fixture checkbox only when the real setup matches it; save.
3. In **Calibration**, remove both magnets without moving the sensor/fixture;
   capture baseline. Starting a new baseline clears old points and model.
   For stepped calibration, set the signal filter's outlier sigma gate to 0
   before collecting; otherwise a large real field change can be held as an outlier.
4. Restore the first magnet to its fixed position. At each of at least ten
   distinct gaps, measure the actual magnet-to-magnet distance with a ruler,
   enter that reference in cm and capture samples. Do not enter board distance.
   The dashboard converts cm to mm for the API and stored calibration data.
5. Fit and validate. The model must pass at least two held-out points with
   RMSE ≤5 mm and maximum error ≤15 mm. Failed validation leaves the gap N/A.
6. **Measurement** displays the calibrated gap in cm, with mm shown underneath.
   Overview, recent estimates, calibration points and validation metrics use cm.
   The OLED retains its existing units. Both show the calibrated gap only within its reference
   range, for the same fixture and sensor. The field reading remains available
   separately in microtesla. No free-space two-magnet localization is implied.

Settings and calibration, including its baseline, persist across restart.
Changing fixture/reference/sensor or signal settings invalidates calibration;
when stored points exist, explicit permission to clear calibration is required
in Settings. Adding/removing required sensors cannot silently reuse a model.

API additions:

- `GET/POST /api/v1/config`: `gap` object with `enabled`, `fixedGeometry`,
  `sensorId` (0–3), `reference` (0=surfaces, 1=centers), `setup` (≤95 UTF-8 bytes).
- Config POST `resetCalibration:true` explicitly permits discarding an old
  experiment when these settings or signal settings change.
- `GET /api/v1/calibration`: persisted points, model, validation and phase.
- Existing baseline/collect/fit endpoints perform calibration. Collect requires
  a confirmed fixture, an established baseline and a measured reference 0–500 mm
  (strictly positive). A one-sensor fixture uses the selected sensor only.

The software workflow is implemented; physical gap accuracy remains unverified
until the user collects and validates measurements on the actual fixture.
