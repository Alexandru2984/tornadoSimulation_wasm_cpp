#!/usr/bin/env python3
"""Tiny leaderboard API for Tornado 3D.

Endpoints (proxied by nginx under /api/):
  GET  /api/scores   -> top 10 scores as JSON
  POST /api/scores   -> submit {"name": str, "score": int, "wave": int}

Stores everything in SQLite next to this file. Submissions are
rate-limited to one per IP every 20 seconds.
"""
import json
import re
import sqlite3
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

DB_PATH = Path(__file__).parent / "scores.db"
NAME_RE = re.compile(r"^[A-Za-z0-9 _.-]{1,16}$")
MAX_BODY = 512
RATE_SECONDS = 20
_last_post = {}  # ip -> unix timestamp of last accepted POST


def db():
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        """CREATE TABLE IF NOT EXISTS scores (
               id      INTEGER PRIMARY KEY AUTOINCREMENT,
               name    TEXT    NOT NULL,
               score   INTEGER NOT NULL,
               wave    INTEGER NOT NULL,
               created TEXT    NOT NULL DEFAULT (datetime('now'))
           )"""
    )
    return conn


class Handler(BaseHTTPRequestHandler):
    server_version = "TornadoLB/1.0"

    def _send(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _client_ip(self):
        fwd = self.headers.get("X-Forwarded-For", "")
        return (fwd.split(",")[0].strip() or self.client_address[0])

    def do_GET(self):
        if self.path.rstrip("/") != "/api/scores":
            return self._send(404, {"error": "not found"})
        with db() as conn:
            rows = conn.execute(
                "SELECT name, score, wave, created FROM scores"
                " ORDER BY score DESC, id ASC LIMIT 10"
            ).fetchall()
        self._send(200, [
            {"name": n, "score": s, "wave": w, "date": c[:10]}
            for n, s, w, c in rows
        ])

    def do_POST(self):
        if self.path.rstrip("/") != "/api/scores":
            return self._send(404, {"error": "not found"})
        ip = self._client_ip()
        now = time.time()
        if now - _last_post.get(ip, 0) < RATE_SECONDS:
            return self._send(429, {"error": "too many requests"})
        try:
            length = min(int(self.headers.get("Content-Length", 0)), MAX_BODY)
            data = json.loads(self.rfile.read(length) or b"{}")
            name = str(data.get("name", "")).strip() or "ANONIM"
            score = int(data.get("score"))
            wave = int(data.get("wave"))
        except (ValueError, TypeError, json.JSONDecodeError):
            return self._send(400, {"error": "bad request"})
        if (not NAME_RE.match(name)
                or not 0 < score <= 1_000_000
                or not 1 <= wave <= 999):
            return self._send(400, {"error": "invalid data"})
        _last_post[ip] = now
        if len(_last_post) > 10_000:  # keep the rate-limit map bounded
            _last_post.clear()
        with db() as conn:
            conn.execute(
                "INSERT INTO scores (name, score, wave) VALUES (?, ?, ?)",
                (name, score, wave),
            )
        self._send(200, {"ok": True})

    def log_message(self, fmt, *args):
        pass  # keep journald quiet; nginx already logs access


if __name__ == "__main__":
    print("Tornado leaderboard listening on 127.0.0.1:8791", flush=True)
    ThreadingHTTPServer(("127.0.0.1", 8791), Handler).serve_forever()
