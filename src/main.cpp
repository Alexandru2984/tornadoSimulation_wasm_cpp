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
#include <map>
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
static const int MAX_PARTICLES   = 2200;
static const int INNER_PARTICLES = 1400;

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
    GLuint brickTex = 0, woodTex = 0, leafTex = 0;

    // Particles
    GLuint particleVAO = 0, particleVBO = 0;
    std::vector<Particle> particles;
    std::mt19937 rng;
    std::uniform_real_distribution<float> rnd01{0.0f, 1.0f};

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

// ── OBJ loader (kept for future use) ─────────────────────────────────
SimpleModel loadSimpleOBJ(const std::string &path) {
    std::ifstream in(path);
    SimpleModel m;
    if (!in) return m;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    struct Vert { glm::vec3 p; glm::vec3 n; };
    std::vector<Vert> verts;
    std::vector<unsigned int> indices;
    std::map<std::string, unsigned int> cache;
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2) continue;
        std::istringstream ss(line);
        std::string tag; ss >> tag;
        if (tag == "v")  { float x,y,z; ss>>x>>y>>z; positions.push_back({x,y,z}); }
        else if (tag == "vn") { float x,y,z; ss>>x>>y>>z; normals.push_back({x,y,z}); }
        else if (tag == "f") {
            std::string a,b,c; ss>>a>>b>>c;
            std::string arr[3]={a,b,c};
            for (int i=0;i<3;++i) {
                auto it = cache.find(arr[i]);
                if (it != cache.end()) { indices.push_back(it->second); continue; }
                std::string s = arr[i];
                int vIdx=0, nIdx=0;
                size_t p1 = s.find('/');
                if (p1==std::string::npos) { vIdx = std::stoi(s); }
                else {
                    vIdx = std::stoi(s.substr(0, p1));
                    size_t p2 = s.find('/', p1+1);
                    if (p2!=std::string::npos) {
                        std::string sn = s.substr(p2+1);
                        if (!sn.empty()) nIdx = std::stoi(sn);
                    }
                }
                glm::vec3 pp = positions[vIdx-1];
                glm::vec3 nn = nIdx>0 ? normals[nIdx-1] : glm::vec3(0,1,0);
                verts.push_back({pp,nn});
                unsigned int id = (unsigned int)verts.size()-1;
                cache[arr[i]] = id;
                indices.push_back(id);
            }
        }
    }
    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    glGenBuffers(1, &m.ebo);
    glBindVertexArray(m.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(Vert), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size()*sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(Vert),(void*)offsetof(Vert,p));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(Vert),(void*)offsetof(Vert,n));
    glEnableVertexAttribArray(2);
    glVertexAttrib3f(2, 0.85f, 0.85f, 0.85f);
    glBindVertexArray(0);
    m.indexCount = (GLsizei)indices.size();
    return m;
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
    glClearColor(0.18f, 0.22f, 0.45f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float aspect = (float)width / (float)height;
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 200.0f);
    glm::mat4 view = s.camera.getView();

    // -- Helper lambdas for uniform setting --
    auto setMat4 = [](GLuint prog, const char* name, const glm::mat4& m) {
        glUniformMatrix4fv(glGetUniformLocation(prog, name), 1, GL_FALSE, glm::value_ptr(m));
    };
    auto setVec3 = [](GLuint prog, const char* name, const glm::vec3& v) {
        glUniform3fv(glGetUniformLocation(prog, name), 1, glm::value_ptr(v));
    };
    auto setFloat = [](GLuint prog, const char* name, float v) {
        glUniform1f(glGetUniformLocation(prog, name), v);
    };
    auto setInt = [](GLuint prog, const char* name, int v) {
        glUniform1i(glGetUniformLocation(prog, name), v);
    };

    // ════════════════════════════════
    // MAIN SHADER — scene + tornado
    // ════════════════════════════════
    glUseProgram(s.program);
    setMat4 (s.program, "uProj",  proj);
    setMat4 (s.program, "uView",  view);
    setFloat(s.program, "uTime",  t);
    setVec3 (s.program, "uCamPos", s.camera.pos);

    // -- Ground --
    {
        glm::mat4 model(1.0f);
        setMat4 (s.program, "uModel",       model);
        setFloat(s.program, "uEnableSwirl", 0.0f);
        setVec3 (s.program, "uTint", glm::vec3(1.0f));
        setFloat(s.program, "uOpacity",     1.0f);
        setInt  (s.program, "uObjType",     3);
        setInt  (s.program, "uHasAlbedo",   0);
        glBindVertexArray(s.groundVAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    }

    // -- Houses --
    {
        setInt  (s.program, "uObjType",     1);
        setFloat(s.program, "uEnableSwirl", 0.0f);
        setFloat(s.program, "uOpacity",     1.0f);
        setInt  (s.program, "uHasAlbedo",   1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s.brickTex);
        setInt(s.program, "uAlbedo", 0);

        static const glm::vec3 housePos[] = {
            {-5,0,-3}, {4,0,-6}, {-7,0,5}, {6,0,4}
        };
        for (const auto& hp : housePos) {
            glm::mat4 model = glm::scale(
                glm::translate(glm::mat4(1.0f), hp), glm::vec3(1.5f));
            setMat4(s.program, "uModel", model);
            setVec3(s.program, "uTint", glm::vec3(1.0f, 0.95f, 0.9f));
            glBindVertexArray(s.house.vao);
            glDrawElements(GL_TRIANGLES, s.house.indexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    // -- Trees --
    {
        setInt (s.program, "uObjType",   2);
        setInt (s.program, "uHasAlbedo", 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s.leafTex);

        static const glm::vec3 treePos[] = {
            {-3,0,-5}, {3,0,-4}, {-6,0,2}, {5,0,6}, {-2,0,7}, {8,0,-2}
        };
        for (const auto& tp : treePos) {
            glm::mat4 model = glm::scale(
                glm::translate(glm::mat4(1.0f), tp), glm::vec3(2.0f));
            setMat4(s.program, "uModel", model);
            setVec3(s.program, "uTint", glm::vec3(1.0f));
            glBindVertexArray(s.tree.vao);
            glDrawElements(GL_TRIANGLES, s.tree.indexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    // -- glTF models (if loaded) --
    if (s.boxLoaded) {
        setInt  (s.program, "uObjType",     0);
        setFloat(s.program, "uEnableSwirl", 0.0f);
        setFloat(s.program, "uOpacity",     1.0f);
        setInt  (s.program, "uHasAlbedo", s.boxModel.texture ? 1 : 0);
        if (s.boxModel.texture) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s.boxModel.texture);
        }
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.5f, 2.0f));
        setMat4(s.program, "uModel", model);
        setVec3(s.program, "uTint", glm::vec3(1.0f));
        glBindVertexArray(s.boxModel.vao);
        glDrawElements(GL_TRIANGLES, s.boxModel.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    if (s.avoLoaded) {
        setInt  (s.program, "uObjType",     0);
        setFloat(s.program, "uEnableSwirl", 0.0f);
        setFloat(s.program, "uOpacity",     1.0f);
        setInt  (s.program, "uHasAlbedo", s.avocadoModel.texture ? 1 : 0);
        if (s.avocadoModel.texture) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s.avocadoModel.texture);
        }
        glm::mat4 model = glm::scale(
            glm::translate(glm::mat4(1.0f), glm::vec3(-3.0f, 0.5f, 3.0f)),
            glm::vec3(30.0f));
        setMat4(s.program, "uModel", model);
        setVec3(s.program, "uTint", glm::vec3(1.0f));
        glBindVertexArray(s.avocadoModel.vao);
        glDrawElements(GL_TRIANGLES, s.avocadoModel.indexCount, GL_UNSIGNED_INT, nullptr);
    }

    // -- Tornado mesh (swirl enabled, semi-transparent) --
    {
        glm::mat4 model = glm::translate(glm::mat4(1.0f),
                              glm::vec3(s.tornadoPos.x, 0.0f, s.tornadoPos.y));
        setMat4 (s.program, "uModel",       model);
        setFloat(s.program, "uEnableSwirl", 1.0f);
        setVec3 (s.program, "uTint", glm::vec3(0.8f, 0.8f, 0.9f));
        setFloat(s.program, "uOpacity",     0.7f);
        setInt  (s.program, "uObjType",     0);
        setInt  (s.program, "uHasAlbedo",   0);
        glBindVertexArray(s.tornadoVAO);
        glDrawElements(GL_TRIANGLES, s.tornadoIndexCount, GL_UNSIGNED_INT, nullptr);
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

        float vortex = inner ? 4.0f : 2.0f;
        float up     = inner ? 2.5f : 1.0f;
        float pull   = inner ? 1.5f : 0.5f;

        p.vel += tangent * vortex * dt;
        p.vel.y += up * dt;
        if (dist > 0.1f)
            p.vel += glm::normalize(glm::vec3(toCenter.x, 0, toCenter.z)) * pull * dt;
        p.vel *= (1.0f - 2.0f * dt);
        p.pos += p.vel * dt;
        p.life -= dt;

        if (p.life <= 0.0f) {
            respawnParticle(p, inner);
            p.pos.x += s.tornadoPos.x;
            p.pos.z += s.tornadoPos.y;
        }
    }

    // Upload to GPU (vec3 pos + float life per particle)
    {
        std::vector<float> buf(MAX_PARTICLES * 4);
        for (int i = 0; i < MAX_PARTICLES; ++i) {
            buf[i*4+0] = s.particles[i].pos.x;
            buf[i*4+1] = s.particles[i].pos.y;
            buf[i*4+2] = s.particles[i].pos.z;
            buf[i*4+3] = s.particles[i].life;
        }
        glBindBuffer(GL_ARRAY_BUFFER, s.particleVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        buf.size() * sizeof(float), buf.data());
    }

    // Draw particles
    glUseProgram(s.particleProgram);
    setMat4(s.particleProgram, "uProj", proj);
    setMat4(s.particleProgram, "uView", view);
    setMat4(s.particleProgram, "uModel", glm::mat4(1.0f));

#ifndef PLATFORM_EMSCRIPTEN
    glEnable(GL_PROGRAM_POINT_SIZE);
#endif

    glBindVertexArray(s.particleVAO);

    // inner (dense dark dust)
    setVec3 (s.particleProgram, "uColor", glm::vec3(0.25f, 0.22f, 0.2f));
    setFloat(s.particleProgram, "uPointScale", 1.5f);
    glDrawArrays(GL_POINTS, 0, INNER_PARTICLES);

    // outer (lighter debris)
    setVec3 (s.particleProgram, "uColor", glm::vec3(0.5f, 0.45f, 0.35f));
    setFloat(s.particleProgram, "uPointScale", 2.0f);
    glDrawArrays(GL_POINTS, INNER_PARTICLES, MAX_PARTICLES - INNER_PARTICLES);

    glBindVertexArray(0);

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
    // Wood
    {
        const int CX = 32, CY = 32;
        std::vector<unsigned char> px(CX*CY*3);
        for (int y = 0; y < CY; ++y) for (int x = 0; x < CX; ++x) {
            int i = (y*CX+x)*3;
            float v = 140.0f + 30.0f * sinf((float)x*0.6f + (y%3));
            px[i]   = (unsigned char)glm::clamp(v+10.0f, 0.0f, 255.0f);
            px[i+1] = (unsigned char)glm::clamp(v-20.0f, 0.0f, 255.0f);
            px[i+2] = (unsigned char)glm::clamp(v-45.0f, 0.0f, 255.0f);
        }
        glGenTextures(1, &app.woodTex);
        glBindTexture(GL_TEXTURE_2D, app.woodTex);
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
        const int segments = 128;
        const int rings    = 80;
        const float ht = 6.0f;
        const float baseR  = 1.5f;

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
    // Procedural house
    // ══════════════════════════════════════
    {
        struct PV { glm::vec3 p; glm::vec3 n; glm::vec2 uv; };
        std::vector<PV> hv;
        std::vector<unsigned int> hi;
        float hw=0.6f, hh=1.0f, hd=0.6f;
        std::vector<glm::vec3> pos = {
            {-hw,0,-hd},{hw,0,-hd},{hw,0,hd},{-hw,0,hd},
            {-hw,hh,-hd},{hw,hh,-hd},{hw,hh,hd},{-hw,hh,hd}};
        for (auto &p: pos)
            hv.push_back({p, {0,1,0}, {(p.x+hw)/(2.f*hw), p.y/(hh+0.6f)}});
        unsigned int ci[] = {0,1,2,0,2,3, 4,5,6,4,6,7, 0,1,5,0,5,4,
                             1,2,6,1,6,5, 2,3,7,2,7,6, 3,0,4,3,4,7};
        hi.insert(hi.end(), std::begin(ci), std::end(ci));
        glm::vec3 apex(0, hh+0.6f, 0);
        hv.push_back({apex, {0,1,0}, {0.5f,1.0f}});
        unsigned int ai = (unsigned int)hv.size()-1;
        unsigned int ri[] = {4,ai,5, 5,ai,6, 6,ai,7, 7,ai,4};
        hi.insert(hi.end(), std::begin(ri), std::end(ri));

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
    // Procedural tree (trunk + layered cones)
    // ══════════════════════════════════════
    {
        struct PV { glm::vec3 p; glm::vec3 n; glm::vec2 uv; };
        std::vector<PV> tv;
        std::vector<unsigned int> ti;
        float tw=0.15f, th=0.5f;
        std::vector<glm::vec3> tpos = {
            {-tw,0,-tw},{tw,0,-tw},{tw,0,tw},{-tw,0,tw},
            {-tw,th,-tw},{tw,th,-tw},{tw,th,tw},{-tw,th,tw}};
        for (auto &p: tpos)
            tv.push_back({p, {0,1,0}, {(p.x+tw)/(2.f*tw), p.y/(th+0.4f)}});
        unsigned int ti2[] = {0,1,2,0,2,3, 4,5,6,4,6,7, 0,1,5,0,5,4,
                              1,2,6,1,6,5, 2,3,7,2,7,6, 3,0,4,3,4,7};
        ti.insert(ti.end(), std::begin(ti2), std::end(ti2));
        int seg = 6;
        for (int layer = 0; layer < 3; ++layer) {
            float baseY  = th + layer*0.25f + 0.1f;
            float radius = 0.6f - layer*0.15f;
            int start = (int)tv.size();
            for (int ss = 0; ss < seg; ++ss) {
                float a = ss/(float)seg * 2.0f * (float)M_PI;
                tv.push_back({{cosf(a)*radius, baseY, sinf(a)*radius},
                              {0,1,0},
                              {cosf(a)*0.5f+0.5f, baseY/(th+1.2f)}});
            }
            tv.push_back({{0, baseY+0.4f, 0}, {0,1,0}, {0.5f,1.0f}});
            int apex = (int)tv.size()-1;
            for (int ss = 0; ss < seg; ++ss) {
                ti.push_back(start+ss);
                ti.push_back(start+((ss+1)%seg));
                ti.push_back(apex);
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
    glDeleteBuffers(1, &app.particleVBO);
    glDeleteVertexArrays(1, &app.particleVAO);
    glDeleteTextures(1, &app.brickTex);
    glDeleteTextures(1, &app.woodTex);
    glDeleteTextures(1, &app.leafTex);
    glDeleteProgram(app.particleProgram);
    glDeleteProgram(app.program);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
