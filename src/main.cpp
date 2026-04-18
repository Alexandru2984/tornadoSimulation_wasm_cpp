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
static const int   MAX_DEBRIS          = 300;
static const float DESTRUCTION_RADIUS  = 2.5f;
static const float DAMAGE_RATE         = 0.8f;
static const int   DEBRIS_PER_HOUSE    = 40;
static const float DEBRIS_LIFETIME     = 5.0f;

// Weather
static const int   MAX_RAIN            = 4000;
static const float RAIN_AREA           = 30.0f;
static const float RAIN_HEIGHT         = 15.0f;
static const float RAIN_SPEED          = 8.0f;
static const float LIGHTNING_MIN_INTERVAL = 2.0f;
static const float LIGHTNING_MAX_INTERVAL = 7.0f;
static const float LIGHTNING_DECAY       = 8.0f;

// Uniform location caches
struct MainUniforms {
    GLint proj=-1, view=-1, model=-1, normalMat=-1, time=-1, camPos=-1;
    GLint enableSwirl=-1, tint=-1, opacity=-1, objType=-1, hasAlbedo=-1, albedo=-1;
    GLint lightningFlash=-1, windBend=-1, windSource=-1;
};
struct ParticleUniforms {
    GLint proj=-1, view=-1, model=-1, color=-1, pointScale=-1;
};
struct SkyUniforms {
    GLint lightningFlash=-1, time=-1;
};
struct RainUniforms {
    GLint proj=-1, view=-1;
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

    // Ground
    GLuint groundVAO = 0, groundVBO = 0, groundEBO = 0;

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
};

// ── Global state ─────────────────────────────────────────────────────
static AppState  app;
static double    g_mouseX = 0.0;
static double    g_mouseY = 0.0;

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

// ── GLFW callback ────────────────────────────────────────────────────
static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    int w, h;
    glfwGetWindowSize(window, &w, &h);
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

    // -- Timing --
    double nowT = glfwGetTime();
    float  dt   = (float)(nowT - s.lastT);
    s.lastT     = nowT;
    if (dt > 0.1f) dt = 0.016f;

    float t = (float)nowT - s.startTime;

    // -- Tornado follows mouse smoothly --
    glm::vec2 target(g_mouseX * 2.5f, g_mouseY * -1.0f * 2.5f);
    s.tornadoPos = s.tornadoPos * 0.92f + target * 0.08f;

    // ── Destruction: damage houses near tornado ──
    for (auto& h : s.houses) {
        if (h.destroyed) continue;
        float dist = glm::length(glm::vec2(h.pos.x - s.tornadoPos.x,
                                            h.pos.z - s.tornadoPos.y));
        if (dist < DESTRUCTION_RADIUS) {
            h.health -= DAMAGE_RATE * dt;
            if (h.health <= 0.0f) {
                h.destroyed = true;
                spawnDebris(h.pos, DEBRIS_PER_HOUSE);
            }
        }
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
        // Ground bounce
        if (d.pos.y < 0.0f) {
            d.pos.y = 0.0f;
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
        if (r.pos.y < 0.0f) respawnRain(r, s.camera.pos);
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

    // -- Ground --
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
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
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

        static const glm::vec3 treePos[] = {
            {-3,0,-5}, {3,0,-4}, {-6,0,2}, {5,0,6}, {-2,0,7}, {8,0,-2}
        };
        for (const auto& tp : treePos) {
            glm::mat4 model = glm::scale(
                glm::translate(glm::mat4(1.0f), tp), glm::vec3(2.0f));
            glm::mat3 nm = normalMat3(model);
            glUniformMatrix4fv(mu.model, 1, GL_FALSE, glm::value_ptr(model));
            glUniformMatrix3fv(mu.normalMat, 1, GL_FALSE, glm::value_ptr(nm));
            glUniform3f(mu.tint, 1.0f, 1.0f, 1.0f);
            glBindVertexArray(s.tree.vao);
            glDrawElements(GL_TRIANGLES, s.tree.indexCount, GL_UNSIGNED_INT, nullptr);
        }
        glUniform1f(mu.windBend, 0.0f);
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

    // -- Tornado mesh (swirl enabled, semi-transparent) --
    {
        glm::mat4 model = glm::translate(glm::mat4(1.0f),
                              glm::vec3(s.tornadoPos.x, 0.0f, s.tornadoPos.y));
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
    for (int i = 0; i < MAX_PARTICLES; ++i) {
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
        for (int i = 0; i < MAX_PARTICLES; ++i) {
            s.particleBuf[i*4+0] = s.particles[i].pos.x;
            s.particleBuf[i*4+1] = s.particles[i].pos.y;
            s.particleBuf[i*4+2] = s.particles[i].pos.z;
            s.particleBuf[i*4+3] = s.particles[i].life;
        }
        glBindBuffer(GL_ARRAY_BUFFER, s.particleVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        s.particleBuf.size() * sizeof(float), s.particleBuf.data());
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

    // inner (dense dark dust)
    glUniform3f(pu.color, 0.25f, 0.22f, 0.2f);
    glUniform1f(pu.pointScale, 1.5f);
    glDrawArrays(GL_POINTS, 0, INNER_PARTICLES);

    // outer (lighter debris)
    glUniform3f(pu.color, 0.5f, 0.45f, 0.35f);
    glUniform1f(pu.pointScale, 2.0f);
    glDrawArrays(GL_POINTS, INNER_PARTICLES, MAX_PARTICLES - INNER_PARTICLES);

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
    }
    // Rain shader uniforms
    {
        GLuint p = app.rainProgram;
        app.ru.proj = glGetUniformLocation(p, "uProj");
        app.ru.view = glGetUniformLocation(p, "uView");
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
    // Ground plane
    // ══════════════════════════════════════
    {
        struct SV { glm::vec3 pos; glm::vec3 normal; glm::vec3 col; };
        std::vector<SV> gv = {
            {{-50,0,-50},{0,1,0},{0.15f,0.45f,0.2f}},
            {{ 50,0,-50},{0,1,0},{0.15f,0.45f,0.2f}},
            {{ 50,0, 50},{0,1,0},{0.15f,0.45f,0.2f}},
            {{-50,0, 50},{0,1,0},{0.15f,0.45f,0.2f}},
        };
        std::vector<unsigned int> gi = {0,1,2, 0,2,3};
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
        glEnableVertexAttribArray(2);
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
        glEnableVertexAttribArray(2);
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
    // Initialize destructible houses
    // ══════════════════════════════════════
    {
        glm::vec3 housePositions[] = {
            {-5,0,-3}, {4,0,-6}, {-7,0,5}, {6,0,4}
        };
        for (const auto& hp : housePositions) {
            DestructibleHouse dh;
            dh.pos = hp;
            dh.health = 1.0f;
            dh.destroyed = false;
            app.houses.push_back(dh);
        }
    }

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
