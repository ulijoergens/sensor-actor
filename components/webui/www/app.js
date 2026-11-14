// Sensor-Actor web UI: hash-routed single page talking to /api/*.
'use strict';

const $ = (sel) => document.querySelector(sel);
const PAGES = ['home', 'sensors', 'sensor', 'actors', 'actor', 'settings', 'status'];
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
  const { actors } = await api('actors');
  for (const a of actors.filter((x) => x.favourite)) {
    const btn = el('button', { type: 'button', className: a.state === 'ON' ? 'switch-on' : '' }, a.state === 'ON' ? 'On' : 'Off');
    btn.disabled = a.state === 'inactive';
    btn.addEventListener('click', (ev) => { ev.preventDefault(); switchActor(a.id, 'TOGGLE').then(refreshHome); });
    cards.push(el('a', { className: 'card', href: '#/actor/' + a.id },
      el('h3', {}, a.label), el('div', { className: 'big state-' + a.state }, a.state), el('div', { className: 'sub' }, btn)));
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

// ---- actors

function switchActor(id, state) {
  return api('actors/' + id + '/set', { state }).catch((e) => alert(e.message));
}

async function refreshActors() {
  const [s, { actors }] = await Promise.all([api('status'), api('actors')]);
  header(s);
  const rows = actors.map((a) => {
    const btn = el('button', { type: 'button', className: a.state === 'ON' ? 'switch-on' : '' }, a.state === 'ON' ? 'On' : 'Off');
    btn.disabled = a.state === 'inactive';
    btn.addEventListener('click', () => switchActor(a.id, 'TOGGLE').then(refreshActors));
    return el('tr', {},
      el('td', {}, el('a', { href: '#/actor/' + a.id }, a.label), el('div', { className: 'sub' }, `${a.id} · GPIO ${a.gpio}${a.active_low ? ' · active low' : ''}`)),
      el('td', { className: 'state-' + a.state }, a.state, el('div', { className: 'sub' }, a.state === 'inactive' ? '' : `by ${a.source}, ${duration(a.changed_s)} ago`)),
      el('td', { className: 'sub' }, a.rule.enabled ? `${a.rule.status}: ${a.rule.text}` : '–'),
      el('td', {}, btn));
  });
  if (!rows.length) rows.push(el('tr', {}, el('td', { colSpan: 4, className: 'muted' }, 'No actors yet. Add one below.')));
  $('#actor-rows').replaceChildren(...rows);
}

$('#actor-add').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const m = $('#actor-add-msg');
  try {
    const r = await api('actors', { gpio: Number(ev.target.gpio.value) });
    location.hash = '#/actor/' + r.id;
  } catch (e) { m.textContent = e.message; m.className = 'msg err'; }
});

let currentActor = null;
const ACTIONS = [['none', '–'], ['on', 'On'], ['off', 'Off']];

function fillActions() {
  for (const n of ['t0_rising', 't0_falling', 't1_rising', 't1_falling']) {
    $('#actor-form')[n].replaceChildren(...ACTIONS.map(([v, t]) => el('option', { value: v }, t)));
  }
}
fillActions();

function rulePreview() {
  const f = $('#actor-form');
  if (!f.rule_enabled.checked) { $('#rule-preview').textContent = ''; return; }
  const [sid, q] = (f.rule_q.value || '|').split('|');
  const name = `${sid} ${q}`;
  const th = [0, 1].filter((t) => f[`t${t}_enabled`].checked && f[`t${t}_value`].value !== '')
    .map((t) => ({ v: Number(f[`t${t}_value`].value), r: f[`t${t}_rising`].value, fl: f[`t${t}_falling`].value }))
    .sort((a, b) => a.v - b.v);
  const parts = [];
  for (const t of th) {
    if (t.r !== 'none') parts.push(`${t.r.toUpperCase()} when ${name} rises above ${t.v}`);
    if (t.fl !== 'none') parts.push(`${t.fl.toUpperCase()} when ${name} falls to ${t.v} or below`);
  }
  $('#rule-preview').textContent = parts.length ? 'Rule: ' + parts.join('; ') : 'Rule: no action yet – set at least one threshold action.';
}

function actorIdFromHash() {
  return decodeURIComponent((location.hash.split('/')[2] || '').split('?')[0]);
}

async function loadActor() {
  const [a, { sensors }] = await Promise.all([api('actors/' + actorIdFromHash()), api('sensors')]);
  currentActor = a.id;
  const f = $('#actor-form');
  f.label.value = a.label; f.id.value = a.id; f.gpio.value = a.gpio;
  f.active.checked = a.active; f.active_low.checked = a.active_low; f.favourite.checked = a.favourite;
  f.power_on.value = a.power_on;
  const r = a.rule;
  const opts = [];
  for (const sn of sensors) for (const q of sn.quantities) {
    opts.push(el('option', { value: `${sn.id}|${q.name}` }, `${sn.label} – ${qtyName(q)} (${q.unit})`));
  }
  const cur = `${r.sensor}|${r.quantity}`;
  if (r.sensor && !opts.some((o) => o.value === cur)) opts.push(el('option', { value: cur }, `${r.sensor} – ${r.quantity} (not found)`));
  f.rule_q.replaceChildren(...opts);
  if (r.sensor) f.rule_q.value = cur;
  f.rule_enabled.checked = r.enabled; f.rule_source.value = r.source; f.rule_fault.value = r.fault;
  r.thresholds.forEach((t, i) => {
    f[`t${i}_enabled`].checked = t.enabled; f[`t${i}_value`].value = t.enabled || t.value ? t.value : '';
    f[`t${i}_rising`].value = t.rising; f[`t${i}_falling`].value = t.falling;
  });
  $('#rule-fields').hidden = !r.enabled;
  rulePreview();
  showActorState(a);
}

function showActorState(a) {
  $('#ad-title').textContent = a.label;
  const st = $('#ad-state');
  st.textContent = a.state; st.className = 'big state-' + a.state;
  $('#ad-state-sub').textContent = a.state === 'inactive' ? 'pin floating' : `by ${a.source}, ${duration(a.changed_s)} ago · GPIO ${a.gpio} level ${a.gpio_level}`;
  $('#ad-rule-status').textContent = a.rule.enabled ? a.rule.status : 'off';
  $('#ad-rule-text').textContent = a.rule.enabled ? a.rule.text : 'No rule.';
  for (const b of document.querySelectorAll('#page-actor [data-cmd]')) b.disabled = a.state === 'inactive';
}

async function refreshActor() {
  if (currentActor) showActorState(await api('actors/' + currentActor));
}

for (const b of document.querySelectorAll('#page-actor [data-cmd]')) {
  b.addEventListener('click', async () => {
    try { showActorState(await api('actors/' + currentActor + '/set', { state: b.dataset.cmd })); } catch (e) { amsg(e.message, 'err'); }
  });
}

$('#actor-form').addEventListener('input', rulePreview);
$('#actor-form').rule_enabled.addEventListener('change', (ev) => { $('#rule-fields').hidden = !ev.target.checked; });

function amsg(text, cls) { const m = $('#actor-msg'); m.textContent = text; m.className = 'msg ' + (cls || ''); }

$('#actor-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const f = ev.target;
  if (!f.checkValidity()) { f.reportValidity(); amsg('Please correct the highlighted fields.', 'err'); return; }
  const [sid, q] = (f.rule_q.value || '|').split('|');
  const body = {
    label: f.label.value.trim(), id: f.id.value.trim(), gpio: Number(f.gpio.value),
    active: f.active.checked, active_low: f.active_low.checked, favourite: f.favourite.checked, power_on: f.power_on.value,
    rule: {
      enabled: f.rule_enabled.checked, source: f.rule_source.value, fault: f.rule_fault.value,
      thresholds: [0, 1].map((t) => ({
        enabled: f[`t${t}_enabled`].checked, value: Number(f[`t${t}_value`].value || 0),
        rising: f[`t${t}_rising`].value, falling: f[`t${t}_falling`].value,
      })),
    },
  };
  if (sid) { body.rule.sensor = sid; body.rule.quantity = q; }
  try {
    const a = await api('actors/' + currentActor, body);
    amsg('Saved.', 'ok');
    if (a.id !== currentActor) { location.hash = '#/actor/' + a.id; return; }
    loadActor();
  } catch (e) { amsg(e.message, 'err'); }
});

$('#btn-actor-delete').addEventListener('click', async () => {
  if (!confirm(`Remove actor "${currentActor}"? Its pin is released.`)) return;
  await fetch('/api/actors/' + currentActor, { method: 'DELETE' });
  location.hash = '#/actors';
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

  const prev = l.previous || [];
  $('#prev-info').textContent = prev.length
    ? `The last ${prev.length} warnings/errors from earlier starts (this is start #${l.boot}).`
    : `No stored warnings or errors from earlier starts (this is start #${l.boot}).`;
  $('#prev-table').hidden = !prev.length;
  $('#prev-body').replaceChildren(...prev.map((e) => el('tr', {},
    el('td', { className: 'num' }, '#' + e.boot),
    el('td', { className: 'num' }, e.time ? new Date(e.time * 1000).toLocaleString() : `${duration(e.uptime_s)} after start`),
    el('td', { className: 'lvl-' + e.level }, e.level),
    el('td', {}, e.text))));

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

async function loadOta() {
  const o = await api('ota');
  const slot = (s) => s && s.version ? `${s.version} (${s.label}, built ${s.built}${s.state ? ', ' + s.state : ''})` : '–';
  const rows = [['Running', slot(o.running)], ['Other slot', slot(o.other)]];
  if (o.last_invalid) rows.push(['Last rejected firmware', slot(o.last_invalid)]);
  rows.push(['Maximum size', `${(o.max_size / 1024).toFixed(0)} KB`]);
  $('#ota-table').replaceChildren(...rows.map(([k, v]) => el('tr', {}, el('th', {}, k), el('td', {}, v))));
  return o;
}

function otaMsg(text, cls) { const m = $('#ota-msg'); m.textContent = text; m.className = 'msg ' + (cls || ''); }

$('#btn-ota').addEventListener('click', () => {
  const file = $('#ota-file').files[0];
  if (!file) { otaMsg('Choose the firmware file first.', 'err'); return; }
  if (!confirm(`Install ${file.name} (${(file.size / 1024).toFixed(0)} KB) and restart the node?`)) return;
  const btn = $('#btn-ota'), bar = $('#ota-progress');
  btn.disabled = true; bar.hidden = false; bar.value = 0;
  otaMsg('Uploading…');
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota');
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.upload.onprogress = (e) => { if (e.lengthComputable) bar.value = (100 * e.loaded) / e.total; };
  xhr.onload = async () => {
    let r = {};
    try { r = JSON.parse(xhr.responseText); } catch (e) { /* ignore */ }
    if (xhr.status !== 200) { otaMsg(r.error || `Upload failed (HTTP ${xhr.status})`, 'err'); btn.disabled = false; bar.hidden = true; return; }
    otaMsg(`Firmware ${r.version} installed. Restarting…`, 'ok');
    for (let i = 0; i < 60; i++) {
      await new Promise((res) => setTimeout(res, 2000));
      try {
        const o = await loadOta();
        if (o.running.version === r.version) { otaMsg(`Running firmware ${r.version} now.`, 'ok'); break; }
        if (i > 15) { otaMsg(`The node is back but runs ${o.running.version}: the new firmware was rolled back.`, 'err'); break; }
      } catch (e) { /* still restarting */ }
    }
    btn.disabled = false; bar.hidden = true;
  };
  xhr.onerror = () => { otaMsg('Upload failed: connection lost.', 'err'); btn.disabled = false; bar.hidden = true; };
  xhr.send(file);
});

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

$('#btn-log-clear').addEventListener('click', async () => {
  if (!confirm('Clear the stored log of earlier starts?')) return;
  await fetch('/api/log', { method: 'DELETE' });
  refreshStatus();
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
  const navPage = name === 'sensor' ? 'sensors' : name === 'actor' ? 'actors' : name;
  for (const a of document.querySelectorAll('nav a[data-page]')) a.classList.toggle('active', a.dataset.page === navPage);

  clearInterval(timer);
  currentSensor = null;
  currentActor = null;
  if (name === 'actor') {
    amsg('');
    loadActor().then(() => { timer = setInterval(() => refreshActor().catch(() => {}), 2000); })
      .catch((e) => { $('#ad-title').textContent = 'Actor not found'; amsg(e.message, 'err'); });
    return;
  }
  if (name === 'sensor') {
    smsg('');
    loadSensor().then(() => { timer = setInterval(() => refreshSensor().catch(() => {}), 2000); })
      .catch((e) => { $('#sd-title').textContent = 'Sensor not found'; smsg(e.message, 'err'); });
    return;
  }
  const refresh = { home: refreshHome, status: refreshStatus, sensors: refreshSensors, actors: refreshActors }[name];
  if (refresh) {
    const run = () => refresh().catch(() => { $('#hdr-dot').className = 'dot err'; });
    run();
    timer = setInterval(run, 2000);   // REQ-UI-1
  } else {
    msg('');
    loadSettings().catch((e) => msg(e.message, 'err'));
    loadOta().catch((e) => otaMsg(e.message, 'err'));
  }
}

window.addEventListener('hashchange', show);
show();
