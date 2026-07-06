# Tornado 3D — C++ / OpenGL / WebAssembly

Interactive 3D tornado destruction game with an infinite procedural world.
Runs natively on desktop (OpenGL 3.3) and in the browser (WebGL 2 via Emscripten/WASM).

## Features

- Procedural tornado mesh with swirl animation + particle system (2200 particles)
- Infinite procedurally generated world: terrain heightmap, water, houses, trees, fences, cars, poles
- Wave-based gameplay: 10 waves with EF0–EF5 tornado scales, combo multiplier, power-ups
- Endless mode after victory (press E), cows that wander and flee from the tornado
- Victory and game-over conditions, score sharing
- Local + global leaderboard (tiny SQLite API served from the VPS under `/api/`)
- Day/night cycle, storm weather (rain, lightning + thunder), camera shake
- Web Audio procedural sound effects (wind, destruction, thunder, jingles)
- HUD with minimap, compass, wave progress — drawn in a single batched call
- Mobile touch controls (virtual joystick + camera look), quality presets, persisted settings
- Dual build: native (OpenGL 3.3) and browser (WebGL 2 / WASM)

## Requirements

- **CMake** >= 3.14
- **C++17 compiler** (g++, clang++)
- **Emscripten SDK** (for WASM build) — [Installation](https://emscripten.org/docs/getting_started/downloads.html)
- Internet connection on first configure (CMake downloads GLFW, GLAD, GLM via FetchContent)

## Build & Run — Desktop (Native)

```bash
# Option 1: script
./run.sh build
./run.sh run

# Option 2: manual
cmake -B build -S .
cmake --build build -j$(nproc)
./build/tornado
```

### Controls
| Input | Action |
|-------|--------|
| Mouse | Controls the tornado position |
| Right click + drag | Rotate camera |
| W/A/S/D | Move camera |
| P / Escape | Pause (web) |
| M | Mute (web) |
| R | Restart (double press mid-game, web) |
| C | Share/copy score (web) |
| E | Endless mode after victory |
| F | Fullscreen (web) |

## Build & Run — WebAssembly (Browser)

### 1. Install Emscripten SDK

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

### 2. Compile

```bash
# Option 1: dedicated script
./build_wasm.sh build

# Option 2: manual
emcmake cmake -B build-wasm -S . -DCMAKE_BUILD_TYPE=Release
emmake cmake --build build-wasm -j$(nproc)
```

### 3. Test in Browser

```bash
# Option 1: script (starts server automatically)
./build_wasm.sh serve

# Option 2: with emrun
emrun --no_browser --port 8080 build-wasm/tornado.html

# Option 3: with Python
cd build-wasm && python3 -m http.server 8080
```

Then open **http://localhost:8080/tornado.html** in your browser.

> **Note:** WebGL 2 is required. Chrome, Firefox and Edge are supported. Safari has partial support.

## Tests

```bash
# Build and run tests
cmake --build build --target test_math
./build/test_math

# Or with CTest
cd build && ctest --output-on-failure
```

Tests cover:
- GLM operations (vectors, matrices, transforms)
- Simulation logic (tornado interpolation, vortex, damping, camera)
- GLSL adaptation (patch #version 330 core -> 300 es)

## Deploy (VPS)

```bash
source ~/emsdk/emsdk_env.sh
./deploy_vps.sh    # builds, runs tests, stamps the version and copies to /var/www
```

The script stamps the git hash into `index.html` (`?v=<hash>` on `tornado.js/.wasm/.data`),
so the long-lived immutable HTTP cache is busted on every deploy.

## Project Structure

```
├── CMakeLists.txt          # Build system (dual: native + Emscripten)
├── build_wasm.sh           # WASM build script
├── deploy_vps.sh           # Build + cache-busted deploy to the VPS web root
├── run.sh                  # Native build & run script
├── web/
│   └── shell.html          # HTML template for WASM (UI, touch controls, leaderboard)
├── src/
│   ├── main.cpp            # Main application (AppState + main_loop)
│   ├── constants.h         # All gameplay/rendering tunables
│   ├── terrain_noise.h     # Hash/FBM noise + terrain height function
│   ├── audio_web.h         # Web Audio EM_JS effects + desktop no-ops
│   ├── font5x7.h           # HUD bitmap font glyphs
│   └── test_math.cpp       # Unit tests (21 tests)
├── server/
│   ├── leaderboard.py      # Global leaderboard API (stdlib + SQLite)
│   └── tornado-leaderboard.service  # systemd unit (nginx proxies /api/ -> :8791)
└── shaders/                # GLSL: scene, particles, sky, rain, HUD
```

## Porting Architecture: Desktop -> WASM

### Main Problem
Emscripten does not support an infinite `while(!shouldClose)` loop — the browser controls frame timing.
Solution: `emscripten_set_main_loop()` calls a callback function each frame.

### Key Changes

| Component | Desktop | Emscripten/WASM |
|-----------|---------|-----------------|
| GL loader | GLAD | Not needed (native ES) |
| GL version | OpenGL 3.3 Core | OpenGL ES 3.0 / WebGL 2 |
| GLFW | Compiled from source | Emscripten port (-sUSE_GLFW=3) |
| Shaders | `#version 330 core` | `#version 300 es` + precision |
| Main loop | `while()` | `emscripten_set_main_loop()` |
| Files | Real filesystem | Virtual FS (--preload-file) |
| Point size | `glEnable(GL_PROGRAM_POINT_SIZE)` | Enabled by default |

### How Shader Adaptation Works
Shaders are written in GLSL 330 core. At load time, the
`adaptShaderSource()` function patches them at runtime:
- Replaces `#version 330 core` with `#version 300 es`
- Adds `precision highp float;` for fragment shaders

### State Management
All GL resources and simulation state live in a global `struct AppState`,
accessible from `main_loop()`. Setup is done once in `main()`.

## Debugging & WASM Optimization

### Debugging
- Add `-g` to the build: `emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Debug`
- Chrome DevTools → Sources → you can set breakpoints directly in C++ (source maps)
- Console.log: `std::cout` and `std::cerr` appear in the browser console
- Check WebGL errors: Chrome → `about:flags` → enable "WebGL Developer Extensions"

### Profiling
- Chrome DevTools → Performance tab → record a frame
- Emscripten: compile with `--profiling` for symbols in the profiler

### Optimization
- Release build: `-O2` or `-O3` (set automatically with `-DCMAKE_BUILD_TYPE=Release`)
- Reduce `MAX_PARTICLES` if FPS is low on mobile
- Batch draw calls: minimize GL state changes between objects
- Procedural textures are small (32x32) — intentional for performance

## Cleanup

```bash
# Delete native build
./run.sh clean

# Delete WASM build
./build_wasm.sh clean

# Delete both
rm -rf build/ build-wasm/
```

## License

Educational / demo project.
