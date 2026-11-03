/* Tornada 3D — page logic around the WebAssembly game.
 *
 * The C++ game (tornado.js/.wasm) owns the simulation. This file owns the
 * page: loading, menus, settings, touch/gamepad input, the end-of-run panel,
 * local history and the global leaderboard (/api/v2, see server/).
 *
 * No inline scripts or styles anywhere, so the CSP needs no 'unsafe-inline'.
 * User-facing strings are written in Romanian and go through t(), which
 * looks up the English text in i18n.js when the page is in English.
 */
(function () {
    'use strict';

    // ── Constants shared with the C++ side (src/web_bridge.h) ─────────
    var STAT = {
        SCORE: 0, WAVE: 1, HOUSES: 2, TREES: 3, PROPS: 4, ANIMALS: 5, VEHICLES: 6,
        BIRDS: 7, MAX_COMBO: 8, RUN_TIME: 9, MODE: 10, DIFFICULTY: 11, ENDLESS: 12,
        DESTROYED: 13, PHASE: 14, TORNADO_X10: 15
    };
    var PHASE = { PLAYING: 0, ANNOUNCE: 1, VICTORY: 2, GAME_OVER: 3, MENU: 4 };
    var OUTCOME = { VICTORY: 0, GAME_OVER: 1, TIME_UP: 2 };
    var MODES = ['classic', 'timeattack', 'daily'];          // = RunMode in src/web_bridge.h
    var MODE_LABELS = ['Clasic', 'Time Attack', 'Provocarea zilei'];
    var DAILY = 2;
    var DIFF_LABELS = ['Ușor', 'Normal', 'Greu'];
    var API = '/api/v2';
    var NAME_RE = /^[A-Za-z0-9 _.\-]{1,16}$/;
    var NUM = new Intl.NumberFormat('ro-RO');      // replaced by applyLanguage()

    var IS_TOUCH = (window.matchMedia && matchMedia('(pointer: coarse)').matches) ||
                   navigator.maxTouchPoints > 0;
    var PREFERS_REDUCED = !!(window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches);

    // ── Tiny helpers ──────────────────────────────────────────────────
    function $(sel, root) { return (root || document).querySelector(sel); }
    function $all(sel, root) { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); }
    function fmt(n) { return NUM.format(n | 0); }
    function clamp(v, a, b) { return Math.min(b, Math.max(a, v)); }
    function fmtTime(sec) {
        sec = Math.max(0, sec | 0);
        return Math.floor(sec / 60) + ':' + ('0' + (sec % 60)).slice(-2);
    }

    // localStorage can throw (private mode, blocked storage): never let it break the game.
    var store = {
        get: function (key, fallback) {
            try {
                var v = localStorage.getItem(key);
                return v === null ? fallback : JSON.parse(v);
            } catch (e) { return fallback; }
        },
        getRaw: function (key, fallback) {
            try { var v = localStorage.getItem(key); return v === null ? fallback : v; }
            catch (e) { return fallback; }
        },
        set: function (key, value) {
            try { localStorage.setItem(key, typeof value === 'string' ? value : JSON.stringify(value)); }
            catch (e) { /* storage unavailable */ }
        },
        remove: function (key) { try { localStorage.removeItem(key); } catch (e) { /* ignore */ } }
    };

    var KEYS = {
        settings: 'tornado3d_settings', scores: 'tornado3d_scores', name: 'tornado3d_name',
        player: 'tornado3d_player', tutorial: 'tornado3d_seen_tutorial', profile: 'tornado3d_profile'
    };

    function toast(msg, ms) {
        var box = $('#toasts');
        var el = document.createElement('div');
        el.className = 'toast';
        el.textContent = msg;
        box.appendChild(el);
        while (box.children.length > 3) box.removeChild(box.firstChild);
        setTimeout(function () {
            el.classList.add('leaving');
            setTimeout(function () { if (el.parentNode) el.parentNode.removeChild(el); }, 300);
        }, ms || 2600);
    }

    // ── Settings ──────────────────────────────────────────────────────
    var defaults = {
        quality: 'auto', volume: 0.8, muted: false, haptics: true,
        reducedMotion: PREFERS_REDUCED, fps: false, difficulty: 1, lang: 'auto'
    };
    var settings = (function () {
        var s = store.get(KEYS.settings, {}) || {};
        var out = {};
        Object.keys(defaults).forEach(function (k) { out[k] = (k in s) ? s[k] : defaults[k]; });
        if (out.quality !== 'auto' && [0, 1, 2].indexOf(out.quality) < 0) out.quality = 'auto';
        if ([0, 1, 2].indexOf(out.difficulty) < 0) out.difficulty = 1;
        if (['auto', 'ro', 'en'].indexOf(out.lang) < 0) out.lang = 'auto';
        out.volume = clamp(+out.volume || 0, 0, 1);
        return out;
    })();
    function saveSettings() { store.set(KEYS.settings, settings); }

    // ── Language ──────────────────────────────────────────────────────
    // Romanian is the page's own language (HTML, title, search engines);
    // English comes from the dictionary in i18n.js, keyed by the Romanian
    // text. The static page is translated in place (text nodes plus a few
    // attributes, originals kept for switching back); everything this file
    // builds goes through t(). The game's HUD gets set_language().
    var EN = window.TORNADO_EN || {};
    var lang = 'ro';
    var pageTranslated = false;
    var PAGE_TITLE = document.title;
    // Elements whose text this file writes itself: the page walker skips
    // them and they are re-rendered when the language changes.
    var DYNAMIC = '#load-text, #global-stats, #build-label, #btn-lang, #error-title, #error-text, #daily-sub, ' +
                  '#end-title, #end-sub, #end-score, #end-stats, #end-unlocks, #submit-status, #btn-submit, ' +
                  '#scores-body, #scores-status, #profile-since, #profile-stats, #ach-list, #ach-count, ' +
                  '#fps, #volume-out, #toasts, #btn-pause, #btn-fullscreen';
    var I18N_ATTRS = ['aria-label', 'title', 'placeholder'];
    var originals = new WeakMap();        // text node -> Romanian text, element -> {attr: text}
    // Search engines render pages with an English browser; they get the
    // page's declared language so the indexed text matches lang/title/meta.
    var IS_CRAWLER = /bot\b|crawl|spider|slurp|lighthouse|inspectiontool/i.test(navigator.userAgent || '');

    function t(ro, vars) {
        var s = lang === 'en' && Object.prototype.hasOwnProperty.call(EN, ro) ? EN[ro] : ro;
        if (!vars) return s;
        return s.replace(/\{(\w+)\}/g, function (m, k) { return k in vars ? String(vars[k]) : m; });
    }

    // 'ro' or 'en' from the browser's preferred languages (first match
    // wins); anything else gets English.
    function detectLang() {
        var list = (navigator.languages && navigator.languages.length) ? navigator.languages : [navigator.language || ''];
        for (var i = 0; i < list.length; i++) {
            var primary = String(list[i]).toLowerCase().split('-')[0];
            if (primary === 'ro' || primary === 'mo') return 'ro';
            if (primary === 'en') return 'en';
        }
        return 'en';
    }
    function wantedLang() {
        if (settings.lang === 'ro' || settings.lang === 'en') return settings.lang;
        return IS_CRAWLER ? 'ro' : detectLang();
    }

    function translateText(s) {
        var key = s.replace(/\s+/g, ' ').trim();
        var out = t(key);
        return out === key ? s : s.match(/^\s*/)[0] + out + s.match(/\s*$/)[0];
    }
    function translatePage() {
        if (lang === 'ro' && !pageTranslated) return;     // still the original markup
        pageTranslated = true;
        var walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, {
            acceptNode: function (n) {
                var p = n.parentElement;
                return p && /\S/.test(n.nodeValue) && !p.closest('script, style, noscript, svg, ' + DYNAMIC)
                    ? NodeFilter.FILTER_ACCEPT : NodeFilter.FILTER_SKIP;
            }
        });
        for (var n = walker.nextNode(); n; n = walker.nextNode()) {
            if (!originals.has(n)) originals.set(n, n.nodeValue);
            n.nodeValue = translateText(originals.get(n));
        }
        $all('[' + I18N_ATTRS.join('], [') + ']').forEach(function (el) {
            if (el.closest(DYNAMIC)) return;
            var o = originals.get(el);
            if (!o) {
                o = {};
                I18N_ATTRS.forEach(function (a) { if (el.hasAttribute(a)) o[a] = el.getAttribute(a); });
                originals.set(el, o);
            }
            Object.keys(o).forEach(function (a) { el.setAttribute(a, translateText(o[a])); });
        });
    }

    function playerName() {
        var n = (store.getRaw(KEYS.name, '') || '').trim();
        return NAME_RE.test(n) ? n : '';
    }
    function setPlayerName(n) {
        n = (n || '').replace(/[^A-Za-z0-9 _.\-]/g, '').slice(0, 16);
        store.set(KEYS.name, n.trim());
        $('#set-name').value = n;
        $('#end-name').value = n;
        return n.trim();
    }
    function cleanNameInput(el) {
        var clean = el.value.replace(/[^A-Za-z0-9 _.\-]/g, '').slice(0, 16);
        if (clean !== el.value) el.value = clean;
        return clean.trim();
    }

    // Random per-device key that "owns" the chosen name on the server.
    function playerKey() {
        var k = store.getRaw(KEYS.player, '');
        if (!/^[A-Za-z0-9_-]{22,64}$/.test(k)) {
            var bytes = new Uint8Array(24);
            crypto.getRandomValues(bytes);
            k = btoa(String.fromCharCode.apply(null, bytes))
                .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
            store.set(KEYS.player, k);
        }
        return k;
    }

    // ── Daily challenge ───────────────────────────────────────────────
    // Everyone plays the same world on a given UTC day: its seed is a hash
    // of the date, and the server ranks a daily run against that day only.
    function utcDay(offsetDays) {
        return new Date(Date.now() - (offsetDays || 0) * 86400000).toISOString().slice(0, 10);
    }
    function dailySeed(day) {                       // FNV-1a, never 0 (0 = classic world)
        var h = 0x811c9dc5;
        for (var i = 0; i < day.length; i++) { h ^= day.charCodeAt(i); h = Math.imul(h, 0x01000193); }
        return (h >>> 0) || 1;
    }
    function fmtDay(day) {
        try {
            return new Intl.DateTimeFormat(lang === 'en' ? 'en-US' : 'ro-RO',
                { day: 'numeric', month: 'long', timeZone: 'UTC' }).format(new Date(day + 'T12:00:00Z'));
        } catch (e) { return day; }
    }
    function untilNextDay() {
        var mins = Math.max(1, Math.ceil((86400000 - Date.now() % 86400000) / 60000));
        return mins >= 60 ? Math.floor(mins / 60) + ' h ' + (mins % 60) + ' min' : mins + ' min';
    }
    function bestLocalDaily(day) {
        var list = store.get(KEYS.scores, []);
        return (Array.isArray(list) ? list : []).reduce(function (m, x) {
            return x.mode === DAILY && x.day === day ? Math.max(m, x.score | 0) : m;
        }, 0);
    }
    function renderDailyButton() {
        var best = bestLocalDaily(utcDay());
        $('#daily-sub').textContent = best
            ? t('Recordul tău azi: {score} · nouă hartă în {time}', { score: fmt(best), time: untilNextDay() })
            : t('Aceeași lume pentru toți · nouă hartă în {time}', { time: untilNextDay() });
    }

    // ── State ─────────────────────────────────────────────────────────
    var state = {
        ready: false,
        inRun: false,         // a run has been started from the menu
        paused: false,
        mode: 0,
        run: null,            // {id, mode, difficulty, submitted, promise}
        lastEnd: null,        // stats of the last finished run
        quality: 2,           // effective quality level 0..2
        dprCap: 2,
        wakeLock: null,
        gamepad: false
    };

    function M(name) {
        var fn = window.Module && Module['_' + name];
        return typeof fn === 'function' ? fn : null;
    }
    function call(name) {
        var fn = M(name);
        if (!fn) return 0;
        return fn.apply(null, Array.prototype.slice.call(arguments, 1));
    }
    function stat(which) { return call('get_run_stat', which) | 0; }
    function phase() { return state.ready ? stat(STAT.PHASE) : PHASE.MENU; }
    function runActive() {
        var p = phase();
        return state.inRun && (p === PHASE.PLAYING || p === PHASE.ANNOUNCE);
    }

    // ── Canvas sizing ─────────────────────────────────────────────────
    // The drawing buffer must match the display size in device pixels; the
    // DPR cap follows the quality level (rendering resolution is what
    // costs the most on phones).
    var canvas = $('#canvas');
    var stage = $('#stage');
    function resizeCanvas() {
        var dpr = Math.min(window.devicePixelRatio || 1, state.dprCap);
        var w = Math.max(1, Math.round(stage.clientWidth * dpr));
        var h = Math.max(1, Math.round(stage.clientHeight * dpr));
        var maxPix = IS_TOUCH ? 2.6e6 : 8.3e6;
        if (w * h > maxPix) { var k = Math.sqrt(maxPix / (w * h)); w = Math.round(w * k); h = Math.round(h * k); }
        if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    }
    window.addEventListener('resize', resizeCanvas);
    if (window.visualViewport) visualViewport.addEventListener('resize', resizeCanvas);
    if (window.ResizeObserver) new ResizeObserver(resizeCanvas).observe(stage);
    canvas.addEventListener('contextmenu', function (e) { e.preventDefault(); });

    // ── Applying settings to the game ─────────────────────────────────
    function applyQuality() {
        var q = settings.quality === 'auto' ? (state.autoQuality != null ? state.autoQuality : (IS_TOUCH ? 1 : 2))
                                            : settings.quality;
        state.quality = q;
        state.dprCap = [1, 1.5, 2][q];
        call('set_quality', q);
        resizeCanvas();
    }
    function effectiveVolume() { return settings.muted ? 0 : settings.volume; }
    function applyAudio() {
        window._tornadoVolume = effectiveVolume();
        var a = window._tornadoAudio;
        if (a && a.master) a.master.gain.value = window._tornadoVolume;
        call('toggle_sound', settings.muted || settings.volume === 0 ? 1 : 0);
        var btn = $('#btn-sound');
        var on = !settings.muted;
        btn.setAttribute('aria-pressed', String(on));
        btn.querySelector('use').setAttribute('href', on ? '#i-sound' : '#i-mute');
    }
    function applyAll() {
        applyLanguage();
        applyQuality();
        applyAudio();
        call('set_difficulty', settings.difficulty);
        call('set_reduced_motion', settings.reducedMotion ? 1 : 0);
        $('#fps').hidden = !settings.fps;
        syncSettingsUi();
    }

    // Haptics toggle: the game calls navigator.vibrate directly.
    (function wrapVibrate() {
        if (!navigator.vibrate) return;
        var real = navigator.vibrate.bind(navigator);
        try {
            navigator.vibrate = function (p) { return settings.haptics ? real(p) : false; };
        } catch (e) { /* read-only in some engines: haptics stay on */ }
    })();

    // ── Keyboard guard ────────────────────────────────────────────────
    // Emscripten's GLFW listens for keys on window in the capture phase and
    // calls preventDefault() on Backspace/Tab, which broke editing the name
    // field, and WASD typed into an input moved the camera. This listener is
    // registered before tornado.js loads, so it runs first and keeps keys
    // away from the game whenever the page (not the game) has the keyboard.
    function isTyping(t) {
        return t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' ||
                     t.tagName === 'SELECT' || t.isContentEditable);
    }
    function openDialog() { return $('dialog[open]'); }

    function onKeyCapture(e) {
        if (e.type === 'keyup') return;          // let releases through: no stuck keys
        if (isTyping(e.target)) { e.stopImmediatePropagation(); return; }
        var dlg = openDialog();
        var code = e.code;
        if (dlg) {
            e.stopImmediatePropagation();
            if (e.type !== 'keydown' || e.repeat) return;
            if (dlg.id === 'dlg-end') {
                if (code === 'KeyR') $('#btn-again').click();
                else if (code === 'KeyE' && !$('#btn-endless').hidden) $('#btn-endless').click();
                else if (code === 'KeyC') share();
            } else if (dlg.id === 'dlg-pause' && (code === 'KeyP')) {
                resume();
            }
            return;
        }
        if (!state.inRun) {                       // start screen
            e.stopImmediatePropagation();
            if (e.type === 'keydown' && code === 'Enter' && e.target === document.body && state.ready) startRun(0);
            return;
        }
        if (e.type !== 'keydown' || e.repeat) return;
        switch (code) {
            case 'KeyP': case 'Escape': togglePause(); break;
            case 'KeyM': toggleMute(); break;
            case 'KeyF': toggleFullscreen(); break;
            case 'KeyC': share(); break;
            case 'KeyR': restartPressed(); break;
            default: break;
        }
    }

    // ── Audio unlock (must happen inside a user gesture) ──────────────
    // iOS Safari / WKWebView only accept "up" events as gestures and may
    // need several attempts, so keep trying until the context runs.
    function unlockAudio() {
        if (!state.ready) return;
        call('notify_user_gesture');
        var a = window._tornadoAudio;
        if (!a || !a.ctx) return;
        if (a.master) a.master.gain.value = effectiveVolume();
        if (a.ctx.state === 'suspended') a.ctx.resume();
        try {
            var src = a.ctx.createBufferSource();
            src.buffer = a.ctx.createBuffer(1, 1, 22050);
            src.connect(a.ctx.destination);
            src.start(0);
        } catch (e) { /* ignore */ }
        if (a.ctx.state === 'running') {
            ['touchend', 'click', 'keydown'].forEach(function (t) { window.removeEventListener(t, unlockAudio, true); });
        }
    }
    // Registered first (capture) so the key guard below cannot swallow it.
    ['touchend', 'click', 'keydown'].forEach(function (t) { window.addEventListener(t, unlockAudio, true); });
    window.addEventListener('keydown', onKeyCapture, true);
    window.addEventListener('keypress', onKeyCapture, true);

    // ── Dialog management ─────────────────────────────────────────────
    // Any dialog opened during a run pauses the game; closing the last one
    // resumes it unless the player paused on purpose.
    function showDialog(id) {
        var d = document.getElementById(id);
        if (d.open) return d;
        if (runActive() && !state.paused) {
            setPaused(true);
            d.dataset.autoPaused = '1';
        }
        if (typeof d.showModal === 'function') d.showModal(); else d.setAttribute('open', '');
        return d;
    }
    function closeDialog(id) {
        var d = typeof id === 'string' ? document.getElementById(id) : id;
        if (d && d.open) d.close();
    }
    $all('dialog').forEach(function (d) {
        d.addEventListener('close', function () {
            if (d.dataset.autoPaused) {
                delete d.dataset.autoPaused;
                if (!openDialog() && state.inRun) setPaused(false);
            }
            if (state.inRun && !openDialog()) canvas.focus({ preventScroll: true });
        });
        // Click on the backdrop closes non-critical dialogs
        d.addEventListener('click', function (e) {
            if (e.target === d && d.id !== 'dlg-end') d.close();
        });
        $all('[data-close]', d).forEach(function (b) { b.addEventListener('click', function () { d.close(); }); });
    });
    // The end-of-run panel needs an explicit choice
    $('#dlg-end').addEventListener('cancel', function (e) { e.preventDefault(); });
    // Esc on the pause menu resumes
    $('#dlg-pause').addEventListener('cancel', function (e) { e.preventDefault(); resume(); });

    // ── Pause ─────────────────────────────────────────────────────────
    function setPaused(p) {
        state.paused = p;
        call('set_paused', p ? 1 : 0);
        var btn = $('#btn-pause');
        btn.querySelector('use').setAttribute('href', p ? '#i-play' : '#i-pause');
        syncHudLabels();
        if (p) releaseWakeLock(); else if (state.inRun) requestWakeLock();
    }
    // Pause/fullscreen buttons change label with their state (and language).
    function syncHudLabels() {
        var pl = t(state.paused ? 'Continuă (P)' : 'Pauză (P)');
        var fl = t(fsElement() ? 'Ieși din ecran complet (F)' : 'Ecran complet (F)');
        [['#btn-pause', pl], ['#btn-fullscreen', fl]].forEach(function (b) {
            var el = $(b[0]);
            el.setAttribute('aria-label', b[1]);
            el.title = b[1];
        });
    }
    function pause() {
        if (!runActive()) return;
        if (!state.paused) setPaused(true);
        var d = document.getElementById('dlg-pause');
        if (!d.open) { if (d.showModal) d.showModal(); else d.setAttribute('open', ''); }
    }
    function resume() {
        $all('dialog[open]').forEach(function (d) { delete d.dataset.autoPaused; d.close(); });
        setPaused(false);
    }
    function togglePause() { if (state.paused) resume(); else pause(); }

    document.addEventListener('visibilitychange', function () {
        if (document.hidden) pause();
        else if (state.inRun && !state.paused) requestWakeLock();
    });
    window.addEventListener('blur', function () { if (state.ready) pause(); });

    // ── Wake lock (keep the phone screen on while playing) ────────────
    function requestWakeLock() {
        if (!('wakeLock' in navigator) || state.wakeLock || document.hidden) return;
        navigator.wakeLock.request('screen').then(function (lock) {
            state.wakeLock = lock;
            lock.addEventListener('release', function () { state.wakeLock = null; });
        }).catch(function () { /* denied / unsupported */ });
    }
    function releaseWakeLock() {
        if (state.wakeLock) { state.wakeLock.release().catch(function () {}); state.wakeLock = null; }
    }

    // ── Fullscreen ────────────────────────────────────────────────────
    var root = document.documentElement;
    var fsSupported = !!(root.requestFullscreen || root.webkitRequestFullscreen);
    function fsElement() { return document.fullscreenElement || document.webkitFullscreenElement; }
    function toggleFullscreen() {
        if (!fsSupported) return;
        if (fsElement()) {
            (document.exitFullscreen || document.webkitExitFullscreen).call(document);
            return;
        }
        var req = (root.requestFullscreen || root.webkitRequestFullscreen).call(root, { navigationUI: 'hide' });
        Promise.resolve(req).then(function () {
            // Phones: landscape is the intended orientation
            if (IS_TOUCH && screen.orientation && screen.orientation.lock) {
                screen.orientation.lock('landscape').catch(function () {});
            }
        }).catch(function () { toast(t('Ecranul complet nu este disponibil aici')); });
    }
    function onFsChange() {
        var on = !!fsElement();
        var btn = $('#btn-fullscreen');
        btn.querySelector('use').setAttribute('href', on ? '#i-shrink' : '#i-expand');
        syncHudLabels();
        setTimeout(resizeCanvas, 60);
    }
    document.addEventListener('fullscreenchange', onFsChange);
    document.addEventListener('webkitfullscreenchange', onFsChange);
    if (!fsSupported) $('#btn-fullscreen').hidden = true;   // iPhone: install as an app instead

    // ── Run lifecycle ─────────────────────────────────────────────────
    function setInRun(on) {
        state.inRun = on;
        document.body.classList.toggle('in-run', on);
        $('#hud').hidden = !on;
        $('#touch-layer').hidden = !(on && IS_TOUCH);
    }

    function startRun(mode) {
        if (!state.ready) return;
        if (!store.getRaw(KEYS.tutorial, '')) {
            // First run: show how to play, start when the player closes it.
            store.set(KEYS.tutorial, '1');
            var d = showDialog('dlg-help');
            d.addEventListener('close', function once() {
                d.removeEventListener('close', once);
                startRun(mode);
            });
            return;
        }
        $all('dialog[open]').forEach(function (d) { delete d.dataset.autoPaused; d.close(); });
        state.mode = mode;
        setInRun(true);
        if (mode === DAILY) {
            state.dailyDay = utcDay();
            call('start_daily', dailySeed(state.dailyDay));   // fires tornado:runstart
        } else {
            call('set_difficulty', settings.difficulty);
            call('start_run', mode);           // fires tornado:runstart
        }
        setPaused(false);
        resizeCanvas();
        canvas.focus({ preventScroll: true });
    }

    function backToMenu() {
        $all('dialog[open]').forEach(function (d) { delete d.dataset.autoPaused; d.close(); });
        call('enter_menu');
        setPaused(false);
        setInRun(false);
        state.run = null;
        releaseWakeLock();
        loadGlobalStats();
        renderDailyButton();
        $('#btn-play').focus({ preventScroll: true });
    }

    var lastRPress = 0;
    function restartPressed() {
        var p = phase();
        var now = Date.now();
        if (p === PHASE.VICTORY || p === PHASE.GAME_OVER || now - lastRPress < 1500) {
            lastRPress = 0;
            startRun(state.mode);
        } else {
            lastRPress = now;
            toast(t('Apasă R din nou pentru restart'), 1500);
        }
    }

    // Server run token, requested as soon as the game reports a new run.
    window.addEventListener('tornado:runstart', function (e) {
        var mode = e.detail.mode | 0, difficulty = e.detail.difficulty | 0;
        var run = { id: null, mode: mode, difficulty: difficulty, submitted: false, error: '' };
        var body = { mode: MODES[mode], difficulty: difficulty };
        if (mode === DAILY) body.day = run.day = state.dailyDay;
        run.promise = fetch(API + '/runs', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body),
            credentials: 'same-origin'
        }).then(function (r) {
            return r.json().catch(function () { return {}; }).then(function (j) {
                if (r.ok && j.run) run.id = j.run;
                else run.error = j.error || 'unavailable';
                return run;
            });
        }).catch(function () { return run; });
        state.run = run;
    });

    function readStats() {
        return {
            score: stat(STAT.SCORE), wave: stat(STAT.WAVE),
            houses: stat(STAT.HOUSES), trees: stat(STAT.TREES), props: stat(STAT.PROPS),
            animals: stat(STAT.ANIMALS), vehicles: stat(STAT.VEHICLES), birds: stat(STAT.BIRDS),
            maxCombo: stat(STAT.MAX_COMBO), time: stat(STAT.RUN_TIME),
            mode: stat(STAT.MODE), difficulty: stat(STAT.DIFFICULTY), endless: stat(STAT.ENDLESS),
            destroyed: stat(STAT.DESTROYED), tornado: stat(STAT.TORNADO_X10) / 10
        };
    }

    // Local history: top 20 across modes; returns true for a new best on this board.
    function saveLocal(st) {
        if (st.score <= 0) return false;
        var list = store.get(KEYS.scores, []);
        if (!Array.isArray(list)) list = [];
        var board = list.filter(function (x) {
            return (x.mode || 0) === st.mode && (x.difficulty == null ? 1 : x.difficulty) === st.difficulty &&
                   (st.mode !== DAILY || x.day === st.day);
        });
        var best = board.reduce(function (m, x) { return Math.max(m, x.score | 0); }, 0);
        var entry = { score: st.score, wave: st.wave, mode: st.mode, difficulty: st.difficulty,
                      date: new Date().toISOString().slice(0, 10) };
        if (st.mode === DAILY) entry.day = st.day;
        list.push(entry);
        list.sort(function (a, b) { return b.score - a.score; });
        store.set(KEYS.scores, list.slice(0, 20));
        return st.score > best;
    }

    // ── Profile: lifetime stats + achievements (this device only) ─────
    var COUNTERS = ['houses', 'trees', 'props', 'animals', 'vehicles', 'birds', 'destroyed'];
    function total(key, goal) { return function (s, p) { return p[key] >= goal; }; }
    function count(key, goal) { return function (p) { return [p[key], goal]; }; }
    var ACHIEVEMENTS = [
        { id: 'first', title: 'Prima furtună', desc: 'Termină prima rundă.', test: total('runs', 1) },
        { id: 'wave5', title: 'Uragan în formare', desc: 'Ajungi la valul 5 în modul clasic.',
          test: function (s) { return s.mode === 0 && s.wave >= 5; } },
        { id: 'victory', title: 'Forța naturii', desc: 'Termini toate cele 10 valuri.',
          test: function (s) { return s.mode === 0 && s.outcome === OUTCOME.VICTORY; } },
        { id: 'victory_hard', title: 'Apocalipsă', desc: 'Câștigi pe dificultatea Greu.',
          test: function (s) { return s.mode === 0 && s.outcome === OUTCOME.VICTORY && s.difficulty === 2; } },
        { id: 'endless15', title: 'Fără sfârșit', desc: 'Ajungi la valul 15 în modul infinit.',
          test: function (s) { return s.mode === 0 && s.wave >= 15; } },
        { id: 'combo10', title: 'Lanț de distrugeri', desc: 'Faci un combo de 10.',
          test: function (s) { return s.maxCombo >= 10; } },
        { id: 'combo25', title: 'Reacție în lanț', desc: 'Faci un combo de 25.',
          test: function (s) { return s.maxCombo >= 25; } },
        { id: 'ta10k', title: 'Contra cronometru', desc: '10.000 de puncte în Time Attack.',
          test: function (s) { return s.mode === 1 && s.score >= 10000; } },
        { id: 'ta25k', title: 'Fulger', desc: '25.000 de puncte în Time Attack.',
          test: function (s) { return s.mode === 1 && s.score >= 25000; } },
        { id: 'score50k', title: 'Record de furtună', desc: '50.000 de puncte într-o rundă clasică.',
          test: function (s) { return s.mode === 0 && s.score >= 50000; } },
        { id: 'houses100', title: 'Demolator', desc: '100 de case distruse în total.',
          test: total('houses', 100), progress: count('houses', 100) },
        { id: 'trees250', title: 'Defrișare', desc: '250 de copaci doborâți în total.',
          test: total('trees', 250), progress: count('trees', 250) },
        { id: 'cars50', title: 'Parcare goală', desc: '50 de mașini luate pe sus.',
          test: total('vehicles', 50), progress: count('vehicles', 50) },
        { id: 'animals50', title: 'Ferma zburătoare', desc: '50 de animale prinse în vârtej.',
          test: total('animals', 50), progress: count('animals', 50) },
        { id: 'birds25', title: 'Prin nori', desc: '25 de păsări prinse în zbor.',
          test: total('birds', 25), progress: count('birds', 25) },
        { id: 'destroy1k', title: 'O mie de bucăți', desc: '1.000 de obiecte distruse în total.',
          test: total('destroyed', 1000), progress: count('destroyed', 1000) },
        { id: 'destroy10k', title: 'Cataclism', desc: '10.000 de obiecte distruse în total.',
          test: total('destroyed', 10000), progress: count('destroyed', 10000) },
        { id: 'runs25', title: 'Vânător de furtuni', desc: '25 de runde jucate.',
          test: total('runs', 25), progress: count('runs', 25) },
        { id: 'streak3', title: 'Sezonul uraganelor', desc: 'Joci 3 zile la rând.',
          test: total('streak', 3), progress: count('streak', 3) },
        { id: 'daily1', title: 'Provocare acceptată', desc: 'Termini o provocare a zilei.',
          test: function (s) { return s.mode === DAILY; } },
        { id: 'daily7', title: 'Cronicarul furtunilor', desc: 'Joci provocarea zilei în 7 zile diferite.',
          test: total('dailyDays', 7), progress: count('dailyDays', 7) },
        { id: 'global', title: 'Pe hartă', desc: 'Trimiți un scor în clasamentul global.' },
        { id: 'daily_podium', title: 'Pe podium', desc: 'Intri în primii 3 la provocarea zilei.' },
        { id: 'top10', title: 'Top 10', desc: 'Intri în primii 10 ai unui clasament global.' }
    ];

    function localDay(d) {
        d = d || new Date();
        return d.getFullYear() + '-' + ('0' + (d.getMonth() + 1)).slice(-2) + '-' + ('0' + d.getDate()).slice(-2);
    }
    function dayNumber(iso) {
        var p = String(iso).split('-');
        return Math.round(Date.UTC(+p[0], +p[1] - 1, +p[2]) / 86400000);
    }
    function loadProfile() {
        var p = store.get(KEYS.profile, null);
        if (!p || typeof p !== 'object') p = {};
        var base = { runs: 0, playSeconds: 0, bestClassic: 0, bestTimeAttack: 0, bestWave: 0, bestCombo: 0,
                     victories: 0, streak: 0, firstDay: '', lastDay: '', unlocked: {},
                     bestDaily: 0, dailyDays: 0, lastDailyDay: '' };
        COUNTERS.forEach(function (k) { base[k] = 0; });
        Object.keys(base).forEach(function (k) {
            if (k === 'unlocked' ? typeof p[k] !== 'object' || !p[k]
                                 : typeof p[k] !== typeof base[k]) p[k] = base[k];
        });
        return p;
    }

    function unlock(p, id) {
        if (p.unlocked[id]) return false;
        p.unlocked[id] = localDay();
        return true;
    }

    // Adds a finished run to the profile and returns newly unlocked achievements.
    // After a victory the same run can continue in endless mode and end again,
    // so only what changed since the last report of this run is counted.
    function recordRun(st) {
        var p = loadProfile();
        var run = state.run;
        var prev = run && run.recorded;
        function delta(k) { return Math.max(0, (st[k] | 0) - (prev ? prev[k] | 0 : 0)); }
        if (!prev) p.runs++;
        p.playSeconds += delta('time');
        COUNTERS.forEach(function (k) { p[k] += delta(k); });
        if (st.mode === 0) {
            p.bestClassic = Math.max(p.bestClassic, st.score);
            p.bestWave = Math.max(p.bestWave, st.wave);
        } else if (st.mode === 1) {
            p.bestTimeAttack = Math.max(p.bestTimeAttack, st.score);
        } else if (st.mode === DAILY) {
            p.bestDaily = Math.max(p.bestDaily, st.score);
            if (p.lastDailyDay !== st.day) { p.dailyDays++; p.lastDailyDay = st.day; }
        }
        p.bestCombo = Math.max(p.bestCombo, st.maxCombo);
        if (st.outcome === OUTCOME.VICTORY) p.victories++;

        var today = localDay();
        if (p.lastDay !== today) {
            p.streak = p.lastDay && dayNumber(today) - dayNumber(p.lastDay) === 1 ? p.streak + 1 : 1;
            p.lastDay = today;
        }
        if (!p.firstDay) p.firstDay = today;
        if (run) run.recorded = st;

        var fresh = ACHIEVEMENTS.filter(function (a) { return a.test && a.test(st, p) && unlock(p, a.id); });
        store.set(KEYS.profile, p);
        return fresh;
    }

    // Achievements earned outside a run end (leaderboard results).
    function unlockLater(ids) {
        var p = loadProfile();
        var fresh = ACHIEVEMENTS.filter(function (a) { return ids.indexOf(a.id) >= 0 && unlock(p, a.id); });
        if (!fresh.length) return;
        store.set(KEYS.profile, p);
        showUnlocks(fresh, true);
    }

    function showUnlocks(list, append) {
        var box = $('#end-unlocks');
        if (!append) box.textContent = '';
        list.forEach(function (a) {
            var chip = document.createElement('span');
            chip.className = 'chip-gold';
            chip.title = t(a.desc);
            chip.appendChild(svgIcon('i-medal'));
            chip.appendChild(document.createTextNode(t(a.title)));
            box.appendChild(chip);
        });
        box.hidden = !box.childNodes.length;
        if (list.length && !$('#dlg-end').open) toast(t('Realizare deblocată: {title}', { title: t(list[0].title) }));
    }

    function svgIcon(id) {
        var ns = 'http://www.w3.org/2000/svg';
        var svg = document.createElementNS(ns, 'svg');
        svg.setAttribute('class', 'icon');
        svg.setAttribute('aria-hidden', 'true');
        var use = document.createElementNS(ns, 'use');
        use.setAttribute('href', '#' + id);
        svg.appendChild(use);
        return svg;
    }

    function fmtDuration(sec) {
        var h = Math.floor(sec / 3600), m = Math.floor(sec % 3600 / 60);
        return h ? h + ' h ' + m + ' min' : m + ' min';
    }

    function renderProfile() {
        var p = loadProfile();
        var name = playerName();
        $('#profile-since').textContent = !p.firstDay
            ? t('Încă nu ai terminat nicio rundă. Statisticile apar după prima.')
            : name ? t('Joci din {date} · în clasament ca {name}', { date: p.firstDay, name: name })
                   : t('Joci din {date}', { date: p.firstDay });
        var items = [
            ['Runde', p.runs], ['Timp de joc', fmtDuration(p.playSeconds)], ['Victorii', p.victories],
            ['Obiecte distruse', p.destroyed], ['Case', p.houses], ['Copaci', p.trees],
            ['Mașini', p.vehicles], ['Animale', p.animals], ['Păsări', p.birds],
            ['Record clasic', p.bestClassic], ['Record Time Attack', p.bestTimeAttack],
            ['Val maxim', p.bestWave], ['Combo maxim', '×' + p.bestCombo], ['Zile la rând', p.streak],
            ['Provocări zilnice', p.dailyDays], ['Record la provocare', p.bestDaily]
        ];
        var grid = $('#profile-stats');
        grid.textContent = '';
        items.forEach(function (it) {
            var box = document.createElement('div');
            var dt = document.createElement('dt'); dt.textContent = t(it[0]);
            var dd = document.createElement('dd'); dd.textContent = typeof it[1] === 'number' ? fmt(it[1]) : it[1];
            box.appendChild(dt); box.appendChild(dd); grid.appendChild(box);
        });

        var list = $('#ach-list');
        list.textContent = '';
        var done = 0;
        ACHIEVEMENTS.forEach(function (a) {
            var when = p.unlocked[a.id];
            if (when) done++;
            var li = document.createElement('li');
            li.className = 'ach' + (when ? ' done' : '');
            li.appendChild(svgIcon(when ? 'i-medal' : 'i-lock'));
            var b = document.createElement('b'); b.textContent = t(a.title); li.appendChild(b);
            var side = document.createElement('span'); side.className = 'prog';
            if (when) side.textContent = when;
            else if (a.progress) { var pr = a.progress(p); side.textContent = fmt(Math.min(pr[0], pr[1])) + '/' + fmt(pr[1]); }
            li.appendChild(side);
            var small = document.createElement('small'); small.textContent = t(a.desc); li.appendChild(small);
            li.setAttribute('aria-label', t(when ? '{title}: {desc} — deblocată' : '{title}: {desc} — blocată',
                                            { title: t(a.title), desc: t(a.desc) }));
            list.appendChild(li);
        });
        $('#ach-count').textContent = done + ' / ' + ACHIEVEMENTS.length;
    }

    function openProfile() {
        renderProfile();
        showDialog('dlg-profile');
    }

    window.addEventListener('tornado:runend', function (e) {
        var outcome = e.detail.outcome | 0;
        var st = readStats();
        st.outcome = outcome;
        if (st.mode === DAILY) st.day = (state.run && state.run.day) || state.dailyDay || utcDay();
        // The leaderboard opens next on the board this run belongs to.
        lb.mode = MODES[st.mode];
        if (st.mode === DAILY) lb.day = st.day === utcDay(1) ? '1' : '0';
        else lb.difficulty = String(st.difficulty);
        state.lastEnd = st;
        var record = saveLocal(st);
        var unlocked = recordRun(st);
        releaseWakeLock();
        renderEnd(st, record);
        showDialog('dlg-end');
        showUnlocks(unlocked, false);     // after opening: shown in the dialog, no toast

        var canContinue = outcome === OUTCOME.VICTORY && st.mode === 0;
        var name = playerName();
        if (st.score <= 0) {
            setSubmitUi('hidden');
        } else if (state.run && state.run.submitted) {
            setSubmitUi('done', t('Scorul acestei runde a fost deja trimis.'));
        } else if (name && !canContinue) {
            submitScore(name);            // known player: submit straight away
        } else {
            setSubmitUi('ready', t(canContinue
                ? 'Trimite acum sau continuă în modul infinit și trimite scorul final.'
                : 'Alege un nume ca să apari în clasamentul global.'));
        }
        if (!canContinue && !name) setTimeout(function () { $('#end-name').focus(); }, 50);
    });

    function renderEnd(st, record) {
        var dlg = $('#dlg-end');
        var title, sub;
        if (st.outcome === OUTCOME.TIME_UP) { title = t('Timp expirat'); sub = st.mode === DAILY ? '' : t('Time Attack · 3 minute'); }
        else if (st.outcome === OUTCOME.VICTORY) { title = t('Victorie!'); sub = t('Toate cele 10 valuri au fost complete'); }
        else {
            title = 'Game over';
            sub = t(st.endless ? 'Mod infinit · valul {wave}' : 'Tornada s-a risipit la valul {wave}', { wave: st.wave });
        }
        dlg.classList.toggle('victory', st.outcome !== OUTCOME.GAME_OVER);
        dlg.classList.toggle('gameover', st.outcome === OUTCOME.GAME_OVER);
        $('#end-title').textContent = title;
        $('#end-sub').textContent = (sub ? sub + ' · ' : '') + boardLabel(st.mode, st.difficulty, st.day);
        $('#end-score').textContent = fmt(st.score);
        $('#end-record').hidden = !record;

        var items = [
            ['Case', st.houses], ['Copaci', st.trees], ['Mașini', st.vehicles],
            ['Animale', st.animals], ['Păsări', st.birds], ['Altele', st.props],
            ['Combo max', '×' + st.maxCombo], ['Timp', fmtTime(st.time)]
        ];
        var grid = $('#end-stats');
        grid.textContent = '';
        items.forEach(function (it) {
            var box = document.createElement('div');
            var dt = document.createElement('dt'); dt.textContent = t(it[0]);
            var dd = document.createElement('dd'); dd.textContent = typeof it[1] === 'number' ? fmt(it[1]) : it[1];
            box.appendChild(dt); box.appendChild(dd); grid.appendChild(box);
        });
        $('#btn-endless').hidden = !(st.outcome === OUTCOME.VICTORY && st.mode === 0);
        $('#end-name').value = playerName();
    }

    function setSubmitUi(mode, msg, kind) {
        var form = $('#submit-form'), status = $('#submit-status'), btn = $('#btn-submit');
        form.hidden = mode === 'hidden';
        status.textContent = msg || '';
        status.className = 'hint' + (kind ? ' ' + kind : '');
        btn.disabled = mode === 'busy' || mode === 'done';
        $('#end-name').disabled = mode === 'busy' || mode === 'done';
        btn.textContent = t(mode === 'busy' ? 'Se trimite…' : (mode === 'done' ? 'Trimis' : 'Trimite'));
    }

    function submitScore(name) {
        var run = state.run, st = state.lastEnd;
        if (!st || st.score <= 0) return;
        if (name && !NAME_RE.test(name)) { setSubmitUi('ready', t('Doar litere, cifre, spațiu și . _ -'), 'err'); return; }
        if (!run) { setSubmitUi('ready', t('Clasamentul global nu este disponibil acum.'), 'err'); return; }
        setSubmitUi('busy', '');
        run.promise.then(function () {
            if (run.error === 'wrong day') throw { wrongDay: true };
            if (!run.id) throw { offline: true };
            return fetch(API + '/scores', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                credentials: 'same-origin',
                body: JSON.stringify({
                    run: run.id, name: name || 'ANONIM', player: playerKey(),
                    score: st.score, wave: st.wave,
                    stats: { houses: st.houses, trees: st.trees, props: st.props, animals: st.animals,
                             vehicles: st.vehicles, birds: st.birds, maxCombo: st.maxCombo, time: st.time }
                })
            });
        }).then(function (r) {
            return r.json().catch(function () { return {}; }).then(function (j) { return { status: r.status, body: j }; });
        }).then(function (res) {
            var err = (res.body && res.body.error) || '';
            if (res.status === 200) {
                run.submitted = true;
                if (name) setPlayerName(name);
                setSubmitUi('done', t('Trimis! Locul #{rank} în clasamentul {board}.',
                    { rank: res.body.rank, board: boardLabel(st.mode, st.difficulty, st.day) }), 'ok');
                scoresCache = {};
                var earned = ['global'];
                if (res.body.rank <= 10) earned.push('top10');
                if (st.mode === DAILY && res.body.rank <= 3) earned.push('daily_podium');
                unlockLater(earned);
            } else if (res.status === 409 && err === 'name taken') {
                setSubmitUi('ready', t('Numele este rezervat de alt jucător. Alege altul.'), 'err');
                $('#end-name').focus();
            } else if (res.status === 409) {
                run.submitted = true;
                setSubmitUi('done', t('Scorul acestei runde a fost deja trimis.'));
            } else if (res.status === 422 && err === 'name not allowed') {
                setSubmitUi('ready', t('Numele nu este permis. Alege altul.'), 'err');
                $('#end-name').focus();
            } else if (res.status === 422) {
                run.submitted = true;
                setSubmitUi('done', t('Scorul nu a putut fi validat de server.'), 'err');
            } else if (res.status === 403 && err === 'banned') {
                run.submitted = true;
                setSubmitUi('done', t('Acest nume sau dispozitiv a fost blocat în clasamentul global.'), 'err');
            } else if (res.status === 429) {
                setSubmitUi('ready', t('Prea multe încercări. Mai așteaptă puțin.'), 'err');
            } else if (res.status === 404) {
                setSubmitUi('done', t('Runda a expirat și nu mai poate fi trimisă.'), 'err');
            } else {
                setSubmitUi('ready', t('Serverul nu răspunde. Încearcă din nou.'), 'err');
            }
        }).catch(function (e) {
            if (e && e.wrongDay) {
                setSubmitUi('done', t('Ceasul dispozitivului nu se potrivește cu ziua provocării, așa că scorul nu poate fi trimis.'), 'err');
                return;
            }
            setSubmitUi(e && e.offline ? 'done' : 'ready',
                t(e && e.offline ? 'Ești offline: scorul a rămas salvat pe acest dispozitiv.'
                                 : 'Eroare de rețea. Încearcă din nou.'), 'err');
        });
    }

    $('#submit-form').addEventListener('submit', function (e) {
        e.preventDefault();
        submitScore(cleanNameInput($('#end-name')));
    });
    $('#end-name').addEventListener('input', function () { cleanNameInput(this); });
    $('#btn-again').addEventListener('click', function () { startRun(state.mode); });
    $('#btn-end-menu').addEventListener('click', backToMenu);
    $('#btn-endless').addEventListener('click', function () {
        if (call('continue_endless')) {
            delete $('#dlg-end').dataset.autoPaused;
            closeDialog('dlg-end');
            setPaused(false);
            if (state.run && state.run.submitted) toast(t('Scorul a fost deja trimis; runda infinită e doar pentru distracție.'), 3500);
            else toast(t('Mod infinit! Scorul final se trimite la sfârșit.'), 2500);
        }
    });
    $('#btn-share').addEventListener('click', share);

    function boardLabel(mode, diff, day) {
        if (mode === DAILY) return t(MODE_LABELS[mode]) + ' · ' + fmtDay(day || utcDay());
        return t(MODE_LABELS[mode]) + ' · ' + t(DIFF_LABELS[diff]);
    }

    function share() {
        var st = state.lastEnd;
        var score = st ? st.score : stat(STAT.SCORE);
        var mode = st ? st.mode : state.mode;
        var diff = st ? st.difficulty : settings.difficulty;
        var text = mode === DAILY
            ? t('Am făcut {score} puncte la provocarea zilei din Tornada 3D ({date}). Poți mai mult?',
                { score: fmt(score), date: fmtDay((st && st.day) || utcDay()) })
            : t('Am făcut {score} puncte în Tornada 3D ({mode}, {diff}). Poți mai mult?',
                { score: fmt(score), mode: t(MODE_LABELS[mode]), diff: t(DIFF_LABELS[diff]) });
        var url = location.origin + '/';
        if (navigator.share) {
            navigator.share({ title: t('Tornada 3D'), text: text, url: url }).catch(function () {});
        } else if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text + ' ' + url)
                .then(function () { toast(t('Copiat în clipboard')); })
                .catch(function () { toast(t('Nu am putut copia textul')); });
        }
    }

    // ── Leaderboard dialog ────────────────────────────────────────────
    var lb = { tab: 'global', mode: 'classic', difficulty: String(settings.difficulty), period: 'all', day: '0' };
    var scoresCache = {};

    function setSegmented(group, value) {
        $all('button', group).forEach(function (b) {
            b.setAttribute('aria-checked', String(b.dataset.value === String(value)));
            b.tabIndex = b.dataset.value === String(value) ? 0 : -1;
        });
    }
    // Arrow keys move within a radio group (WAI-ARIA radio pattern)
    $all('.segmented').forEach(function (group) {
        group.addEventListener('keydown', function (e) {
            if (['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].indexOf(e.key) < 0) return;
            var btns = $all('button', group);
            var i = btns.indexOf(document.activeElement);
            if (i < 0) return;
            e.preventDefault();
            var next = btns[(i + (e.key === 'ArrowRight' || e.key === 'ArrowDown' ? 1 : btns.length - 1)) % btns.length];
            next.focus();
            next.click();
        });
    });

    function renderRows(rows, highlight) {
        var body = $('#scores-body');
        body.textContent = '';
        rows.forEach(function (r, i) {
            var tr = document.createElement('tr');
            if (highlight && r.name && r.name.toLowerCase() === highlight.toLowerCase()) tr.className = 'me';
            var name = r.name === 'ANONIM' ? t('ANONIM') : (r.name || '—');   // server's placeholder name
            [r.rank || i + 1, name, fmt(r.score), r.wave, r.date || ''].forEach(function (v, c) {
                var td = document.createElement('td');
                td.textContent = v;
                if (c === 2) td.className = 'num';
                if (c === 3) td.className = 'num wave';
                if (c === 4) td.className = 'date';
                tr.appendChild(td);
            });
            body.appendChild(tr);
        });
    }

    function loadScores() {
        var status = $('#scores-status');
        var me = playerName();
        var daily = lb.mode === 'daily';
        var day = utcDay(+lb.day);
        $('#scores-panel').setAttribute('aria-labelledby', lb.tab === 'global' ? 'tab-global' : 'tab-local');
        $('[data-filter="period"]').hidden = lb.tab !== 'global' || daily;
        $('[data-filter="difficulty"]').hidden = daily;
        $('[data-filter="day"]').hidden = !daily;
        $('table.scores').classList.toggle('timed', lb.mode !== 'classic');
        if (lb.tab === 'local') {
            var list = store.get(KEYS.scores, []);
            if (!Array.isArray(list)) list = [];
            var rows = list.filter(function (x) {
                if (daily) return x.mode === DAILY && x.day === day;
                return MODES[x.mode || 0] === lb.mode && String(x.difficulty == null ? 1 : x.difficulty) === lb.difficulty;
            }).slice(0, 10).map(function (x, i) {
                return { rank: i + 1, name: me || t('Tu'), score: x.score, wave: x.wave, date: x.date };
            });
            renderRows(rows, null);
            status.textContent = rows.length ? '' : t('Niciun scor pe acest dispozitiv pentru filtrul ales.');
            return;
        }
        var key = daily ? 'daily|' + day : [lb.mode, lb.difficulty, lb.period].join('|');
        var url = daily ? API + '/scores?mode=daily&day=' + day + '&limit=25'
                        : API + '/scores?mode=' + lb.mode + '&difficulty=' + lb.difficulty + '&period=' + lb.period + '&limit=25';
        var cached = scoresCache[key];
        if (cached && Date.now() - cached.at < 15000) {
            renderRows(cached.rows, me);
            status.textContent = cached.rows.length ? '' : t('Încă nu există scoruri aici. Fii primul!');
            return;
        }
        renderRows([], null);
        status.textContent = t('Se încarcă…');
        fetch(url, { credentials: 'same-origin' })
            .then(function (r) { if (!r.ok) throw r.status; return r.json(); })
            .then(function (j) {
                var rows = (j && Array.isArray(j.scores)) ? j.scores : [];
                scoresCache[key] = { at: Date.now(), rows: rows };
                if (url !== currentUrl()) return;            // filters changed meanwhile
                renderRows(rows, me);
                status.textContent = rows.length ? '' : t('Încă nu există scoruri aici. Fii primul!');
            })
            .catch(function () { status.textContent = t('Clasamentul nu este disponibil momentan.'); });
    }

    function currentUrl() {
        return lb.mode === 'daily' ? API + '/scores?mode=daily&day=' + utcDay(+lb.day) + '&limit=25'
            : API + '/scores?mode=' + lb.mode + '&difficulty=' + lb.difficulty + '&period=' + lb.period + '&limit=25';
    }

    function openScores() {
        setSegmented($('[data-filter="mode"]'), lb.mode);
        setSegmented($('[data-filter="difficulty"]'), lb.difficulty);
        setSegmented($('[data-filter="period"]'), lb.period);
        setSegmented($('[data-filter="day"]'), lb.day);
        showDialog('dlg-scores');
        loadScores();
    }
    $all('[data-filter]').forEach(function (group) {
        group.addEventListener('click', function (e) {
            var b = e.target.closest('button');
            if (!b) return;
            lb[group.dataset.filter] = b.dataset.value;
            setSegmented(group, b.dataset.value);
            loadScores();
        });
    });
    [['#tab-global', 'global'], ['#tab-local', 'local']].forEach(function (t) {
        $(t[0]).addEventListener('click', function () {
            lb.tab = t[1];
            $('#tab-global').setAttribute('aria-selected', String(t[1] === 'global'));
            $('#tab-local').setAttribute('aria-selected', String(t[1] === 'local'));
            loadScores();
        });
    });

    function loadGlobalStats() {
        fetch(API + '/stats', { credentials: 'same-origin' })
            .then(function (r) { if (!r.ok) throw r.status; return r.json(); })
            .then(function (s) { if (s) { state.globalStats = s; renderGlobalStats(); } })
            .catch(function () { /* offline: leave it empty */ });
    }
    function renderGlobalStats() {
        var s = state.globalStats;
        if (!s) return;
        var el = $('#global-stats');
        if (!s.scores) { el.textContent = t('Clasamentul global e gol — primul scor trimis ia locul 1.'); return; }
        var parts = [];
        if (s.houses) parts.push(t('{n} case', { n: fmt(s.houses) }));
        if (s.vehicles) parts.push(t('{n} mașini', { n: fmt(s.vehicles) }));
        if (s.animals) parts.push(t('{n} animale', { n: fmt(s.animals) }));
        el.textContent = parts.length
            ? t('Jucătorii au distrus deja {list}. Record: {best} puncte.', { list: parts.join(', '), best: fmt(s.best) })
            : t('Record global: {best} puncte.', { best: fmt(s.best) });
    }

    // ── Settings dialog ───────────────────────────────────────────────
    function syncSettingsUi() {
        $all('[data-setting="quality"]').forEach(function (g) { setSegmented(g, settings.quality); });
        $all('[data-setting="difficulty"]').forEach(function (g) { setSegmented(g, settings.difficulty); });
        $all('[data-setting="lang"]').forEach(function (g) { setSegmented(g, settings.lang); });
        $('#set-volume').value = Math.round(settings.volume * 100);
        $('#volume-out').textContent = Math.round(settings.volume * 100) + '%';
        $('#set-sound').checked = !settings.muted;
        $('#set-haptics').checked = settings.haptics;
        $('#set-haptics').closest('.toggle').hidden = !navigator.vibrate;
        $('#set-motion').checked = settings.reducedMotion;
        $('#set-fps').checked = settings.fps;
        $('#set-name').value = playerName();
        $('#profile-code').value = playerKey();
    }
    $all('[data-setting]').forEach(function (group) {
        group.addEventListener('click', function (e) {
            var b = e.target.closest('button');
            if (!b) return;
            var key = group.dataset.setting;
            var v = key === 'lang' || b.dataset.value === 'auto' ? b.dataset.value : +b.dataset.value;
            if (settings[key] === v) return;
            settings[key] = v;
            saveSettings();
            if (key === 'lang') applyLanguage();
            if (key === 'quality') { state.autoQuality = null; applyQuality(); }
            if (key === 'difficulty') {
                call('set_difficulty', v);
                lb.difficulty = String(v);
                if (runActive() || state.paused) toast(t('Dificultatea se aplică de la următoarea rundă'));
            }
            syncSettingsUi();
        });
    });
    $('#set-volume').addEventListener('input', function () {
        settings.volume = clamp(this.value / 100, 0, 1);
        if (settings.volume > 0) settings.muted = false;
        applyAudio(); syncSettingsUi(); saveSettings();
    });
    $('#set-sound').addEventListener('change', function () { settings.muted = !this.checked; applyAudio(); saveSettings(); });
    $('#set-haptics').addEventListener('change', function () { settings.haptics = this.checked; saveSettings(); });
    $('#set-motion').addEventListener('change', function () {
        settings.reducedMotion = this.checked;
        call('set_reduced_motion', this.checked ? 1 : 0);
        saveSettings();
    });
    $('#set-fps').addEventListener('change', function () { settings.fps = this.checked; $('#fps').hidden = !this.checked; saveSettings(); });
    $('#set-name').addEventListener('input', function () { setPlayerName(cleanNameInput(this)); });
    $('#btn-copy-profile').addEventListener('click', function () {
        var code = playerKey();
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(code).then(function () { toast(t('Cod copiat')); }, function () { $('#profile-code').select(); });
        } else { $('#profile-code').select(); }
    });
    $('#btn-import-profile').addEventListener('click', function () {
        var v = $('#profile-import').value.trim();
        if (!/^[A-Za-z0-9_-]{22,64}$/.test(v)) { toast(t('Cod de profil invalid')); return; }
        store.set(KEYS.player, v);
        $('#profile-import').value = '';
        syncSettingsUi();
        toast(t('Profil importat'));
    });
    $('#btn-clear-data').addEventListener('click', function () {
        if (!confirm(t('Ștergi scorurile locale, statisticile, realizările și setările de pe acest dispozitiv? Codul de profil (numele rezervat) rămâne.'))) return;
        [KEYS.settings, KEYS.scores, KEYS.tutorial, KEYS.profile].forEach(store.remove);
        Object.keys(defaults).forEach(function (k) { settings[k] = defaults[k]; });
        applyAll();
        toast(t('Datele locale au fost șterse'));
    });
    $('#btn-lang').addEventListener('click', function () {
        settings.lang = lang === 'en' ? 'ro' : 'en';
        saveSettings();
        applyLanguage();
        syncSettingsUi();
    });
    function toggleMute() { settings.muted = !settings.muted; applyAudio(); saveSettings(); syncSettingsUi(); }

    // ── Buttons ───────────────────────────────────────────────────────
    $('#btn-play').addEventListener('click', function () { startRun(0); });
    $('#btn-timeattack').addEventListener('click', function () { startRun(1); });
    $('#btn-daily').addEventListener('click', function () { startRun(DAILY); });
    $('#btn-scores').addEventListener('click', openScores);
    $('#btn-profile').addEventListener('click', openProfile);
    $('#btn-hud-scores').addEventListener('click', openScores);
    $('#btn-help').addEventListener('click', function () { showDialog('dlg-help'); });
    $('#btn-settings').addEventListener('click', function () { syncSettingsUi(); showDialog('dlg-settings'); });
    $('#btn-hud-settings').addEventListener('click', function () { syncSettingsUi(); showDialog('dlg-settings'); });
    $('#btn-pause').addEventListener('click', togglePause);
    $('#btn-sound').addEventListener('click', toggleMute);
    $('#btn-fullscreen').addEventListener('click', toggleFullscreen);
    $('#btn-resume').addEventListener('click', resume);
    $('#btn-restart').addEventListener('click', function () { startRun(state.mode); });
    $('#btn-pause-settings').addEventListener('click', function () { syncSettingsUi(); showDialog('dlg-settings'); });
    $('#btn-pause-help').addEventListener('click', function () { showDialog('dlg-help'); });
    $('#btn-pause-menu').addEventListener('click', backToMenu);
    $('#btn-reload').addEventListener('click', function () { location.reload(); });

    // ── Touch controls ────────────────────────────────────────────────
    function initTouch() {
        if (!IS_TOUCH) return;
        document.body.classList.add('touch');
        var layer = $('#touch-layer');
        var base = $('#joy-base'), knob = $('#joy-knob'), dot = $('#look-dot');
        var R = 56;
        var move = null, look = null;
        function pos(t) { var r = layer.getBoundingClientRect(); return { x: t.clientX - r.left, y: t.clientY - r.top }; }
        function place(el, p) { el.style.left = p.x + 'px'; el.style.top = p.y + 'px'; el.style.display = 'block'; }
        function aim(p) {
            call('touch_set_cursor', (p.x / layer.clientWidth) * 2 - 1, -((p.y / layer.clientHeight) * 2 - 1));
        }
        layer.addEventListener('touchstart', function (e) {
            e.preventDefault();
            for (var i = 0; i < e.changedTouches.length; i++) {
                var t = e.changedTouches[i], p = pos(t);
                if (p.x < layer.clientWidth / 2 && move === null) {
                    move = { id: t.identifier, o: p };
                    place(base, p); place(knob, p);
                } else if (p.x >= layer.clientWidth / 2 && look === null) {
                    look = { id: t.identifier, last: p };
                    place(dot, p);
                    call('touch_set_look', 0, 0, 1);
                    aim(p);
                }
            }
        }, { passive: false });
        layer.addEventListener('touchmove', function (e) {
            e.preventDefault();
            for (var i = 0; i < e.changedTouches.length; i++) {
                var t = e.changedTouches[i], p = pos(t);
                if (move && t.identifier === move.id) {
                    var dx = p.x - move.o.x, dy = p.y - move.o.y;
                    var d = Math.sqrt(dx * dx + dy * dy), k = d > R ? R / d : 1;
                    call('touch_set_move', dx * k / R, -dy * k / R);
                    place(knob, { x: move.o.x + dx * k, y: move.o.y + dy * k });
                }
                if (look && t.identifier === look.id) {
                    call('touch_set_look', p.x - look.last.x, look.last.y - p.y, 1);
                    look.last = p;
                    place(dot, p);
                    aim(p);
                }
            }
        }, { passive: false });
        function end(e) {
            for (var i = 0; i < e.changedTouches.length; i++) {
                var t = e.changedTouches[i];
                if (move && t.identifier === move.id) {
                    move = null; call('touch_set_move', 0, 0);
                    base.style.display = knob.style.display = 'none';
                }
                if (look && t.identifier === look.id) {
                    look = null; call('touch_set_look', 0, 0, 0);
                    dot.style.display = 'none';
                }
            }
        }
        layer.addEventListener('touchend', end, { passive: false });
        layer.addEventListener('touchcancel', end, { passive: false });
        // iOS Safari pinch-zoom gestures on the game area
        document.addEventListener('gesturestart', function (e) { if (stage.contains(e.target)) e.preventDefault(); });
    }

    // ── Per-frame page loop: gamepad, FPS meter, auto quality ─────────
    var pad = { cursor: [0, 0], moving: false, looking: false, start: false, a: false };
    var perf = { frames: 0, t0: 0, low: 0, shown: 0 };
    function axis(v) { return Math.abs(v) < 0.15 ? 0 : v; }

    function pollGamepad(dt) {
        var pads = navigator.getGamepads ? navigator.getGamepads() : [];
        var gp = null;
        for (var i = 0; i < pads.length; i++) if (pads[i] && pads[i].connected) { gp = pads[i]; break; }
        if (!gp) return;
        var b = gp.buttons;
        var startDown = !!(b[9] && b[9].pressed);
        if (startDown && !pad.start && state.inRun) togglePause();
        pad.start = startDown;
        var aDown = !!(b[0] && b[0].pressed);
        if (aDown && !pad.a) {
            if (!state.inRun && state.ready && !openDialog()) startRun(0);
            else if ($('#dlg-end').open) startRun(state.mode);
        }
        pad.a = aDown;
        if (!runActive() || state.paused) return;

        var mx = axis(gp.axes[0] || 0), my = axis(gp.axes[1] || 0);
        if (mx || my) { call('touch_set_move', mx, -my); pad.moving = true; }
        else if (pad.moving) { call('touch_set_move', 0, 0); pad.moving = false; }

        var cx = axis(gp.axes[2] || 0), cy = axis(gp.axes[3] || 0);
        if (cx || cy) {
            pad.cursor[0] = clamp(pad.cursor[0] + cx * dt * 1.4, -1, 1);
            pad.cursor[1] = clamp(pad.cursor[1] - cy * dt * 1.4, -1, 1);
            call('touch_set_cursor', pad.cursor[0], pad.cursor[1]);
        }
        var turn = ((b[5] && b[5].pressed) ? 1 : 0) - ((b[4] && b[4].pressed) ? 1 : 0);
        if (turn) { call('touch_set_look', turn * 600 * dt, 0, 1); pad.looking = true; }
        else if (pad.looking) { call('touch_set_look', 0, 0, 0); pad.looking = false; }
    }

    function pageLoop(ts) {
        requestAnimationFrame(pageLoop);
        if (!state.ready) return;
        var dt = perf.last ? Math.min(0.1, (ts - perf.last) / 1000) : 0.016;
        perf.last = ts;
        if (state.gamepad) pollGamepad(dt);

        if (!perf.t0) perf.t0 = ts;
        perf.frames++;
        var span = ts - perf.t0;
        if (span >= 1000) {
            var fps = perf.frames * 1000 / span;
            perf.frames = 0; perf.t0 = ts;
            if (settings.fps) $('#fps').textContent = Math.round(fps) + ' FPS · ' + t(['Scăzută', 'Medie', 'Ridicată'][state.quality]);
            // Auto quality: step down after ~4 s of sustained low frame rate.
            if (settings.quality === 'auto' && phase() === PHASE.PLAYING && !state.paused && !document.hidden) {
                perf.low = fps < 28 ? perf.low + 1 : 0;
                if (perf.low >= 4 && state.quality > 0) {
                    perf.low = 0;
                    state.autoQuality = state.quality - 1;
                    applyQuality();
                    toast(t('Calitate redusă automat pentru un joc mai fluid'));
                }
            }
        }
    }

    window.addEventListener('gamepadconnected', function () {
        state.gamepad = true;
        toast(t('Gamepad conectat'));
    });

    // ── Loading the game ──────────────────────────────────────────────
    var build = { build: 'dev', js: 'tornado.js', wasm: 'tornado.wasm', data: 'tornado.data' };
    try { build = JSON.parse($('#build-info').textContent); } catch (e) { /* defaults */ }
    function renderBuildLabel() {
        if (build.build && build.build !== 'dev') $('#build-label').textContent = t('versiunea {v}', { v: build.build });
    }

    // title/text are Romanian keys; a raw browser message is passed as text.
    function showError(title, text) {
        $('#error-title').textContent = t(title);
        $('#error-text').textContent = t(text);
        $('#screen-error').hidden = false;
        $('#screen-start').hidden = true;
    }

    function setProgress(frac, text, vars) {
        $('#load-bar').style.width = Math.round(clamp(frac, 0.04, 1) * 100) + '%';
        if (text) { state.loadText = [text, vars]; $('#load-text').textContent = t(text, vars); }
    }

    // Applies the language setting to the page, the game and the parts of
    // the UI this file has already drawn.
    function applyLanguage() {
        var next = wantedLang();
        var changed = next !== lang;
        lang = next;
        NUM = new Intl.NumberFormat(lang === 'en' ? 'en-US' : 'ro-RO');
        root.lang = lang;
        document.title = t(PAGE_TITLE);
        translatePage();
        syncHudLabels();
        var other = lang === 'en' ? 'ro' : 'en';
        $('#btn-lang').textContent = other === 'en' ? 'English' : 'Română';
        $('#btn-lang').lang = other;
        call('set_language', lang === 'en' ? 1 : 0);
        if (!changed) return;
        renderGlobalStats();
        renderBuildLabel();
        renderDailyButton();
        if (state.loadText && !state.ready) $('#load-text').textContent = t(state.loadText[0], state.loadText[1]);
        if ($('#dlg-profile').open) renderProfile();
        if ($('#dlg-scores').open) loadScores();
    }

    function onReady() {
        state.ready = true;
        applyAll();
        $('#loader').classList.add('done');
        $('#btn-play').disabled = false;
        $('#btn-timeattack').disabled = false;
        $('#btn-daily').disabled = false;
        if (document.activeElement === document.body) $('#btn-play').focus({ preventScroll: true });
        resizeCanvas();
    }

    function supportsWebGL2() {
        try { return !!document.createElement('canvas').getContext('webgl2'); }
        catch (e) { return false; }
    }

    function boot() {
        applyLanguage();
        renderBuildLabel();
        renderDailyButton();
        setInterval(function () { if (!state.inRun) renderDailyButton(); }, 30000);
        syncSettingsUi();
        initTouch();
        loadGlobalStats();
        requestAnimationFrame(pageLoop);

        if (!supportsWebGL2()) {
            showError('Browserul nu suportă WebGL 2',
                'Tornada 3D are nevoie de WebGL 2. Folosește o versiune recentă de Chrome, Edge, Firefox sau Safari și verifică dacă accelerarea hardware este activată.');
            return;
        }
        resizeCanvas();
        window.Module = {
            canvas: canvas,
            locateFile: function (path) {
                if (/\.wasm$/.test(path)) return build.wasm;
                if (/\.data$/.test(path)) return build.data;
                return path;
            },
            print: function (t) { console.log(t); },
            printErr: function (t) { console.warn(t); },
            setStatus: function (text) {
                // "Downloading data... (12345/67890)"
                var m = /\((\d+(?:\.\d+)?)\/(\d+)\)/.exec(text || '');
                if (m) setProgress(0.2 + 0.7 * (+m[1] / +m[2]), 'Se descarcă… {p}%', { p: Math.round(100 * +m[1] / +m[2]) });
            },
            monitorRunDependencies: function (left) {
                if (left === 0) setProgress(0.95, 'Pregătire…');
            },
            postRun: [onReady],   // runs after main(): the game is waiting in its menu
            onAbort: function () {
                showError('Jocul s-a oprit neașteptat', 'A apărut o eroare internă. Reîncarcă pagina pentru a încerca din nou.');
            }
        };
        setProgress(0.08, 'Se descarcă…');
        var s = document.createElement('script');
        s.src = build.js;
        s.async = true;
        s.onerror = function () {
            showError('Jocul nu a putut fi descărcat', 'Verifică conexiunea la internet și reîncarcă pagina.');
        };
        document.body.appendChild(s);
    }

    window.addEventListener('error', function (e) {
        if (!state.ready && e && e.filename && /tornado/.test(e.filename)) {
            showError('Jocul nu a putut porni', String(e.message || 'Eroare necunoscută'));
        }
    });

    // ── Offline play + install ───────────────────────────────────────
    // Only deployed builds register the worker: in dev it would serve stale
    // files after every rebuild, so any leftover registration is removed.
    if ('serviceWorker' in navigator) {
        if (build.build !== 'dev') {
            var hadController = !!navigator.serviceWorker.controller;
            window.addEventListener('load', function () {
                // updateViaCache 'none': update checks always reach the server, even
                // where a CDN rewrites sw.js's Cache-Control into a long max-age.
                navigator.serviceWorker.register('sw.js', { updateViaCache: 'none' })
                    .catch(function () { /* offline mode is optional */ });
            });
            navigator.serviceWorker.addEventListener('controllerchange', function () {
                // A new deploy took over. Never reload mid-run; just say so.
                if (hadController) toast(t('Versiune nouă disponibilă — reîncarcă pagina când termini runda.'));
                hadController = true;
            });
        } else {
            navigator.serviceWorker.getRegistrations().then(function (regs) {
                regs.forEach(function (r) { r.unregister(); });
            }).catch(function () {});
        }
    }

    var installPrompt = null;
    var btnInstall = $('#btn-install');
    window.addEventListener('beforeinstallprompt', function (e) {
        e.preventDefault();              // show our own button instead of the mini-infobar
        installPrompt = e;
        btnInstall.hidden = false;
    });
    btnInstall.addEventListener('click', function () {
        if (!installPrompt) return;
        installPrompt.prompt();
        installPrompt.userChoice.then(function () {
            installPrompt = null;
            btnInstall.hidden = true;
        });
    });
    window.addEventListener('appinstalled', function () {
        installPrompt = null;
        btnInstall.hidden = true;
        toast(t('Tornada 3D a fost instalată'));
    });

    boot();
})();
