# SCRATCHSOR dashboard

The default phone product now has two pages: Measure and Records / Export.
Use the existing light palette, large readable held result and one clear
one-second Measure button. Patient name, gender and hand on top precede it;
live raw graphs and advanced configuration stay collapsed. Records group by
patient and offer CSV/ZIP export. See `docs/phone-product.md`. The desktop
collector described below remains available at `/legacy.html`.

The current dashboard collects raw magnetic sensor data for training a model
on the user's computer. It runs locally without external fonts or libraries.

Use a pale off-white ground, white panels, thin green-gray borders, small corner
radii, dark green emphasis and readable dark secondary text. Retain red and amber for
faults and warnings. Use system sans for navigation and labels; monospace and
tabular numerals for measurements. Keep keyboard focus visible.

The desktop overview centers on a live field-history chart, flanked by compact
connection, Wi-Fi and hardware panels. A sensor readings table
and device information follow. On narrow screens, put the chart first, stack
panels, wrap navigation and scroll the readings table within its own container.

Training data is the default page: measured distance, a fixed 3-second recorder,
raw CSV download, a realtime graph of Bx/By/Bz/|B| for the selected sensor and
a live readings table. Keep Overview, Live Sensors, Wi-Fi and Settings; exports are inside Training data.
Remove calibration, profile selection, classification and model-fitting controls.
All visible fields and charts use raw microtesla, without smoothing. Unavailable
or stale readings show a dash. Bx/By/Bz keep their true signed directions;
|B| is the non-negative vector magnitude. Selectors, plots and legends must
stay synchronized when navigating. The hardware target is 50 Hz with an
ordered 256-frame RAM recorder. In 50 Hz mode the OLED shows sensor availability
and updates only on status changes between trials, preserving capture timing.


The USB recording page uses durable batch sessions on the PC. A session
selector and ZIP export sit above a two-column workspace: manual distance
entry/recording on the left, the realtime raw graph on the right. Stack them
on phones. Below, show only manually created distance sections, with radio
selection, independently numbered trial chips and a CSV link for that distance.
Checkboxes include/exclude trials reversibly. Show starting, recording, saving,
saved and recoverable error states; lock distance changes during a capture/save.
The primary action records the next trial. Export all produces a ZIP with one
CSV per distance. Session data persists after refreshes/restarts. No automatic
range plan, fixed trial target, calibration or separate Export page is needed.

Clicking a trial chip loads that trial's saved raw graph with captured elapsed
timestamps on a 0–3 second axis, separate from live data. Keep Back to live
visible in recorded mode. Put trial rename/remove actions beside its graph;
keep inclusion checkboxes separate from the graph-selection buttons. Session
rename/remove belong in the toolbar and distance edit/remove in its section.
Require confirmation for deletion, preserve stable trial numbering, and update
reference labels consistently when a distance is corrected.
