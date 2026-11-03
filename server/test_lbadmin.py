#!/usr/bin/env python3
"""Tests for server/lbadmin.py (moderation CLI).   python3 server/test_lbadmin.py"""
import contextlib
import hashlib
import io
import os
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lbadmin  # noqa: E402
import leaderboard as lb  # noqa: E402

KEY_A, KEY_B = "a" * 30, "b" * 30
HASH_A, HASH_B = (hashlib.sha256(k.encode()).hexdigest() for k in (KEY_A, KEY_B))


class LbAdmin(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = str(Path(self.tmp.name) / "scores.db")
        self._old = lb.DB_PATH
        lb.DB_PATH = Path(self.db)
        lb.init_db()
        c = sqlite3.connect(self.db)
        rows = [  # name, score, player_hash
            ("Cheater", 999_000, HASH_A),
            ("ANONIM", 888_000, HASH_A),      # same device, anonymous
            ("Alias", 777_000, HASH_A),       # second name of the same device
            ("Honest", 5_000, HASH_B),
            ("ANONIM", 4_000, ""),            # pre-tracking row
        ]
        for name, score, h in rows:
            c.execute("INSERT INTO scores (name, score, wave, mode, difficulty, run_time, created_ts,"
                      " player_hash) VALUES (?, ?, 3, 'classic', 1, 120, 1800000000, ?)", (name, score, h))
        c.executemany("INSERT INTO players VALUES (?, ?, 0)",
                      [("cheater", HASH_A), ("alias", HASH_A), ("honest", HASH_B)])
        c.commit()
        c.close()

    def tearDown(self):
        lb.DB_PATH = self._old
        self.tmp.cleanup()

    def run_cli(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            lbadmin.main(["--db", self.db, "--yes", *argv])
        return out.getvalue()

    def names(self):
        c = sqlite3.connect(self.db)
        try:
            return sorted(r[0] for r in c.execute("SELECT name FROM scores"))
        finally:
            c.close()

    def bans(self):
        c = sqlite3.connect(self.db)
        try:
            return set(c.execute("SELECT kind, value FROM bans"))
        finally:
            c.close()

    def test_listing(self):
        out = self.run_cli("top", "-n", "2")
        self.assertIn("Cheater", out)
        self.assertIn("999000", out)
        self.assertNotIn("Honest", out)
        self.assertIn("Honest", self.run_cli("recent", "-n", "3") + self.run_cli("show", "4"))

    def test_daily_board(self):
        c = sqlite3.connect(self.db)
        c.executemany("INSERT INTO scores (name, score, wave, mode, difficulty, created_ts, day)"
                      " VALUES (?, ?, 1, 'daily', 1, 1800000000, ?)",
                      [("Monday", 3000, "2027-03-01"), ("Tuesday", 4000, "2027-03-02")])
        c.commit()
        c.close()
        out = self.run_cli("top", "-m", "daily", "--day", "2027-03-01")
        self.assertIn("Monday", out)
        self.assertIn("daily/2027-03-01", out)
        self.assertNotIn("Tuesday", out)
        self.assertNotIn("Cheater", out)

    def test_ban_by_name_takes_the_device_and_its_other_names(self):
        self.run_cli("ban", "--name", "cheater", "--reason", "impossible score")
        self.assertEqual(self.names(), ["ANONIM", "Honest"])   # only the pre-tracking anon row survives
        self.assertEqual(self.bans(), {("name", "cheater"), ("name", "alias"), ("player", HASH_A)})

    def test_ban_by_anonymous_score_and_keep_scores(self):
        self.run_cli("ban", "--score", "2", "--keep-scores")
        self.assertIn(("player", HASH_A), self.bans())
        self.assertEqual(len(self.names()), 5)

    def test_untracked_anonymous_score_cannot_be_banned(self):
        with self.assertRaises(SystemExit):
            self.run_cli("ban", "--score", "5")

    def test_unban_delete_release(self):
        self.run_cli("ban", "--name", "Honest", "--keep-scores")
        self.run_cli("unban", "--name", "honest")
        self.run_cli("unban", "--player", HASH_B)
        self.assertEqual(self.bans(), set())
        self.run_cli("delete", "1", "3")
        self.assertNotIn("Cheater", self.names())
        self.run_cli("release", "Honest")
        c = sqlite3.connect(self.db)
        self.assertIsNone(c.execute("SELECT 1 FROM players WHERE name_key = 'honest'").fetchone())
        c.close()

    def test_refuses_other_users_database(self):
        real = os.geteuid
        os.geteuid = lambda: real() + 1
        try:
            with self.assertRaises(SystemExit) as cm:
                self.run_cli("top")
            self.assertIn("sudo -u", str(cm.exception))
        finally:
            os.geteuid = real

    def test_destructive_commands_need_confirmation(self):
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
            lbadmin.main(["--db", self.db, "delete", "1"])     # stdin is not a TTY here
        self.assertIn("Cheater", self.names())


if __name__ == "__main__":
    unittest.main()
