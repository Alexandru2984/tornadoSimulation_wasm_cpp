#pragma once
// =====================================================================
// C++ -> page bridge: run lifecycle events and run statistics.
//
// The game owns the simulation; the page (web/app.js) owns everything
// around it: menus, the end-of-run panel, local history and the global
// leaderboard. The game only announces when a run starts and ends and
// answers get_run_stat(which) queries.
// =====================================================================

#ifdef PLATFORM_EMSCRIPTEN
  #include <emscripten/emscripten.h>
#endif

// The web build replaces the in-canvas end screens with HTML panels and
// starts in an attract-mode menu instead of dropping straight into a run.
#ifdef PLATFORM_EMSCRIPTEN
static constexpr bool kWebUi = true;
#else
static constexpr bool kWebUi = false;
#endif

// get_run_stat(which) — keep in sync with STAT in web/app.js
enum RunStat {
    STAT_SCORE = 0, STAT_WAVE, STAT_HOUSES, STAT_TREES, STAT_PROPS,
    STAT_ANIMALS, STAT_VEHICLES, STAT_BIRDS, STAT_MAX_COMBO, STAT_RUN_TIME,
    STAT_MODE, STAT_DIFFICULTY, STAT_ENDLESS, STAT_DESTROYED, STAT_PHASE,
    STAT_TORNADO_X10
};

// Run modes — keep in sync with MODES in web/app.js and the API's MODES.
// Daily: Time Attack rules on the day's world, Normal difficulty, same start.
enum RunMode { MODE_CLASSIC = 0, MODE_TIME_ATTACK = 1, MODE_DAILY = 2 };

// How a run ended — keep in sync with the 'tornado:runend' handler in app.js
enum RunEnd { RUN_END_VICTORY = 0, RUN_END_GAME_OVER = 1, RUN_END_TIME_UP = 2 };

#ifdef PLATFORM_EMSCRIPTEN
// Events are dispatched on the next tick so page handlers never re-enter the
// WASM module from inside main_loop().
EM_JS(void, js_onRunStart, (int mode, int difficulty), {
    setTimeout(function() {
        window.dispatchEvent(new CustomEvent("tornado:runstart",
            {detail: {mode: mode, difficulty: difficulty}}));
    }, 0);
});

EM_JS(void, js_onRunEnd, (int outcome), {
    setTimeout(function() {
        window.dispatchEvent(new CustomEvent("tornado:runend",
            {detail: {outcome: outcome}}));
    }, 0);
});

EM_JS(int, js_getHighScore, (), {
    try {
        var scores = JSON.parse(localStorage.getItem("tornado3d_scores") || "[]");
        return (scores.length > 0 && scores[0].score | 0) || 0;
    } catch (e) { return 0; }
});
#endif

static void notifyRunStart(int mode, int difficulty) {
#ifdef PLATFORM_EMSCRIPTEN
    js_onRunStart(mode, difficulty);
#else
    (void)mode; (void)difficulty;
#endif
}

static void notifyRunEnd(RunEnd outcome) {
#ifdef PLATFORM_EMSCRIPTEN
    js_onRunEnd((int)outcome);
#else
    (void)outcome;
#endif
}

static int getHighScore() {
#ifdef PLATFORM_EMSCRIPTEN
    return js_getHighScore();
#else
    return 0;
#endif
}
