'use strict';
/* ==========================================================================
   SCRATCHSOR web UI — vanilla ES2020, zero external resources.
   Wire contract: docs/api.md + include/protocol.h (v1).
     REST base : same-origin /api/v1/*
     WebSocket : ws://<host>:81/ws  (frames keyed on field "t")
     Auth      : mutating POSTs send header X-SCRATCHSOR-Token = AP password
   ========================================================================== */

/* ---------- tiny DOM / format helpers ---------- */
const $  = (s, r) => (r || document).querySelector(s);
const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
const setText = (id, v) => { const e = document.getElementById(id); if (e) e.textContent = v; return e; };
const setHtml = (id, v) => { const e = document.getElementById(id); if (e) e.innerHTML = v; return e; };
const esc = v => String(v == null ? '' : v)
  .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
  .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const fin = v => typeof v === 'number' && isFinite(v);
const num = (v, d = 2) => fin(v) ? v.toFixed(d) : '—';
const fmtUt  = v => num(v, 2);
const fmtConf = v => fin(v) ? Math.round(v * 100) + '%' : '—';
const fmtAge = ms => ms < 950 ? 'Just now' : (ms < 60000 ? Math.round(ms / 1000) + ' s ago' : Math.round(ms / 60000) + ' min ago');
const fmtKB = b => fin(b) ? Math.round(b / 1024) + ' KB' : '—';
const fmtClock = t => { const d = new Date(t);
  return [d.getHours(), d.getMinutes(), d.getSeconds()].map(x => String(x).padStart(2, '0')).join(':'); };
const fmtUptime = ms => {
  if (!fin(ms)) return '—';
  const s = Math.floor(ms / 1000), h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
  if (h) return h + ' h ' + m + ' m';
  if (m) return m + ' m ' + (s % 60) + ' s';
  return s + ' s';
};
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const rssiWord = r => r > -55 ? 'excellent' : r > -65 ? 'good' : r > -75 ? 'fair' : 'weak';

/* ---------- contract constants ---------- */
const API = '/api/v1';
const HIST_CAP = 1500;                       // 30 s @ 50 Hz per sensor per metric
const SERIES_COLORS = ['#176b48', '#1d60ac', '#7952a8', '#996119'];
const SENSOR_BAD = new Set(['NOT_FOUND', 'NACK', 'TIMEOUT', 'SATURATED', 'COMM_ERROR']);
const sensorCls = st => st === 'OK' ? 'ok' : (SENSOR_BAD.has(st) ? 'bad' : 'mute');
const WS_LABEL = {
  connecting: ['connecting…', 'mute'], live: ['live', 'ok'], stale: ['stale', 'warn'],
  offline: ['reconnecting…', 'warn'], polling: ['http polling', 'info'],
  usb: ['USB · live', 'ok'],
};

/* ---------- toasts ---------- */
function toast(msg, kind = 'info', ms = 4200) {
  const box = $('#toasts');
  if (!box) return;
  while (box.children.length >= 4) box.firstChild.remove();
  const t = document.createElement('div');
  t.className = 'toast toast-' + kind;
  t.setAttribute('role', 'status');
  t.innerHTML = '<span class="toast-msg"></span><button class="toast-x" aria-label="Dismiss">×</button>';
  $('.toast-msg', t).textContent = msg;
  let dead = false;
  const kill = () => { if (dead) return; dead = true; t.classList.add('out'); setTimeout(() => t.remove(), 180); };
  $('.toast-x', t).addEventListener('click', kill);
  box.appendChild(t);
  if (ms > 0) setTimeout(kill, ms);
}

/* ---------- modal (token prompt + confirms) ---------- */
function openModal(opts) {
  return new Promise(resolve => {
    const prev = document.activeElement;
    const wrap = document.createElement('div');
    wrap.className = 'modal-wrap';
    wrap.innerHTML = '<div class="modal" role="dialog" aria-modal="true"><h3></h3>' +
                     '<div class="modal-body"></div><div class="modal-actions"></div></div>';
    const m = wrap.firstElementChild;
    m.setAttribute('aria-label', opts.title || 'dialog');
    $('h3', m).textContent = opts.title || '';
    $('.modal-body', m).innerHTML = opts.bodyHtml || '';
    const acts = $('.modal-actions', m);
    const done = v => {
      document.removeEventListener('keydown', onKey, true);
      wrap.remove();
      resolve(v);
      if (prev && prev.focus) try { prev.focus(); } catch (e) {}
    };
    (opts.actions || []).forEach(a => {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'btn ' + (a.cls || 'btn-ghost');
      b.textContent = a.label;
      if (a.primary) b.dataset.primary = '1';
      b.addEventListener('click', () => {
        const v = a.get ? a.get(wrap) : a.value;
        if (v === undefined) return;              // action vetoed (e.g. empty input)
        done(v);
      });
      acts.appendChild(b);
    });
    function onKey(e) {
      if (e.key === 'Escape' && opts.escapable !== false) { e.stopPropagation(); done(null); }
      else if (e.key === 'Enter' && e.target && e.target.tagName === 'INPUT') {
        const p = acts.querySelector('[data-primary]');
        if (p) { e.preventDefault(); p.click(); }
      }
    }
    document.addEventListener('keydown', onKey, true);
    if (opts.escapable !== false)
      wrap.addEventListener('click', e => { if (e.target === wrap) done(null); });
    $('#modal-root').appendChild(wrap);
    const f = wrap.querySelector('input') || acts.querySelector('[data-primary]') || acts.querySelector('button');
    if (f) f.focus();
  });
}
const confirmDlg = (title, body, okLabel = 'Confirm', danger = true) =>
  openModal({
    title, bodyHtml: '<p>' + esc(body) + '</p>',
    actions: [{ label: 'Cancel', value: false },
              { label: okLabel, value: true, cls: danger ? 'btn-danger' : 'btn-primary', primary: true }],
  }).then(v => v === true);

/* ---------- admin token (AP password, session-scoped) ---------- */
const Token = {
  KEY: 'scratchsor.token', _p: null,
  get()   { try { return sessionStorage.getItem(this.KEY) || ''; } catch (e) { return ''; } },
  set(v)  { try { sessionStorage.setItem(this.KEY, v); } catch (e) {} },
  clear() { try { sessionStorage.removeItem(this.KEY); } catch (e) {} },
  ensure(note) {
    const cur = this.get();
    if (cur) return Promise.resolve(cur);
    if (this._p) return this._p;
    this._p = openModal({
      title: 'Admin token required',
      bodyHtml:
        (note ? '<p class="warn-text">' + esc(note) + '</p>' : '') +
        '<p>Enter the device <b>AP password</b> matching the device Wi-Fi name (SCRATCHSOR-XXXX). ' +
        'It authorizes changes for this browser session only.</p>' +
        '<label class="field"><span>AP password</span>' +
        '<input id="tok-input" type="password" autocomplete="off" autocapitalize="off" spellcheck="false"></label>',
      actions: [
        { label: 'Cancel', value: null },
        { label: 'Authorize', cls: 'btn-primary', primary: true, get: w => {
            const i = $('#tok-input', w), v = i.value.trim();
            if (!v) { i.classList.add('invalid'); i.focus(); return undefined; }
            return v;
          } },
      ],
    }).then(v => {
      this._p = null;
      if (!v) throw new ApiError('no_token', 'Token entry cancelled', 0);
      this.set(v);
      return v;
    });
    return this._p;
  },
};

/* ---------- REST helper ---------- */
class ApiError extends Error {
  constructor(code, detail, status) { super(detail || code); this.code = code; this.detail = detail || ''; this.status = status || 0; }
}
const errText = e => e instanceof ApiError ? (e.detail || e.code) : (e && e.message) || 'Unexpected error';

async function rawApi(path, method, body, tok) {
  const ctl = new AbortController();
  const to = setTimeout(() => ctl.abort(), 15000);
  try {
    const h = {};
    if (body !== undefined) h['Content-Type'] = 'application/json';
    if (tok) h['X-SCRATCHSOR-Token'] = tok;
    const res = await fetch(API + path, {
      method, headers: h,
      body: body !== undefined ? JSON.stringify(body) : undefined,
      signal: ctl.signal,
    });
    let data = null;
    const ct = res.headers.get('content-type') || '';
    if (ct.indexOf('json') >= 0) { try { data = await res.json(); } catch (e) {} }
    if (!res.ok)
      throw new ApiError((data && data.error) || ('http_' + res.status),
                         (data && data.detail) || res.statusText || 'Request failed', res.status);
    return data;
  } catch (e) {
    if (e instanceof ApiError) throw e;
    if (e && e.name === 'AbortError') throw new ApiError('timeout', 'Request timed out', 0);
    throw new ApiError('network', (e && e.message) || 'Network error', 0);
  } finally { clearTimeout(to); }
}

async function api(path, opts = {}) {
  const method = opts.method || 'GET';
  if (method === 'GET') return rawApi(path, 'GET');
  let tok = Token.get();
  if (!tok) tok = await Token.ensure();
  try {
    return await rawApi(path, method, opts.body, tok);
  } catch (e) {
    if (e.status === 401) {                            // stale token → re-prompt, retry once
      Token.clear();
      tok = await Token.ensure('Token rejected — use the device Wi-Fi name as the password/token.');
      return rawApi(path, method, opts.body, tok);
    }
    throw e;
  }
}

async function withBusy(btn, fn) {
  if (!btn || btn.disabled) return;
  btn.disabled = true; btn.classList.add('busy');
  try { await fn(); }
  catch (e) { if (!(e instanceof ApiError && e.code === 'no_token')) toast(errText(e), 'error'); }
  finally { btn.disabled = false; btn.classList.remove('busy'); }
}

/* ---------- pub/sub + shared state ---------- */
const bus = {
  m: {},
  on(e, f) { (this.m[e] || (this.m[e] = new Set())).add(f); return () => this.m[e].delete(f); },
  emit(e, d) { const s = this.m[e]; if (s) s.forEach(f => { try { f(d); } catch (err) { console.error('[bus]', err); } }); },
};

const S = {
  ws: 'connecting',                 // connecting | live | stale | offline | polling
  sensors: null, lastSensorsAt: 0,
  status: null, calib: null,
  record: { active: false, count: 0, max: 256 },
  device: null, health: null, config: null, wifi: null, measurements: null,
};

const hist = [];                    // per-sensor metric history, shared by all views
for (let i = 0; i < 4; i++) hist.push({ x: new RingBuf(HIST_CAP), y: new RingBuf(HIST_CAP), z: new RingBuf(HIST_CAP), m: new RingBuf(HIST_CAP) });

function setWs(st) {
  if (S.ws === st) return;
  S.ws = st;
  document.body.dataset.ws = st;
  const lm = WS_LABEL[st] || [st, 'mute'];
  const p = $('#conn-pill');
  if (p) { p.className = 'conn conn-' + lm[1]; setText('conn-text', lm[0]); }
  bus.emit('ws', st);
  poller.eval();
}

function onFrame(m) {
  if (!m || !m.t) return;
  switch (m.t) {
    case 'sensors':
      S.sensors = m; S.lastSensorsAt = Date.now();
      if (Array.isArray(m.s)) m.s.forEach(s => {
        const h = hist[s.id];
        if (h) for (const k of ['x','y','z','m']) h[k].push(s.st==='OK' && !s.sat && fin(s['r'+k]) ? s['r'+k] : NaN);
      });
      bus.emit('sensors', m);
      break;
    case 'status': S.status = m; bus.emit('status', m); break;
    case 'calib':  S.calib  = m; bus.emit('calib',  m); break;
    case 'record': S.record = { ...m, active: !!m.active, count: m.count | 0, max: m.max || 256 };
                   bus.emit('record', S.record); break;
    case 'error':  toast('Device error: ' + (m.detail || m.code || m.error || 'unknown'), 'error');
                   bus.emit('error', m); break;
    case 'hello':  if (m.name) setText('foot-device', m.name);
                   if (!S.device && m.name) S.device = { name: m.name, fw: m.fw, api: m.api };
                   break;
    /* pong: presence is enough */
  }
}

/* ---------- WebSocket client (reconnect + heartbeat + stale detect) ---------- */
class WsClient {
  constructor(url) { this.url = url; this.ws = null; this.n = 0; this.tm = null; this.dead = false; }
  start() {
    this.connect();
    this.hb = setInterval(() => this.send({ t: 'ping' }), 10000);
    this.st = setInterval(() => this.tick(), 1000);
  }
  connect() {
    if (this.dead) return;
    try { this.ws = new WebSocket(this.url); }
    catch (e) { this.later(); return; }
    this.ws.onopen = () => {
      this.n = 0;
      setWs('live');
      this.send({ t: 'subscribe', ch: ['sensors', 'status', 'calib', 'record'] });
    };
    this.ws.onmessage = ev => { let m; try { m = JSON.parse(ev.data); } catch (e) { return; } onFrame(m); };
    this.ws.onclose = () => { if (!this.dead) { setWs('offline'); this.later(); } };
    this.ws.onerror = () => { try { this.ws.close(); } catch (e) {} };
  }
  later() {
    if (this.dead) return;
    const d = Math.min(8000, 500 * Math.pow(2, this.n++)) * (0.75 + Math.random() * 0.5);
    clearTimeout(this.tm);
    this.tm = setTimeout(() => this.connect(), d);
  }
  send(o) { if (this.ws && this.ws.readyState === 1) { try { this.ws.send(JSON.stringify(o)); } catch (e) {} } }
  tick() {
    if (this.ws && this.ws.readyState === 1) {
      const stale = Date.now() - S.lastSensorsAt > 3000;
      if (stale && S.ws === 'live') setWs('stale');
      else if (!stale && S.ws === 'stale') setWs('live');
    }
    poller.eval();
  }
  stop() {
    this.dead = true;
    clearTimeout(this.tm); clearInterval(this.hb); clearInterval(this.st);
    if (this.ws) try { this.ws.close(); } catch (e) {}
  }
}

/* ---------- HTTP fallback: poll /sensors while WS is unreachable ---------- */
const poller = {
  on: false, t: null,
  eval() {
    const wsUp = (S.ws === 'live' || S.ws === 'usb' || S.ws === 'stale');
    if (!wsUp && !this.on) { this.on = true; this.loop(); }
    if (wsUp && this.on) { this.on = false; clearTimeout(this.t); }
  },
  async loop() {
    if (!this.on) return;
    let any = false;
    try {
      const d = await rawApi('/sensors', 'GET');
      if (this.on && d) { any = true; d.t = 'sensors'; setWs('polling'); onFrame(d); }
    } catch (e) { /* keep retrying while WS is down */ }
    try {
      // REST /sensors has no est — /measurements carries est + recorder state
      const m = await rawApi('/measurements', 'GET');
      if (this.on && m) {
        any = true;
        S.measurements = m;
        if (m.record) { S.record = m.record; bus.emit('record', S.record); }
        bus.emit('sensors', S.sensors);          // refresh est-dependent UI
      }
    } catch (e) {}
    if (this.on && !any && S.ws === 'polling') setWs('offline');   // HTTP dead too
    if (this.on) this.t = setTimeout(() => this.loop(), 2000);
  },
};

/* ---------- wifi shape normalizer (REST status vs WS status frame) ---------- */
function wifiInfo() {
  const r = S.wifi || {}, rs = r.sta || {}, ra = r.ap || {};
  const w = (S.status && S.status.wifi) || {};
  return {
    mode:      w.mode      !== undefined ? w.mode      : r.mode,
    staState:  w.sta       !== undefined ? w.sta       : rs.state,
    staSsid:   w.staSsid   !== undefined ? w.staSsid   : rs.ssid,
    staIp:     w.staIp     !== undefined ? w.staIp     : rs.ip,
    rssi:      w.rssi      !== undefined ? w.rssi      : rs.rssi,
    staError:  rs.error || '',
    apSsid:    w.apSsid    !== undefined ? w.apSsid    : ra.ssid,
    apIp:      w.apIp      !== undefined ? w.apIp      : ra.ip,
    apClients: w.apClients !== undefined ? w.apClients : ra.clients,
  };
}

/* ==========================================================================
   VIEWS — each: { title, html(), enter(), leave() }
   ========================================================================== */
const Views = {

/* ---------- 1. Overview ---------- */
overview: {
  title: 'Overview', _s: [], _t: 0, chart: null,
  html() { return `
  <section class="grid">
    <div class="card ov-connection"><h2>Connection</h2>
      <p><span class="pill pill-mute" id="ov-ws">—</span></p>
      <dl class="kv">
        <dt>Last update</dt><dd id="ov-age">—</dd>
        <dt>Sample rate</dt><dd id="ov-rate">—</dd>
        <dt>Frames</dt><dd id="ov-seq">—</dd></dl></div>
    <div class="card ov-wifi"><h2>Wi-Fi</h2>
      <dl class="kv">
        <dt>Mode</dt><dd id="ov-wmode">—</dd>
        <dt>Station</dt><dd id="ov-sta">—</dd>
        <dt>Station RSSI</dt><dd id="ov-rssi">—</dd>
        <dt>Access point</dt><dd id="ov-ap">—</dd>
        <dt>AP clients</dt><dd id="ov-clients">—</dd></dl></div>
    <div class="card ov-hardware"><h2>Hardware status</h2>
      <div class="pillrow" id="ov-sensors"><span class="pill pill-mute">S1</span><span class="pill pill-mute">S2</span><span class="pill pill-mute">S3</span><span class="pill pill-mute">S4</span></div>
      <dl class="kv">
        <dt>Mux</dt><dd id="ov-mux">—</dd>
        <dt>OLED</dt><dd id="ov-oled">—</dd>
        <dt>Controller</dt><dd id="ov-oledc">—</dd></dl></div>
    <div class="card overview-chart">
      <div class="chart-head"><h2>Magnetic field</h2><span class="hint">Raw magnitude · 30 s history</span></div>
      <div class="legend">${SERIES_COLORS.map((c, i) =>
        '<span class="series-label"><i style="background:' + c + '"></i>Sensor ' + (i + 1) + '</span>').join('')}</div>
      <canvas id="ov-chart" role="img" aria-label="Live magnetic field magnitude for connected sensors over the last 30 seconds"></canvas>
      <a class="chart-link" href="#/sensors">Explore axes and sensor history →</a>
    </div>
    <div class="card overview-readings"><div class="chart-head"><h2>Sensor readings</h2><span class="hint">Raw field · µT</span></div>
      <div class="table-scroll"><table class="tbl"><thead><tr><th scope="col">Sensor</th><th scope="col">Status</th><th scope="col">Bx</th><th scope="col">By</th><th scope="col">Bz</th><th scope="col">|B|</th></tr></thead>
      <tbody id="ov-readings"><tr><td colspan="6">Waiting for sensor readings…</td></tr></tbody></table></div>
    </div>
    <div class="card ov-device"><h2>Device</h2>
      <dl class="kv">
        <dt>Name</dt><dd id="ov-name">—</dd>
        <dt>Firmware</dt><dd id="ov-fw">—</dd>
        <dt>Uptime</dt><dd id="ov-up">—</dd>
        <dt>Free heap</dt><dd id="ov-heap">—</dd></dl></div>
  </section>`; },
  enter() {
    this.chart = new CanvasChart($('#ov-chart'), {
      series: SERIES_COLORS.map((color, i) => ({ name: 'S' + (i + 1), color })),
      capacity: HIST_CAP, windowSec: 30, unit: 'µT', fps: 15,
    });
    this.chart.setBuffers(hist.map(h => h.m));
    this._s = [bus.on('sensors', () => this.live()), bus.on('status', () => this.stat()),
               bus.on('ws', () => this.wsPill())];
    this._t = setInterval(() => this.live(), 1000);
    this.wsPill(); this.live(); this.stat();
    api('/device').then(d => { S.device = d;
      setText('ov-name', d.name || '—');
      setText('ov-fw', (d.fw || '—') + ' · api ' + (d.api || '?'));
      setText('foot-device', d.name || 'SCRATCHSOR');
    }).catch(() => {});
    api('/wifi/status').then(d => { S.wifi = d; this.stat(); }).catch(() => {});
    api('/health').then(d => { S.health = d; this.stat(); }).catch(() => {});
    api('/measurements').then(d => { S.measurements = d; this.stat(); }).catch(() => {});
  },
  leave() {
    this._s.forEach(u => u()); clearInterval(this._t);
    if (this.chart) this.chart.destroy();
    this.chart = null;
  },
  wsPill() { const p = $('#ov-ws'); if (!p) return;
    const lm = WS_LABEL[S.ws] || [S.ws, 'mute'];
    p.textContent = lm[0]; p.className = 'pill pill-' + lm[1]; },
  age() { setText('ov-age', S.lastSensorsAt ? fmtAge(Date.now() - S.lastSensorsAt) : 'Waiting for readings'); },
  live() {
    const m = S.sensors;
    const stale=!!S.lastSensorsAt && Date.now()-S.lastSensorsAt>3000;
    if (m) {
      setText('ov-rate', num(m.rate, 1) + ' Hz');
      setText('ov-seq', m.seq != null ? '#' + m.seq : '—');
      if (Array.isArray(m.s)) {
        setHtml('ov-sensors', m.s.map(s =>
          '<span class="pill pill-' + sensorCls(stale?'TIMEOUT':s.st) + '" title="' + esc(s.st || 'UNKNOWN') +
          (s.sat ? ' · saturated' : '') + '">S' + (s.id + 1) + '</span>').join(''));
        setHtml('ov-readings', m.s.map(s => {
          const available = !stale && (s.st === 'OK' || s.sat);
          if (this.chart && this.chart.series[s.id]) this.chart.series[s.id].visible = available;
          return '<tr><th scope="row">Sensor ' + (s.id + 1) + '</th><td><span class="pill pill-sm pill-' +
            sensorCls(stale?'TIMEOUT':s.st) + '">' + esc(s.st === 'NOT_FOUND' ? 'Not detected' : s.st || 'Unknown') +
            '</span></td>' + ['x', 'y', 'z', 'm'].map(key => '<td>' + (available ? fmtUt(s['r'+key]) : '—') + '</td>').join('') + '</tr>';
        }).join(''));
        if (this.chart && !document.hidden) this.chart.requestDraw();
      }
      this.age();
    }
  },
  stat() {
    const w = wifiInfo();
    setText('ov-wmode', w.mode || '—');
    setText('ov-sta', (w.staState || '—') + (w.staSsid ? ' · ' + w.staSsid : ''));
    setText('ov-rssi', fin(w.rssi) && w.rssi !== 0 ? w.rssi + ' dBm' : '—');
    setText('ov-ap', w.apSsid || '—');
    setText('ov-clients', w.apClients != null ? w.apClients : '—');
    const st = S.status, h = S.health;
    if (st) { setText('ov-up', fmtUptime(st.up)); setText('ov-heap', fmtKB(st.heap)); }
    else if (S.device) { setText('ov-up', fmtUptime(S.device.uptimeMs)); setText('ov-heap', fmtKB(S.device.heap)); }
    const mux  = h ? h.mux : (st && st.health ? st.health.mux : undefined);
    const oled = h ? h.oled : (st && st.health ? st.health.oled : undefined);
    if (mux  !== undefined) setText('ov-mux',  mux  ? 'OK' : 'not found');
    if (oled !== undefined) setText('ov-oled', oled ? 'OK' : 'not found');
    if (h) setText('ov-oledc', h.controller || '—');
    // sensor pills via health until live frames arrive
    if (!S.sensors) {
      let arr = null;
      if (st && st.health && st.health.sensors) arr = st.health.sensors.map(s => ({ st: s }));
      else if (h && h.sensors) arr = h.sensors;                       // [{id,st,ok,err}]
      if (arr) setHtml('ov-sensors', arr.map((s, i) =>
        '<span class="pill pill-' + sensorCls(s.st) + '" title="' + esc(s.st) +
        (s.ok != null ? ' · ok ' + s.ok + ' / err ' + (s.err || 0) : '') + '">S' + (i + 1) + '</span>').join(''));
    }
  },
},

/* ---------- 2. Live Sensors ---------- */
sensors: {
  title: 'Live Sensors', _s: [],
  chart: null, metric: 'm', only: -1, vis: [true, true, true, true], _vis: null,
  html() { return `
  <section class="grid grid-sen">${[0, 1, 2, 3].map(i => `
    <div class="card sen-card s${i + 1}">
      <div class="sen-head"><h2>S${i + 1}</h2><span class="pill pill-mute" id="sen-st-${i}">—</span></div>
      <div class="sen-mag"><span class="live-val" id="sen-m-${i}">—</span><span class="unit">µT |B|</span></div>
      <dl class="kv kv3">
        <dt>Bx</dt><dd id="sen-x-${i}">—</dd>
        <dt>By</dt><dd id="sen-y-${i}">—</dd>
        <dt>Bz</dt><dd id="sen-z-${i}">—</dd></dl>
      <div class="qbar" title="signal quality"><div class="qbar-fill" id="sen-q-${i}"></div></div>
    </div>`).join('')}
  </section>
  <section class="card chart-card">
    <div class="chart-head">
      <h2>Field history <span class="hint">30 s window</span></h2>
      <div class="chart-ctl">
        <label class="field fi"><span>Metric</span>
          <select id="ch-metric"><option value="m">|B|</option><option value="x">Bx</option><option value="y">By</option><option value="z">Bz</option></select></label>
        <label class="field fi"><span>Sensor</span>
          <select id="ch-only"><option value="-1">All</option><option value="0">S1</option><option value="1">S2</option><option value="2">S3</option><option value="3">S4</option></select></label>
      </div>
    </div>
    <div class="legend" id="ch-legend">${[0, 1, 2, 3].map(i =>
      '<button class="legend-btn" data-i="' + i + '" aria-pressed="true">' +
      '<span class="sw s' + (i + 1) + '"></span><span>S' + (i + 1) + '</span>' +
      '<span class="lg-v" id="lg-v-' + i + '"></span></button>').join('')}
    </div>
    <canvas id="sen-chart" role="img" aria-label="Rolling 30 second chart of sensor field values"></canvas>
  </section>`; },
  enter() {
    this.chart = new CanvasChart($('#sen-chart'), {
      series: SERIES_COLORS.map((c, i) => ({ name: 'S' + (i + 1), color: c })),
      capacity: HIST_CAP, windowSec: 30, unit: 'µT', fps: 15,
    });
    $('#ch-metric').value = this.metric;
    $('#ch-only').value = String(this.only);
    this.reload();
    $('#ch-metric').addEventListener('change', e => { this.metric = e.target.value; this.reload(); });
    $('#ch-only').addEventListener('change', e => { this.only = +e.target.value; this.applyVis(); });
    $$('#ch-legend .legend-btn').forEach(b => b.addEventListener('click', () => {
      const i = +b.dataset.i; this.vis[i] = !this.vis[i]; this.applyVis();
    }));
    this._s = [bus.on('sensors', m => this.upd(m))];
    this._vis = () => { if (!document.hidden && this.chart) this.chart.requestDraw(); };
    document.addEventListener('visibilitychange', this._vis);
    this.upd(S.sensors);
  },
  leave() {
    this._s.forEach(u => u());
    document.removeEventListener('visibilitychange', this._vis);
    if (this.chart) this.chart.destroy();
    this.chart = null;
  },
  reload() { if (!this.chart) return; this.chart.setBuffers(hist.map(h => h[this.metric])); this.applyVis(); },
  applyVis() {
    if (!this.chart) return;
    const only = this.only;
    this.vis.forEach((v, i) => this.chart.setVisible(i, only < 0 ? v : i === only));
    $$('#ch-legend .legend-btn').forEach(b => {
      const i = +b.dataset.i, on = only < 0 ? this.vis[i] : i === only;
      b.setAttribute('aria-pressed', on ? 'true' : 'false');
      b.classList.toggle('off', !on);
      b.disabled = only >= 0;
    });
    if (this.chart) this.chart.draw();
  },
  upd(m) {
    if (!m || !m.s) return;
    m.s.forEach(s => {
      const i = s.id; if (i < 0 || i > 3) return;
      setText('sen-x-' + i, fmtUt(s.rx)); setText('sen-y-' + i, fmtUt(s.ry));
      setText('sen-z-' + i, fmtUt(s.rz)); setText('sen-m-' + i, fmtUt(s.rm));
      const p = $('#sen-st-' + i);
      if (p) { p.textContent = s.sat ? 'SAT' : (s.st || '—'); p.className = 'pill pill-' + (s.sat ? 'warn' : sensorCls(s.st)); }
      const q = $('#sen-q-' + i);
      if (q) { q.style.width = clamp((fin(s.q) ? s.q : 0) * 100, 0, 100) + '%'; q.classList.toggle('low', !(s.q >= 0.4)); }
      setText('lg-v-' + i, fmtUt(s['r'+this.metric]));
    });
    if (this.chart && !document.hidden) this.chart.requestDraw();   // paused while tab hidden
  },
},

/* ---------- Training sessions ---------- */
measure: {
  title: 'Training data', _s: [], chart: null, sensor: 0, sessionId: null, distanceId: null, data: null,
  html(){return `
    <div class="session-bar">
      <label class="field fi"><span>Session saved on this PC</span><select id="me-session" aria-label="Recording session"><option>Loading sessions…</option></select></label>
      <button class="btn btn-ghost" id="me-new">New session</button>
      <button class="btn btn-ghost" id="me-session-rename">Rename session</button>
      <button class="btn btn-ghost" id="me-session-remove">Remove session</button>
      <a class="btn" id="me-export" aria-disabled="true" download>Export all distances (ZIP)</a>
    </div>
    <p class="dim" id="me-summary" role="status">Loading your saved trials…</p>
    <div class="batch-workspace">
      <section class="card batch-recorder">
        <h2>Add a measured distance</h2>
        <form id="me-distance-form" class="batch-distance-form">
          <label class="field"><span>Distance (cm)</span><input id="me-ref" type="number" min="-50" max="50" step="0.0001" required placeholder="e.g. 2.0" aria-describedby="me-ref-help"></label>
          <button class="btn" id="me-add" type="submit">Add distance</button>
        </form>
        <p class="dim" id="me-ref-help">Add only distances you actually measure. Select an existing section to add more trials at that distance. Range −50 to +50 cm.</p>
        <hr>
        <h2 id="me-selection">Select a distance to record</h2>
        <p>Hold the magnet still for the full 3 seconds.</p>
        <button class="btn btn-primary batch-record-button" id="me-rec" disabled>Record 3 seconds</button>
        <p id="me-count" class="capture-status" role="status" aria-live="polite"></p>
        <div class="row" id="me-recovery" hidden><button class="btn" id="me-retry">Retry saving</button><button class="btn btn-ghost" id="me-recover">Clear failed PC attempt</button></div>
        <p class="dim">The PC records the incoming USB stream for 3 seconds and saves it before the next recording. Trial numbers start at 1 for each distance.</p>
      </section>
      <section class="card chart-card">
        <div class="chart-head"><h2 id="me-chart-title">Live raw magnetic field</h2><div class="row"><button class="btn btn-ghost" id="me-live" hidden>Back to live</button><label class="field fi"><span>Sensor</span><select id="me-sensor">${[0,1,2,3].map(i=>'<option value="'+i+'">S'+(i+1)+'</option>').join('')}</select></label></div></div>
        <p id="me-trial-name" hidden></p>
        <div class="legend">${['Bx','By','Bz','|B|'].map((name,i)=>'<span class="series-label"><i style="background:'+SERIES_COLORS[i]+'"></i>'+name+'</span>').join('')}</div>
        <canvas id="me-chart" role="img" aria-label="Live raw Bx, By, Bz and field magnitude for the selected sensor"></canvas>
        <p id="me-rate" class="dim"></p>
        <div class="row" id="me-trial-actions" hidden><button class="btn btn-ghost" id="me-trial-rename">Rename trial</button><button class="btn btn-ghost" id="me-trial-remove">Remove trial</button></div>
        <p class="dim" id="me-graph-note"></p>
      </section>
    </div>
    <section class="card batch-coverage">
      <div class="row spread"><h2>Measured distances and trials</h2><span id="me-coverage-count" class="dim"></span></div>
      <p class="dim">Click a trial to view its saved graph. Uncheck a bad trial to exclude it without deleting its data. Each distance exports its included trials to one CSV.</p>
      <div id="me-distances"><p class="dim">Your distance sections will appear here.</p></div>
    </section>
    <details class="card"><summary>Live raw readings for all sensors (µT)</summary>
      <div class="table-scroll"><table class="tbl"><thead><tr><th scope="col">Sensor</th><th scope="col">Status</th><th scope="col">Bx</th><th scope="col">By</th><th scope="col">Bz</th><th scope="col">|B|</th></tr></thead><tbody id="me-readings"></tbody></table></div>
    </details>`;},
  enter(){
    $('#me-sensor').value=String(this.sensor);this.graph();
    $('#me-sensor').onchange=e=>{this.sensor=Number(e.target.value);this.graph();};
    $('#me-live').onclick=()=>this.goLive();
    for(const [button,kind,action] of [['me-session-rename','session','editItem'],['me-session-remove','session','removeItem'],['me-trial-rename','trial','editItem'],['me-trial-remove','trial','removeItem']])$('#'+button).onclick=e=>withBusy(e.currentTarget,()=>this[action](kind));
    this._s=[bus.on('sensors',()=>this.upd()),bus.on('record',()=>this.rec())];
    $('#me-session').onchange=e=>{this.sessionId=e.target.value;this.distanceId=null;this.load();};
    $('#me-new').onclick=e=>withBusy(e.currentTarget,async()=>{
      const name=await openModal({title:'New recording session',bodyHtml:'<label class="field"><span>Session name</span><input id="new-session-name" maxlength="80" required value="Magnet session"></label>',actions:[{label:'Cancel',value:null},{label:'Create session',primary:true,cls:'btn-primary',get:wrap=>{const input=$('#new-session-name',wrap);return input.reportValidity()?input.value.trim():undefined;}}]});
      if(name)this.apply(await rawApi('/sessions','POST',{name}),true);
    });
    $('#me-distance-form').onsubmit=async e=>{
      e.preventDefault();
      await withBusy($('#me-add'),async()=>{
        if(!this.data || !$('#me-ref').reportValidity())return;
        this.apply(await rawApi('/sessions/'+this.sessionId+'/distances','POST',{distanceCm:$('#me-ref').value}));
        $('#me-ref').value='';
      });this.rec();
    };
    $('#me-rec').onclick=async e=>{
      if(this._starting)return;
      this._starting=true;this._version=(this._version||0)+1;
      try{await withBusy(e.currentTarget,async()=>{
        if(!this.distanceId || this.data?.capture)return;
        this.goLive();
        this.renderDistances();this.rec();
        this.apply(await rawApi('/sessions/'+this.sessionId+'/record','POST',{distanceId:this.distanceId}));
      });}finally{this._starting=false;this.renderDistances();this.rec();}
    };
    $('#me-distances').onchange=e=>{
      if(e.target.matches('[data-distance]')){if(this.data?.capture || this._starting)return;this.distanceId=Number(e.target.value);this.renderDistances();this.rec();}
      if(e.target.matches('[data-trial]'))withBusy(e.target,async()=>{
        try{this.apply(await rawApi('/sessions/'+this.sessionId+'/trials/'+e.target.dataset.trial,'POST',{included:e.target.checked}));}
        catch(error){e.target.checked=!e.target.checked;throw error;}
      });
    };
    $('#me-distances').onclick=e=>{
      // Open the saved graph from the whole chip, including its frame count.
      // The inclusion checkbox remains a separate export-only action.
      const chip=e.target.closest('.trial-chip');
      const trial=chip && !e.target.closest('.trial-include')?chip.querySelector('[data-view-trial]'):null;
      const edit=e.target.closest('[data-edit-distance]'),remove=e.target.closest('[data-remove-distance]');
      if(trial)withBusy(trial,()=>this.showTrial(Number(trial.dataset.viewTrial)));
      if(edit)withBusy(edit,()=>this.editItem('distance',Number(edit.dataset.editDistance)));
      if(remove)withBusy(remove,()=>this.removeItem('distance',Number(remove.dataset.removeDistance)));
    };
    for(const [id,path] of [['me-retry','retry'],['me-recover','recover']])$('#'+id).onclick=e=>withBusy(e.currentTarget,async()=>{this.apply(await rawApi('/sessions/'+this.sessionId+'/'+path,'POST',{}));});
    this.load();this.upd();this.rec();
    this._age=setInterval(()=>this.upd(),1000);
    this._sessionPoll=setInterval(()=>this.load(),750);
  },
  leave(){this._s.forEach(f=>f());clearInterval(this._age);clearInterval(this._sessionPoll);this.chart?.destroy();this.chart=null;},
  graph(){
    if(!$('#me-chart'))return;
    const frames=this._trial?.frames;
    const buffers=['x','y','z','m'].map(k=>{
      if(!frames)return hist[this.sensor][k];
      const buffer=new RingBuf(Math.max(1,frames.length));
      for(const frame of frames){const s=frame.s.find(s=>s.id===this.sensor);buffer.push(s?.valid && s.st==='OK' && !s.sat?s['r'+k]:NaN);}
      return buffer;
    });
    this.chart?.destroy();
    this.chart=new CanvasChart($('#me-chart'),{series:['Bx','By','Bz','|B|'].map((name,i)=>({name,color:SERIES_COLORS[i]})),capacity:frames?frames.length:HIST_CAP,windowSec:frames?3:30,times:frames?frames.map(f=>f.elapsedMs):null,unit:'µT',fps:30});
    this.chart.setBuffers(buffers);this.upd();
  },
  goLive(){this._graphRequest=(this._graphRequest||0)+1;this._trial=null;this.graph();this.renderDistances();},
  async showTrial(id){
    const version=this._graphRequest=(this._graphRequest||0)+1,session=this.sessionId;
    const data=await rawApi('/sessions/'+session+'/trials/'+id,'GET');
    if(!$('#me-chart') || version!==this._graphRequest || session!==this.sessionId)return;
    this._trial=data;this.graph();this.renderDistances();this.rec();
    $('#me-chart').setAttribute('aria-label','Recorded Trial '+data.trial.number+' raw magnetic field, 0 to 3 seconds');
    $('#me-chart').closest('.chart-card')?.scrollIntoView({behavior:'smooth',block:'center'});
  },
  async editItem(kind,id){
    const item=kind==='session'?this.data.session:kind==='trial'?this._trial?.trial:this.data.session.distances.find(d=>d.id===id);
    if(!item)return;
    const numeric=kind==='distance',value=numeric?item.label:item.name||'';
    const title=numeric?'Edit measured distance':kind==='trial'?'Rename Trial '+item.number:'Rename session';
    const hint=numeric?'Updates the reference labels for every trial in this section. Raw magnetic readings stay intact.':kind==='trial'?'The original trial number stays the same. Leave the name blank to use only its number.':'';
    const result=await openModal({title,bodyHtml:'<label class="field"><span>'+(numeric?'Measured distance (cm)':'Name')+'</span><input id="batch-edit-value" '+(numeric?'type="number" min="-50" max="50" step="0.0001"':'type="text" maxlength="80"')+' '+(kind==='trial'?'':'required')+' value="'+esc(value)+'"></label><p class="dim">'+esc(hint)+'</p>',actions:[{label:'Cancel',value:null},{label:'Save',primary:true,cls:'btn-primary',get:wrap=>{const input=$('#batch-edit-value',wrap);return input.reportValidity()?{value:input.value}:undefined;}}]});
    if(!result)return;
    const path='/sessions/'+this.sessionId+(kind==='session'?'':'/'+(numeric?'distances':'trials')+'/'+item.id)+'/rename';
    this.apply(await rawApi(path,'POST',numeric?{distanceCm:result.value}:{name:result.value}));
  },
  async removeItem(kind,id){
    const item=kind==='session'?this.data.session:kind==='trial'?this._trial?.trial:this.data.session.distances.find(d=>d.id===id);
    if(!item)return;
    const label=kind==='session'?item.name:kind==='trial'?'Trial '+item.number+(item.name?' · '+item.name:''):item.label+' cm';
    const detail=kind==='trial'?'This permanently deletes this saved trial. Its number will not be reused.':'This permanently deletes this '+kind+' and all its saved trials.';
    if(!await confirmDlg('Remove '+label+'?',detail+' Previously exported files are unchanged.','Remove'))return;
    const path='/sessions/'+this.sessionId+(kind==='session'?'':'/'+(kind==='distance'?'distances':'trials')+'/'+item.id)+'/remove';
    this.apply(await rawApi(path,'POST',{}),kind==='session');
  },
  async load(){
    if(location.hostname!=='127.0.0.1' && location.hostname!=='localhost'){this._offline=true;setText('me-summary','Batch sessions need the PC USB dashboard at http://127.0.0.1:8765/. The raw graph remains available here.');this.rec();return;}
    if(this._loading || this._starting)return;this._loading=true;
    const requested=this.sessionId;
    const version=this._version||0;
    try{
      let data;
      try{data=await rawApi('/sessions'+(requested?'/'+requested:''),'GET');}
      catch(error){if(error.status===404 && requested)data=await rawApi('/sessions','GET');else throw error;}
      if(!requested){
        let remembered;try{remembered=localStorage.getItem('scratchsor-session');}catch(e){}
        const selected=data.capture?.session_id || remembered;
        if(selected && data.sessions.some(s=>s.id===selected) && selected!==data.session.id)data=await rawApi('/sessions/'+selected,'GET');
      }
      if(data.capture && data.capture.session_id!==data.session.id)data=await rawApi('/sessions/'+data.capture.session_id,'GET');
      if($('#me-session') && this.sessionId===requested && version===(this._version||0)){this._offline=false;this.apply(data);}
    }catch(e){if($('#me-session') && this.sessionId===requested && version===(this._version||0)){this._offline=true;setText('me-summary','Session storage is unavailable. '+errText(e));this.rec();}}
    finally{this._loading=false;}
  },
  apply(data,newSession=false){
    if(!$('#me-session'))return;
    this._version=(this._version||0)+1;
    const previousSession=this.sessionId;
    this.data=data;this.sessionId=data.session.id;this._offline=false;
    try{localStorage.setItem('scratchsor-session',this.sessionId);}catch(e){}
    if(newSession || previousSession!==this.sessionId)this.distanceId=null;
    if(data.selectedDistanceId)this.distanceId=data.selectedDistanceId;
    if(data.capture?.session_id===this.sessionId)this.distanceId=data.capture.distance_id;
    const distances=data.session.distances;
    if(!distances.some(d=>d.id===this.distanceId))this.distanceId=distances[0]?.id??null;
    const options=data.sessions.map(s=>'<option value="'+s.id+'">'+esc(s.name)+' · '+esc(new Date(s.created).toLocaleString('en-GB',{timeZone:'Asia/Bangkok',dateStyle:'short',timeStyle:'short'}))+'</option>').join('');
    if($('#me-session').innerHTML!==options)$('#me-session').innerHTML=options;
    $('#me-session').value=this.sessionId;
    if(this._trial){
      const group=distances.find(d=>d.trials.some(t=>t.id===this._trial.trial.id));
      if(!group || previousSession!==this.sessionId)this.goLive();
      else this._trial.trial={...this._trial.trial,...group.trials.find(t=>t.id===this._trial.trial.id),distanceCm:group.distanceCm,distanceLabel:group.label};
    }
    this.renderDistances();this.rec();
    this.upd();
  },
  renderDistances(){
    if(!this.data || !$('#me-distances'))return;
    const distances=this.data.session.distances;
    const locked=!!this.data.capture||!!this._starting;
    const all=distances.flatMap(d=>d.trials),included=all.filter(t=>t.included);
    setText('me-summary','Distances: '+distances.length+' · Saved trials: '+all.length+' · Included in exports: '+included.length+' · Saved on this PC');
    setText('me-coverage-count',all.reduce((n,t)=>n+t.frames,0)+' frames saved');
    const html=distances.length?distances.map(d=>{
      const kept=d.trials.filter(t=>t.included),selected=d.id===this.distanceId;
      const trials=d.trials.map(t=>`<div class="trial-chip ${t.included?'':'trial-excluded'} ${this._trial?.trial.id===t.id?'trial-selected':''}" title="${t.hz.toFixed(1)} Hz · ${t.healthy}/${t.frames*4} valid sensor rows">
        <label class="trial-include"><input type="checkbox" data-trial="${t.id}" ${t.included?'checked':''} aria-label="Include ${esc(d.label)} cm Trial ${t.number} in exports"></label>
        <button type="button" class="trial-open" data-view-trial="${t.id}" aria-label="View saved graph: ${esc(d.label)} cm Trial ${t.number}" aria-pressed="${this._trial?.trial.id===t.id}"><span>Trial ${t.number}</span>${t.name?'<span class="trial-name">'+esc(t.name)+'</span>':''}</button>
        <small>${t.frames} frames${t.healthy<t.frames*4?' · review':''}</small></div>`).join('');
      return `<section class="distance-group ${selected?'is-selected':''}">
        <div class="row spread distance-heading">
          <label class="distance-choice"><input type="radio" name="batch-distance" data-distance value="${d.id}" ${selected?'checked':''} ${locked?'disabled':''}><strong>${esc(d.label)} cm</strong><span class="dim">${d.trials.length} saved · ${kept.length} included</span></label>
          <div class="row distance-actions"><button type="button" class="btn btn-ghost" data-edit-distance="${d.id}" ${locked?'disabled':''}>Edit distance</button><button type="button" class="btn btn-ghost" data-remove-distance="${d.id}" ${locked?'disabled':''}>Remove</button><a class="btn btn-ghost" ${kept.length&&!locked?'href="/api/v1/sessions/'+this.sessionId+'/distances/'+d.id+'.csv" download':'aria-disabled="true"'}>Download CSV</a></div>
        </div>
        <div class="trial-list">${trials||'<p class="dim">No trials yet. Select this distance and record its first trial.</p>'}</div>
      </section>`;
    }).join(''):'<p class="batch-empty">Add your first measured distance above, then record as many trials as you need.</p>';
    if($('#me-distances').innerHTML!==html)$('#me-distances').innerHTML=html;
    const link=$('#me-export'),ready=included.length>0 && !locked;
    link.setAttribute('aria-disabled',String(!ready));if(ready)link.href='/api/v1/sessions/'+this.sessionId+'/export.zip';else link.removeAttribute('href');
  },
  upd(){
    if(!$('#me-readings'))return;
    const stale=!!S.lastSensorsAt && Date.now()-S.lastSensorsAt>3000;
    const trial=this._trial?.trial;
    setText('me-chart-title',trial?'Recorded Trial '+trial.number+' · '+trial.distanceLabel+' cm':'Live raw magnetic field');
    setText('me-trial-name',trial?.name||'');$('#me-trial-name').hidden=!trial?.name;
    $('#me-live').hidden=!trial;$('#me-trial-actions').hidden=!trial;
    setText('me-rate',trial?'Saved graph · '+trial.frames+' frames · '+num(trial.hz,1)+' Hz · 0–3 seconds · invalid/saturated readings appear as gaps':stale?'Waiting for fresh data':'Sensor acquisition '+num(S.sensors?.rate,1)+' Hz · PC records received USB frames · µT');
    setText('me-graph-note',trial?'This graph shows the saved capture. Viewing it does not change current recordings.':'Axes keep their signed direction. |B| is non-negative field strength. Graph and raw data stay live while recording.');
    setHtml('me-readings',(S.sensors?.s||[]).map(s=>{
      const ok=!stale && s.st==='OK' && !s.sat;
      return '<tr><th scope="row">S'+(s.id+1)+'</th><td>'+esc(stale?'STALE':s.sat?'SATURATED':s.st||'—')+'</td>'+['rx','ry','rz','rm'].map(k=>'<td class="mono">'+(ok&&fin(s[k])?fmtUt(s[k]):'—')+'</td>').join('')+'</tr>';
    }).join('')||'<tr><td colspan="6">Waiting for sensor readings…</td></tr>');
    if(this.chart && !trial){this.chart.series.forEach(v=>v.visible=!stale);if(!document.hidden)this.chart.requestDraw();}
  },
  rec(){
    if(!$('#me-rec'))return;
    const capture=this.data?.capture,group=this.data?.session.distances.find(d=>d.id===this.distanceId);
    const busy=!!capture||!!this._starting,bad=capture?.state==='error';
    const next=group?group.nextTrial??Math.max(0,...group.trials.map(t=>t.number))+1:1;
    setText('me-selection',group?group.label+' cm · next is Trial '+next:'Select a measured distance');
    setText('me-rec',capture?.state==='recording'?'Recording…':capture?.state==='saving'?'Saving trial…':'Record Trial '+next+' · 3 seconds');
    const remaining=S.record.startedMs===capture?.started_ms?S.record.remainingMs:capture?.remainingMs;
    setText('me-count',this._offline?'Session connection lost. Reconnect before recording.':capture?(capture.state==='recording'?'Hold still · '+((remaining??3000)/1000).toFixed(1)+' s remaining · '+(capture.frames||0)+' frames':capture.detail||'Saving trial…'):group?group.trials.length+(group.trials.length===1?' trial saved at ':' trials saved at ')+group.label+' cm. Ready for Trial '+next+'.':'Add a measured distance to begin.');
    $('#me-count').classList.toggle('warn-text',bad||!!this._offline);
    $('#me-rec').disabled=!group || busy || !!S.record.active || !!this._offline;
    $('#me-ref').disabled=busy;$('#me-add').disabled=busy||!this.data||!!this._offline;
    $('#me-session').disabled=busy;$('#me-new').disabled=busy;
    for(const button of document.querySelectorAll('#me-session-rename,#me-session-remove,#me-trial-rename,#me-trial-remove,[data-edit-distance],[data-remove-distance]'))button.disabled=busy||!this.data;
    $('#me-recovery').hidden=!bad;
  },
},

/* ---------- 5. Wi-Fi ---------- */
wifi: {
  title: 'Wi-Fi', _s: [], _poll: 0,
  html() { return `
  <section class="card"><h2>Station (STA)</h2>
    <dl class="kv">
      <dt>State</dt><dd id="wf-state">—</dd>
      <dt>Network</dt><dd id="wf-ssid">—</dd>
      <dt>IP</dt><dd id="wf-ip">—</dd>
      <dt>Signal</dt><dd id="wf-rssi">—</dd>
      <dt>Last error</dt><dd id="wf-err">—</dd></dl>
    <div class="row" style="margin-top:10px"><button class="btn btn-danger" id="wf-forget">Forget network</button></div>
  </section>
  <section class="card"><h2>Join a network</h2>
    <div class="row"><button class="btn" id="wf-scan">Scan for networks</button><span class="dim" id="wf-scanst"></span></div>
    <div id="wf-nets"></div>
    <form id="wf-form" autocomplete="off">
      <label class="field"><span>SSID</span><input id="wf-ssid-in" type="text" required autocomplete="off" spellcheck="false"></label>
      <label class="field"><span>Password</span><input id="wf-pass-in" type="password" autocomplete="off"></label>
      <button class="btn btn-primary" type="submit" id="wf-connect">Connect</button>
    </form>
  </section>
  <section class="card"><h2>Access point</h2>
    <dl class="kv">
      <dt>SSID</dt><dd id="wf-ap-ssid">—</dd>
      <dt>IP</dt><dd id="wf-ap-ip">—</dd>
      <dt>Clients</dt><dd id="wf-ap-n">—</dd></dl>
    <p class="dim">The AP stays up as the management interface unless disabled in Settings.</p>
  </section>
  <section class="card"><h2>Troubleshooting</h2>
    <ul class="tips">
      <li>Connect attempts are bounded (~15 s); the device falls back to its AP automatically.</li>
      <li>Lost STA connectivity? Rejoin the SCRATCHSOR AP and browse to <b>192.168.4.1</b>.</li>
      <li>The stored STA password is never shown — re-enter it to change networks.</li>
      <li>RSSI above −70 dBm is recommended for a stable link.</li>
    </ul>
  </section>`; },
  enter() {
    this._s = [bus.on('status', () => this.upd())];
    this.refresh();
    $('#wf-scan').addEventListener('click', e => withBusy(e.currentTarget, () => this.scan()));
    $('#wf-form').addEventListener('submit', e => {
      e.preventDefault();
      withBusy($('#wf-connect'), async () => {
        const ssid = $('#wf-ssid-in').value.trim(), pass = $('#wf-pass-in').value;
        if (!ssid) { toast('SSID required', 'error'); return; }
        await api('/wifi/connect', { method: 'POST', body: { ssid, pass } });
        $('#wf-pass-in').value = '';
        toast('Connecting to “' + ssid + '”…', 'info');
        this.pollSta();
      });
    });
    $('#wf-forget').addEventListener('click', e => withBusy(e.currentTarget, async () => {
      if (!await confirmDlg('Forget Wi-Fi network?', 'The device disconnects and clears stored station credentials. The AP stays reachable.', 'Forget')) return;
      await api('/wifi/forget', { method: 'POST' });
      toast('Station credentials cleared', 'ok');
      this.refresh();
    }));
    this.upd();
  },
  leave() { this._s.forEach(u => u()); clearTimeout(this._poll); },
  refresh() { api('/wifi/status').then(d => { S.wifi = d; this.upd(); }).catch(e => toast(errText(e), 'error')); },
  upd() {
    const w = wifiInfo();
    setText('wf-state', w.staState || '—');
    setText('wf-ssid', w.staSsid || '—');
    setText('wf-ip', w.staIp || '—');
    setText('wf-rssi', fin(w.rssi) && w.rssi !== 0 ? w.rssi + ' dBm (' + rssiWord(w.rssi) + ')' : '—');
    setText('wf-err', w.staError || '—');
    setText('wf-ap-ssid', w.apSsid || '—');
    setText('wf-ap-ip', w.apIp || '—');
    setText('wf-ap-n', w.apClients != null ? w.apClients : '—');
  },
  async scan() {
    const box = $('#wf-nets');
    for (let i = 0; i < 24; i++) {
      const d = await api('/wifi/scan');
      if (!document.getElementById('wf-nets')) return;      // navigated away
      if (d && d.done) { this.renderNets(d.nets || []); setText('wf-scanst', (d.nets || []).length + ' found'); return; }
      setText('wf-scanst', 'scanning…');
      await sleep(1500);
    }
    setText('wf-scanst', 'scan timed out');
    if (box) box.innerHTML = '<p class="dim">No results — try again.</p>';
  },
  renderNets(nets) {
    const box = $('#wf-nets'); if (!box) return;
    nets = nets.slice().sort((a, b) => (b.rssi || -99) - (a.rssi || -99));
    if (!nets.length) { box.innerHTML = '<p class="dim">No networks found.</p>'; return; }
    box.innerHTML = '<ul class="net-list">' + nets.map((n, i) => {
      const r = fin(n.rssi) ? n.rssi : -99;
      const bars = r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1;
      return '<li><button type="button" class="net" data-i="' + i + '">' +
        '<span class="net-ssid">' + esc(n.ssid || '(hidden)') + '</span>' +
        '<span class="net-meta"><span class="pill pill-sm ' + (n.open ? 'pill-warn' : 'pill-mute') + '">' +
        (n.open ? 'open' : 'secured') + '</span>' +
        '<span class="bars b' + bars + '" aria-label="' + r + ' dBm"><i></i><i></i><i></i><i></i></span>' +
        '<span class="dim">' + r + '</span></span></button></li>';
    }).join('') + '</ul>';
    $$('.net', box).forEach(b => b.addEventListener('click', () => {
      const n = nets[+b.dataset.i];
      const inp = $('#wf-ssid-in');
      if (inp) { inp.value = n.ssid || ''; const pw = $('#wf-pass-in'); if (pw) pw.focus(); }
    }));
  },
  async pollSta() {
    clearTimeout(this._poll);
    for (let i = 0; i < 8; i++) {
      await sleep(2000);
      if (!document.getElementById('wf-state')) return;
      api('/wifi/status').then(d => { S.wifi = d; this.upd(); }).catch(() => {});
      const st = wifiInfo().staState;
      if (st === 'CONNECTED' || st === 'FAILED') return;
    }
  },
},

/* ---------- 6. Settings ---------- */
settings: {
  title: 'Settings', _s: [],
  html() { return `
  <form id="set-form">
  <section class="card"><h2>Device</h2>
    <label class="field"><span>Device name</span><input id="s-name" type="text" maxlength="31" autocomplete="off"></label>
    <label class="switch-row"><span>Keep access point running <span class="hint">(management interface)</span></span>
      <span class="switch"><input id="s-appers" type="checkbox"><span class="sw-track"></span></span></label>
  </section>
  <section class="card"><h2>Acquisition</h2>
    <label class="field"><span>Sample rate — <b id="s-ratev">50 Hz</b> (1–50)</span>
      <input id="s-rate" type="range" min="1" max="50" step="1" value="50"></label>
    <label class="switch-row"><span>Read temperature</span>
      <span class="switch"><input id="s-temp" type="checkbox"><span class="sw-track"></span></span></label>
  </section>
  <section class="card"><h2>OLED</h2>
    <label class="field"><span>Refresh rate (1–10 Hz)</span><input id="s-ohz" type="number" min="1" max="10" value="4"></label>
    <label class="field"><span>Rotation disabled during measurement</span><input id="s-orot" type="number" disabled min="0" max="60" value="5"></label>
    <label class="switch-row"><span>Flip display</span>
      <span class="switch"><input id="s-oflip" type="checkbox"><span class="sw-track"></span></span></label>
    <label class="field"><span>Controller override</span>
      <select id="s-octl"><option value="0">Auto-detect</option><option value="1">SSD1306</option><option value="2">SH1106</option></select></label>
  </section>
  <div class="row" style="margin-bottom:12px">
    <button class="btn btn-primary" type="submit" id="set-save">Save settings</button>
    <span class="dim" id="set-st"></span>
  </div>
  </form>

  <section class="card"><h2>Security</h2>
    <p class="dim">The admin token matches the device Wi-Fi name/password (SCRATCHSOR-XXXX). It is kept in
      this tab's session storage only.</p>
    <div class="row">
      <button class="btn" id="set-token">Re-enter token</button>
      <button class="btn btn-ghost" id="set-token-clear">Forget token</button>
      <span class="dim" id="set-tokst"></span>
    </div>
  </section>
  <section class="card danger-zone"><h2>Maintenance</h2>
    <div class="row"><button class="btn" id="set-restart">Restart device</button></div>
    <hr>
    <label class="switch-row"><span>Keep Wi-Fi credentials</span>
      <span class="switch"><input id="s-keepwifi" type="checkbox" checked><span class="sw-track"></span></span></label>
    <p class="dim">Factory reset wipes settings and calibration data.</p>
    <button class="btn btn-danger" id="set-factory">Factory reset</button>
  </section>
  <section class="card"><h2>Diagnostics</h2>
    <div class="row"><button class="btn" id="set-diag">Load diagnostics</button></div>
    <pre class="diag" id="set-diag-out" hidden></pre>
  </section>`; },
  enter() {
    this._s = [];
    api('/config').then(c => { S.config = c; this.fill(c); }).catch(e => toast(errText(e), 'error'));
    $('#s-rate').addEventListener('input', () => setText('s-ratev', $('#s-rate').value + ' Hz'));
    $('#set-form').addEventListener('submit', e => {
      e.preventDefault();
      withBusy($('#set-save'), async () => {
        const saved = await api('/config', { method: 'POST', body: this.gather() });
        S.config = saved;
        setText('set-st', 'saved');
        toast('Settings saved', 'ok');
      });
    });
    $('#s-appers').addEventListener('change', e => {
      if (!e.target.checked)
        toast('Disabling the AP removes this management interface — make sure STA is connected first.', 'warn', 6000);
    });
    $('#set-token').addEventListener('click', e => withBusy(e.currentTarget, async () => {
      Token.clear();
      await Token.ensure();
      setText('set-tokst', 'token stored for this session');
    }));
    $('#set-token-clear').addEventListener('click', () => { Token.clear(); setText('set-tokst', 'token forgotten'); });
    $('#set-restart').addEventListener('click', e => withBusy(e.currentTarget, async () => {
      if (!await confirmDlg('Restart device?', 'The device reboots; live data pauses for a few seconds.', 'Restart', false)) return;
      await api('/restart', { method: 'POST' });
      toast('Device restarting — it will reconnect shortly', 'info', 6000);
    }));
    $('#set-factory').addEventListener('click', e => withBusy(e.currentTarget, async () => {
      const keep = $('#s-keepwifi').checked;
      if (!await confirmDlg('Factory reset?', 'Settings and calibration data are wiped. ' +
          (keep ? 'Wi-Fi credentials are kept.' : 'Wi-Fi credentials are also cleared — you will need the AP to reconnect.'), 'Reset everything')) return;
      await api('/factory_reset', { method: 'POST', body: { keepWifi: keep } });
      toast('Factory reset issued — device rebooting', 'info', 6000);
    }));
    $('#set-diag').addEventListener('click', e => withBusy(e.currentTarget, async () => {
      const out = {};
      try { out.device = await api('/device'); } catch (x) { out.device = errText(x); }
      try { out.health = await api('/health'); } catch (x) { out.health = errText(x); }
      try { out.wifi = await api('/wifi/status'); } catch (x) { out.wifi = errText(x); }
      const pre = $('#set-diag-out');
      if (pre) { pre.hidden = false; pre.textContent = JSON.stringify(out, null, 2); }
    }));
    setText('set-tokst', Token.get() ? 'token stored for this session' : 'no token yet');
  },
  leave() { this._s.forEach(u => u()); },
  fill(c) {
    $('#s-name').value = c.deviceName || '';
    $('#s-appers').checked = !!c.apPersistent;
    const s = c.sensor || {};
    $('#s-rate').value = fin(s.rateHz) ? s.rateHz : 50;
    setText('s-ratev', num(fin(s.rateHz) ? s.rateHz : 50, 0) + ' Hz');
    $('#s-temp').checked = s.temp !== false;
    const o = c.oled || {};
    $('#s-ohz').value = o.refreshHz != null ? o.refreshHz : 4;
    $('#s-orot').value = o.rotateSec != null ? o.rotateSec : 5;
    $('#s-oflip').checked = !!o.flip;
    $('#s-octl').value = o.controller != null ? o.controller : 0;
  },
  gather() {
    return {
      deviceName: $('#s-name').value.trim() || 'SCRATCHSOR',
      apPersistent: $('#s-appers').checked,
      sensor: { rateHz: clamp(+$('#s-rate').value || 50, 1, 50), temp: $('#s-temp').checked },
      oled: {
        refreshHz: clamp(+$('#s-ohz').value || 4, 1, 10),
        rotateSec: clamp(+$('#s-orot').value || 0, 0, 60),
        flip: $('#s-oflip').checked,
        controller: clamp(+$('#s-octl').value || 0, 0, 2),
      },
    };
  },
},


};

/* ---------- hash router ---------- */
let current = null, currentName = '';
function route() {
  let name = (location.hash || '').replace(/^#\/?/, '').split('?')[0] || 'measure';
  if (!Views[name]) { name = 'measure'; history.replaceState(null,'','#/measure'); }
  if (name === currentName) return;
  if (current && current.leave) current.leave();
  currentName = name;
  current = Views[name];
  $$('#main-nav a').forEach(a => {
    a.classList.toggle('active', a.dataset.route === name);
    if (a.dataset.route === name) a.setAttribute('aria-current', 'page');
    else a.removeAttribute('aria-current');
  });
  const v = $('#view');
  v.innerHTML = '<h1>' + esc(current.title) + '</h1>' + current.html();
  v.className = 'view view-' + name;
  document.title = 'SCRATCHSOR · ' + current.title;
  if (current.enter) current.enter();
  v.focus({ preventScroll: true });
}
window.addEventListener('hashchange', route);

/* ---------- boot ---------- */
function boot() {
  route();
  if (location.hostname === '127.0.0.1' || location.hostname === 'localhost') {
    const events = new EventSource('/usb/events');
    let connected = false;
    events.onmessage = ev => {
      let m;
      try { m = JSON.parse(ev.data); } catch (e) { return; }
      if (m.t === 'usb') {
        connected = m.connected === true;
        setWs(connected ? 'usb' : 'offline');
      } else {
        onFrame(m);
        if (connected && m.t === 'sensors') setWs('usb');
      }
    };
    events.onerror = () => { connected = false; setWs('offline'); };
    setInterval(() => {
      if (connected && S.lastSensorsAt && Date.now() - S.lastSensorsAt > 3000) setWs('stale');
    }, 1000);
  } else {
    const host = location.hostname || '192.168.4.1';
    const ws = new WsClient('ws://' + host + ':81/ws');
    ws.start();
  }
  api('/device').then(d => {
    S.device = d;
    setText('foot-device', (d && d.name) || 'SCRATCHSOR');
  }).catch(() => {});
}
if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
else boot();
