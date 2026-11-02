#!/usr/bin/env python3
"""Package a WASM build into a deployable static site with hashed file names.

    python3 tools/package_site.py --build build-wasm --out dist --version 1a2b3c4

Copies web/ plus the Emscripten output into --out and gives every file that
nginx serves as "immutable" a content hash in its name (tornado.<hash>.wasm,
app.<hash>.js, ...). Then it rewrites the references:

  index.html  stylesheet, i18n.js, app.js, preload links, the build-info JSON block
  404.html    stylesheet
  sw.js       VERSION and the precache ASSETS list
  sitemap.xml <lastmod>

and checks that every local href/src in the HTML points at a real file, so
a broken reference fails the deploy instead of the page. Only the standard
library is used; nothing outside --out is modified.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import struct
import sys

ENGINE = ("tornado.js", "tornado.wasm", "tornado.data")   # from the build dir
PAGE_ASSETS = ("i18n.js", "app.js", "style.css")         # from web/
HASHED = ENGINE + PAGE_ASSETS                             # long-cached by nginx
# Precached by the service worker besides the hashed files (the page itself
# is always cached by sw.js).
PRECACHE_EXTRA = ("manifest.webmanifest", "favicon.svg", "icons/icon-192.png")


class PackageError(Exception):
    pass


def digest(path, n=10):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()[:n]


def hashed_name(name, h):
    stem, ext = os.path.splitext(name)
    return f"{stem}.{h}{ext}"


def replace_once(text, old, new, where):
    n = text.count(old)
    if n != 1:
        raise PackageError(f"{where}: expected exactly one {old!r}, found {n}")
    return text.replace(old, new)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def local_refs(html):
    """href/src values that point at files on this site."""
    for ref in re.findall(r'\b(?:href|src)="([^"]+)"', html):
        if re.match(r"^(?:[a-z][a-z0-9+.-]*:|//|#)", ref, re.I):
            continue                      # absolute URL, protocol-relative, fragment
        ref = ref.split("#")[0].split("?")[0]
        if ref in ("", "/"):
            continue
        yield ref


def image_size(path):
    """(width, height) of a PNG or WebP file, or None."""
    with open(path, "rb") as f:
        head = f.read(32)
    if head[:8] == b"\x89PNG\r\n\x1a\n":
        return struct.unpack(">II", head[16:24])
    if head[:4] == b"RIFF" and head[8:12] == b"WEBP":
        chunk = head[12:16]
        if chunk == b"VP8X":                      # extended: 24-bit (size - 1)
            return (int.from_bytes(head[24:27], "little") + 1, int.from_bytes(head[27:30], "little") + 1)
        if chunk == b"VP8 ":                      # lossy: 14-bit sizes after the frame tag
            w, h = struct.unpack("<HH", head[26:30])
            return (w & 0x3FFF, h & 0x3FFF)
        if chunk == b"VP8L":                      # lossless: 14-bit (size - 1) fields
            bits = int.from_bytes(head[21:25], "little")
            return ((bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1)
    return None


def check_screenshots(out, manifest):
    """Manifest screenshots exist, match their declared size and meet what
    Chrome's install dialog accepts (320-3840 px, aspect <= 2.3, wide =
    landscape, narrow = portrait)."""
    problems = []
    for shot in manifest.get("screenshots", []):
        path = os.path.join(out, shot["src"])
        if not os.path.isfile(path):
            problems.append(f"manifest: {shot['src']} does not exist")
            continue
        size = image_size(path)
        declared = tuple(int(x) for x in shot.get("sizes", "0x0").split("x"))
        if size != declared:
            problems.append(f"manifest: {shot['src']} is {size}, declared {shot.get('sizes')}")
            continue
        w, h = size
        if not (320 <= min(w, h) and max(w, h) <= 3840 and max(w, h) / min(w, h) <= 2.3):
            problems.append(f"manifest: {shot['src']} size {w}x{h} is outside what browsers accept")
        if {"wide": w <= h, "narrow": w >= h}.get(shot.get("form_factor"), False):
            problems.append(f"manifest: {shot['src']} does not fit form_factor {shot['form_factor']}")
        if not shot.get("label"):
            problems.append(f"manifest: {shot['src']} has no label")
    return problems


def safe_out_dir(out, repo):
    """Refuse to wipe anything that is not a subdirectory of the repo."""
    out_r, repo_r = os.path.realpath(out), os.path.realpath(repo)
    if out_r == repo_r or not out_r.startswith(repo_r + os.sep):
        raise PackageError(f"--out must be a subdirectory of {repo_r} (got {out_r})")
    return out_r


def package(build, web, out, version, repo, today=None):
    out = safe_out_dir(out, repo)
    for name in ENGINE:
        if not os.path.isfile(os.path.join(build, name)):
            raise PackageError(f"missing {name} in {build} (build first)")
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,40}", version):
        raise PackageError(f"bad version string {version!r}")

    if os.path.exists(out):
        shutil.rmtree(out)
    # Static site: everything in web/ except dotfiles.
    shutil.copytree(web, out, ignore=shutil.ignore_patterns(".*"))
    for name in ENGINE:
        shutil.copy2(os.path.join(build, name), os.path.join(out, name))

    names = {}
    for name in HASHED:
        src = os.path.join(out, name)
        names[name] = hashed_name(name, digest(src))
        os.replace(src, os.path.join(out, names[name]))

    # index.html
    p = os.path.join(out, "index.html")
    html = read(p)
    html = replace_once(html, 'href="style.css"', f'href="{names["style.css"]}"', "index.html")
    for name in ("i18n.js", "app.js"):
        html = replace_once(html, f'src="{name}"', f'src="{names[name]}"', "index.html")
    for name in ENGINE:
        html = replace_once(html, f'<link rel="preload" href="{name}"',
                            f'<link rel="preload" href="{names[name]}"', "index.html")
    m = re.search(r'(<script id="build-info" type="application/json">)(.*?)(</script>)', html, re.S)
    if not m:
        raise PackageError("index.html: build-info block not found")
    info = json.loads(m.group(2))
    info.update(build=version, js=names["tornado.js"], wasm=names["tornado.wasm"], data=names["tornado.data"])
    html = html[:m.start(2)] + json.dumps(info, separators=(",", ":")) + html[m.end(2):]
    write(p, html)

    # 404.html (absolute URLs: it is served at any depth)
    p = os.path.join(out, "404.html")
    write(p, replace_once(read(p), 'href="/style.css"', f'href="/{names["style.css"]}"', "404.html"))

    # sw.js
    p = os.path.join(out, "sw.js")
    assets = [names[n] for n in HASHED] + list(PRECACHE_EXTRA)
    sw = read(p)
    sw = replace_once(sw, "var VERSION = 'dev';", f"var VERSION = {json.dumps(version)};", "sw.js")
    sw = replace_once(sw, "var ASSETS = [];", f"var ASSETS = {json.dumps(assets)};", "sw.js")
    write(p, sw)

    # sitemap.xml
    p = os.path.join(out, "sitemap.xml")
    day = (today or datetime.date.today()).isoformat()
    write(p, replace_once(read(p), "</loc>", f"</loc>\n        <lastmod>{day}</lastmod>", "sitemap.xml"))

    verify(out, assets)

    # Readable by the web server whatever the umask was.
    os.chmod(out, 0o755)
    for root, dirs, files in os.walk(out):
        for d in dirs:
            os.chmod(os.path.join(root, d), 0o755)
        for f in files:
            os.chmod(os.path.join(root, f), 0o644)
    return names


def verify(out, assets):
    problems = []
    for page in ("index.html", "404.html"):
        html = read(os.path.join(out, page))
        for ref in local_refs(html):
            if not os.path.isfile(os.path.join(out, ref.lstrip("/"))):
                problems.append(f"{page}: {ref} does not exist")
    info = json.loads(re.search(r'id="build-info"[^>]*>(.*?)</script>', read(os.path.join(out, "index.html")), re.S).group(1))
    for key in ("js", "wasm", "data"):
        if not os.path.isfile(os.path.join(out, info[key])):
            problems.append(f"build-info: {info[key]} does not exist")
    manifest = json.loads(read(os.path.join(out, "manifest.webmanifest")))
    for icon in manifest.get("icons", []):
        path = os.path.join(out, icon["src"])
        if not os.path.isfile(path):
            problems.append(f"manifest: {icon['src']} does not exist")
        elif icon.get("sizes", "any") != "any":
            declared = tuple(int(x) for x in icon["sizes"].split("x"))
            if image_size(path) != declared:
                problems.append(f"manifest: {icon['src']} is {image_size(path)}, declared {icon['sizes']}")
    problems += check_screenshots(out, manifest)
    for a in assets:
        if not os.path.isfile(os.path.join(out, a)):
            problems.append(f"sw.js: precached {a} does not exist")
    for leftover in HASHED:
        if os.path.exists(os.path.join(out, leftover)):
            problems.append(f"unhashed {leftover} left behind")
    if problems:
        raise PackageError("verification failed:\n  " + "\n  ".join(problems))


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", default=os.path.join(repo, "build-wasm"))
    ap.add_argument("--web", default=os.path.join(repo, "web"))
    ap.add_argument("--out", default=os.path.join(repo, "dist"))
    ap.add_argument("--version", required=True)
    a = ap.parse_args()
    try:
        names = package(a.build, a.web, a.out, a.version, repo)
    except PackageError as e:
        sys.exit(f"[package] {e}")
    total = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(a.out) for f in fs)
    for k, v in names.items():
        print(f"  {k:<14} -> {v}")
    print(f"[package] {a.out}: build {a.version}, {total // 1024} KiB")


if __name__ == "__main__":
    main()
