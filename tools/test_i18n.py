#!/usr/bin/env python3
"""Checks the English dictionary (web/i18n.js) against the page.

    python3 tools/test_i18n.py

Every Romanian string the player can see must have an English entry:
text and aria-label/title/placeholder in index.html, strings passed to t()
in app.js, achievement titles/descriptions and any app.js literal with
Romanian diacritics. Entries must keep the same {placeholders}, contain no
Romanian, and be used somewhere (a stale key usually means a typo).
"""
import json
import os
import re
import sys
import unittest
from html.parser import HTMLParser

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(REPO, "web")
RO_LETTERS = re.compile(r"[ăâîșțşţĂÂÎȘȚŞŢ]")
# Shown as-is in both languages.
SAME = {"Normal", "Auto", "Global", "Time Attack", "Time Attack · 3 min", "Restart", "restart ·",
        "Game over", "Mouse", "Desktop:", "Gamepad:", "C++", "WebAssembly", "WebGL 2", "GitHub",
        "Română", "English", "Top 10"}


def read(name):
    with open(os.path.join(WEB, name), encoding="utf-8") as f:
        return f.read()


def load_dictionary():
    m = re.search(r"window\.TORNADO_EN = (\{.*\});\s*$", read("i18n.js"), re.S)
    if not m:
        raise AssertionError("i18n.js: 'window.TORNADO_EN = {...};' not found")
    return json.loads(m.group(1))


class PageText(HTMLParser):
    """Visible text of the page as app.js's walker sees it."""
    SKIP = {"script", "style", "svg", "noscript", "head"}
    VOID = {"meta", "link", "input", "br", "img", "use", "path", "circle", "rect", "source"}

    def __init__(self):
        super().__init__()
        self.stack, self.strings, self.title = [], set(), None

    def handle_starttag(self, tag, attrs):
        if tag not in self.VOID:
            self.stack.append(tag)
        if "head" in self.stack:
            return
        for k, v in attrs:
            if k in ("aria-label", "title", "placeholder") and v:
                self.add(v)

    def handle_endtag(self, tag):
        if tag in self.stack:
            while self.stack and self.stack.pop() != tag:
                pass

    def handle_data(self, data):
        if self.stack and self.stack[-1] == "title":
            self.title = data.strip()
        elif not self.SKIP.intersection(self.stack):
            self.add(data)

    def add(self, text):
        text = re.sub(r"\s+", " ", text).strip()
        if re.search(r"[A-Za-zĂÂÎȘȚăâîșț]{2}", text):
            self.strings.add(text)


def js_literals(src):
    """All single-quoted string literals in a JS source, unescaped."""
    return [bytes(m, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
            if "\\" in m else m
            for m in re.findall(r"'((?:[^'\\\n]|\\.)*)'", src)]


def t_keys(src):
    """String literals that reach t(): every literal in the first argument
    of a t(...) call, except ones being compared (mode === 'busy')."""
    keys = set()
    for m in re.finditer(r"(?<![\w.$])t\(", src):
        depth, i, arg_end = 1, m.end(), None
        quote = None
        while i < len(src) and depth:
            c = src[i]
            if quote:
                if c == "\\":
                    i += 1
                elif c == quote:
                    quote = None
            elif c in "'\"":
                quote = c
            elif c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == "," and depth == 1 and arg_end is None:
                arg_end = i
            i += 1
        arg = src[m.end():arg_end if arg_end is not None else i - 1]
        for lit in re.finditer(r"'((?:[^'\\]|\\.)*)'", arg):
            if not arg[:lit.start()].rstrip().endswith(("===", "!==")):
                keys.add(lit.group(1))
    return keys


class DictionaryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.en = load_dictionary()
        page = PageText()
        page.feed(read("index.html"))
        cls.page_strings, cls.page_title = page.strings, page.title
        cls.app = read("app.js")
        cls.literals = set(js_literals(cls.app))

    def missing(self, strings):
        return sorted(s for s in strings if s not in self.en and s not in SAME)

    def test_page_text_is_translated(self):
        self.assertTrue(self.page_strings, "no text found in index.html")
        self.assertEqual(self.missing(self.page_strings), [])
        self.assertIn(self.page_title, self.en, "the <title> needs an entry")

    def test_strings_built_by_app_js_are_translated(self):
        keys = t_keys(self.app)
        self.assertGreater(len(keys), 50)
        self.assertEqual(self.missing(keys), [])

    def test_achievements_are_translated(self):
        found = re.findall(r"\b(?:title|desc): '((?:[^'\\]|\\.)*)'", self.app)
        self.assertGreater(len(found), 30)
        self.assertEqual(self.missing(found), [])

    def test_no_untranslated_romanian_literal(self):
        ro = {s for s in self.literals if RO_LETTERS.search(s)}
        self.assertEqual(self.missing(ro), [])

    def test_ui_sinks_never_get_raw_text(self):
        # toast/textContent/setSubmitUi take translated text: t('...'), not '...'.
        raw = re.findall(r"(?:toast\(|textContent = |setSubmitUi\('\w+', )'([^']*[A-Za-z][^']*)'", self.app)
        self.assertEqual([s for s in raw if s not in SAME], [])

    def test_placeholders_match(self):
        bad = [k for k, v in self.en.items()
               if sorted(re.findall(r"\{(\w+)\}", k)) != sorted(re.findall(r"\{(\w+)\}", v))]
        self.assertEqual(bad, [])

    def test_english_has_no_romanian(self):
        self.assertEqual([v for v in self.en.values() if RO_LETTERS.search(v)], [])

    def test_entries_are_trimmed_and_used(self):
        self.assertEqual([k for k in self.en if k != re.sub(r"\s+", " ", k).strip()], [])
        used = self.page_strings | self.literals | {self.page_title}
        self.assertEqual(sorted(k for k in self.en if k not in used), [])

    def test_dynamic_elements_exist(self):
        m = re.search(r"var DYNAMIC = ((?:'[^']*'\s*\+?\s*)+);", self.app)
        self.assertIsNotNone(m)
        ids = re.findall(r"#([\w-]+)", m.group(1))
        html = read("index.html")
        self.assertEqual([i for i in ids if f'id="{i}"' not in html], [])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + sys.argv[1:], verbosity=1)
