// Browser smoke test: boots the packaged site under the production CSP and
// walks the main UI paths on a desktop and a phone viewport.
//
//   python3 tools/package_site.py --version ci        # -> dist/
//   cd tests/e2e && npm ci && npx playwright install chromium
//   node smoke.mjs ../../dist
//
// Starts its own leaderboard (temp DB) and tools/dev_server.py, so nothing
// else needs to run. CHROME=/path/to/chrome uses an existing browser.
// Fails on any console error/warning, page error or failed request.
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..', '..');
const site = path.resolve(process.argv[2] || path.join(repo, 'dist'));
const WEB_PORT = 18180 + Math.floor(Math.random() * 500);
const API_PORT = WEB_PORT + 1000;
const BASE = `http://localhost:${WEB_PORT}`;

const failures = [];
const check = (ok, what) => { console.log(`${ok ? '  ok ' : ' FAIL'}  ${what}`); if (!ok) failures.push(what); };

const tmp = mkdtempSync(path.join(tmpdir(), 'tornado-e2e-'));
const procs = [];
function start(args, env) {
    const p = spawn('python3', args, { cwd: repo, env: { ...process.env, ...env }, stdio: ['ignore', 'ignore', 'pipe'] });
    let err = ''; p.stderr.on('data', d => { err += d; });
    p.on('exit', code => { if (code && code !== 143 && code !== null) console.error(`${args[0]} exited ${code}\n${err.slice(-2000)}`); });
    procs.push(p);
}
async function waitFor(url) {
    for (let i = 0; i < 100; i++) {
        try { if ((await fetch(url)).ok) return; } catch { /* not up yet */ }
        await new Promise(r => setTimeout(r, 100));
    }
    throw new Error(`timeout waiting for ${url}`);
}

function watch(page) {
    const problems = [];
    page.on('console', m => { if (m.type() === 'error' || m.type() === 'warning') problems.push(`console.${m.type()}: ${m.text()}`); });
    page.on('pageerror', e => problems.push(`pageerror: ${e.message}`));
    page.on('requestfailed', r => problems.push(`request failed: ${r.url()} ${(r.failure() || {}).errorText}`));
    page.on('response', r => { if (r.status() >= 400) problems.push(`HTTP ${r.status()}: ${r.url()}`); });
    return problems;
}

async function desktop(browser) {
    console.log('desktop 1366x768');
    const ctx = await browser.newContext({ viewport: { width: 1366, height: 768 } });
    const page = await ctx.newPage();
    const problems = watch(page);
    const runs = [];
    page.on('response', r => { if (r.url().endsWith('/api/v2/runs')) runs.push(r.status()); });

    await page.goto(BASE + '/');
    await page.waitForFunction(() => !document.querySelector('#btn-play').disabled, null, { timeout: 120000 });
    check(true, 'engine loaded, play button enabled');
    const stats = await page.waitForFunction(() => document.querySelector('#global-stats').textContent.trim().length > 0,
        null, { timeout: 10000 }).then(() => true, () => false);
    check(stats, 'global stats line filled from the API');

    await page.click('#btn-play');
    check(await page.isVisible('#dlg-help'), 'first run opens the help dialog');
    await page.click('#dlg-help .btn-primary');
    await page.waitForFunction(() => document.body.classList.contains('in-run'), null, { timeout: 15000 });
    check(true, 'run started (body.in-run)');
    await page.waitForTimeout(500);
    check(runs.includes(201), 'server issued a run token (POST /api/v2/runs 201)');

    await page.keyboard.press('KeyP');
    await page.waitForSelector('#dlg-pause[open]', { timeout: 5000 });
    check(true, 'P opens the pause dialog');
    await page.click('#btn-resume');

    await page.evaluate(() => window.dispatchEvent(new CustomEvent('tornado:runend', { detail: { outcome: 1 } })));
    await page.waitForSelector('#dlg-end[open]', { timeout: 5000 });
    check(true, 'run end shows the results dialog');
    const chips = await page.$$eval('#end-unlocks .chip-gold', els => els.map(e => e.textContent));
    check(chips.includes('Prima furtună'), 'first finished run unlocks an achievement in the results dialog');
    await page.click('#btn-end-menu');

    await page.click('#btn-profile');
    await page.waitForSelector('#dlg-profile[open]', { timeout: 5000 });
    check(/^1 \/ \d+$/.test((await page.textContent('#ach-count')).trim()), 'profile lists 1 unlocked achievement');
    await page.keyboard.press('Escape');

    await page.click('#btn-scores');
    await page.waitForSelector('#dlg-scores[open]', { timeout: 5000 });
    await page.waitForTimeout(500);
    check(true, 'leaderboard dialog opens');
    await page.keyboard.press('Escape');

    check(problems.length === 0, 'desktop: no console errors, page errors or failed requests' +
        (problems.length ? '\n        ' + problems.join('\n        ') : ''));
    await ctx.close();
}

async function phone(browser) {
    console.log('phone 390x844 (touch)');
    const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true });
    const page = await ctx.newPage();
    const problems = watch(page);
    await page.goto(BASE + '/');
    await page.waitForFunction(() => !document.querySelector('#btn-play').disabled, null, { timeout: 120000 });
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth);
    check(!overflow, 'no horizontal overflow');
    const playBox = await page.locator('#btn-play').boundingBox();
    check(playBox && playBox.y + playBox.height <= 844 && playBox.height >= 44, 'play button visible above the fold, >=44px tall');
    check(problems.length === 0, 'phone: no console errors, page errors or failed requests' +
        (problems.length ? '\n        ' + problems.join('\n        ') : ''));
    await ctx.close();
}

async function notFound(browser) {
    const page = await browser.newPage();
    const res = await page.goto(BASE + '/does/not/exist');
    check(res.status() === 404 && /404/.test(await page.textContent('body')), 'unknown path -> 404 page with 404 status');
    await page.close();
}

try {
    start(['server/leaderboard.py'], {
        TORNADO_DB: path.join(tmp, 'scores.db'), TORNADO_PORT: String(API_PORT), TORNADO_ORIGINS: BASE,
    });
    start(['tools/dev_server.py', site, '--port', String(WEB_PORT), '--api', `http://127.0.0.1:${API_PORT}`]);
    await waitFor(`http://127.0.0.1:${API_PORT}/api/v2/health`);
    await waitFor(BASE + '/');

    const browser = await chromium.launch({
        executablePath: process.env.CHROME || undefined,
        args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'],
    });
    try {
        await desktop(browser);
        await phone(browser);
        await notFound(browser);
    } finally {
        await browser.close();
    }
} catch (e) {
    failures.push(String(e && e.stack || e));
    console.error(e);
} finally {
    procs.forEach(p => p.kill());
    rmSync(tmp, { recursive: true, force: true });
}

console.log(failures.length ? `\n${failures.length} check(s) failed` : '\nall smoke checks passed');
process.exit(failures.length ? 1 : 0);
