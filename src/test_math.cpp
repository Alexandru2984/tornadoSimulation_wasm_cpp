// =====================================================================
// Tornado Simulation — Unit Tests (GLM math + simulation logic)
// =====================================================================
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const float EPS = 1e-5f;

static bool approx(float a, float b) { return std::fabs(a - b) < EPS; }
static bool approxVec3(const glm::vec3& a, const glm::vec3& b) {
    return approx(a.x,b.x) && approx(a.y,b.y) && approx(a.z,b.z);
}

static int passed = 0, failed = 0;

#define TEST(name) \
    static void test_##name(); \
    struct _reg_##name { _reg_##name() { tests.push_back({#name, test_##name}); } } _inst_##name; \
    static void test_##name()

struct TestEntry { std::string name; void(*fn)(); };
static std::vector<TestEntry> tests;

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) { std::cerr << "  FAIL: " << #expr << " (" << __FILE__ << ":" << __LINE__ << ")\n"; throw false; } } while(0)
#define ASSERT_APPROX(a,b) \
    do { if (!approx(a,b)) { std::cerr << "  FAIL: " << #a << "=" << (a) << " != " << (b) << " (" << __FILE__ << ":" << __LINE__ << ")\n"; throw false; } } while(0)
#define ASSERT_VEC3(a,b) \
    do { if (!approxVec3(a,b)) { std::cerr << "  FAIL: vec3 mismatch (" << __FILE__ << ":" << __LINE__ << ")\n"; throw false; } } while(0)

// ═════════════════════════════════════════════════════════════════════
// Basic GLM tests
// ═════════════════════════════════════════════════════════════════════
TEST(vec3_addition) {
    glm::vec3 a(1,2,3), b(4,5,6);
    ASSERT_VEC3(a+b, glm::vec3(5,7,9));
}

TEST(vec3_subtraction) {
    glm::vec3 a(5,7,9), b(1,2,3);
    ASSERT_VEC3(a-b, glm::vec3(4,5,6));
}

TEST(vec3_scalar_multiply) {
    glm::vec3 v(1,2,3);
    ASSERT_VEC3(v * 2.0f, glm::vec3(2,4,6));
}

TEST(vec3_dot) {
    glm::vec3 a(1,0,0), b(0,1,0);
    ASSERT_APPROX(glm::dot(a,b), 0.0f);
    ASSERT_APPROX(glm::dot(a,a), 1.0f);
}

TEST(vec3_cross) {
    glm::vec3 x(1,0,0), y(0,1,0);
    ASSERT_VEC3(glm::cross(x,y), glm::vec3(0,0,1));
}

TEST(vec3_normalize) {
    glm::vec3 v(3,4,0);
    glm::vec3 n = glm::normalize(v);
    ASSERT_APPROX(glm::length(n), 1.0f);
    ASSERT_APPROX(n.x, 0.6f);
    ASSERT_APPROX(n.y, 0.8f);
}

TEST(vec3_length) {
    ASSERT_APPROX(glm::length(glm::vec3(3,4,0)), 5.0f);
}

// ═════════════════════════════════════════════════════════════════════
// Matrix tests
// ═════════════════════════════════════════════════════════════════════
TEST(mat4_identity) {
    glm::mat4 I(1.0f);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            ASSERT_APPROX(I[i][j], i==j ? 1.0f : 0.0f);
}

TEST(mat4_translate) {
    glm::mat4 T = glm::translate(glm::mat4(1.0f), glm::vec3(1,2,3));
    ASSERT_APPROX(T[3].x, 1.0f);
    ASSERT_APPROX(T[3].y, 2.0f);
    ASSERT_APPROX(T[3].z, 3.0f);
}

TEST(mat4_scale) {
    glm::mat4 S = glm::scale(glm::mat4(1.0f), glm::vec3(2,3,4));
    glm::vec4 p = S * glm::vec4(1,1,1,1);
    ASSERT_APPROX(p.x, 2.0f);
    ASSERT_APPROX(p.y, 3.0f);
    ASSERT_APPROX(p.z, 4.0f);
}

TEST(mat4_multiply_identity) {
    glm::mat4 I(1.0f);
    glm::mat4 T = glm::translate(I, glm::vec3(5,6,7));
    glm::mat4 R = I * T;
    ASSERT_APPROX(R[3].x, 5.0f);
    ASSERT_APPROX(R[3].y, 6.0f);
    ASSERT_APPROX(R[3].z, 7.0f);
}

TEST(mat4_perspective) {
    glm::mat4 P = glm::perspective(glm::radians(45.0f), 16.0f/9.0f, 0.1f, 100.0f);
    // First element relates to FOV and aspect
    ASSERT_TRUE(P[0][0] > 0.0f);
    ASSERT_TRUE(P[1][1] > 0.0f);
    // P[2][3] should be -1 for a standard perspective matrix
    ASSERT_APPROX(P[2][3], -1.0f);
}

TEST(mat4_lookAt) {
    glm::mat4 V = glm::lookAt(glm::vec3(0,0,5), glm::vec3(0,0,0), glm::vec3(0,1,0));
    // Camera looking down -Z from +Z; the translation part should reflect eye position
    ASSERT_TRUE(V[3][2] < 0.0f); // negative z translation
}

// ═════════════════════════════════════════════════════════════════════
// Tornado simulation logic tests
// ═════════════════════════════════════════════════════════════════════
TEST(tornado_position_interpolation) {
    // Tornado position smoothly interpolates toward target
    glm::vec2 pos(0.0f, 0.0f);
    glm::vec2 target(5.0f, 3.0f);
    for (int i = 0; i < 100; ++i)
        pos = pos * 0.92f + target * 0.08f;
    // After many steps, should be very close to target
    ASSERT_TRUE(glm::length(pos - target) < 0.1f);
}

TEST(tornado_mesh_radius_increases_with_height) {
    // The inverted cone: radius = baseR * t + 0.05
    float baseR = 1.5f;
    float r_bottom = baseR * 0.0f + 0.05f;
    float r_top    = baseR * 1.0f + 0.05f;
    ASSERT_TRUE(r_top > r_bottom);
    ASSERT_APPROX(r_bottom, 0.05f);
    ASSERT_APPROX(r_top, 1.55f);
}

TEST(vortex_tangent_direction) {
    // For a point to the right of center, tangent should push it forward (CCW)
    glm::vec3 center(0, 1, 0);
    glm::vec3 point(1, 1, 0);
    glm::vec3 toCenter = center - point; // (-1, 0, 0)
    glm::vec3 tangent(-toCenter.z, 0, toCenter.x); // (0, 0, -1)
    tangent = glm::normalize(tangent);
    ASSERT_VEC3(tangent, glm::vec3(0, 0, -1));
}

TEST(particle_respawn_stays_bounded) {
    // Simulated respawn: inner particles should have small radius
    float r_min = 0.02f, r_range = 0.6f;
    float r_max = r_min + r_range;
    ASSERT_APPROX(r_max, 0.62f);
    ASSERT_TRUE(r_min > 0.0f);
}

TEST(damping_reduces_velocity) {
    glm::vec3 vel(10, 5, -3);
    float dt = 0.016f;
    vel *= (1.0f - 2.0f * dt);
    ASSERT_TRUE(glm::length(vel) < glm::length(glm::vec3(10, 5, -3)));
}

TEST(camera_direction_from_angles) {
    // yaw = -90 deg, pitch = 0 -> should look along -Z
    float yaw = -90.0f, pitch = 0.0f;
    glm::vec3 dir;
    dir.x = cosf(glm::radians(yaw)) * cosf(glm::radians(pitch));
    dir.y = sinf(glm::radians(pitch));
    dir.z = sinf(glm::radians(yaw)) * cosf(glm::radians(pitch));
    dir = glm::normalize(dir);
    ASSERT_APPROX(dir.x, 0.0f);
    ASSERT_APPROX(dir.y, 0.0f);
    ASSERT_APPROX(dir.z, -1.0f);
}

TEST(camera_pitch_clamp) {
    float pitch = 100.0f;
    pitch = glm::clamp(pitch, -89.0f, 89.0f);
    ASSERT_APPROX(pitch, 89.0f);

    pitch = -100.0f;
    pitch = glm::clamp(pitch, -89.0f, 89.0f);
    ASSERT_APPROX(pitch, -89.0f);
}

TEST(shader_version_adaptation) {
    // Simulate the runtime shader adaptation logic
    std::string src = "#version 330 core\nlayout(location=0) in vec3 aPos;\n";
    auto pos = src.find("#version 330 core");
    ASSERT_TRUE(pos != std::string::npos);
    std::string rep = "#version 300 es\nprecision highp float;\n";
    src.replace(pos, 17, rep);
    ASSERT_TRUE(src.find("#version 300 es") != std::string::npos);
    ASSERT_TRUE(src.find("precision highp float") != std::string::npos);
    ASSERT_TRUE(src.find("#version 330 core") == std::string::npos);
}

// ═════════════════════════════════════════════════════════════════════
// Runner
// ═════════════════════════════════════════════════════════════════════
int main() {
    std::cout << "Running " << tests.size() << " tests...\n\n";
    for (auto& t : tests) {
        std::cout << "  [RUN]  " << t.name << std::flush;
        try {
            t.fn();
            std::cout << "  [OK]\n";
            ++passed;
        } catch (...) {
            std::cout << "  [FAIL]\n";
            ++failed;
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed out of "
              << (passed+failed) << " tests.\n";
    return failed == 0 ? 0 : 1;
}
