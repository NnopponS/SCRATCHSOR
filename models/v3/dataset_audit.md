# Dataset 2 quality audit — 2026-11-12

All nine original CSVs remain byte-for-byte unchanged. They contain **97 trials, 55,040 sensor rows and 13,760 complete four-sensor frames**. Labels are 0–8 cm and match `ref_mm` and filenames; no dataset-1 label subtraction applies here.

## Communication and timing

Every exported row has `st=OK`, `valid=1`, `saturated=0`, finite magnetic values and consistent frame/time metadata. No duplicates, incomplete frames or inconsistent labels were found. Received trial rates are 46.98–47.98 Hz; the longest gap is 42 ms, below the live pipeline's 120 ms reset threshold. None of the trials contains the later 1 Hz communication-error pattern. Temperature NaN is expected because the current USB sensor payload omits temperature; it was not treated as a magnetic-data error.

Saved timestamps run from **16:42:18 to 17:00:53 Asia/Bangkok on 2026-11-12**. The OLED-refresh recovery config was written at 17:05:38. Exact fault onset was not logged, so timing alone cannot prove when it began; the normal per-frame data directly shows that these trials did not record that communication failure. Board uptime resets between the 5 cm and 4 cm recording blocks, indicating a board restart/power cycle within this collection session. That does not alone invalidate the data or prove the physical setup was unchanged.

## Separate numeric-range concern

There are **63 abrupt near-full-output-span jumps in 11 trials**, despite all rows' OK flags. For example, 2 cm trial 10 sensor 1 X changes from −4859.7 to +4908.0 µT in 21 ms. These are consistent with range wrapping or an encoding/range problem, not ordinary small sensor noise. This is an inference, not proof that every affected label is wrong or that every other trial is free of range ambiguity.

The check assumes the gain-7/resolution-0 scale: X/Y 0.150 µT/count and Z 0.242 µT/count, with signed 16-bit output when temperature compensation is off. All exported axis values match that quantization lattice within rounding tolerance. A jump over 85% of a full output span is flagged. The CSVs do not preserve a full register/settings dump, so the setting is inferred from the value lattice and current device configuration. [Melexis datasheet, tables 17 and 21](https://media.melexis.com/-/media/files/documents/datasheets/mlx90393-datasheet-melexis.pdf).

| Overlap | Trial numbers to review/repeat | Flagged jumps |
| --- | --- | ---: |
| 2 cm | 4, 10 | 13 |
| 3 cm | 7, 11, 12 | 14 |
| 4 cm | 1 | 4 |
| 6 cm | 4, 5, 9, 10 | 29 |
| 7 cm | 3 | 3 |

Two additional trials approach 95% of that assumed output boundary without a detected jump: 4 cm trial 7 and 6 cm trial 1. Inspect those too. Preserve all originals. Check the sensor range/encoding before recollecting the flagged cases, so the same pattern does not simply recur. Any range or fixture change should be recorded as a new dataset/session with an appropriate new no-magnet reference.

## Training decision

The later communication fault is not a reason to discard the entire folder. The dataset supports an **experimental** model, with the numeric-range concern retained explicitly. No trial was silently dropped, corrected or renamed. The selected magnitude-only model reduces sensitivity to sign changes, but it does not reconstruct missing physical field values or resolve every range ambiguity. The model needs fresh-session validation.

All 97 trials were used for the final v3 fit after evaluation. Before that refit, the method was fitted on 79 trials and evaluated on 18 reserved trials. One-second replay through the actual PC prediction/measurement code completed 34/36 attempts; **32/36 (88.9%)** were within ±0.5 cm, counting failures as misses. Ordinary disjoint-window aggregation scored 51/54 (94.4%); that higher number is not the end-to-end success rate. This exceeds the requested 80% development target but does not establish 80% accuracy on new testers, sessions or unmeasured half-centimetre points.

Prioritize fresh **8 cm** trials: only 3/6 disjoint reserved one-second estimates there were within ±0.5 cm. Also record half-centimetre positions 0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5 and 7.5 cm, and review/repeat the flagged trials above. Entire-dataset recollection is not required merely because of the later communication failure. Keep a fresh validation set outside training.

Detailed row/frame checks and original SHA-256 fingerprints: `dataset_audit.json`. Prediction results: `measurement_replay.json`, `holdout_predictions.csv` and `holdout_one_second_predictions.csv`.
