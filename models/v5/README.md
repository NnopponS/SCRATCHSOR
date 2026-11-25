# V5 overlap range model

Firmware `0.3.0-range-v5` combines a new three-class range classifier with the unchanged v4 numeric regressor. Outputs are **<0 cm**, a numeric estimate in **0–8 cm**, or **>8 cm**. Tail results are qualitative estimates: their numeric `resultCm` is null. Unknown, changing, unhealthy or absent-magnet readings remain Unsure / No magnet. Neither an exact negative distance nor an exact distance above 8 cm was learned.

## Training data and labels

All 137 trials were used in the final fit: 97 in-range trials from `raw-training-data/2` and 40 new trials from `raw-training-data/3` (20 below zero, 20 above eight). These yielded 3,820 feature windows. Source CSVs, previous models, browser records and the recordings database were not edited. Input SHA-256 values are recorded in `model.json` and `metrics.json`.

The below-zero CSV contains placeholder numeric labels of 0 cm; these were interpreted as **BELOW_ZERO using its filename**, not numeric zero. The higher-eight CSV contains numeric labels of 9 cm; its filename defines **ABOVE_EIGHT**, not a verified exact 9 cm target. No artificial negative labels were invented.

## Development evaluation

Nested five-fold evaluation held out entire trials; three inner folds selected among four model candidates. Scaling, kernels, weights and support radii were fitted using training trials only. Each held-out trial provided two real one-second attempts. Rejections count as failures.

| True region | Below predicted | In range predicted | Above predicted | Rejected |
| --- | ---: | ---: | ---: | ---: |
| Below zero | 35 | 0 | 0 | 5 |
| 0–8 cm | 0 | 188 | 1 | 5 |
| Above eight | 0 | 0 | 39 | 1 |

Correct classification was **262/274 attempts (95.6%)**, including rejections in the denominator. Below-zero success was **35/40 (87.5%)** and above-eight success **39/40 (97.5%)**. Accepted-attempt accuracy was 262/263 (99.6%). These are **range-classification scores**, not numeric distance accuracy.

All in-range trials come from dataset 2, while both tail classes come from dataset 3. Session and region are therefore confounded: the model may recognize differences between recording sessions. Two attempts from one trial are also correlated. This is development evaluation, not independent accuracy on a new tester or session. Fresh recordings around 0 and 8 cm, both tails, and no magnet under the same setup are the next useful validation. Half-centimetre accuracy remains unverified.

## Embedded pipeline

The final model is a radial-basis-feature softmax classifier with 16 inputs, 137 trial prototypes and three outputs, selected as `rbf_local`. It adds about 10.8 KB of float constants plus prototype labels. It uses normalized XYZ direction and log field magnitudes from 200 ms median windows; no high-pass filter is used.

One-second capture features are combined by their median. Class probability must reach 0.80, the top-class margin 0.20, and the input must lie within the learned support radius for its predicted class. A tail result additionally requires at least 80% of captured windows to independently accept that same tail. Softmax confidence is a decision score, not a calibrated probability of correctness.

Raw health, freshness and no-magnet gates remain enabled. Numeric v4 prediction, support, head disagreement and motion guards run for the in-range class. Confident in-range predictions within -0.25 to 8.25 cm are bounded to 0–8 cm, matching the PC regressor's existing boundary allowance. Tail classification runs first and is never converted into an endpoint numeric value. Saved raw samples remain unfiltered.

The six v4 regression heads were not retrained on qualitative tail labels. Their original independent-development numeric evaluation is in [the v4 model card](../v4/README.md). `pipeline_sanity.json` replays the new classifier decisions with the already-fitted v4 numeric guards; its numeric error is training-data sanity only, not new held-out numeric accuracy.

Python and exported C++ classification/acceptance agreed on all 4,094 real window/capture feature vectors; maximum probability difference was approximately 0.000000606. See `native_verification.json`. The firmware reports range model `v5-2239e5f0b09f` and separately reports the retained numeric model version.

## Reproduce

```powershell
python tools/train_range_v5.py --output models/v5-retrain --work .pio/v5-retrain
python tools/export_range_model.py
python tools/verify_range_v5.py
pio run -e esp32dev
pio run -e esp32dev -t buildfs
python tools/package_phone_product.py
python tools/flash_board.py COM7
```

Training refuses to overwrite an existing model directory. The export command above exports the existing `models/v5/model.json`; use `--help` to choose another model. USB reset is automatic. Flashing preserves both NVS partitions; do not erase flash. Existing no-magnet calibration remains compatible because the numeric model and raw feature settings are unchanged.
