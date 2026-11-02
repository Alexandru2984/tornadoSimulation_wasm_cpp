#pragma once
// ─────────────────────────────────────────────────────────────────────
// In-game (HUD) text in Romanian and English.
//
// The web page picks the language and calls set_language(); the desktop
// build follows $LANG. Every format string must use the same conversion
// specifiers in the same order in both languages (test_math.cpp checks),
// because the same snprintf arguments are passed whichever one is shown.
// The bitmap HUD font is ASCII-only: no diacritics here.
// ─────────────────────────────────────────────────────────────────────

enum Lang { LANG_RO = 0, LANG_EN = 1 };

enum HudStr {
    S_SCORE, S_DESTROYED, S_BREAKDOWN, S_TORNADO_SCALE, S_COMBO,
    S_TA_CLOCK, S_WAVE_ENDLESS, S_WAVE_OF,
    S_PU_SPEED, S_PU_SIZE, S_PU_MAGNET, S_PU_SHIELD, S_PU_SCORE, S_PU_FREEZE,
    S_FADING, S_TIME_ATTACK, S_WAVE_N, S_TA_SUBTITLE, S_EF, S_DESTROY_N,
    S_END_LINE1, S_END_LINE2, S_END_LINE3,
    S_TIME_UP, S_VICTORY, S_TA_3MIN, S_ALL_WAVES, S_MAX_AND_BEST,
    S_PRESS_C_R, S_PRESS_E, S_GAME_OVER, S_DISSIPATED, S_WAVE_REACHED, S_PRESS_R,
    S_COUNT
};

static const char* const HUD_TEXT[S_COUNT][2] = {
    /* S_SCORE         */ {"SCOR: %d",                        "SCORE: %d"},
    /* S_DESTROYED     */ {"DISTRUSE: %d",                    "DESTROYED: %d"},
    /* S_BREAKDOWN     */ {"CASE:%d COPACI:%d ALTE:%d",       "HOUSES:%d TREES:%d OTHER:%d"},
    /* S_TORNADO_SCALE */ {"TORNADA x%.1f",                   "TORNADO x%.1f"},
    /* S_COMBO         */ {"COMBO x%d  x%.1f",                "COMBO x%d  x%.1f"},
    /* S_TA_CLOCK      */ {"TIMP %d:%02d  EF%d",              "TIME %d:%02d  EF%d"},
    /* S_WAVE_ENDLESS  */ {"VAL %d  EF%d  INFINIT",           "WAVE %d  EF%d  ENDLESS"},
    /* S_WAVE_OF       */ {"VAL %d/%d  EF%d",                 "WAVE %d/%d  EF%d"},
    /* S_PU_SPEED      */ {"VITEZA",                          "SPEED"},
    /* S_PU_SIZE       */ {"MARIME x2",                       "SIZE x2"},
    /* S_PU_MAGNET     */ {"MAGNET",                          "MAGNET"},
    /* S_PU_SHIELD     */ {"SCUT",                            "SHIELD"},
    /* S_PU_SCORE      */ {"SCOR x2",                         "SCORE x2"},
    /* S_PU_FREEZE     */ {"INGHET",                          "FREEZE"},
    /* S_FADING        */ {"TORNADA SLABESTE: %d",            "TORNADO FADING: %d"},
    /* S_TIME_ATTACK   */ {"TIME ATTACK",                     "TIME ATTACK"},
    /* S_WAVE_N        */ {"VALUL %d",                        "WAVE %d"},
    /* S_TA_SUBTITLE   */ {"3 MINUTE - SCOR MAXIM",           "3 MINUTES - MAX SCORE"},
    /* S_EF            */ {"TORNADA EF%d",                    "TORNADO EF%d"},
    /* S_DESTROY_N     */ {"DISTRUGE %d OBIECTE",             "DESTROY %d OBJECTS"},
    /* S_END_LINE1     */ {"CASE %d  COPACI %d  ALTE %d",     "HOUSES %d  TREES %d  OTHER %d"},
    /* S_END_LINE2     */ {"ANIMALE %d  MASINI %d  PASARI %d", "ANIMALS %d  CARS %d  BIRDS %d"},
    /* S_END_LINE3     */ {"COMBO MAX %d   TIMP %d:%02d",     "MAX COMBO %d   TIME %d:%02d"},
    /* S_TIME_UP       */ {"TIMP EXPIRAT",                    "TIME UP"},
    /* S_VICTORY       */ {"VICTORIE",                        "VICTORY"},
    /* S_TA_3MIN       */ {"TIME ATTACK - 3 MINUTE",          "TIME ATTACK - 3 MINUTES"},
    /* S_ALL_WAVES     */ {"TOATE CELE %d VALURI COMPLETE",   "ALL %d WAVES COMPLETE"},
    /* S_MAX_AND_BEST  */ {"TORNADA MAX x%.1f   RECORD %d",   "MAX TORNADO x%.1f   BEST %d"},
    /* S_PRESS_C_R     */ {"APASA C = COPIAZA   R = RESTART", "PRESS C = COPY   R = RESTART"},
    /* S_PRESS_E       */ {"APASA E = MOD INFINIT",           "PRESS E = ENDLESS MODE"},
    /* S_GAME_OVER     */ {"GAME OVER",                       "GAME OVER"},
    /* S_DISSIPATED    */ {"TORNADA S-A RISIPIT",             "THE TORNADO DIED OUT"},
    /* S_WAVE_REACHED  */ {"VAL ATINS %d/%d   RECORD %d",     "WAVE REACHED %d/%d   BEST %d"},
    /* S_PRESS_R       */ {"APASA R PENTRU RESTART",          "PRESS R TO RESTART"},
};

inline int g_lang = LANG_RO;

inline const char* tr(HudStr id) {
    return HUD_TEXT[id][g_lang == LANG_EN ? 1 : 0];
}
