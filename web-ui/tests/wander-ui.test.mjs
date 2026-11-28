import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import test from 'node:test';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { serve } from '../scripts/serve.mjs';
import { pickOption } from './browser-helpers.mjs';
import { validator, readFixture } from './helpers.mjs';
import { generateRawPathShape } from '../src/viewport/path-shapes.js';
import { wanderEndEnvelope } from '../src/viewport/movement-path.js';
import { DEFAULT_WANDER, normalizeWander, isDefaultWander } from '../src/components/modulation-model.js';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';

const require = createRequire(import.meta.url);
let playwright;
for (const name of [process.env.PLAYWRIGHT_MODULE, 'playwright', 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'].filter(Boolean)) {
  try { playwright = require(name); break; } catch {}
}
const CHROME = process.env.CHROME_PATH || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SHOTS_DIR = process.env.VERB_V04B_SHOTS_DIR || path.resolve(__dirname, '../../out/V04b/shots');

test('V04b model & envelope math: normalizeWander, isDefaultWander, wanderEndEnvelope', () => {
  assert.equal(isDefaultWander(undefined), true);
  assert.equal(isDefaultWander(DEFAULT_WANDER), true);
  assert.equal(isDefaultWander({ ...DEFAULT_WANDER, enabled: true }), false);

  const clamped = normalizeWander({
    enabled: 1,
    ampHorizM: 99,
    ampVertM: -3,
    rateHz: 0.001,
    smoothness: 2,
    pinEnds: false,
    horizOnly: 1,
    seed: 42
  });
  assert.deepEqual(clamped, {
    enabled: true,
    ampHorizM: 20,
    ampVertM: 0,
    rateHz: 0.05,
    smoothness: 1,
    pinEnds: false,
    horizOnly: true,
    seed: 42
  });

  // Closed path or pinEnds=false -> envelope is 1 everywhere
  for (const u of [0, 0.05, 0.5, 0.95, 1]) {
    assert.equal(wanderEndEnvelope(u, true, true), 1);
    assert.equal(wanderEndEnvelope(u, false, false), 1);
  }
  // Open path with pinEnds=true tapers to 0 at u=0 and u=1 over 15% smoothstep
  assert.equal(wanderEndEnvelope(0, false, true), 0);
  assert.equal(wanderEndEnvelope(1, false, true), 0);
  assert.ok(Math.abs(wanderEndEnvelope(0.075, false, true) - 0.5) < 1e-9);
  assert.equal(wanderEndEnvelope(0.15, false, true), 1);
  assert.equal(wanderEndEnvelope(0.5, false, true), 1);
  assert.equal(wanderEndEnvelope(0.85, false, true), 1);
});

test('V04b mock bridge & schema validation: path.setWander, path.set/setFlags wander, undo/redo, default omission', async () => {
  const bridge = await createMockBridge({ read: readFixture, validator, fixture: 'default-physical' });
  const store = createStore(bridge);
  try {
    await store.start();

    assert.equal(store.state.document.paths?.[0]?.wander, undefined);
    const circle = generateRawPathShape({ kind: 'circle', cx: 3, cy: 4, z: 1.5, radiusX: 1.8, nodes: 16 });
    const ackSet = await store.command({
      op: 'path.set',
      object: 0,
      points: circle.points,
      enabled: true,
      closed: true,
      smooth: true,
      wander: {
        enabled: true,
        ampHorizM: 0.6,
        ampVertM: 0.3,
        rateHz: 0.8,
        smoothness: 0.85,
        pinEnds: true,
        horizOnly: false,
        seed: 123
      }
    });
    assert.equal(ackSet.kind, 'ack');
    assert.equal(store.state.document.paths[0].wander.enabled, true);
    assert.equal(store.state.document.paths[0].wander.ampHorizM, 0.6);
    assert.equal(store.state.document.paths[0].wander.seed, 123);

    const ackWander = await store.command({
      op: 'path.setWander',
      object: 0,
      wander: {
        enabled: true,
        ampHorizM: 0.9,
        ampVertM: 0.4,
        rateHz: 1.25,
        smoothness: 0.7,
        pinEnds: false,
        horizOnly: true,
        seed: 456
      }
    });
    assert.equal(ackWander.kind, 'ack');
    assert.equal(store.state.document.paths[0].wander.ampHorizM, 0.9);
    assert.equal(store.state.document.paths[0].wander.horizOnly, true);
    assert.equal(store.state.document.paths[0].wander.seed, 456);

    // Undo restores previous wander state in one step
    const ackUndo = await store.command({ op: 'history.undo' });
    assert.equal(ackUndo.kind, 'ack');
    assert.equal(store.state.document.paths[0].wander.ampHorizM, 0.6);
    assert.equal(store.state.document.paths[0].wander.seed, 123);

    // Resetting to default omits wander from document.paths[0]
    const ackDefault = await store.command({
      op: 'path.setWander',
      object: 0,
      wander: { ...DEFAULT_WANDER }
    });
    assert.equal(ackDefault.kind, 'ack');
    assert.equal('wander' in store.state.document.paths[0], false);
  } finally {
    store.dispose();
    bridge.dispose();
  }
});

test('V04b Browser UI: Path wander inspector, Reseed, tube in Plan & 3D, Pin ends on open path, compact 1100x560, Undo & screenshots', { timeout: 60000 }, async t => {
  if (!playwright) { t.skip('Playwright not available'); return; }
  await fs.mkdir(SHOTS_DIR, { recursive: true });
  const { server, url } = await serve();
  let browser;
  try {
    browser = await playwright.chromium.launch({ headless: true, executablePath: CHROME });
    const page = await browser.newPage({ viewport: { width: 1280, height: 720 }, reducedMotion: 'reduce' });
    page.setDefaultTimeout(5000);
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.goto(`${url}/?fixture=default-physical&dev=0`);
    await page.waitForFunction(() => window.verbRoom?.store?.ready);

    // Select Source 1 and apply a closed Circle shape (radius 1.8 m, centered on listener)
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'source', index: 0 }] }, false));
    await page.waitForSelector('.path-section');
    await pickOption(page, 'Path shape', 'Circle');
    await page.waitForSelector('.path-shape-panel[data-shape-kind="circle"]');
    await page.getByRole('button', { name: 'Centre shape on listener', exact: true }).click();
    const radiusInput = page.getByRole('textbox', { name: 'Shape radius, m', exact: true });
    await radiusInput.dblclick();
    await radiusInput.fill('1.8');
    await radiusInput.press('Enter');
    await page.getByRole('button', { name: 'Apply path shape', exact: true }).click();
    await page.waitForFunction(() => (window.verbRoom.store.state.document.paths?.[0]?.points?.length ?? 0) === 16);
    await page.getByRole('button', { name: 'Close shape generator', exact: true }).click();

    // Expand Path wander nested accordion on Main page and verify it is disabled in Physical mode
    const wanderAccHead = page.locator('[data-acc-id="main.path.wander"]');
    await wanderAccHead.scrollIntoViewIfNeeded();
    await wanderAccHead.click();
    await page.waitForSelector('.path-wander.is-physical');
    assert.equal(await page.getByRole('switch', { name: 'Enable path wander', exact: true }).isDisabled(), true);

    // Switch to Creative mode
    await page.getByRole('button', { name: 'Creative', exact: true }).click();
    await page.waitForSelector('.path-wander:not(.is-physical)');
    await page.locator('.path-wander').scrollIntoViewIfNeeded();
    await page.waitForTimeout(80);
    await page.screenshot({ path: path.join(SHOTS_DIR, 'wander-off-1280x720.png') });

    // Enable Path wander
    await page.getByRole('switch', { name: 'Enable path wander', exact: true }).click();
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths?.[0]?.wander?.enabled === true);
    await page.waitForSelector('.path-wander-bake-note');
    assert.equal(await page.locator('.path-wander-bake-note').textContent(), 'Path wander bypasses baked tables.');

    // Click Reseed and verify seed changes in 1 undoable command
    const seedBefore = await page.evaluate(() => window.verbRoom.store.state.document.paths[0].wander.seed);
    await page.getByRole('button', { name: 'Reseed path wander', exact: true }).click();
    await page.waitForFunction(prev => window.verbRoom.store.state.document.paths?.[0]?.wander?.seed !== prev, seedBefore);
    const seedAfter = await page.evaluate(() => window.verbRoom.store.state.document.paths[0].wander.seed);
    assert.notEqual(seedAfter, seedBefore);

    // Enable Endless (∞) as well and set Horiz amp = 0.50 m, Vert amp = 0.30 m, Rate = 0.75 Hz
    await page.evaluate(() => window.verbRoom.store.command({
      op: 'path.setFlags',
      object: 0,
      endless: true,
      endlessPeriodS: 8,
      wander: {
        enabled: true,
        ampHorizM: 0.5,
        ampVertM: 0.3,
        rateHz: 0.75,
        smoothness: 0.8,
        pinEnds: true,
        horizOnly: false,
        seed: 42
      }
    }));
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths?.[0]?.wander?.seed === 42);
    const readoutText = await page.locator('.path-wander-readout').textContent();
    assert.ok(readoutText.includes('±0.50 m H') && readoutText.includes('±0.30 m V') && readoutText.includes('corr 1.33 s'),
      `Unexpected wander readout: ${readoutText}`);

    // Capture Plan view with translucent emerald tube + Path wander inspector (1280x720)
    await page.locator('.path-wander').scrollIntoViewIfNeeded();
    await page.waitForTimeout(80);
    await page.screenshot({ path: path.join(SHOTS_DIR, 'wander-on-plan-tube-1280x720.png') });

    // Switch to 3D view and capture 3D tube (horizontal + vertical envelopes)
    await page.evaluate(() => window.verbRoom.store.setView({ projection: '3d' }));
    await page.locator('.path-wander').scrollIntoViewIfNeeded();
    await page.waitForTimeout(80);
    await page.screenshot({ path: path.join(SHOTS_DIR, 'wander-on-3d-tube-1280x720.png') });

    // Return to Plan view and capture compact 1100x560 viewport
    await page.evaluate(() => window.verbRoom.store.setView({ projection: 'plan' }));
    await page.setViewportSize({ width: 1100, height: 560 });
    await page.locator('.path-wander').scrollIntoViewIfNeeded();
    await page.waitForTimeout(80);
    await page.screenshot({ path: path.join(SHOTS_DIR, 'wander-compact-1100x560.png') });

    // Test open path with Pin ends ON (tapered tube at start and end)
    await page.setViewportSize({ width: 1280, height: 720 });
    await page.evaluate(() => window.verbRoom.store.command({
      op: 'path.set',
      object: 0,
      points: [[1.2, 1.8, 1.5], [2.4, 5.8, 1.5], [4.8, 2.2, 1.5], [4.8, 6.4, 1.5]],
      enabled: true,
      closed: false,
      smooth: true,
      wander: {
        enabled: true,
        ampHorizM: 0.55,
        ampVertM: 0.3,
        rateHz: 0.8,
        smoothness: 0.8,
        pinEnds: true,
        horizOnly: false,
        seed: 77
      }
    }));
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths?.[0]?.closed === false);
    await page.locator('.path-wander').scrollIntoViewIfNeeded();
    await page.waitForTimeout(80);
    await page.screenshot({ path: path.join(SHOTS_DIR, 'wander-open-pin-ends-1280x720.png') });

    // Toggle Horiz only checkbox -> Vert amp slider becomes disabled and readout shows 'Z off'
    await page.getByRole('switch', { name: 'Horizontal only path wander', exact: true }).click();
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths?.[0]?.wander?.horizOnly === true);
    assert.ok((await page.locator('.path-wander-readout').textContent()).includes('Z off'));

    // Single Undo reverts Horiz only
    await page.evaluate(() => window.verbRoom.store.command({ op: 'history.undo' }));
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths?.[0]?.wander?.horizOnly === false);

    assert.deepEqual(errors, []);
  } finally {
    await browser?.close();
    server.close();
  }
});
