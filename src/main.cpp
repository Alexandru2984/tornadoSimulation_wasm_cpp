// =====================================================================
// Tornado 3D simulation — desktop (OpenGL 3.3) & browser (WebGL 2 / Emscripten)
// =====================================================================

// ── Platform includes ────────────────────────────────────────────────
#ifdef PLATFORM_EMSCRIPTEN
  #include <emscripten/emscripten.h>
  #include <emscripten/html5.h>
  #include <GLES3/gl3.h>
  #include <GLFW/glfw3.h>
#else
  #include <glad/glad.h>
  #include <GLFW/glfw3.h>
#endif

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <cmath>
#include <random>
#include <chrono>
#include <string>
#include <cstddef>
#include <unordered_set>
#include <algorithm>
#include <deque>

// For getcwd fallback paths (works on both POSIX & Emscripten)
#ifdef _WIN32
  #include <direct.h>
  #define GET_CWD _getcwd
#else
  #include <unistd.h>
  #include <limits.h>
  #define GET_CWD getcwd
#endif

#include "constants.h"
#include "terrain_noise.h"
#include "audio_web.h"
#include "font5x7.h"

// Uniform location caches
struct MainUniforms {
    GLint proj=-1, view=-1, model=-1, normalMat=-1, time=-1, camPos=-1;
    GLint enableSwirl=-1, tint=-1, opacity=-1, objType=-1, hasAlbedo=-1, albedo=-1;
    GLint lightningFlash=-1, windBend=-1, windSource=-1;
    GLint timeOfDay=-1, waterLevel=-1;
};
struct ParticleUniforms {
    GLint proj=-1, view=-1, model=-1, color=-1, pointScale=-1;
};
struct SkyUniforms {
    GLint lightningFlash=-1, time=-1, timeOfDay=-1;
};
struct RainUniforms {
    GLint proj=-1, view=-1;
};
struct HudUniforms {
    GLint fontTex=-1;
};

// ── Structs ──────────────────────────────────────────────────────────
struct Camera {
    glm::vec3 pos{0.0f, 2.0f, 8.0f};
    float yaw   = -90.0f;
    float pitch  = 0.0f;
    float speed  = 5.0f;
    float sensitivity = 0.12f;
    glm::mat4 getView() const {
        glm::vec3 dir;
        dir.x = cosf(glm::radians(yaw)) * cosf(glm::radians(pitch));
        dir.y = sinf(glm::radians(pitch));
        dir.z = sinf(glm::radians(yaw)) * cosf(glm::radians(pitch));
        return glm::lookAt(pos, pos + glm::normalize(dir), glm::vec3(0, 1, 0));
    }
};

struct Particle { glm::vec3 pos; glm::vec3 vel; float life; };
struct Vertex   { glm::vec3 pos; glm::vec3 normal; glm::vec3 col; };

struct SimpleModel { GLuint vao = 0, vbo = 0, ebo = 0; GLsizei indexCount = 0; };

struct DestructibleHouse {
    glm::vec3 pos;
    float health = 1.0f;
    bool destroyed = false;
    int chunkX = 0, chunkZ = 0;  // which chunk owns this
    int genIdx = 0;              // deterministic index within the chunk
};

struct Debris {
    glm::vec3 pos;
    glm::vec3 vel;
    float rotAngle;
    glm::vec3 rotAxis;
    float angVel;
    float life;
    float size;
    int colorType; // 0=brick, 1=wood, 2=roof
};

struct LightningState {
    float nextFlash = 3.0f;
    float intensity = 0.0f;
};

struct RainParticle {
    glm::vec3 pos;
};

struct ChunkTree {
    glm::vec3 pos;
    int chunkX = 0, chunkZ = 0;
    float health = 1.0f;
    bool destroyed = false;
    int genIdx = 0;
};

// New destructible objects
struct ChunkProp {
    glm::vec3 pos;
    int chunkX = 0, chunkZ = 0;
    float health = 1.0f;
    bool destroyed = false;
    int propType; // 0=fence, 1=car, 2=pole
    float yaw;    // random rotation
    int genIdx = 0;
};

struct ScorchMark {
    glm::vec3 pos;
    float radius;
};

// Cow that wanders around and flees from the tornado
struct ChunkAnimal {
    glm::vec3 pos;
    int chunkX = 0, chunkZ = 0;
    int genIdx = 0;
    float health = 1.0f;
    bool destroyed = false;
    float yaw = 0.0f;          // facing direction
    glm::vec2 wanderDir{1.0f, 0.0f};
    float wanderTimer = 0.0f;  // seconds until a new wander direction
    float speed = 0.0f;        // current movement speed (for the waddle)
};

struct Score {
    int housesDestroyed = 0;
    int treesDestroyed  = 0;
    int propsDestroyed  = 0;
    int totalDestroyed  = 0;
    int scorePoints     = 0;
};

// Game state machine
enum class GamePhase { PLAYING, WAVE_ANNOUNCE, VICTORY, GAME_OVER };

// Wave definition
struct Wave {
    int number      = 1;
    int target      = 5;     // destroys needed to advance
    int destroyed   = 0;     // current wave progress
    float timer     = 0.0f;  // time in this wave
    float announceTimer = 0.0f;
    int efScale     = 0;     // EF0..EF5
};

// Power-up types
enum class PowerUpType { SPEED_BOOST, SIZE_DOUBLE, MAGNET, SHIELD, SCORE_2X };
static const int POWERUP_TYPE_COUNT = 5;
struct PowerUp {
    glm::vec3 pos;
    PowerUpType type;
    float spawnTime = 0.0f;
    bool collected = false;
};

// Active power-up effect
struct ActivePowerUp {
    PowerUpType type;
    float remaining = 0.0f;
};

// Track which chunks are currently loaded
struct ChunkKey {
    int x, z;
    bool operator==(const ChunkKey& o) const { return x==o.x && z==o.z; }
};
struct ChunkKeyHash {
    size_t operator()(const ChunkKey& k) const {
        // Boost-style hash_combine for better distribution than XOR-with-shift.
        size_t h1 = std::hash<int>()(k.x);
        size_t h2 = std::hash<int>()(k.z);
        return h1 ^ (h2 * 2654435761ULL + 0x9e3779b9ULL + (h1 << 6) + (h1 >> 2));
    }
};

// All state that main_loop() needs, collected in one place so both
// main() and main_loop() can access it.
struct AppState {
    GLFWwindow* window = nullptr;
    Camera camera;
    glm::vec2 tornadoPos{0.0f, 0.0f};
    float startTime = 0.0f;

    // Shaders
    GLuint program         = 0;
    GLuint particleProgram = 0;

    // Tornado mesh
    GLuint tornadoVAO = 0, tornadoVBO = 0, tornadoEBO = 0;
    GLsizei tornadoIndexCount = 0;

    // Ground / terrain
    GLuint groundVAO = 0, groundVBO = 0, groundEBO = 0;
    GLsizei terrainIndexCount = 0;  // replaces hardcoded 6
    glm::vec2 terrainCenter{0.0f, 0.0f}; // world XZ the grid is centred on

    // Water
    GLuint waterVAO = 0, waterVBO = 0, waterEBO = 0;
    GLsizei waterIndexCount = 0;

    // Day/night
    float dayTime = 0.25f;  // 0=midnight, 0.25=sunrise, 0.5=noon, 0.75=sunset

    // Scene models
    SimpleModel house;
    SimpleModel tree;

    // Textures
    GLuint brickTex = 0, leafTex = 0;

    // Uniform location caches
    MainUniforms mu;
    ParticleUniforms pu;
    SkyUniforms su;
    RainUniforms ru;
    HudUniforms hu;

    // Score & tornado growth
    Score score;
    float tornadoScale = 1.0f;
    float lastDestroyTime = 0.0f;  // for decay grace period

    // Combo multiplier
    int   comboCount      = 0;
    float comboTimer      = 0.0f;
    float comboMultiplier = 1.0f;

    // Camera shake (impact feedback)
    float shakeAmp = 0.0f;

    // Tornado path trail (world XZ positions)
    std::deque<glm::vec2> tornadoTrail;
    float trailSampleTimer = 0.0f;

    // Wave system
    GamePhase gamePhase = GamePhase::WAVE_ANNOUNCE;
    Wave wave;
    float victoryTimer = 0.0f;   // also reused as the game-over screen timer
    float minScaleTimer = 0.0f;  // time spent at minimum tornado size
    bool endlessMode = false;    // keep spawning waves after wave 10
    bool timeAttack = false;     // score-chase mode with a countdown
    float timeAttackRemaining = 0.0f;

    // Power-ups
    std::vector<PowerUp> powerUps;
    std::vector<ActivePowerUp> activePowerUps;
    float powerUpSpawnTimer = 5.0f; // first spawn after 5s

    // Sound state
    bool soundInitialized = false;

    // Destruction
    std::vector<DestructibleHouse> houses;
    std::vector<Debris> debrisPieces;
    SimpleModel debrisCube;

    // Weather
    LightningState lightning;
    std::vector<RainParticle> rainDrops;
    GLuint rainVAO = 0, rainVBO = 0;
    std::vector<float> rainBuf;

    // Sky
    GLuint skyVAO = 0;
    GLuint skyProgram = 0;
    GLuint rainProgram = 0;
    GLuint hudProgram = 0;
    GLuint fontTex = 0;
    GLuint hudVAO = 0, hudVBO = 0;
    std::vector<float> hudBuf;    // batched HUD vertices for one draw call
    size_t hudVBOCapacity = 0;    // current VBO size in bytes

    // Props (fences, cars, poles)
    std::vector<ChunkProp> chunkProps;
    SimpleModel fence;
    SimpleModel car;
    SimpleModel pole;

    // Animals
    std::vector<ChunkAnimal> animals;
    SimpleModel cow;

    // Ground scorch marks
    std::vector<ScorchMark> scorchMarks;
    float scorchTimer = 0.0f;
    GLuint scorchVAO = 0, scorchVBO = 0, scorchEBO = 0; // simple flat quad for scorch

    // Particles
    GLuint particleVAO = 0, particleVBO = 0;
    std::vector<Particle> particles;
    std::mt19937 rng;
    std::uniform_real_distribution<float> rnd01{0.0f, 1.0f};
    std::vector<float> particleBuf; // persistent upload buffer

    // Timing / FPS
    double lastT     = 0.0;
    double fpsTimer  = 0.0;
    int    fpsFrames = 0;

    // Mouse-look state
    bool   mouseLookActive = false;
    double lastMx = 0.0, lastMy = 0.0;

    // Chunk system
    std::vector<ChunkTree> chunkTrees;
    std::unordered_set<ChunkKey, ChunkKeyHash> loadedChunks;

    // Persistent record of destroyed objects so unloading and reloading a
    // chunk does not resurrect them (see makeObjKey).
    std::unordered_set<uint64_t> destroyedObjs;
};

// ── Global state ─────────────────────────────────────────────────────
static AppState  app;
static double    g_mouseX = 0.0;
static double    g_mouseY = 0.0;

// Touch input state (set from JavaScript on mobile)
static float g_touchMoveX  = 0.0f;  // virtual joystick -1..1
static float g_touchMoveY  = 0.0f;
static float g_touchLookDX = 0.0f;  // camera look delta (pixels/frame)
static float g_touchLookDY = 0.0f;
static bool  g_touchLookActive = false;

// Runtime-adjustable state (can be set from JS)
static int   g_activeParticles = MAX_PARTICLES;  // quality preset control
static bool  g_paused          = false;

#ifdef PLATFORM_EMSCRIPTEN
extern "C" {
    EMSCRIPTEN_KEEPALIVE void touch_set_move(float x, float y) {
        g_touchMoveX = std::isfinite(x) ? std::clamp(x, -1.0f, 1.0f) : 0.0f;
        g_touchMoveY = std::isfinite(y) ? std::clamp(y, -1.0f, 1.0f) : 0.0f;
    }
    EMSCRIPTEN_KEEPALIVE void touch_set_look(float dx, float dy, int active) {
        g_touchLookDX = std::isfinite(dx) ? std::clamp(dx, -500.0f, 500.0f) : 0.0f;
        g_touchLookDY = std::isfinite(dy) ? std::clamp(dy, -500.0f, 500.0f) : 0.0f;
        g_touchLookActive = (active != 0);
    }
    EMSCRIPTEN_KEEPALIVE void touch_set_cursor(float nx, float ny) {
        g_mouseX = std::isfinite(nx) ? (double)std::clamp(nx, -2.0f, 2.0f) : 0.0;
        g_mouseY = std::isfinite(ny) ? (double)std::clamp(ny, -2.0f, 2.0f) : 0.0;
    }
    // Quality preset: 0=low(600), 1=medium(1400), 2=high(2200)
    EMSCRIPTEN_KEEPALIVE void set_quality(int level) {
        const int counts[3] = {600, 1400, MAX_PARTICLES};
        g_activeParticles = counts[std::clamp(level, 0, 2)];
    }
    EMSCRIPTEN_KEEPALIVE void toggle_sound(int muted) {
        g_soundMuted = (muted != 0);
    }
    EMSCRIPTEN_KEEPALIVE void set_paused(int paused) {
        g_paused = (paused != 0);
    }
    EMSCRIPTEN_KEEPALIVE void restart_game() {
        // Allow restart from any game phase, not just VICTORY
        app.score = Score{};
        app.tornadoScale = 1.0f;
        app.lastDestroyTime = (float)(glfwGetTime() - app.startTime);
        app.wave = Wave{};
        app.wave.announceTimer = 0.0f;
        app.gamePhase = GamePhase::WAVE_ANNOUNCE;
        app.victoryTimer = 0.0f;
        app.minScaleTimer = 0.0f;
        app.endlessMode = false;
        app.activePowerUps.clear();
        app.powerUps.clear();
        app.powerUpSpawnTimer = 5.0f;
        for (auto& h  : app.houses)      { h.health  = 1.0f; h.destroyed  = false; }
        for (auto& tr : app.chunkTrees)  { tr.health = 1.0f; tr.destroyed = false; }
        for (auto& pr : app.chunkProps)  { pr.health = 1.0f; pr.destroyed = false; }
        for (auto& an : app.animals)     { an.health = 1.0f; an.destroyed = false; }
        app.destroyedObjs.clear();
        app.debrisPieces.clear();
        app.scorchMarks.clear();
        app.comboCount = 0;
        app.comboTimer = 0.0f;
        app.comboMultiplier = 1.0f;
        app.tornadoTrail.clear();
        app.trailSampleTimer = 0.0f;
        app.timeAttack = false;
        app.timeAttackRemaining = 0.0f;
        g_paused = false;
    }
    EMSCRIPTEN_KEEPALIVE void start_time_attack() {
        restart_game();
        app.timeAttack = true;
        app.timeAttackRemaining = TIME_ATTACK_SECONDS;
    }
    EMSCRIPTEN_KEEPALIVE int get_score_points() {
        return app.score.scorePoints;
    }
    // 0=PLAYING, 1=WAVE_ANNOUNCE, 2=VICTORY, 3=GAME_OVER
    EMSCRIPTEN_KEEPALIVE int get_game_phase() {
        return (int)app.gamePhase;
    }
}
#endif

// ── Utility: file loading ────────────────────────────────────────────
static std::string loadFile(const char* path) {
    std::vector<std::string> tries;
    tries.push_back(std::string(path));
    tries.push_back(std::string("./") + path);
    tries.push_back(std::string("../") + path);
    char cwd[4096];
    if (GET_CWD(cwd, sizeof(cwd)) != nullptr) {
        tries.push_back(std::string(cwd) + "/" + path);
        tries.push_back(std::string(cwd) + "/../" + path);
    }
    for (const auto &p : tries) {
        std::ifstream in(p);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
    }
    std::cerr << "Failed to open (tried multiple): " << path << std::endl;
    return {};
}

// Patch GLSL version line for the current platform.
// Desktop  -> #version 330 core
// WebGL 2  -> #version 300 es  (+ precision qualifier for fragment shaders)
static std::string adaptShaderSource(const std::string& src, bool isFragment) {
#ifdef PLATFORM_EMSCRIPTEN
    std::string out = src;
    auto pos = out.find("#version 330 core");
    if (pos != std::string::npos) {
        std::string rep = "#version 300 es\n";
        if (isFragment) rep += "precision highp float;\nprecision highp int;\n";
        out.replace(pos, 17, rep);
    }
    return out;
#else
    (void)isFragment;
    return src;
#endif
}

// ── Shader helpers ───────────────────────────────────────────────────
static GLuint compileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetShaderInfoLog(shader, len, nullptr, &log[0]);
        std::cerr << "Shader compile error:\n" << log << "\nSource:\n" << src << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint linkProgram(GLuint v, GLuint f) {
    if (!v || !f) return 0; // shader compilation already reported its error
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetProgramInfoLog(p, len, nullptr, &log[0]);
        std::cerr << "Program link error: " << log << std::endl;
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// ── Particle respawn ─────────────────────────────────────────────────
static void respawnParticle(Particle& p, bool inner) {
    auto& rng   = app.rng;
    auto& rnd01 = app.rnd01;
    float a = rnd01(rng) * 2.0f * (float)M_PI;
    float r = inner ? (0.02f + rnd01(rng)*0.6f)
                    : (0.6f  + rnd01(rng)*(0.6f + rnd01(rng)*1.4f));
    float y = inner ? (0.05f + rnd01(rng)*0.6f)
                    : (0.0f  + rnd01(rng)*2.0f);
    p.pos = glm::vec3(r * cosf(a), y, r * sinf(a));
    if (inner)
        p.vel = glm::vec3(-p.pos.z*3.0f, 2.0f + rnd01(rng)*1.5f, p.pos.x*3.0f);
    else
        p.vel = glm::vec3(-p.pos.z*1.2f + (rnd01(rng)-0.5f)*2.0f,
                           0.6f + rnd01(rng)*1.5f,
                           p.pos.x*1.2f + (rnd01(rng)-0.5f)*2.0f);
    p.life = inner ? (0.4f + rnd01(rng)*1.2f) : (0.8f + rnd01(rng)*2.0f);
}

// ── Rain respawn ─────────────────────────────────────────────────────
static void respawnRain(RainParticle& r, const glm::vec3& camPos) {
    auto& rng   = app.rng;
    auto& rnd01 = app.rnd01;
    r.pos.x = camPos.x + (rnd01(rng) - 0.5f) * RAIN_AREA;
    r.pos.y = RAIN_HEIGHT + rnd01(rng) * 5.0f;
    r.pos.z = camPos.z + (rnd01(rng) - 0.5f) * RAIN_AREA;
}

// ── Spawn debris from destroyed position ─────────────────────────────
static void spawnDebris(const glm::vec3& pos, int count) {
    auto& rng   = app.rng;
    auto& rnd01 = app.rnd01;
    for (int i = 0; i < count && (int)app.debrisPieces.size() < MAX_DEBRIS; ++i) {
        Debris d;
        d.pos = pos + glm::vec3((rnd01(rng)-0.5f)*1.2f,
                                 rnd01(rng)*1.0f,
                                (rnd01(rng)-0.5f)*1.2f);
        d.vel = glm::vec3((rnd01(rng)-0.5f)*4.0f,
                           3.0f + rnd01(rng)*5.0f,
                          (rnd01(rng)-0.5f)*4.0f);
        d.rotAxis = glm::normalize(glm::vec3(rnd01(rng)-0.5f,
                                              rnd01(rng)-0.5f,
                                              rnd01(rng)-0.5f));
        d.rotAngle = rnd01(rng) * 6.28f;
        d.angVel   = (rnd01(rng)-0.5f) * 10.0f;
        d.life     = DEBRIS_LIFETIME * (0.5f + rnd01(rng)*0.5f);
        d.size     = 0.06f + rnd01(rng) * 0.14f;
        d.colorType = (int)(rnd01(rng) * 3.0f) % 3;
        app.debrisPieces.push_back(d);
    }
}

// Shared vertex layout for terrain-style meshes
struct SceneVert { glm::vec3 pos; glm::vec3 normal; glm::vec3 col; };

// (Re)build the terrain vertex grid centred on (centerX, centerZ) and upload
// it into the existing VBO. Called once at startup and again whenever the
// camera strays far from the current centre — the ground follows the player,
// making the world effectively infinite.
static void buildTerrainMesh(float centerX, float centerZ) {
    const int GN = TERRAIN_GRID + 1; // vertices per axis
    static std::vector<SceneVert> gv; // persistent to avoid realloc on rebuilds
    gv.resize((size_t)GN * GN);
    float step = (TERRAIN_EXTENT * 2.0f) / TERRAIN_GRID;

    for (int iz = 0; iz < GN; ++iz) {
        for (int ix = 0; ix < GN; ++ix) {
            float x = centerX - TERRAIN_EXTENT + ix * step;
            float z = centerZ - TERRAIN_EXTENT + iz * step;
            float y = getTerrainHeight(x, z);
            // Color based on height
            glm::vec3 col;
            if (y < WATER_LEVEL + 0.05f) {
                col = glm::vec3(0.55f, 0.50f, 0.38f); // sand/beach
            } else if (y < 2.0f) {
                col = glm::vec3(0.15f, 0.45f, 0.2f);  // grass
            } else if (y < 4.0f) {
                float t2 = (y - 2.0f) / 2.0f;
                col = glm::mix(glm::vec3(0.15f,0.45f,0.2f), glm::vec3(0.4f,0.35f,0.28f), t2); // grass→rock
            } else {
                float t2 = glm::clamp((y - 4.0f) / 2.0f, 0.0f, 1.0f);
                col = glm::mix(glm::vec3(0.4f,0.35f,0.28f), glm::vec3(0.85f,0.85f,0.9f), t2); // rock→snow
            }
            gv[iz * GN + ix] = {glm::vec3(x, y, z), glm::vec3(0,1,0), col};
        }
    }
    // Compute normals from adjacent heights
    for (int iz = 0; iz < GN; ++iz) {
        for (int ix = 0; ix < GN; ++ix) {
            float hL = (ix > 0)    ? gv[iz*GN + ix-1].pos.y : gv[iz*GN+ix].pos.y;
            float hR = (ix < GN-1) ? gv[iz*GN + ix+1].pos.y : gv[iz*GN+ix].pos.y;
            float hD = (iz > 0)    ? gv[(iz-1)*GN + ix].pos.y : gv[iz*GN+ix].pos.y;
            float hU = (iz < GN-1) ? gv[(iz+1)*GN + ix].pos.y : gv[iz*GN+ix].pos.y;
            gv[iz * GN + ix].normal = glm::normalize(glm::vec3(hL - hR, 2.0f * step, hD - hU));
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, app.groundVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    (GLsizeiptr)(gv.size() * sizeof(SceneVert)), gv.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    app.terrainCenter = glm::vec2(centerX, centerZ);
}

// Pack (chunk coords, object category, per-chunk index) into a stable key.
// Categories: 0=house, 1=tree, 2=prop. 16-bit chunk coords cover ±655 km.
static uint64_t makeObjKey(int cx, int cz, int type, int idx) {
    return ((uint64_t)(uint16_t)(int16_t)cx << 48) |
           ((uint64_t)(uint16_t)(int16_t)cz << 32) |
           ((uint64_t)(uint32_t)type          << 16) |
           (uint64_t)(uint32_t)(idx & 0xFFFF);
}

// ── Generate objects for a chunk (deterministic by chunk coords) ─────
static void generateChunk(int cx, int cz) {
    ChunkKey key{cx, cz};
    if (app.loadedChunks.count(key)) return;
    app.loadedChunks.insert(key);

    // Deterministic seed from chunk coords — cast to uint32_t before multiply
    // so arithmetic wraps without undefined behaviour.
    uint32_t seed = ((uint32_t)cx * 73856093u) ^ ((uint32_t)cz * 19349663u);
    std::mt19937 cRng(seed);
    std::uniform_real_distribution<float> r01(0.0f, 1.0f);

    float ox = cx * CHUNK_SIZE;
    float oz = cz * CHUNK_SIZE;

    // Houses
    for (int i = 0; i < HOUSES_PER_CHUNK; ++i) {
        float px = ox + r01(cRng) * CHUNK_SIZE;
        float pz = oz + r01(cRng) * CHUNK_SIZE;
        float py = getTerrainHeight(px, pz);
        if (py < WATER_LEVEL + 0.2f) continue; // skip water areas
        DestructibleHouse h;
        h.pos = glm::vec3(px, py - 0.08f, pz); // slight embed so slopes don't leave gaps
        h.health = 1.0f;
        h.destroyed = false;
        h.chunkX = cx;
        h.chunkZ = cz;
        h.genIdx = i;
        if (app.destroyedObjs.count(makeObjKey(cx, cz, 0, h.genIdx))) {
            h.destroyed = true; h.health = 0.0f;
        }
        app.houses.push_back(h);
    }
    // Trees
    for (int i = 0; i < TREES_PER_CHUNK; ++i) {
        float px = ox + r01(cRng) * CHUNK_SIZE;
        float pz = oz + r01(cRng) * CHUNK_SIZE;
        float py = getTerrainHeight(px, pz);
        if (py < WATER_LEVEL + 0.1f) continue; // skip water areas
        ChunkTree t;
        t.pos = glm::vec3(px, py - 0.05f, pz);
        t.chunkX = cx;
        t.chunkZ = cz;
        t.health = 1.0f;
        t.destroyed = false;
        t.genIdx = i;
        if (app.destroyedObjs.count(makeObjKey(cx, cz, 1, t.genIdx))) {
            t.destroyed = true; t.health = 0.0f;
        }
        app.chunkTrees.push_back(t);
    }
    // Props: fences, cars, poles
    for (int i = 0; i < FENCES_PER_CHUNK; ++i) {
        float px = ox + r01(cRng) * CHUNK_SIZE;
        float pz = oz + r01(cRng) * CHUNK_SIZE;
        float py = getTerrainHeight(px, pz);
        if (py < WATER_LEVEL + 0.15f) continue;
        ChunkProp p;
        p.pos = glm::vec3(px, py - 0.04f, pz);
        p.chunkX = cx; p.chunkZ = cz;
        p.propType = 0; // fence
        p.yaw = r01(cRng) * 6.28f;
        p.genIdx = i; // fences: 0..99
        if (app.destroyedObjs.count(makeObjKey(cx, cz, 2, p.genIdx))) {
            p.destroyed = true; p.health = 0.0f;
        }
        app.chunkProps.push_back(p);
    }
    for (int i = 0; i < CARS_PER_CHUNK; ++i) {
        float px = ox + r01(cRng) * CHUNK_SIZE;
        float pz = oz + r01(cRng) * CHUNK_SIZE;
        float py = getTerrainHeight(px, pz);
        if (py < WATER_LEVEL + 0.15f) continue;
        ChunkProp p;
        p.pos = glm::vec3(px, py - 0.04f, pz);
        p.chunkX = cx; p.chunkZ = cz;
        p.propType = 1; // car
        p.yaw = r01(cRng) * 6.28f;
        p.genIdx = 100 + i; // cars: 100..199
        if (app.destroyedObjs.count(makeObjKey(cx, cz, 2, p.genIdx))) {
            p.destroyed = true; p.health = 0.0f;
        }
        app.chunkProps.push_back(p);
    }
    for (int i = 0; i < POLES_PER_CHUNK; ++i) {
        float px = ox + r01(cRng) * CHUNK_SIZE;
        float pz = oz + r01(cRng) * CHUNK_SIZE;
        float py = getTerrainHeight(px, pz);
        if (py < WATER_LEVEL + 0.1f) continue;
        ChunkProp p;
        p.pos = glm::vec3(px, py - 0.04f, pz);
        p.chunkX = cx; p.chunkZ = cz;
        p.propType = 2; // pole
        p.yaw = 0.0f;
        p.genIdx = 200 + i; // poles: 200..299
        if (app.destroyedObjs.count(makeObjKey(cx, cz, 2, p.genIdx))) {
            p.destroyed = true; p.health = 0.0f;
        }
        app.chunkProps.push_back(p);
    }
    // Animals (cows)
    for (int i = 0; i < ANIMALS_PER_CHUNK; ++i) {
        float px = ox + r01(cRng) * CHUNK_SIZE;
        float pz = oz + r01(cRng) * CHUNK_SIZE;
        float py = getTerrainHeight(px, pz);
        if (py < WATER_LEVEL + 0.15f) continue;
        ChunkAnimal a;
        a.pos = glm::vec3(px, py, pz);
        a.chunkX = cx; a.chunkZ = cz;
        a.genIdx = i;
        a.yaw = r01(cRng) * 6.28f;
        a.wanderDir = glm::vec2(cosf(a.yaw), sinf(a.yaw));
        a.wanderTimer = 1.0f + r01(cRng) * 3.0f;
        if (app.destroyedObjs.count(makeObjKey(cx, cz, 3, a.genIdx))) {
            a.destroyed = true; a.health = 0.0f;
        }
        app.animals.push_back(a);
    }
}

// ── Update loaded chunks around player position ──────────────────────
static void updateChunks(const glm::vec3& playerPos) {
    int pcx = (int)floorf(playerPos.x / CHUNK_SIZE);
    int pcz = (int)floorf(playerPos.z / CHUNK_SIZE);

    // Generate new chunks in radius
    for (int dx = -CHUNK_RADIUS; dx <= CHUNK_RADIUS; ++dx) {
        for (int dz = -CHUNK_RADIUS; dz <= CHUNK_RADIUS; ++dz) {
            generateChunk(pcx + dx, pcz + dz);
        }
    }

    // Remove chunks too far away
    int removeRadius = CHUNK_RADIUS + 2;
    auto chunkTooFar = [&](int cx, int cz) {
        return std::abs(cx - pcx) > removeRadius ||
               std::abs(cz - pcz) > removeRadius;
    };

    // Remove far houses (skip destroyed ones that had debris — keep debris alive)
    app.houses.erase(
        std::remove_if(app.houses.begin(), app.houses.end(),
            [&](const DestructibleHouse& h) {
                return chunkTooFar(h.chunkX, h.chunkZ);
            }),
        app.houses.end());

    // Remove far trees
    app.chunkTrees.erase(
        std::remove_if(app.chunkTrees.begin(), app.chunkTrees.end(),
            [&](const ChunkTree& t) {
                return chunkTooFar(t.chunkX, t.chunkZ);
            }),
        app.chunkTrees.end());

    // Remove far props
    app.chunkProps.erase(
        std::remove_if(app.chunkProps.begin(), app.chunkProps.end(),
            [&](const ChunkProp& p) {
                return chunkTooFar(p.chunkX, p.chunkZ);
            }),
        app.chunkProps.end());

    // Remove far animals
    app.animals.erase(
        std::remove_if(app.animals.begin(), app.animals.end(),
            [&](const ChunkAnimal& a) {
                return chunkTooFar(a.chunkX, a.chunkZ);
            }),
        app.animals.end());

    // Untrack removed chunks
    for (auto it = app.loadedChunks.begin(); it != app.loadedChunks.end(); ) {
        if (chunkTooFar(it->x, it->z))
            it = app.loadedChunks.erase(it);
        else
            ++it;
    }
}

// ── GLFW callback ────────────────────────────────────────────────────
static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    int w, h;
    glfwGetWindowSize(window, &w, &h);
    if (w <= 0 || h <= 0) return;
    g_mouseX =  (xpos / (double)w) * 2.0 - 1.0;
    g_mouseY = -((ypos / (double)h) * 2.0 - 1.0);
}



// ═════════════════════════════════════════════════════════════════════
// main_loop — called every frame by the browser (Emscripten) or by
//             the while-loop (desktop).
// ═════════════════════════════════════════════════════════════════════
static void main_loop() {
    auto& s = app;
    if (!s.window) return;

    // Skip physics/input updates while paused; glfwPollEvents keeps the
    // window responsive so the user can un-pause via button or keyboard.
    if (g_paused) {
        glfwPollEvents();
        return;
    }

    // -- Timing --
    double nowT = glfwGetTime();
    float  dt   = (float)(nowT - s.lastT);
    s.lastT     = nowT;
    if (dt > 0.1f || dt < 0.0f) dt = 0.016f;

    float t = (float)nowT - s.startTime;

    // -- Update chunks around player --
    updateChunks(s.camera.pos);

    // -- Re-centre the terrain grid when the camera strays far from it --
    if (fabsf(s.camera.pos.x - s.terrainCenter.x) > TERRAIN_RECENTER_DIST ||
        fabsf(s.camera.pos.z - s.terrainCenter.y) > TERRAIN_RECENTER_DIST) {
        float cx = floorf(s.camera.pos.x / CHUNK_SIZE + 0.5f) * CHUNK_SIZE;
        float cz = floorf(s.camera.pos.z / CHUNK_SIZE + 0.5f) * CHUNK_SIZE;
        buildTerrainMesh(cx, cz);
    }

    // -- Day/night cycle --
    s.dayTime += DAY_CYCLE_SPEED * dt;
    if (s.dayTime > 1.0f) s.dayTime -= 1.0f;

    // -- Tornado follows mouse via ground-plane raycast --
    // Now intersects terrain instead of y=0
    {
        int w, h;
        glfwGetFramebufferSize(s.window, &w, &h);
        float aspect = (w > 0 && h > 0) ? (float)w / (float)h : 1.0f;
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 200.0f);
        glm::mat4 view = s.camera.getView();
        glm::mat4 invVP = glm::inverse(proj * view);

        // NDC from mouse
        glm::vec4 nearNDC(g_mouseX, g_mouseY, -1.0f, 1.0f);
        glm::vec4 farNDC(g_mouseX, g_mouseY, 1.0f, 1.0f);
        glm::vec4 nearW = invVP * nearNDC; nearW /= nearW.w;
        glm::vec4 farW  = invVP * farNDC;  farW  /= farW.w;

        glm::vec3 rayO(nearW);
        glm::vec3 rayD = glm::normalize(glm::vec3(farW) - rayO);

        // March ray to find terrain intersection
        float bestT = -1.0f;
        for (float step = 0.5f; step < 150.0f; step += 0.5f) {
            glm::vec3 p = rayO + rayD * step;
            float th = getTerrainHeight(p.x, p.z);
            if (p.y <= th) {
                bestT = step;
                break;
            }
        }
        // Fallback: intersect y=0 plane
        if (bestT < 0.0f && fabsf(rayD.y) > 0.0001f) {
            float tHit = -rayO.y / rayD.y;
            if (tHit > 0.0f) bestT = tHit;
        }
        if (bestT > 0.0f) {
            glm::vec3 hit = rayO + rayD * bestT;
            glm::vec2 target(hit.x, hit.z);
            s.tornadoPos = glm::mix(s.tornadoPos, target, 1.0f - expf(-6.0f * dt));
        }
    }

    // ── Initialize sound on first frame (needs user gesture context) ──
    if (!s.soundInitialized) { initSound(); s.soundInitialized = true; }

    // ── Wave system logic ──
    if (s.gamePhase == GamePhase::WAVE_ANNOUNCE) {
        s.wave.announceTimer += dt;
        if (s.wave.announceTimer >= WAVE_ANNOUNCE_TIME) {
            s.gamePhase = GamePhase::PLAYING;
            s.wave.announceTimer = 0.0f;
        }
    }

    // ── Tornado decay (shrinks when idle) ──
    bool hasShield = false;
    for (auto& ap : s.activePowerUps)
        if (ap.type == PowerUpType::SHIELD) hasShield = true;

    if (s.gamePhase == GamePhase::PLAYING && !hasShield) {
        float timeSinceDestroy = t - s.lastDestroyTime;
        if (timeSinceDestroy > TORNADO_DECAY_GRACE) {
            s.tornadoScale -= TORNADO_DECAY_RATE * dt;
            if (s.tornadoScale < TORNADO_MIN_SCALE) {
                s.tornadoScale = TORNADO_MIN_SCALE;
            }
        }
    }

    // ── Game over: tornado starved at minimum size for too long ──
    // (disabled in Time Attack — there the only end condition is the clock)
    if (s.gamePhase == GamePhase::PLAYING && !s.timeAttack) {
        if (s.tornadoScale <= TORNADO_MIN_SCALE + 0.001f && !hasShield)
            s.minScaleTimer += dt;
        else
            s.minScaleTimer = 0.0f;
        if (s.minScaleTimer >= GAMEOVER_FADE_TIME) {
            s.gamePhase = GamePhase::GAME_OVER;
            s.victoryTimer = 0.0f;
            playGameOverSound();
            saveScore(s.score.scorePoints, s.wave.number);
        }
    }

    // ── Time Attack: countdown + EF ramp ──
    if (s.timeAttack && s.gamePhase == GamePhase::PLAYING) {
        s.timeAttackRemaining -= dt;
        float elapsed = TIME_ATTACK_SECONDS - s.timeAttackRemaining;
        s.wave.efScale = std::clamp((int)(elapsed / 30.0f), 0, 5);
        if (s.timeAttackRemaining <= 0.0f) {
            s.timeAttackRemaining = 0.0f;
            s.gamePhase = GamePhase::VICTORY;   // reuse the summary screen
            s.victoryTimer = 0.0f;
            playVictorySound();
            saveScore(s.score.scorePoints, s.wave.number);
        }
    }

    // ── Update active power-ups ──
    float speedMult = 1.0f;
    float sizeMult  = 1.0f;
    bool hasMagnet = false;
    float scoreMult = 1.0f;
    for (auto it = s.activePowerUps.begin(); it != s.activePowerUps.end();) {
        it->remaining -= dt;
        if (it->remaining <= 0.0f) {
            it = s.activePowerUps.erase(it);
        } else {
            if (it->type == PowerUpType::SPEED_BOOST) speedMult = 1.8f;
            if (it->type == PowerUpType::SIZE_DOUBLE) sizeMult  = 2.0f;
            if (it->type == PowerUpType::MAGNET)      hasMagnet = true;
            if (it->type == PowerUpType::SCORE_2X)    scoreMult = 2.0f;
            ++it;
        }
    }

    // ── Power-up spawning ──
    if (s.gamePhase == GamePhase::PLAYING) {
        s.powerUpSpawnTimer -= dt;
        if (s.powerUpSpawnTimer <= 0.0f && (int)s.powerUps.size() < MAX_POWERUPS) {
            s.powerUpSpawnTimer = POWERUP_SPAWN_INTERVAL;
            // Spawn near player but not too close
            float angle = s.rnd01(s.rng) * 6.28f;
            float dist  = 15.0f + s.rnd01(s.rng) * 25.0f;
            float px = s.camera.pos.x + cosf(angle) * dist;
            float pz = s.camera.pos.z + sinf(angle) * dist;
            float py = getTerrainHeight(px, pz);
            if (py > WATER_LEVEL) { // don't drop power-ups into lakes
                PowerUp pu;
                pu.pos = glm::vec3(px, py, pz);
                pu.type = (PowerUpType)((int)(s.rnd01(s.rng) * POWERUP_TYPE_COUNT) % POWERUP_TYPE_COUNT);
                pu.spawnTime = t;
                s.powerUps.push_back(pu);
            }
        }
    }

    // ── Power-up collection ──
    for (auto it = s.powerUps.begin(); it != s.powerUps.end();) {
        if (it->collected) { ++it; continue; }
        float dist = glm::length(glm::vec2(it->pos.x - s.tornadoPos.x,
                                            it->pos.z - s.tornadoPos.y));
        float collectR = POWERUP_COLLECT_RADIUS * s.tornadoScale;
        if (hasMagnet) collectR *= 3.0f;
        if (dist < collectR) {
            // Same type already active: refresh its timer instead of stacking
            bool refreshed = false;
            for (auto& ap : s.activePowerUps) {
                if (ap.type == it->type) {
                    ap.remaining = POWERUP_DURATION;
                    refreshed = true;
                    break;
                }
            }
            if (!refreshed) {
                ActivePowerUp ap;
                ap.type = it->type;
                ap.remaining = POWERUP_DURATION;
                s.activePowerUps.push_back(ap);
            }
            playPowerUpSound();
            it = s.powerUps.erase(it);
        } else {
            // Magnet: pull power-ups toward tornado
            if (hasMagnet && dist < collectR * 2.0f) {
                glm::vec2 dir = glm::normalize(s.tornadoPos - glm::vec2(it->pos.x, it->pos.z));
                it->pos.x += dir.x * 8.0f * dt;
                it->pos.z += dir.y * 8.0f * dt;
                it->pos.y = getTerrainHeight(it->pos.x, it->pos.z);
            }
            ++it;
        }
    }

    // ── Update wind sound based on tornado size ──
    updateWindVolume(s.tornadoScale);

    // ── Combo timer decay ──
    if (s.comboTimer > 0.0f) {
        s.comboTimer -= dt;
        if (s.comboTimer <= 0.0f) {
            s.comboCount      = 0;
            s.comboMultiplier = 1.0f;
        }
    }

    // ── Destruction: damage houses near tornado ──
    float efMult = EF_RADIUS_MULT[std::clamp(s.wave.efScale, 0, 5)];
    float effectiveRadius = DESTRUCTION_RADIUS * s.tornadoScale * sizeMult * efMult;
    if (s.gamePhase == GamePhase::GAME_OVER) effectiveRadius = 0.0f; // dead tornado

    bool destroyedSomething = false;
    float newPoints = 0.0f;
    for (auto& h : s.houses) {
        if (h.destroyed) continue;
        float dist = glm::length(glm::vec2(h.pos.x - s.tornadoPos.x,
                                            h.pos.z - s.tornadoPos.y));
        if (dist < effectiveRadius) {
            h.health -= DAMAGE_RATE * speedMult * dt;
            if (h.health <= 0.0f) {
                h.destroyed = true;
                s.destroyedObjs.insert(makeObjKey(h.chunkX, h.chunkZ, 0, h.genIdx));
                spawnDebris(h.pos, DEBRIS_PER_HOUSE);
                s.score.housesDestroyed++;
                s.score.totalDestroyed++;
                s.wave.destroyed++;
                destroyedSomething = true;
                newPoints += 100.0f;
                s.tornadoScale = std::min(s.tornadoScale + TORNADO_GROWTH_PER_OBJ, TORNADO_MAX_SCALE);
                if ((int)s.scorchMarks.size() >= MAX_SCORCH_MARKS)
                    s.scorchMarks.erase(s.scorchMarks.begin());
                s.scorchMarks.push_back({h.pos, 1.5f});
            }
        }
    }

    // ── Destruction: damage trees near tornado ──
    for (auto& tr : s.chunkTrees) {
        if (tr.destroyed) continue;
        float dist = glm::length(glm::vec2(tr.pos.x - s.tornadoPos.x,
                                            tr.pos.z - s.tornadoPos.y));
        if (dist < effectiveRadius) {
            tr.health -= DAMAGE_RATE * 1.5f * speedMult * dt;
            if (tr.health <= 0.0f) {
                tr.destroyed = true;
                s.destroyedObjs.insert(makeObjKey(tr.chunkX, tr.chunkZ, 1, tr.genIdx));
                spawnDebris(tr.pos, DEBRIS_PER_TREE);
                s.score.treesDestroyed++;
                s.score.totalDestroyed++;
                s.wave.destroyed++;
                destroyedSomething = true;
                newPoints += 30.0f;
                s.tornadoScale = std::min(s.tornadoScale + TORNADO_GROWTH_PER_OBJ * 0.5f, TORNADO_MAX_SCALE);
            }
        }
    }

    // ── Destruction: damage props near tornado ──
    for (auto& pr : s.chunkProps) {
        if (pr.destroyed) continue;
        float dist = glm::length(glm::vec2(pr.pos.x - s.tornadoPos.x,
                                            pr.pos.z - s.tornadoPos.y));
        if (dist < effectiveRadius) {
            float rate = (pr.propType == 1) ? DAMAGE_RATE * 0.8f : DAMAGE_RATE * 3.0f;
            pr.health -= rate * speedMult * dt;
            if (pr.health <= 0.0f) {
                pr.destroyed = true;
                s.destroyedObjs.insert(makeObjKey(pr.chunkX, pr.chunkZ, 2, pr.genIdx));
                int debrisCount = (pr.propType == 1) ? 25 : 8;
                spawnDebris(pr.pos, debrisCount);
                s.score.propsDestroyed++;
                s.score.totalDestroyed++;
                s.wave.destroyed++;
                destroyedSomething = true;
                // car=50pts, fence=20pts, pole=15pts
                float propPts = (pr.propType == 1) ? 50.0f : (pr.propType == 0 ? 20.0f : 15.0f);
                newPoints += propPts;
                s.tornadoScale = std::min(s.tornadoScale + TORNADO_GROWTH_PER_OBJ * 0.3f, TORNADO_MAX_SCALE);
            }
        }
    }

    // ── Animals: wander, flee from the tornado, get destroyed ──
    for (auto& an : s.animals) {
        if (an.destroyed) continue;
        glm::vec2 toTor(s.tornadoPos.x - an.pos.x, s.tornadoPos.y - an.pos.z);
        float torDist = glm::length(toTor);

        glm::vec2 moveDir;
        float moveSpeed;
        float panicR = ANIMAL_FLEE_RADIUS * s.tornadoScale;
        if (torDist < panicR && torDist > 0.01f) {
            moveDir = -toTor / torDist;                // run away from the tornado
            float panic = 1.0f - torDist / panicR;     // faster when it is closer
            moveSpeed = ANIMAL_FLEE_SPEED * (0.6f + 0.8f * panic);
        } else {
            an.wanderTimer -= dt;
            if (an.wanderTimer <= 0.0f) {
                float wa = s.rnd01(s.rng) * 6.28f;
                an.wanderDir = glm::vec2(cosf(wa), sinf(wa));
                an.wanderTimer = 2.0f + s.rnd01(s.rng) * 4.0f;
            }
            moveDir = an.wanderDir;
            moveSpeed = ANIMAL_WANDER_SPEED;
        }

        float nx = an.pos.x + moveDir.x * moveSpeed * dt;
        float nz = an.pos.z + moveDir.y * moveSpeed * dt;
        float nh = getTerrainHeight(nx, nz);
        if (nh > WATER_LEVEL + 0.1f) {                 // refuse to walk into water
            an.pos.x = nx; an.pos.z = nz; an.pos.y = nh;
            an.speed = moveSpeed;
            an.yaw = atan2f(-moveDir.y, moveDir.x);
        } else {
            an.wanderDir = -an.wanderDir;              // bounce off the shore
            an.speed = 0.0f;
        }

        if (torDist < effectiveRadius) {
            an.health -= DAMAGE_RATE * 4.0f * speedMult * dt;
            if (an.health <= 0.0f) {
                an.destroyed = true;
                s.destroyedObjs.insert(makeObjKey(an.chunkX, an.chunkZ, 3, an.genIdx));
                spawnDebris(an.pos, 10);
                s.score.propsDestroyed++;
                s.score.totalDestroyed++;
                s.wave.destroyed++;
                destroyedSomething = true;
                newPoints += 75.0f;
                playMooSound();
                s.tornadoScale = std::min(s.tornadoScale + TORNADO_GROWTH_PER_OBJ * 0.3f, TORNADO_MAX_SCALE);
            }
        }
    }

    // ── Post-destruction: sounds, combo, scoring, wave check ──
    if (destroyedSomething) {
        s.lastDestroyTime = t;
        playDestroySound();
        s.shakeAmp = std::min(s.shakeAmp + 0.05f + 0.03f * s.tornadoScale, 0.3f);
        // Combo: increment, apply multiplier, reset timer
        s.comboCount++;
        s.comboTimer      = 2.5f;
        s.comboMultiplier = 1.0f + std::min(s.comboCount / 3.0f, 4.0f);
        s.score.scorePoints += (int)(newPoints * s.comboMultiplier * scoreMult);
    }

    // Wave completion check (Time Attack ignores waves entirely)
    if (s.gamePhase == GamePhase::PLAYING && !s.timeAttack &&
        s.wave.destroyed >= s.wave.target) {
        if (s.wave.number >= TOTAL_WAVES && !s.endlessMode) {
            s.gamePhase = GamePhase::VICTORY;
            s.victoryTimer = 0.0f;
            playVictorySound();
            saveScore(s.score.scorePoints, s.wave.number);
        } else {
            s.wave.number++;
            s.wave.target = WAVE_BASE_TARGET + (s.wave.number - 1) * 3;
            s.wave.destroyed = 0;
            s.wave.announceTimer = 0.0f;
            s.wave.efScale = std::min(s.wave.number / 2, 5);
            s.gamePhase = GamePhase::WAVE_ANNOUNCE;
            playWaveSound();
        }
    }

    // ── Ground scorch: tornado leaves dark trail ──
    s.scorchTimer += dt;
    if (s.scorchTimer > 0.15f) {
        s.scorchTimer = 0.0f;
        if ((int)s.scorchMarks.size() >= MAX_SCORCH_MARKS)
            s.scorchMarks.erase(s.scorchMarks.begin());
        s.scorchMarks.push_back({glm::vec3(s.tornadoPos.x, 0.01f, s.tornadoPos.y),
                                  0.8f * s.tornadoScale});
    }

    // ── Tornado trail sampling ──
    s.trailSampleTimer += dt;
    if (s.trailSampleTimer >= 0.15f) {
        s.trailSampleTimer = 0.0f;
        s.tornadoTrail.push_back(s.tornadoPos);
        if ((int)s.tornadoTrail.size() > 40)
            s.tornadoTrail.pop_front();
    }

    // ── Debris physics ──
    for (auto it = s.debrisPieces.begin(); it != s.debrisPieces.end(); ) {
        Debris& d = *it;
        d.vel.y -= 9.8f * dt; // gravity
        // Vortex forces
        glm::vec3 toCenter = glm::vec3(s.tornadoPos.x, d.pos.y, s.tornadoPos.y) - d.pos;
        float dist = glm::length(glm::vec2(toCenter.x, toCenter.z));
        glm::vec3 tangent(-toCenter.z, 0, toCenter.x);
        if (dist > 0.01f) tangent /= dist;
        d.vel += tangent * 2.5f * dt;
        d.vel.y += 2.0f * dt;
        if (dist > 0.1f)
            d.vel += glm::normalize(glm::vec3(toCenter.x, 0, toCenter.z)) * 1.2f * dt;
        d.vel *= (1.0f - 1.2f * dt);
        d.pos += d.vel * dt;
        d.rotAngle += d.angVel * dt;
        // Ground bounce (terrain-aware)
        float groundH = getTerrainHeight(d.pos.x, d.pos.z);
        if (d.pos.y < groundH) {
            d.pos.y = groundH;
            d.vel.y = -d.vel.y * 0.3f;
            d.vel.x *= 0.7f; d.vel.z *= 0.7f;
            d.angVel *= 0.7f;
        }
        d.life -= dt;
        if (d.life <= 0.0f) { it = s.debrisPieces.erase(it); }
        else { ++it; }
    }

    // ── Lightning ──
    s.lightning.nextFlash -= dt;
    if (s.lightning.nextFlash <= 0.0f) {
        s.lightning.intensity = 0.7f + s.rnd01(s.rng) * 0.3f;
        s.lightning.nextFlash = LIGHTNING_MIN_INTERVAL +
            s.rnd01(s.rng) * (LIGHTNING_MAX_INTERVAL - LIGHTNING_MIN_INTERVAL);
        playThunderSound();
        s.shakeAmp = std::min(s.shakeAmp + 0.04f, 0.3f);
    }
    s.lightning.intensity *= expf(-LIGHTNING_DECAY * dt);
    if (s.lightning.intensity < 0.01f) s.lightning.intensity = 0.0f;

    // ── Rain ──
    glm::vec2 windDir = glm::length(s.tornadoPos) > 0.01f
        ? glm::normalize(s.tornadoPos) * 0.3f
        : glm::vec2(0.1f, 0.0f);
    for (auto& r : s.rainDrops) {
        r.pos.y -= RAIN_SPEED * dt;
        r.pos.x += windDir.x * dt * 3.0f + (s.rnd01(s.rng) - 0.5f) * 0.02f;
        r.pos.z += windDir.y * dt * 3.0f;
        // Terrain rises up to ~7.5 — only pay the noise cost for low drops
        if (r.pos.y < 8.5f) {
            float floorY = std::max(getTerrainHeight(r.pos.x, r.pos.z), WATER_LEVEL);
            if (r.pos.y < floorY) respawnRain(r, s.camera.pos);
        }
    }

    // -- Camera: mouse-look (RMB) --
    if (glfwGetMouseButton(s.window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
        double mx, my;
        glfwGetCursorPos(s.window, &mx, &my);
        if (!s.mouseLookActive) {
            s.mouseLookActive = true;
            s.lastMx = mx; s.lastMy = my;
        }
        double dx = mx - s.lastMx;
        double dy = s.lastMy - my;
        s.lastMx = mx; s.lastMy = my;
        s.camera.yaw   += (float)dx * s.camera.sensitivity;
        s.camera.pitch += (float)dy * s.camera.sensitivity;
        s.camera.pitch  = glm::clamp(s.camera.pitch, -89.0f, 89.0f);
    } else {
        s.mouseLookActive = false;
    }

    // Touch camera look (from virtual right-side drag on mobile)
    if (g_touchLookActive) {
        s.camera.yaw   += g_touchLookDX * s.camera.sensitivity;
        s.camera.pitch += g_touchLookDY * s.camera.sensitivity;
        s.camera.pitch  = glm::clamp(s.camera.pitch, -89.0f, 89.0f);
        g_touchLookDX = 0.0f;
        g_touchLookDY = 0.0f;
    }

    // -- Camera: WASD --
    glm::vec3 forward;
    forward.x = cosf(glm::radians(s.camera.yaw)) * cosf(glm::radians(s.camera.pitch));
    forward.y = sinf(glm::radians(s.camera.pitch));
    forward.z = sinf(glm::radians(s.camera.yaw)) * cosf(glm::radians(s.camera.pitch));
    forward = glm::normalize(forward);
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0,1,0)));
    if (glfwGetKey(s.window, GLFW_KEY_W) == GLFW_PRESS) s.camera.pos += forward * s.camera.speed * dt;
    if (glfwGetKey(s.window, GLFW_KEY_S) == GLFW_PRESS) s.camera.pos -= forward * s.camera.speed * dt;
    if (glfwGetKey(s.window, GLFW_KEY_A) == GLFW_PRESS) s.camera.pos -= right   * s.camera.speed * dt;
    if (glfwGetKey(s.window, GLFW_KEY_D) == GLFW_PRESS) s.camera.pos += right   * s.camera.speed * dt;

    // Virtual joystick (mobile touch)
    if (fabsf(g_touchMoveX) > 0.05f || fabsf(g_touchMoveY) > 0.05f) {
        glm::vec3 fwd2d = glm::normalize(glm::vec3(forward.x, 0.0f, forward.z));
        glm::vec3 rgt2d = glm::normalize(glm::cross(fwd2d, glm::vec3(0,1,0)));
        s.camera.pos += fwd2d * g_touchMoveY * s.camera.speed * dt;
        s.camera.pos += rgt2d * g_touchMoveX * s.camera.speed * dt;
    }

    // Clamp camera above terrain (min 1.5 units above ground)
    float terrainAtCam = getTerrainHeight(s.camera.pos.x, s.camera.pos.z);
    float minCamY = terrainAtCam + 1.5f;
    if (minCamY < 0.5f) minCamY = 0.5f; // absolute minimum
    if (s.camera.pos.y < minCamY) s.camera.pos.y = minCamY;

    // -- Framebuffer / clear --
    int width, height;
    glfwGetFramebufferSize(s.window, &width, &height);
    if (width == 0 || height == 0) return;
    glViewport(0, 0, width, height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // ── Storm sky (fullscreen, behind everything) ──
    glDisable(GL_DEPTH_TEST);
    glUseProgram(s.skyProgram);
    glUniform1f(s.su.lightningFlash, s.lightning.intensity);
    glUniform1f(s.su.time, t);
    glUniform1f(s.su.timeOfDay, s.dayTime);
    glBindVertexArray(s.skyVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEnable(GL_DEPTH_TEST);

    float aspect = (float)width / (float)height;
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 200.0f);
    glm::mat4 view = s.camera.getView();

    // Camera shake: jitter in view space, decaying exponentially
    s.shakeAmp *= expf(-5.0f * dt);
    if (s.shakeAmp > 0.003f) {
        glm::vec3 jitter((s.rnd01(s.rng) - 0.5f) * s.shakeAmp,
                         (s.rnd01(s.rng) - 0.5f) * s.shakeAmp, 0.0f);
        view = glm::translate(glm::mat4(1.0f), jitter) * view;
    }

    // -- Helper: compute normal matrix from model matrix --
    auto normalMat3 = [](const glm::mat4& model) -> glm::mat3 {
        return glm::mat3(glm::transpose(glm::inverse(model)));
    };

    // ════════════════════════════════
    // MAIN SHADER — scene + tornado
    // ════════════════════════════════
    glUseProgram(s.program);
    auto& mu = s.mu;
    glUniformMatrix4fv(mu.proj, 1, GL_FALSE, glm::value_ptr(proj));
    glUniformMatrix4fv(mu.view, 1, GL_FALSE, glm::value_ptr(view));
    glUniform1f(mu.time, t);
    glUniform3fv(mu.camPos, 1, glm::value_ptr(s.camera.pos));
    glUniform1f(mu.lightningFlash, s.lightning.intensity);
    glUniform1f(mu.windBend, 0.0f);
    glUniform3f(mu.windSource, s.tornadoPos.x, 0.0f, s.tornadoPos.y);
    glUniform1f(mu.timeOfDay, s.dayTime);
    glUniform1f(mu.waterLevel, WATER_LEVEL);

    // -- Terrain ground --
    {
        glm::mat4 model(1.0f);
        glm::mat3 nm = normalMat3(model);
        glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
        glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
        glUniform1f(mu.enableSwirl, 0.0f);
        glUniform3f(mu.tint, 1.0f, 1.0f, 1.0f);
        glUniform1f(mu.opacity, 1.0f);
        glUniform1i(mu.objType, 3);
        glUniform1i(mu.hasAlbedo, 0);
        glBindVertexArray(s.groundVAO);
        glDrawElements(GL_TRIANGLES, s.terrainIndexCount, GL_UNSIGNED_INT, nullptr);
    }

    // -- Water plane (follows the terrain grid so it is also infinite) --
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glm::mat4 model = glm::translate(glm::mat4(1.0f),
                              glm::vec3(s.terrainCenter.x, 0.0f, s.terrainCenter.y));
        glm::mat3 nm = normalMat3(model);
        glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
        glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
        glUniform1f(mu.enableSwirl, 0.0f);
        glUniform3f(mu.tint, 1.0f, 1.0f, 1.0f);
        glUniform1f(mu.opacity, 0.65f);
        glUniform1i(mu.objType, 6);  // water type
        glUniform1i(mu.hasAlbedo, 0);
        glBindVertexArray(s.waterVAO);
        glDrawElements(GL_TRIANGLES, s.waterIndexCount, GL_UNSIGNED_INT, nullptr);
        glDisable(GL_BLEND);
    }

    // -- Houses --
    {
        glUniform1i(mu.objType, 1);
        glUniform1f(mu.enableSwirl, 0.0f);
        glUniform1f(mu.opacity, 1.0f);
        glUniform1i(mu.hasAlbedo, 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s.brickTex);
        glUniform1i(mu.albedo, 0);

        for (auto& h : s.houses) {
            if (h.destroyed) continue;
            glm::mat4 model = glm::translate(glm::mat4(1.0f), h.pos);
            // Shake when damaged
            if (h.health < 1.0f) {
                float shake = (1.0f - h.health) * 0.12f;
                model = glm::translate(model, glm::vec3(
                    (s.rnd01(s.rng)-0.5f)*shake, 0.0f,
                    (s.rnd01(s.rng)-0.5f)*shake));
            }
            model = glm::scale(model, glm::vec3(1.5f));
            glm::mat3 nm = normalMat3(model);
            glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
            glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
            // Tint reddish when damaged
            float dmg = 1.0f - h.health;
            glUniform3f(mu.tint, 1.0f, 0.95f - dmg*0.3f, 0.9f - dmg*0.4f);
            glBindVertexArray(s.house.vao);
            glDrawElements(GL_TRIANGLES, s.house.indexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    // -- Trees --
    {
        glUniform1i(mu.objType, 2);
        glUniform1i(mu.hasAlbedo, 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s.leafTex);

        glUniform1f(mu.windBend, 1.5f);

        for (auto& ct : s.chunkTrees) {
            if (ct.destroyed) continue;
            glm::mat4 model = glm::translate(glm::mat4(1.0f), ct.pos);
            // Shake when damaged
            if (ct.health < 1.0f) {
                float shake = (1.0f - ct.health) * 0.3f;
                model = glm::translate(model, glm::vec3(
                    (s.rnd01(s.rng)-0.5f)*shake, 0.0f,
                    (s.rnd01(s.rng)-0.5f)*shake));
            }
            model = glm::scale(model, glm::vec3(2.0f));
            glm::mat3 nm = normalMat3(model);
            glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
            glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
            glUniform3f(mu.tint, 1.0f, 1.0f, 1.0f);
            glBindVertexArray(s.tree.vao);
            glDrawElements(GL_TRIANGLES, s.tree.indexCount, GL_UNSIGNED_INT, nullptr);
        }
        glUniform1f(mu.windBend, 0.0f);
    }

    // -- Props (fences, cars, poles) --
    {
        glUniform1i(mu.objType, 5); // reuse debris shader type
        glUniform1f(mu.enableSwirl, 0.0f);
        glUniform1i(mu.hasAlbedo, 0);

        for (auto& pr : s.chunkProps) {
            if (pr.destroyed) continue;
            const SimpleModel* mdl = nullptr;
            glm::vec3 tint;
            float sc = 1.0f;
            if (pr.propType == 0) { mdl = &s.fence; tint = glm::vec3(0.6f, 0.45f, 0.3f); sc = 1.0f; }        // fence
            else if (pr.propType == 1) { mdl = &s.car; tint = glm::vec3(0.7f, 0.15f, 0.1f); sc = 0.8f; }      // car
            else { mdl = &s.pole; tint = glm::vec3(0.5f, 0.5f, 0.55f); sc = 1.0f; }                            // pole
            if (!mdl || !mdl->vao) continue;

            glm::mat4 model = glm::translate(glm::mat4(1.0f), pr.pos);
            model = glm::rotate(model, pr.yaw, glm::vec3(0,1,0));
            // Shake when damaged
            if (pr.health < 1.0f) {
                float shake = (1.0f - pr.health) * 0.15f;
                model = glm::translate(model, glm::vec3(
                    (s.rnd01(s.rng)-0.5f)*shake, 0.0f,
                    (s.rnd01(s.rng)-0.5f)*shake));
            }
            model = glm::scale(model, glm::vec3(sc));
            glm::mat3 nm = normalMat3(model);
            glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
            glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
            glUniform3fv(mu.tint, 1, glm::value_ptr(tint));
            glUniform1f(mu.opacity, 1.0f);
            glBindVertexArray(mdl->vao);
            glDrawElements(GL_TRIANGLES, mdl->indexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    // -- Animals (cows) --
    if (!s.animals.empty() && s.cow.vao) {
        glUniform1i(mu.objType, 5);
        glUniform1f(mu.enableSwirl, 0.0f);
        glUniform1i(mu.hasAlbedo, 0);
        glUniform1f(mu.opacity, 1.0f);

        for (const auto& an : s.animals) {
            if (an.destroyed) continue;
            // Waddle bob while moving
            float bob = (an.speed > 0.1f)
                ? fabsf(sinf(t * 9.0f + (float)an.genIdx * 2.1f)) * 0.05f : 0.0f;
            glm::mat4 model = glm::translate(glm::mat4(1.0f),
                                  glm::vec3(an.pos.x, an.pos.y + bob, an.pos.z));
            model = glm::rotate(model, an.yaw, glm::vec3(0, 1, 0));
            glm::mat3 nm = normalMat3(model);
            glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
            glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
            // Alternate white and brown cows
            glm::vec3 tint = (an.genIdx % 2 == 0)
                ? glm::vec3(0.92f, 0.90f, 0.85f) : glm::vec3(0.55f, 0.38f, 0.25f);
            glUniform3fv(mu.tint, 1, glm::value_ptr(tint));
            glBindVertexArray(s.cow.vao);
            glDrawElements(GL_TRIANGLES, s.cow.indexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    // -- Power-ups (floating, glowing cubes) --
    if (!s.powerUps.empty() && s.debrisCube.vao) {
        glUniform1i(mu.objType, 5);
        glUniform1f(mu.enableSwirl, 0.0f);
        glUniform1i(mu.hasAlbedo, 0);

        for (const auto& pu : s.powerUps) {
            if (pu.collected) continue;
            // Per-type color
            glm::vec3 puColor(1.0f);
            switch (pu.type) {
                case PowerUpType::SPEED_BOOST: puColor = glm::vec3(0.2f, 1.0f, 0.3f); break; // green
                case PowerUpType::SIZE_DOUBLE: puColor = glm::vec3(1.0f, 0.3f, 0.8f); break; // pink
                case PowerUpType::MAGNET:      puColor = glm::vec3(0.3f, 0.5f, 1.0f); break; // blue
                case PowerUpType::SHIELD:      puColor = glm::vec3(1.0f, 0.9f, 0.2f); break; // gold
                case PowerUpType::SCORE_2X:    puColor = glm::vec3(1.0f, 0.55f, 0.0f); break; // orange
            }
            float bob = sinf(t * POWERUP_BOB_SPEED + pu.spawnTime * 3.0f) * POWERUP_BOB_HEIGHT;
            float spin = t * 2.0f + pu.spawnTime;
            glm::mat4 model = glm::translate(glm::mat4(1.0f),
                                  glm::vec3(pu.pos.x, pu.pos.y + 0.8f + bob, pu.pos.z));
            model = glm::rotate(model, spin, glm::vec3(0, 1, 0));
            model = glm::scale(model, glm::vec3(0.35f));
            glm::mat3 nm = normalMat3(model);
            glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
            glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
            // Pulse glow
            float pulse = 0.7f + 0.3f * sinf(t * 5.0f + pu.spawnTime);
            glUniform3f(mu.tint, puColor.x * pulse, puColor.y * pulse, puColor.z * pulse);
            glUniform1f(mu.opacity, 0.85f);
            glBindVertexArray(s.debrisCube.vao);
            glDrawElements(GL_TRIANGLES, s.debrisCube.indexCount, GL_UNSIGNED_INT, nullptr);
        }
        glUniform1f(mu.opacity, 1.0f);
    }

    // -- Scorch marks on ground --
    if (!s.scorchMarks.empty() && s.scorchVAO) {
        glUniform1i(mu.objType, 3);
        glUniform1i(mu.hasAlbedo, 0);
        glUniform1f(mu.enableSwirl, 0.0f);
        for (const auto& sm : s.scorchMarks) {
            float sy = getTerrainHeight(sm.pos.x, sm.pos.z) + 0.02f;
            glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(sm.pos.x, sy, sm.pos.z));
            model = glm::scale(model, glm::vec3(sm.radius, 1.0f, sm.radius));
            glm::mat3 nm = normalMat3(model);
            glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
            glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
            glUniform3f(mu.tint, 0.3f, 0.3f, 0.25f); // dark scorch
            glUniform1f(mu.opacity, 0.7f);
            glBindVertexArray(s.scorchVAO);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
        }
        glUniform1f(mu.opacity, 1.0f);
    }

    // -- Tornado path trail (ghost cones) --
    if (!s.tornadoTrail.empty()) {
        int trailLen = (int)s.tornadoTrail.size();
        int trailStart = std::max(0, trailLen - 10);
        glUniform1f(mu.enableSwirl, 1.0f);
        glUniform1i(mu.objType, 0);
        glUniform1i(mu.hasAlbedo, 0);
        for (int ti = trailStart; ti < trailLen - 1; ++ti) {
            float age = (float)(trailLen - 1 - ti) / 10.0f; // 0=newest, 1=oldest
            float opacity = (1.0f - age) * 0.22f;
            if (opacity < 0.01f) continue;
            float ghostScale = s.tornadoScale * (0.25f + 0.15f * (1.0f - age));
            glm::vec2 tp = s.tornadoTrail[ti];
            float ty = getTerrainHeight(tp.x, tp.y);
            glm::mat4 trailModel = glm::translate(glm::mat4(1.0f), glm::vec3(tp.x, ty, tp.y));
            trailModel = glm::scale(trailModel, glm::vec3(ghostScale));
            glm::mat3 nm = normalMat3(trailModel);
            glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(trailModel));
            glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
            glUniform3f(mu.tint, 0.55f, 0.55f, 0.75f);
            glUniform1f(mu.opacity, opacity);
            glBindVertexArray(s.tornadoVAO);
            glDrawElements(GL_TRIANGLES, s.tornadoIndexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    // -- Tornado mesh (swirl enabled, semi-transparent) --
    {
        float tornadoTerrainY = getTerrainHeight(s.tornadoPos.x, s.tornadoPos.y);
        glm::mat4 model = glm::translate(glm::mat4(1.0f),
                              glm::vec3(s.tornadoPos.x, tornadoTerrainY, s.tornadoPos.y));
        model = glm::scale(model, glm::vec3(s.tornadoScale, s.tornadoScale, s.tornadoScale));
        glm::mat3 nm = normalMat3(model);
        glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
        glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
        glUniform1f(mu.enableSwirl, 1.0f);
        // Darker, angrier funnel as the EF scale climbs (EF0 pale grey → EF5 near-black)
        float ef = std::clamp(s.wave.efScale, 0, 5) / 5.0f;
        glm::vec3 tornadoTint = glm::mix(glm::vec3(0.8f, 0.8f, 0.9f),
                                         glm::vec3(0.28f, 0.26f, 0.34f), ef);
        glUniform3fv(mu.tint, 1, glm::value_ptr(tornadoTint));
        glUniform1f(mu.opacity, 0.7f + 0.2f * ef); // denser at high EF
        glUniform1i(mu.objType, 0);
        glUniform1i(mu.hasAlbedo, 0);
        glBindVertexArray(s.tornadoVAO);
        glDrawElements(GL_TRIANGLES, s.tornadoIndexCount, GL_UNSIGNED_INT, nullptr);
    }

    // -- 3D debris pieces --
    if (!s.debrisPieces.empty() && s.debrisCube.vao) {
        glUniform1i(mu.objType, 5);
        glUniform1f(mu.enableSwirl, 0.0f);
        glUniform1i(mu.hasAlbedo, 0);

        for (const auto& d : s.debrisPieces) {
            glm::mat4 model = glm::translate(glm::mat4(1.0f), d.pos);
            model = glm::rotate(model, d.rotAngle, d.rotAxis);
            model = glm::scale(model, glm::vec3(d.size));
            glm::mat3 nm = normalMat3(model);
            glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
            glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));

            glm::vec3 tint;
            if (d.colorType == 0) tint = glm::vec3(0.8f, 0.4f, 0.3f);       // brick
            else if (d.colorType == 1) tint = glm::vec3(0.6f, 0.4f, 0.2f);   // wood
            else tint = glm::vec3(0.4f, 0.25f, 0.2f);                          // roof
            glUniform3fv(mu.tint, 1, glm::value_ptr(tint));

            float alpha = glm::clamp(d.life / 1.0f, 0.0f, 1.0f);
            glUniform1f(mu.opacity, alpha);

            glBindVertexArray(s.debrisCube.vao);
            glDrawElements(GL_TRIANGLES, s.debrisCube.indexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    // ════════════════════════════════
    // PARTICLES — update, upload, draw
    // ════════════════════════════════
    for (int i = 0; i < g_activeParticles; ++i) {
        Particle& p = s.particles[i];
        bool inner = (i < INNER_PARTICLES);

        glm::vec3 toCenter = glm::vec3(s.tornadoPos.x, p.pos.y, s.tornadoPos.y) - p.pos;
        float dist = glm::length(glm::vec2(toCenter.x, toCenter.z));
        glm::vec3 tangent = glm::vec3(-toCenter.z, 0.0f, toCenter.x);
        if (dist > 0.01f) tangent /= dist;

        float vortex = inner ? VORTEX_INNER : VORTEX_OUTER;
        float up     = inner ? UPLIFT_INNER : UPLIFT_OUTER;
        float pull   = inner ? PULL_INNER   : PULL_OUTER;

        p.vel += tangent * vortex * dt;
        p.vel.y += up * dt;
        if (dist > 0.1f)
            p.vel += glm::normalize(glm::vec3(toCenter.x, 0, toCenter.z)) * pull * dt;
        p.vel *= (1.0f - VEL_DAMPING * dt);
        p.pos += p.vel * dt;
        p.life -= dt;

        if (p.life <= 0.0f) {
            respawnParticle(p, inner);
            p.pos.x += s.tornadoPos.x;
            p.pos.z += s.tornadoPos.y;
        }
    }

    // Upload to GPU (vec3 pos + float life per particle) — persistent buffer
    {
        for (int i = 0; i < g_activeParticles; ++i) {
            s.particleBuf[i*4+0] = s.particles[i].pos.x;
            s.particleBuf[i*4+1] = s.particles[i].pos.y;
            s.particleBuf[i*4+2] = s.particles[i].pos.z;
            s.particleBuf[i*4+3] = s.particles[i].life;
        }
        glBindBuffer(GL_ARRAY_BUFFER, s.particleVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        g_activeParticles * 4 * sizeof(float), s.particleBuf.data());
    }

    // Draw particles
    glUseProgram(s.particleProgram);
    auto& pu = s.pu;
    glUniformMatrix4fv(pu.proj, 1, GL_FALSE, glm::value_ptr(proj));
    glUniformMatrix4fv(pu.view, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(pu.model, 1, GL_FALSE, glm::value_ptr(glm::mat4(1.0f)));

#ifndef PLATFORM_EMSCRIPTEN
    glEnable(GL_PROGRAM_POINT_SIZE);
#endif

    glBindVertexArray(s.particleVAO);

    int innerDraw = std::min(INNER_PARTICLES, g_activeParticles);
    int outerDraw = std::max(0, g_activeParticles - INNER_PARTICLES);

    // Debris darkens with EF scale so a violent tornado looks dirtier
    float efp = std::clamp(s.wave.efScale, 0, 5) / 5.0f;
    float efDark = 1.0f - 0.35f * efp;

    // inner (dense dark dust)
    glUniform3f(pu.color, 0.25f * efDark, 0.22f * efDark, 0.2f * efDark);
    glUniform1f(pu.pointScale, 1.5f);
    if (innerDraw > 0) glDrawArrays(GL_POINTS, 0, innerDraw);

    // outer (lighter debris)
    glUniform3f(pu.color, 0.5f * efDark, 0.45f * efDark, 0.35f * efDark);
    glUniform1f(pu.pointScale, 2.0f);
    if (outerDraw > 0) glDrawArrays(GL_POINTS, INNER_PARTICLES, outerDraw);

    glBindVertexArray(0);

    // ════════════════════════════════
    // RAIN — upload + draw as point sprites
    // ════════════════════════════════
    if (!s.rainDrops.empty()) {
        for (int i = 0; i < MAX_RAIN; ++i) {
            s.rainBuf[i*4+0] = s.rainDrops[i].pos.x;
            s.rainBuf[i*4+1] = s.rainDrops[i].pos.y;
            s.rainBuf[i*4+2] = s.rainDrops[i].pos.z;
            s.rainBuf[i*4+3] = 1.0f;
        }
        glBindBuffer(GL_ARRAY_BUFFER, s.rainVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        (GLsizeiptr)(s.rainBuf.size() * sizeof(float)), s.rainBuf.data());

        glUseProgram(s.rainProgram);
        glUniformMatrix4fv(s.ru.proj, 1, GL_FALSE, glm::value_ptr(proj));
        glUniformMatrix4fv(s.ru.view, 1, GL_FALSE, glm::value_ptr(view));
        glBindVertexArray(s.rainVAO);
        glDrawArrays(GL_POINTS, 0, MAX_RAIN);
        glBindVertexArray(0);
    }

    // ════════════════════════════════
    // HUD — score, wave, power-ups, minimap, overlays
    // ════════════════════════════════
    {
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // All HUD elements are appended into one vertex buffer and drawn with
        // a single call at the end of this block (order = append order).
        auto& hb = s.hudBuf;
        hb.clear();
        auto pushVert = [&](float x, float y, float u, float v,
                            const glm::vec3& c, float a, float mode) {
            hb.insert(hb.end(), {x, y, u, v, c.x, c.y, c.z, a, mode});
        };

        // Append a text line: each char is a textured quad (font atlas mode)
        auto renderLine = [&](const char* text, float startX, float startY,
                              float charW, float charH, glm::vec3 color) {
            float cx = startX;
            int _rlCount = 0;
            for (const char* c = text; *c && _rlCount < 256; ++c, ++_rlCount) {
                int ch = (int)(unsigned char)*c;
                if (ch < 32 || ch > 126) ch = 32;
                int col = (ch - 32) % 16;
                int row = (ch - 32) / 16;
                float u0 = col / 16.0f, u1 = (col+1) / 16.0f;
                float v0 = row / 6.0f,  v1 = (row+1) / 6.0f;
                float x0 = cx, x1 = cx + charW;
                float y0 = startY, y1 = startY + charH;
                pushVert(x0,y0,u0,v1,color,1.0f,1.0f);
                pushVert(x1,y0,u1,v1,color,1.0f,1.0f);
                pushVert(x1,y1,u1,v0,color,1.0f,1.0f);
                pushVert(x0,y0,u0,v1,color,1.0f,1.0f);
                pushVert(x1,y1,u1,v0,color,1.0f,1.0f);
                pushVert(x0,y1,u0,v0,color,1.0f,1.0f);
                cx += charW;
            }
        };

        // Append a filled quad with explicit alpha (solid, no texture)
        auto renderQuadA = [&](float x0, float y0, float x1, float y1,
                                glm::vec3 color, float alpha) {
            pushVert(x0,y0,0,0,color,alpha,0.0f);
            pushVert(x1,y0,0,0,color,alpha,0.0f);
            pushVert(x1,y1,0,0,color,alpha,0.0f);
            pushVert(x0,y0,0,0,color,alpha,0.0f);
            pushVert(x1,y1,0,0,color,alpha,0.0f);
            pushVert(x0,y1,0,0,color,alpha,0.0f);
        };

        // Append a fully opaque filled quad
        auto renderQuad = [&](float x0, float y0, float x1, float y1, glm::vec3 color) {
            renderQuadA(x0, y0, x1, y1, color, 1.0f);
        };

        float cw = 0.022f, ch = 0.045f;
        float scw = cw * 0.8f, sch = ch * 0.8f;

        // ── Top-left: score info ──
        char buf[64];
        snprintf(buf, sizeof(buf), "SCOR: %d", s.score.scorePoints);
        renderLine(buf, -0.98f, 0.92f, cw, ch, glm::vec3(1.0f, 0.85f, 0.0f));

        snprintf(buf, sizeof(buf), "DISTRUSE: %d", s.score.totalDestroyed);
        renderLine(buf, -0.98f, 0.86f, scw, sch, glm::vec3(1.0f, 0.9f, 0.3f));

        snprintf(buf, sizeof(buf), "CASE:%d COPACI:%d ALTE:%d",
                 s.score.housesDestroyed, s.score.treesDestroyed, s.score.propsDestroyed);
        renderLine(buf, -0.98f, 0.81f, scw * 0.85f, sch * 0.85f, glm::vec3(0.8f, 0.8f, 0.8f));

        snprintf(buf, sizeof(buf), "TORNADA x%.1f", s.tornadoScale);
        renderLine(buf, -0.98f, 0.76f, scw * 0.85f, sch * 0.85f, glm::vec3(1.0f, 0.5f, 0.3f));

        // ── Combo multiplier display ──
        if (s.comboCount > 0) {
            float comboPulse = 0.7f + 0.3f * sinf(t * 8.0f);
            snprintf(buf, sizeof(buf), "COMBO x%d  x%.1f", s.comboCount, s.comboMultiplier);
            renderLine(buf, -0.98f, 0.70f, scw * 0.85f, sch * 0.85f,
                       glm::vec3(1.0f, 0.4f, 0.1f) * comboPulse);
        }

        // ── Top-right: wave info / Time Attack clock ──
        if (s.timeAttack) {
            int secs = (int)ceilf(s.timeAttackRemaining);
            snprintf(buf, sizeof(buf), "TIMP %d:%02d  EF%d", secs / 60, secs % 60, s.wave.efScale);
            // Red pulse in the final 10 seconds
            glm::vec3 clockCol = (secs <= 10)
                ? glm::vec3(1.0f, 0.3f, 0.2f) * (0.6f + 0.4f * sinf(t * 8.0f))
                : glm::vec3(0.5f, 0.9f, 1.0f);
            renderLine(buf, 0.52f, 0.92f, cw, ch, clockCol);

            // Time bar
            float frac = glm::clamp(s.timeAttackRemaining / TIME_ATTACK_SECONDS, 0.0f, 1.0f);
            float barX0 = 0.52f, barX1 = 0.96f, barY = 0.88f, barH = 0.02f;
            renderQuad(barX0, barY, barX1, barY + barH, glm::vec3(0.2f, 0.2f, 0.2f));
            renderQuad(barX0, barY, barX0 + (barX1 - barX0) * frac, barY + barH,
                       glm::vec3(1.0f, 0.7f, 0.2f));
        } else {
            if (s.endlessMode)
                snprintf(buf, sizeof(buf), "VAL %d  EF%d  INFINIT",
                         s.wave.number, s.wave.efScale);
            else
                snprintf(buf, sizeof(buf), "VAL %d/%d  EF%d",
                         s.wave.number, TOTAL_WAVES, s.wave.efScale);
            renderLine(buf, 0.52f, 0.92f, cw, ch, glm::vec3(0.5f, 0.9f, 1.0f));

            // Wave progress bar
            float progress = (s.wave.target > 0) ?
                (float)s.wave.destroyed / (float)s.wave.target : 0.0f;
            progress = glm::clamp(progress, 0.0f, 1.0f);
            float barX0 = 0.52f, barX1 = 0.96f, barY = 0.88f, barH = 0.02f;
            renderQuad(barX0, barY, barX1, barY + barH, glm::vec3(0.2f, 0.2f, 0.2f));
            renderQuad(barX0, barY, barX0 + (barX1 - barX0) * progress, barY + barH,
                       glm::vec3(0.3f, 0.8f, 1.0f));

            snprintf(buf, sizeof(buf), "%d/%d", s.wave.destroyed, s.wave.target);
            renderLine(buf, 0.52f, 0.83f, scw * 0.8f, sch * 0.8f, glm::vec3(0.7f, 0.7f, 0.7f));
        }

        // ── Active power-ups (left side, below score) ──
        float puY = 0.63f;
        for (const auto& ap : s.activePowerUps) {
            const char* name = "";
            glm::vec3 puCol(1.0f);
            switch (ap.type) {
                case PowerUpType::SPEED_BOOST: name = "SPEED"; puCol = glm::vec3(0.2f, 1.0f, 0.3f); break;
                case PowerUpType::SIZE_DOUBLE: name = "SIZE x2"; puCol = glm::vec3(1.0f, 0.3f, 0.8f); break;
                case PowerUpType::MAGNET:      name = "MAGNET"; puCol = glm::vec3(0.3f, 0.5f, 1.0f); break;
                case PowerUpType::SHIELD:      name = "SHIELD"; puCol = glm::vec3(1.0f, 0.9f, 0.2f); break;
                case PowerUpType::SCORE_2X:    name = "SCOR x2"; puCol = glm::vec3(1.0f, 0.55f, 0.0f); break;
            }
            snprintf(buf, sizeof(buf), "%s %.1fs", name, ap.remaining);
            // Flash when about to expire
            float alpha = (ap.remaining < 2.0f) ? (0.5f + 0.5f * sinf(t * 8.0f)) : 1.0f;
            renderLine(buf, -0.98f, puY, scw * 0.75f, sch * 0.75f,
                       puCol * alpha);
            puY -= 0.05f;
        }

        // ── Compass: tornado direction indicator (top-center) ──
        {
            // Camera forward/right vectors in world XZ
            float yawRad = glm::radians(s.camera.yaw);
            glm::vec2 camFwd( cosf(yawRad), sinf(yawRad));
            glm::vec2 camRight(-sinf(yawRad), cosf(yawRad));
            glm::vec2 toTornado = glm::vec2(s.tornadoPos.x - s.camera.pos.x,
                                            s.tornadoPos.y - s.camera.pos.z);
            float dist3d = glm::length(toTornado);
            float bearing = 0.0f;
            if (dist3d > 0.1f) {
                glm::vec2 dir = toTornado / dist3d;
                float fwdComp   = glm::dot(dir, camFwd);
                float rightComp = glm::dot(dir, camRight);
                bearing = atan2f(rightComp, fwdComp);
            }

            float cX = 0.0f, cY = 0.73f;
            float outerR = 0.07f;
            float dotS2 = 0.006f;

            // Circle outline (12 dots) — x compressed by aspect so it stays round
            for (int i = 0; i < 12; ++i) {
                float a = (float)i * (float)M_PI / 6.0f;
                float dx = outerR * sinf(a) / aspect, dy = outerR * cosf(a);
                renderQuadA(cX+dx-dotS2/aspect, cY+dy-dotS2, cX+dx+dotS2/aspect, cY+dy+dotS2,
                            glm::vec3(0.4f, 0.5f, 0.6f), 0.6f);
            }
            // Tornado indicator dot
            float iR = outerR * 0.8f;
            float idx = iR * sinf(bearing) / aspect, idy = iR * cosf(bearing);
            float bigS = 0.013f;
            renderQuad(cX+idx-bigS/aspect, cY+idy-bigS, cX+idx+bigS/aspect, cY+idy+bigS,
                       glm::vec3(1.0f, 0.4f, 0.1f));
            // Distance text
            snprintf(buf, sizeof(buf), "%.0fm", dist3d);
            float tw = (float)strlen(buf) * scw * 0.7f;
            renderLine(buf, cX - tw * 0.5f, cY - outerR - sch * 0.75f,
                       scw * 0.7f, sch * 0.7f, glm::vec3(0.9f, 0.7f, 0.5f));
        }

        // ── Minimap (bottom-right corner, square on screen) ──
        {
            float mmR  = MINIMAP_NDC_SIZE;         // half-height in NDC
            float mmRx = mmR / aspect;             // half-width, aspect-corrected
            float mmX  = 1.0f - 0.05f - mmRx;      // center X in NDC
            float mmY  = -0.72f;                   // center Y in NDC

            // Background
            renderQuad(mmX - mmRx, mmY - mmR, mmX + mmRx, mmY + mmR,
                       glm::vec3(0.05f, 0.08f, 0.05f));

            // Border
            float bw = 0.005f, bwx = 0.005f / aspect;
            renderQuad(mmX - mmRx - bwx, mmY - mmR - bw, mmX + mmRx + bwx, mmY - mmR,
                       glm::vec3(0.3f, 0.5f, 0.3f));
            renderQuad(mmX - mmRx - bwx, mmY + mmR, mmX + mmRx + bwx, mmY + mmR + bw,
                       glm::vec3(0.3f, 0.5f, 0.3f));
            renderQuad(mmX - mmRx - bwx, mmY - mmR, mmX - mmRx, mmY + mmR,
                       glm::vec3(0.3f, 0.5f, 0.3f));
            renderQuad(mmX + mmRx, mmY - mmR, mmX + mmRx + bwx, mmY + mmR,
                       glm::vec3(0.3f, 0.5f, 0.3f));

            // Convert world pos to minimap NDC
            auto worldToMM = [&](float wx, float wz) -> glm::vec2 {
                float dx = wx - s.camera.pos.x;
                float dz = wz - s.camera.pos.z;
                float mx = mmX + (dx / MINIMAP_RADIUS) * mmRx;
                float my = mmY - (dz / MINIMAP_RADIUS) * mmR;
                return glm::vec2(mx, my);
            };
            auto inMM = [&](glm::vec2 p) {
                return p.x > mmX - mmRx && p.x < mmX + mmRx &&
                       p.y > mmY - mmR && p.y < mmY + mmR;
            };
            // Square dot regardless of screen aspect
            auto renderDot = [&](glm::vec2 p, float hs, glm::vec3 color, float alphaV) {
                renderQuadA(p.x - hs / aspect, p.y - hs, p.x + hs / aspect, p.y + hs,
                            color, alphaV);
            };

            float dotS = 0.006f;

            // Houses (red dots)
            for (const auto& h : s.houses) {
                if (h.destroyed) continue;
                glm::vec2 p = worldToMM(h.pos.x, h.pos.z);
                if (inMM(p)) renderDot(p, dotS, glm::vec3(0.9f, 0.3f, 0.2f), 1.0f);
            }
            // Trees (green dots)
            for (const auto& tr : s.chunkTrees) {
                if (tr.destroyed) continue;
                glm::vec2 p = worldToMM(tr.pos.x, tr.pos.z);
                if (inMM(p)) renderDot(p, dotS*0.7f, glm::vec3(0.2f, 0.7f, 0.2f), 1.0f);
            }
            // Props (gray dots)
            for (const auto& pr : s.chunkProps) {
                if (pr.destroyed) continue;
                glm::vec2 p = worldToMM(pr.pos.x, pr.pos.z);
                if (inMM(p)) renderDot(p, dotS*0.5f, glm::vec3(0.5f, 0.5f, 0.5f), 1.0f);
            }
            // Animals (pinkish dots)
            for (const auto& an : s.animals) {
                if (an.destroyed) continue;
                glm::vec2 p = worldToMM(an.pos.x, an.pos.z);
                if (inMM(p)) renderDot(p, dotS*0.7f, glm::vec3(1.0f, 0.75f, 0.75f), 1.0f);
            }
            // Power-ups (bright colored dots)
            for (const auto& pu : s.powerUps) {
                if (pu.collected) continue;
                glm::vec2 p = worldToMM(pu.pos.x, pu.pos.z);
                if (inMM(p)) renderDot(p, dotS, glm::vec3(1.0f, 1.0f, 0.0f), 1.0f);
            }
            // Tornado trail (faded gray dots)
            {
                int trailLen = (int)s.tornadoTrail.size();
                for (int ti = 0; ti < trailLen; ++ti) {
                    float age = (trailLen > 1) ? (float)(trailLen - 1 - ti) / (float)(trailLen - 1) : 0.0f;
                    float trailAlpha = (1.0f - age) * 0.45f;
                    if (trailAlpha < 0.05f) continue;
                    glm::vec2 p = worldToMM(s.tornadoTrail[ti].x, s.tornadoTrail[ti].y);
                    if (inMM(p)) renderDot(p, dotS*0.4f, glm::vec3(0.6f, 0.6f, 0.7f), trailAlpha);
                }
            }
            // Tornado (white cross)
            {
                glm::vec2 tp = worldToMM(s.tornadoPos.x, s.tornadoPos.y);
                float cs = dotS * 2.0f;
                if (inMM(tp)) {
                    renderQuad(tp.x-cs/aspect, tp.y-dotS*0.5f, tp.x+cs/aspect, tp.y+dotS*0.5f,
                               glm::vec3(1.0f, 1.0f, 1.0f));
                    renderQuad(tp.x-dotS*0.5f/aspect, tp.y-cs, tp.x+dotS*0.5f/aspect, tp.y+cs,
                               glm::vec3(1.0f, 1.0f, 1.0f));
                }
            }
            // Player (cyan dot at center)
            renderDot(glm::vec2(mmX, mmY), dotS, glm::vec3(0.0f, 1.0f, 1.0f), 1.0f);
        }

        // ── Tornado fading warning (game over countdown) ──
        if (s.gamePhase == GamePhase::PLAYING && s.minScaleTimer > 0.5f) {
            float remain = GAMEOVER_FADE_TIME - s.minScaleTimer;
            float wPulse = 0.6f + 0.4f * sinf(t * 6.0f);
            snprintf(buf, sizeof(buf), "TORNADA SLABESTE: %d", (int)ceilf(remain));
            float ww = (float)strlen(buf) * 0.028f;
            renderLine(buf, -ww * 0.5f, 0.55f, 0.028f, 0.056f,
                       glm::vec3(1.0f, 0.25f, 0.15f) * wPulse);
        }

        // ── Wave announcement overlay ──
        if (s.gamePhase == GamePhase::WAVE_ANNOUNCE) {
            float alpha = 1.0f;
            if (s.wave.announceTimer > WAVE_ANNOUNCE_TIME - 0.5f)
                alpha = (WAVE_ANNOUNCE_TIME - s.wave.announceTimer) * 2.0f;

            float bigCW = 0.05f, bigCH = 0.1f;
            if (s.timeAttack)
                snprintf(buf, sizeof(buf), "TIME ATTACK");
            else
                snprintf(buf, sizeof(buf), "VALUL %d", s.wave.number);
            float textW = (float)strlen(buf) * bigCW;
            renderLine(buf, -textW * 0.5f, 0.1f, bigCW, bigCH,
                       glm::vec3(0.5f, 0.9f, 1.0f) * alpha);

            float smCW = 0.025f, smCH = 0.05f;
            if (s.timeAttack) {
                const char* sub = "3 MINUTE - SCOR MAXIM";
                float stW = (float)strlen(sub) * smCW;
                renderLine(sub, -stW * 0.5f, -0.02f, smCW, smCH,
                           glm::vec3(1.0f, 0.7f, 0.3f) * alpha);
            } else {
                // EF scale label
                static const glm::vec3 EF_COLORS[6] = {
                    {0.5f,0.9f,0.5f}, {0.8f,0.9f,0.3f}, {1.0f,0.8f,0.2f},
                    {1.0f,0.55f,0.1f}, {1.0f,0.25f,0.05f}, {1.0f,0.1f,0.1f}
                };
                snprintf(buf, sizeof(buf), "TORNADA EF%d", s.wave.efScale);
                float efW = (float)strlen(buf) * 0.032f;
                renderLine(buf, -efW * 0.5f, 0.03f, 0.032f, 0.064f,
                           EF_COLORS[std::clamp(s.wave.efScale, 0, 5)] * alpha);

                snprintf(buf, sizeof(buf), "DISTRUGE %d OBIECTE", s.wave.target);
                float stW = (float)strlen(buf) * smCW;
                renderLine(buf, -stW * 0.5f, -0.05f, smCW, smCH,
                           glm::vec3(0.8f, 0.8f, 0.8f) * alpha);
            }
        }

        // ── Victory screen ──
        if (s.gamePhase == GamePhase::VICTORY) {
            s.victoryTimer += dt;

            // Semi-dark background
            renderQuadA(-0.62f, -0.42f, 0.62f, 0.48f,
                       glm::vec3(0.02f, 0.02f, 0.05f), 0.85f);

            float bigCW = 0.06f, bigCH = 0.12f;
            const char* victoryText = s.timeAttack ? "TIMP EXPIRAT" : "VICTORIE";
            float vw = (float)strlen(victoryText) * bigCW;
            float pulse = 0.7f + 0.3f * sinf(t * 3.0f);
            renderLine(victoryText, -vw * 0.5f, 0.25f, bigCW, bigCH,
                       glm::vec3(1.0f, 0.85f, 0.0f) * pulse);

            float smCW = 0.02f, smCH = 0.04f;
            if (s.timeAttack)
                snprintf(buf, sizeof(buf), "TIME ATTACK - 3 MINUTE");
            else
                snprintf(buf, sizeof(buf), "TOATE CELE %d VALURI COMPLETE", TOTAL_WAVES);
            float bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, 0.17f, smCW, smCH,
                       glm::vec3(0.7f, 0.9f, 1.0f));

            snprintf(buf, sizeof(buf), "SCOR: %d", s.score.scorePoints);
            bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, 0.10f, smCW, smCH,
                       glm::vec3(1.0f, 0.9f, 0.2f));

            snprintf(buf, sizeof(buf), "TOTAL DISTRUSE: %d", s.score.totalDestroyed);
            bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, 0.04f, smCW, smCH,
                       glm::vec3(0.9f, 0.9f, 0.9f));

            snprintf(buf, sizeof(buf), "TORNADA MAX: x%.1f", s.tornadoScale);
            bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, -0.03f, smCW, smCH,
                       glm::vec3(1.0f, 0.5f, 0.3f));

            int hi = getHighScore();
            snprintf(buf, sizeof(buf), "RECORD: %d", hi);
            bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, -0.11f, smCW, smCH,
                       glm::vec3(1.0f, 0.9f, 0.3f));

            if (s.victoryTimer > 2.0f) {
                float blink = (sinf(t * 4.0f) > 0.0f) ? 1.0f : 0.3f;
                const char* copyHint = "APASA C PENTRU COPIERE SCOR";
                float chw = (float)strlen(copyHint) * smCW * 0.85f;
                renderLine(copyHint, -chw * 0.5f, -0.22f, smCW * 0.85f, smCH * 0.85f,
                           glm::vec3(0.4f, 0.9f, 0.9f) * blink);
                const char* restart = "APASA R PENTRU RESTART";
                float rw = (float)strlen(restart) * smCW;
                renderLine(restart, -rw * 0.5f, -0.30f, smCW, smCH,
                           glm::vec3(0.6f, 0.8f, 0.6f) * blink);
                if (!s.timeAttack) {
                    const char* endless = "APASA E PENTRU MOD INFINIT";
                    float ew = (float)strlen(endless) * smCW * 0.85f;
                    renderLine(endless, -ew * 0.5f, -0.38f, smCW * 0.85f, smCH * 0.85f,
                               glm::vec3(0.9f, 0.6f, 1.0f) * blink);
                }
            }
        }

        // ── Game over screen ──
        if (s.gamePhase == GamePhase::GAME_OVER) {
            s.victoryTimer += dt;

            renderQuadA(-0.62f, -0.42f, 0.62f, 0.48f,
                        glm::vec3(0.06f, 0.02f, 0.02f), 0.85f);

            float bigCW = 0.06f, bigCH = 0.12f;
            const char* goText = "GAME OVER";
            float gw = (float)strlen(goText) * bigCW;
            renderLine(goText, -gw * 0.5f, 0.25f, bigCW, bigCH,
                       glm::vec3(1.0f, 0.25f, 0.2f));

            float smCW = 0.02f, smCH = 0.04f;
            const char* sub = "TORNADA S-A RISIPIT";
            float bw2 = (float)strlen(sub) * smCW;
            renderLine(sub, -bw2 * 0.5f, 0.17f, smCW, smCH,
                       glm::vec3(0.8f, 0.7f, 0.7f));

            snprintf(buf, sizeof(buf), "SCOR: %d", s.score.scorePoints);
            bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, 0.10f, smCW, smCH,
                       glm::vec3(1.0f, 0.9f, 0.2f));

            snprintf(buf, sizeof(buf), "VAL ATINS: %d/%d", s.wave.number, TOTAL_WAVES);
            bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, 0.04f, smCW, smCH,
                       glm::vec3(0.7f, 0.85f, 1.0f));

            snprintf(buf, sizeof(buf), "TOTAL DISTRUSE: %d", s.score.totalDestroyed);
            bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, -0.03f, smCW, smCH,
                       glm::vec3(0.9f, 0.9f, 0.9f));

            int hi = getHighScore();
            snprintf(buf, sizeof(buf), "RECORD: %d", hi);
            bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, -0.11f, smCW, smCH,
                       glm::vec3(1.0f, 0.9f, 0.3f));

            if (s.victoryTimer > 1.5f) {
                float blink = (sinf(t * 4.0f) > 0.0f) ? 1.0f : 0.3f;
                const char* restart = "APASA R PENTRU RESTART";
                float rw = (float)strlen(restart) * smCW;
                renderLine(restart, -rw * 0.5f, -0.24f, smCW, smCH,
                           glm::vec3(0.6f, 0.8f, 0.6f) * blink);
            }
        }

        // ── Lightning full-screen flash ──
        if (s.lightning.intensity > 0.01f) {
            float flashAlpha = s.lightning.intensity * 0.28f;
            renderQuadA(-1.0f, -1.0f, 1.0f, 1.0f,
                        glm::vec3(0.9f, 0.95f, 1.0f), flashAlpha);
        }

        // ── Upload the whole HUD once and draw it in a single call ──
        if (!hb.empty()) {
            glUseProgram(s.hudProgram);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s.fontTex);
            glUniform1i(s.hu.fontTex, 0);
            glBindVertexArray(s.hudVAO);
            glBindBuffer(GL_ARRAY_BUFFER, s.hudVBO);
            size_t bytes = hb.size() * sizeof(float);
            if (bytes > s.hudVBOCapacity) {
                s.hudVBOCapacity = bytes * 2;
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)s.hudVBOCapacity,
                             nullptr, GL_STREAM_DRAW);
            }
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, hb.data());
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(hb.size() / 9));
            glBindVertexArray(0);
        }

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    // ── Endless mode on E key (during a normal victory, not Time Attack) ──
    if (s.gamePhase == GamePhase::VICTORY && !s.timeAttack &&
        glfwGetKey(s.window, GLFW_KEY_E) == GLFW_PRESS) {
        s.endlessMode = true;
        s.wave.number++;
        s.wave.target = WAVE_BASE_TARGET + (s.wave.number - 1) * 3;
        s.wave.destroyed = 0;
        s.wave.announceTimer = 0.0f;
        s.wave.efScale = 5;
        s.gamePhase = GamePhase::WAVE_ANNOUNCE;
        s.victoryTimer = 0.0f;
        s.lastDestroyTime = t;
        playWaveSound();
    }

    // ── Restart on R key (during victory or game over) ──
    if (s.gamePhase == GamePhase::VICTORY || s.gamePhase == GamePhase::GAME_OVER) {
        if (glfwGetKey(s.window, GLFW_KEY_R) == GLFW_PRESS) {
            s.score = Score{};
            s.tornadoScale = 1.0f;
            s.lastDestroyTime = t;
            s.wave = Wave{};
            s.wave.announceTimer = 0.0f;
            s.gamePhase = GamePhase::WAVE_ANNOUNCE;
            s.victoryTimer = 0.0f;
            s.minScaleTimer = 0.0f;
            s.endlessMode = false;
            s.timeAttack = false;
            s.timeAttackRemaining = 0.0f;
            s.activePowerUps.clear();
            s.powerUps.clear();
            s.powerUpSpawnTimer = 5.0f;
            for (auto& h : s.houses)      { h.health = 1.0f; h.destroyed = false; }
            for (auto& tr : s.chunkTrees) { tr.health = 1.0f; tr.destroyed = false; }
            for (auto& pr : s.chunkProps) { pr.health = 1.0f; pr.destroyed = false; }
            for (auto& an : s.animals)    { an.health = 1.0f; an.destroyed = false; }
            s.destroyedObjs.clear();
            s.debrisPieces.clear();
            s.scorchMarks.clear();
            s.comboCount = 0;
            s.comboTimer = 0.0f;
            s.comboMultiplier = 1.0f;
            s.tornadoTrail.clear();
            s.trailSampleTimer = 0.0f;
        }
    }

    // -- FPS counter --
    s.fpsFrames++;
    double nowF = glfwGetTime();
    if (nowF - s.fpsTimer >= 1.0) {
        int fps = (int)((double)s.fpsFrames / (nowF - s.fpsTimer));
        s.fpsFrames = 0;
        s.fpsTimer  = nowF;
        std::string title = "Tornada 3D - FPS: " + std::to_string(fps);
        glfwSetWindowTitle(s.window, title.c_str());
    }

#ifndef PLATFORM_EMSCRIPTEN
    glfwSwapBuffers(s.window);
    glfwPollEvents();
#endif
}

// ═════════════════════════════════════════════════════════════════════
// main() — one-time setup, then enter loop
// ═════════════════════════════════════════════════════════════════════
int main() {
    if (!glfwInit()) { std::cerr << "glfwInit failed\n"; return -1; }

#ifdef PLATFORM_EMSCRIPTEN
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif

    GLFWwindow* window = glfwCreateWindow(
        1280, 720, "Tornada 3D - urmareste mouse", nullptr, nullptr);
    if (!window) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    app.window = window;

#ifndef PLATFORM_EMSCRIPTEN
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to init GLAD\n";
        return -1;
    }
#endif

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ── Load & compile main shaders ──
    std::string vertSrc = loadFile("shaders/vertex.glsl");
    std::string fragSrc = loadFile("shaders/fragment.glsl");
    if (vertSrc.empty() || fragSrc.empty()) {
        vertSrc = loadFile("../shaders/vertex.glsl");
        fragSrc = loadFile("../shaders/fragment.glsl");
    }
    if (vertSrc.empty() || fragSrc.empty()) {
        std::cerr << "Couldn't load main shader files.\n"; return -1;
    }
    vertSrc = adaptShaderSource(vertSrc, false);
    fragSrc = adaptShaderSource(fragSrc, true);

    GLuint vs = compileShader(GL_VERTEX_SHADER,   vertSrc.c_str());
    GLuint fs = compileShader(GL_FRAGMENT_SHADER,  fragSrc.c_str());
    app.program = linkProgram(vs, fs);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!app.program) { std::cerr << "Main shader program failed.\n"; return -1; }

    // ── Load & compile particle shaders ──
    std::string pvs = loadFile("shaders/particle_vertex.glsl");
    std::string pfs = loadFile("shaders/particle_fragment.glsl");
    if (pvs.empty() || pfs.empty()) {
        pvs = loadFile("../shaders/particle_vertex.glsl");
        pfs = loadFile("../shaders/particle_fragment.glsl");
    }
    pvs = adaptShaderSource(pvs, false);
    pfs = adaptShaderSource(pfs, true);

    GLuint pvsi = compileShader(GL_VERTEX_SHADER,   pvs.c_str());
    GLuint pfsi = compileShader(GL_FRAGMENT_SHADER,  pfs.c_str());
    app.particleProgram = linkProgram(pvsi, pfsi);
    glDeleteShader(pvsi); glDeleteShader(pfsi);
    if (!app.particleProgram) { std::cerr << "Particle shader program failed.\n"; return -1; }

    // ── Load & compile sky shaders ──
    {
        std::string svs = loadFile("shaders/sky_vertex.glsl");
        std::string sfs = loadFile("shaders/sky_fragment.glsl");
        if (svs.empty() || sfs.empty()) {
            svs = loadFile("../shaders/sky_vertex.glsl");
            sfs = loadFile("../shaders/sky_fragment.glsl");
        }
        svs = adaptShaderSource(svs, false);
        sfs = adaptShaderSource(sfs, true);
        GLuint sv = compileShader(GL_VERTEX_SHADER, svs.c_str());
        GLuint sf = compileShader(GL_FRAGMENT_SHADER, sfs.c_str());
        app.skyProgram = linkProgram(sv, sf);
        glDeleteShader(sv); glDeleteShader(sf);
        if (!app.skyProgram) { std::cerr << "Sky shader program failed.\n"; return -1; }
    }

    // ── Load & compile rain shaders ──
    {
        std::string rvs = loadFile("shaders/rain_vertex.glsl");
        std::string rfs = loadFile("shaders/rain_fragment.glsl");
        if (rvs.empty() || rfs.empty()) {
            rvs = loadFile("../shaders/rain_vertex.glsl");
            rfs = loadFile("../shaders/rain_fragment.glsl");
        }
        rvs = adaptShaderSource(rvs, false);
        rfs = adaptShaderSource(rfs, true);
        GLuint rv = compileShader(GL_VERTEX_SHADER, rvs.c_str());
        GLuint rf = compileShader(GL_FRAGMENT_SHADER, rfs.c_str());
        app.rainProgram = linkProgram(rv, rf);
        glDeleteShader(rv); glDeleteShader(rf);
        if (!app.rainProgram) { std::cerr << "Rain shader program failed.\n"; return -1; }
    }

    // ── Load & compile HUD shaders ──
    {
        std::string hvs = loadFile("shaders/hud_vertex.glsl");
        std::string hfs = loadFile("shaders/hud_fragment.glsl");
        if (hvs.empty() || hfs.empty()) {
            hvs = loadFile("../shaders/hud_vertex.glsl");
            hfs = loadFile("../shaders/hud_fragment.glsl");
        }
        hvs = adaptShaderSource(hvs, false);
        hfs = adaptShaderSource(hfs, true);
        GLuint hv = compileShader(GL_VERTEX_SHADER, hvs.c_str());
        GLuint hf = compileShader(GL_FRAGMENT_SHADER, hfs.c_str());
        app.hudProgram = linkProgram(hv, hf);
        glDeleteShader(hv); glDeleteShader(hf);
        if (!app.hudProgram) { std::cerr << "HUD shader program failed.\n"; return -1; }
    }

    // ── Cache uniform locations ──
    {
        GLuint p = app.program;
        auto& m = app.mu;
        m.proj        = glGetUniformLocation(p, "uProj");
        m.view        = glGetUniformLocation(p, "uView");
        m.model       = glGetUniformLocation(p, "uModel");
        m.normalMat   = glGetUniformLocation(p, "uNormalMat");
        m.time        = glGetUniformLocation(p, "uTime");
        m.camPos      = glGetUniformLocation(p, "uCamPos");
        m.enableSwirl = glGetUniformLocation(p, "uEnableSwirl");
        m.tint        = glGetUniformLocation(p, "uTint");
        m.opacity     = glGetUniformLocation(p, "uOpacity");
        m.objType     = glGetUniformLocation(p, "uObjType");
        m.hasAlbedo   = glGetUniformLocation(p, "uHasAlbedo");
        m.albedo      = glGetUniformLocation(p, "uAlbedo");
        m.lightningFlash = glGetUniformLocation(p, "uLightningFlash");
        m.windBend    = glGetUniformLocation(p, "uWindBend");
        m.windSource  = glGetUniformLocation(p, "uWindSource");
        m.timeOfDay   = glGetUniformLocation(p, "uTimeOfDay");
        m.waterLevel  = glGetUniformLocation(p, "uWaterLevel");
    }
    {
        GLuint p = app.particleProgram;
        auto& u = app.pu;
        u.proj       = glGetUniformLocation(p, "uProj");
        u.view       = glGetUniformLocation(p, "uView");
        u.model      = glGetUniformLocation(p, "uModel");
        u.color      = glGetUniformLocation(p, "uColor");
        u.pointScale = glGetUniformLocation(p, "uPointScale");
    }
    // Sky shader uniforms
    {
        GLuint p = app.skyProgram;
        app.su.lightningFlash = glGetUniformLocation(p, "uLightningFlash");
        app.su.time           = glGetUniformLocation(p, "uTime");
        app.su.timeOfDay      = glGetUniformLocation(p, "uTimeOfDay");
    }
    // Rain shader uniforms
    {
        GLuint p = app.rainProgram;
        app.ru.proj = glGetUniformLocation(p, "uProj");
        app.ru.view = glGetUniformLocation(p, "uView");
    }
    // HUD shader uniforms
    {
        GLuint p = app.hudProgram;
        app.hu.fontTex = glGetUniformLocation(p, "uFontTex");
    }

    // ══════════════════════════════════════
    // Procedural textures
    // ══════════════════════════════════════
    // Brick
    {
        const int CX = 32, CY = 32;
        std::vector<unsigned char> px(CX * CY * 3);
        for (int y = 0; y < CY; ++y) for (int x = 0; x < CX; ++x) {
            int i = (y*CX+x)*3;
            int bx = x/6, by = y/6;
            bool mortar = ((bx+by)%2==0) ? (x%6==0||y%6==0) : (x%6==0);
            if (mortar) { px[i]=200; px[i+1]=180; px[i+2]=165; }
            else { px[i]=(unsigned char)(155+(bx%3)*10); px[i+1]=(unsigned char)(60+(by%2)*15); px[i+2]=40; }
        }
        glGenTextures(1, &app.brickTex);
        glBindTexture(GL_TEXTURE_2D, app.brickTex);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,CX,CY,0,GL_RGB,GL_UNSIGNED_BYTE,px.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    }
    // Leaf
    {
        const int CX = 32, CY = 32;
        std::vector<unsigned char> px(CX*CY*3);
        for (int y = 0; y < CY; ++y) for (int x = 0; x < CX; ++x) {
            int i = (y*CX+x)*3;
            unsigned char g = 80 + (unsigned char)((x*37+y*23)%120);
            px[i]   = (unsigned char)(g*0.4f);
            px[i+1] = g;
            px[i+2] = (unsigned char)(g*0.6f);
        }
        glGenTextures(1, &app.leafTex);
        glBindTexture(GL_TEXTURE_2D, app.leafTex);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,CX,CY,0,GL_RGB,GL_UNSIGNED_BYTE,px.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    }

    // ══════════════════════════════════════
    // Procedural bitmap font (16x6 grid, chars 32-127)
    // ══════════════════════════════════════
    {
        // Tiny 5x7 bitmap patterns for printable ASCII
        // Each char is 5 wide, 7 tall, packed in a 16x6 grid → 80x42 texture
        const int GW = 5, GH = 7, COLS = 16, ROWS = 6;
        const int TW = GW * COLS, TH = GH * ROWS;
        std::vector<unsigned char> font(TW * TH, 0);

        // Minimal bitmaps for digits 0-9, A-Z, a-z, and some punctuation
        // Each string is 7 rows of 5 bits
        for (auto& g : FONT_GLYPHS) {
            int idx = g.ch - 32;
            int col = idx % COLS, row = idx / COLS;
            for (int gy = 0; gy < GH; ++gy) {
                for (int gx = 0; gx < GW; ++gx) {
                    if (g.bits[gy][gx] == '1') {
                        int px_x = col * GW + gx;
                        int px_y = row * GH + gy;
                        font[px_y * TW + px_x] = 255;
                    }
                }
            }
        }

        glGenTextures(1, &app.fontTex);
        glBindTexture(GL_TEXTURE_2D, app.fontTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, TW, TH, 0, GL_RED, GL_UNSIGNED_BYTE, font.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // ══════════════════════════════════════
    // HUD VAO/VBO (dynamic text quads)
    // ══════════════════════════════════════
    {
        glGenVertexArrays(1, &app.hudVAO);
        glGenBuffers(1, &app.hudVBO);
        glBindVertexArray(app.hudVAO);
        glBindBuffer(GL_ARRAY_BUFFER, app.hudVBO);
        app.hudVBOCapacity = 4096 * 9 * sizeof(float);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)app.hudVBOCapacity, nullptr, GL_STREAM_DRAW);
        // layout: vec2 pos, vec2 uv, vec4 color, float mode per vertex
        const GLsizei stride = 9 * sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(2*sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(4*sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)(8*sizeof(float)));
        glBindVertexArray(0);
    }

    // ══════════════════════════════════════
    // Prop meshes: fence, car, pole
    // ══════════════════════════════════════
    {
        // Helper to create a simple box mesh with per-face normals
        auto makeBox = [](SimpleModel& mdl, float hw, float hh, float hd, glm::vec3 color) {
            struct PV { glm::vec3 p; glm::vec3 n; };
            std::vector<PV> verts;
            std::vector<unsigned int> idx;
            auto addFace = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 n) {
                unsigned int base = (unsigned int)verts.size();
                verts.push_back({a,n}); verts.push_back({b,n});
                verts.push_back({c,n}); verts.push_back({d,n});
                idx.push_back(base); idx.push_back(base+1); idx.push_back(base+2);
                idx.push_back(base); idx.push_back(base+2); idx.push_back(base+3);
            };
            glm::vec3 v[8] = {
                {-hw,0,-hd},{hw,0,-hd},{hw,0,hd},{-hw,0,hd},
                {-hw,hh,-hd},{hw,hh,-hd},{hw,hh,hd},{-hw,hh,hd}
            };
            addFace(v[3],v[2],v[1],v[0], { 0,-1, 0});
            addFace(v[4],v[5],v[6],v[7], { 0, 1, 0});
            addFace(v[0],v[1],v[5],v[4], { 0, 0,-1});
            addFace(v[1],v[2],v[6],v[5], { 1, 0, 0});
            addFace(v[2],v[3],v[7],v[6], { 0, 0, 1});
            addFace(v[3],v[0],v[4],v[7], {-1, 0, 0});

            glGenVertexArrays(1, &mdl.vao);
            glGenBuffers(1, &mdl.vbo);
            glGenBuffers(1, &mdl.ebo);
            glBindVertexArray(mdl.vao);
            glBindBuffer(GL_ARRAY_BUFFER, mdl.vbo);
            glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(PV), verts.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mdl.ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size()*sizeof(unsigned int), idx.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(PV),(void*)offsetof(PV,p));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(PV),(void*)offsetof(PV,n));
            glDisableVertexAttribArray(2);
            glVertexAttrib3f(2, color.x, color.y, color.z);
            glBindVertexArray(0);
            mdl.indexCount = (GLsizei)idx.size();
        };

        // Fence: flat wide, low
        makeBox(app.fence, 1.0f, 0.5f, 0.05f, glm::vec3(0.6f, 0.45f, 0.3f));
        // Car: wider low box
        makeBox(app.car, 0.6f, 0.45f, 0.3f, glm::vec3(0.7f, 0.15f, 0.1f));
        // Pole: thin tall
        makeBox(app.pole, 0.05f, 2.5f, 0.05f, glm::vec3(0.5f, 0.5f, 0.55f));
    }

    // ══════════════════════════════════════
    // Cow mesh: body + head + legs (faces +X)
    // ══════════════════════════════════════
    {
        struct PV { glm::vec3 p; glm::vec3 n; };
        std::vector<PV> cv;
        std::vector<unsigned int> ci;
        auto addBox = [&](glm::vec3 c, glm::vec3 h) { // center + half-extents
            glm::vec3 v[8] = {
                {c.x-h.x,c.y-h.y,c.z-h.z},{c.x+h.x,c.y-h.y,c.z-h.z},
                {c.x+h.x,c.y-h.y,c.z+h.z},{c.x-h.x,c.y-h.y,c.z+h.z},
                {c.x-h.x,c.y+h.y,c.z-h.z},{c.x+h.x,c.y+h.y,c.z-h.z},
                {c.x+h.x,c.y+h.y,c.z+h.z},{c.x-h.x,c.y+h.y,c.z+h.z}
            };
            auto face = [&](int a, int b, int cc, int d, glm::vec3 n) {
                unsigned int base = (unsigned int)cv.size();
                cv.push_back({v[a],n}); cv.push_back({v[b],n});
                cv.push_back({v[cc],n}); cv.push_back({v[d],n});
                ci.insert(ci.end(), {base, base+1, base+2, base, base+2, base+3});
            };
            face(3,2,1,0,{0,-1,0}); face(4,5,6,7,{0,1,0});
            face(0,1,5,4,{0,0,-1}); face(1,2,6,5,{1,0,0});
            face(2,3,7,6,{0,0,1});  face(3,0,4,7,{-1,0,0});
        };
        addBox(glm::vec3(0.0f, 0.45f, 0.0f), glm::vec3(0.45f, 0.22f, 0.22f)); // body
        addBox(glm::vec3(0.50f, 0.64f, 0.0f), glm::vec3(0.14f, 0.13f, 0.12f)); // head
        addBox(glm::vec3( 0.30f, 0.12f,  0.13f), glm::vec3(0.05f, 0.12f, 0.05f)); // legs
        addBox(glm::vec3( 0.30f, 0.12f, -0.13f), glm::vec3(0.05f, 0.12f, 0.05f));
        addBox(glm::vec3(-0.30f, 0.12f,  0.13f), glm::vec3(0.05f, 0.12f, 0.05f));
        addBox(glm::vec3(-0.30f, 0.12f, -0.13f), glm::vec3(0.05f, 0.12f, 0.05f));

        glGenVertexArrays(1, &app.cow.vao);
        glGenBuffers(1, &app.cow.vbo);
        glGenBuffers(1, &app.cow.ebo);
        glBindVertexArray(app.cow.vao);
        glBindBuffer(GL_ARRAY_BUFFER, app.cow.vbo);
        glBufferData(GL_ARRAY_BUFFER, cv.size()*sizeof(PV), cv.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, app.cow.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, ci.size()*sizeof(unsigned int), ci.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(PV),(void*)offsetof(PV,p));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(PV),(void*)offsetof(PV,n));
        glDisableVertexAttribArray(2);
        glVertexAttrib3f(2, 1.0f, 1.0f, 1.0f);
        glBindVertexArray(0);
        app.cow.indexCount = (GLsizei)ci.size();
    }

    // ══════════════════════════════════════
    // Tornado mesh — stacked rings (inverted cone)
    // ══════════════════════════════════════
    {
        const int segments = TORNADO_SEGMENTS;
        const int rings    = TORNADO_RINGS;
        const float ht     = TORNADO_HEIGHT;
        const float baseR  = TORNADO_BASE_R;

        std::vector<Vertex> verts;
        std::vector<unsigned int> idx;

        for (int r = 0; r <= rings; ++r) {
            float rt = (float)r / (float)rings;
            float y  = rt * ht;
            float rad = baseR * rt + 0.05f;
            for (int ss = 0; ss <= segments; ++ss) {
                float ang = ss / (float)segments * 2.0f * (float)M_PI;
                float ca = cosf(ang), sa = sinf(ang);
                glm::vec3 p(rad*ca, y, rad*sa);
                float dr_dy = baseR / ht;
                glm::vec3 n = glm::normalize(glm::vec3(ca, dr_dy, sa));
                glm::vec3 c = glm::vec3(0.5f, 0.5f, 0.6f) * (0.4f + 0.6f * rt);
                verts.push_back({p, n, c});
            }
        }
        for (int r = 0; r < rings; ++r) for (int ss = 0; ss < segments; ++ss) {
            unsigned int i0 = r*(segments+1)+ss;
            unsigned int i1 = (r+1)*(segments+1)+ss;
            unsigned int i2 = r*(segments+1)+(ss+1);
            unsigned int i3 = (r+1)*(segments+1)+(ss+1);
            idx.push_back(i0); idx.push_back(i1); idx.push_back(i2);
            idx.push_back(i2); idx.push_back(i1); idx.push_back(i3);
        }

        glGenVertexArrays(1, &app.tornadoVAO);
        glGenBuffers(1, &app.tornadoVBO);
        glGenBuffers(1, &app.tornadoEBO);
        glBindVertexArray(app.tornadoVAO);
        glBindBuffer(GL_ARRAY_BUFFER, app.tornadoVBO);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(Vertex), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, app.tornadoEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size()*sizeof(unsigned int), idx.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)offsetof(Vertex,pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)offsetof(Vertex,normal));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)offsetof(Vertex,col));
        glBindVertexArray(0);
        app.tornadoIndexCount = (GLsizei)idx.size();
    }

    // ══════════════════════════════════════
    // Terrain heightmap grid
    // ══════════════════════════════════════
    {
        const int GN = TERRAIN_GRID + 1; // vertices per axis
        // Static topology; vertex data is filled in by buildTerrainMesh()
        std::vector<unsigned int> gi;
        gi.reserve(TERRAIN_GRID * TERRAIN_GRID * 6);
        for (int iz = 0; iz < TERRAIN_GRID; ++iz) {
            for (int ix = 0; ix < TERRAIN_GRID; ++ix) {
                unsigned int i0 = iz * GN + ix;
                unsigned int i1 = iz * GN + ix + 1;
                unsigned int i2 = (iz+1) * GN + ix;
                unsigned int i3 = (iz+1) * GN + ix + 1;
                gi.push_back(i0); gi.push_back(i1); gi.push_back(i2);
                gi.push_back(i2); gi.push_back(i1); gi.push_back(i3);
            }
        }
        app.terrainIndexCount = (GLsizei)gi.size();

        glGenVertexArrays(1, &app.groundVAO);
        glGenBuffers(1, &app.groundVBO);
        glGenBuffers(1, &app.groundEBO);
        glBindVertexArray(app.groundVAO);
        glBindBuffer(GL_ARRAY_BUFFER, app.groundVBO);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)GN * GN * sizeof(SceneVert)),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, app.groundEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, gi.size()*sizeof(unsigned int), gi.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(SceneVert),(void*)offsetof(SceneVert,pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(SceneVert),(void*)offsetof(SceneVert,normal));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,sizeof(SceneVert),(void*)offsetof(SceneVert,col));
        glBindVertexArray(0);

        buildTerrainMesh(0.0f, 0.0f);
    }

    // ══════════════════════════════════════
    // Water plane
    // ══════════════════════════════════════
    {
        struct SV { glm::vec3 pos; glm::vec3 normal; glm::vec3 col; };
        const int WN = 33; // water grid resolution
        const float WE = TERRAIN_EXTENT;
        float wstep = (WE * 2.0f) / (WN - 1);
        std::vector<SV> wv(WN * WN);
        for (int iz = 0; iz < WN; ++iz) {
            for (int ix = 0; ix < WN; ++ix) {
                float x = -WE + ix * wstep;
                float z = -WE + iz * wstep;
                wv[iz * WN + ix] = {glm::vec3(x, WATER_LEVEL, z),
                                     glm::vec3(0, 1, 0),
                                     glm::vec3(0.15f, 0.35f, 0.55f)};
            }
        }
        std::vector<unsigned int> wi;
        wi.reserve((WN-1) * (WN-1) * 6);
        for (int iz = 0; iz < WN-1; ++iz) {
            for (int ix = 0; ix < WN-1; ++ix) {
                unsigned int i0 = iz * WN + ix;
                unsigned int i1 = iz * WN + ix + 1;
                unsigned int i2 = (iz+1) * WN + ix;
                unsigned int i3 = (iz+1) * WN + ix + 1;
                wi.push_back(i0); wi.push_back(i1); wi.push_back(i2);
                wi.push_back(i2); wi.push_back(i1); wi.push_back(i3);
            }
        }
        app.waterIndexCount = (GLsizei)wi.size();

        glGenVertexArrays(1, &app.waterVAO);
        glGenBuffers(1, &app.waterVBO);
        glGenBuffers(1, &app.waterEBO);
        glBindVertexArray(app.waterVAO);
        glBindBuffer(GL_ARRAY_BUFFER, app.waterVBO);
        glBufferData(GL_ARRAY_BUFFER, wv.size()*sizeof(SV), wv.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, app.waterEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, wi.size()*sizeof(unsigned int), wi.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)offsetof(SV,pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)offsetof(SV,normal));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)offsetof(SV,col));
        glBindVertexArray(0);
    }

    // ══════════════════════════════════════
    // Scorch mark quad (simple flat square)
    // ══════════════════════════════════════
    {
        struct SV { glm::vec3 pos; glm::vec3 normal; glm::vec3 col; };
        std::vector<SV> sv = {
            {{-1,0,-1},{0,1,0},{0.15f,0.45f,0.2f}},
            {{ 1,0,-1},{0,1,0},{0.15f,0.45f,0.2f}},
            {{ 1,0, 1},{0,1,0},{0.15f,0.45f,0.2f}},
            {{-1,0, 1},{0,1,0},{0.15f,0.45f,0.2f}},
        };
        std::vector<unsigned int> si = {0,1,2, 0,2,3};
        glGenVertexArrays(1, &app.scorchVAO);
        glGenBuffers(1, &app.scorchVBO);
        glGenBuffers(1, &app.scorchEBO);
        glBindVertexArray(app.scorchVAO);
        glBindBuffer(GL_ARRAY_BUFFER, app.scorchVBO);
        glBufferData(GL_ARRAY_BUFFER, sv.size()*sizeof(SV), sv.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, app.scorchEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, si.size()*sizeof(unsigned int), si.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)offsetof(SV,pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)offsetof(SV,normal));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)offsetof(SV,col));
        glBindVertexArray(0);
    }

    // ══════════════════════════════════════
    // Procedural house (per-face normals)
    // ══════════════════════════════════════
    {
        struct PV { glm::vec3 p; glm::vec3 n; glm::vec2 uv; };
        std::vector<PV> hv;
        std::vector<unsigned int> hi;
        float hw=0.6f, hh=1.0f, hd=0.6f;

        // Helper: add a quad with correct face normal and UVs
        auto addQuad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                           glm::vec3 n) {
            unsigned int base = (unsigned int)hv.size();
            hv.push_back({a, n, {0,0}});
            hv.push_back({b, n, {1,0}});
            hv.push_back({c, n, {1,1}});
            hv.push_back({d, n, {0,1}});
            hi.push_back(base); hi.push_back(base+1); hi.push_back(base+2);
            hi.push_back(base); hi.push_back(base+2); hi.push_back(base+3);
        };

        // Box corners: bottom 0-3, top 4-7
        glm::vec3 v[8] = {
            {-hw,0,-hd},{hw,0,-hd},{hw,0,hd},{-hw,0,hd},
            {-hw,hh,-hd},{hw,hh,-hd},{hw,hh,hd},{-hw,hh,hd}
        };

        addQuad(v[3],v[2],v[1],v[0], {0,-1,0}); // bottom
        addQuad(v[4],v[5],v[6],v[7], {0, 1,0}); // top
        addQuad(v[0],v[1],v[5],v[4], {0, 0,-1}); // front (-z)
        addQuad(v[1],v[2],v[6],v[5], {1, 0, 0}); // right (+x)
        addQuad(v[2],v[3],v[7],v[6], {0, 0, 1}); // back  (+z)
        addQuad(v[3],v[0],v[4],v[7], {-1,0, 0}); // left  (-x)

        // Roof pyramid
        glm::vec3 apex(0, hh+0.6f, 0);
        auto addTri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c) {
            glm::vec3 n = glm::normalize(glm::cross(b-a, c-a));
            unsigned int base = (unsigned int)hv.size();
            hv.push_back({a, n, {0,0}});
            hv.push_back({b, n, {1,0}});
            hv.push_back({c, n, {0.5f,1}});
            hi.push_back(base); hi.push_back(base+1); hi.push_back(base+2);
        };
        addTri(v[4], v[5], apex);
        addTri(v[5], v[6], apex);
        addTri(v[6], v[7], apex);
        addTri(v[7], v[4], apex);

        glGenVertexArrays(1, &app.house.vao);
        glGenBuffers(1, &app.house.vbo);
        glGenBuffers(1, &app.house.ebo);
        glBindVertexArray(app.house.vao);
        glBindBuffer(GL_ARRAY_BUFFER, app.house.vbo);
        glBufferData(GL_ARRAY_BUFFER, hv.size()*sizeof(PV), hv.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, app.house.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, hi.size()*sizeof(unsigned int), hi.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(PV),(void*)offsetof(PV,p));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(PV),(void*)offsetof(PV,n));
        glDisableVertexAttribArray(2);
        glVertexAttrib3f(2, 0.85f, 0.85f, 0.85f);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3,2,GL_FLOAT,GL_FALSE,sizeof(PV),(void*)offsetof(PV,uv));
        glBindVertexArray(0);
        app.house.indexCount = (GLsizei)hi.size();
    }

    // ══════════════════════════════════════
    // Procedural tree (trunk with per-face normals + foliage cones)
    // ══════════════════════════════════════
    {
        struct PV { glm::vec3 p; glm::vec3 n; glm::vec2 uv; };
        std::vector<PV> tv;
        std::vector<unsigned int> ti;
        float tw=0.15f, th=0.5f;

        // Helper: add a quad with correct face normal
        auto addQuad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                           glm::vec3 n) {
            unsigned int base = (unsigned int)tv.size();
            tv.push_back({a, n, {0,0}});
            tv.push_back({b, n, {1,0}});
            tv.push_back({c, n, {1,1}});
            tv.push_back({d, n, {0,1}});
            ti.push_back(base); ti.push_back(base+1); ti.push_back(base+2);
            ti.push_back(base); ti.push_back(base+2); ti.push_back(base+3);
        };

        // Trunk box corners
        glm::vec3 v[8] = {
            {-tw,0,-tw},{tw,0,-tw},{tw,0,tw},{-tw,0,tw},
            {-tw,th,-tw},{tw,th,-tw},{tw,th,tw},{-tw,th,tw}
        };

        addQuad(v[3],v[2],v[1],v[0], {0,-1,0}); // bottom
        addQuad(v[4],v[5],v[6],v[7], {0, 1,0}); // top
        addQuad(v[0],v[1],v[5],v[4], {0, 0,-1});
        addQuad(v[1],v[2],v[6],v[5], {1, 0, 0});
        addQuad(v[2],v[3],v[7],v[6], {0, 0, 1});
        addQuad(v[3],v[0],v[4],v[7], {-1,0, 0});

        // Foliage: layered cones with computed normals
        int seg = 6;
        for (int layer = 0; layer < 3; ++layer) {
            float baseY  = th + layer*0.25f + 0.1f;
            float radius = 0.6f - layer*0.15f;
            float coneH  = 0.4f;
            glm::vec3 apex(0, baseY+coneH, 0);
            for (int ss = 0; ss < seg; ++ss) {
                float a0 = ss/(float)seg * 2.0f * (float)M_PI;
                float a1 = ((ss+1)%seg)/(float)seg * 2.0f * (float)M_PI;
                glm::vec3 p0(cosf(a0)*radius, baseY, sinf(a0)*radius);
                glm::vec3 p1(cosf(a1)*radius, baseY, sinf(a1)*radius);
                glm::vec3 n = glm::normalize(glm::cross(p1-p0, apex-p0));
                unsigned int base = (unsigned int)tv.size();
                tv.push_back({p0, n, {0,0}});
                tv.push_back({p1, n, {1,0}});
                tv.push_back({apex, n, {0.5f,1}});
                ti.push_back(base); ti.push_back(base+1); ti.push_back(base+2);
            }
        }

        glGenVertexArrays(1, &app.tree.vao);
        glGenBuffers(1, &app.tree.vbo);
        glGenBuffers(1, &app.tree.ebo);
        glBindVertexArray(app.tree.vao);
        glBindBuffer(GL_ARRAY_BUFFER, app.tree.vbo);
        glBufferData(GL_ARRAY_BUFFER, tv.size()*sizeof(PV), tv.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, app.tree.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, ti.size()*sizeof(unsigned int), ti.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(PV),(void*)offsetof(PV,p));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(PV),(void*)offsetof(PV,n));
        glDisableVertexAttribArray(2);
        glVertexAttrib3f(2, 0.8f, 0.9f, 0.8f);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3,2,GL_FLOAT,GL_FALSE,sizeof(PV),(void*)offsetof(PV,uv));
        glBindVertexArray(0);
        app.tree.indexCount = (GLsizei)ti.size();
    }

    // ══════════════════════════════════════
    // Sky — empty VAO for fullscreen triangle via gl_VertexID
    // ══════════════════════════════════════
    glGenVertexArrays(1, &app.skyVAO);

    // ══════════════════════════════════════
    // Debris cube (unit cube for tumbling debris pieces)
    // ══════════════════════════════════════
    {
        struct DV { glm::vec3 p; glm::vec3 n; };
        std::vector<DV> dv;
        std::vector<unsigned int> di;
        auto addFace = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                            glm::vec3 n) {
            unsigned int base = (unsigned int)dv.size();
            dv.push_back({a, n}); dv.push_back({b, n}); dv.push_back({c, n}); dv.push_back({d, n});
            di.push_back(base); di.push_back(base+1); di.push_back(base+2);
            di.push_back(base); di.push_back(base+2); di.push_back(base+3);
        };
        float h = 0.5f;
        glm::vec3 v[8] = {
            {-h,-h,-h},{h,-h,-h},{h,-h,h},{-h,-h,h},
            {-h, h,-h},{h, h,-h},{h, h,h},{-h, h,h}
        };
        addFace(v[3],v[2],v[1],v[0], { 0,-1, 0});
        addFace(v[4],v[5],v[6],v[7], { 0, 1, 0});
        addFace(v[0],v[1],v[5],v[4], { 0, 0,-1});
        addFace(v[1],v[2],v[6],v[5], { 1, 0, 0});
        addFace(v[2],v[3],v[7],v[6], { 0, 0, 1});
        addFace(v[3],v[0],v[4],v[7], {-1, 0, 0});

        glGenVertexArrays(1, &app.debrisCube.vao);
        glGenBuffers(1, &app.debrisCube.vbo);
        glGenBuffers(1, &app.debrisCube.ebo);
        glBindVertexArray(app.debrisCube.vao);
        glBindBuffer(GL_ARRAY_BUFFER, app.debrisCube.vbo);
        glBufferData(GL_ARRAY_BUFFER, dv.size()*sizeof(DV), dv.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, app.debrisCube.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, di.size()*sizeof(unsigned int), di.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(DV),(void*)offsetof(DV,p));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(DV),(void*)offsetof(DV,n));
        glVertexAttrib3f(2, 0.7f, 0.7f, 0.7f); // default vertex color
        glBindVertexArray(0);
        app.debrisCube.indexCount = (GLsizei)di.size();
    }

    // ══════════════════════════════════════
    // Initialize world chunks around start position
    // ══════════════════════════════════════
    updateChunks(app.camera.pos);

    // ══════════════════════════════════════
    // Rain particles + VAO/VBO
    // ══════════════════════════════════════
    {
        app.rainDrops.resize(MAX_RAIN);
        app.rainBuf.resize(MAX_RAIN * 4);
        glm::vec3 startCam = app.camera.pos;
        for (auto& r : app.rainDrops) respawnRain(r, startCam);

        glGenVertexArrays(1, &app.rainVAO);
        glGenBuffers(1, &app.rainVBO);
        glBindVertexArray(app.rainVAO);
        glBindBuffer(GL_ARRAY_BUFFER, app.rainVBO);
        glBufferData(GL_ARRAY_BUFFER, MAX_RAIN * 4 * sizeof(float), nullptr, GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(3*sizeof(float)));
        glBindVertexArray(0);
    }

    // ══════════════════════════════════════
    // Particles
    // ══════════════════════════════════════
    app.particles.resize(MAX_PARTICLES);
    app.particleBuf.resize(MAX_PARTICLES * 4);
    app.rng.seed((unsigned)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    for (int i = 0; i < INNER_PARTICLES; ++i)  respawnParticle(app.particles[i], true);
    for (int i = INNER_PARTICLES; i < MAX_PARTICLES; ++i) respawnParticle(app.particles[i], false);

    glGenVertexArrays(1, &app.particleVAO);
    glGenBuffers(1, &app.particleVBO);
    glBindVertexArray(app.particleVAO);
    glBindBuffer(GL_ARRAY_BUFFER, app.particleVBO);
    glBufferData(GL_ARRAY_BUFFER, MAX_PARTICLES * 4 * sizeof(float), nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(3*sizeof(float)));
    glBindVertexArray(0);

    // ── Timing init ──
    app.startTime = (float)glfwGetTime();
    app.lastT     = glfwGetTime();
    app.fpsTimer  = glfwGetTime();
    app.lastDestroyTime = app.startTime;
    app.wave.target = WAVE_BASE_TARGET;
    app.gamePhase = GamePhase::WAVE_ANNOUNCE;

    // ══════════════════════════════════════
    // Enter main loop
    // ══════════════════════════════════════
    std::cout << "Tornado simulation started. WASD to move, RMB to look around, mouse to steer tornado.\n";

#ifdef PLATFORM_EMSCRIPTEN
    emscripten_set_main_loop(main_loop, 0, 1);
#else
    while (!glfwWindowShouldClose(window))
        main_loop();
#endif

    // ── Cleanup ──
    glDeleteBuffers(1, &app.tornadoVBO);
    glDeleteBuffers(1, &app.tornadoEBO);
    glDeleteVertexArrays(1, &app.tornadoVAO);
    glDeleteBuffers(1, &app.groundVBO);
    glDeleteBuffers(1, &app.groundEBO);
    glDeleteVertexArrays(1, &app.groundVAO);
    glDeleteBuffers(1, &app.waterVBO);
    glDeleteBuffers(1, &app.waterEBO);
    glDeleteVertexArrays(1, &app.waterVAO);
    glDeleteBuffers(1, &app.scorchVBO);
    glDeleteBuffers(1, &app.scorchEBO);
    glDeleteVertexArrays(1, &app.scorchVAO);
    if (app.house.vao) { glDeleteBuffers(1,&app.house.vbo); glDeleteBuffers(1,&app.house.ebo); glDeleteVertexArrays(1,&app.house.vao); }
    if (app.tree.vao)  { glDeleteBuffers(1,&app.tree.vbo);  glDeleteBuffers(1,&app.tree.ebo);  glDeleteVertexArrays(1,&app.tree.vao);  }
    if (app.debrisCube.vao) { glDeleteBuffers(1,&app.debrisCube.vbo); glDeleteBuffers(1,&app.debrisCube.ebo); glDeleteVertexArrays(1,&app.debrisCube.vao); }
    glDeleteBuffers(1, &app.particleVBO);
    glDeleteVertexArrays(1, &app.particleVAO);
    glDeleteBuffers(1, &app.rainVBO);
    glDeleteVertexArrays(1, &app.rainVAO);
    glDeleteVertexArrays(1, &app.skyVAO);
    glDeleteTextures(1, &app.brickTex);
    glDeleteTextures(1, &app.leafTex);
    glDeleteTextures(1, &app.fontTex);
    glDeleteBuffers(1, &app.hudVBO);
    glDeleteVertexArrays(1, &app.hudVAO);
    glDeleteProgram(app.hudProgram);
    if (app.fence.vao) { glDeleteBuffers(1,&app.fence.vbo); glDeleteBuffers(1,&app.fence.ebo); glDeleteVertexArrays(1,&app.fence.vao); }
    if (app.car.vao) { glDeleteBuffers(1,&app.car.vbo); glDeleteBuffers(1,&app.car.ebo); glDeleteVertexArrays(1,&app.car.vao); }
    if (app.pole.vao) { glDeleteBuffers(1,&app.pole.vbo); glDeleteBuffers(1,&app.pole.ebo); glDeleteVertexArrays(1,&app.pole.vao); }
    if (app.cow.vao) { glDeleteBuffers(1,&app.cow.vbo); glDeleteBuffers(1,&app.cow.ebo); glDeleteVertexArrays(1,&app.cow.vao); }
    glDeleteProgram(app.particleProgram);
    glDeleteProgram(app.skyProgram);
    glDeleteProgram(app.rainProgram);
    glDeleteProgram(app.program);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
