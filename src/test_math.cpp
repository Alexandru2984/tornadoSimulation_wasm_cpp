// =====================================================================
// Tornado Simulation — Unit Tests (GLM math + simulation logic)
// =====================================================================
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "constants.h"
#include "terrain_noise.h"
#include "i18n.h"
#include "font5x7.h"
#include <cctype>

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
// ═════════════════════════════════════════════════════════════════════
// Real game code (constants.h / terrain_noise.h), not re-implementations
// ═════════════════════════════════════════════════════════════════════
TEST(terrain_is_deterministic) {
    for (int i = 0; i < 200; ++i) {
        float x = (i * 37 % 400) - 200.0f, z = (i * 91 % 400) - 200.0f;
        ASSERT_TRUE(getTerrainHeight(x, z) == getTerrainHeight(x, z));
    }
}

TEST(spawn_plateau_is_dry_land) {
    // The player starts at the origin; objects there must not be underwater.
    for (float x = -6.0f; x <= 6.0f; x += 1.5f)
        for (float z = -6.0f; z <= 6.0f; z += 1.5f)
            ASSERT_TRUE(getTerrainHeight(x, z) > WATER_LEVEL + 0.2f);
}

TEST(terrain_height_bounded_and_finite) {
    for (int i = 0; i < 2000; ++i) {
        float x = (float)((i * 7919) % 20000) - 10000.0f;
        float z = (float)((i * 104729) % 20000) - 10000.0f;
        float h = getTerrainHeight(x, z);
        ASSERT_TRUE(std::isfinite(h));
        ASSERT_TRUE(h > -10.0f && h < 10.0f);
    }
}

// The classic world (seed 0) must never change by accident: scores and
// muscle memory depend on it. Values recorded before world seeds existed.
TEST(classic_world_is_unchanged) {
    const float golden[][3] = {
        {40.0f, -75.0f, 0.370006f}, {-130.0f, 210.0f, -2.107702f}, {512.0f, 512.0f, -0.329167f},
        {-900.0f, 33.0f, 1.173089f}, {77.0f, 1500.0f, 1.462182f},
    };
    g_worldSeed = 0;
    for (auto& g : golden) ASSERT_TRUE(fabsf(getTerrainHeight(g[0], g[1]) - g[2]) < 1e-4f);
}

// Daily worlds: a different landscape, still starting on dry flat ground.
TEST(daily_worlds_differ_and_keep_a_dry_spawn) {
    const uint32_t seeds[] = {1u, 20261103u, 0xDEADBEEFu};
    std::vector<float> classic;
    g_worldSeed = 0;
    for (int i = 0; i < 100; ++i) classic.push_back(getTerrainHeight(i * 13.7f - 600.0f, i * 29.3f - 900.0f));
    for (uint32_t seed : seeds) {
        g_worldSeed = seed;
        int differ = 0;
        for (int i = 0; i < 100; ++i) {
            float h = getTerrainHeight(i * 13.7f - 600.0f, i * 29.3f - 900.0f);
            ASSERT_TRUE(std::isfinite(h) && h > -10.0f && h < 10.0f);
            if (fabsf(h - classic[i]) > 1e-3f) ++differ;
        }
        ASSERT_TRUE(differ > 90);
        for (float x = -6.0f; x <= 6.0f; x += 1.5f)
            for (float z = -6.0f; z <= 6.0f; z += 1.5f)
                ASSERT_TRUE(getTerrainHeight(x, z) > WATER_LEVEL + 0.2f);
    }
    g_worldSeed = 0;
}

TEST(hash_noise_range) {
    for (int i = -500; i < 500; ++i) {
        float n = hashNoise(i, i * 3 + 7);
        ASSERT_TRUE(n >= -1.0f && n <= 1.0f);
    }
}

TEST(wave_targets_match_leaderboard_rules) {
    // server/leaderboard.py: kills_to_reach_wave(11) == 185
    int kills = 0;
    for (int w = 1; w <= TOTAL_WAVES; ++w) kills += WAVE_BASE_TARGET + (w - 1) * WAVE_TARGET_STEP;
    ASSERT_TRUE(kills == 185);
}

TEST(combo_multiplier_caps) {
    auto mult = [](int combo) { return 1.0f + std::min(combo / COMBO_STEP, COMBO_MAX_MULT - 1.0f); };
    ASSERT_APPROX(mult(0), 1.0f);
    ASSERT_APPROX(mult(3), 2.0f);
    ASSERT_APPROX(mult(1000), COMBO_MAX_MULT);
}

// ── HUD translations (i18n.h) ────────────────────────────────────────
// Conversion specifiers of a printf format, e.g. "%d:%02d x%.1f" -> "d02d.1f".
static std::string printfSpecs(const char* fmt) {
    std::string out;
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') continue;
        if (*++p == '%') continue;
        while (*p && !std::isalpha((unsigned char)*p)) out += *p++;
        if (*p) out += *p;
        out += ' ';
    }
    return out;
}

TEST(hud_translations_share_format_arguments) {
    for (int i = 0; i < S_COUNT; ++i) {
        ASSERT_TRUE(HUD_TEXT[i][0] && HUD_TEXT[i][1] && HUD_TEXT[i][0][0] && HUD_TEXT[i][1][0]);
        ASSERT_TRUE(printfSpecs(HUD_TEXT[i][0]) == printfSpecs(HUD_TEXT[i][1]));
    }
    ASSERT_TRUE(printfSpecs("TIME %d:%02d  EF%d x%.1f") == "d 02d d .1f ");
}

static bool hasGlyph(char c) {
    for (const GlyphDef& g : FONT_GLYPHS)
        if (g.ch == c) return true;
    return false;
}

// Every character the HUD can print has a glyph in font5x7.h (a missing one
// renders as a gap): the literal text of both languages, plus the digits,
// '.' and '-' that the %d/%f conversions produce.
TEST(hud_text_has_glyphs_in_the_bitmap_font) {
    for (int i = 0; i < S_COUNT; ++i)
        for (int l = 0; l < 2; ++l)
            for (const char* p = HUD_TEXT[i][l]; *p; ++p) {
                if (*p == '%' && p[1] != '%') {          // skip the conversion spec
                    while (*p && !std::isalpha((unsigned char)*p)) ++p;
                    while (*p && std::isalpha((unsigned char)*p) && *p != 'd' && *p != 'f' && *p != 's') ++p;
                    if (!*p) break;
                    continue;
                }
                ASSERT_TRUE((unsigned char)*p >= 32 && (unsigned char)*p < 127);
                ASSERT_TRUE(hasGlyph(*p));
            }
    for (const char* p = "0123456789.-"; *p; ++p) ASSERT_TRUE(hasGlyph(*p));
}

TEST(hud_language_switch) {
    g_lang = LANG_EN;
    ASSERT_TRUE(std::string(tr(S_VICTORY)) == "VICTORY");
    g_lang = LANG_RO;
    ASSERT_TRUE(std::string(tr(S_VICTORY)) == "VICTORIE");
}

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
