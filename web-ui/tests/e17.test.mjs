import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { validator, readFixture } from './helpers.mjs';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';
import { pathBakeView, bakeFooterText } from '../src/components/path-bake.js';
import { serve } from '../scripts/serve.mjs';

const require = createRequire(import.meta.url);
const offPassReason = 'Live motion is outside the baked recording - estimating the tail';
const keptReason = 'Recording kept, not used: drawn paths are baked instead. Press Discard if you do not need it.';

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

  bridge.setBakeStatus({ passState: 2, passSamples: 16 });
  await store.command({ op: 'path.recordStop' });
  assert.equal(store.telemetry['status.load'].pathBake.passState, 3);
  assert.ok(store.telemetry['status.load'].pathBake.passSamples >= 2);
  assert.equal(store.telemetry['status.load'].pathBake.bakeSource, 'recorded');
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
  await store.command({ op: 'path.recordClear' });
  assert.equal(store.telemetry['status.load'].pathBake.passState, 0);
  assert.equal(store.telemetry['status.load'].pathBake.bakeSource, 'none');
  assert.equal(store.state.revision, revision);
  await store.command({ op: 'path.recordPass' });
  await store.command({ op: 'path.recordStop' });
  assert.equal(store.telemetry['status.load'].pathBake.passState, 0);
});

test('E17 off-pass and outdated warnings stay visible in pathBakeView and bakeFooterText', () => {
  const off = { state: 'baked', object: -1, name: 'Recorded pass', reason: offPassReason };
  assert.ok(pathBakeView(off, 0).text.includes(offPassReason));
  assert.ok(bakeFooterText(off).includes(offPassReason));
  assert.equal(pathBakeView({ state: 'outdated', object: -1 }, 0).warn, true);
});

test('F9 inspector separates host automation and baking, with truthful source and warnings', async () => {
  const pw = require(process.env.PLAYWRIGHT_MODULE || 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package');
  const { server, url } = await serve({ port: 0, bakeDurationMs: 350 });
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
    const clear = page.locator('[data-clear-recording="0"]');
    const source = page.locator('.path-bake-source');
    const status = page.locator('.path-bake-status');
    assert.deepEqual(await page.locator('.path-bake-caption').allTextContents(), ['Host automation', 'Bake late tail']);
    assert.equal(await record.textContent(), 'Record automation');
    assert.equal(await clear.isDisabled(), true);
    assert.equal(await source.textContent(), 'Bake source: nothing to bake');
    const setting = page.locator('[data-orientation-mode="0"]');
    assert.match(await record.getAttribute('title'), /host automation.*Record automation.*press Stop, then press Bake path/i);
    assert.match(await bake.getAttribute('title'), /1–2 minutes.*drawn paths in one group, press Bake path only/i);
    assert.equal(await page.locator('[data-clear-bake="0"]').getAttribute('title'),
      'Discard all baked tables. Drawn paths and the recording stay.');
    assert.match(await setting.getAttribute('title'), /Manual: the Azimuth control.*Baked tails use the angles captured/i);

    await page.locator('[data-record-pass="0"]').click();
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.passState === 1);
    assert.equal(await record.textContent(), 'Armed - Stop');
    assert.equal(await clear.isEnabled(), true);
    assert.match(await record.getAttribute('title'), /starts when the host plays.*Stop to cancel/i);

    await page.evaluate(() => window.verbRoom.bridge.setBakeStatus({ passState: 2, passSamples: 16 }));
    assert.equal(await record.textContent(), 'Recording 16 - Stop');
    assert.equal(await clear.isEnabled(), true);
    await record.click();
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.passState === 3);
    assert.match(await record.getAttribute('title'), /16 samples ready.*Bake path.*Record again/i);
    assert.equal(await record.textContent(), 'Recorded 16 - Record again');
    assert.equal(await source.textContent(), 'Bake source: recorded automation (16 samples)');
    assert.match(await status.textContent(), /Recorded - now press Bake path to compute the tail/);
    assert.equal(await status.evaluate(e => e.classList.contains('warn')), true);

    await bake.click();
    await page.waitForFunction(() => document.querySelector('[data-bake-path="0"]').textContent === 'Cancel');
    await bake.click();
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.state === 'cancelled');
    await bake.click();
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.state === 'baked');
    assert.equal(await status.textContent(), 'Baked');

    // Verify off-pass banner stays visible in inspector and footer.
    await page.evaluate(reason => window.verbRoom.bridge.setBakeStatus({
      state: 'baked', object: -1, name: 'Recorded pass', percent: 100,
      reason, passState: 3, passSamples: 20, paths: []
    }), offPassReason);
    await page.waitForSelector('.footer-bake:not([hidden])');
    assert.ok((await page.locator('.footer-bake-text').textContent()).includes(offPassReason));
    assert.ok((await status.textContent()).includes(offPassReason));

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
    await page.evaluate(async () => {
      await window.verbRoom.store.command({ op: 'path.set', object: 0, points: [[1,1,1.5],[3,1,1.5]], enabled: true });
    });
    assert.equal(await source.textContent(), 'Bake source: drawn paths (independent)');
    assert.equal(await page.locator('.path-recording-note').textContent(), keptReason);
    assert.doesNotMatch(await status.textContent(), /now press|outside the baked recording/);
    await page.evaluate(() => window.verbRoom.store.command({ op: 'path.setFlags', object: 0, group: 0 }));
    assert.equal(await source.textContent(), 'Bake source: drawn paths (Group 1)');
    assert.ok(await page.locator('.path-bake-row').first().locator('[data-clear-recording="0"]').count());
    assert.ok(await page.locator('.path-bake-row').last().locator('[data-clear-bake="0"]').count());
    await bake.click();
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.state === 'baked');
    await page.locator('[data-clear-bake="0"]').click();
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.state === 'notBaked');
    assert.equal(await clear.isEnabled(), true);
    await bake.click();
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.state === 'baked');
    await clear.click();
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.passState === 0);
    assert.equal(await status.textContent(), 'Baked');
    assert.equal(await clear.isDisabled(), true);
    assert.equal(await page.locator('.path-recording-note').isHidden(), true);

    // A disabled selected path should bake the other eligible paths shown by Bake source.
    await page.evaluate(async () => {
      const { store } = window.verbRoom;
      await store.command({ op: 'path.setFlags', object: 0, enabled: false });
      await store.command({ op: 'path.set', object: 32, points: [[1,2,1.5],[3,2,1.5]], enabled: true });
    });
    assert.equal(await source.textContent(), 'Bake source: drawn paths (independent)');
    await bake.click();
    await page.waitForFunction(() => window.verbRoom.bridge.audit.findLast(({ direction, message }) =>
      direction === 'out' && message.payload?.op === 'path.bake')?.message.payload.object === 'all');
    await page.waitForFunction(() => window.verbRoom.store.telemetry['status.load']?.pathBake?.state === 'baked');
    assert.equal(await status.textContent(), 'Baked');
  } finally {
    await browser.close();
    server.close();
  }
});
