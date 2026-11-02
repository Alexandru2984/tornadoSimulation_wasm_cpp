// Shared by smoke.mjs and screenshots.mjs: serves a packaged site the way
// production does (tools/dev_server.py applies the nginx security headers,
// CSP included) with its own leaderboard on a temporary database, and
// launches Chromium with software WebGL (SwiftShader) so no GPU is needed.
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

async function waitFor(url) {
    for (let i = 0; i < 100; i++) {
        try { if ((await fetch(url)).ok) return; } catch { /* not up yet */ }
        await new Promise(r => setTimeout(r, 100));
    }
    throw new Error(`timeout waiting for ${url}`);
}

// -> { base, stop() }
export async function startSite(site) {
    const webPort = 18180 + Math.floor(Math.random() * 500);
    const apiPort = webPort + 1000;
    const base = `http://localhost:${webPort}`;
    const tmp = mkdtempSync(path.join(tmpdir(), 'tornado-e2e-'));
    const procs = [];
    const start = (args, env) => {
        const p = spawn('python3', args, { cwd: repo, env: { ...process.env, ...env }, stdio: ['ignore', 'ignore', 'pipe'] });
        let err = ''; p.stderr.on('data', d => { err += d; });
        p.on('exit', code => { if (code && code !== 143 && code !== null) console.error(`${args[0]} exited ${code}\n${err.slice(-2000)}`); });
        procs.push(p);
    };
    const stop = () => { procs.forEach(p => p.kill()); rmSync(tmp, { recursive: true, force: true }); };
    try {
        start(['server/leaderboard.py'], {
            TORNADO_DB: path.join(tmp, 'scores.db'), TORNADO_PORT: String(apiPort), TORNADO_ORIGINS: base,
        });
        start(['tools/dev_server.py', site, '--port', String(webPort), '--api', `http://127.0.0.1:${apiPort}`]);
        await waitFor(`http://127.0.0.1:${apiPort}/api/v2/health`);
        await waitFor(base + '/');
    } catch (e) {
        stop();
        throw e;
    }
    return { base, stop };
}

// CHROME=/path/to/chrome uses an existing browser instead of Playwright's.
export function launchBrowser() {
    return chromium.launch({
        executablePath: process.env.CHROME || undefined,
        args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'],
    });
}

export function waitForEngine(page) {
    return page.waitForFunction(() => !document.querySelector('#btn-play').disabled, null, { timeout: 120000 });
}
