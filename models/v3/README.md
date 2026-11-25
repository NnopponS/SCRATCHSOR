# Fingertip overlap model v3 — dataset 2

Historical version, superseded by [v4](../v4/README.md). V4 and v3 were compared on identical nested whole-trial splits; the active launcher now uses v4. The v3 weights, previous results and original data remain available.

Active PC candidate trained from **all 97 trials** in `raw-training-data/2` on 2026-11-12. Target: middle-to-middle fingertip overlap, **0–8 cm**. Those labels are already correct numerically; no offset was applied. All original CSVs, recordings and prior models are preserved. Firmware was not flashed during this model update.

## Result and scope

The requested target is interpreted as **80% of one-second measurement attempts within ±0.5 cm**. During development, the selected method reached **32/36 attempts (88.9%)**, including rejected attempts as misses, on 18 reserved trials before fitting all 97. There were 34 completed measurements; mean absolute error among completed results was **0.238 cm**, with 95th-percentile absolute error **0.775 cm**. These 36 attempts are clustered within 18 trials, not independent people or sessions.

The final full-data fit includes those reserved trials and therefore is not itself independently validated on them. The baseline audit had already inspected that fixed partition; call these development results, not a fresh external test. A new recording session kept outside training is required to establish future-use accuracy. All measured labels are integers; output at 0.5 cm steps does not establish accuracy at unmeasured half-centimetre positions.

## Pipeline and selection

Compare 90 model/feature combinations with four-fold whole-trial CV on the 79-trial development pool. Feature candidates included raw XYZ+magnitudes, four magnitudes, signed-log axes, unit directions plus magnitudes and experimental circular encoding. Models included ridge, quadratic ridge, bounded nearest neighbors and small continuous networks. Selection used development one-second aggregate success, with compact continuous regression preferred for a later embedded model.

The selected model uses **four features**: apply a 200 ms median per raw XYZ axis, calculate each sensor's vector magnitude, then `log1p(magnitude / 100 µT)`. Standardize using training means/scales and run a **4 → 24 tanh → 1** continuous network with L2 coefficient 0.0003. The neural network has **145 parameters (580 float32 bytes)**, before scaling, prototype guard, baseline, buffers and code. Measured pure-Python inference on this PC is about **0.091 ms**; it is not ESP32 timing.

Whole-trial CV on the development pool gave 97.0% of disjoint one-second estimates within ±0.5 cm. Reserved disjoint-window estimates gave 51/54 (94.4%), MAE 0.246 cm. Actual runtime replay was lower at 88.9% and is the main displayed metric. Its two rejected attempts count as misses. No metric was calculated on random frame splits. Raw 200 ms regression-window scores: 94.0% within ±0.5 cm and MAE 0.238 cm. These different denominators should not be interchanged.

## One-second button and guards

Press **Measure · 1 second** when ready. The trained 200 ms input median remains. The measurement takes the median of fresh instantaneous regression outputs during that second, bypassing old 800 ms preview smoothing history. The last completed result remains until another button-triggered measurement succeeds, including through movement, missing magnets, refresh and disconnection. The actual-overlap field is never a model input. Missing/stale sensors, absent magnets, unfamiliar poses and out-of-range readings cancel a candidate and retain the last result.

The existing measured raw-axis no-magnet reference is reused with explicit provenance and tied to the new model hash; no fresh no-magnet capture was invented. Every dataset-2 window is above its activation threshold (minimum about 603.6 µT versus 77.0 µT). Recapture if mounting or magnetic surroundings change. Guard/prototype rejection is a heuristic, not a confidence interval.

## Data-quality limits and next measurements

See [dataset audit](dataset_audit.md). All recorded sensor/validity flags passed, but 11 trials have suspected numeric-range wrapping. The magnitude representation reduces sensitivity to sign discontinuities; it does not certify those physical measurements or recover lost range information. No trial was removed from the requested full-data fit.

Collect more independently repositioned **8 cm** trials first: only half of its disjoint reserved one-second estimates were within ±0.5 cm. Review/repeat the 11 flagged trials, checking range/encoding before recollecting. Then add **0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5 and 7.5 cm**. Randomize the order and keep a fresh validation session untouched. Do not change sensor/magnet geometry or range settings without recording the change as a new session and rechecking the model.

## Run and reproduce

Run **Start USB Dashboard.cmd** for http://127.0.0.1:8766/, then **Start PC Model.cmd** for http://127.0.0.1:8780/. Both the launcher and server default to `models/v3/model.json`. ESP32 only streams; the PC runs the model. No recordings database is opened by the model service.

To reproduce the selected model into a new directory using the retained search and audit provenance:

```powershell
python tools/train_distance_v3.py --data raw-training-data/2 --output models/v3-rerun --benchmark-report models/v3/feature_benchmark.json --audit-report models/v3/dataset_audit.json
```

This reproduces regression/evaluation and the all-data fit. A compatible measured no-magnet reference is also needed before live use. The trainer refuses to overwrite existing models and checks dataset fingerprints. `evaluation_model.json` is the 79-trial regression used for reserved evaluation; load it with background loading disabled for offline analysis, then bind an appropriate reference if replaying.

V3 has a practical small embedded representation, but the old 16-feature C exporter intentionally rejects it to prevent an incorrect export. A dedicated four-log-magnitude export and board measurements are later work. No firmware model was installed.
