// Regenerates the gameplay screenshots in web/screenshots/ (used by the
// web app manifest for the richer install dialog, the JSON-LD and the
// README) from a packaged build:
//
//   python3 tools/package_site.py --version shots     # -> dist/
//   cd tests/e2e && node screenshots.mjs ../../dist [menu|gameplay]
//
// Plays a real run in headless Chromium (software WebGL, so it takes a few
// minutes) and captures only while a wave is being played, right after
// something was destroyed. The world is procedural and timing differs from
// run to run, so look at the results before committing them.
import { mkdirSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { repo, startSite, launchBrowser, waitForEngine } from './harness.mjs';

const site = path.resolve(process.argv[2] || path.join(repo, 'dist'));
const only = process.argv[3] || '';                       // regenerate just one kind
const OUT = path.join(repo, 'web', 'screenshots');
const QUALITY = 0.82;

// PNG -> WebP through a canvas on a blank page (no CSP there).
async function toWebp(browser, png) {
    const page = await browser.newPage();
    try {
        return Buffer.from(await page.evaluate(async ([b64, q]) => {
            const img = new Image();
            img.src = 'data:image/png;base64,' + b64;
            await img.decode();
            const c = document.createElement('canvas');
            c.width = img.naturalWidth; c.height = img.naturalHeight;
            c.getContext('2d').drawImage(img, 0, 0);
            return c.toDataURL('image/webp', q).split(',')[1];
        }, [png.toString('base64'), QUALITY]), 'base64');
    } finally {
        await page.close();
    }
}

async function save(browser, name, png) {
    const webp = await toWebp(browser, png);
    writeFileSync(path.join(OUT, name), webp);
    console.log(`  ${name}  ${Math.round(webp.length / 1024)} KiB`);
}

async function newPage(browser, base, opts) {
    const ctx = await browser.newContext({ locale: 'ro-RO', ...opts });
    await ctx.addInitScript(() => localStorage.setItem('tornado3d_seen_tutorial', '1'));
    const page = await ctx.newPage();
    await page.goto(base + '/');
    await waitForEngine(page);
    await page.waitForTimeout(1500);                       // let the attract scene settle
    // The temporary leaderboard is empty, which says nothing about the real one.
    await page.evaluate(() => { document.querySelector('#global-stats').textContent = ''; document.activeElement.blur(); });
    return page;
}

// Plays for `seconds`, steering in slow sweeps across the screen while
// driving forward, and returns the last frame captured while a wave was
// being played (phase 0) shortly after the score went up.
async function play(page, seconds, keyMove) {
    const { width, height } = page.viewportSize();
    await page.click('#btn-play');
    await page.waitForFunction(() => document.body.classList.contains('in-run'));
    await page.evaluate(() => { const h = document.querySelector('#rotate-hint'); if (h) h.hidden = true; });
    const stat = i => page.evaluate(k => Module._get_run_stat(k), i);
    let best = null, lastScore = 0;
    const t0 = Date.now();
    if (keyMove) await page.keyboard.down('KeyW');
    while (Date.now() - t0 < seconds * 1000) {
        const a = (Date.now() - t0) / 5000;
        await page.mouse.move(width * (0.5 + 0.2 * Math.cos(a)), height * (0.62 + 0.08 * Math.sin(2 * a)));
        await page.waitForTimeout(500);
        const score = await stat(0), phase = await stat(14);
        if (phase === 0 && score > lastScore && Date.now() - t0 > seconds * 400) {
            best = await page.screenshot();
        }
        lastScore = score;
    }
    if (keyMove) await page.keyboard.up('KeyW');
    return best || page.screenshot();
}

mkdirSync(OUT, { recursive: true });
const server = await startSite(site);
const browser = await launchBrowser();
try {
    console.log('desktop (wide)');
    let page = await newPage(browser, server.base, { viewport: { width: 1280, height: 720 } });
    if (only !== 'gameplay') await save(browser, 'menu-wide.webp', await page.screenshot());
    if (only !== 'menu') await save(browser, 'gameplay-wide.webp', await play(page, 150, true));
    await page.context().close();

    if (only !== 'gameplay') {
        console.log('phone portrait (narrow)');
        const phone = { viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true };
        page = await newPage(browser, server.base, phone);
        await save(browser, 'menu-narrow.webp', await page.screenshot());
        await page.context().close();
    }
} finally {
    await browser.close();
    server.stop();
}
