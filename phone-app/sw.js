// Service worker: lets the app open from the home screen without a network.
// Network first so updates show up straight away; cached copy when offline.
const CACHE = 'acs550-remote-v2';
const SHELL = [
  './',
  'index.html',
  'app.css',
  'app.js',
  'vendor/mqtt.min.js',
  'manifest.webmanifest',
  'fonts/LibreFranklin-latin.woff2',
  'brand/powermate-logo-white.png',
  'brand/powermate-mark.png',
  'icons/icon-192.png',
  'icons/icon-512.png',
  'icons/apple-touch-icon.png',
];

self.addEventListener('install', (event) => {
  event.waitUntil(caches.open(CACHE).then((cache) => cache.addAll(SHELL)));
  self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
  );
  self.clients.claim();
});

self.addEventListener('fetch', (event) => {
  const req = event.request;
  if (req.method !== 'GET' || new URL(req.url).origin !== self.location.origin) return;
  event.respondWith(
    fetch(req)
      .then((res) => {
        const copy = res.clone();
        caches.open(CACHE).then((cache) => cache.put(req, copy));
        return res;
      })
      .catch(() => caches.match(req).then((hit) => hit || caches.match('index.html')))
  );
});
