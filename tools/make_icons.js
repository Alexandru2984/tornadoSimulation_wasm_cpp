#!/usr/bin/env node
/* Regenerates web/icons/*.png, web/favicon.ico and web/og-image.jpg from web/favicon.svg.
 * Optional tooling (the PNGs are committed); needs Playwright + Chromium:
 *   npm i -D playwright && npx playwright install chromium
 *   node tools/make_icons.js
 */
const path = require('path');
const fs = require('fs');
const { chromium } = require('playwright');

const WEB = path.join(__dirname, '..', 'web');
const svg = fs.readFileSync(path.join(WEB, 'favicon.svg'), 'utf8');
const strokes = svg.match(/<g[\s\S]*<\/g>/)[0];
const defs = svg.match(/<defs>[\s\S]*<\/defs>/)[0];

// Full-bleed square (maskable: artwork kept inside the central 60% safe zone)
function square(maskable) {
    const s = maskable ? 0.6 : 0.86, o = (64 - 64 * s) / 2;
    return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">${defs}
      <rect width="64" height="64" fill="url(#bg)"/>
      <g transform="translate(${o} ${o}) scale(${s})">${strokes}</g></svg>`;
}

const og = `<!doctype html><html><head><meta charset="utf-8"><style>
  body{margin:0;width:1200px;height:630px;font-family:system-ui,"Segoe UI",Ubuntu,sans-serif;color:#e9effc;
       background:radial-gradient(circle at 78% 30%,#24407a 0,#101a35 45%,#070b16 100%);overflow:hidden;position:relative}
  .rain{position:absolute;inset:0;background:repeating-linear-gradient(105deg,rgba(160,200,255,.07) 0 2px,transparent 2px 26px)}
  .funnel{position:absolute;right:70px;top:110px;width:330px;height:390px}
  .wrap{position:absolute;left:80px;top:96px;bottom:56px;width:700px;display:flex;flex-direction:column}
  h1{font-size:110px;line-height:1;margin:0 0 26px;white-space:nowrap;font-weight:800;letter-spacing:-3px;
     background:linear-gradient(180deg,#fff,#9fdcff);-webkit-background-clip:text;color:transparent}
  p{font-size:34px;line-height:1.35;margin:0 0 34px;color:#b9c7e6}
  .chips{display:flex;gap:14px}
  .chip{font-size:24px;font-weight:700;padding:10px 20px;border-radius:999px;border:2px solid rgba(150,190,255,.35);background:rgba(255,255,255,.06)}
  .url{margin-top:auto;font-size:26px;color:#5cc8ff;font-weight:700}
</style></head><body><div class="rain"></div>
  <svg class="funnel" viewBox="0 0 64 74">${defs}<g fill="none" stroke="url(#fg)" stroke-width="5.2" stroke-linecap="round" opacity=".95">
    <path d="M4 8h56M11 20h41M17 32h29M24 44h17M28 56h9M31 67h4"/></g></svg>
  <div class="wrap"><h1>Tornada 3D</h1>
    <p>Controlează o tornadă și distruge tot ce prinzi într-o lume 3D infinită.</p>
    <div class="chips"><span class="chip">10 valuri</span><span class="chip">Time Attack</span><span class="chip">Clasament global</span></div>
    <div class="url">tornado.micutu.com · gratuit, direct în browser</div></div></body></html>`;

(async () => {
    const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });
    const page = await browser.newPage();
    const shots = [
        ['icons/icon-192.png', 192, square(false)], ['icons/icon-512.png', 512, square(false)],
        ['icons/maskable-512.png', 512, square(true)], ['icons/apple-touch-icon.png', 180, square(false)],
    ];
    for (const [file, size, art] of shots) {
        await page.setViewportSize({ width: size, height: size });
        await page.setContent(`<html><body style="margin:0">${art.replace('<svg ', `<svg width="${size}" height="${size}" `)}</body></html>`);
        await page.screenshot({ path: path.join(WEB, file), clip: { x: 0, y: 0, width: size, height: size } });
    }
    // favicon.ico for crawlers and old browsers that ignore <link rel=icon>:
    // an ICO container holding one 32x32 PNG (valid since Windows Vista).
    await page.setViewportSize({ width: 32, height: 32 });
    await page.setContent(`<html><body style="margin:0">${svg.replace('<svg ', '<svg width="32" height="32" ')}</body></html>`);
    const png = await page.screenshot({ clip: { x: 0, y: 0, width: 32, height: 32 }, omitBackground: true });
    const ico = Buffer.alloc(22);
    ico.writeUInt16LE(0, 0); ico.writeUInt16LE(1, 2); ico.writeUInt16LE(1, 4);   // ICONDIR: type 1, 1 image
    ico.writeUInt8(32, 6); ico.writeUInt8(32, 7);                                   // 32x32, no palette
    ico.writeUInt16LE(1, 10); ico.writeUInt16LE(32, 12);                            // planes, bpp
    ico.writeUInt32LE(png.length, 14); ico.writeUInt32LE(22, 18);                   // size, offset
    fs.writeFileSync(path.join(WEB, 'favicon.ico'), Buffer.concat([ico, png]));

    await page.setViewportSize({ width: 1200, height: 630 });
    await page.setContent(og);
    // JPEG: the rain texture makes a PNG ~500 KB; crawlers want small previews.
    await page.screenshot({ path: path.join(WEB, 'og-image.jpg'), type: 'jpeg', quality: 85 });
    await browser.close();
    console.log('icons + og-image written to web/');
})().catch(e => { console.error(e); process.exit(1); });
