#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
# Build Tornado 3D for WebAssembly (Emscripten)
# Usage:
#   ./build_wasm.sh [build|clean|serve|help]
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

BUILD_DIR="build-wasm"
PORT="${PORT:-8080}"

function check_emscripten() {
    if ! command -v emcmake &>/dev/null; then
        echo "[EROARE] Emscripten SDK nu este in PATH."
        echo "  Instaleaza: https://emscripten.org/docs/getting_started/downloads.html"
        echo "  Apoi: source emsdk_env.sh"
        exit 1
    fi
}

function build() {
    check_emscripten
    echo "[INFO] Configurare CMake cu Emscripten..."
    emcmake cmake -B "$BUILD_DIR" -S . \
        -DCMAKE_BUILD_TYPE=Release
    echo "[INFO] Compilare..."
    emmake cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"
    echo ""
    echo "[OK] Build complet! Fisiere in $BUILD_DIR/"
    echo "  -> index.html, app.js, style.css, tornado.js, tornado.wasm, tornado.data"
    echo ""
    echo "  Pentru a testa: ./build_wasm.sh serve"
}

function clean() {
    echo "[INFO] Se sterge $BUILD_DIR/..."
    rm -rf "$BUILD_DIR"
    echo "[OK] Curat."
}

function serve() {
    if [ ! -f "$BUILD_DIR/index.html" ] || [ ! -f "$BUILD_DIR/tornado.wasm" ]; then
        echo "[INFO] Build-ul nu exista. Se compileaza mai intai..."
        build
    fi
    # Same headers as production (strict CSP included) + /api/ proxied to the
    # leaderboard on :8791 if it is running (python3 server/leaderboard.py).
    exec python3 "$(dirname "$0")/tools/dev_server.py" "$BUILD_DIR" --port "$PORT"
}

function help() {
    echo "Build & serve Tornado 3D (WebAssembly/Emscripten)"
    echo ""
    echo "Optiuni:"
    echo "  build    - Compileaza pentru WASM (implicit)"
    echo "  clean    - Sterge directorul de build WASM"
    echo "  serve    - Porneste serverul local (headere ca in productie) pe http://localhost:\$PORT"
    echo "  help     - Afiseaza acest mesaj"
    echo ""
    echo "Variabile de mediu:"
    echo "  PORT=8080   - Portul pentru serverul local"
}

case "${1:-build}" in
    build)   build ;;
    clean)   clean ;;
    serve)   serve ;;
    help)    help  ;;
    *)
        echo "[EROARE] Optiune necunoscuta: $1"
        help
        exit 1
        ;;
esac
