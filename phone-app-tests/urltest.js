// Checks brokerUrl() from phone-app/app.js against the inputs people paste.
const fs = require('fs');
const src = fs.readFileSync(process.argv[2], 'utf8');
const fn = src.match(/function brokerUrl\(input\) \{[\s\S]*?\n\}/)[0];
const brokerUrl = new Function(fn + '; return brokerUrl;')();

const cases = [
  ['abc123.ala.us-east-1.emqxsl.com', 'wss://abc123.ala.us-east-1.emqxsl.com:8084/mqtt'],
  ['  abc123.ala.us-east-1.emqxsl.com  ', 'wss://abc123.ala.us-east-1.emqxsl.com:8084/mqtt'],
  ['mqtts://abc123.emqxsl.com:8883', 'wss://abc123.emqxsl.com:8084/mqtt'],
  ['mqtt://abc123.emqxsl.com:1883', 'wss://abc123.emqxsl.com:8084/mqtt'],
  ['https://abc123.emqxsl.com', 'wss://abc123.emqxsl.com:8084/mqtt'],
  ['wss://abc123.emqxsl.com:8084/mqtt', 'wss://abc123.emqxsl.com:8084/mqtt'],
  ['WSS://abc123.emqxsl.com:8084/mqtt', 'wss://abc123.emqxsl.com:8084/mqtt'],
  ['wss://abc123.emqxsl.com', 'wss://abc123.emqxsl.com:8084/mqtt'],
  ['ws://192.168.1.10:8083/mqtt', 'ws://192.168.1.10:8083/mqtt'],
  ['ws://192.168.1.10', 'ws://192.168.1.10:8083/mqtt'],
  ['abc123.emqxsl.com:8084', 'wss://abc123.emqxsl.com:8084/mqtt'],
  ['abc123.emqxsl.com:8443', 'wss://abc123.emqxsl.com:8443/mqtt'],
  ['abc123.emqxsl.com:8443/mqtt', 'wss://abc123.emqxsl.com:8443/mqtt'],
  ['abc123.emqxsl.com:8883', 'wss://abc123.emqxsl.com:8084/mqtt'],
  ['abc123.emqxsl.com:1883', 'wss://abc123.emqxsl.com:8084/mqtt'],
  ['', null],
  ['has space.com', null],
];
let bad = 0;
for (const [input, want] of cases) {
  const got = brokerUrl(input);
  const ok = got === want;
  if (!ok) bad++;
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${JSON.stringify(input)} -> ${got}${ok ? '' : '  (want ' + want + ')'}`);
}
process.exit(bad ? 1 : 0);
