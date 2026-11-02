#!/usr/bin/env python3
"""Leaderboard API for Tornado 3D (stdlib only: http.server + sqlite3).

nginx proxies /api/ to this service, which listens on loopback only.

API v2
------
  GET  /api/v2/health
  POST /api/v2/runs     {"mode": "classic"|"timeattack", "difficulty": 0..2}
                        -> {"run": "<id>", "mode", "difficulty"}
  GET  /api/v2/scores?mode=classic&difficulty=1&period=all|month|week|day&limit=10
                        -> {"mode", "difficulty", "period", "scores": [...]}
  POST /api/v2/scores   {"run", "name", "score", "wave", "player",
                         "stats": {"houses", "trees", "props", "animals",
                                   "vehicles", "birds", "maxCombo", "time"}}
                        -> {"ok": true, "rank": n}
  GET  /api/v2/stats    -> global destruction counters

Legacy (pre-v2 pages still cached in browsers)
  GET  /api/scores      -> classic/normal top 10 in the old array format
  POST /api/scores      -> 410 Gone

Why runs?
  The game runs entirely in the browser, so a score is ultimately a claim
  made by the client. v1 accepted any {"score": 1000000} from curl. v2 makes
  a forged score cost real effort: the server hands out a single-use run
  ID when a run starts and remembers *when* it started; a submission must
  quote that ID, may be used once, and must be consistent with the elapsed
  wall-clock time, the per-category destruction counts and the game's own
  scoring rules (see check_submission). A determined attacker who rewrites
  the client can still lie consistently and wait it out, but blind
  injection, replay and "score 999999 at wave 1" are gone.

Privacy: IP addresses are used only in memory for rate limiting; nothing
that identifies a person is written to disk. Player keys are stored hashed.
"""
import hashlib
import ipaddress
import json
import os
import re
import secrets
import sqlite3
import sys
import threading
import time
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

# ── Configuration ────────────────────────────────────────────────────
DB_PATH = Path(os.environ.get("TORNADO_DB", str(Path(__file__).parent / "scores.db")))
HOST = os.environ.get("TORNADO_HOST", "127.0.0.1")
PORT = int(os.environ.get("TORNADO_PORT", "8791"))
# Origins allowed to call the write endpoints from a browser.
ALLOWED_ORIGINS = frozenset(
    o.strip() for o in os.environ.get(
        "TORNADO_ORIGINS", "https://tornado.micutu.com").split(",") if o.strip())

CLOCK = time.time            # patched by the tests
MAX_BODY = 2048
RUN_TTL = 6 * 3600           # a run must be submitted within 6 hours
MAX_RUNS = 200_000           # hard cap on the runs table
SCORE_RETENTION_DAYS = 400   # non-top scores older than this are pruned
KEEP_TOP_PER_BOARD = 1000

MODES = ("classic", "timeattack")
PERIODS = {"all": None, "month": 30 * 86400, "week": 7 * 86400, "day": 86400}
NAME_RE = re.compile(r"^[A-Za-z0-9 _.-]{1,16}$")
PLAYER_RE = re.compile(r"^[A-Za-z0-9_-]{22,64}$")
ANON = "ANONIM"

# ── Game rules mirrored from src/main.cpp + src/constants.h ──────────
# Keep in sync with the C++ code (test_leaderboard.py pins these values).
PTS_HOUSE, PTS_TREE, PTS_VEHICLE, PTS_BIRD = 100, 30, 90, 60
PTS_PROP_MIN, PTS_PROP_MAX = 15, 50          # pole .. parked car
PTS_ANIMAL_MIN, PTS_ANIMAL_MAX = 50, 75      # sheep .. cow
MAX_COMBO_MULT = 5.0                         # 1 + min(combo/3, 4)
MAX_SCORE_MULT = 2.0                         # SCORE x2 power-up
WAVE_BASE_TARGET, WAVE_TARGET_STEP = 5, 3
TIME_ATTACK_SECONDS = 180
MAX_KILLS_PER_SEC = 60                       # generous upper bound
KILL_SLACK = 200
TIME_SLACK = 10                              # network latency at run start
MAX_SCORE = 50_000_000
MAX_WAVE = 999
STAT_KEYS = ("houses", "trees", "props", "animals", "vehicles", "birds")

# ── Name filter ──────────────────────────────────────────────────────
# Long words are matched as substrings of the squashed name ("f.u.c.k");
# short ones only as whole tokens, so "Mercury", "Populate" or "Future"
# are no longer rejected for containing "cur", "pula" or "futu".
BLOCK_SUBSTR = (
    "fuck", "shit", "cunt", "nigger", "nigga", "faggot", "retard", "bitch",
    "pussy", "whore", "hitler", "pizda", "coaie", "dickhead", "cocksuck",
)
BLOCK_TOKEN = (
    "dick", "cock", "rape", "nazi", "pula", "muie", "cur", "futu", "fut",
    "sex", "porn", "admin", "moderator",
)
LEET = str.maketrans("013457@$", "oieastas")


def name_is_allowed(name):
    if not NAME_RE.match(name) or not any(c.isalnum() for c in name):
        return False
    low = name.lower().translate(LEET)
    squashed = re.sub(r"[^a-z0-9]", "", low)
    if any(bad in squashed for bad in BLOCK_SUBSTR):
        return False
    tokens = set(re.split(r"[^a-z0-9]+", low)) | {squashed}
    return not any(bad in tokens for bad in BLOCK_TOKEN)


def kills_to_reach_wave(wave):
    """Destroys needed to *reach* `wave` (waves 1..wave-1 completed)."""
    n = max(0, wave - 1)
    return WAVE_BASE_TARGET * n + WAVE_TARGET_STEP * n * (n - 1) // 2


def check_submission(run, data, now):
    """Validate a submission against its run. Returns (row, None) or (None, error)."""
    try:
        score = int(data["score"])
        wave = int(data["wave"])
        stats = data.get("stats") or {}
        if not isinstance(stats, dict):
            return None, "bad stats"
        counts = {k: int(stats.get(k, 0)) for k in STAT_KEYS}
        max_combo = int(stats.get("maxCombo", 0))
        run_time = float(stats.get("time", 0))
    except (KeyError, TypeError, ValueError, OverflowError):
        return None, "bad request"

    if not 0 < score <= MAX_SCORE or not 1 <= wave <= MAX_WAVE:
        return None, "out of range"
    if any(v < 0 or v > 1_000_000 for v in counts.values()):
        return None, "out of range"
    if not (0 <= max_combo <= 1_000_000) or not (0.0 <= run_time <= RUN_TTL):
        return None, "out of range"

    kills = sum(counts.values())
    elapsed = now - run["started"]
    if max_combo > kills:
        return None, "inconsistent combo"
    if run_time > elapsed + TIME_SLACK:
        return None, "inconsistent time"
    if kills > MAX_KILLS_PER_SEC * elapsed + KILL_SLACK:
        return None, "implausible rate"

    base_max = (PTS_HOUSE * counts["houses"] + PTS_TREE * counts["trees"]
                + PTS_PROP_MAX * counts["props"] + PTS_ANIMAL_MAX * counts["animals"]
                + PTS_VEHICLE * counts["vehicles"] + PTS_BIRD * counts["birds"])
    base_min = (PTS_HOUSE * counts["houses"] + PTS_TREE * counts["trees"]
                + PTS_PROP_MIN * counts["props"] + PTS_ANIMAL_MIN * counts["animals"]
                + PTS_VEHICLE * counts["vehicles"] + PTS_BIRD * counts["birds"])
    if score > base_max * MAX_COMBO_MULT * MAX_SCORE_MULT:
        return None, "implausible score"
    if score < base_min - kills:          # int() truncation per scoring batch
        return None, "implausible score"

    if run["mode"] == "timeattack":
        if wave != 1:
            return None, "inconsistent wave"
        if elapsed < TIME_ATTACK_SECONDS - TIME_SLACK:
            return None, "too early"
    elif kills < kills_to_reach_wave(wave):
        return None, "inconsistent wave"

    row = dict(score=score, wave=wave, max_combo=max_combo,
               run_time=int(min(run_time, elapsed)), **counts)
    return row, None


# ── Rate limiting (token bucket per client, bounded LRU) ─────────────
class RateLimiter:
    def __init__(self, rate, burst, max_keys=50_000):
        self.rate, self.burst, self.max_keys = rate, burst, max_keys
        self._buckets = OrderedDict()
        self._lock = threading.Lock()

    def allow(self, key):
        now = time.monotonic()
        with self._lock:
            tokens, last = self._buckets.pop(key, (self.burst, now))
            tokens = min(self.burst, tokens + (now - last) * self.rate)
            ok = tokens >= 1.0
            if ok:
                tokens -= 1.0
            self._buckets[key] = (tokens, now)
            # Evicting the least recently seen key can only ever *refill* a
            # bucket, never lock an honest client out.
            while len(self._buckets) > self.max_keys:
                self._buckets.popitem(last=False)
            return ok


LIMIT_RUNS = RateLimiter(rate=1 / 3, burst=10)      # one run per 3 s
LIMIT_SCORES = RateLimiter(rate=1 / 15, burst=4)    # one score per 15 s
LIMIT_READS = RateLimiter(rate=5, burst=30)


def client_key(handler):
    """Rate-limit key: the real client IP, IPv6 grouped by /64."""
    peer = handler.client_address[0]
    ip = peer
    # Only trust proxy headers when the request came from loopback (nginx).
    # X-Real-IP is set by nginx; otherwise the LAST X-Forwarded-For hop is the
    # one nginx appended — earlier entries are whatever the client claimed.
    if peer in ("127.0.0.1", "::1"):
        fwd = handler.headers.get("X-Forwarded-For", "").split(",")[-1].strip()
        ip = handler.headers.get("X-Real-IP", "").strip() or fwd or peer
    try:
        addr = ipaddress.ip_address(ip)
    except ValueError:
        return peer
    if addr.version == 6:
        return str(ipaddress.ip_network(f"{addr}/64", strict=False))
    return str(addr)


# ── Database ─────────────────────────────────────────────────────────
SCORE_COLUMNS = {
    "mode": "TEXT NOT NULL DEFAULT 'classic'",
    "difficulty": "INTEGER NOT NULL DEFAULT 1",
    "houses": "INTEGER NOT NULL DEFAULT 0",
    "trees": "INTEGER NOT NULL DEFAULT 0",
    "props": "INTEGER NOT NULL DEFAULT 0",
    "animals": "INTEGER NOT NULL DEFAULT 0",
    "vehicles": "INTEGER NOT NULL DEFAULT 0",
    "birds": "INTEGER NOT NULL DEFAULT 0",
    "max_combo": "INTEGER NOT NULL DEFAULT 0",
    "run_time": "INTEGER NOT NULL DEFAULT 0",
    "created_ts": "INTEGER NOT NULL DEFAULT 0",
}


def connect():
    conn = sqlite3.connect(DB_PATH, timeout=5, isolation_level=None)
    conn.execute("PRAGMA busy_timeout = 5000")
    conn.execute("PRAGMA foreign_keys = ON")
    return conn


def init_db():
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = connect()
    try:
        conn.execute("PRAGMA journal_mode = WAL")
        conn.execute(
            """CREATE TABLE IF NOT EXISTS scores (
                   id      INTEGER PRIMARY KEY AUTOINCREMENT,
                   name    TEXT    NOT NULL,
                   score   INTEGER NOT NULL,
                   wave    INTEGER NOT NULL,
                   created TEXT    NOT NULL DEFAULT (datetime('now'))
               )""")
        have = {r[1] for r in conn.execute("PRAGMA table_info(scores)")}
        for col, decl in SCORE_COLUMNS.items():
            if col not in have:   # in-place migration of the v1 table
                conn.execute(f"ALTER TABLE scores ADD COLUMN {col} {decl}")
        conn.execute("UPDATE scores SET created_ts = CAST(strftime('%s', created) AS INTEGER)"
                     " WHERE created_ts = 0")
        conn.execute("CREATE INDEX IF NOT EXISTS scores_board"
                     " ON scores (mode, difficulty, score DESC)")
        conn.execute("CREATE INDEX IF NOT EXISTS scores_created ON scores (created_ts)")
        conn.execute(
            """CREATE TABLE IF NOT EXISTS runs (
                   id         TEXT    PRIMARY KEY,
                   mode       TEXT    NOT NULL,
                   difficulty INTEGER NOT NULL,
                   started    REAL    NOT NULL,
                   used       INTEGER NOT NULL DEFAULT 0
               ) WITHOUT ROWID""")
        conn.execute("CREATE INDEX IF NOT EXISTS runs_started ON runs (started)")
        conn.execute(
            """CREATE TABLE IF NOT EXISTS players (
                   name_key  TEXT PRIMARY KEY,
                   key_hash  TEXT NOT NULL,
                   created   INTEGER NOT NULL
               ) WITHOUT ROWID""")
    finally:
        conn.close()


def maintenance(now=None):
    """Drop expired runs and prune scores that can never be shown again."""
    now = CLOCK() if now is None else now
    conn = connect()
    try:
        conn.execute("DELETE FROM runs WHERE started < ?", (now - RUN_TTL,))
        excess = conn.execute("SELECT COUNT(*) FROM runs").fetchone()[0] - MAX_RUNS
        if excess > 0:
            conn.execute("DELETE FROM runs WHERE id IN"
                         " (SELECT id FROM runs ORDER BY started LIMIT ?)", (excess,))
        conn.execute(
            """DELETE FROM scores WHERE created_ts < ? AND id NOT IN (
                   SELECT id FROM (
                       SELECT id, ROW_NUMBER() OVER (
                           PARTITION BY mode, difficulty ORDER BY score DESC, id) AS rn
                       FROM scores) WHERE rn <= ?)""",
            (int(now) - SCORE_RETENTION_DAYS * 86400, KEEP_TOP_PER_BOARD))
    finally:
        conn.close()


def maintenance_loop():
    while True:
        time.sleep(600)
        try:
            maintenance()
        except sqlite3.Error as exc:
            print(f"maintenance failed: {exc}", file=sys.stderr, flush=True)


def board_query(mode, difficulty, period, limit, now):
    since = 0 if PERIODS[period] is None else int(now) - PERIODS[period]
    conn = connect()
    try:
        # Best entry per player name; anonymous runs are not merged together.
        rows = conn.execute(
            """SELECT name, score, wave, created_ts, max_combo FROM (
                   SELECT name, score, wave, created_ts, max_combo,
                          ROW_NUMBER() OVER (
                              PARTITION BY CASE WHEN name = ? THEN 'anon:' || id
                                                ELSE lower(name) END
                              ORDER BY score DESC, id) AS rn,
                          id
                   FROM scores
                   WHERE mode = ? AND difficulty = ? AND created_ts >= ?)
               WHERE rn = 1 ORDER BY score DESC, id LIMIT ?""",
            (ANON, mode, difficulty, since, limit)).fetchall()
    finally:
        conn.close()
    return [{"rank": i + 1, "name": n, "score": s, "wave": w,
             "date": time.strftime("%Y-%m-%d", time.gmtime(c)), "maxCombo": mc}
            for i, (n, s, w, c, mc) in enumerate(rows)]


_stats_cache = {"at": 0.0, "data": None}
_stats_lock = threading.Lock()


def global_stats(now):
    with _stats_lock:
        if _stats_cache["data"] is not None and now - _stats_cache["at"] < 60:
            return _stats_cache["data"]
        conn = connect()
        try:
            r = conn.execute(
                "SELECT COUNT(*), COALESCE(SUM(houses),0), COALESCE(SUM(trees),0),"
                " COALESCE(SUM(props),0), COALESCE(SUM(animals),0),"
                " COALESCE(SUM(vehicles),0), COALESCE(SUM(birds),0),"
                " COALESCE(MAX(score),0) FROM scores").fetchone()
        finally:
            conn.close()
        data = {"scores": r[0], "houses": r[1], "trees": r[2], "props": r[3],
                "animals": r[4], "vehicles": r[5], "birds": r[6], "best": r[7]}
        _stats_cache.update(at=now, data=data)
        return data


# ── HTTP layer ───────────────────────────────────────────────────────
class ApiError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code, self.message = code, message


class Handler(BaseHTTPRequestHandler):
    server_version = "TornadoLB"
    sys_version = ""
    # HTTP/1.0: one request per connection, so an error path that never reads
    # the body cannot leave bytes behind to be parsed as a second request.
    protocol_version = "HTTP/1.0"
    timeout = 10              # socket timeout: no slow-loris threads

    # -- helpers --
    def _send(self, code, payload, cache="no-store"):
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", cache)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _read_json(self):
        ctype = self.headers.get("Content-Type", "").split(";")[0].strip().lower()
        if ctype != "application/json":
            raise ApiError(415, "expected application/json")
        raw_len = self.headers.get("Content-Length")
        if raw_len is None or not raw_len.isdigit():
            raise ApiError(411, "length required")
        length = int(raw_len)
        if length > MAX_BODY:
            raise ApiError(413, "body too large")
        try:
            data = json.loads(self.rfile.read(length) or b"{}")
        except (json.JSONDecodeError, UnicodeDecodeError):
            raise ApiError(400, "bad json") from None
        if not isinstance(data, dict):
            raise ApiError(400, "bad json")
        return data

    def _check_origin(self):
        """Browsers send Origin on cross-origin POSTs; refuse foreign pages."""
        origin = self.headers.get("Origin")
        if origin is not None and origin not in ALLOWED_ORIGINS:
            raise ApiError(403, "forbidden origin")
        if self.headers.get("Sec-Fetch-Site", "same-origin") not in (
                "same-origin", "none"):
            raise ApiError(403, "cross-site request")

    def _route(self):
        return urlsplit(self.path).path.rstrip("/") or "/"

    def _dispatch(self, table):
        try:
            fn = table.get(self._route())
            if fn is None:
                raise ApiError(404, "not found")
            fn()
        except ApiError as e:
            self._send(e.code, {"error": e.message})
        except sqlite3.Error as e:
            print(f"db error: {e}", file=sys.stderr, flush=True)
            self._send(503, {"error": "storage busy"})
        except Exception as e:  # never leak a traceback to the client
            print(f"unhandled: {type(e).__name__}: {e}", file=sys.stderr, flush=True)
            self._send(500, {"error": "internal error"})

    # -- verbs --
    def do_GET(self):
        self._dispatch({
            "/api/v2/health": self.get_health,
            "/api/v2/scores": self.get_scores,
            "/api/v2/stats": self.get_stats,
            "/api/scores": self.get_scores_legacy,
        })

    do_HEAD = do_GET

    def do_POST(self):
        self._dispatch({
            "/api/v2/runs": self.post_run,
            "/api/v2/scores": self.post_score,
            "/api/scores": self.post_score_legacy,
        })

    def do_PUT(self):
        self._send(405, {"error": "method not allowed"})

    do_DELETE = do_PATCH = do_OPTIONS = do_PUT

    # -- endpoints --
    def get_health(self):
        self._send(200, {"ok": True})

    def _read_limit(self):
        if not LIMIT_READS.allow(client_key(self)):
            raise ApiError(429, "too many requests")

    def get_scores(self):
        self._read_limit()
        q = parse_qs(urlsplit(self.path).query)
        mode = q.get("mode", ["classic"])[0]
        period = q.get("period", ["all"])[0]
        try:
            difficulty = int(q.get("difficulty", ["1"])[0])
            limit = int(q.get("limit", ["10"])[0])
        except ValueError:
            raise ApiError(400, "bad query") from None
        if mode not in MODES or period not in PERIODS or difficulty not in (0, 1, 2):
            raise ApiError(400, "bad query")
        limit = max(1, min(limit, 50))
        scores = board_query(mode, difficulty, period, limit, CLOCK())
        self._send(200, {"mode": mode, "difficulty": difficulty, "period": period,
                         "scores": scores}, cache="public, max-age=10")

    def get_scores_legacy(self):
        self._read_limit()
        rows = board_query("classic", 1, "all", 10, CLOCK())
        self._send(200, [{k: r[k] for k in ("name", "score", "wave", "date")}
                         for r in rows], cache="public, max-age=10")

    def get_stats(self):
        self._read_limit()
        self._send(200, global_stats(CLOCK()), cache="public, max-age=60")

    def post_score_legacy(self):
        self._send(410, {"error": "this page is out of date, please reload"})

    def post_run(self):
        self._check_origin()
        data = self._read_json()
        if not LIMIT_RUNS.allow(client_key(self)):
            raise ApiError(429, "too many requests")
        mode = data.get("mode", "classic")
        difficulty = data.get("difficulty", 1)
        if mode not in MODES or difficulty not in (0, 1, 2) or isinstance(difficulty, bool):
            raise ApiError(400, "bad request")
        run_id = secrets.token_urlsafe(18)
        conn = connect()
        try:
            conn.execute("INSERT INTO runs (id, mode, difficulty, started) VALUES (?,?,?,?)",
                         (run_id, mode, difficulty, CLOCK()))
        finally:
            conn.close()
        self._send(201, {"run": run_id, "mode": mode, "difficulty": difficulty})

    def post_score(self):
        self._check_origin()
        data = self._read_json()
        if not LIMIT_SCORES.allow(client_key(self)):
            raise ApiError(429, "too many requests")

        run_id = data.get("run")
        if not isinstance(run_id, str) or not 16 <= len(run_id) <= 64:
            raise ApiError(400, "run required")
        name = data.get("name") or ANON
        if not isinstance(name, str):
            raise ApiError(400, "bad name")
        name = re.sub(r"\s+", " ", name).strip() or ANON
        if name.upper() == ANON:
            name = ANON
        if not name_is_allowed(name):
            raise ApiError(422, "name not allowed")
        player = data.get("player")
        if name != ANON and (not isinstance(player, str) or not PLAYER_RE.match(player)):
            raise ApiError(400, "player key required")

        now = CLOCK()
        conn = connect()
        try:
            # BEGIN IMMEDIATE takes the write lock up front, so two concurrent
            # submissions of the same run cannot both see it as unused.
            conn.execute("BEGIN IMMEDIATE")
            try:
                r = conn.execute("SELECT mode, difficulty, started, used FROM runs WHERE id = ?",
                                 (run_id,)).fetchone()
                if r is None or now - r[2] > RUN_TTL:
                    raise ApiError(404, "unknown or expired run")
                if r[3]:
                    raise ApiError(409, "run already submitted")
                run = {"mode": r[0], "difficulty": r[1], "started": r[2]}
                row, err = check_submission(run, data, now)
                if err:
                    # Burn the run: no retrying with tweaked numbers.
                    conn.execute("UPDATE runs SET used = 1 WHERE id = ?", (run_id,))
                    conn.execute("COMMIT")
                    raise ApiError(422, err)

                if name != ANON:
                    key_hash = hashlib.sha256(player.encode()).hexdigest()
                    owner = conn.execute("SELECT key_hash FROM players WHERE name_key = ?",
                                         (name.lower(),)).fetchone()
                    if owner is None:
                        conn.execute("INSERT INTO players (name_key, key_hash, created)"
                                     " VALUES (?,?,?)", (name.lower(), key_hash, int(now)))
                    elif not secrets.compare_digest(owner[0], key_hash):
                        raise ApiError(409, "name taken")

                conn.execute("UPDATE runs SET used = 1 WHERE id = ?", (run_id,))
                conn.execute(
                    """INSERT INTO scores (name, score, wave, mode, difficulty, houses, trees,
                           props, animals, vehicles, birds, max_combo, run_time, created_ts)
                       VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                    (name, row["score"], row["wave"], run["mode"], run["difficulty"],
                     row["houses"], row["trees"], row["props"], row["animals"],
                     row["vehicles"], row["birds"], row["max_combo"], row["run_time"],
                     int(now)))
                rank = conn.execute(
                    "SELECT COUNT(*) + 1 FROM scores WHERE mode = ? AND difficulty = ?"
                    " AND score > ?", (run["mode"], run["difficulty"], row["score"])).fetchone()[0]
                conn.execute("COMMIT")
            except BaseException:
                if conn.in_transaction:
                    conn.execute("ROLLBACK")
                raise
        finally:
            conn.close()
        self._send(200, {"ok": True, "rank": rank, "mode": run["mode"],
                         "difficulty": run["difficulty"]})

    def log_message(self, fmt, *args):
        pass  # nginx already logs access; errors go to stderr explicitly


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 64


def main():
    init_db()
    maintenance()
    threading.Thread(target=maintenance_loop, daemon=True).start()
    srv = Server((HOST, PORT), Handler)
    print(f"Tornado leaderboard listening on {HOST}:{PORT}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
