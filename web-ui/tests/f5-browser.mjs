import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { mkdir } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { resolve } from 'node:path';
import { serve } from '../scripts/serve.mjs';
import { pickOption } from './browser-helpers.mjs';

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
const shots = resolve(process.argv[2] || 'shots');
await mkdir(shots, { recursive: true });
const { server, url } = await serve();
let browser;
try {
  browser = await playwright.chromium.launch({ headless: true, ...(executablePath ? { executablePath } : {}) });
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 }, reducedMotion: 'reduce' });
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  const open = async fixture => {
    await page.goto(`${url}/?fixture=${fixture}#sources`);
    await page.waitForFunction(() => window.verbRoom?.store.ready && document.querySelector('.sources-panel'));
  };
  const capture = async name => {
    const clip = await page.locator('.bottom-sources').boundingBox();
    assert.ok(clip);
    await page.screenshot({ clip, path: resolve(shots, `${name}.png`) });
  };
  const assertLayout = async () => {
    const overflow = await page.evaluate(() => {
      const elements = [...document.querySelectorAll('.sources-panel, .source-actions, .mix-scroll, .mix-row')];
      return elements.filter(node => node.scrollWidth > node.clientWidth + 1).map(node => node.className);
    });
    assert.deepEqual(overflow, []);
  };
  const first = page.getByRole('button', { name: 'Source 1 Sum', exact: true });
  const second = page.getByRole('button', { name: 'Source 2 Sum', exact: true });
  const all = page.getByRole('button', { name: 'Sum for all sources', exact: true });
  const formats = page.getByRole('combobox', { name: 'All formats…', exact: true });
  const inspector = page.getByRole('switch', { name: 'Sum', exact: true });
  const free = page.getByRole('switch', { name: 'Free pts', exact: true });
  const waitAttr = (locator, name, value) => locator.and(page.locator(`[${name}="${value}"]`)).waitFor();
  const sourceEdits = () => page.evaluate(() => window.verbRoom.bridge.audit
    .filter(entry => entry.direction === 'out' && entry.message.payload.op === 'source.setConfig')
    .map(entry => entry.message.payload));
  const undo = () => page.evaluate(() => window.verbRoom.store.command({ op: 'history.undo' }));

  await open('legacy-creative');
  assert.ok(await first.isDisabled() && await all.isDisabled());
  await first.evaluate(node => node.click());
  assert.deepEqual(await sourceEdits(), []);
  await capture('few-mono');
  await page.evaluate(() => window.verbRoom.bridge.setHost({ inputChannels: 4 }));
  await formats.focus();
  await formats.press('Enter');
  await formats.press('ArrowDown');
  await formats.press('Enter');
  await page.waitForFunction(() => window.verbRoom.store.state.document.sources.every(s => s.format === 1));
  await waitAttr(all, 'aria-pressed', 'false');
  await page.getByRole('button', { name: 'Select source 1', exact: true }).click();
  await inspector.waitFor();
  await free.click();
  await page.waitForFunction(() => window.verbRoom.store.state.document.sources[0].freePoints);
  await capture('few-sum-off');
  await first.focus();
  await first.press('Space');
  await waitAttr(first, 'aria-pressed', 'true');
  assert.equal(await first.evaluate(node => node === document.activeElement), true);
  assert.deepEqual((await sourceEdits()).at(-1), { op: 'source.setConfig', index: 0, sum: true });
  assert.equal(await inspector.getAttribute('aria-checked'), 'true');
  assert.equal(await free.getAttribute('aria-checked'), 'true');
  assert.ok(await free.isDisabled());
  await waitAttr(all, 'aria-pressed', 'mixed');
  await capture('few-mixed');
  await page.evaluate(() => window.verbRoom.bridge.automate('s1g', -3));
  assert.equal(await first.getAttribute('aria-pressed'), 'true');
  await inspector.click();
  await waitAttr(first, 'aria-pressed', 'false');
  assert.ok(await free.isEnabled());
  await undo();
  await waitAttr(first, 'aria-pressed', 'true');
  await page.getByRole('button', { name: 'Select source 1', exact: true }).click();
  assert.equal(await inspector.getAttribute('aria-checked'), 'true');
  console.log('PASS keyboard format and row Sum, mono silence, telemetry, Inspector sync and undo');

  await all.focus();
  await all.press('Enter');
  await waitAttr(all, 'aria-pressed', 'true');
  await waitAttr(second, 'aria-pressed', 'true');
  await page.waitForFunction(() => window.verbRoom.store.telemetry['history.changed']?.undoLabel === 'Sum all on');
  await capture('few-sum-on');
  await undo();
  await waitAttr(all, 'aria-pressed', 'mixed');
  await assertLayout();
  console.log('PASS mixed Sum and one-step bulk undo');

  await page.setViewportSize({ width: 900, height: 600 });
  await open('large-hall');
  const last = page.getByRole('button', { name: 'Source 32 Sum', exact: true });
  await last.waitFor();
  await page.evaluate(() => window.verbRoom.store.command({ op: 'source.setConfig', index: 31, name: 'W'.repeat(128) }));
  await assertLayout();
  assert.equal(await page.locator('.mix-scroll').evaluate(node => node.scrollHeight > node.clientHeight), true);
  await capture('32-narrow-top');
  await page.locator('.mix-scroll').evaluate(node => { node.scrollTop = node.scrollHeight; });
  await capture('32-narrow-bottom');
  await pickOption(page, 'All formats…', 'Quad');
  await page.getByRole('status').filter({ hasText: 'sources did not fit (unchanged)' }).waitFor();
  assert.equal(await page.evaluate(() => window.verbRoom.store.state.document.sources.some(s => s.format === 3)), false);
  await assertLayout();
  await page.locator('.mix-scroll').evaluate(node => { node.scrollTop = 0; });
  await capture('32-narrow-overflow-warning');
  await page.evaluate(() => {
    const { bridge } = window.verbRoom;
    const send = bridge.send;
    bridge.send = message => {
      if (message.transactionId && message.payload.op === 'source.setConfig' && message.payload.index === 1) {
        bridge.send = send;
        return send({ ...message, payload: { ...message.payload, format: 1, firstIn: 63 } });
      }
      return send(message);
    };
  });
  await pickOption(page, 'All formats…', 'Mono');
  await page.getByRole('status').filter({ hasText: 'All formats cancelled' }).waitFor();
  assert.equal(await page.evaluate(() => window.verbRoom.store.state.document.sources.every((s, i) =>
    s.format === 1 && s.firstIn === i * 2)), true);
  await page.evaluate(() => window.verbRoom.store.clearError());
  await capture('32-narrow-rollback-warning');
  await pickOption(page, 'All formats…', 'Mono');
  await page.waitForFunction(() => window.verbRoom.store.state.document.sources.every((s, i) => s.format === 0 && s.firstIn === i));
  assert.ok(await last.isDisabled() && await all.isDisabled());
  assert.deepEqual(errors, []);
  console.log('PASS 32 sources, narrow rack, maximum-length source name, scrolling, overflow warning and renumbering');
} finally {
  await browser?.close();
  await new Promise(resolve => server.close(resolve));
}
