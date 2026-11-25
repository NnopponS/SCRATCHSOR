// Run in the dashboard browser context, e.g. DevTools evaluate_script sourcePath.
async () => {
  const assert = (condition, message) => { if (!condition) throw new Error(message); };
  assert(fmtAge(0) === 'Just now', 'Fresh readings need a readable age');
  assert(fmtAge(2000) === '2 s ago', 'Seconds need an age suffix');
  assert(fmtAge(120000) === '2 min ago', 'Minutes need an age suffix');
  const original = location.hash;
  const results = [];
  try {
    for (const [name, view] of Object.entries(Views)) {
      location.hash = '#/' + name;
      await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
      assert(document.querySelector('h1')?.textContent === view.title, name + ': missing page title');
      assert(document.querySelector('[aria-current="page"]')?.dataset.route === name,
        name + ': active navigation is not identified');
      assert(document.documentElement.scrollWidth <= innerWidth, name + ': horizontal overflow');
      if (name === 'overview') {
        assert(Views.overview.chart?.cv.isConnected, 'Overview chart must be mounted');
        if (S.sensors?.s) {
          const rows = document.querySelectorAll('#ov-readings tr');
          assert(rows.length === S.sensors.s.length, 'Readings must match the live sensors');
          S.sensors.s.forEach((sensor, i) => {
            if (sensor.st !== 'OK' && !sensor.sat)
              assert(rows[i].lastElementChild.textContent === '—', 'Unavailable sensors must not show a zero measurement');
          });
        }
      } else {
        assert(Views.overview.chart === null, 'Overview chart must be released on navigation');
      }
      results.push(name);
    }
    return { width: innerWidth, checked: results };
  } finally {
    location.hash = original;
  }
}
