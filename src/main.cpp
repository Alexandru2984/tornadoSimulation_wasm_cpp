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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// For getcwd fallback paths (works on both POSIX & Emscripten)
#ifdef _WIN32
  #include <direct.h>
  #define GET_CWD _getcwd
#else
  #include <unistd.h>
  #include <limits.h>
  #define GET_CWD getcwd
#endif

#include "gltf_loader.h"

// ── Constants ────────────────────────────────────────────────────────
static const int   MAX_PARTICLES   = 2200;
static const int   INNER_PARTICLES = 1400;

// Tornado mesh
static const int   TORNADO_SEGMENTS = 64;
static const int   TORNADO_RINGS    = 40;
static const float TORNADO_HEIGHT   = 6.0f;
static const float TORNADO_BASE_R   = 1.5f;

// Vortex physics
static const float VORTEX_INNER    = 4.0f;
static const float VORTEX_OUTER    = 2.0f;
static const float UPLIFT_INNER    = 2.5f;
static const float UPLIFT_OUTER    = 1.0f;
static const float PULL_INNER      = 1.5f;
static const float PULL_OUTER      = 0.5f;
static const float VEL_DAMPING     = 2.0f;

// Destruction
static const int   MAX_DEBRIS          = 500;
static const float DESTRUCTION_RADIUS  = 3.5f;
static const float DAMAGE_RATE         = 2.0f;
static const int   DEBRIS_PER_HOUSE    = 40;
static const int   DEBRIS_PER_TREE     = 15;
static const float DEBRIS_LIFETIME     = 5.0f;

// Tornado growth
static const float TORNADO_GROWTH_PER_OBJ = 0.04f; // scale bump per destroyed object
static const float TORNADO_MAX_SCALE      = 3.0f;

// EF-scale destruction radius multipliers (index = EF0..EF5)
static const float EF_RADIUS_MULT[6] = {0.75f, 0.85f, 1.0f, 1.15f, 1.3f, 1.5f};

// Weather
static const int   MAX_RAIN            = 4000;
static const float RAIN_AREA           = 30.0f;
static const float RAIN_HEIGHT         = 15.0f;
static const float RAIN_SPEED          = 8.0f;
static const float LIGHTNING_MIN_INTERVAL = 2.0f;
static const float LIGHTNING_MAX_INTERVAL = 7.0f;
static const float LIGHTNING_DECAY       = 8.0f;

// World chunks (procedural infinite world)
static const float CHUNK_SIZE            = 20.0f;
static const int   CHUNK_RADIUS          = 3;      // chunks around player
static const int   HOUSES_PER_CHUNK      = 3;
static const int   TREES_PER_CHUNK       = 5;
static const int   FENCES_PER_CHUNK      = 4;
static const int   CARS_PER_CHUNK        = 1;
static const int   POLES_PER_CHUNK       = 2;
static const int   MAX_SCORCH_MARKS      = 200;

// Tornado decay (shrinks when idle)
static const float TORNADO_DECAY_RATE    = 0.03f;  // per second
static const float TORNADO_MIN_SCALE     = 0.4f;   // smallest possible
static const float TORNADO_DECAY_GRACE   = 2.0f;   // seconds after last destroy before decay starts

// Wave system
static const int   TOTAL_WAVES           = 10;
static const float WAVE_ANNOUNCE_TIME    = 3.0f;    // seconds to show "WAVE X"
static const int   WAVE_BASE_TARGET      = 5;       // wave 1 target = 5 destroys

// Power-ups
static const int   MAX_POWERUPS          = 3;       // max on map
static const float POWERUP_SPAWN_INTERVAL = 8.0f;   // seconds between spawn tries
static const float POWERUP_COLLECT_RADIUS = 2.5f;
static const float POWERUP_DURATION       = 6.0f;   // seconds (for timed effects)
static const float POWERUP_BOB_SPEED      = 3.0f;
static const float POWERUP_BOB_HEIGHT     = 0.3f;

// Minimap
static const float MINIMAP_RADIUS        = 60.0f;   // world-space range
static const float MINIMAP_NDC_SIZE      = 0.22f;   // screen fraction

// Terrain heightmap
static const int   TERRAIN_GRID          = 128;      // cells per axis
static const float TERRAIN_EXTENT        = 200.0f;   // half-size in world units
static const float WATER_LEVEL           = 0.3f;     // Y where water surface sits

// Day/night cycle
static const float DAY_CYCLE_SPEED       = 0.015f;   // full cycle ~67 seconds

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
    GLint fontTex=-1, color=-1, alpha=-1;
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
};

// New destructible objects
struct ChunkProp {
    glm::vec3 pos;
    int chunkX = 0, chunkZ = 0;
    float health = 1.0f;
    bool destroyed = false;
    int propType; // 0=fence, 1=car, 2=pole
    float yaw;    // random rotation
};

struct ScorchMark {
    glm::vec3 pos;
    float radius;
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
enum class PowerUpType { SPEED_BOOST, SIZE_DOUBLE, MAGNET, SHIELD };
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

    // Water
    GLuint waterVAO = 0, waterVBO = 0, waterEBO = 0;
    GLsizei waterIndexCount = 0;

    // Day/night
    float dayTime = 0.25f;  // 0=midnight, 0.25=sunrise, 0.5=noon, 0.75=sunset

    // Scene models
    SimpleModel house;
    SimpleModel tree;
    GLTFModel   boxModel;      bool boxLoaded = false;
    GLTFModel   avocadoModel;  bool avoLoaded = false;

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

    // Tornado path trail (world XZ positions)
    std::deque<glm::vec2> tornadoTrail;
    float trailSampleTimer = 0.0f;

    // Wave system
    GamePhase gamePhase = GamePhase::WAVE_ANNOUNCE;
    Wave wave;
    float victoryTimer = 0.0f;

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

    // Props (fences, cars, poles)
    std::vector<ChunkProp> chunkProps;
    SimpleModel fence;
    SimpleModel car;
    SimpleModel pole;

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
static bool  g_soundMuted      = false;
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
        app.activePowerUps.clear();
        app.powerUps.clear();
        app.powerUpSpawnTimer = 5.0f;
        for (auto& h  : app.houses)      { h.health  = 1.0f; h.destroyed  = false; }
        for (auto& tr : app.chunkTrees)  { tr.health = 1.0f; tr.destroyed = false; }
        for (auto& pr : app.chunkProps)  { pr.health = 1.0f; pr.destroyed = false; }
        app.debrisPieces.clear();
        app.scorchMarks.clear();
        app.comboCount = 0;
        app.comboTimer = 0.0f;
        app.comboMultiplier = 1.0f;
        app.tornadoTrail.clear();
        app.trailSampleTimer = 0.0f;
        g_paused = false;
    }
    EMSCRIPTEN_KEEPALIVE int get_score_points() {
        return app.score.scorePoints;
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

// ── Procedural terrain noise ─────────────────────────────────────────
static float hashNoise(int ix, int iz) {
    // Use uint32_t for all arithmetic — signed overflow is undefined behaviour.
    uint32_t n = (uint32_t)ix * 374761393u + (uint32_t)iz * 668265263u;
    n = (n << 13) ^ n;
    return 1.0f - (float)((n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu) / 1073741824.0f;
}

static float smoothNoise(float x, float z) {
    int ix = (int)floorf(x);
    int iz = (int)floorf(z);
    float fx = x - ix;
    float fz = z - iz;
    // Smoothstep
    fx = fx * fx * (3.0f - 2.0f * fx);
    fz = fz * fz * (3.0f - 2.0f * fz);
    float a = hashNoise(ix, iz);
    float b = hashNoise(ix + 1, iz);
    float c = hashNoise(ix, iz + 1);
    float d = hashNoise(ix + 1, iz + 1);
    return a + fx * (b - a) + fz * (c - a) + fx * fz * (a - b - c + d);
}

static float fbmNoise(float x, float z, int octaves, float persistence = 0.5f) {
    float total = 0.0f, amplitude = 1.0f, frequency = 1.0f, maxVal = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        total += smoothNoise(x * frequency, z * frequency) * amplitude;
        maxVal += amplitude;
        amplitude *= persistence;
        frequency *= 2.0f;
    }
    return total / maxVal;
}

// Get terrain height at any world position
static float getTerrainHeight(float x, float z) {
    // Large-scale hills/mountains
    float h = fbmNoise(x * 0.012f, z * 0.012f, 5, 0.55f) * 6.0f;
    // Medium ridges
    h += fbmNoise(x * 0.035f + 50.0f, z * 0.035f + 50.0f, 3, 0.5f) * 1.5f;
    // Flatten near origin so player starts on flat ground
    float dist = sqrtf(x * x + z * z);
    float flatFactor = glm::smoothstep(8.0f, 50.0f, dist);
    h *= flatFactor;
    // Raise the flattened start plateau above the water level so the
    // spawn area is dry land and destructible objects spawn nearby
    h += (1.0f - flatFactor) * 1.2f;
    return h;
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
        h.pos = glm::vec3(px, py, pz);
        h.health = 1.0f;
        h.destroyed = false;
        h.chunkX = cx;
        h.chunkZ = cz;
        app.houses.push_back(h);
    }
    // Trees
    for (int i = 0; i < TREES_PER_CHUNK; ++i) {
        float px = ox + r01(cRng) * CHUNK_SIZE;
        float pz = oz + r01(cRng) * CHUNK_SIZE;
        float py = getTerrainHeight(px, pz);
        if (py < WATER_LEVEL + 0.1f) continue; // skip water areas
        ChunkTree t;
        t.pos = glm::vec3(px, py, pz);
        t.chunkX = cx;
        t.chunkZ = cz;
        t.health = 1.0f;
        t.destroyed = false;
        app.chunkTrees.push_back(t);
    }
    // Props: fences, cars, poles
    for (int i = 0; i < FENCES_PER_CHUNK; ++i) {
        float px = ox + r01(cRng) * CHUNK_SIZE;
        float pz = oz + r01(cRng) * CHUNK_SIZE;
        float py = getTerrainHeight(px, pz);
        if (py < WATER_LEVEL + 0.15f) continue;
        ChunkProp p;
        p.pos = glm::vec3(px, py, pz);
        p.chunkX = cx; p.chunkZ = cz;
        p.propType = 0; // fence
        p.yaw = r01(cRng) * 6.28f;
        app.chunkProps.push_back(p);
    }
    for (int i = 0; i < CARS_PER_CHUNK; ++i) {
        float px = ox + r01(cRng) * CHUNK_SIZE;
        float pz = oz + r01(cRng) * CHUNK_SIZE;
        float py = getTerrainHeight(px, pz);
        if (py < WATER_LEVEL + 0.15f) continue;
        ChunkProp p;
        p.pos = glm::vec3(px, py, pz);
        p.chunkX = cx; p.chunkZ = cz;
        p.propType = 1; // car
        p.yaw = r01(cRng) * 6.28f;
        app.chunkProps.push_back(p);
    }
    for (int i = 0; i < POLES_PER_CHUNK; ++i) {
        float px = ox + r01(cRng) * CHUNK_SIZE;
        float pz = oz + r01(cRng) * CHUNK_SIZE;
        float py = getTerrainHeight(px, pz);
        if (py < WATER_LEVEL + 0.1f) continue;
        ChunkProp p;
        p.pos = glm::vec3(px, py, pz);
        p.chunkX = cx; p.chunkZ = cz;
        p.propType = 2; // pole
        p.yaw = 0.0f;
        app.chunkProps.push_back(p);
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

// ── Sound effects (Web Audio API via Emscripten) ─────────────────────
#ifdef PLATFORM_EMSCRIPTEN

EM_JS(void, js_initSound, (), {
    if (window._tornadoAudio) return;
    var ctx = new (window.AudioContext || window.webkitAudioContext)();
    window._tornadoAudio = {ctx: ctx, windGain: null};
    var bufSize = ctx.sampleRate * 2;
    var buf = ctx.createBuffer(1, bufSize, ctx.sampleRate);
    var d = buf.getChannelData(0);
    for (var i = 0; i < bufSize; i++) d[i] = Math.random() * 2 - 1;
    var src = ctx.createBufferSource();
    src.buffer = buf; src.loop = true;
    var bp = ctx.createBiquadFilter();
    bp.type = "bandpass"; bp.frequency.value = 300; bp.Q.value = 0.5;
    var gain = ctx.createGain(); gain.gain.value = 0.15;
    src.connect(bp); bp.connect(gain); gain.connect(ctx.destination);
    src.start();
    window._tornadoAudio.windGain = gain;
    window._tornadoAudio.windFilter = bp;
});

EM_JS(void, js_playDestroySound, (), {
    var a = window._tornadoAudio; if (!a) return;
    var ctx = a.ctx;
    var len = (ctx.sampleRate * 0.3)|0;
    var buf = ctx.createBuffer(1, len, ctx.sampleRate);
    var d = buf.getChannelData(0);
    for (var i = 0; i < len; i++) {
        var env = 1.0 - i / len;
        d[i] = (Math.random() * 2 - 1) * env * env;
    }
    var src = ctx.createBufferSource(); src.buffer = buf;
    var lp = ctx.createBiquadFilter(); lp.type = "lowpass"; lp.frequency.value = 400;
    var g = ctx.createGain(); g.gain.value = 0.35;
    src.connect(lp); lp.connect(g); g.connect(ctx.destination);
    src.start();
});

EM_JS(void, js_playPowerUpSound, (), {
    var a = window._tornadoAudio; if (!a) return;
    var ctx = a.ctx;
    var osc = ctx.createOscillator();
    osc.type = "sine";
    osc.frequency.setValueAtTime(400, ctx.currentTime);
    osc.frequency.linearRampToValueAtTime(900, ctx.currentTime + 0.15);
    var g = ctx.createGain();
    g.gain.setValueAtTime(0.25, ctx.currentTime);
    g.gain.linearRampToValueAtTime(0, ctx.currentTime + 0.2);
    osc.connect(g); g.connect(ctx.destination);
    osc.start(); osc.stop(ctx.currentTime + 0.2);
});

EM_JS(void, js_playWaveSound, (), {
    var a = window._tornadoAudio; if (!a) return;
    var ctx = a.ctx;
    var freqs = [330, 440, 550];
    for (var i = 0; i < 3; i++) {
        var osc = ctx.createOscillator();
        osc.type = "square";
        osc.frequency.value = freqs[i];
        var g = ctx.createGain();
        g.gain.setValueAtTime(0, ctx.currentTime + i*0.12);
        g.gain.linearRampToValueAtTime(0.15, ctx.currentTime + i*0.12 + 0.03);
        g.gain.linearRampToValueAtTime(0, ctx.currentTime + i*0.12 + 0.3);
        osc.connect(g); g.connect(ctx.destination);
        osc.start(ctx.currentTime + i*0.12);
        osc.stop(ctx.currentTime + i*0.12 + 0.3);
    }
});

EM_JS(void, js_playThunderSound, (), {
    var a = window._tornadoAudio; if (!a) return;
    var ctx = a.ctx;
    var len = (ctx.sampleRate * 1.5)|0;
    var buf = ctx.createBuffer(1, len, ctx.sampleRate);
    var d = buf.getChannelData(0);
    for (var i = 0; i < len; i++) {
        var env = Math.exp(-3.0 * i / len);
        d[i] = (Math.random() * 2 - 1) * env;
    }
    var src = ctx.createBufferSource(); src.buffer = buf;
    var lp = ctx.createBiquadFilter(); lp.type = "lowpass"; lp.frequency.value = 200;
    var g = ctx.createGain(); g.gain.value = 0.4;
    src.connect(lp); lp.connect(g); g.connect(ctx.destination);
    src.start();
});

EM_JS(void, js_updateWindVolume, (float scale), {
    var a = window._tornadoAudio; if (!a || !a.windGain) return;
    a.windGain.gain.value = 0.1 + scale * 0.12;
    a.windFilter.frequency.value = 200 + scale * 150;
});

EM_JS(void, js_playVictorySound, (), {
    var a = window._tornadoAudio; if (!a) return;
    var ctx = a.ctx;
    var notes = [523, 659, 784, 1047];
    for (var i = 0; i < notes.length; i++) {
        var osc = ctx.createOscillator();
        osc.type = "sine"; osc.frequency.value = notes[i];
        var g = ctx.createGain();
        g.gain.setValueAtTime(0, ctx.currentTime + i*0.2);
        g.gain.linearRampToValueAtTime(0.2, ctx.currentTime + i*0.2 + 0.05);
        g.gain.linearRampToValueAtTime(0, ctx.currentTime + i*0.2 + 0.5);
        osc.connect(g); g.connect(ctx.destination);
        osc.start(ctx.currentTime + i*0.2);
        osc.stop(ctx.currentTime + i*0.2 + 0.5);
    }
});

EM_JS(void, js_saveScore, (int score, int wave), {
    try {
        var key = "tornado3d_scores";
        var scores = JSON.parse(localStorage.getItem(key) || "[]");
        scores.push({score: score, wave: wave, date: new Date().toLocaleDateString()});
        scores.sort(function(a,b) { return b.score - a.score; });
        if (scores.length > 10) scores = scores.slice(0, 10);
        localStorage.setItem(key, JSON.stringify(scores));
    } catch(e) {}
});

EM_JS(int, js_getHighScore, (), {
    try {
        var scores = JSON.parse(localStorage.getItem("tornado3d_scores") || "[]");
        return scores.length > 0 ? scores[0].score : 0;
    } catch(e) { return 0; }
});

#endif // PLATFORM_EMSCRIPTEN

static void initSound() {
#ifdef PLATFORM_EMSCRIPTEN
    js_initSound();
    app.soundInitialized = true;
#endif
}
static void playDestroySound() {
#ifdef PLATFORM_EMSCRIPTEN
    if (!g_soundMuted) js_playDestroySound();
#endif
}
static void playPowerUpSound() {
#ifdef PLATFORM_EMSCRIPTEN
    if (!g_soundMuted) js_playPowerUpSound();
#endif
}
static void playWaveSound() {
#ifdef PLATFORM_EMSCRIPTEN
    if (!g_soundMuted) js_playWaveSound();
#endif
}
static void playThunderSound() {
#ifdef PLATFORM_EMSCRIPTEN
    if (!g_soundMuted) js_playThunderSound();
#endif
}
static void updateWindVolume(float tornadoScale) {
#ifdef PLATFORM_EMSCRIPTEN
    if (!g_soundMuted) js_updateWindVolume(tornadoScale);
#endif
}
static void playVictorySound() {
#ifdef PLATFORM_EMSCRIPTEN
    if (!g_soundMuted) js_playVictorySound();
#endif
}
static void saveScore(int score, int wave) {
#ifdef PLATFORM_EMSCRIPTEN
    js_saveScore(score, wave);
#endif
}
static int getHighScore() {
#ifdef PLATFORM_EMSCRIPTEN
    return js_getHighScore();
#else
    return 0;
#endif
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
    if (!s.soundInitialized) initSound();

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

    // ── Update active power-ups ──
    float speedMult = 1.0f;
    float sizeMult  = 1.0f;
    bool hasMagnet = false;
    for (auto it = s.activePowerUps.begin(); it != s.activePowerUps.end();) {
        it->remaining -= dt;
        if (it->remaining <= 0.0f) {
            it = s.activePowerUps.erase(it);
        } else {
            if (it->type == PowerUpType::SPEED_BOOST) speedMult = 1.8f;
            if (it->type == PowerUpType::SIZE_DOUBLE) sizeMult  = 2.0f;
            if (it->type == PowerUpType::MAGNET)      hasMagnet = true;
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
                pu.type = (PowerUpType)((int)(s.rnd01(s.rng) * 4.0f) % 4);
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

    // ── Post-destruction: sounds, combo, scoring, wave check ──
    if (destroyedSomething) {
        s.lastDestroyTime = t;
        playDestroySound();
        // Combo: increment, apply multiplier, reset timer
        s.comboCount++;
        s.comboTimer      = 2.5f;
        s.comboMultiplier = 1.0f + std::min(s.comboCount / 3.0f, 4.0f);
        s.score.scorePoints += (int)(newPoints * s.comboMultiplier);
    }

    // Wave completion check
    if (s.gamePhase == GamePhase::PLAYING &&
        s.wave.destroyed >= s.wave.target) {
        if (s.wave.number >= TOTAL_WAVES) {
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

    // -- Water plane --
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glm::mat4 model(1.0f);
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

    // -- glTF models (if loaded) --
    if (s.boxLoaded) {
        glUniform1i(mu.objType, 0);
        glUniform1f(mu.enableSwirl, 0.0f);
        glUniform1f(mu.opacity, 1.0f);
        glUniform1i(mu.hasAlbedo, s.boxModel.texture ? 1 : 0);
        if (s.boxModel.texture) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s.boxModel.texture);
        }
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.5f, 2.0f));
        glm::mat3 nm = normalMat3(model);
        glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
        glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
        glUniform3f(mu.tint, 1.0f, 1.0f, 1.0f);
        glBindVertexArray(s.boxModel.vao);
        glDrawElements(GL_TRIANGLES, s.boxModel.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    if (s.avoLoaded) {
        glUniform1i(mu.objType, 0);
        glUniform1f(mu.enableSwirl, 0.0f);
        glUniform1f(mu.opacity, 1.0f);
        glUniform1i(mu.hasAlbedo, s.avocadoModel.texture ? 1 : 0);
        if (s.avocadoModel.texture) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s.avocadoModel.texture);
        }
        glm::mat4 model = glm::scale(
            glm::translate(glm::mat4(1.0f), glm::vec3(-3.0f, 0.5f, 3.0f)),
            glm::vec3(30.0f));
        glm::mat3 nm = normalMat3(model);
        glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
        glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
        glUniform3f(mu.tint, 1.0f, 1.0f, 1.0f);
        glBindVertexArray(s.avocadoModel.vao);
        glDrawElements(GL_TRIANGLES, s.avocadoModel.indexCount, GL_UNSIGNED_INT, nullptr);
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
        glUniform3f(mu.tint, 0.8f, 0.8f, 0.9f);
        glUniform1f(mu.opacity, 0.7f);
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

    // inner (dense dark dust)
    glUniform3f(pu.color, 0.25f, 0.22f, 0.2f);
    glUniform1f(pu.pointScale, 1.5f);
    if (innerDraw > 0) glDrawArrays(GL_POINTS, 0, innerDraw);

    // outer (lighter debris)
    glUniform3f(pu.color, 0.5f, 0.45f, 0.35f);
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
        glUseProgram(s.hudProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s.fontTex);
        glUniform1i(s.hu.fontTex, 0);

        // Render function: each char is a textured quad (font atlas mode)
        auto renderLine = [&](const char* text, float startX, float startY,
                              float charW, float charH, glm::vec3 color) {
            glUniform1f(s.hu.alpha, -1.0f); // font texture mode
            glUniform3fv(s.hu.color, 1, glm::value_ptr(color));
            std::vector<float> verts;
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
                verts.insert(verts.end(), {x0,y0, u0,v1, x1,y0, u1,v1, x1,y1, u1,v0,
                                            x0,y0, u0,v1, x1,y1, u1,v0, x0,y1, u0,v0});
                cx += charW;
            }
            glBindVertexArray(s.hudVAO);
            glBindBuffer(GL_ARRAY_BUFFER, s.hudVBO);
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            (GLsizeiptr)(verts.size()*sizeof(float)), verts.data());
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size()/4));
        };

        // Helper: render a filled quad with explicit alpha (solid, no texture)
        auto renderQuadA = [&](float x0, float y0, float x1, float y1,
                                glm::vec3 color, float alpha) {
            glUniform1f(s.hu.alpha, alpha);
            glUniform3fv(s.hu.color, 1, glm::value_ptr(color));
            float u = 0.0f;
            float verts[] = {
                x0,y0, u,u, x1,y0, u,u, x1,y1, u,u,
                x0,y0, u,u, x1,y1, u,u, x0,y1, u,u
            };
            glBindVertexArray(s.hudVAO);
            glBindBuffer(GL_ARRAY_BUFFER, s.hudVBO);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        };

        // Helper: render a fully opaque filled quad
        auto renderQuad = [&](float x0, float y0, float x1, float y1, glm::vec3 color) {
            renderQuadA(x0, y0, x1, y1, color, 1.0f);
        };

        float cw = 0.022f, ch = 0.045f;
        float scw = cw * 0.8f, sch = ch * 0.8f;

        // ── Top-left: score info ──
        char buf[64];
        snprintf(buf, sizeof(buf), "SCORE: %d", s.score.scorePoints);
        renderLine(buf, -0.98f, 0.92f, cw, ch, glm::vec3(1.0f, 0.85f, 0.0f));

        snprintf(buf, sizeof(buf), "DESTROYED: %d", s.score.totalDestroyed);
        renderLine(buf, -0.98f, 0.86f, scw, sch, glm::vec3(1.0f, 0.9f, 0.3f));

        snprintf(buf, sizeof(buf), "H:%d T:%d P:%d",
                 s.score.housesDestroyed, s.score.treesDestroyed, s.score.propsDestroyed);
        renderLine(buf, -0.98f, 0.81f, scw * 0.85f, sch * 0.85f, glm::vec3(0.8f, 0.8f, 0.8f));

        snprintf(buf, sizeof(buf), "TORNADO x%.1f", s.tornadoScale);
        renderLine(buf, -0.98f, 0.76f, scw * 0.85f, sch * 0.85f, glm::vec3(1.0f, 0.5f, 0.3f));

        // ── Combo multiplier display ──
        if (s.comboCount > 0) {
            float comboPulse = 0.7f + 0.3f * sinf(t * 8.0f);
            snprintf(buf, sizeof(buf), "COMBO x%d  x%.1f", s.comboCount, s.comboMultiplier);
            renderLine(buf, -0.98f, 0.70f, scw * 0.85f, sch * 0.85f,
                       glm::vec3(1.0f, 0.4f, 0.1f) * comboPulse);
        }

        // ── Top-right: wave info ──
        snprintf(buf, sizeof(buf), "WAVE %d/%d  EF%d",
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

            // Circle outline (12 dots)
            for (int i = 0; i < 12; ++i) {
                float a = (float)i * (float)M_PI / 6.0f;
                float dx = outerR * sinf(a), dy = outerR * cosf(a);
                renderQuadA(cX+dx-dotS2, cY+dy-dotS2, cX+dx+dotS2, cY+dy+dotS2,
                            glm::vec3(0.4f, 0.5f, 0.6f), 0.6f);
            }
            // Tornado indicator dot
            float iR = outerR * 0.8f;
            float idx = iR * sinf(bearing), idy = iR * cosf(bearing);
            float bigS = 0.013f;
            renderQuad(cX+idx-bigS, cY+idy-bigS, cX+idx+bigS, cY+idy+bigS,
                       glm::vec3(1.0f, 0.4f, 0.1f));
            // Distance text
            snprintf(buf, sizeof(buf), "%.0fm", dist3d);
            float tw = (float)strlen(buf) * scw * 0.7f;
            renderLine(buf, cX - tw * 0.5f, cY - outerR - sch * 0.75f,
                       scw * 0.7f, sch * 0.7f, glm::vec3(0.9f, 0.7f, 0.5f));
        }

        // ── Minimap (bottom-right corner) ──
        {
            float mmX = 0.72f;  // center X in NDC
            float mmY = -0.72f; // center Y in NDC
            float mmR = MINIMAP_NDC_SIZE;

            // Background
            renderQuad(mmX - mmR, mmY - mmR, mmX + mmR, mmY + mmR,
                       glm::vec3(0.05f, 0.08f, 0.05f));

            // Border
            float bw = 0.005f;
            renderQuad(mmX - mmR - bw, mmY - mmR - bw, mmX + mmR + bw, mmY - mmR,
                       glm::vec3(0.3f, 0.5f, 0.3f));
            renderQuad(mmX - mmR - bw, mmY + mmR, mmX + mmR + bw, mmY + mmR + bw,
                       glm::vec3(0.3f, 0.5f, 0.3f));
            renderQuad(mmX - mmR - bw, mmY - mmR, mmX - mmR, mmY + mmR,
                       glm::vec3(0.3f, 0.5f, 0.3f));
            renderQuad(mmX + mmR, mmY - mmR, mmX + mmR + bw, mmY + mmR,
                       glm::vec3(0.3f, 0.5f, 0.3f));

            // Convert world pos to minimap NDC
            auto worldToMM = [&](float wx, float wz) -> glm::vec2 {
                float dx = wx - s.camera.pos.x;
                float dz = wz - s.camera.pos.z;
                float mx = mmX + (dx / MINIMAP_RADIUS) * mmR;
                float my = mmY - (dz / MINIMAP_RADIUS) * mmR;
                return glm::vec2(mx, my);
            };
            auto inMM = [&](glm::vec2 p) {
                return p.x > mmX - mmR && p.x < mmX + mmR &&
                       p.y > mmY - mmR && p.y < mmY + mmR;
            };

            float dotS = 0.006f;

            // Houses (red dots)
            for (const auto& h : s.houses) {
                if (h.destroyed) continue;
                glm::vec2 p = worldToMM(h.pos.x, h.pos.z);
                if (inMM(p))
                    renderQuad(p.x-dotS, p.y-dotS, p.x+dotS, p.y+dotS,
                               glm::vec3(0.9f, 0.3f, 0.2f));
            }
            // Trees (green dots)
            for (const auto& tr : s.chunkTrees) {
                if (tr.destroyed) continue;
                glm::vec2 p = worldToMM(tr.pos.x, tr.pos.z);
                if (inMM(p))
                    renderQuad(p.x-dotS*0.7f, p.y-dotS*0.7f, p.x+dotS*0.7f, p.y+dotS*0.7f,
                               glm::vec3(0.2f, 0.7f, 0.2f));
            }
            // Props (gray dots)
            for (const auto& pr : s.chunkProps) {
                if (pr.destroyed) continue;
                glm::vec2 p = worldToMM(pr.pos.x, pr.pos.z);
                if (inMM(p))
                    renderQuad(p.x-dotS*0.5f, p.y-dotS*0.5f, p.x+dotS*0.5f, p.y+dotS*0.5f,
                               glm::vec3(0.5f, 0.5f, 0.5f));
            }
            // Power-ups (bright colored dots)
            for (const auto& pu : s.powerUps) {
                if (pu.collected) continue;
                glm::vec2 p = worldToMM(pu.pos.x, pu.pos.z);
                if (inMM(p))
                    renderQuad(p.x-dotS, p.y-dotS, p.x+dotS, p.y+dotS,
                               glm::vec3(1.0f, 1.0f, 0.0f));
            }
            // Tornado trail (faded gray dots)
            {
                int trailLen = (int)s.tornadoTrail.size();
                for (int ti = 0; ti < trailLen; ++ti) {
                    float age = (trailLen > 1) ? (float)(trailLen - 1 - ti) / (float)(trailLen - 1) : 0.0f;
                    float trailAlpha = (1.0f - age) * 0.45f;
                    if (trailAlpha < 0.05f) continue;
                    glm::vec2 p = worldToMM(s.tornadoTrail[ti].x, s.tornadoTrail[ti].y);
                    if (inMM(p))
                        renderQuadA(p.x-dotS*0.4f, p.y-dotS*0.4f, p.x+dotS*0.4f, p.y+dotS*0.4f,
                                    glm::vec3(0.6f, 0.6f, 0.7f), trailAlpha);
                }
            }
            // Tornado (white cross)
            {
                glm::vec2 tp = worldToMM(s.tornadoPos.x, s.tornadoPos.y);
                float cs = dotS * 2.0f;
                if (inMM(tp)) {
                    renderQuad(tp.x-cs, tp.y-dotS*0.5f, tp.x+cs, tp.y+dotS*0.5f,
                               glm::vec3(1.0f, 1.0f, 1.0f));
                    renderQuad(tp.x-dotS*0.5f, tp.y-cs, tp.x+dotS*0.5f, tp.y+cs,
                               glm::vec3(1.0f, 1.0f, 1.0f));
                }
            }
            // Player (cyan dot at center)
            renderQuad(mmX-dotS, mmY-dotS, mmX+dotS, mmY+dotS,
                       glm::vec3(0.0f, 1.0f, 1.0f));
        }

        // ── Wave announcement overlay ──
        if (s.gamePhase == GamePhase::WAVE_ANNOUNCE) {
            float alpha = 1.0f;
            if (s.wave.announceTimer > WAVE_ANNOUNCE_TIME - 0.5f)
                alpha = (WAVE_ANNOUNCE_TIME - s.wave.announceTimer) * 2.0f;

            float bigCW = 0.05f, bigCH = 0.1f;
            snprintf(buf, sizeof(buf), "WAVE %d", s.wave.number);
            float textW = (float)strlen(buf) * bigCW;
            renderLine(buf, -textW * 0.5f, 0.1f, bigCW, bigCH,
                       glm::vec3(0.5f, 0.9f, 1.0f) * alpha);

            // EF scale label
            static const glm::vec3 EF_COLORS[6] = {
                {0.5f,0.9f,0.5f}, {0.8f,0.9f,0.3f}, {1.0f,0.8f,0.2f},
                {1.0f,0.55f,0.1f}, {1.0f,0.25f,0.05f}, {1.0f,0.1f,0.1f}
            };
            snprintf(buf, sizeof(buf), "EF%d TORNADO", s.wave.efScale);
            float efW = (float)strlen(buf) * 0.032f;
            renderLine(buf, -efW * 0.5f, 0.03f, 0.032f, 0.064f,
                       EF_COLORS[std::clamp(s.wave.efScale, 0, 5)] * alpha);

            float smCW = 0.025f, smCH = 0.05f;
            snprintf(buf, sizeof(buf), "DESTROY %d OBJECTS", s.wave.target);
            float stW = (float)strlen(buf) * smCW;
            renderLine(buf, -stW * 0.5f, -0.05f, smCW, smCH,
                       glm::vec3(0.8f, 0.8f, 0.8f) * alpha);
        }

        // ── Victory screen ──
        if (s.gamePhase == GamePhase::VICTORY) {
            s.victoryTimer += dt;

            // Semi-dark background
            renderQuadA(-0.62f, -0.42f, 0.62f, 0.48f,
                       glm::vec3(0.02f, 0.02f, 0.05f), 0.85f);

            float bigCW = 0.06f, bigCH = 0.12f;
            const char* victoryText = "VICTORY";
            float vw = 7.0f * bigCW;
            float pulse = 0.7f + 0.3f * sinf(t * 3.0f);
            renderLine(victoryText, -vw * 0.5f, 0.25f, bigCW, bigCH,
                       glm::vec3(1.0f, 0.85f, 0.0f) * pulse);

            float smCW = 0.02f, smCH = 0.04f;
            snprintf(buf, sizeof(buf), "ALL %d WAVES COMPLETE", TOTAL_WAVES);
            float bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, 0.17f, smCW, smCH,
                       glm::vec3(0.7f, 0.9f, 1.0f));

            snprintf(buf, sizeof(buf), "SCORE: %d", s.score.scorePoints);
            bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, 0.10f, smCW, smCH,
                       glm::vec3(1.0f, 0.9f, 0.2f));

            snprintf(buf, sizeof(buf), "TOTAL DESTROYED: %d", s.score.totalDestroyed);
            bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, 0.04f, smCW, smCH,
                       glm::vec3(0.9f, 0.9f, 0.9f));

            snprintf(buf, sizeof(buf), "MAX TORNADO: x%.1f", s.tornadoScale);
            bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, -0.03f, smCW, smCH,
                       glm::vec3(1.0f, 0.5f, 0.3f));

            int hi = getHighScore();
            snprintf(buf, sizeof(buf), "HIGH SCORE: %d", hi);
            bw2 = (float)strlen(buf) * smCW;
            renderLine(buf, -bw2 * 0.5f, -0.11f, smCW, smCH,
                       glm::vec3(1.0f, 0.9f, 0.3f));

            if (s.victoryTimer > 2.0f) {
                float blink = (sinf(t * 4.0f) > 0.0f) ? 1.0f : 0.3f;
                const char* copyHint = "PRESS C TO COPY SCORE";
                float chw = (float)strlen(copyHint) * smCW * 0.85f;
                renderLine(copyHint, -chw * 0.5f, -0.22f, smCW * 0.85f, smCH * 0.85f,
                           glm::vec3(0.4f, 0.9f, 0.9f) * blink);
                const char* restart = "PRESS R TO RESTART";
                float rw = (float)strlen(restart) * smCW;
                renderLine(restart, -rw * 0.5f, -0.30f, smCW, smCH,
                           glm::vec3(0.6f, 0.8f, 0.6f) * blink);
            }
        }

        // ── Lightning full-screen flash ──
        if (s.lightning.intensity > 0.01f) {
            float flashAlpha = s.lightning.intensity * 0.28f;
            renderQuadA(-1.0f, -1.0f, 1.0f, 1.0f,
                        glm::vec3(0.9f, 0.95f, 1.0f), flashAlpha);
        }

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    // ── Restart on R key (during victory) ──
    if (s.gamePhase == GamePhase::VICTORY) {
        if (glfwGetKey(s.window, GLFW_KEY_R) == GLFW_PRESS) {
            s.score = Score{};
            s.tornadoScale = 1.0f;
            s.lastDestroyTime = t;
            s.wave = Wave{};
            s.wave.announceTimer = 0.0f;
            s.gamePhase = GamePhase::WAVE_ANNOUNCE;
            s.victoryTimer = 0.0f;
            s.activePowerUps.clear();
            s.powerUps.clear();
            s.powerUpSpawnTimer = 5.0f;
            for (auto& h : s.houses)      { h.health = 1.0f; h.destroyed = false; }
            for (auto& tr : s.chunkTrees) { tr.health = 1.0f; tr.destroyed = false; }
            for (auto& pr : s.chunkProps) { pr.health = 1.0f; pr.destroyed = false; }
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
        app.hu.color   = glGetUniformLocation(p, "uColor");
        app.hu.alpha   = glGetUniformLocation(p, "uAlpha");
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
        struct GlyphDef { int ch; const char* bits[7]; };
        GlyphDef glyphs[] = {
            {'0', {"01110","10001","10011","10101","11001","10001","01110"}},
            {'1', {"00100","01100","00100","00100","00100","00100","01110"}},
            {'2', {"01110","10001","00001","00110","01000","10000","11111"}},
            {'3', {"01110","10001","00001","00110","00001","10001","01110"}},
            {'4', {"00010","00110","01010","10010","11111","00010","00010"}},
            {'5', {"11111","10000","11110","00001","00001","10001","01110"}},
            {'6', {"01110","10000","11110","10001","10001","10001","01110"}},
            {'7', {"11111","00001","00010","00100","01000","01000","01000"}},
            {'8', {"01110","10001","10001","01110","10001","10001","01110"}},
            {'9', {"01110","10001","10001","01111","00001","00001","01110"}},
            {'A', {"01110","10001","10001","11111","10001","10001","10001"}},
            {'B', {"11110","10001","10001","11110","10001","10001","11110"}},
            {'C', {"01110","10001","10000","10000","10000","10001","01110"}},
            {'D', {"11110","10001","10001","10001","10001","10001","11110"}},
            {'E', {"11111","10000","10000","11110","10000","10000","11111"}},
            {'F', {"11111","10000","10000","11110","10000","10000","10000"}},
            {'G', {"01110","10001","10000","10111","10001","10001","01110"}},
            {'H', {"10001","10001","10001","11111","10001","10001","10001"}},
            {'I', {"01110","00100","00100","00100","00100","00100","01110"}},
            {'K', {"10001","10010","10100","11000","10100","10010","10001"}},
            {'L', {"10000","10000","10000","10000","10000","10000","11111"}},
            {'M', {"10001","11011","10101","10101","10001","10001","10001"}},
            {'N', {"10001","11001","10101","10011","10001","10001","10001"}},
            {'O', {"01110","10001","10001","10001","10001","10001","01110"}},
            {'P', {"11110","10001","10001","11110","10000","10000","10000"}},
            {'R', {"11110","10001","10001","11110","10100","10010","10001"}},
            {'S', {"01110","10001","10000","01110","00001","10001","01110"}},
            {'T', {"11111","00100","00100","00100","00100","00100","00100"}},
            {'U', {"10001","10001","10001","10001","10001","10001","01110"}},
            {'V', {"10001","10001","10001","10001","01010","01010","00100"}},
            {'W', {"10001","10001","10001","10101","10101","11011","10001"}},
            {'X', {"10001","01010","00100","00100","00100","01010","10001"}},
            {'Y', {"10001","01010","00100","00100","00100","00100","00100"}},
            {'Z', {"11111","00001","00010","00100","01000","10000","11111"}},
            {':', {"00000","00100","00100","00000","00100","00100","00000"}},
            {'.', {"00000","00000","00000","00000","00000","01100","01100"}},
            {'x', {"00000","00000","10001","01010","00100","01010","10001"}},
            {' ', {"00000","00000","00000","00000","00000","00000","00000"}},
        };
        for (auto& g : glyphs) {
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
        glBufferData(GL_ARRAY_BUFFER, 256 * 6 * 4 * sizeof(float), nullptr, GL_STREAM_DRAW);
        // layout: vec2 pos, vec2 uv per vertex
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
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
        struct SV { glm::vec3 pos; glm::vec3 normal; glm::vec3 col; };
        const int GN = TERRAIN_GRID + 1; // vertices per axis
        std::vector<SV> gv(GN * GN);
        float step = (TERRAIN_EXTENT * 2.0f) / TERRAIN_GRID;

        // Generate heights
        for (int iz = 0; iz < GN; ++iz) {
            for (int ix = 0; ix < GN; ++ix) {
                float x = -TERRAIN_EXTENT + ix * step;
                float z = -TERRAIN_EXTENT + iz * step;
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
                glm::vec3 n = glm::normalize(glm::vec3(hL - hR, 2.0f * step, hD - hU));
                gv[iz * GN + ix].normal = n;
            }
        }
        // Generate indices
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
        glBufferData(GL_ARRAY_BUFFER, gv.size()*sizeof(SV), gv.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, app.groundEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, gi.size()*sizeof(unsigned int), gi.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)offsetof(SV,pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)offsetof(SV,normal));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)offsetof(SV,col));
        glBindVertexArray(0);
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
    // glTF models (optional)
    // ══════════════════════════════════════
    {
        std::string boxP = "assets/models/BoxTextured.gltf";
        std::string avoP = "assets/models/Avocado.gltf";
        if (loadSimpleGLTF(boxP, app.boxModel))            app.boxLoaded = true;
        else if (loadSimpleGLTF("../"+boxP, app.boxModel)) app.boxLoaded = true;
        if (loadSimpleGLTF(avoP, app.avocadoModel))            app.avoLoaded = true;
        else if (loadSimpleGLTF("../"+avoP, app.avocadoModel)) app.avoLoaded = true;
        if (app.boxLoaded) std::cout << "Loaded glTF: " << boxP << std::endl;
        if (app.avoLoaded) std::cout << "Loaded glTF: " << avoP << std::endl;
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
    // glTF models
    if (app.boxModel.vao)  { glDeleteBuffers(1,&app.boxModel.vbo); glDeleteBuffers(1,&app.boxModel.ebo); glDeleteVertexArrays(1,&app.boxModel.vao); if (app.boxModel.texture) glDeleteTextures(1,&app.boxModel.texture); }
    if (app.avocadoModel.vao) { glDeleteBuffers(1,&app.avocadoModel.vbo); glDeleteBuffers(1,&app.avocadoModel.ebo); glDeleteVertexArrays(1,&app.avocadoModel.vao); if (app.avocadoModel.texture) glDeleteTextures(1,&app.avocadoModel.texture); }
    glDeleteProgram(app.particleProgram);
    glDeleteProgram(app.skyProgram);
    glDeleteProgram(app.rainProgram);
    glDeleteProgram(app.program);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
