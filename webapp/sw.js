// Minimal service worker: enables PWA install on iOS/Android.
// We intentionally do NOT cache anything — webkbm is useless without the host,
// so falling back to a cached shell would be misleading.
self.addEventListener('install', e => self.skipWaiting());
self.addEventListener('activate', e => self.clients.claim());
self.addEventListener('fetch', () => {});
