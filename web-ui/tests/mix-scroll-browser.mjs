import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { createRequire } from 'node:module';
import { serve } from '../scripts/serve.mjs';

// Regression: in a low window the Mix tab must scroll instead of clipping its last controls.
const require = createRequire(import.meta.url);
const candidates = [process.env.PLAYWRIGHT_MODULE, 'playwright', 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'];
let playwright;
for (const candidate of candidates.filter(Boolean)) {
  try { playwright = require(candidate); break; } catch {}
}
assert.ok(playwright, 'Set PLAYWRIGHT_MODULE to a Playwright installation');
const executablePath = process.env.CHROME_PATH || [
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
].find(existsSync);
const { server, url } = await serve();
let browser;
try {
  browser = await playwright.chromium.launch({ headless: true, ...(executablePath ? { executablePath } : {}) });
  for (const [width, height] of [[1280, 560], [1024, 480], [1280, 900]]) {
    const page = await browser.newPage({ viewport: { width, height }, reducedMotion: 'reduce' });
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(`${url}/?fixture=large-hall#mix`);
    await page.waitForFunction(() => window.verbRoom?.store.ready && document.querySelector('.mix-columns'));
    const info = await page.evaluate(() => {
      const view = document.querySelector('.mix-scroll-view');
      view.scrollTop = view.scrollHeight;
      const box = view.getBoundingClientRect();
      const knobs = [...document.querySelectorAll('.mix-columns .knob')];
      const last = knobs.at(-1).getBoundingClientRect();
      return { scrolls: view.scrollHeight > view.clientHeight + 1, top: view.scrollTop, knobs: knobs.length, lastBottom: last.bottom, viewBottom: box.bottom, viewTop: box.top };
    });
    console.log(width, height, JSON.stringify(info));
    assert.ok(info.knobs >= 8, 'all Mix knobs rendered');
    assert.ok(info.lastBottom <= info.viewBottom + 1, `last knob reachable at ${width}x${height}`);
    assert.deepEqual(errors, []);
    await page.close();
  }
  console.log('mix scroll passed');
} finally {
  await browser?.close();
  server.close();
}
