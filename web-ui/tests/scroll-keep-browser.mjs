import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { createRequire } from 'node:module';
import { serve } from '../scripts/serve.mjs';

// Regression: with a long Sources list scrolled to the bottom, editing the last sources must not
// scroll the list back to the top (the whole UI re-renders after every command).
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
  const page = await browser.newPage({ viewport: { width: 900, height: 600 }, reducedMotion: 'reduce' });
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(`${url}/?fixture=large-hall#sources`);
  await page.waitForFunction(() => window.verbRoom?.store.ready && document.querySelector('.sources-panel'));
  const list = page.locator('.mix-scroll');
  assert.equal(await list.evaluate(node => node.scrollHeight > node.clientHeight), true);
  const scrollToBottom = () => list.evaluate(node => { node.scrollTop = node.scrollHeight; return node.scrollTop; });
  const top = () => list.evaluate(node => node.scrollTop);
  // after a re-render the list may legitimately be clamped when its content got shorter, but never jump to the top
  const expectKept = async (before, label) => {
    const state = await list.evaluate(node => ({ top: node.scrollTop, max: node.scrollHeight - node.clientHeight }));
    console.log(label, JSON.stringify({ before, ...state }));
    assert.ok(Math.abs(state.top - Math.min(before, state.max)) <= 2, label);
  };
  const settle = () => page.waitForTimeout(400);

  // 1. Σ button of the last source (selects nothing, only edits the config)
  let before = await scrollToBottom();
  assert.ok(before > 100, 'the list scrolls');
  await page.getByRole('button', { name: 'Source 32 Sum', exact: true }).click();
  await settle();
  await expectKept(before, 'Σ of the last source keeps the scroll position');

  // 2. a command for one of the last sources (name edit)
  before = await scrollToBottom();
  await page.evaluate(() => window.verbRoom.store.command({ op: 'source.setConfig', index: 30, name: 'Edited last but one' }));
  await settle();
  await expectKept(before, 'a config edit keeps the scroll position');

  // 3. selecting a source in the list re-renders the inspector as well
  before = await scrollToBottom();
  await page.getByRole('button', { name: 'Select source 31', exact: true }).evaluate(node => node.click()); // no Playwright auto-scroll
  await settle();
  await expectKept(before, 'selecting a source keeps the scroll position');

  // 4. gain of the last source through the host-parameter path
  before = await scrollToBottom();
  await page.evaluate(() => window.verbRoom.bridge.automate?.('s32g', -6));
  await settle();
  await expectKept(before, 'a parameter change keeps the scroll position');
  assert.deepEqual(errors, []);
  console.log('PASS the Sources list keeps its scroll position after edits');
} finally {
  await browser?.close();
  server.close();
}
