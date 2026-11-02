#!/usr/bin/env python3
"""Local dev server: static files + /api/ proxy, with production headers.

Serves a build directory the way nginx does in production — same
Content-Security-Policy and security headers — so a CSP violation or a
missing MIME type shows up locally instead of after a deploy. Requests to
/api/ are forwarded to the leaderboard service.

    python3 server/leaderboard.py                      # terminal 1 (API on :8791)
    TORNADO_ORIGINS=http://localhost:8080 python3 server/leaderboard.py   # (allow the dev origin)
    python3 tools/dev_server.py build-wasm             # terminal 2 -> http://localhost:8080

Options: --port 8080  --api http://127.0.0.1:8791
"""
import argparse
import http.client
import http.server
import os
import sys
from functools import partial
from urllib.parse import urlsplit

# Keep in sync with server/nginx/security-headers-tornado.conf
CSP = ("default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self'; "
       "img-src 'self' data: blob:; font-src 'self'; connect-src 'self'; "
       "worker-src 'self'; manifest-src 'self'; object-src 'none'; base-uri 'none'; "
       "form-action 'none'; frame-ancestors 'none'")

HEADERS = {
    "Content-Security-Policy": CSP,
    "X-Content-Type-Options": "nosniff",
    "X-Frame-Options": "DENY",
    "Referrer-Policy": "strict-origin-when-cross-origin",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Resource-Policy": "same-origin",
    "Permissions-Policy": "camera=(), microphone=(), geolocation=(), payment=(), usb=()",
}

TYPES = {".wasm": "application/wasm", ".data": "application/octet-stream",
         ".webmanifest": "application/manifest+json", ".js": "text/javascript",
         ".svg": "image/svg+xml"}


class Handler(http.server.SimpleHTTPRequestHandler):
    api = "http://127.0.0.1:8791"
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, **TYPES}

    def end_headers(self):
        for k, v in HEADERS.items():
            self.send_header(k, v)
        if self.path.split("?")[0].endswith((".html", "/", "sw.js")):
            self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def _proxy(self):
        target = urlsplit(self.api)
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None
        headers = {k: v for k, v in self.headers.items()
                   if k.lower() in ("content-type", "content-length", "origin", "sec-fetch-site")}
        headers["X-Real-IP"] = self.client_address[0]
        try:
            conn = http.client.HTTPConnection(target.hostname, target.port or 80, timeout=10)
            conn.request(self.command, self.path, body=body, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
        except OSError as e:
            self.send_error(502, f"API unreachable: {e}")
            return
        self.send_response(resp.status)
        for k, v in resp.getheaders():
            if k.lower() in ("content-type", "cache-control"):
                self.send_header(k, v)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.startswith("/api/"):
            return self._proxy()
        return super().do_GET()

    def do_POST(self):
        if self.path.startswith("/api/"):
            return self._proxy()
        self.send_error(405)

    def log_message(self, fmt, *args):
        sys.stderr.write("  %s\n" % (fmt % args))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("directory", nargs="?", default="build-wasm")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", 8080)))
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--api", default="http://127.0.0.1:8791")
    a = ap.parse_args()
    Handler.api = a.api
    srv = http.server.ThreadingHTTPServer((a.bind, a.port), partial(Handler, directory=a.directory))
    print(f"Serving {a.directory} on http://localhost:{a.port}  (API -> {a.api})", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
