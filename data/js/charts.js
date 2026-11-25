'use strict';
/* ==========================================================================
   SCRATCHSOR charts — dependency-free Canvas line chart + ring buffer.
   Sized for a rolling ~30 s window at ~10 Hz (300 pts/series), drawn at a
   throttled ~15 fps via requestAnimationFrame.
   ========================================================================== */

class RingBuf {
  constructor(cap) { this.cap = cap; this.a = new Float32Array(cap); this.n = 0; this.h = 0; }
  push(v) { this.a[this.h] = v; this.h = (this.h + 1) % this.cap; if (this.n < this.cap) this.n++; }
  get length() { return this.n; }
  at(i) { return this.a[(this.h - this.n + i + this.cap) % this.cap]; }  // i=0 → oldest
  last() { return this.n ? this.at(this.n - 1) : NaN; }
}

class CanvasChart {
  constructor(canvas, opts = {}) {
    this.cv = canvas;
    this.cx = canvas.getContext('2d');
    this.series = (opts.series || []).map(s => ({
      name: s.name || '', color: s.color || '#8af', visible: true, buf: null }));
    this.capacity = opts.capacity || 300;
    this.windowSec = opts.windowSec || 30;
    this.times = opts.times || null;             // recorded capture elapsed milliseconds
    this.unit = opts.unit || '';
    this._min = 1000 / (opts.fps || 15);
    this._last = 0; this._pend = false; this._raf = 0;
    if (typeof ResizeObserver !== 'undefined') {
      this._ro = new ResizeObserver(() => this.draw());
      this._ro.observe(canvas);
    } else {
      this._onrs = () => this.draw();
      window.addEventListener('resize', this._onrs);
    }
    this.draw();
  }
  destroy() {
    if (this._ro) this._ro.disconnect();
    if (this._onrs) window.removeEventListener('resize', this._onrs);
    cancelAnimationFrame(this._raf);
  }
  setBuffers(bufs) {
    for (let i = 0; i < bufs.length && i < this.series.length; i++) this.series[i].buf = bufs[i];
    this.draw();
  }
  setVisible(i, v) { if (this.series[i]) { this.series[i].visible = !!v; this.draw(); } }
  requestDraw() {                                   // rAF-throttled, max ~fps
    if (this._pend) return;
    this._pend = true;
    this._raf = requestAnimationFrame(t => {
      this._pend = false;
      if (t - this._last < this._min) { this.requestDraw(); return; }
      this._last = t; this.draw();
    });
  }
  _box() {                                          // size canvas to CSS box × DPR
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const r = this.cv.getBoundingClientRect();
    const w = Math.max(60, r.width), h = Math.max(60, r.height || 220);
    const W = Math.round(w * dpr), H = Math.round(h * dpr);
    if (this.cv.width !== W || this.cv.height !== H) { this.cv.width = W; this.cv.height = H; }
    this.cx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { w, h };
  }
  _colors() {
    const g = n => getComputedStyle(this.cv).getPropertyValue(n).trim();
    return { grid: g('--chart-grid') || '#1d2a3c', txt: g('--chart-txt') || '#677992' };
  }
  draw() {
    if (!this.cv.isConnected) return;
    const { w, h } = this._box(), cx = this.cx, c = this._colors();
    cx.clearRect(0, 0, w, h);
    const L = 44, R = 10, T = 10, B = 22, pw = w - L - R, ph = h - T - B;
    const vis = this.series.filter(s => s.visible && s.buf && s.buf.length > (this.times ? 0 : 1));

    // autoscale Y over visible data, 12% padding
    let lo = Infinity, hi = -Infinity;
    vis.forEach(s => { const b = s.buf;
      for (let i = 0; i < b.length; i++) { const v = b.at(i);
        if (isFinite(v)) { if (v < lo) lo = v; if (v > hi) hi = v; } } });
    if (!isFinite(lo)) { lo = 0; hi = 1; }
    if (hi - lo < 1e-6) { hi += 0.5; lo -= 0.5; }
    const pad = (hi - lo) * 0.12; lo -= pad; hi += pad;
    const dec = Math.abs(hi - lo) < 12 ? 1 : 0;

    // horizontal grid + y labels
    cx.font = '11px ui-monospace, Consolas, monospace';
    cx.lineWidth = 1; cx.strokeStyle = c.grid; cx.fillStyle = c.txt;
    cx.textAlign = 'left'; cx.textBaseline = 'middle';
    cx.beginPath();
    for (let g = 0; g <= 4; g++) {
      const y = T + ph * g / 4;
      cx.moveTo(L + .5, y); cx.lineTo(w - R, y);
      cx.fillText((hi - (hi - lo) * g / 4).toFixed(dec), 5, y);
    }
    cx.stroke();

    // x time labels (oldest → now), right-aligned data
    cx.textAlign = 'center'; cx.textBaseline = 'top';
    for (let g = 0; g <= 3; g++) {
      const x = L + pw * g / 3;
      const s = this.times ? Math.round(this.windowSec*g/3) : -Math.round(this.windowSec * (1 - g / 3));
      cx.fillText(this.times ? s+'s' : s === 0 ? 'now' : s + 's', Math.min(Math.max(x, L + 12), w - R - 14), h - B + 7);
    }
    if (this.unit) { cx.textAlign = 'right'; cx.fillText(this.unit, w - R, T + 9); }

    if (!vis.length) {
      cx.textAlign = 'center'; cx.textBaseline = 'middle';
      cx.fillText(this.times ? 'no valid samples for this sensor' : 'waiting for data…', L + pw / 2, T + ph / 2);
      return;
    }

    // series lines + last-value dot (newest at right edge)
    const span = Math.max(2, this.capacity - 1);
    const xAt = (i,n) => this.times ? L+pw*this.times[i]/(this.windowSec*1000) : L+pw*(this.capacity-n+i)/span;
    vis.forEach(s => {
      const b = s.buf, n = b.length;
      cx.strokeStyle = s.color; cx.lineWidth = 1.5; cx.beginPath();
      let started = false;
      for (let i = 0; i < n; i++) {
        const v = b.at(i);
        if (!isFinite(v)) { started = false; continue; }
        const x = xAt(i,n);
        const y = T + ph * (1 - (v - lo) / (hi - lo));
        if (started) cx.lineTo(x, y); else cx.moveTo(x, y);
        started = true;
      }
      cx.stroke();
      const lv = b.last();
      if (isFinite(lv)) {
        const y = T + ph * (1 - (lv - lo) / (hi - lo));
        cx.fillStyle = s.color; cx.beginPath();
        cx.arc(this.times ? xAt(n-1,n) : w-R-2, Math.max(T + 3, Math.min(h - B - 3, y)), 2.6, 0, 6.3);
        cx.fill();
      }
    });
  }
}
