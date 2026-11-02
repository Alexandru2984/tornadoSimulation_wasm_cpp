// Browser smoke test: boots the packaged site under the production CSP and
// walks the main UI paths on a desktop and a phone viewport, in Romanian
// and in English.
//
//   python3 tools/package_site.py --version ci        # -> dist/
//   cd tests/e2e && npm ci && npx playwright install chromium
//   node smoke.mjs ../../dist
//
// Starts its own leaderboard (temp DB) and tools/dev_server.py, so nothing
// else needs to run. CHROME=/path/to/chrome uses an existing browser.
// Fails on any console error/warning, page error or failed request.
import path from 'node:path';
import { repo, startSite, launchBrowser, waitForEngine } from './harness.mjs';

const site = path.resolve(process.argv[2] || path.join(repo, 'dist'));
let BASE;

const failures = [];
const check = (ok, what) => { console.log(`${ok ? '  ok ' : ' FAIL'}  ${what}`); if (!ok) failures.push(what); };

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
    const ctx = await browser.newContext({ viewport: { width: 1366, height: 768 }, locale: 'ro-RO' });
    const page = await ctx.newPage();
    const problems = watch(page);
    const runs = [];
    page.on('response', r => { if (r.url().endsWith('/api/v2/runs')) runs.push(r.status()); });

    await page.goto(BASE + '/');
    await waitForEngine(page);
    check(true, 'engine loaded, play button enabled');
    const stats = await page.waitForFunction(() => document.querySelector('#global-stats').textContent.trim().length > 0,
        null, { timeout: 10000 }).then(() => true, () => false);
    check(stats, 'global stats line filled from the API');
    const cdp = await ctx.newCDPSession(page);
    const manifest = await cdp.send('Page.getAppManifest');
    check(manifest.url.endsWith('/manifest.webmanifest') && manifest.errors.length === 0,
        'Chrome parses the web app manifest without errors' +
        (manifest.errors.length ? ': ' + manifest.errors.map(e => e.message).join('; ') : ''));

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
    await page.waitForTimeout(1500);
    const title = await page.title();
    check(/^Tornada 3D — /.test(title) && !/FPS/.test(title), 'the game leaves the page title alone during a run');

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
    const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true, locale: 'ro-RO' });
    const page = await ctx.newPage();
    const problems = watch(page);
    await page.goto(BASE + '/');
    await waitForEngine(page);
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth);
    check(!overflow, 'no horizontal overflow');
    const playBox = await page.locator('#btn-play').boundingBox();
    check(playBox && playBox.y + playBox.height <= 844 && playBox.height >= 44, 'play button visible above the fold, >=44px tall');
    check(problems.length === 0, 'phone: no console errors, page errors or failed requests' +
        (problems.length ? '\n        ' + problems.join('\n        ') : ''));
    await ctx.close();
}

// Romanian text left on the page (diacritics are a reliable tell), outside
// elements that are Romanian on purpose (lang="ro", e.g. the language switch).
function romanianLeftovers() {
    const out = [];
    const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
    for (let n = walker.nextNode(); n; n = walker.nextNode()) {
        const p = n.parentElement;
        if (!p || p.closest('script, style, noscript, [lang="ro"]')) continue;
        if (/[ăâîșțĂÂÎȘȚ]/.test(n.nodeValue)) out.push(n.nodeValue.trim().slice(0, 60));
    }
    document.querySelectorAll('[aria-label], [title], [placeholder]').forEach(el => {
        if (el.closest('[lang="ro"]')) return;
        ['aria-label', 'title', 'placeholder'].forEach(a => {
            const v = el.getAttribute(a);
            if (v && /[ăâîșțĂÂÎȘȚ]/.test(v)) out.push(`${a}="${v}"`);
        });
    });
    return out;
}

async function english(browser) {
    console.log('english (browser locale en-US)');
    const ctx = await browser.newContext({ viewport: { width: 1366, height: 768 }, locale: 'en-US' });
    const page = await ctx.newPage();
    const problems = watch(page);
    await page.goto(BASE + '/');
    await waitForEngine(page);
    await page.waitForFunction(() => document.querySelector('#global-stats').textContent.trim().length > 0,
        null, { timeout: 10000 }).catch(() => {});
    check(await page.evaluate(() => document.documentElement.lang) === 'en', 'English browser gets <html lang="en">');
    check((await page.textContent('#btn-play')).trim() === 'Play' && /^Tornado 3D/.test(await page.title()),
        'page text and title are in English');
    const left = await page.evaluate(romanianLeftovers);
    check(left.length === 0, 'no Romanian text left on the page' + (left.length ? ': ' + left.slice(0, 5).join(' | ') : ''));
    check(await page.evaluate(() => typeof Module._set_language === 'function'), 'game exports set_language for the HUD');

    await page.click('#btn-settings');
    await page.click('[data-setting="lang"] [data-value="ro"]');
    check((await page.textContent('#btn-play')).trim() === 'Joacă' &&
          await page.evaluate(() => document.documentElement.lang) === 'ro' &&
          /^Tornada 3D/.test(await page.title()), 'switching to Română restores the original page');
    await page.click('[data-setting="lang"] [data-value="en"]');
    await page.keyboard.press('Escape');
    await page.reload();
    await waitForEngine(page);
    check((await page.textContent('#btn-play')).trim() === 'Play', 'language choice survives a reload');
    await page.click('#btn-profile');
    await page.waitForSelector('#dlg-profile[open]', { timeout: 5000 });
    check(/Achievements/.test(await page.textContent('#dlg-profile')) &&
          (await page.evaluate(romanianLeftovers)).length === 0, 'profile dialog renders in English');
    await page.keyboard.press('Escape');

    check(problems.length === 0, 'english: no console errors, page errors or failed requests' +
        (problems.length ? '\n        ' + problems.join('\n        ') : ''));
    await ctx.close();
}

async function notFound(browser) {
    const page = await browser.newPage();
    const res = await page.goto(BASE + '/does/not/exist');
    check(res.status() === 404 && /404/.test(await page.textContent('body')), 'unknown path -> 404 page with 404 status');
    await page.close();
}

let server;
try {
    server = await startSite(site);
    BASE = server.base;
    const browser = await launchBrowser();
    try {
        await desktop(browser);
        await phone(browser);
        await english(browser);
        await notFound(browser);
    } finally {
        await browser.close();
    }
} catch (e) {
    failures.push(String(e && e.stack || e));
    console.error(e);
} finally {
    if (server) server.stop();
}

console.log(failures.length ? `\n${failures.length} check(s) failed` : '\nall smoke checks passed');
process.exit(failures.length ? 1 : 0);
