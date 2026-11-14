// Sensor-Actor web UI: hash-routed single page talking to /api/*.
'use strict';

const $ = (sel) => document.querySelector(sel);
const PAGES = ['home', 'sensors', 'sensor', 'settings', 'status'];
let timer = null;

async function api(path, body) {
  const opt = body === undefined ? {} : {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
  };
  const r = await fetch('/api/' + path, opt);
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.error || ('HTTP ' + r.status));
  return data;
}

function duration(s) {
  s = Math.floor(s);
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  if (d) return `${d}d ${h}h`;
  if (h) return `${h}h ${m}m`;
  if (m) return `${m}m ${s % 60}s`;
  return `${s}s`;
}

function wifiText(w) {
  if (w.state === 'connected') return [`Connected`, `${w.ssid} · ${w.ip} · ${w.rssi} dBm`];
  if (w.state === 'access point') {
    const retry = w.ssid ? ` · retrying "${w.ssid}" in ${duration(w.next_retry_s)}` : '';
    return ['Access point', `${w.ap_ssid} · ${w.ap_ip}${retry}`];
  }
  if (w.state === 'reconnecting' || w.state === 'connecting') {
    return ['Reconnecting', `${w.ssid} · lost ${duration(w.lost_for_s)} · next try ${duration(w.next_retry_s)}`];
  }
  return [w.state, ''];
}

function mqttText(m) {
  if (m.state === 'not configured') return ['Off', 'No broker configured'];
  if (m.state === 'connected') return ['Connected', `${m.broker} · ${m.published} messages sent`];
  return ['Disconnected', `${m.broker} · for ${duration(m.disconnected_s)}${m.last_error ? ' · ' + m.last_error : ''}`];
}

// Small DOM helper: el('td', {className: 'num'}, 'text', childNode, ...)
function el(tag, props, ...children) {
  const e = Object.assign(document.createElement(tag), props || {});
  for (const c of children) if (c !== null && c !== undefined) e.append(c);
  return e;
}

function qtyName(q) { return q.name.charAt(0).toUpperCase() + q.name.slice(1); }

function fmt(v, q) {
  if (v === undefined || v === null) return '–';
  const digits = q.unit === 'mm' ? 0 : 1;
  return `${Number(v).toFixed(digits)} ${q.unit}`;
}

// ------------------------------------------------------------- pages

async function refreshHome() {
  const s = await api('status');
  header(s);
  const [w, wsub] = wifiText(s.wifi);
  $('#h-wifi').textContent = w;
  $('#h-wifi-sub').textContent = wsub;
  const [m, msub] = mqttText(s.mqtt);
  $('#h-mqtt').textContent = m;
  $('#h-mqtt-sub').textContent = msub;
  $('#h-uptime').textContent = duration(s.device.uptime_s);
  $('#h-reset').textContent = 'Last start: ' + s.device.reset_reason;
  $('#h-log').textContent = s.log.count;

  const { sensors } = await api('sensors');
  const cards = [];
  for (const sn of sensors.filter((x) => x.favourite)) {
    for (const q of sn.quantities) {
      cards.push(el('a', { className: 'card' + (q.stale ? ' stale' : ''), href: '#/sensor/' + sn.id },
        el('h3', {}, `${sn.label} · ${qtyName(q)}`),
        el('div', { className: 'big' }, q.valid ? fmt(q.value, q) : '–'),
        el('div', { className: 'sub' }, q.valid ? `median ${fmt(q.median, q)}` : sn.state)));
    }
  }
  $('#h-favs').replaceChildren(...cards);
  $('#h-favs-empty').hidden = cards.length > 0;
}

async function refreshSensors() {
  const [s, { sensors }] = await Promise.all([api('status'), api('sensors')]);
  header(s);
  const rows = [];
  for (const sn of sensors) {
    const qs = sn.quantities.length ? sn.quantities : [{ name: '', unit: '' }];
    qs.forEach((q, i) => {
      const first = i === 0;
      const tr = el('tr', { className: q.stale ? 'stale' : '' },
        el('td', {}, first ? el('a', { href: '#/sensor/' + sn.id }, sn.label) : '',
           first ? el('div', { className: 'sub' }, `${sn.id} · ${sn.chip_name} 0x${sn.addr.toString(16)}`) : '',
           q.name ? el('div', { className: 'sub' }, qtyName(q)) : ''),
        el('td', { className: 'num' }, q.valid ? fmt(q.value, q) : '–'),
        el('td', { className: 'num' }, q.valid ? fmt(q.median, q) : '–'),
        el('td', { className: 'state-' + sn.state, title: sn.error }, first ? sn.state + (sn.error ? ': ' + sn.error : '') : ''),
        el('td', {}, first ? activeToggle(sn) : ''));
      rows.push(tr);
    });
  }
  if (!rows.length) rows.push(el('tr', {}, el('td', { colSpan: 5, className: 'muted' }, 'No sensors yet. Use "Scan bus" below.')));
  $('#sensor-rows').replaceChildren(...rows);
}

function activeToggle(sn) {
  const cb = el('input', { type: 'checkbox', checked: sn.active, title: 'Active' });
  cb.addEventListener('change', async () => {
    try { await api('sensors/' + sn.id, { active: cb.checked }); } catch (e) { alert(e.message); }
    refreshSensors();
  });
  return cb;
}

$('#btn-scan').addEventListener('click', async () => {
  const m = $('#scan-msg');
  m.textContent = 'Scanning…'; m.className = 'msg';
  try {
    const { devices } = await api('scan');
    m.textContent = devices.length ? `${devices.length} device(s) found.` : 'Nothing found: check wiring, 3.3 V power and pull-up resistors.';
    $('#scan-table').hidden = !devices.length;
    $('#scan-rows').replaceChildren(...devices.map((d) => {
      let action;
      if (d.configured) action = el('a', { href: '#/sensor/' + d.sensor_id }, 'configured as ' + d.sensor_id);
      else if (!d.supported) action = el('span', { className: 'muted' }, 'not supported');
      else {
        action = el('button', { type: 'button', className: 'primary' }, 'Add');
        action.addEventListener('click', async () => {
          try {
            const r = await api('sensors', { addr: d.addr, chip: d.chip });
            location.hash = '#/sensor/' + r.id;
          } catch (e) { m.textContent = e.message; m.className = 'msg err'; }
        });
      }
      return el('tr', {}, el('td', { className: 'num' }, '0x' + d.addr.toString(16).padStart(2, '0')),
        el('td', {}, d.chip_name), el('td', { className: 'muted' }, d.detail), el('td', {}, action));
    }));
  } catch (e) { m.textContent = e.message; m.className = 'msg err'; }
});

// ---- sensor detail

let currentSensor = null;

function sensorIdFromHash() {
  return decodeURIComponent((location.hash.split('/')[2] || '').split('?')[0]);
}

async function loadSensor() {
  const sn = await api('sensors/' + sensorIdFromHash());
  currentSensor = sn.id;
  const f = $('#sensor-form');
  f.label.value = sn.label; f.id.value = sn.id;
  f.active.checked = sn.active; f.favourite.checked = sn.favourite;
  f.interval_s.value = sn.interval_s; f.median_n.value = sn.median_n;
  $('#sd-vl').hidden = sn.vl_mode === undefined;
  if (sn.vl_mode !== undefined) { f.vl_mode.value = sn.vl_mode; f.mount_mm.value = sn.mount_mm; }
  $('#sd-hw').textContent = `${sn.chip_name} at I2C address 0x${sn.addr.toString(16).padStart(2, '0')}`;
  $('#sd-offsets').replaceChildren(...sn.quantities.map((q) =>
    el('label', {}, `${qtyName(q)} (${q.unit})`,
      el('input', { type: 'number', step: 'any', name: 'off_' + q.name, value: sn.offsets[q.name] !== undefined ? sn.offsets[q.name] : 0 }))));
  showSensorValues(sn);
}

function showSensorValues(sn) {
  $('#sd-title').textContent = sn.label;
  $('#sd-values').replaceChildren(...sn.quantities.map((q) =>
    el('div', { className: 'card' + (q.stale ? ' stale' : '') },
      el('h3', {}, qtyName(q)),
      el('div', { className: 'big' }, q.valid ? fmt(q.value, q) : '–'),
      el('div', { className: 'sub' }, q.valid ? `median ${fmt(q.median, q)} · ${duration(q.age_s)} ago` : 'no reading yet'))));
  $('#sd-error').textContent = sn.state === 'ok' ? '' : `State: ${sn.state}${sn.error ? ' – ' + sn.error : ''}`;
}

async function refreshSensor() {
  if (!currentSensor) return;
  showSensorValues(await api('sensors/' + currentSensor));
}

function smsg(text, cls) { const m = $('#sensor-msg'); m.textContent = text; m.className = 'msg ' + (cls || ''); }

$('#sensor-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const f = ev.target;
  if (!f.checkValidity()) { f.reportValidity(); smsg('Please correct the highlighted fields.', 'err'); return; }
  const body = {
    label: f.label.value.trim(), id: f.id.value.trim(), active: f.active.checked, favourite: f.favourite.checked,
    interval_s: Number(f.interval_s.value), median_n: Number(f.median_n.value), offsets: {},
  };
  if (!$('#sd-vl').hidden) { body.vl_mode = f.vl_mode.value; body.mount_mm = Number(f.mount_mm.value || 0); }
  for (const inp of f.querySelectorAll('[name^=off_]')) body.offsets[inp.name.slice(4)] = Number(inp.value || 0);
  try {
    const sn = await api('sensors/' + currentSensor, body);
    smsg('Saved.', 'ok');
    if (sn.id !== currentSensor) { location.hash = '#/sensor/' + sn.id; return; }
    loadSensor();
  } catch (e) { smsg(e.message, 'err'); }
});

$('#btn-sensor-delete').addEventListener('click', async () => {
  if (!confirm(`Remove sensor "${currentSensor}"? Its settings are deleted; it can be added again with a scan.`)) return;
  try {
    await fetch('/api/sensors/' + currentSensor, { method: 'DELETE' });
    location.hash = '#/sensors';
  } catch (e) { smsg(e.message, 'err'); }
});


async function refreshStatus() {
  const [s, l] = await Promise.all([api('status'), api('log')]);
  header(s);
  const [w, wsub] = wifiText(s.wifi);
  const rows = [
    ['Device', `${s.device.name} (${s.device.id})`],
    ['Firmware', `${s.device.firmware} · ESP-IDF ${s.device.idf} · slot ${s.device.partition}`],
    ['Uptime', `${duration(s.device.uptime_s)} · last start: ${s.device.reset_reason}`],
    ['Free memory', `${(s.device.heap_free / 1024).toFixed(0)} KB (lowest ${(s.device.heap_min / 1024).toFixed(0)} KB)`],
    ['WiFi', `${w}${wsub ? ' · ' + wsub : ''}`],
    ['Connection attempts', s.wifi.state === 'connected' ? '–' : String(s.wifi.attempts)],
    ['MQTT', mqttText(s.mqtt).join(' · ')],
    ['Warnings / errors', String(s.log.count)],
  ];
  const t = $('#status-table');
  t.replaceChildren(...rows.map(([k, v]) => {
    const tr = document.createElement('tr');
    const th = document.createElement('th'); th.textContent = k;
    const td = document.createElement('td'); td.textContent = v;
    tr.append(th, td);
    return tr;
  }));

  const body = $('#log-body');
  if (!l.entries.length) {
    const tr = document.createElement('tr');
    const td = document.createElement('td'); td.colSpan = 3; td.className = 'muted';
    td.textContent = 'No warnings or errors since the last start.';
    tr.append(td);
    body.replaceChildren(tr);
    return;
  }
  body.replaceChildren(...l.entries.map((e) => {
    const tr = document.createElement('tr');
    const when = document.createElement('td');
    when.textContent = e.time ? new Date(e.time * 1000).toLocaleString()
                              : `${duration((l.uptime_ms - e.uptime_ms) / 1000)} ago`;
    const lvl = document.createElement('td'); lvl.textContent = e.level; lvl.className = 'lvl-' + e.level;
    const txt = document.createElement('td'); txt.textContent = e.text;
    tr.append(when, lvl, txt);
    return tr;
  }));
}

async function loadSettings() {
  const s = await api('settings');
  const f = $('#settings');
  for (const el of f.elements) {
    if (!el.name || !(el.name in s)) continue;
    if (el.type === 'checkbox') el.checked = !!s[el.name];
    else el.value = s[el.name];
  }
  for (const el of f.querySelectorAll('[data-secret]')) {
    el.value = '';
    el.placeholder = s[el.dataset.secret + '_set'] ? '(unchanged)' : '(none)';
  }
  f.wifi_pass_clear.checked = false;
  f.mqtt_pass_clear.checked = false;
}

function settingsPayload(f) {
  const out = {};
  for (const el of f.elements) {
    if (!el.name) continue;
    if (el.type === 'checkbox') out[el.name] = el.checked;
    else if (el.type === 'number') out[el.name] = Number(el.value);
    else if (el.dataset.secret) { if (el.value) out[el.name] = el.value; }
    else out[el.name] = el.value.trim();
  }
  return out;
}

function msg(text, cls) {
  const m = $('#settings-msg');
  m.textContent = text;
  m.className = 'msg ' + (cls || '');
}

async function waitForRestart() {
  // Poll until the device answers again (it may come back on another address if WiFi changed).
  for (let i = 0; i < 40; i++) {
    await new Promise((r) => setTimeout(r, 1500));
    try { await api('status'); msg('Restarted with the new settings.', 'ok'); loadSettings(); return; } catch (e) { /* still down */ }
  }
  msg('The device did not come back on this address. If you changed the WiFi settings, find it on the new network.', 'err');
}

$('#settings').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const f = ev.target;
  if (!f.checkValidity()) { f.reportValidity(); msg('Please correct the highlighted fields.', 'err'); return; }
  const btn = f.querySelector('button[type=submit]');
  btn.disabled = true;
  try {
    await api('settings', settingsPayload(f));
    msg('Saved. Restarting…', 'ok');
    waitForRestart();
  } catch (e) {
    msg(e.message, 'err');
  } finally {
    btn.disabled = false;
  }
});

$('#btn-reboot').addEventListener('click', async () => {
  if (!confirm('Restart the device?')) return;
  try { await api('reboot', {}); msg('Restarting…', 'ok'); waitForRestart(); } catch (e) { msg(e.message, 'err'); }
});

// ------------------------------------------------------------- shell

function header(s) {
  $('#hdr-name').textContent = s.device.name;
  document.title = s.device.name;
  const dot = $('#hdr-dot');
  dot.className = 'dot ' + (s.wifi.state === 'connected' ? 'ok' : s.wifi.state === 'access point' ? 'warn' : 'err');
}

function show() {
  const page = (location.hash.replace(/^#\/?/, '') || 'home').split('/')[0];
  const name = PAGES.includes(page) ? page : 'home';
  for (const p of PAGES) $('#page-' + p).hidden = p !== name;
  const navPage = name === 'sensor' ? 'sensors' : name;
  for (const a of document.querySelectorAll('nav a[data-page]')) a.classList.toggle('active', a.dataset.page === navPage);

  clearInterval(timer);
  currentSensor = null;
  if (name === 'sensor') {
    smsg('');
    loadSensor().then(() => { timer = setInterval(() => refreshSensor().catch(() => {}), 2000); })
      .catch((e) => { $('#sd-title').textContent = 'Sensor not found'; smsg(e.message, 'err'); });
    return;
  }
  const refresh = { home: refreshHome, status: refreshStatus, sensors: refreshSensors }[name];
  if (refresh) {
    const run = () => refresh().catch(() => { $('#hdr-dot').className = 'dot err'; });
    run();
    timer = setInterval(run, 2000);   // REQ-UI-1
  } else {
    msg('');
    loadSettings().catch((e) => msg(e.message, 'err'));
  }
}

window.addEventListener('hashchange', show);
show();
