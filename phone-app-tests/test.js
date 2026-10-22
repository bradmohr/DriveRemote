// End-to-end test of phone-app/ against a local MQTT broker and a simulated panel.
const http = require('http');
const { WebSocketServer, createWebSocketStream } = require('ws');
const mqtt = require('mqtt');
const { JSDOM } = require('jsdom');

const APP = process.argv[2];
const PORT = 18083;
const URL_WS = `ws://127.0.0.1:${PORT}/mqtt`;
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${ok ? '' : '  -> ' + detail}`);
  if (!ok) failures++;
}

(async () => {
  // Broker with username/password like EMQX
  const broker = require('aedes')();
  broker.authenticate = (client, user, pass, cb) => cb(null, user === 'phone' && pass && pass.toString() === 'secret');
  const server = http.createServer();
  new WebSocketServer({ server, path: '/mqtt' }).on('connection', (ws) => broker.handle(createWebSocketStream(ws)));
  await new Promise((r) => server.listen(PORT, r));

  const fromApp = [];
  broker.on('publish', (p, c) => {
    if (c && c.id.startsWith('acs550-app-')) fromApp.push({ topic: p.topic, payload: p.payload.toString(), retain: p.retain, qos: p.qos });
  });
  const lastFromApp = () => fromApp[fromApp.length - 1] || {};

  // Simulated panel: retained values as the firmware publishes them
  const panel = mqtt.connect(URL_WS, { username: 'phone', password: 'secret', clientId: 'panel-sim' });
  await new Promise((r) => panel.on('connect', r));
  const retained = {
    'Drive/status': 'online', 'Drive/state': 'RUNNING - AT SPEED', 'Drive/0102': '1487', 'Drive/0103': '49.6',
    'Drive/0104': '61.2', 'Drive/0105': '48.3', 'Drive/0106': '33.0', 'Drive/0107': '648', 'Drive/0109': '455',
    'Drive/0110': '41.5', 'Drive/0401': '0', 'Drive/0004': String(0x0337), 'Drive/0002': '1500',
    'Drive/1105': '1800', 'Drive/9904': '1', 'Drive/9907': '60.0', 'Drive/9908': '1780',
  };
  for (const [t, v] of Object.entries(retained)) panel.publish(t, v, { retain: true, qos: 1 });
  await wait(200);

  // Load the app
  const dom = await JSDOM.fromFile(APP, { runScripts: 'dangerously', resources: 'usable', pretendToBeVisual: true });
  await new Promise((r) => dom.window.addEventListener('load', r));
  const w = dom.window, d = w.document, $ = (s) => d.querySelector(s);
  const errors = [];
  w.addEventListener('error', (e) => errors.push(e.message));
  const setInput = (sel, v) => { $(sel).value = v; $(sel).dispatchEvent(new w.Event('input')); };

  check('no settings: opens Settings tab', $('#tab-settings').classList.contains('active'), d.querySelector('.tab.active').id);
  check('no settings: banner asks for broker', /Settings/.test($('#banner').textContent), $('#banner').textContent);

  // Wrong password first
  $('#sUrl').value = URL_WS; $('#sUser').value = 'phone'; $('#sPass').value = 'wrong';
  $('#btnSave').click();
  await wait(1500);
  check('bad password: not connected', !$('#pBroker .dot').classList.contains('ok'), $('#pBroker .dot').className);
  check('bad password: error in log', /Broker:/i.test($('#log').textContent), $('#log').textContent.slice(-200));

  // Correct login
  $('#tab-settings').classList.add('active');
  $('#sPass').value = 'secret';
  $('#btnSave').click();
  await wait(1500);
  check('connects to broker', $('#pBroker .dot').classList.contains('ok'), $('#pBroker .dot').className);
  check('switches to Drive tab', $('#tab-drive').classList.contains('active'), '');
  check('panel online dot', $('#pPanel .dot').classList.contains('ok'), $('#pPanel .dot').className);
  check('drive dot ok', $('#pDrive .dot').classList.contains('ok'), $('#pDrive .dot').className);
  check('speed shows 1487', $('#bigSpeed').textContent === '1487', $('#bigSpeed').textContent);
  check('run state + class', $('#runState').textContent === 'RUNNING - AT SPEED' && $('#runState').classList.contains('run'), $('#runState').className);
  check('quick freq 49.6', $('#q0103 .v').textContent === '49.6', $('#q0103 .v').textContent);
  check('temp tile 41.5', $('#v0110 .v').textContent === '41.5', $('#v0110 .v').textContent);
  check('gauge max = 1800 (1105)', $('#gMax').textContent === '1800', $('#gMax').textContent);
  check('setpoint shown 1500 FWD', $('#spNow').textContent === '1500 rpm FWD', $('#spNow').textContent);
  check('setpoint input 1500', $('#spNum').value === '1500', $('#spNum').value);
  check('slider max 1800', $('#spSlider').max === '1800', $('#spSlider').max);
  check('status bit 0 on, bit 3 (fault) off', $('#b0').classList.contains('on') && !$('#b3').classList.contains('on'), '');
  check('no banner when all is well', $('#banner').hidden, $('#banner').textContent);
  check('last fault text', $('#lastFault').textContent === '0 - None', $('#lastFault').textContent);

  // START needs confirmation
  const before = fromApp.length;
  $('#btnStart').click();
  await wait(100);
  check('START asks for confirmation', $('#modal').classList.contains('show') && fromApp.length === before, '');
  $('#mOk').click();
  await wait(300);
  check('START published', lastFromApp().topic === 'Drive/0001' && lastFromApp().payload === 'START', JSON.stringify(lastFromApp()));
  check('command not retained, QoS1', lastFromApp().retain === false && lastFromApp().qos === 1, JSON.stringify(lastFromApp()));
  check('own START echo does not break values', $('#bigSpeed').textContent === '1487', $('#bigSpeed').textContent);

  // STOP is immediate
  $('#btnStop').click();
  await wait(300);
  check('STOP published immediately', lastFromApp().payload === 'STOP' && !$('#modal').classList.contains('show'), JSON.stringify(lastFromApp()));

  // Setpoint: reverse while running needs confirmation, then sends -1200
  setInput('#spNum', '1200');
  $('#dirR').click();
  await wait(50);
  check('pending marker visible', !$('#spPending').hidden, '');
  $('#btnSetSpeed').click();
  await wait(100);
  check('reverse while running asks', $('#modal').classList.contains('show') && /REVERSE/.test($('#mText').textContent), $('#mText').textContent);
  $('#mOk').click();
  await wait(300);
  check('setpoint -1200 published', lastFromApp().topic === 'Drive/0002' && lastFromApp().payload === '-1200', JSON.stringify(lastFromApp()));
  panel.publish('Drive/0002', '-1200', { retain: true, qos: 1 });
  await wait(3300);
  check('panel confirmation shown', $('#spNow').textContent === '1200 rpm REV', $('#spNow').textContent);
  check('REV button highlighted', $('#dirR').classList.contains('on'), '');

  // Over-limit setpoint is clamped to 1105
  $('#dirF').click();
  setInput('#spNum', '5000');
  $('#btnSetSpeed').click();
  await wait(100);
  if ($('#modal').classList.contains('show')) $('#mOk').click();
  await wait(300);
  check('setpoint clamped to 1800', lastFromApp().payload === '1800', JSON.stringify(lastFromApp()));

  // Parameter read: "?" then the panel's answer
  $('#parNo').value = '2202';
  $('#btnParRead').click();
  await wait(300);
  check('param read publishes ?', lastFromApp().topic === 'Drive/2202' && lastFromApp().payload === '?', JSON.stringify(lastFromApp()));
  panel.publish('Drive/2202', '12.5', { retain: true, qos: 1 });
  await wait(300);
  check('param result shown', /2202 ACCELER TIME 1 = 12.5 s/.test($('#parOut').textContent), $('#parOut').textContent);
  check('param value filled', $('#parVal').value === '12.5', $('#parVal').value);

  // Parameter write needs confirmation
  $('#parVal').value = '15';
  $('#btnParWrite').click();
  await wait(100);
  $('#mOk').click();
  await wait(300);
  check('param write published', lastFromApp().topic === 'Drive/2202' && lastFromApp().payload === '15', JSON.stringify(lastFromApp()));

  // Panel log lines are colour-coded
  panel.publish('Drive/log', '14:02:11 E Drive FAULT: 1 OVERCURRENT');
  await wait(300);
  check('panel log line shown as error', d.querySelector('#log').lastChild.className === 'err', d.querySelector('#log').lastChild.className);

  // Panel goes offline (last will)
  panel.publish('Drive/status', 'offline', { retain: true, qos: 1 });
  await wait(300);
  check('offline banner', /offline/i.test($('#banner').textContent) && !$('#banner').hidden, $('#banner').textContent);
  check('values greyed when stale', d.body.classList.contains('stale'), d.body.className);
  panel.publish('Drive/status', 'online', { retain: true, qos: 1 });

  // Command suffix mode
  $('#sSuffix').value = '/set';
  $('#btnSave').click();
  await wait(1500);
  $('#btnStop').click();
  await wait(300);
  check('suffix mode: STOP to Drive/0001/set', lastFromApp().topic === 'Drive/0001/set' && lastFromApp().payload === 'STOP', JSON.stringify(lastFromApp()));
  check('suffix mode: values still read', $('#bigSpeed').textContent === '1487', $('#bigSpeed').textContent);

  check('no script errors', errors.length === 0, errors.join(' | '));

  console.log(failures ? `\n${failures} FAILED` : '\nALL PASSED');
  panel.end(true);
  w.close();
  server.close();
  broker.close();
  process.exit(failures ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(2); });
