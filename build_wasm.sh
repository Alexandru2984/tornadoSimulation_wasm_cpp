#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
# Build Tornado 3D for WebAssembly (Emscripten)
# Usage:
#   ./build_wasm.sh [build|clean|serve|help]
# ─────────────────────────────────────────────────────────────────────
set -e

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
    echo "  -> tornado.html, tornado.js, tornado.wasm, tornado.data"
    echo ""
    echo "  Pentru a testa: ./build_wasm.sh serve"
}

function clean() {
    echo "[INFO] Se sterge $BUILD_DIR/..."
    rm -rf "$BUILD_DIR"
    echo "[OK] Curat."
}

function serve() {
    if [ ! -f "$BUILD_DIR/tornado.html" ]; then
        echo "[INFO] Build-ul nu exista. Se compileaza mai intai..."
        build
    fi
    echo "[INFO] Server local pe http://localhost:$PORT/tornado.html"
    echo "  (Ctrl+C pentru oprire)"
    cd "$BUILD_DIR"

    # Try emrun first (has COOP/COEP support built-in)
    if command -v emrun &>/dev/null; then
        emrun --no_browser --port "$PORT" tornado.html
    elif command -v python3 &>/dev/null; then
        # Custom server with COOP/COEP headers required for SharedArrayBuffer
        python3 - <<'PYEOF'
import http.server, sys, os
port = int(os.environ.get('PORT', 8080))
class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        super().end_headers()
    def log_message(self, fmt, *args):
        print(f'  {self.address_string()} {fmt % args}', flush=True)
print(f'[Server] Listening on http://localhost:{port}', flush=True)
http.server.HTTPServer(('', port), Handler).serve_forever()
PYEOF
    else
        echo "[EROARE] Nu s-a gasit emrun sau python3 pentru server local."
        exit 1
    fi
}

function help() {
    echo "Build & serve Tornado 3D (WebAssembly/Emscripten)"
    echo ""
    echo "Optiuni:"
    echo "  build    - Compileaza pentru WASM (implicit)"
    echo "  clean    - Sterge directorul de build WASM"
    echo "  serve    - Porneste server local si deschide in browser"
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
