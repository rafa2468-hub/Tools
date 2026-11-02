// Lift Log service worker: serves the app from the cache first (so it opens instantly
// with no signal at the gym) and refreshes the cache in the background, so an update
// shows up the next time the app is opened. Bump CACHE with every release.
const CACHE = 'liftlog-v5';
const ASSETS = ['./', './index.html', './manifest.webmanifest', './icon-180.png', './icon-192.png', './icon-512.png'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(ASSETS)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', e => {
  e.waitUntil(caches.keys()
    .then(keys => Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k))))
    .then(() => self.clients.claim()));
});

self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.method !== 'GET' || new URL(req.url).origin !== self.location.origin) return;
  const refresh = caches.open(CACHE).then(c => fetch(req).then(res => {
    if (res && res.ok) c.put(req, res.clone());
    return res;
  })).catch(() => null);
  e.waitUntil(refresh.then(() => undefined));
  e.respondWith(caches.match(req, { ignoreSearch: true }).then(hit => hit || refresh.then(res =>
    res || (req.mode === 'navigate' ? caches.match('./index.html') : Response.error()))));
});
