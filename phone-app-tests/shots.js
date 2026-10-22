// Phone-size screenshots of phone-app/ with a simulated panel on a local broker.
const http = require('http');
const path = require('path');
const { WebSocketServer, createWebSocketStream } = require('ws');
const mqtt = require('mqtt');
const puppeteer = require('puppeteer-core');

const APP = process.argv[2];
const OUT = process.argv[3];
const EDGE = 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const PORT = 18084;
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  require('fs').mkdirSync(OUT, { recursive: true });
  const broker = require('aedes')();
  const server = http.createServer();
  new WebSocketServer({ server, path: '/mqtt' }).on('connection', (ws) => broker.handle(createWebSocketStream(ws)));
  await new Promise((r) => server.listen(PORT, r));

  const panel = mqtt.connect(`ws://127.0.0.1:${PORT}/mqtt`, { clientId: 'panel-sim' });
  await new Promise((r) => panel.on('connect', r));
  const retained = {
    'Drive/status': 'online', 'Drive/state': 'RUNNING - AT SPEED', 'Drive/0102': '1487', 'Drive/0103': '49.6',
    'Drive/0104': '61.2', 'Drive/0105': '48.3', 'Drive/0106': '33.0', 'Drive/0107': '648', 'Drive/0109': '455',
    'Drive/0110': '41.5', 'Drive/0401': '28', 'Drive/0004': String(0x0337), 'Drive/0002': '1500',
    'Drive/1105': '1800', 'Drive/9904': '1', 'Drive/9907': '60.0', 'Drive/9908': '1780',
  };
  for (const [t, v] of Object.entries(retained)) panel.publish(t, v, { retain: true, qos: 1 });
  await wait(200);

  const browser = await puppeteer.launch({ executablePath: EDGE, headless: true, args: ['--allow-file-access-from-files'] });
  const page = await browser.newPage();
  await page.setViewport({ width: 390, height: 844, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
  const url = require('url').pathToFileURL(path.resolve(APP)).href;
  await page.goto(url);
  await page.evaluate((port) => {
    localStorage.setItem('acs550-remote', JSON.stringify({ url: `ws://127.0.0.1:${port}/mqtt`, user: '', pass: '', base: 'Drive', suffix: '', remember: true }));
  }, PORT);
  await page.reload();
  await wait(1800);
  ['Drive/log'].forEach(() => {
    panel.publish('Drive/log', '14:02:11 T Setpoint 1500 rpm FWD (MQTT)');
  });
  await wait(400);

  const shot = async (name) => page.screenshot({ path: path.join(OUT, name), fullPage: true });
  await page.screenshot({ path: path.join(OUT, 'drive-first-screen.png') });
  await shot('drive.png');
  await page.click('.tabbar button[data-tab="values"]');
  await wait(200);
  await shot('values.png');
  await page.click('.tabbar button[data-tab="settings"]');
  await wait(200);
  await shot('settings.png');
  await page.click('.tabbar button[data-tab="drive"]');
  await page.click('#btnStart');
  await wait(200);
  await page.screenshot({ path: path.join(OUT, 'confirm.png') });

  const fontOk = await page.evaluate(() => document.fonts.check('900 16px "Libre Franklin"'));
  console.log('Libre Franklin loaded:', fontOk);
  await browser.close();
  panel.end(true);
  server.close();
  broker.close();
  process.exit(0);
})().catch((e) => { console.error(e); process.exit(1); });
