# Fingertip overlap model v2 — corrected labels

Historical version, superseded by [v3](../v3/README.md), which uses all of dataset 2. The launcher now defaults to v3. The v2 artifacts below are retained unchanged for comparison.

Active PC model, trained 2026-11-12. Target: middle-to-middle fingertip overlap, **0–8 cm**. ESP32 continues streaming raw sensor data; no firmware was changed.

## Label correction and preservation

The user confirmed that the original 0 cm file is 0 cm, and original 2, 3, 4, 5, 6, 7, 8 and 9 cm files represent **1, 2, 3, 4, 5, 6, 7 and 8 cm**. The trainer automatically reads `raw-training-data/1/label_corrections.json`, checks each recorded label, and applies the actual label before splitting, tuning and fitting. It does not apply a blanket subtraction to predictions. Original trial IDs retain their recorded labels for provenance; reference columns in this version's prediction export use corrected labels.

All nine original CSVs match their v1 SHA-256 fingerprints. No CSV, saved recording or database label was rewritten or deleted. V1 remains available as a historical model using the superseded labels. Future captures should use actual overlap labels in a new dataset folder, without this correction.

## Pipeline and live behavior

- Require four fresh healthy unsaturated sensors and the measured no-magnet presence gate. The existing v1 raw-axis reference is reused with explicit provenance and rebound to this model SHA-256; no new background was captured. Relabeling does not change its raw sensor baseline. Recapture if mounting or surroundings change.
- Apply a 200 ms median to each of the 12 raw XYZ axes, append four magnitudes, standardize with training statistics, then run a **16 → 32 tanh → 1** continuous regressor. No high-pass filtering is used. The network has 577 parameters, or 2,308 float32 bytes before scaling, support, calibration and code.
- Reject unfamiliar inputs and out-of-range regression outputs using the existing guards. These are heuristics, not confidence probabilities.
- Press **Measure · 1 second** when ready, then hold still. Only this explicit press starts a measurement. Collect fresh accepted predictions during the next one second and display their median; use the instantaneous regression output after the trained 200 ms input filter, so the 800 ms live smoothing history cannot bias a newly requested measurement. The result stays visible until another button-triggered measurement completes, including during movement, missing magnets, refresh and disconnection. Persistence is tied to the model SHA-256, so a v1 value is not reused after correcting labels.
- A measurement requires at least six distinct valid predictions spanning at least 600 ms within its one-second capture, with no prediction gap above 350 ms. Invalid/missing magnets, stale sensors, unfamiliar poses or insufficient samples cancel it and retain the previous result. A large prediction spread or drift adds a movement warning to the completed result. It does not silently restart or wait indefinitely for steadiness. The optional actual-overlap input only displays error and never enters the model or measurement calculation.
- The page obtains model name, range, metrics and missing half-centimetre positions from the active artifact. It clears cached results if the model changes.

## Training and reserved-trial evidence

Read 90 trials across nine CSVs; excluded unhealthy/incomplete frames as in v1. Compared 30 candidate/filter combinations with four-fold whole-trial cross-validation on 72 trials, fitting preprocessing inside each fold. V2 selected a 32-unit MLP with L2 coefficient 0.001 and a 200 ms input window. Reserved 18 trials (270 windows) outside tuning and fitting.

The reserved IDs are the same as v1's holdout, with corrected reference labels. These are reused single-session validation recordings, **not a fresh external validation session**. The new numbers should not be interpreted as an independent accuracy improvement over v1 with its incorrect target labels.

| Reserved-trial metric | Result |
| --- | ---: |
| Window mean absolute error | 0.366 cm |
| Trial median mean absolute error | 0.333 cm |
| Root mean square error | 0.533 cm |
| 95th percentile absolute error | 1.408 cm |
| Within ±0.25 cm | 53.3% |
| Within ±0.5 cm | 73.3% |
| Pure-Python inference on this PC | 0.420 ms |

| Actual overlap | Window mean absolute error |
| --- | ---: |
| 0 cm | 0.543 cm |
| 1 cm | 0.219 cm |
| 2 cm | 0.104 cm |
| 3 cm | 0.121 cm |
| 4 cm | 0.430 cm |
| 5 cm | 0.900 cm |
| 6 cm | 0.077 cm |
| 7 cm | 0.255 cm |
| 8 cm | 0.645 cm |

Window counts are not independent poses. One recording session cannot establish robustness to remounting, other testers or another day. Smoothing improves steadiness, not physical accuracy. Half-centimetre display steps do not establish half-centimetre accuracy. See `metrics.json`, `holdout_predictions.csv`, `runtime.json` and `verification.json`.

## Next data

All integer overlaps **0, 1, 2, 3, 4, 5, 6, 7 and 8 cm** are present. Collect **0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5 and 7.5 cm** next, with independent repositioning and randomized order. Also collect a separate untouched validation session across 0–8 cm. Keep magnet identity, orientation and mounting consistent. There is no measured 8.5 or 9 cm label after this correction; either needs new data before extending the range.

## Run and reproduce

Run **Start USB Dashboard.cmd** for the collector at http://127.0.0.1:8766/, then **Start PC Model.cmd** for the corrected model at http://127.0.0.1:8780/. The model reads the bridge event stream and does not open the COM port or the recordings database.

The active launcher and `tools/pc_model.py` default to `models/v2/model.json`. To reproduce training into a new folder:

```powershell
python tools/train_distance_model.py --data raw-training-data/1 --output models/v2-rerun --name "Fingertip overlap v2 rerun"
```

The manifest is discovered automatically; it may also be passed with `--labels`. The trainer refuses to overwrite a nonempty output folder. Reproduction trains the regression; reuse or capture a compatible background reference before live prediction. Keep all prior models. No v2 model is embedded on the ESP32; export and board evaluation remain later work. Original design research is in `docs/model-v1-research.md`, with its superseded-label note.
