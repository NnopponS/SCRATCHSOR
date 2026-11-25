# Corrected overlap labels

The user corrected these labels on 2026-11-12. The original CSV filenames,
`distance_cm`, `ref_mm`, sensor values and trial IDs are preserved byte for byte.
The trainer automatically applies `label_corrections.json` when reading this folder.

| Original CSV label (cm) | Actual overlap (cm) |
| ---: | ---: |
| 0 | 0 |
| 2 | 1 |
| 3 | 2 |
| 4 | 3 |
| 5 | 4 |
| 6 | 5 |
| 7 | 6 |
| 8 | 7 |
| 9 | 8 |

This mapping applies only to these nine files. Record new trials with their actual
measured overlap in a new dataset folder; do not subtract 1 from new measurements.
The active model and updated accuracy report are in `models/v2`.
