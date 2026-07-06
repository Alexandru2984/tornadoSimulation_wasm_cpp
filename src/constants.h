#pragma once
// =====================================================================
// Tornado 3D — all gameplay/rendering tunables in one place
// =====================================================================

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Particles
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
static const int   ANIMALS_PER_CHUNK     = 2;
static const int   MAX_SCORCH_MARKS      = 200;

// Animals (cows that flee from the tornado)
static const float ANIMAL_FLEE_RADIUS    = 12.0f;  // start running at this distance
static const float ANIMAL_FLEE_SPEED     = 3.2f;
static const float ANIMAL_WANDER_SPEED   = 0.7f;

// Tornado decay (shrinks when idle)
static const float TORNADO_DECAY_RATE    = 0.03f;  // per second
static const float TORNADO_MIN_SCALE     = 0.4f;   // smallest possible
static const float TORNADO_DECAY_GRACE   = 2.0f;   // seconds after last destroy before decay starts

// Wave system
static const int   TOTAL_WAVES           = 10;
static const float WAVE_ANNOUNCE_TIME    = 3.0f;    // seconds to show "WAVE X"
static const int   WAVE_BASE_TARGET      = 5;       // wave 1 target = 5 destroys

// Game over: tornado stuck at minimum size for this long = defeat
static const float GAMEOVER_FADE_TIME    = 12.0f;

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
static const int   TERRAIN_GRID          = 160;      // cells per axis
static const float TERRAIN_EXTENT        = 200.0f;   // half-size in world units
static const float TERRAIN_RECENTER_DIST = 60.0f;    // rebuild grid when camera strays this far
static const float WATER_LEVEL           = 0.3f;     // Y where water surface sits

// Day/night cycle
static const float DAY_CYCLE_SPEED       = 0.015f;   // full cycle ~67 seconds
