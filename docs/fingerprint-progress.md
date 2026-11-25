# Magnetic fingerprint implementation

Approved plan: conversation, 2026-11-09. Active source is the workspace root;
the nested Git checkout is not the implementation target.

- [x] Pure classifier, thresholds, window and native checks
- [x] Calibration, persistence, config migration and live measurement
- [x] API/USB/stream, browser dashboard and OLED
- [x] Documentation, complete verification and independent review

Ruling: SSD1306/SH1106 are monochrome. OLED uses small filled/hollow circles
for online/offline; browser uses green/red. Actual red/green requires a color display.
Ruling: validation scores are advisory, as explicitly chosen by the user.
Ruling: preserve legacy distance blobs without loading them into fingerprint mode.

Ruling: a fixed fingerprint blob needs room for atomic replacement. Added dedicated
64 KB reach_nvs in unused OTA-slot space, preserving all existing data offsets.
Review caught incompatible-delete reactivation and stale Overview categories;
both now have regression checks and fixes.

Final verification 2026-11-09:
- Firmware, LittleFS and board_bringup builds passed.
- Python unittest discovery: 38 passed; native Unity suites: 35 passed.
- Browser category/signed-input/stale-result/light-theme checks passed.
- Seven-view layout checks passed at desktop and 390px mobile.
- Mechanical design detector: no findings. SVG/PNG wiring labels inspected.
- Independent review confirmed all three reported defects were corrected.
- No serial ports detected: no upload, physical OLED check or accuracy claim.
