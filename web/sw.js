/* Tornada 3D service worker: makes the game playable offline after one visit.
 *
 * VERSION and ASSETS are filled in by deploy_vps.sh (content-hashed file
 * names), so every deploy gets a fresh cache and old ones are dropped. In a
 * dev build app.js never registers this worker.
 *
 *   navigations   network first, cached page as offline fallback
 *   /api/         never touched (scores must be live)
 *   static files  cache first (hashed names never change content)
 */
'use strict';

var VERSION = 'dev';
var ASSETS = [];
var CACHE = 'tornado-' + VERSION;
var PAGE = new URL('./', self.registration.scope).href;

self.addEventListener('install', function (event) {
    event.waitUntil(
        caches.open(CACHE)
            .then(function (cache) { return cache.addAll([PAGE].concat(ASSETS)); })
            .then(function () { return self.skipWaiting(); })
    );
});

self.addEventListener('activate', function (event) {
    event.waitUntil(
        caches.keys()
            .then(function (keys) {
                return Promise.all(keys.filter(function (k) {
                    return k.indexOf('tornado-') === 0 && k !== CACHE;
                }).map(function (k) { return caches.delete(k); }));
            })
            .then(function () { return self.clients.claim(); })
    );
});

function cachePut(request, response) {
    var copy = response.clone();
    caches.open(CACHE).then(function (cache) { return cache.put(request, copy); }).catch(function () {});
}

self.addEventListener('fetch', function (event) {
    var req = event.request;
    if (req.method !== 'GET') return;
    var url = new URL(req.url);
    if (url.origin !== self.location.origin) return;
    if (url.pathname.indexOf('/api/') === 0 || url.pathname.indexOf('/cdn-cgi/') === 0) return;

    if (req.mode === 'navigate') {
        event.respondWith(
            fetch(req).then(function (res) {
                if (res.ok && url.pathname === '/') cachePut(PAGE, res);
                return res;
            }).catch(function () {
                return caches.match(PAGE).then(function (hit) {
                    return hit || new Response('Offline', { status: 503, headers: { 'Content-Type': 'text/plain; charset=utf-8' } });
                });
            })
        );
        return;
    }

    event.respondWith(
        caches.match(req).then(function (hit) {
            return hit || fetch(req).then(function (res) {
                if (res.ok && res.type === 'basic') cachePut(req, res);
                return res;
            });
        })
    );
});
