# Fingertip overlap model v4 — existing-data improvement

PC model trained on **all 97 trials in dataset 2**, range **0–8 cm**, 2026-11-12. No recordings were deleted, overwritten, relabeled or synthesized. No firmware was flashed. The one-second Measure button and retained-result behavior remain.

## Direct comparison with v3

Both methods were retrained and evaluated on the **same outer trial splits**, using actual saved sensor frames through `LivePredictor` and `ManualMeasurement`. Each trial contributes two scheduled one-second attempts. All failures and rejections count as misses.

| Nested development replay | V3 | V4 |
| --- | ---: | ---: |
| Attempts within ±0.5 cm | 177/194 (91.2%) | 184/194 (94.8%) |
| Completed attempts | 186/194 | 191/194 |
| Mean absolute error, completed results | 0.188 cm | 0.151 cm |
| 95th-percentile absolute error, completed results | 0.493 cm | 0.405 cm |

The paired success gain is 3.6 percentage points; completed-result MAE is about 19.9% lower. Do not compare the v4 percentage with v3's earlier 32/36 score as though their denominators were the same. The paired comparison above supersedes that for judging this improvement.

A paired bootstrap resampling whole trials gives an approximate 95% interval of **-1.0 to 8.8 percentage points** for the success gain. This quantifies sampling uncertainty within the reused session, not new-session robustness. The point estimate improves, but the data remains limited.

## Evaluation design and limits

Five outer folds keep whole trials together and balance measured distances. Within each outer training pool, three inner trial folds select one of four fixed candidate pipelines. Scaling, neural parameters, input-support prototypes and guard thresholds are fitted only on the relevant training split. The outer trial never enters that split's fitting or selection. The v3 reference retains its original training pipeline and guards. V3's previously selected settings are fixed for this comparison.

After nested evaluation, three-fold grouped selection on all trials chose **direction_mix_50**, then the production model was refitted using all 97 trials (2704 rolling input windows). Outer-fold choices varied; nested scores evaluate the selection/training procedure rather than an independent test of that final all-data fit. The same dataset had already been inspected during earlier development. These are **reused-session development results**, not proof of 94.8% accuracy on a new tester, remounting or another day. No measured half-centimetre labels exist, so half-centimetre interpolation accuracy remains unverified.

## What changed

1. Compute each real sensor vector's magnitude **before** taking the 200 ms temporal median. This prevents positive/negative raw-axis excursions from being averaged into an artificial low field strength. The saved raw values remain unchanged; suspected range-wrap artifacts are not reconstructed into certified physical measurements.
2. Train with equal contribution per distance and trial, so the extra 3/4 cm trials do not dominate the objective. Use three deterministic neural initializations for each input representation.
3. The selected continuous ensemble combines three **4 → 24 tanh → 1** magnitude networks with three **16 → 32 tanh → 1** direction-and-magnitude networks, using equal total weight for the two representations. The direction inputs are per-frame unit-axis components followed by temporal medians; strength inputs are `log1p(median(magnitude) / 100 µT)`. Models share training-set standardization, and magnitude heads select the four strength coordinates. It has **2166 neural parameters (8,664 float32 bytes)** before scalers, support prototypes, baseline, buffers and code. It uses no integer-label lookup or rounding to produce its continuous prediction. The bounded-output candidate was evaluated but was not selected.
4. During the requested one second, aggregate the accepted feature vectors, then predict once from their per-feature median. Earlier models instead took the median of instantaneous predicted distances. The current actual-overlap field is still used only for displaying error.
5. Calibrate field-pattern support on training-window variation and other-trial prototypes, instead of relying solely on distances between trial medians. This reduced false rejections in the comparison. Missing/stale sensors, absent magnets and unsupported captures still interrupt measurement, keeping the previous result.

The UI still has **Measure · 1 second**. It never restarts measurement automatically or adds extra hold time. After a successful capture, the value remains until the next requested measurement completes. No-magnet detection uses the same measured raw-axis reference and hysteresis; no new calibration was invented. Replaying its 235 original background frames produced {'NO_MAGNET': 47}, with no live overlap output.

## Error by actual overlap

| Actual overlap | V3 within ±0.5 cm | V4 within ±0.5 cm | V4 completed-result MAE |
| --- | ---: | ---: | ---: |
| 0 cm | 17/20 | 19/20 | 0.194 cm |
| 1 cm | 18/20 | 19/20 | 0.144 cm |
| 2 cm | 20/20 | 20/20 | 0.104 cm |
| 3 cm | 23/26 | 25/26 | 0.136 cm |
| 4 cm | 28/28 | 28/28 | 0.111 cm |
| 5 cm | 20/20 | 20/20 | 0.085 cm |
| 6 cm | 20/20 | 18/20 | 0.175 cm |
| 7 cm | 20/20 | 20/20 | 0.124 cm |
| 8 cm | 11/20 | 15/20 | 0.321 cm |

Eight-centimetre cases and the flagged range-jump trials still deserve new independent recordings. The [original dataset audit](dataset_audit.md) is retained unchanged from v3 and describes those data limitations. It is not evidence that the range-wrap concern was fixed in the hardware. Raw files and their hashes are in `metrics.json`/`dataset_audit.json`.

## Runtime and reproduction

Measured pure-Python inference is about **0.176 ms/prediction** on this PC. NumPy and portable inference agreed across 2704 real input windows to maximum absolute difference **1.78e-15 cm**. These are PC figures; board memory/latency still need measuring before embedding.

Run **Start USB Dashboard.cmd**, then **Start PC Model.cmd**. Collector: http://127.0.0.1:8766/. Model: http://127.0.0.1:8780/. The launcher/server default to `models/v4/model.json`. The model service reads the existing USB stream and does not open the COM port or recordings database.

To reproduce into a new directory:

```powershell
python tools/train_distance_v4.py --data raw-training-data/2 --output models/v4-rerun --work .pio/v4-rerun
```

Existing output directories are protected. Original v3 and its previous metrics remain available. V4 requires its documented window preprocessing and feature aggregation to be reproduced for a later C/ESP32 export; the old 16-feature exporter deliberately rejects this model. No model was installed on the ESP32.
