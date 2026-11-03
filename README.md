# Tornada 3D — C++ / OpenGL / WebAssembly

Interactive 3D tornado destruction game with an infinite procedural world.
Runs natively on desktop (OpenGL 3.3) and in the browser (WebGL 2 via
Emscripten/WASM). Live at **https://tornado.micutu.com**.

[![CI](https://github.com/Alexandru2984/tornadoSimulation_wasm_cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/Alexandru2984/tornadoSimulation_wasm_cpp/actions/workflows/ci.yml)

![Tornada 3D: the tornado tearing through a village](web/screenshots/gameplay-wide.webp)

## Features

**Game**
- Procedural tornado mesh with swirl animation + particle system
- Infinite procedurally generated world: terrain, water, houses, trees, fences, cars, poles
- 10 waves with EF0–EF5 tornado scales, combo multiplier up to ×5, endless mode after victory
- Time Attack (3-minute score chase) and 3 difficulty levels, locked for the whole run
- Daily challenge: a different world every UTC day (seeded terrain and objects), the same
  start, light and power-up sequence for everyone, with its own daily leaderboard
- 6 power-ups (speed, size ×2, magnet, shield, score ×2, freeze) that expire if ignored
- Cows, sheep, cars and birds that flee; day/night cycle; rain, lightning and thunder
- Procedural Web Audio sound with a master volume

**Web**
- Full-screen responsive UI (desktop, phone portrait/landscape, notch safe areas)
- Touch joystick + look, keyboard, mouse and gamepad; reduced-motion option
- Pause on tab switch, wake lock while playing, fullscreen + landscape lock
- Global leaderboard (classic / time attack × difficulty × all-time / month / week / day),
  local history, score sharing, portable player profile code
- Player profile: lifetime statistics and 21 achievements (per run, cumulative, streaks,
  leaderboard placings), unlocked live on the results screen
- Romanian and English (page and in-game HUD), picked from the browser's languages and
  switchable in Settings; the HTML stays Romanian for search engines
- Installable app (manifest, icons, gameplay screenshots for the install dialog) that also
  plays offline after the first visit
- SEO: canonical URL, Open Graph / X preview card, JSON-LD, sitemap, robots, real 404

**Engineering**
- Strict Content-Security-Policy (no inline script or style), security headers on every response
- Leaderboard API v2 with server-issued run tokens and plausibility checks
- Content-hashed assets, atomic deploys with smoke test and one-command rollback
- CI: native + WASM builds, unit tests, packaging tests, lint, headless-browser smoke test

## Requirements

- **CMake** ≥ 3.14 (4.x works) and a **C++17** compiler
- **Emscripten SDK** for the web build (CI and production use 6.0.13)
- **Python** ≥ 3.10 for the leaderboard service and tools (standard library only)
- Internet on first configure: CMake fetches GLFW, glad and GLM as SHA-256-pinned tarballs
- Native build on Linux needs X11/GL headers (`sudo apt install xorg-dev libgl-dev`)

## Desktop (native)

```bash
./run.sh build && ./run.sh run
# or
cmake -B build -S . && cmake --build build -j"$(nproc)" && ./build/tornado
```

Configure with `-DTORNADO_BUILD_GAME=OFF` to build only the unit tests (no GLFW/X11 needed).

## Web (WebAssembly)

```bash
source ~/emsdk/emsdk_env.sh
./build_wasm.sh build        # -> build-wasm/ (index.html, app.js, style.css, tornado.*)
./build_wasm.sh serve        # http://localhost:8080, production headers incl. CSP
```

For the leaderboard locally, run the API in a second terminal and allow the dev origin:

```bash
TORNADO_ORIGINS=http://localhost:8080 TORNADO_DB=/tmp/scores.db python3 server/leaderboard.py
```

`tools/dev_server.py` reads the security headers from `server/nginx/security-headers-tornado.conf`,
so a CSP violation shows up locally exactly as it would in production.
`./build_wasm.sh dist` builds the deployable package (hashed names, service worker active);
serve it with `python3 tools/dev_server.py dist`.

The page is `web/index.html` (plain static file) plus `web/app.js`, which checks WebGL 2 support
and then loads `tornado.js`. C++ ⇄ JS talk through exported functions (`start_run`, `set_paused`,
`get_run_stat`, …) and `tornado:runstart` / `tornado:runend` DOM events (`src/web_bridge.h`).

### Controls

| Input | Action |
|-------|--------|
| Mouse / right finger | Steer the tornado |
| Right click + drag | Rotate the camera |
| W/A/S/D, arrows / left finger | Move the camera |
| P or Esc | Pause |
| M | Sound on/off |
| F | Fullscreen |
| R ×2 | Restart the run |
| C | Share the score |
| Gamepad | Left stick moves the camera, right stick steers, LB/RB turn, A plays, Start pauses |

### Languages

The page is written in Romanian. English comes from `web/i18n.js`, a dictionary keyed by the
Romanian text: `app.js` translates the static page in place and passes the strings it builds
through `t('…')`. The in-game HUD text lives in `src/i18n.h` (ASCII only, the bitmap font has
no diacritics) and follows `set_language()` from the page, or `$LANG` on desktop.

When adding UI text, write it in Romanian and add the English entry; `tools/test_i18n.py`
fails on any string without one, on mismatched `{placeholders}` and on unused entries.
Search engines always get the Romanian page.

## Tests

```bash
ctest --test-dir build --output-on-failure      # C++ (math, terrain, constants, HUD text)
python3 server/test_leaderboard.py              # leaderboard API (anti-forgery, limits, migration)
python3 server/test_lbadmin.py                  # moderation CLI
python3 tools/test_package_site.py              # deploy packaging
python3 tools/test_i18n.py                      # English dictionary covers every page string
cd tests/e2e && npm ci && npx playwright install chromium && node smoke.mjs ../../dist
```

The browser smoke test boots the packaged site in headless Chromium (SwiftShader) under the
production CSP and fails on any console error or failed request. CI runs all of the above
(`.github/workflows/ci.yml`).

`node screenshots.mjs ../../dist` (same directory) plays a real run and regenerates
`web/screenshots/`; the packager checks that their sizes match the manifest.

## Leaderboard API (v2)

`server/leaderboard.py` — Python standard library + SQLite (WAL), listening on 127.0.0.1:8791
behind nginx.

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/v2/health` | liveness |
| POST | `/api/v2/runs` | `{mode, difficulty}` → `{run}`: one-time token issued when a run starts; daily runs also send `day` (UTC) |
| POST | `/api/v2/scores` | `{run, name, score, wave, player, stats}` → `{rank}` |
| GET | `/api/v2/scores?mode&difficulty&period&limit` | best score per name |
| GET | `/api/v2/scores?mode=daily&day=YYYY-MM-DD` | one day's daily challenge (default today, UTC) |
| GET | `/api/v2/stats` | global totals (cached 60 s) |
| GET | `/api/scores` | legacy read-only list; legacy POST answers 410 |

A score is accepted only with an unused, unexpired run token of the same mode, and only if it is
plausible for the elapsed server time (points per object and combo limits mirror
`src/constants.h`; a test keeps them in sync). Names are claimed per device with a secret player
key (export/import it under Settings). Rate limits are per client (IPv6 grouped by /64), and
browser POSTs from other origins are refused (CSRF).

The daily challenge's world comes from a hash of the UTC date, computed by the page; the server
only checks that the day a daily run claims is its own (±5 minutes around midnight) and ranks the
run on that day's board, even if it is submitted after midnight.

These checks make forging a score much harder; they cannot make it impossible, because the game
runs on the player's machine. Treat the leaderboard as a fun ranking, not proof.

Environment: `TORNADO_DB`, `TORNADO_HOST`, `TORNADO_PORT`, `TORNADO_ORIGINS`.

### Moderation

`server/lbadmin.py`, installed with a wrapper as `tornado-lbadmin` (runs as the database owner):

```bash
tornado-lbadmin top -m classic -d 1       # board with time, points/second, device id
tornado-lbadmin top -m daily --day 2026-11-03   # one day's daily challenge
tornado-lbadmin recent -n 30              # latest submissions
tornado-lbadmin show 123                  # one score + what else that device sent
tornado-lbadmin delete 123 124
tornado-lbadmin ban --score 123 --reason "impossible score"   # name(s) + device, scores removed
tornado-lbadmin ban --name SomeName --keep-scores
tornado-lbadmin bans | unban --name X | unban --player HASH
tornado-lbadmin release SomeName          # free a name whose owner lost the profile code
```

A ban covers the name, the device (player key) that claimed it and every other name that
device claimed; banned submissions get `403 banned`. Someone who clears their browser data
gets a new device key, so bans slow cheaters down rather than stop them for good.

## Production (VPS)

```
Cloudflare → cloudflared (tunnel) → nginx :443 ─┬─ /var/www/tornado.micutu.com → tornado-releases/release-…
                                               └─ /api/ → 127.0.0.1:8791 (tornado-leaderboard.service)
```

| File in repo | Installed as |
|--------------|--------------|
| `server/nginx/tornado.micutu.com.conf` | `/etc/nginx/sites-enabled/tornado.micutu.com` |
| `server/nginx/security-headers-tornado.conf` | `/etc/nginx/snippets/` |
| `server/nginx/tornado-rate-limit.conf` | `/etc/nginx/conf.d/` |
| `server/tornado-leaderboard.service` | `/etc/systemd/system/` (sandboxed `tornadolb` user) |
| `server/leaderboard.py`, `server/lbadmin.py` | `/opt/tornado-leaderboard/` (synced by the deploy script) |
| `server/tornado-lbadmin` | `/usr/local/sbin/` (synced by the deploy script) |
| `server/backup_scores.sh` | `/usr/local/sbin/tornado-leaderboard-backup` |
| `server/tornado-leaderboard-backup.{service,timer}` | `/etc/systemd/system/` (daily, 14 days kept) |

### Deploy

```bash
source ~/emsdk/emsdk_env.sh
./deploy_vps.sh            # build, test, package, deploy (refuses uncommitted changes)
./deploy_vps.sh list       # releases, * = live
./deploy_vps.sh rollback   # back to the previous release
```

Each deploy writes a new root-owned release to `/var/www/tornado-releases/`, swaps the site
symlink atomically, then smoke-tests through nginx (build id, hashed files, wasm MIME type,
uncached `sw.js`, API health) and swaps back if anything fails. If `server/leaderboard.py`
changed, the database is backed up first and the previous code is restored if the health
check fails. Asset names carry a content hash, so nothing needs purging on Cloudflare.

## Project structure

```
├── CMakeLists.txt            # native + Emscripten build, pinned dependencies
├── build_wasm.sh             # build | dist | serve | clean
├── deploy_vps.sh             # atomic deploy / rollback on the VPS
├── run.sh                    # native build & run
├── src/
│   ├── main.cpp              # game (AppState + main loop)
│   ├── constants.h           # gameplay/rendering tunables (shared with the API checks)
│   ├── web_bridge.h          # run lifecycle events + stats for the web UI
│   ├── audio_web.h           # Web Audio effects (no-ops on desktop)
│   ├── i18n.h                # HUD text, Romanian + English
│   ├── terrain_noise.h, font5x7.h
│   └── test_math.cpp         # unit tests
├── shaders/                  # GLSL 330 core, patched to GLSL ES 300 for WebGL
├── web/                      # static site: index.html, app.js, i18n.js, style.css, sw.js,
│                             # manifest, icons, screenshots, robots.txt, sitemap.xml, 404.html
├── server/                   # leaderboard API + tests, systemd units, backup, nginx/
├── tools/                    # dev_server.py, package_site.py (+ tests), make_icons.js
├── tests/e2e/                # Playwright smoke test + screenshot tool (locked dependencies)
└── .github/                  # CI workflow, Dependabot
```

## Porting notes: desktop → WASM

| Component | Desktop | Emscripten/WASM |
|-----------|---------|-----------------|
| GL loader | glad | not needed |
| GL version | OpenGL 3.3 Core | OpenGL ES 3.0 / WebGL 2 |
| GLFW | built from source | Emscripten port (`-sUSE_GLFW=3`) |
| Shaders | `#version 330 core` | patched at load time to `#version 300 es` + precision |
| Main loop | `while()` | `emscripten_set_main_loop()` |
| Files | real filesystem | virtual FS (`--preload-file shaders`) |

Debugging: build with `-DCMAKE_BUILD_TYPE=Debug` for source maps in Chrome DevTools;
`--profiling` keeps symbols for the Performance panel. Lower the quality preset (Settings) if
FPS is low on old phones; the web UI also steps quality down automatically.

## Security

See [SECURITY.md](SECURITY.md) for how to report a vulnerability and what the deployment does
to protect players and the server.

## License

No license file yet: all rights reserved by the author unless one is added.
