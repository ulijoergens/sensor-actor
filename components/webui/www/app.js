// Sensor-Actor web UI: hash-routed single page talking to /api/*.
'use strict';

const $ = (sel) => document.querySelector(sel);
const PAGES = ['home', 'settings', 'status'];
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

// ------------------------------------------------------------- pages

async function refreshHome() {
  const s = await api('status');
  header(s);
  const [w, wsub] = wifiText(s.wifi);
  $('#h-wifi').textContent = w;
  $('#h-wifi-sub').textContent = wsub;
  $('#h-mqtt').textContent = s.mqtt.state === 'not configured' ? 'Off' : '–';
  $('#h-mqtt-sub').textContent = s.mqtt.state;
  $('#h-uptime').textContent = duration(s.device.uptime_s);
  $('#h-reset').textContent = 'Last start: ' + s.device.reset_reason;
  $('#h-log').textContent = s.log.count;
}

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
    ['MQTT', s.mqtt.state],
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
  for (const a of document.querySelectorAll('nav a[data-page]')) a.classList.toggle('active', a.dataset.page === name);

  clearInterval(timer);
  const refresh = { home: refreshHome, status: refreshStatus }[name];
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
