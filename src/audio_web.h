#pragma once
// =====================================================================
// Web Audio sound effects (EM_JS) + no-op desktop fallbacks
// =====================================================================

#ifdef PLATFORM_EMSCRIPTEN
  #include <emscripten/emscripten.h>
#endif

// Runtime mute flag (set from JS via toggle_sound)
static bool g_soundMuted = false;

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

    // Low storm drone: two detuned oscillators with a slow swell (LFO)
    var d1 = ctx.createOscillator(); d1.type = "sawtooth"; d1.frequency.value = 55;
    var d2 = ctx.createOscillator(); d2.type = "sawtooth"; d2.frequency.value = 55.7;
    var dlp = ctx.createBiquadFilter(); dlp.type = "lowpass"; dlp.frequency.value = 120;
    var dg = ctx.createGain(); dg.gain.value = 0.045;
    var lfo = ctx.createOscillator(); lfo.type = "sine"; lfo.frequency.value = 0.08;
    var lfoG = ctx.createGain(); lfoG.gain.value = 0.02;
    lfo.connect(lfoG); lfoG.connect(dg.gain);
    d1.connect(dlp); d2.connect(dlp); dlp.connect(dg); dg.connect(ctx.destination);
    d1.start(); d2.start(); lfo.start();
    window._tornadoAudio.musicGain = dg;
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

EM_JS(void, js_playMooSound, (), {
    var a = window._tornadoAudio; if (!a) return;
    var ctx = a.ctx;
    var osc = ctx.createOscillator();
    osc.type = "sawtooth";
    osc.frequency.setValueAtTime(180, ctx.currentTime);
    osc.frequency.linearRampToValueAtTime(120, ctx.currentTime + 0.35);
    var lp = ctx.createBiquadFilter(); lp.type = "lowpass"; lp.frequency.value = 500;
    var g = ctx.createGain();
    g.gain.setValueAtTime(0.001, ctx.currentTime);
    g.gain.linearRampToValueAtTime(0.22, ctx.currentTime + 0.06);
    g.gain.linearRampToValueAtTime(0, ctx.currentTime + 0.4);
    osc.connect(lp); lp.connect(g); g.connect(ctx.destination);
    osc.start(); osc.stop(ctx.currentTime + 0.4);
});

EM_JS(void, js_playGameOverSound, (), {
    var a = window._tornadoAudio; if (!a) return;
    var ctx = a.ctx;
    var notes = [392, 330, 262, 196];
    for (var i = 0; i < notes.length; i++) {
        var osc = ctx.createOscillator();
        osc.type = "sine"; osc.frequency.value = notes[i];
        var g = ctx.createGain();
        g.gain.setValueAtTime(0, ctx.currentTime + i*0.25);
        g.gain.linearRampToValueAtTime(0.2, ctx.currentTime + i*0.25 + 0.05);
        g.gain.linearRampToValueAtTime(0, ctx.currentTime + i*0.25 + 0.6);
        osc.connect(g); g.connect(ctx.destination);
        osc.start(ctx.currentTime + i*0.25);
        osc.stop(ctx.currentTime + i*0.25 + 0.6);
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
    // Submit to the global leaderboard too (fire and forget)
    try {
        if (score > 0 && typeof fetch === "function") {
            var name = (localStorage.getItem("tornado3d_name") || "ANONIM").slice(0, 16);
            fetch("/api/scores", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({name: name, score: score, wave: wave})
            }).catch(function(){});
        }
    } catch(e) {}
});

EM_JS(int, js_getHighScore, (), {
    try {
        var scores = JSON.parse(localStorage.getItem("tornado3d_scores") || "[]");
        return scores.length > 0 ? scores[0].score : 0;
    } catch(e) { return 0; }
});

EM_JS(void, js_playBaaSound, (), {
    var a = window._tornadoAudio; if (!a) return;
    var ctx = a.ctx;
    var osc = ctx.createOscillator();
    osc.type = "sawtooth";
    osc.frequency.setValueAtTime(340, ctx.currentTime);
    osc.frequency.linearRampToValueAtTime(300, ctx.currentTime + 0.1);
    osc.frequency.linearRampToValueAtTime(360, ctx.currentTime + 0.25);
    var lp = ctx.createBiquadFilter(); lp.type = "lowpass"; lp.frequency.value = 900;
    var g = ctx.createGain();
    g.gain.setValueAtTime(0.001, ctx.currentTime);
    g.gain.linearRampToValueAtTime(0.16, ctx.currentTime + 0.04);
    g.gain.linearRampToValueAtTime(0, ctx.currentTime + 0.3);
    osc.connect(lp); lp.connect(g); g.connect(ctx.destination);
    osc.start(); osc.stop(ctx.currentTime + 0.3);
});

// Haptic feedback — a no-op on devices without a vibration motor
EM_JS(void, js_vibrate, (int ms), {
    if (navigator.vibrate) { try { navigator.vibrate(ms); } catch(e) {} }
});

#endif // PLATFORM_EMSCRIPTEN

static void initSound() {
#ifdef PLATFORM_EMSCRIPTEN
    js_initSound();
#endif
}
static void vibrate(int ms) {
#ifdef PLATFORM_EMSCRIPTEN
    js_vibrate(ms);
#else
    (void)ms;
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
static void playGameOverSound() {
#ifdef PLATFORM_EMSCRIPTEN
    if (!g_soundMuted) js_playGameOverSound();
#endif
}
static void playMooSound() {
#ifdef PLATFORM_EMSCRIPTEN
    if (!g_soundMuted) js_playMooSound();
#endif
}
static void playBaaSound() {
#ifdef PLATFORM_EMSCRIPTEN
    if (!g_soundMuted) js_playBaaSound();
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
