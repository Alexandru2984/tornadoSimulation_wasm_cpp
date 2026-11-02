#!/usr/bin/env python3
"""Tests for tools/package_site.py against the real web/ directory.

    python3 tools/test_package_site.py
"""
import datetime
import json
import os
import re
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import package_site as ps  # noqa: E402


class PackageSiteTest(unittest.TestCase):
    def setUp(self):
        # Output must live inside the "repo": use a temp repo root holding
        # a fake build dir (engine files are opaque to the packager).
        self.root = tempfile.mkdtemp(prefix="pkgtest-")
        self.build = os.path.join(self.root, "build")
        os.makedirs(self.build)
        for name, body in (("tornado.js", b"console.log(1)"), ("tornado.wasm", b"\0asm\1\0\0\0"),
                           ("tornado.data", b"shader data")):
            with open(os.path.join(self.build, name), "wb") as f:
                f.write(body)
        self.out = os.path.join(self.root, "dist")

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def run_package(self, version="abc1234"):
        return ps.package(self.build, os.path.join(REPO, "web"), self.out, version, self.root,
                          today=datetime.date(2026, 1, 2))

    def read(self, name):
        with open(os.path.join(self.out, name), encoding="utf-8") as f:
            return f.read()

    def test_hashed_names_and_references(self):
        names = self.run_package()
        for src, dst in names.items():
            self.assertRegex(dst, r"^[a-z]+\.[0-9a-f]{10}\.[a-z]+$")
            self.assertTrue(os.path.isfile(os.path.join(self.out, dst)), dst)
            self.assertFalse(os.path.exists(os.path.join(self.out, src)), src)
        html = self.read("index.html")
        info = json.loads(re.search(r'id="build-info"[^>]*>(.*?)</script>', html, re.S).group(1))
        self.assertEqual(info, {"build": "abc1234", "js": names["tornado.js"],
                                "wasm": names["tornado.wasm"], "data": names["tornado.data"]})
        self.assertIn(f'href="{names["style.css"]}"', html)
        self.assertIn(f'src="{names["app.js"]}"', html)
        for n in ps.ENGINE:
            self.assertIn(f'<link rel="preload" href="{names[n]}"', html)
        self.assertIn(f'href="/{names["style.css"]}"', self.read("404.html"))

    def test_service_worker_and_sitemap(self):
        names = self.run_package("v-9")
        sw = self.read("sw.js")
        self.assertIn('var VERSION = "v-9";', sw)
        assets = json.loads(re.search(r"var ASSETS = (\[.*?\]);", sw).group(1))
        self.assertEqual(set(names.values()) - set(assets), set())
        self.assertIn("<lastmod>2026-01-02</lastmod>", self.read("sitemap.xml"))

    def test_hash_follows_content(self):
        a = self.run_package()["tornado.wasm"]
        with open(os.path.join(self.build, "tornado.wasm"), "ab") as f:
            f.write(b"changed")
        b = self.run_package()["tornado.wasm"]
        self.assertNotEqual(a, b)
        self.assertEqual(sorted(f for f in os.listdir(self.out) if f.endswith(".wasm")), [b])

    def test_rejects_bad_input(self):
        with self.assertRaises(ps.PackageError):
            self.run_package("bad version!")
        with self.assertRaises(ps.PackageError):     # would wipe something outside the repo
            ps.package(self.build, os.path.join(REPO, "web"), tempfile.gettempdir(), "x", self.root)
        with self.assertRaises(ps.PackageError):
            ps.package(self.build, os.path.join(REPO, "web"), self.root, "x", self.root)
        os.remove(os.path.join(self.build, "tornado.data"))
        with self.assertRaises(ps.PackageError):
            self.run_package()

    def test_verify_catches_broken_reference(self):
        self.run_package()
        with open(os.path.join(self.out, "index.html"), "a", encoding="utf-8") as f:
            f.write('<img src="missing.png">')
        with self.assertRaises(ps.PackageError):
            ps.verify(self.out, [])

    def test_files_world_readable(self):
        self.run_package()
        for root, dirs, files in os.walk(self.out):
            for f in files:
                self.assertEqual(os.stat(os.path.join(root, f)).st_mode & 0o777, 0o644, f)


if __name__ == "__main__":
    unittest.main(verbosity=1)
