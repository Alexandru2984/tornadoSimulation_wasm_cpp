#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
# Build, test and deploy Tornada 3D on this VPS — atomic, with rollback.
#
#   ./deploy_vps.sh             build + test + package + deploy a release
#   ./deploy_vps.sh rollback    point the site back at the previous release
#   ./deploy_vps.sh list        show releases (* = live)
#
# Layout (same pattern as the other sites on this box):
#   /var/www/tornado-releases/release-<UTC>-<commit>/   immutable, root-owned
#   /var/www/tornado.micutu.com -> one of the above     swapped with rename(2)
#
# A deploy never edits files the live site is serving: the new release is
# written next to the old one and the symlink is replaced atomically, then
# smoke-tested through nginx; a failed check swaps straight back.
# Asset names carry a content hash (tools/package_site.py), so browsers
# and Cloudflare can cache them forever and no purge is needed.
#
# The leaderboard code is synced to /opt first (DB backup → install →
# restart → health check → restore the old code if the check fails).
#
# Environment:
#   SITE_LINK=/var/www/tornado.micutu.com    RELEASES=/var/www/tornado-releases
#   KEEP=5                 releases kept (the live and previous one always are)
#   PUBLIC_HOST=tornado.micutu.com           host used for the smoke test
#   LB_UNIT=<auto>         leaderboard systemd unit (auto-detected)
#   LB_PORT=8791           SKIP_LB=1 skips the leaderboard sync
#   ALLOW_DIRTY=1          deploy uncommitted changes (build id gets -dirty)
#   SKIP_BUILD=1           reuse build-wasm/ as is (still tested and packaged)
# Needs: emsdk in PATH (source ~/emsdk/emsdk_env.sh), passwordless sudo.
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

SITE_LINK="${SITE_LINK:-/var/www/tornado.micutu.com}"
RELEASES="${RELEASES:-/var/www/tornado-releases}"
KEEP="${KEEP:-5}"
PUBLIC_HOST="${PUBLIC_HOST:-tornado.micutu.com}"
LB_PORT="${LB_PORT:-8791}"
LB_DST="/opt/tornado-leaderboard/leaderboard.py"
BUILD_DIR="build-wasm"
DIST="dist"

log()  { printf '\033[1;34m[deploy]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[ ok ]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

need_sudo() {
    sudo -n true 2>/dev/null || die "needs passwordless sudo (run 'sudo -v' first)"
}

live_release() {
    if [ -L "$SITE_LINK" ]; then readlink -f "$SITE_LINK"; fi
}

# Releases oldest → newest (names sort by their UTC timestamp).
list_releases() {
    find "$RELEASES" -mindepth 1 -maxdepth 1 -type d \( -name 'release-*' -o -name 'legacy-*' \) 2>/dev/null | sort
}

# rename(2) over the old link: readers see either the old or the new tree.
point_site_at() {
    local target="$1" tmp="${SITE_LINK}.new.$$"
    sudo ln -sfn "$target" "$tmp"
    sudo mv -Tf "$tmp" "$SITE_LINK"
}

# Fetch through the local nginx with the real Host/SNI (what cloudflared does).
fetch() {
    curl -fsS --max-time 10 --resolve "${PUBLIC_HOST}:443:127.0.0.1" "https://${PUBLIC_HOST}$1" "${@:2}"
}

smoke_test() {
    local build="$1" page info
    page="$(fetch / )" || { echo "GET / failed"; return 1; }
    grep -q "\"build\":\"${build}\"" <<<"$page" || { echo "live page is not build ${build}"; return 1; }
    info="$(grep -o '<script id="build-info"[^<]*' <<<"$page" | sed 's/.*>//')"
    local f
    for f in $(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d["js"], d["wasm"], d["data"])' "$info"); do
        fetch "/$f" -o /dev/null || { echo "GET /$f failed"; return 1; }
    done
    fetch "/$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["wasm"])' "$info")" -o /dev/null -w '%{content_type}' \
        | grep -q '^application/wasm' || { echo "wasm served with the wrong MIME type"; return 1; }
    # A service worker cached as "immutable" would pin every visitor to this
    # release for a year: nginx must serve sw.js (and the page) uncached.
    local sw_cc
    sw_cc="$(fetch /sw.js -o /dev/null -D - | tr -d '\r' | grep -i '^cache-control:' || true)"
    grep -qi 'no-cache\|no-store\|max-age=0' <<<"$sw_cc" || { echo "sw.js must be served with Cache-Control: no-cache (got '${sw_cc:-none}')"; return 1; }
    fetch /api/v2/health -o /dev/null || { echo "API health check failed"; return 1; }
}

detect_lb_unit() {
    if [ -n "${LB_UNIT:-}" ]; then echo "$LB_UNIT"; return; fi
    local u
    for u in tornado-leaderboard.service tornado-leaderboard-canary.service; do
        if systemctl is-active --quiet "$u"; then echo "$u"; return; fi
    done
    for u in tornado-leaderboard.service tornado-leaderboard-canary.service; do
        if systemctl cat "$u" >/dev/null 2>&1; then echo "$u"; return; fi
    done
}

lb_healthy() {
    for _ in $(seq 1 20); do
        curl -fsS --max-time 2 "http://127.0.0.1:${LB_PORT}/api/v2/health" >/dev/null 2>&1 && return 0
        sleep 0.5
    done
    return 1
}

sync_leaderboard() {
    if [ "${SKIP_LB:-0}" = 1 ]; then log "leaderboard: skipped (SKIP_LB=1)"; return; fi
    local unit; unit="$(detect_lb_unit)"
    [ -n "$unit" ] || die "no leaderboard unit found (set LB_UNIT or SKIP_LB=1)"
    [ -d "$(dirname "$LB_DST")" ] || die "$(dirname "$LB_DST") missing — install the service first (see README)"
    # Moderation tool + wrapper: no restart needed, just keep them current.
    sudo install -o root -g root -m 0644 server/lbadmin.py "$(dirname "$LB_DST")/lbadmin.py"
    sudo install -o root -g root -m 0755 server/tornado-lbadmin /usr/local/sbin/tornado-lbadmin
    if sudo cmp -s server/leaderboard.py "$LB_DST"; then
        ok "leaderboard: unchanged ($unit)"
        return
    fi
    log "leaderboard: updating $unit"
    if systemctl cat tornado-leaderboard-backup.service >/dev/null 2>&1; then
        log "leaderboard: backing up the database first"
        sudo systemctl start tornado-leaderboard-backup.service || die "database backup failed — not touching the service"
    fi
    if [ -f "$LB_DST" ]; then sudo cp -p "$LB_DST" "$LB_DST.prev"; fi
    sudo install -o root -g root -m 0644 server/leaderboard.py "$LB_DST"
    sudo systemctl restart "$unit"
    if lb_healthy; then
        ok "leaderboard: healthy on :$LB_PORT"
    else
        log "leaderboard: health check failed — restoring the previous code"
        if [ -f "$LB_DST.prev" ]; then
            sudo install -o root -g root -m 0644 "$LB_DST.prev" "$LB_DST"
            sudo systemctl restart "$unit"
        fi
        sudo journalctl -u "$unit" -n 20 --no-pager || true
        die "leaderboard update rolled back"
    fi
}

prune_releases() {
    local live r all=()
    live="$(live_release)"
    mapfile -t all < <(list_releases)
    local n=${#all[@]}
    (( n > KEEP )) || return 0
    for r in "${all[@]:0:n-KEEP}"; do
        [ "$r" = "$live" ] && continue
        log "pruning $(basename "$r")"
        sudo rm -rf --one-file-system -- "$r"
    done
}

cmd_list() {
    local live r; live="$(live_release)"
    list_releases | while read -r r; do
        if [ "$r" = "$live" ]; then echo "* $(basename "$r")"; else echo "  $(basename "$r")"; fi
    done
}

cmd_rollback() {
    need_sudo
    local live prev="" r
    live="$(live_release)"
    [ -n "$live" ] || die "$SITE_LINK is not a release symlink"
    while read -r r; do
        [ "$r" = "$live" ] && break
        prev="$r"
    done < <(list_releases)
    [ -n "$prev" ] || die "no release older than $(basename "$live")"
    point_site_at "$prev"
    ok "site now serves $(basename "$prev") (was $(basename "$live"))"
    log "note: the leaderboard code is not rolled back; $LB_DST.prev holds the previous version"
}

cmd_deploy() {
    need_sudo
    [ "$(id -u)" -ne 0 ] || die "run as your normal user (sudo is used where needed)"
    command -v emcmake >/dev/null || die "Emscripten not in PATH: source ~/emsdk/emsdk_env.sh"

    local rev build
    rev="$(git rev-parse --short HEAD)"
    if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
        [ "${ALLOW_DIRTY:-0}" = 1 ] || die "uncommitted changes (commit them or set ALLOW_DIRTY=1)"
        build="${rev}-dirty"
    else
        build="$rev"
    fi

    if [ "${SKIP_BUILD:-0}" != 1 ]; then
        log "building WASM (Release)"
        emcmake cmake -B "$BUILD_DIR" -S . -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"
    fi
    log "running tests"
    ctest --test-dir "$BUILD_DIR" --output-on-failure
    local t
    for t in server/test_leaderboard.py server/test_lbadmin.py tools/test_package_site.py; do
        python3 "$t" >/dev/null 2>&1 || python3 "$t" || die "$t failed"
    done
    ok "tests passed"

    log "packaging build $build"
    python3 tools/package_site.py --build "$BUILD_DIR" --out "$DIST" --version "$build"

    sync_leaderboard

    local stamp release prev_live
    stamp="$(date -u +%Y%m%dT%H%M%SZ)"
    release="$RELEASES/release-${stamp}-${build}"
    sudo install -d -o root -g root -m 0755 "$RELEASES"
    sudo cp -a "$DIST" "$release.tmp"
    sudo chown -R root:root "$release.tmp"
    sudo mv -T "$release.tmp" "$release"

    prev_live="$(live_release)"
    if [ -e "$SITE_LINK" ] && [ ! -L "$SITE_LINK" ]; then
        # First run: keep the hand-copied site as a release we can roll back to.
        local legacy="$RELEASES/legacy-${stamp}"
        log "moving the existing $SITE_LINK to $(basename "$legacy")"
        sudo mv -T "$SITE_LINK" "$legacy"
        prev_live="$legacy"
    fi
    point_site_at "$release"
    log "live: $(basename "$release") — smoke testing through nginx"

    local why
    if ! why="$(smoke_test "$build" 2>&1)"; then
        if [ -n "$prev_live" ]; then
            point_site_at "$prev_live"
            # Drop the bad release so a later 'rollback' can never land on it.
            sudo rm -rf --one-file-system -- "$release"
            die "smoke test failed ($why) — rolled back to $(basename "$prev_live")"
        fi
        die "smoke test failed ($why) and there is no previous release to go back to"
    fi
    ok "smoke test passed"
    prune_releases
    ok "deployed build $build → https://${PUBLIC_HOST}/"
}

case "${1:-deploy}" in
    deploy)   cmd_deploy ;;
    rollback) cmd_rollback ;;
    list)     cmd_list ;;
    -h|--help|help) sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//' ;;
    *) die "unknown command: $1 (deploy | rollback | list)" ;;
esac
