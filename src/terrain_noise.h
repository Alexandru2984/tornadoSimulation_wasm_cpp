#pragma once
// =====================================================================
// Procedural terrain noise & height function
// =====================================================================

#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

#include "constants.h"

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
