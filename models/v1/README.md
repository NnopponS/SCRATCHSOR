# Fingertip overlap model v1

Historical version, superseded by [v2](../v2/README.md). The user corrected the labels on 2026-11-12: recorded 0 stays 0, and recorded 2–9 cm means 1–8 cm. The weights and metrics below use the original incorrect labels and are retained for provenance. The launcher now uses v2; do not use v1 for corrected measurements.

Trained 2026-11-12. Target: middle-to-middle fingertip overlap in cm. Current scope: PC inference; ESP32 streams raw readings. No firmware model was installed.

## Selected pipeline

- Require four fresh, healthy, unsaturated sensor readings. Missing/stale data clears the live estimate; the last completed result is retained separately.
- Require a saved no-magnet reference. A separate background-change gate runs before regression; near-background readings return **NO MAGNET** with no numeric overlap. No magnet is not a 0 cm overlap example.
- Apply a rolling 200 ms median separately to the 12 raw XYZ axes; append the four vector magnitudes. No ambient zeroing or device filter changes.
- Smooth accepted predictions with a rolling 800 ms median. Automatically lock a robust result after approximately 0.5 seconds of steady readings, and collect the next result when the signal changes or becomes invalid, keeping the last completed result visible. There is no measurement button.
- Standardize with training-set means/scales, then run a 16 → 24 tanh → 1 continuous regressor. Update up to 10 times/second.
- Reject unfamiliar input patterns using distance to training-trial prototypes; reject predictions outside the trained range. This is a heuristic, not a calibrated confidence probability.
- Show the continuous estimate and a nearest-0.5 cm display. Rounding is not evidence of 0.5 cm accuracy.

## Evidence

Read 9 CSVs containing 51,036 sensor rows and 90 trials from one recording session. Excluded incomplete/unhealthy frames, including one saturated sensor row. Inputs were read-only and SHA-256 fingerprints are in metrics.json.

Compared 30 model/filter configurations: linear ridge, quadratic ridge, RBF regression and small neural networks, with 200/400 ms median windows. Four-fold cross-validation kept entire trials together and fitted scaling/model transforms within each fold. Retained 18 whole trials (two per distance) outside tuning; the shipped v1 uses the other 72 trials.

| Held-out metric | Result |
| --- | ---: |
| Window mean absolute error | 0.491 cm |
| Trial median mean absolute error | 0.461 cm |
| Root mean square error | 0.712 cm |
| 95th percentile absolute error | 1.673 cm |
| Within ±0.25 cm | 47.0% |
| Within ±0.5 cm | 67.8% |
| Pure-Python inference, this PC | 0.250 ms/prediction |

The 270 held-out windows come from 18 trials, not 270 independent poses. Repositioning independence is not established by the CSV metadata. All recordings come from one session, so these scores do not establish performance after remounting or on another day.

### Held-out error by measured overlap

| Reference | Mean absolute error | 95th percentile |
| --- | ---: | ---: |
| 0 cm | 1.392 cm | 2.234 cm |
| 2 cm | 0.367 cm | 0.727 cm |
| 3 cm | 0.134 cm | 0.245 cm |
| 4 cm | 0.173 cm | 0.339 cm |
| 5 cm | 0.273 cm | 0.348 cm |
| 6 cm | 0.676 cm | 1.069 cm |
| 7 cm | 0.102 cm | 0.173 cm |
| 8 cm | 0.235 cm | 0.367 cm |
| 9 cm | 1.067 cm | 1.339 cm |

### Interpolation stress check

For each interior label below, all its training-pool trials were withheld, the selected pipeline was retrained on the other labels, and that label was predicted. This tests missing-distance sensitivity, not actual half-centimetre accuracy.

| Entirely withheld overlap | Mean absolute error |
| --- | ---: |
| 2 cm | 0.882 cm |
| 3 cm | 2.355 cm |
| 4 cm | 1.159 cm |
| 5 cm | 0.769 cm |
| 6 cm | 1.110 cm |
| 7 cm | 0.921 cm |
| 8 cm | 2.183 cm |

## What to measure next

1. First fill the 0–2 cm gap: **0.5, 1.0 and 1.5 cm**. Then collect **2.5, 3.5, 4.5, 5.5, 6.5, 7.5 and 8.5 cm**. These positions are currently unmeasured.
2. Aim initially for 5–10 independently repositioned trials per new distance. Move away and return between trials; keep the measured overlap steady during each capture. Randomize distance order instead of collecting every distance in one ascending sequence.
3. Keep sensor mounting, magnet identity/polarity, magnet-to-finger attachment and orientation consistent with actual use. Record those setup details. Changing the setup can invalidate the model.
4. Collect a separate validation session at every 0.5 cm position, initially 3–5 trials each. Keep it outside training until evaluation. Add realistic hand/orientation variation within intended use, rather than reusing near-identical poses.
5. The trained range is 0–9 cm overlap. Negative gaps or overlap above 9 cm need additional labeled measurements before reliable use.

V1 has not demonstrated reliable half-centimetre accuracy. Larger filter windows did not improve validation enough to justify additional lag. New physical labels are more valuable than synthetic intermediate examples.

## Run on this PC

The USB collector runs at http://127.0.0.1:8766/ and the model page at http://127.0.0.1:8780/. Both are local. The model reads the collector event stream, so it does not compete for the COM port or use ESP32 recording commands.

1. Connect the board and run **Start USB Dashboard.cmd**. If several ports exist, pass `--port COM7` using the board’s current port.
2. Run **Start PC Model.cmd**, or open the model page if it is already running.
3. Hold known overlaps and compare with the live result. The optional actual-overlap input changes only the error display; it is never a model input. Use the collector to save new training trials and export them to a new raw-training-data folder.
4. Keep every version and input dataset. Retrain with `python tools/train_distance_model.py --data raw-training-data/N --output models/vN`; choose the model with `python tools/pc_model.py --model models/vN/model.json`. The trainer refuses to overwrite a nonempty output version.

## Later ESP32 path

The fixed-size C++ candidate export is `distance_model_v1.h`: approximately **6,536 bytes of float32 parameters**, including scaling, support prototypes and the current no-magnet reference; the neural network itself has 433 trainable parameters (1,732 float32 bytes). Filtering buffers, code and application memory are separate. PC timing is not ESP32 timing.

Before embedding the final model, reproduce the exact median/feature ordering, add runtime sensor/staleness guards, compare C and PC outputs, and measure board latency, memory and accuracy with the real mounting. No TFLite runtime is required for this small network.

See [primary-source model research](../../docs/model-v1-research.md), metrics.json, holdout_predictions.csv and verification.json for reproducible details.

## No-magnet correction — 2026-11-12

The original regressor accepted the empty-field pattern and reported approximately 3.35 cm. Its training data contained only labeled overlap examples, and distance to positive prototypes did not reject this case.

A separate five-second reference was captured while the user reported no magnets: 235 healthy frames, retained in `background-no-magnet-capture.json`. The baseline is the per-axis median. The presence score is the largest of the four sensors' vector changes from that baseline, in microtesla. The no-magnet threshold is twice the measured 99th-percentile background score, with a 20 µT floor; the activation threshold is 1.5 times that threshold for hysteresis. Regression inputs and trained weights are unchanged.

For this capture the thresholds are approximately 51.36 µT off and 77.03 µT on. The weakest recorded magnet window was approximately 296.24 µT. Replaying the background suppresses every warmed-up overlap output, while none of the 1,348 healthy recorded magnet windows falls below the activation threshold. This is a setup-specific check, not a general magnet-identity classifier or an independent accuracy study. See `no_magnet_verification.json`.

The PC page has **Capture no-magnet reference · 5 seconds**. Remove all magnets and keep sensors still before using it. References persist across restarts and are tied to the model file; recapture if the mounting or magnetic surroundings change. Failed/unstable captures retain the previous reference. These calibration files are separate from training CSVs and the recordings database.

The overlap error metrics above remain the original regression-only metrics. Background subtraction is used only for presence detection, not as an untrained transformation of network inputs. The C++ candidate now includes `magnetPresent()` and `magnetSignal()`; a later firmware caller must check presence before calling `predict()`. Recapturing the PC reference requires re-export before any later board deployment. No firmware was changed.

## Automatic steady hold

The PC now keeps the trained 200 ms input window and adds an 800 ms median over accepted overlap predictions. A separate automatic state machine waits for about 0.5 seconds of steady predictions, then holds their median on the main display. Small jitter does not move the held value. A sustained change of more than 0.25 cm in the smoothed preview (or more than 0.75 cm in the instantaneous estimate) releases it after 0.3 seconds and starts measuring the new pose. Missing magnets, invalid sensors, stale data and unfamiliar poses clear the candidate window. The last completed result remains visible until a new valid steady result replaces it.

The steady window requires limited raw/smoothed prediction spread and limited drift; changing signals keep the display in settling mode. With the initial sensor and smoothing warm-up, the steady-hold target is 0.5 seconds; initial sensor/filter settling can add some delay. The actual-overlap input is used only to display error, never to stabilize or correct predictions. There are no additional buttons or duration selectors.

Replaying the existing recordings reduced mean successive prediction changes from approximately 0.056 cm to 0.021 cm. This measures display steadiness, not an improvement in physical accuracy. Original validation errors above are unchanged. See `stability_verification.json`. All processing remains on the PC, and the later firmware integration must reproduce this postprocessing as well as the exported neural network.

### Retain the last result between measurements

The automatic hold target is now 0.5 seconds. A completed result remains on screen during movement, settling, missing magnets, disconnection and page refresh; the current live status remains visible separately. Only a newly completed steady measurement replaces the number. The last result is persisted in `last_result.json`, tied to the model SHA-256. Candidate windows still reset on invalid data, so the old value never trains or contributes to the next result. The page labels retained values as the last measured overlap and includes the capture time.
