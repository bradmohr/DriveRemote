'use strict';
/*
 * ACS550 Remote - phone app for the ACS550 panel.
 *
 * The phone never talks to the panel directly. Both connect to the same MQTT
 * broker (EMQX): the panel publishes scaled values to <base>/XXXX and listens
 * for commands on <base>/XXXX<suffix>. See README.md section 6.
 */

const STORE_KEY = 'acs550-remote';
const $ = (sel) => document.querySelector(sel);

// ---- ACS550 tables (match src/acs550.cpp in the firmware) -------------------
const VALUES = [
  { par: '0102', label: 'Motor speed', unit: 'rpm' },
  { par: '0103', label: 'Output freq', unit: 'Hz' },
  { par: '0104', label: 'Current', unit: 'A' },
  { par: '0105', label: 'Torque', unit: '%' },
  { par: '0106', label: 'Power', unit: 'kW' },
  { par: '0107', label: 'DC bus', unit: 'V' },
  { par: '0109', label: 'Output volts', unit: 'V' },
  { par: '0110', label: 'Drive temp', unit: '°C' },
];
const QUICK = ['0103', '0104', '0106', '0105'];

const SPEED_REF = '*'; // rpm in vector mode, Hz in scalar mode (9904 = 3)
const PARAMS = {
  '0001': ['CONTROL WORD', ''], '0002': ['SPEED SETPOINT', 'rpm'], '0004': ['STATUS WORD', ''],
  '0101': ['SPEED & DIR', 'rpm'], '0102': ['SPEED', 'rpm'], '0103': ['OUTPUT FREQ', 'Hz'],
  '0104': ['CURRENT', 'A'], '0105': ['TORQUE', '%'], '0106': ['POWER', 'kW'],
  '0107': ['DC BUS VOLTAGE', 'V'], '0109': ['OUTPUT VOLTAGE', 'V'], '0110': ['DRIVE TEMP', '°C'],
  '0111': ['EXTERNAL REF 1', SPEED_REF], '0112': ['EXTERNAL REF 2', '%'], '0113': ['CTRL LOCATION', ''],
  '0114': ['RUN TIME (R)', 'h'], '0115': ['KWH COUNTER (R)', 'kWh'], '0120': ['AI 1', '%'], '0121': ['AI 2', '%'],
  '0124': ['AO 1', 'mA'], '0125': ['AO 2', 'mA'], '0126': ['PID 1 OUTPUT', '%'],
  '0401': ['LAST FAULT', ''], '0412': ['PREVIOUS FAULT 1', ''], '0413': ['PREVIOUS FAULT 2', ''],
  '1104': ['REF1 MIN', SPEED_REF], '1105': ['REF1 MAX', SPEED_REF],
  '2001': ['MINIMUM SPEED', 'rpm'], '2002': ['MAXIMUM SPEED', 'rpm'], '2003': ['MAX CURRENT', 'A'],
  '2007': ['MINIMUM FREQ', 'Hz'], '2008': ['MAXIMUM FREQ', 'Hz'],
  '2202': ['ACCELER TIME 1', 's'], '2203': ['DECELER TIME 1', 's'], '2205': ['ACCELER TIME 2', 's'],
  '2206': ['DECELER TIME 2', 's'], '2208': ['EMERG DEC TIME', 's'], '3019': ['COMM FAULT TIME', 's'],
  '9904': ['MOTOR CTRL MODE', ''], '9905': ['MOTOR NOM VOLT', 'V'], '9906': ['MOTOR NOM CURR', 'A'],
  '9907': ['MOTOR NOM FREQ', 'Hz'], '9908': ['MOTOR NOM SPEED', 'rpm'], '9909': ['MOTOR NOM POWER', ''],
};

const SW_BITS = [
  ['Ready to switch on', ''], ['Ready to run', ''], ['Running (ref enabled)', ''], ['FAULT', 'bad'],
  ['OFF2 inactive', ''], ['OFF3 inactive', ''], ['Switch-on inhibited', 'warn'], ['ALARM', 'warn'],
  ['At setpoint', ''], ['Remote (fieldbus)', ''], ['Above limit', ''], ['EXT2 selected', ''], ['Ext run enable', ''],
];

const FAULTS = {
  0: 'None', 1: 'OVERCURRENT', 2: 'DC OVERVOLT', 3: 'DEV OVERTEMP', 4: 'SHORT CIRC', 6: 'DC UNDERVOLT',
  7: 'AI1 LOSS', 8: 'AI2 LOSS', 9: 'MOT OVERTEMP', 10: 'PANEL LOSS', 11: 'ID RUN FAIL', 12: 'MOTOR STALL',
  14: 'EXT FAULT 1', 15: 'EXT FAULT 2', 16: 'EARTH FAULT', 18: 'THERM FAIL', 19: 'OPEX LINK', 20: 'OPEX PWR',
  21: 'CURR MEAS', 22: 'SUPPLY PHASE', 24: 'OVERSPEED', 26: 'DRIVE ID', 27: 'CONFIG FILE',
  28: 'SERIAL 1 ERR (comm loss)', 29: 'EFB CON FILE', 34: 'MOTOR PHASE', 35: 'OUTP WIRING',
};

// ---- state -------------------------------------------------------------------
const state = {
  vals: {},          // 'XXXX' -> payload as published by the panel
  panel: '',         // <base>/status: 'online' | 'offline'
  run: '',           // <base>/state, e.g. 'RUNNING - AT SPEED'
  brokerKind: '',    // '' | 'ok' | 'warn' | 'bad'
  brokerText: 'Not connected',
};
let cfg = loadConfig();
let client = null;
let dir = 1;               // direction for the next setpoint send
let spDirty = false;       // user has changed the setpoint but not sent it
let spHoldUntil = 0;       // keep the user's value on screen until the panel confirms
let pendingRead = null;    // { par } while waiting for a "?" answer
let modalAction = null;
let toastTimer = null;
let renderQueued = false;

// ---- helpers -----------------------------------------------------------------
const pad4 = (p) => String(p).padStart(4, '0');
const timeNow = () => new Date().toLocaleTimeString([], { hour12: false });
const isNumber = (s) => /^-?\d+(\.\d+)?$/.test(s);

function num(par) {
  const v = parseFloat(state.vals[par]);
  return Number.isFinite(v) ? v : NaN;
}
const scalarMode = () => num('9904') === 3;
const nameOf = (par) => (PARAMS[par] ? PARAMS[par][0] : '');
function unitOf(par) {
  const p = PARAMS[par];
  if (!p) return '';
  return p[1] === SPEED_REF ? (scalarMode() ? 'Hz' : 'rpm') : p[1];
}
// Largest setpoint in rpm = 1105 REF1 MAX expressed in rpm (same rule as the panel)
function spMaxRpm() {
  const r1 = num('1105'), nf = num('9907'), ns = num('9908');
  if (!(r1 > 0)) return NaN;
  if (scalarMode()) return nf > 0 && ns > 0 ? (r1 * ns) / nf : NaN;
  return r1;
}
const sliderMax = () => (spMaxRpm() > 0 ? Math.round(spMaxRpm()) : 1800);
const connected = () => !!(client && client.connected);
const isRunning = () => state.run.startsWith('RUNNING');

function cmdSuffix() {
  const s = (cfg.suffix || '').trim();
  return s && s[0] !== '/' ? '/' + s : s;
}

function loadConfig() {
  const d = { url: '', user: '', pass: '', base: 'Drive', suffix: '', remember: true };
  try {
    return Object.assign(d, JSON.parse(localStorage.getItem(STORE_KEY) || '{}'));
  } catch (e) {
    return d;
  }
}

function saveConfig() {
  const c = Object.assign({}, cfg);
  if (!c.remember) c.pass = '';
  try {
    localStorage.setItem(STORE_KEY, JSON.stringify(c));
  } catch (e) { /* private mode: settings last for this session only */ }
}

// ---- log, toast, confirm -------------------------------------------------------
function addLog(text, cls) {
  const el = $('#log');
  const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 40;
  const line = document.createElement('div');
  line.className = cls || '';
  line.textContent = text;
  el.appendChild(line);
  while (el.children.length > 300) el.removeChild(el.firstChild);
  if (atBottom) el.scrollTop = el.scrollHeight;
  const last = $('#lastLog');
  last.textContent = text;
  last.className = 'lastlog ' + (cls || '');
}
const appLog = (msg, cls) => addLog(`${timeNow()} app ${msg}`, cls);
// Panel lines look like "14:02:11 T Start: CW 0x047F REF 16667"
const panelLogClass = (line) => ({ E: 'err', O: 'ok', T: 'tx', W: 'warn' })[line.charAt(9)] || '';

function toast(msg) {
  const t = $('#toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove('show'), 2800);
}

function confirmBox(title, text, okLabel, okClass, action) {
  $('#mTitle').textContent = title;
  $('#mText').textContent = text;
  const ok = $('#mOk');
  ok.textContent = okLabel;
  ok.className = 'btn ' + okClass;
  modalAction = action;
  $('#modal').classList.add('show');
}

// ---- MQTT ----------------------------------------------------------------------
function setBroker(kind, text) {
  state.brokerKind = kind;
  state.brokerText = text;
  scheduleRender();
}

function connect() {
  disconnect(true);
  state.vals = {};
  state.panel = '';
  state.run = '';
  if (!cfg.url) {
    setBroker('', 'Not set up');
    return;
  }
  setBroker('warn', 'Connecting…');
  client = mqtt.connect(cfg.url, {
    clientId: 'acs550-app-' + Math.random().toString(16).slice(2, 10),
    username: cfg.user || undefined,
    password: cfg.pass || undefined,
    clean: true,
    keepalive: 30,
    reconnectPeriod: 4000,
    connectTimeout: 10000,
    timerVariant: 'native', // plain timers; the app reconnects whenever it comes back to the foreground
  });
  const c = client;
  c.on('connect', () => {
    setBroker('ok', 'Connected');
    c.subscribe(cfg.base + '/#', { qos: 1 });
    appLog('Connected to ' + new URL(cfg.url).host, 'ok');
  });
  c.on('reconnect', () => setBroker('warn', 'Reconnecting…'));
  c.on('offline', () => setBroker('bad', 'Offline'));
  c.on('close', () => { if (client === c && !c.connected && state.brokerKind === 'ok') setBroker('bad', 'Disconnected'); });
  c.on('error', (err) => {
    appLog('Broker: ' + err.message, 'err');
    setBroker('bad', err.message);
  });
  c.on('message', onMessage);
}

function disconnect(silent) {
  if (client) {
    client.end(true);
    client = null;
  }
  if (!silent) {
    setBroker('', 'Disconnected');
    appLog('Disconnected');
  }
}

function onMessage(topic, payload) {
  const prefix = cfg.base + '/';
  if (!topic.startsWith(prefix)) return;
  const sub = topic.slice(prefix.length);
  const text = payload.toString().trim();
  const sfx = cmdSuffix();
  if (sfx && sub.endsWith(sfx)) return; // command topics

  if (sub === 'log') addLog(text, panelLogClass(text));
  else if (sub === 'state') state.run = text;
  else if (sub === 'status') state.panel = text;
  else if (/^\d{4}$/.test(sub) && isNumber(text)) {
    // (non-numbers on these topics are commands such as START or ?, not values)
    state.vals[sub] = text;
    if (pendingRead && pendingRead.par === sub) {
      pendingRead = null;
      parResult(`${sub} ${nameOf(sub)} = ${text} ${unitOf(sub)}`.trim(), 'ok');
      $('#parVal').value = text;
    }
  } else return;
  scheduleRender();
}

// Publish a command to <base>/XXXX<suffix>. Never retained: the panel ignores retained commands.
function send(par, payload) {
  if (!connected()) {
    toast('Not connected to the broker');
    return false;
  }
  const topic = `${cfg.base}/${pad4(par)}${cmdSuffix()}`;
  client.publish(topic, String(payload), { qos: 1, retain: false }, (err) => {
    if (err) appLog('Publish failed: ' + err.message, 'err');
  });
  appLog(`sent ${topic} = ${payload}`, 'tx');
  if (state.panel !== 'online') toast('Sent, but the panel is offline');
  return true;
}

// ---- rendering -----------------------------------------------------------------
function scheduleRender() {
  if (renderQueued) return;
  renderQueued = true;
  requestAnimationFrame(render);
}

function setDot(sel, kind) {
  $(sel + ' .dot').className = 'dot ' + (kind || '');
}

function setValue(id, par) {
  const el = document.getElementById(id);
  if (el) el.querySelector('.v').textContent = state.vals[par] ?? '--';
}

function render() {
  renderQueued = false;
  const online = connected();
  const panelUp = online && state.panel === 'online';

  setDot('#pBroker', state.brokerKind);
  setDot('#pPanel', !online ? '' : state.panel === 'online' ? 'ok' : state.panel === 'offline' ? 'bad' : 'warn');
  setDot('#pDrive', !panelUp || !state.run ? '' : state.run === 'NO COMMS' ? 'bad' : 'ok');

  let banner = '';
  if (!cfg.url) banner = 'Enter your broker details in Settings.';
  else if (!online) banner = 'Not connected to the broker: ' + state.brokerText;
  else if (state.panel === 'offline') banner = 'The panel is offline. Commands will not reach the drive.';
  else if (state.run === 'NO COMMS') banner = 'The panel cannot reach the drive (no Modbus replies).';
  $('#banner').textContent = banner;
  $('#banner').hidden = !banner;
  document.body.classList.toggle('stale', !panelUp);

  // Gauge
  const speed = num('0102');
  const gmax = Math.max(spMaxRpm() || 0, num('9908') || 0) || 1500;
  $('#bigSpeed').textContent = Number.isFinite(speed) ? Math.round(speed) : '----';
  const frac = Number.isFinite(speed) ? Math.min(1, Math.abs(speed) / gmax) : 0;
  $('#arc').setAttribute('stroke-dasharray', `${(frac * 377).toFixed(1)} 400`);
  $('#gMax').textContent = Math.round(gmax);

  const rs = $('#runState');
  rs.textContent = state.run || '--';
  rs.className = 'runstate' + (isRunning() ? ' run'
    : state.run === 'FAULT' || state.run === 'NO COMMS' ? ' fault'
    : /LOCAL|INHIBITED/.test(state.run) ? ' warn' : '');

  QUICK.forEach((p) => setValue('q' + p, p));
  VALUES.forEach((v) => setValue('v' + v.par, v.par));

  // Setpoint: show the panel's value unless the user is editing
  const sp = num('0002');
  $('#spNow').textContent = Number.isFinite(sp) ? `${Math.abs(Math.round(sp))} rpm ${sp < 0 ? 'REV' : 'FWD'}` : '--';
  $('#spSlider').max = sliderMax();
  if (!spDirty && Date.now() > spHoldUntil && Number.isFinite(sp)) {
    $('#spNum').value = Math.abs(Math.round(sp));
    $('#spSlider').value = Math.abs(sp);
    dir = sp < 0 ? -1 : 1;
  }
  $('#dirF').classList.toggle('on', dir > 0);
  $('#dirR').classList.toggle('on', dir < 0);
  $('#spPending').hidden = !spDirty;

  // Values page
  const lf = state.vals['0401'];
  $('#lastFault').textContent = lf != null ? `${lf} - ${FAULTS[+lf] || 'see manual'}` : '--';
  const sw = num('0004');
  $('#swHex').textContent = Number.isFinite(sw) ? '0x' + (sw & 0xffff).toString(16).toUpperCase().padStart(4, '0') : '--';
  SW_BITS.forEach((b, i) => {
    document.getElementById('b' + i).classList.toggle('on', Number.isFinite(sw) && ((sw >> i) & 1) === 1);
  });

  ['#btnStart', '#btnStop', '#btnCoast', '#btnQuick', '#btnReset', '#btnSetSpeed', '#btnParRead', '#btnParWrite']
    .forEach((s) => { $(s).disabled = !online; });
  $('#connInfo').textContent = cfg.url ? `${state.brokerText} · ${cfg.base}/XXXX${cmdSuffix()}` : '';
}

// ---- drive page ------------------------------------------------------------------
function setSetpointInputs(v) {
  $('#spNum').value = v;
  $('#spSlider').value = v;
  spDirty = true;
  scheduleRender();
}

function step(sign) {
  const inc = sliderMax() >= 600 ? 10 : 1;
  const cur = parseFloat($('#spNum').value) || 0;
  const v = Math.max(0, Math.min(sliderMax(), Math.round(cur / inc) * inc + sign * inc));
  setSetpointInputs(v);
}

function sendSetpoint() {
  let v = parseFloat($('#spNum').value);
  if (!Number.isFinite(v) || v < 0) {
    toast('Enter a speed in rpm');
    return;
  }
  const max = spMaxRpm();
  if (max > 0 && v > max) {
    v = Math.floor(max);
    toast(`Limited to ${v} rpm (1105 REF1 MAX)`);
  }
  const value = Math.round(v) * dir;
  const go = () => {
    if (send(2, value)) {
      spDirty = false;
      spHoldUntil = Date.now() + 3000;
      $('#spNum').value = Math.abs(value);
      scheduleRender();
    }
  };
  const current = num('0002');
  if (isRunning() && Number.isFinite(current) && Math.sign(value || 1) !== Math.sign(current || 1)) {
    confirmBox('Reverse direction', `The motor is running. Change direction to ${dir < 0 ? 'REVERSE' : 'FORWARD'} at ${Math.abs(value)} rpm?`,
      'Reverse', 'amber', go);
  } else {
    go();
  }
}

// ---- param page ----------------------------------------------------------------
function parResult(text, cls) {
  const el = $('#parOut');
  el.textContent = text;
  el.className = 'par-out ' + (cls || '');
}

function parFromInput() {
  const p = parseInt($('#parNo').value, 10);
  if (!(p > 0 && p <= 9999)) {
    parResult('Enter a parameter number, e.g. 1105 or 2202', 'err');
    return null;
  }
  if (p === 1 || p === 2) {
    parResult('Use the Drive page for start/stop and speed', 'err');
    return null;
  }
  return pad4(p);
}

function readParam() {
  const par = parFromInput();
  if (!par || !send(par, '?')) return;
  pendingRead = { par };
  parResult(`Reading ${par} ${nameOf(par)}…`, '');
  setTimeout(() => {
    if (pendingRead && pendingRead.par === par) {
      pendingRead = null;
      parResult(`No answer for ${par}. Is the panel online?`, 'err');
    }
  }, 6000);
}

function writeParam() {
  const par = parFromInput();
  if (!par) return;
  const v = $('#parVal').value.trim();
  if (!isNumber(v)) {
    parResult('Enter the value to write', 'err');
    return;
  }
  confirmBox('Write parameter', `Write ${v} ${unitOf(par)} to ${par} ${nameOf(par)}? The drive uses the new value immediately.`,
    'Write', 'amber', () => {
      if (send(par, v)) parResult(`Sent ${par} = ${v}. Check the Log for the panel's answer.`, '');
    });
}

// ---- settings ------------------------------------------------------------------
function fillSettings() {
  $('#sUrl').value = cfg.url;
  $('#sUser').value = cfg.user;
  $('#sPass').value = cfg.pass;
  $('#sBase').value = cfg.base;
  $('#sSuffix').value = cfg.suffix;
  $('#sRemember').checked = cfg.remember;
}

// Accept whatever the user pastes from the EMQX console: a bare address,
// host:port, mqtts://..., https://... or a full ws(s):// URL.
function brokerUrl(input) {
  const s = input.trim();
  const m = s.match(/^([a-z][a-z0-9+.-]*):\/\/(.*)$/i);
  const scheme = m ? m[1].toLowerCase() : '';
  const rest = m ? m[2] : s;
  const hp = rest.match(/^([^/:\s]+)(?::(\d+))?(\/\S*)?$/);
  if (!hp) return null;
  const isWs = scheme === 'ws' || scheme === 'wss' || scheme === 'http' || scheme === 'https';
  const secure = scheme !== 'ws' && scheme !== 'http';
  // Keep a typed WebSocket port; MQTT TCP ports (1883/8883) don't work from a browser
  const tcpPort = hp[2] === '1883' || hp[2] === '8883';
  const keep = isWs || (!scheme && !tcpPort);
  const port = keep && hp[2] ? hp[2] : secure ? '8084' : '8083';
  const path = keep && hp[3] ? hp[3] : '/mqtt';
  return `${secure ? 'wss' : 'ws'}://${hp[1]}:${port}${path}`;
}

function saveSettings() {
  const url = brokerUrl($('#sUrl').value);
  if (!url) {
    toast('Enter the EMQX address, e.g. abc123.ala.us-east-1.emqxsl.com');
    return;
  }
  $('#sUrl').value = url;
  if (location.protocol === 'https:' && /^ws:/i.test(url)) {
    toast('Use wss:// (browsers block ws:// from an https page)');
    return;
  }
  const base = $('#sBase').value.trim().replace(/\/+$/, '') || 'Drive';
  if (/[+#]/.test(base) || /[+#]/.test($('#sSuffix').value)) {
    toast('Topics cannot contain + or #');
    return;
  }
  cfg = {
    url,
    user: $('#sUser').value.trim(),
    pass: $('#sPass').value,
    base,
    suffix: $('#sSuffix').value.trim(),
    remember: $('#sRemember').checked,
  };
  saveConfig();
  connect();
  showTab('drive');
}

// ---- tabs + startup ----------------------------------------------------------------
function showTab(name) {
  document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.id === 'tab-' + name));
  document.querySelectorAll('.tabbar button').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
  if (name === 'log') $('#log').scrollTop = $('#log').scrollHeight;
}

function tile(id, label, par, unit) {
  return `<div class="val" id="${id}"><div class="k">${label} · ${par}</div>` +
    `<div><span class="v">--</span><span class="u">${unit}</span></div></div>`;
}

function init() {
  $('#quick').innerHTML = QUICK.map((p) => {
    const v = VALUES.find((x) => x.par === p);
    return tile('q' + p, v.label, p, v.unit);
  }).join('');
  $('#vals').innerHTML = VALUES.map((v) => tile('v' + v.par, v.label, v.par, v.unit)).join('');
  $('#bits').innerHTML = SW_BITS.map((b, i) => `<div class="bit ${b[1]}" id="b${i}"><i></i>${i}: ${b[0]}</div>`).join('');

  document.querySelectorAll('.tabbar button').forEach((b) => { b.onclick = () => showTab(b.dataset.tab); });

  $('#btnStart').onclick = () => confirmBox('Start drive', 'Start the motor now?', 'START', 'start', () => send(1, 'START'));
  $('#btnStop').onclick = () => send(1, 'STOP');
  $('#btnCoast').onclick = () => send(1, 'COAST');
  $('#btnQuick').onclick = () => send(1, 'QSTOP');
  $('#btnReset').onclick = () => send(1, 'RESET');

  $('#spSlider').oninput = () => setSetpointInputs($('#spSlider').value);
  $('#spNum').oninput = () => { $('#spSlider').value = $('#spNum').value; spDirty = true; scheduleRender(); };
  $('#spMinus').onclick = () => step(-1);
  $('#spPlus').onclick = () => step(1);
  $('#dirF').onclick = () => { dir = 1; spDirty = true; scheduleRender(); };
  $('#dirR').onclick = () => { dir = -1; spDirty = true; scheduleRender(); };
  $('#btnSetSpeed').onclick = sendSetpoint;

  $('#btnParRead').onclick = readParam;
  $('#btnParWrite').onclick = writeParam;
  $('#btnClearLog').onclick = () => { $('#log').innerHTML = ''; $('#lastLog').textContent = ''; };

  $('#btnSave').onclick = saveSettings;
  $('#btnDisconnect').onclick = () => disconnect(false);
  $('#sShow').onchange = () => { $('#sPass').type = $('#sShow').checked ? 'text' : 'password'; };

  $('#mOk').onclick = () => {
    $('#modal').classList.remove('show');
    const action = modalAction;
    modalAction = null;
    if (action) action();
  };
  $('#mCancel').onclick = () => {
    $('#modal').classList.remove('show');
    modalAction = null;
  };

  // Phones drop sockets in the background; reconnect as soon as the app is visible again
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible' && client && !client.connected) client.reconnect();
  });

  if ('serviceWorker' in navigator && location.protocol === 'https:') {
    navigator.serviceWorker.register('sw.js').catch(() => {});
  }

  fillSettings();
  if (cfg.url) connect();
  else showTab('settings');
  render();
}

init();
