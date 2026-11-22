import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { validator, readFixture } from './helpers.mjs';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';
import { pathBakeView, bakeFooterText } from '../src/components/path-bake.js';
import { serve } from '../scripts/serve.mjs';

const require = createRequire(import.meta.url);

test('E17 record actions are typed non-document worker commands and mock supports recorded-pass bake & outdated re-record', async t => {
  const bridge = await createMockBridge({ read: readFixture, validator, delayMs: 0, bakeDurationMs: 40 });
  const store = createStore(bridge);
  t.after(() => { store.dispose(); bridge.dispose(); });
  await store.start();

  // Blocked without paths or recorded pass offers record a pass cleanly in ASCII.
  await store.command({ op: 'path.bake', object: 'all' });
  const emptyBlocked = store.telemetry['status.load'].pathBake;
  assert.equal(emptyBlocked.state, 'blocked');
  assert.match(emptyBlocked.reason, /record a pass/i);

  // Also offered when uncoupled listener + source are blocked.
  await store.command({ op: 'path.set', object: 0, points: [[1, 2, 1.5], [4, 2, 1.5]], enabled: true });
  await store.command({ op: 'path.set', object: 32, points: [[2, 3, 1.5], [3, 3, 1.5]], enabled: true });
  await store.command({ op: 'path.bake', object: 'all' });
  const uncoupledBlocked = store.telemetry['status.load'].pathBake;
  assert.equal(uncoupledBlocked.state, 'blocked');
  assert.match(uncoupledBlocked.reason, /record a pass/i);
  assert.match(uncoupledBlocked.reason, /^[\x20-\x7E]+$/);

  const revision = store.state.revision;
  await store.command({ op: 'path.recordPass' });
  assert.equal(store.telemetry['status.load'].pathBake.passState, 1);
  assert.equal(store.state.revision, revision);

  await store.command({ op: 'path.recordStop' });
  assert.equal(store.telemetry['status.load'].pathBake.passState, 3);
  assert.ok(store.telemetry['status.load'].pathBake.passSamples >= 2);
  assert.equal(store.state.revision, revision);

  // Baking uncoupled listener+source now succeeds via Recorded pass.
  await store.command({ op: 'path.bake', object: 'all' });
  await new Promise(r => setTimeout(r, 70));
  const baked = store.telemetry['status.load'].pathBake;
  assert.equal(baked.state, 'baked');
  assert.equal(baked.name, 'Recorded pass');

  // Re-recording outdates the baked table.
  await store.command({ op: 'path.recordPass' });
  assert.equal(store.telemetry['status.load'].pathBake.state, 'outdated');
  assert.equal(store.telemetry['status.load'].pathBake.passState, 1);
});

test('E17 off-pass and outdated warnings stay visible in pathBakeView and bakeFooterText', () => {
  const off = { state: 'baked', object: -1, name: 'Recorded pass', reason: 'Off recorded pass - motion estimator' };
  assert.match(pathBakeView(off, 0).text, /Off recorded pass/);
  assert.match(bakeFooterText(off), /Off recorded pass/);
  assert.equal(pathBakeView({ state: 'outdated', object: -1 }, 0).warn, true);
});

test('E17 inspector Record pass, Bake path, and outdated footer Bake again work without MovementPaths in browser UI', async () => {
  const pw = require(process.env.PLAYWRIGHT_MODULE || 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package');
  const { server, url } = await serve({ port: 0, bakeDurationMs: 40 });
  const browser = await pw.chromium.launch({ headless: true, executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe' });
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
    await page.goto(url);
    await page.waitForFunction(() => window.verbRoom?.store?.ready);

    // Select Source 1 with NO MovementPath (DAW automation workflow).
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'source', index: 0 }] }));
    await page.waitForSelector('[data-record-pass="0"]');
    await page.waitForSelector('[data-bake-path="0"]');
    const record = page.locator('[data-record-pass="0"]');
    const bake = page.locator('[data-bake-path="0"]');
    const setting = page.locator('[data-orientation-mode="0"]');
    assert.match(await record.getAttribute('title'), /DAW automation.*press Stop, then press Bake path.*automation changes/i);
    assert.match(await bake.getAttribute('title'), /1–2 minutes.*drawn paths.*recording after Record pass/i);
    assert.equal(await page.locator('[data-clear-bake="0"]').getAttribute('title'),
      'Discard all baked tables (paths and recorded pass stay); bake again when needed');
    assert.match(await setting.getAttribute('title'), /Manual: the Azimuth control.*Baked tails use the angles captured/i);

    await page.locator('[data-record-pass="0"]').click();
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.passState === 1);
    assert.match(await page.locator('[data-record-pass="0"]').textContent(), /Armed/);
    assert.match(await record.getAttribute('title'), /starts when the host plays.*Stop to cancel/i);

    await page.locator('[data-record-pass="0"]').click();
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.passState === 3);
    assert.match(await record.getAttribute('title'), /16 samples ready.*Bake path.*Record again/i);
    assert.match(await bake.getAttribute('title'), /Uses the recorded pass/);

    await page.locator('[data-bake-path="0"]').click();
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.state === 'baked');
    assert.match(await page.locator('.path-bake-status').textContent(), /Baked/);

    // Verify off-pass banner stays visible in inspector and footer.
    await page.evaluate(() => window.verbRoom.bridge.setBakeStatus({
      state: 'baked', object: -1, name: 'Recorded pass', percent: 100,
      reason: 'Off recorded pass - motion estimator', passState: 3, passSamples: 20, paths: []
    }));
    await page.waitForSelector('.footer-bake:not([hidden])');
    assert.match(await page.locator('.footer-bake-text').textContent(), /Off recorded pass/);
    assert.match(await page.locator('.path-bake-status').textContent(), /Off recorded pass/);

    // Re-record triggers outdated warning and footer Bake again button.
    await page.locator('[data-record-pass="0"]').click();
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.state === 'outdated');
    await page.waitForSelector('.footer-bake.warn:not([hidden])');
    await page.waitForSelector('.footer-bake-again:not([hidden])');
    assert.equal(await page.locator('[data-bake-path="0"]').evaluate(e => e.classList.contains('warn')), true);
    for (const [state, title] of [[2, /being recorded.*Stop when the movement ends/], [4, /42 samples ready.*Bake path.*Record again/]]) {
      await page.evaluate(passState => window.verbRoom.bridge.setBakeStatus({
        state: 'notBaked', object: -1, passState, passSamples: 42, paths: []
      }), state);
      await page.waitForFunction(passState => window.verbRoom.store.telemetry['status.load']?.pathBake?.passState === passState, state);
      assert.match(await record.getAttribute('title'), title);
    }
  } finally {
    await browser.close();
    server.close();
  }
});
