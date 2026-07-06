#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
# Build & deploy Tornado 3D to the VPS web root.
#
# Nginx serves tornado.js/.wasm/.data with "Cache-Control: immutable,
# max-age=1y", so this script stamps the build version (git short hash)
# into index.html — the ?v= query string forces browsers to fetch the
# new assets after every deploy.
#
# Usage:
#   source ~/emsdk/emsdk_env.sh   # once per shell
#   ./deploy_vps.sh
#
# Environment:
#   DEST=/var/www/tornado.micutu.com   # override the deploy target
# ─────────────────────────────────────────────────────────────────────
set -e

BUILD_DIR="build-wasm"
DEST="${DEST:-/var/www/tornado.micutu.com}"

if ! command -v emcmake &>/dev/null; then
    echo "[EROARE] Emscripten SDK nu este in PATH. Ruleaza: source ~/emsdk/emsdk_env.sh"
    exit 1
fi

echo "[INFO] Build WASM..."
emcmake cmake -B "$BUILD_DIR" -S . -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"

echo "[INFO] Rulare teste..."
ctest --test-dir "$BUILD_DIR" --output-on-failure

V=$(git rev-parse --short HEAD 2>/dev/null || date +%s)

echo "[INFO] Deploy versiunea $V -> $DEST"
mkdir -p "$DEST"
cp "$BUILD_DIR/tornado.js" "$BUILD_DIR/tornado.wasm" "$BUILD_DIR/tornado.data" "$DEST/"
# Patterns cover both the raw shell and Emscripten's minified output
sed -e "s|var TORNADO_BUILD = '';|var TORNADO_BUILD = '$V';|" \
    -e "s|TORNADO_BUILD=\"\"|TORNADO_BUILD=\"$V\"|" \
    -e "s|src=\"tornado.js\"|src=\"tornado.js?v=$V\"|" \
    -e "s|src=tornado.js|src=\"tornado.js?v=$V\"|" \
    "$BUILD_DIR/tornado.html" > "$DEST/index.html"

if ! grep -q "tornado.js?v=$V" "$DEST/index.html"; then
    echo "[EROARE] Stamparea versiunii a esuat — verifica sed-urile din deploy_vps.sh"
    exit 1
fi

echo "[OK] Deploy complet: https://tornado.micutu.com (build $V)"
