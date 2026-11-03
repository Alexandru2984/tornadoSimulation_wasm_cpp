#!/usr/bin/env python3
"""Tests for the leaderboard API (stdlib unittest, no network beyond loopback).

Run:  python3 -m unittest -v server/test_leaderboard.py
"""
import json
import os
import re
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import leaderboard as lb  # noqa: E402

ORIGIN = "https://tornado.micutu.com"
PLAYER = "p" * 32
OTHER_PLAYER = "q" * 32


def plausible_stats(kills_houses=20, time=120):
    return {"houses": kills_houses, "trees": 10, "props": 5, "animals": 3,
            "vehicles": 2, "birds": 1, "maxCombo": 8, "time": time}


class NameFilter(unittest.TestCase):
    def test_allows_innocent_names(self):
        for n in ("Mercury", "Curtis", "Populate", "Future", "Alex_99",
                  "Ion Popescu", "a.b-c", "Dickens", "Cockburn"[:4] + "pit", "grape"):
            self.assertTrue(lb.name_is_allowed(n), n)

    def test_blocks_abuse(self):
        for n in ("f.u.c.k", "FUCK you", "sh1t", "cur", "Pula", "admin",
                  "b i t c h", "n4zi", "rape"):
            self.assertFalse(lb.name_is_allowed(n), n)

    def test_rejects_bad_shapes(self):
        for n in ("", "...", "----", "x" * 17, "<script>", "ăîș", "a\nb"):
            self.assertFalse(lb.name_is_allowed(n), repr(n))


class Rules(unittest.TestCase):
    def test_kills_to_reach_wave(self):
        self.assertEqual(lb.kills_to_reach_wave(1), 0)
        self.assertEqual(lb.kills_to_reach_wave(2), 5)
        self.assertEqual(lb.kills_to_reach_wave(3), 13)
        # all 10 waves completed (reaching endless wave 11)
        self.assertEqual(lb.kills_to_reach_wave(11), 185)

    def test_constants_match_cpp(self):
        src = (HERE.parent / "src" / "constants.h").read_text()

        def const(name):
            m = re.search(rf"\b{name}\s*=\s*([0-9.]+)f?;", src)
            self.assertIsNotNone(m, name)
            return float(m.group(1))

        self.assertEqual(const("WAVE_BASE_TARGET"), lb.WAVE_BASE_TARGET)
        self.assertEqual(const("WAVE_TARGET_STEP"), lb.WAVE_TARGET_STEP)
        self.assertEqual(const("TIME_ATTACK_SECONDS"), lb.TIME_ATTACK_SECONDS)
        self.assertEqual(const("PTS_HOUSE"), lb.PTS_HOUSE)
        self.assertEqual(const("PTS_TREE"), lb.PTS_TREE)
        self.assertEqual(const("PTS_VEHICLE"), lb.PTS_VEHICLE)
        self.assertEqual(const("PTS_BIRD"), lb.PTS_BIRD)
        self.assertEqual(const("PTS_COW"), lb.PTS_ANIMAL_MAX)
        self.assertEqual(const("PTS_SHEEP"), lb.PTS_ANIMAL_MIN)
        self.assertEqual(const("PTS_PARKED_CAR"), lb.PTS_PROP_MAX)
        self.assertEqual(const("PTS_POLE"), lb.PTS_PROP_MIN)
        self.assertEqual(const("COMBO_MAX_MULT"), lb.MAX_COMBO_MULT)

    def _run(self, mode="classic", started=1000.0):
        return {"mode": mode, "difficulty": 1, "started": started}

    def _check(self, run, now, **over):
        data = {"score": 5000, "wave": 3, "stats": plausible_stats()}
        data.update(over)
        return lb.check_submission(run, data, now)

    def test_plausible_classic(self):
        row, err = self._check(self._run(), 1200.0)
        self.assertIsNone(err)
        self.assertEqual(row["houses"], 20)

    def test_score_too_high_for_destruction(self):
        _, err = self._check(self._run(), 1200.0, score=1_000_000)
        self.assertEqual(err, "implausible score")

    def test_score_too_low_for_destruction(self):
        _, err = self._check(self._run(), 1200.0, score=10)
        self.assertEqual(err, "implausible score")

    def test_wave_without_enough_kills(self):
        _, err = self._check(self._run(), 1200.0, wave=10)
        self.assertEqual(err, "inconsistent wave")

    def test_combo_cannot_exceed_kills(self):
        st = plausible_stats()
        st["maxCombo"] = 999
        _, err = self._check(self._run(), 1200.0, stats=st)
        self.assertEqual(err, "inconsistent combo")

    def test_client_time_cannot_exceed_wall_clock(self):
        _, err = self._check(self._run(), 1030.0, stats=plausible_stats(time=500))
        self.assertEqual(err, "inconsistent time")

    def test_kill_rate(self):
        st = plausible_stats(kills_houses=100_000, time=1)
        _, err = self._check(self._run(), 1002.0, stats=st, score=10_000_000)
        self.assertEqual(err, "implausible rate")

    def test_time_attack_needs_full_duration(self):
        _, err = self._check(self._run("timeattack"), 1060.0, wave=1,
                             stats=plausible_stats(time=55))
        self.assertEqual(err, "too early")
        row, err = self._check(self._run("timeattack"), 1180.0, wave=1)
        self.assertIsNone(err)

    def test_time_attack_has_no_waves(self):
        _, err = self._check(self._run("timeattack"), 1200.0, wave=4)
        self.assertEqual(err, "inconsistent wave")

    def test_garbage(self):
        run = self._run()
        for bad in ({"score": "x", "wave": 1}, {"wave": 1}, {"score": 1e400, "wave": 1},
                    {"score": 10, "wave": 1, "stats": []},
                    {"score": 10, "wave": 1, "stats": {"time": float("nan")}}):
            _, err = lb.check_submission(run, bad, 1200.0)
            self.assertIsNotNone(err, bad)


class RateLimiterTest(unittest.TestCase):
    def test_burst_then_block(self):
        rl = lb.RateLimiter(rate=0.0001, burst=3)
        self.assertEqual([rl.allow("a") for _ in range(4)], [True, True, True, False])
        self.assertTrue(rl.allow("b"))

    def test_bounded(self):
        rl = lb.RateLimiter(rate=1, burst=1, max_keys=100)
        for i in range(1000):
            rl.allow(str(i))
        self.assertLessEqual(len(rl._buckets), 100)


class FakeHandler:
    def __init__(self, peer, real_ip=None, xff=None):
        self.client_address = (peer, 1234)
        self.headers = {}
        if real_ip:
            self.headers["X-Real-IP"] = real_ip
        if xff:
            self.headers["X-Forwarded-For"] = xff


class ClientKey(unittest.TestCase):
    def test_trusts_proxy_header_only_from_loopback(self):
        self.assertEqual(lb.client_key(FakeHandler("127.0.0.1", "203.0.113.9")), "203.0.113.9")
        self.assertEqual(lb.client_key(FakeHandler("198.51.100.1", "203.0.113.9")), "198.51.100.1")

    def test_spoofed_forwarded_for_is_ignored(self):
        # client claims 1.2.3.4, nginx appended the real address
        h = FakeHandler("127.0.0.1", xff="1.2.3.4, 203.0.113.7")
        self.assertEqual(lb.client_key(h), "203.0.113.7")

    def test_ipv6_grouped_by_64(self):
        a = lb.client_key(FakeHandler("127.0.0.1", "2001:db8:1:2:aaaa::1"))
        b = lb.client_key(FakeHandler("127.0.0.1", "2001:db8:1:2:bbbb::9"))
        self.assertEqual(a, b)
        self.assertEqual(a, "2001:db8:1:2::/64")


class Api(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        lb.DB_PATH = Path(cls.tmp.name) / "scores.db"
        lb.init_db()
        cls.srv = lb.Server(("127.0.0.1", 0), lb.Handler)
        cls.base = f"http://127.0.0.1:{cls.srv.server_address[1]}"
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()
        cls.tmp.cleanup()

    def setUp(self):
        self.now = 1_800_000_000.0
        lb.CLOCK = lambda: self.now
        lb.LIMIT_RUNS = lb.RateLimiter(rate=1000, burst=1000)
        lb.LIMIT_SCORES = lb.RateLimiter(rate=1000, burst=1000)
        lb.LIMIT_READS = lb.RateLimiter(rate=1000, burst=1000)
        lb._stats_cache.update(at=0.0, data=None)

    def req(self, method, path, body=None, headers=None, raw=None):
        h = {"Origin": ORIGIN}
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            h["Content-Type"] = "application/json"
        if raw is not None:
            data = raw
        h.update(headers or {})
        r = urllib.request.Request(self.base + path, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(r, timeout=5) as resp:
                payload = resp.read()
                return resp.status, (json.loads(payload) if payload else None), resp.headers
        except urllib.error.HTTPError as e:
            payload = e.read()
            return e.code, (json.loads(payload) if payload else None), e.headers

    def start_run(self, mode="classic", difficulty=1):
        code, body, _ = self.req("POST", "/api/v2/runs", {"mode": mode, "difficulty": difficulty})
        self.assertEqual(code, 201, body)
        return body["run"]

    def submit(self, run, name="Tester", score=5000, wave=3, player=PLAYER, stats=None):
        return self.req("POST", "/api/v2/scores", {
            "run": run, "name": name, "score": score, "wave": wave,
            "player": player, "stats": stats or plausible_stats()})

    def test_health_and_head(self):
        self.assertEqual(self.req("GET", "/api/v2/health")[0], 200)
        self.assertEqual(self.req("HEAD", "/api/v2/health")[0], 200)
        self.assertEqual(self.req("GET", "/api/nope")[0], 404)
        self.assertEqual(self.req("DELETE", "/api/v2/scores")[0], 405)

    def test_health_fails_when_storage_is_unusable(self):
        good = lb.DB_PATH
        lb.DB_PATH = Path(self.tmp.name) / "missing-dir" / "scores.db"
        try:
            status, body, _ = self.req("GET", "/api/v2/health")
        finally:
            lb.DB_PATH = good
        self.assertEqual((status, body), (503, {"error": "storage busy"}))

    def test_banned_device_and_name(self):
        import hashlib
        import sqlite3
        other = "z" * 40
        c = sqlite3.connect(lb.DB_PATH)
        c.executemany("INSERT INTO bans (kind, value, created) VALUES (?, ?, 0)",
                      [("player", hashlib.sha256(other.encode()).hexdigest()), ("name", "badguy")])
        c.commit()
        c.close()
        try:
            run = self.start_run()
            self.now += 150
            # banned device: refused even anonymously
            self.assertEqual(self.submit(run, name="ANONIM", player=other)[:2], (403, {"error": "banned"}))
            # banned name: refused for any device
            self.assertEqual(self.submit(run, name="BadGuy")[:2], (403, {"error": "banned"}))
            # the run was not burned by the refusals
            code, body, _ = self.submit(run, name="Clean")
            self.assertEqual(code, 200, body)
            c = sqlite3.connect(lb.DB_PATH)
            h = c.execute("SELECT player_hash FROM scores WHERE name = 'Clean'").fetchone()[0]
            c.close()
            self.assertEqual(h, hashlib.sha256(PLAYER.encode()).hexdigest())
        finally:
            c = sqlite3.connect(lb.DB_PATH)
            c.execute("DELETE FROM bans")
            c.commit()
            c.close()

    def test_full_flow_and_replay(self):
        run = self.start_run(difficulty=2)
        self.now += 150
        code, body, _ = self.submit(run, name="Flow", score=7000)
        self.assertEqual(code, 200, body)
        self.assertEqual(body["difficulty"], 2)
        self.assertGreaterEqual(body["rank"], 1)
        # replay of the same run
        code, body, _ = self.submit(run, name="Flow", score=7000)
        self.assertEqual(code, 409)
        # shows up on the right board only
        code, body, hdr = self.req("GET", "/api/v2/scores?mode=classic&difficulty=2")
        self.assertEqual(code, 200)
        self.assertIn("Flow", [s["name"] for s in body["scores"]])
        self.assertIn("max-age", hdr["Cache-Control"])
        _, body, _ = self.req("GET", "/api/v2/scores?mode=classic&difficulty=0")
        self.assertNotIn("Flow", [s["name"] for s in body["scores"]])

    def test_forged_score_is_rejected_and_run_burned(self):
        run = self.start_run()
        self.now += 60
        code, body, _ = self.submit(run, score=1_000_000, wave=999)
        self.assertEqual(code, 422)
        # cannot retry with "fixed" numbers on the same run
        code, _, _ = self.submit(run)
        self.assertEqual(code, 409)

    def test_unknown_and_expired_run(self):
        self.assertEqual(self.submit("x" * 24)[0], 404)
        run = self.start_run()
        self.now += lb.RUN_TTL + 1
        self.assertEqual(self.submit(run)[0], 404)

    def test_run_required(self):
        code, _, _ = self.req("POST", "/api/v2/scores", {"name": "a", "score": 5, "wave": 1})
        self.assertEqual(code, 400)

    def test_name_ownership(self):
        r1 = self.start_run()
        self.now += 150
        self.assertEqual(self.submit(r1, name="Owner")[0], 200)
        r2 = self.start_run()
        self.now += 150
        code, body, _ = self.submit(r2, name="owner", player=OTHER_PLAYER)
        self.assertEqual(code, 409, body)
        self.assertEqual(body["error"], "name taken")
        # the run was not burned by the name clash; the owner can still use it
        self.assertEqual(self.submit(r2, name="Owner")[0], 200)

    def test_anonymous_needs_no_player_key(self):
        run = self.start_run()
        self.now += 150
        code, body, _ = self.submit(run, name="anonim", player=None)
        self.assertEqual(code, 200, body)

    def test_bad_names(self):
        run = self.start_run()
        self.now += 150
        self.assertEqual(self.submit(run, name="f.u.c.k")[0], 422)
        self.assertEqual(self.submit(run, name="<b>x</b>")[0], 422)

    def test_origin_and_fetch_metadata(self):
        code, _, _ = self.req("POST", "/api/v2/runs", {"mode": "classic", "difficulty": 1},
                              headers={"Origin": "https://evil.example"})
        self.assertEqual(code, 403)
        code, _, _ = self.req("POST", "/api/v2/runs", {"mode": "classic", "difficulty": 1},
                              headers={"Sec-Fetch-Site": "cross-site"})
        self.assertEqual(code, 403)

    def test_body_validation(self):
        code, _, _ = self.req("POST", "/api/v2/runs", raw=b"mode=classic",
                              headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(code, 415)
        big = json.dumps({"mode": "classic", "pad": "x" * 5000}).encode()
        code, _, _ = self.req("POST", "/api/v2/runs", raw=big,
                              headers={"Content-Type": "application/json"})
        self.assertEqual(code, 413)
        code, _, _ = self.req("POST", "/api/v2/runs", raw=b"[1,2]",
                              headers={"Content-Type": "application/json"})
        self.assertEqual(code, 400)
        for bad in ({"mode": "god"}, {"mode": "classic", "difficulty": 7},
                    {"mode": "classic", "difficulty": True}):
            self.assertEqual(self.req("POST", "/api/v2/runs", bad)[0], 400, bad)

    def test_query_validation(self):
        for q in ("mode=x", "difficulty=9", "period=year", "difficulty=abc"):
            self.assertEqual(self.req("GET", "/api/v2/scores?" + q)[0], 400, q)
        code, body, _ = self.req("GET", "/api/v2/scores?limit=100000")
        self.assertEqual(code, 200)
        self.assertLessEqual(len(body["scores"]), 50)

    def test_period_filter(self):
        run = self.start_run(mode="classic", difficulty=0)
        self.now += 150
        self.assertEqual(self.submit(run, name="OldTimer", player="o" * 32)[0], 200)
        self.now += 10 * 86400
        _, body, _ = self.req("GET", "/api/v2/scores?difficulty=0&period=week")
        self.assertNotIn("OldTimer", [s["name"] for s in body["scores"]])
        _, body, _ = self.req("GET", "/api/v2/scores?difficulty=0&period=all")
        self.assertIn("OldTimer", [s["name"] for s in body["scores"]])

    def test_best_entry_per_player(self):
        for score in (5000, 6000):
            run = self.start_run(difficulty=0)
            self.now += 150
            self.assertEqual(self.submit(run, name="Dup", player="d" * 32, score=score)[0], 200)
        _, body, _ = self.req("GET", "/api/v2/scores?difficulty=0")
        dups = [s for s in body["scores"] if s["name"] == "Dup"]
        self.assertEqual(len(dups), 1)
        self.assertEqual(dups[0]["score"], 6000)

    def test_time_attack_flow(self):
        run = self.start_run(mode="timeattack")
        self.now += 100
        self.assertEqual(self.submit(run, wave=1)[0], 422)
        run = self.start_run(mode="timeattack")
        self.now += 181
        self.assertEqual(self.submit(run, wave=1, name="Speedy", player="s" * 32)[0], 200)
        _, body, _ = self.req("GET", "/api/v2/scores?mode=timeattack")
        self.assertIn("Speedy", [s["name"] for s in body["scores"]])

    def start_daily(self, day=None):
        day = day or lb.utc_day(self.now)
        code, body, _ = self.req("POST", "/api/v2/runs", {"mode": "daily", "difficulty": 1, "day": day})
        self.assertEqual(code, 201, body)
        self.assertEqual(body["day"], day)
        return body["run"]

    def test_daily_flow(self):
        today = lb.utc_day(self.now)
        run = self.start_daily()
        self.now += 100
        self.assertEqual(self.submit(run, wave=1)[0], 422)          # 3-minute rule
        run = self.start_daily()
        self.now += 181
        code, body, _ = self.submit(run, wave=1, name="Daily1", player="y" * 32)
        self.assertEqual((code, body["day"], body["rank"]), (200, today, 1))
        _, body, _ = self.req("GET", "/api/v2/scores?mode=daily")
        self.assertEqual(body["day"], today)
        self.assertIn("Daily1", [r["name"] for r in body["scores"]])
        _, body, _ = self.req("GET", "/api/v2/scores?mode=daily&day=2020-01-01")
        self.assertEqual(body["scores"], [])
        # Daily scores stay off the regular boards.
        _, body, _ = self.req("GET", "/api/v2/scores?mode=timeattack")
        self.assertNotIn("Daily1", [r["name"] for r in body["scores"]])

    def test_daily_run_rules(self):
        today = lb.utc_day(self.now)
        bad = [{"mode": "daily", "difficulty": 0, "day": today},          # always Normal
               {"mode": "daily", "difficulty": 1},                        # day required
               {"mode": "daily", "difficulty": 1, "day": "2020-01-01"},   # wrong day
               {"mode": "daily", "difficulty": 1, "day": 20261103}]
        codes = [self.req("POST", "/api/v2/runs", b)[0] for b in bad]
        self.assertEqual(codes, [400, 422, 422, 422])
        self.assertEqual(self.req("GET", "/api/v2/scores?mode=daily&day=x")[0], 400)

    def test_daily_run_belongs_to_the_day_it_started(self):
        import calendar
        midnight = calendar.timegm((2027, 3, 1, 0, 0, 0))
        self.now = midnight - 120                                     # 23:58 UTC
        day1 = lb.utc_day(self.now)
        # A client clock a little ahead already says it is tomorrow: accepted.
        self.start_daily(lb.utc_day(midnight))
        run = self.start_daily(day1)
        self.now = midnight + 180                                     # 00:03, next day
        self.assertEqual(self.submit(run, wave=1, name="Owl", player="w" * 32)[0], 200)
        _, body, _ = self.req("GET", f"/api/v2/scores?mode=daily&day={day1}")
        self.assertIn("Owl", [r["name"] for r in body["scores"]])
        _, body, _ = self.req("GET", "/api/v2/scores?mode=daily")
        self.assertEqual((body["day"], body["scores"]), (lb.utc_day(midnight), []))

    def test_daily_rank_is_per_day(self):
        run = self.start_daily()
        self.now += 181
        self.assertEqual(self.submit(run, wave=1, score=9000, name="Big", player="b" * 32)[1]["rank"], 1)
        self.now += 86400
        run = self.start_daily()
        self.now += 181
        self.assertEqual(self.submit(run, wave=1, score=3000, name="Small", player="m" * 32)[1]["rank"], 1)

    def test_legacy_endpoints(self):
        code, body, _ = self.req("GET", "/api/scores")
        self.assertEqual(code, 200)
        self.assertIsInstance(body, list)
        code, _, _ = self.req("POST", "/api/scores", {"name": "x", "score": 9, "wave": 1})
        self.assertEqual(code, 410)

    def test_stats(self):
        code, body, _ = self.req("GET", "/api/v2/stats")
        self.assertEqual(code, 200)
        for k in ("scores", "houses", "trees", "best"):
            self.assertIn(k, body)

    def test_rate_limit(self):
        lb.LIMIT_RUNS = lb.RateLimiter(rate=0.0001, burst=2)
        codes = [self.req("POST", "/api/v2/runs", {"mode": "classic", "difficulty": 1})[0]
                 for _ in range(3)]
        self.assertEqual(codes, [201, 201, 429])

    def test_concurrent_double_submit(self):
        run = self.start_run()
        self.now += 150
        results = []

        def go():
            results.append(self.submit(run, name="Racer", player="r" * 32)[0])

        threads = [threading.Thread(target=go) for _ in range(6)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(results.count(200), 1, results)


class Migration(unittest.TestCase):
    def test_v1_database_is_migrated(self):
        import sqlite3
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "v1.db"
            c = sqlite3.connect(path)
            c.execute("""CREATE TABLE scores (id INTEGER PRIMARY KEY AUTOINCREMENT,
                         name TEXT NOT NULL, score INTEGER NOT NULL, wave INTEGER NOT NULL,
                         created TEXT NOT NULL DEFAULT (datetime('now')))""")
            c.execute("INSERT INTO scores (name, score, wave, created)"
                      " VALUES ('ANONIM', 47318, 10, '2026-07-07 10:00:00')")
            c.commit()
            c.close()
            old = lb.DB_PATH
            lb.DB_PATH = path
            try:
                lb.init_db()
                rows = lb.board_query("classic", 1, "all", 10, 1_800_000_000)
            finally:
                lb.DB_PATH = old
            self.assertEqual(rows[0]["score"], 47318)
            self.assertEqual(rows[0]["date"], "2026-07-07")


    def test_runs_table_gets_a_day_column(self):
        import sqlite3
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "v2.db"
            c = sqlite3.connect(path)
            c.execute("""CREATE TABLE runs (id TEXT PRIMARY KEY, mode TEXT NOT NULL,
                         difficulty INTEGER NOT NULL, started REAL NOT NULL,
                         used INTEGER NOT NULL DEFAULT 0) WITHOUT ROWID""")
            c.execute("INSERT INTO runs VALUES ('r' , 'classic', 1, 1.0, 0)")
            c.commit()
            c.close()
            old = lb.DB_PATH
            lb.DB_PATH = path
            try:
                lb.init_db()
                c = sqlite3.connect(path)
                self.assertEqual(c.execute("SELECT day FROM runs WHERE id = 'r'").fetchone(), ("",))
                c.close()
            finally:
                lb.DB_PATH = old


class WalFiles(unittest.TestCase):
    def test_wal_files_survive_other_connections(self):
        # The root-run backup must never be the one that creates -wal/-shm.
        with tempfile.TemporaryDirectory() as d:
            old = lb.DB_PATH
            lb.DB_PATH = Path(d) / "scores.db"
            try:
                lb.init_db()
                keeper = lb.hold_wal_files()
                try:
                    for _ in range(3):
                        c = lb.connect()
                        c.execute("INSERT INTO runs (id, mode, difficulty, started) VALUES (hex(randomblob(8)), 'classic', 1, 0)")
                        c.close()
                    lb.maintenance(now=10**10)
                    for suffix in ("-wal", "-shm"):
                        self.assertTrue(Path(str(lb.DB_PATH) + suffix).exists(), suffix)
                finally:
                    keeper.close()
                self.assertFalse(Path(str(lb.DB_PATH) + "-wal").exists())   # clean close removes them
            finally:
                lb.DB_PATH = old


if __name__ == "__main__":
    unittest.main()
