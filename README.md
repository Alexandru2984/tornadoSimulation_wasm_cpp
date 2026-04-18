# Tornado 3D — C++ / OpenGL / WebAssembly

Simulare 3D interactiva a unei tornade cu particule, case, copaci si modele glTF.
Ruleaza nativ pe desktop (OpenGL 3.3) si in browser (WebGL 2 prin Emscripten/WASM).

## Functionalitati

- Mesh tornado procedural cu animatie swirl
- Sistem de particule cu praf interior si debris exterior (2200 particule)
- Scena cu sol, case, copaci (generate procedural) si modele glTF
- Iluminare directionala cu specular si Fresnel
- Camera cu WASD + mouse look (click dreapta)
- Tornada urmareste pozitia mouse-ului
- Build dual: nativ (OpenGL 3.3) si browser (WebGL 2 / WASM)

## Cerinte

- **CMake** >= 3.14
- **Compilator C++17** (g++, clang++)
- **Emscripten SDK** (pentru build WASM) — [Instalare](https://emscripten.org/docs/getting_started/downloads.html)
- Conexiune internet la prima configurare (CMake descarca GLFW, GLAD, GLM via FetchContent)

## Build & Run — Desktop (Nativ)

```bash
# Varianta 1: script
./run.sh build
./run.sh run

# Varianta 2: manual
cmake -B build -S .
cmake --build build -j$(nproc)
./build/tornado
```

### Controluri
| Input | Actiune |
|-------|---------|
| Mouse | Controleaza pozitia tornadei |
| Click dreapta + drag | Roteste camera |
| W/A/S/D | Misca camera |

## Build & Run — WebAssembly (Browser)

### 1. Instaleaza Emscripten SDK

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

### 2. Compileaza

```bash
# Varianta 1: script dedicat
./build_wasm.sh build

# Varianta 2: manual
emcmake cmake -B build-wasm -S . -DCMAKE_BUILD_TYPE=Release
emmake cmake --build build-wasm -j$(nproc)
```

### 3. Testeaza in browser

```bash
# Varianta 1: script (porneste server automat)
./build_wasm.sh serve

# Varianta 2: cu emrun
emrun --no_browser --port 8080 build-wasm/tornado.html

# Varianta 3: cu Python
cd build-wasm && python3 -m http.server 8080
```

Apoi deschide **http://localhost:8080/tornado.html** in browser.

> **Nota:** WebGL 2 este necesar. Chrome, Firefox si Edge sunt suportate. Safari are suport partial.

## Teste

```bash
# Build si run teste
cmake --build build --target test_math
./build/test_math

# Sau cu CTest
cd build && ctest --output-on-failure
```

Testele acopera:
- Operatii GLM (vectori, matrice, transformari)
- Logica de simulare (interpolare tornado, vortex, damping, camera)
- Adaptarea GLSL (patch #version 330 core -> 300 es)

## Structura proiect

```
├── CMakeLists.txt          # Build system (dual: nativ + Emscripten)
├── build_wasm.sh           # Script build WASM
├── run.sh                  # Script build & run nativ
├── web/
│   └── shell.html          # Template HTML pentru WASM
├── src/
│   ├── main.cpp            # Aplicatia principala (AppState + main_loop)
│   ├── gltf_loader.h       # Loader glTF simplu
│   ├── tinygltf_impl.cpp   # Implementare tinygltf + stb_image
│   └── test_math.cpp       # Teste unitare (21 teste)
├── shaders/
│   ├── vertex.glsl         # Vertex shader principal (swirl)
│   ├── fragment.glsl       # Fragment shader (obiecte + tornado)
│   ├── particle_vertex.glsl
│   └── particle_fragment.glsl
├── assets/models/           # Modele glTF (BoxTextured, Avocado)
└── vendor/                  # Dependinte header-only (tinygltf, stb, json)
```

## Arhitectura portarii desktop -> WASM

### Problema principala
Emscripten nu suporta bucla infinita `while(!shouldClose)` — browserul controleaza frame-urile.
Solutia: `emscripten_set_main_loop()` apeleaza o functie callback la fiecare frame.

### Modificari cheie

| Component | Desktop | Emscripten/WASM |
|-----------|---------|-----------------|
| GL loader | GLAD | Nu e necesar (ES nativ) |
| GL version | OpenGL 3.3 Core | OpenGL ES 3.0 / WebGL 2 |
| GLFW | Compilat din sursa | Port Emscripten (-sUSE_GLFW=3) |
| Shadere | `#version 330 core` | `#version 300 es` + precision |
| Main loop | `while()` | `emscripten_set_main_loop()` |
| Fisiere | Filesystem real | Virtual FS (--preload-file) |
| Point size | `glEnable(GL_PROGRAM_POINT_SIZE)` | Implicit activat |

### Cum functioneaza adaptarea shaderelor
Shaderele raman scrise in GLSL 330 core. La incarcare, functia
`adaptShaderSource()` face patch la runtime:
- Inlocuieste `#version 330 core` cu `#version 300 es`
- Adauga `precision highp float;` pentru fragment shaders

### State management
Toate resursele GL si starea simularii sunt intr-un `struct AppState` global,
accesibil din `main_loop()`. Setup-ul se face o singura data in `main()`.

## Debugging & Optimizare WASM

### Debugging
- Adauga `-g` la build: `emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Debug`
- Chrome DevTools → Sources → poti pune breakpoints direct in C++ (source maps)
- Console.log: `std::cout` si `std::cerr` apar in consola browser-ului
- Verifica erori WebGL: Chrome → `about:flags` → activeaza "WebGL Developer Extensions"

### Profiling
- Chrome DevTools → Performance tab → inregistreaza un frame
- Emscripten: compileaza cu `--profiling` pentru simboluri in profiler

### Optimizare
- Release build: `-O2` sau `-O3` (setat automat cu `-DCMAKE_BUILD_TYPE=Release`)
- Reduce `MAX_PARTICLES` daca FPS-ul e scazut pe mobile
- Fa batch draw calls: minimizeaza schimbarile de stare GL intre obiecte
- Texturile procedurale sunt mici (32x32) — intentionat pentru performanta

## Curatare

```bash
# Sterge build nativ
./run.sh clean

# Sterge build WASM
./build_wasm.sh clean

# Sterge ambele
rm -rf build/ build-wasm/
```

## Licenta

Proiect educational / demo.
