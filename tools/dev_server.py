#!/usr/bin/env python3
"""Local dev server: static files + /api/ proxy, with production headers.

Serves a build directory the way nginx does in production: the security
headers (CSP included) are read from server/nginx/security-headers-tornado.conf,
missing paths get 404.html with a 404 status, and /api/ is forwarded to the
leaderboard service. A CSP violation or a wrong MIME type therefore shows
up locally instead of after a deploy.

    TORNADO_ORIGINS=http://localhost:8080 python3 server/leaderboard.py   # terminal 1 (API on :8791)
    python3 tools/dev_server.py build-wasm     # terminal 2 -> http://localhost:8080
    python3 tools/dev_server.py dist           # the packaged site (service worker active)

Options: --port 8080  --api http://127.0.0.1:8791
"""
import argparse
import http.client
import http.server
import os
import re
import secrets
import sys
from functools import partial
from urllib.parse import urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
SNIPPET = os.path.join(HERE, "..", "server", "nginx", "security-headers-tornado.conf")


def load_headers(path=SNIPPET):
    """The production security headers, read from the nginx snippet itself.

    Only what cannot work on plain-HTTP localhost is dropped (HSTS and
    upgrade-insecure-requests); $request_id becomes a per-request nonce.
    """
    headers = {}
    with open(path, encoding="utf-8") as f:
        for name, value in re.findall(r'^add_header\s+(\S+)\s+"([^"]*)"', f.read(), re.M):
            if name.lower() == "strict-transport-security":
                continue
            if name.lower() == "content-security-policy":
                value = re.sub(r";\s*upgrade-insecure-requests", "", value)
            headers[name] = value
    if "Content-Security-Policy" not in headers:
        raise SystemExit(f"no CSP found in {path}")
    return headers


HEADERS = load_headers()

TYPES = {".wasm": "application/wasm", ".data": "application/octet-stream",
         ".webmanifest": "application/manifest+json", ".js": "text/javascript",
         ".svg": "image/svg+xml", ".ico": "image/x-icon", ".xml": "application/xml",
         ".webp": "image/webp"}


class Handler(http.server.SimpleHTTPRequestHandler):
    api = "http://127.0.0.1:8791"
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, **TYPES}

    def end_headers(self):
        nonce = secrets.token_hex(16)
        for k, v in HEADERS.items():
            self.send_header(k, v.replace("$request_id", nonce))
        if self.path.split("?")[0].endswith((".html", "/", "sw.js", ".webmanifest")):
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

    def send_error(self, code, message=None, explain=None):
        # Like nginx's error_page 404 /404.html: real 404 status, friendly page.
        page = os.path.join(self.directory, "404.html")
        if code == 404 and self.command in ("GET", "HEAD") and os.path.isfile(page):
            with open(page, "rb") as f:
                body = f.read()
            self.send_response(404)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command == "GET":
                self.wfile.write(body)
            return
        super().send_error(code, message, explain)

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
