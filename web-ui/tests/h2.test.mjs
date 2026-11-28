import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { existsSync } from 'node:fs';
import { mkdir } from 'node:fs/promises';
import { join } from 'node:path';
import { serve } from '../scripts/serve.mjs';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';
import { readFixture, validator } from './helpers.mjs';
import {
  positionedDryWarning,
  POSITIONED_DRY_MULTICHANNEL_WARNING,
  SOURCES_OUTPUT_HINT
} from '../src/state/selectors.js';

const require = createRequire(import.meta.url);

test('H2 positionedDryWarning helper stays silent in mono/stereo and fires in multichannel/Ambisonics when Positioned dry is off', async () => {
  const fixture = await readFixture('snapshot.default-physical');
  const base = JSON.parse(JSON.stringify(fixture.messages.find(m => m.kind === 'snapshot').payload));

  // Default stereo: silent whether Positioned dry is ON or OFF
  assert.equal(positionedDryWarning(base), '');
  const stereoOff = structuredClone(base);
  stereoOff.parameters.values.spatialDry = 0;
  assert.equal(positionedDryWarning(stereoOff), '');

  // Stereo XY & Mono & 2ch Ring with spatialDry off: still silent
  for (const layout of [0, 1, 2, 8]) {
    const s = structuredClone(stereoOff);
    s.document.output.layout = layout;
    s.host.effectiveLayout = layout;
    s.host.renderedChannels = layout === 0 ? 1 : 2;
    assert.equal(positionedDryWarning(s), '', `layout ${layout} at <=2ch should not warn`);
  }

  // Surround / LCR / Quad / 5.1 / 7.1 / 7.1.4 / AmbiX 1..3 / Ring (>2ch): warn only when spatialDry is OFF
  for (const [layout, hostOut, rendered] of [
    [3, 4, 3], [4, 4, 4], [5, 6, 6], [6, 8, 8], [7, 12, 12],
    [8, 16, 16], [8, 64, 16], [9, 4, 4], [10, 10, 9], [11, 16, 16], [11, 64, 16],
    [9, 2, 2]
  ]) {
    const on = structuredClone(base);
    on.document.output.layout = layout;
    on.host.effectiveLayout = layout;
    on.host.outputChannels = hostOut;
    on.host.renderedChannels = rendered;
    assert.equal(positionedDryWarning(on), '', `layout ${layout} (${rendered}/${hostOut}) must not warn when spatialDry ON`);

    const off = structuredClone(on);
    off.parameters.values.spatialDry = 0;
    assert.equal(positionedDryWarning(off), POSITIONED_DRY_MULTICHANNEL_WARNING,
      `layout ${layout} (${rendered}/${hostOut}) must warn when spatialDry OFF`);
  }
});

test('H2 mock bridge & schema accept 64 host outputChannels while capping renderedChannels at 16', async t => {
  const bridge = await createMockBridge({ read: readFixture, validator, fixture: 'large-hall', delayMs: 0 });
  const store = createStore(bridge);
  t.after(() => { store.dispose(); bridge.dispose(); });
  await store.start();

  bridge.setHost({ inputChannels: 64, outputChannels: 64 });
  assert.equal(store.state.host.outputChannels, 64);
  assert.equal(store.state.host.renderedChannels, 2);

  await store.command({ op: 'output.set', layout: 11 }); // AmbiX 3rd order
  assert.equal(store.state.host.outputChannels, 64);
  assert.equal(store.state.host.renderedChannels, 16);

  await store.command({ op: 'output.set', layout: -1 }); // Match track -> Ring capped at 16 rendered
  assert.equal(store.state.host.outputChannels, 64);
  assert.equal(store.state.host.renderedChannels, 16);

  bridge.audit.filter(e => ['in', 'out'].includes(e.direction)).forEach(e => validator.assert(e.message));
});

test('H2 web UI: Sources hint always visible, Positioned dry warning reactive in AmbiX/surround only, Rendered / host 16/64 ch', async () => {
  const candidates = [process.env.PLAYWRIGHT_MODULE, 'playwright', 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'].filter(Boolean);
  let pw;
  for (const c of candidates) { try { pw = require(c); break; } catch {} }
  assert.ok(pw, 'Playwright available');
  const executablePath = process.env.CHROME_PATH || [
    'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
  ].find(existsSync);
  const { server, url } = await serve({ port: 0 });
  const browser = await pw.chromium.launch({ headless: true, ...(executablePath ? { executablePath } : {}) });
  const errors = [];
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 }, reducedMotion: 'reduce' });
    page.setDefaultTimeout(5000);
    page.on('pageerror', e => errors.push(e.message));
    page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });

    for (const [w, h] of [[1280, 800], [1100, 560]]) {
      await page.setViewportSize({ width: w, height: h });
      await page.goto(`${url}/?fixture=default-physical#sources`);
      await page.waitForFunction(() => window.verbRoom?.store?.ready && document.querySelector('.sources-output-hint'));
      const hintInfo = await page.evaluate(() => {
        const el = document.querySelector('.sources-output-hint');
        const cs = getComputedStyle(el);
        const r = el.getBoundingClientRect();
        const panel = document.querySelector('.sources-panel').getBoundingClientRect();
        const mixer = document.querySelector('.mixer').getBoundingClientRect();
        const firstRow = document.querySelector('.mix-row:not(.mix-head)').getBoundingClientRect();
        return {
          text: el.textContent,
          fontSize: parseFloat(cs.fontSize),
          visible: r.width > 0 && r.height > 0,
          clippedX: el.scrollWidth > el.clientWidth + 1,
          insidePanel: r.left >= panel.left - 1 && r.right <= panel.right + 1 && r.bottom <= mixer.top + 2,
          mixerHeight: mixer.height,
          firstRowVisible: firstRow.height >= 20 && firstRow.bottom <= mixer.bottom + 1,
          docOverflow: document.documentElement.scrollWidth - document.documentElement.clientWidth
        };
      });
      assert.equal(hintInfo.text, SOURCES_OUTPUT_HINT);
      assert.ok(hintInfo.fontSize >= 11, `hint font ${hintInfo.fontSize} >= 11`);
      assert.equal(hintInfo.visible, true);
      assert.equal(hintInfo.clippedX, false);
      assert.equal(hintInfo.insidePanel, true);
      assert.ok(hintInfo.mixerHeight >= 50, `mixer remains visible at ${w}x${h}: ${hintInfo.mixerHeight}px`);
      assert.equal(hintInfo.firstRowVisible, true, `first mixer row unclipped at ${w}x${h}`);
      assert.equal(hintInfo.docOverflow, 0);
    }

    // Mix tab: default stereo must NOT warn even with Positioned dry toggled OFF
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.getByRole('tab', { name: 'Mix', exact: true }).click();
    await page.waitForFunction(() => document.querySelector('.mix-output'));
    assert.equal(await page.locator('.mix-dry-warning').isVisible(), false);
    await page.getByRole('switch', { name: 'Positioned dry', exact: true }).click();
    await page.waitForFunction(() => window.verbRoom.store.state.parameters.values.spatialDry === 0);
    assert.equal(await page.locator('.mix-dry-warning').isVisible(), false, 'default stereo with Positioned dry OFF must not warn');

    // Switch host to 64/64 and output layout to AmbiX 3rd order -> warning appears immediately & Settings shows 16 / 64 ch
    await page.evaluate(async () => {
      window.verbRoom.bridge.setHost({ inputChannels: 64, outputChannels: 64 });
      await window.verbRoom.store.command({ op: 'output.set', layout: 11 });
    });
    await page.waitForFunction(() => document.querySelector('.mix-dry-warning')?.hidden === false);
    assert.equal(await page.locator('.mix-dry-warning').textContent(), POSITIONED_DRY_MULTICHANNEL_WARNING);
    assert.ok(await page.locator('.mix-dry-warning').evaluate(e => parseFloat(getComputedStyle(e).fontSize) >= 11 && e.getBoundingClientRect().height > 0));

    // Re-enabling Positioned dry hides the warning
    await page.getByRole('switch', { name: 'Positioned dry', exact: true }).click();
    await page.waitForFunction(() => document.querySelector('.mix-dry-warning')?.hidden === true);

    // Settings tab shows 16 / 64 ch and warns when Positioned dry is off
    await page.evaluate(() => {
      const b = window.verbRoom.bridge;
      b.parameters.begin('spatialDry'); b.parameters.change('spatialDry', 0); b.parameters.end('spatialDry');
    });
    await page.getByRole('tab', { name: 'Settings', exact: true }).click();
    await page.waitForFunction(() => /16 \/ 64 ch/.test(document.body.innerText));
    assert.equal(await page.locator('.output-dry-warning').isVisible(), true);
    assert.equal(await page.locator('.output-dry-warning').textContent(), POSITIONED_DRY_MULTICHANNEL_WARNING);
    assert.deepEqual(errors, []);
  } finally {
    await browser.close();
    server.close();
  }
});

test('MO3b Motion toggle, preparing and ready, Bake bypass, scoped undo and compact headers', async () => {
  const candidates = [process.env.PLAYWRIGHT_MODULE, 'playwright', 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'].filter(Boolean);
  let pw;
  for (const candidate of candidates) { try { pw = require(candidate); break; } catch {} }
  assert.ok(pw, 'Playwright available');
  const executablePath = process.env.CHROME_PATH || [
    'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
  ].find(existsSync);
  const shots = process.env.VERB_MOTION_SHOTS;
  if (shots) await mkdir(shots, { recursive: true });
  const { server, url } = await serve({ port: 0 });
  const browser = await pw.chromium.launch({ headless: true, ...(executablePath ? { executablePath } : {}) });
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 720 }, reducedMotion: 'reduce' });
    page.setDefaultTimeout(5000);
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    const capture = async name => { if (shots) await page.screenshot({ path: join(shots, name + '.png') }); };
    const motion = page.getByRole('button', { name: 'Motion', exact: true });
    const status = page.locator('.rebuild-indicator');
    const waitMotion = value => page.waitForFunction(v => document.querySelector('.motion-toggle')?.getAttribute('aria-pressed') === String(v), value);
    await page.goto(url);
    await waitMotion(false);
    assert.match(await motion.getAttribute('title'), /follows moving objects.*without waiting for Bake.*single rooms.*multi-room.*bypassed/i);
    await capture('motion-off-1280x720');
    await page.getByRole('button', { name: 'Creative', exact: true }).click();
    await page.evaluate(() => {
      const { store, bridge } = window.verbRoom;
      store.setView({ selection: [{ kind: 'source', index: 0 }] }, false);
      bridge.setBakeStatus({ state: 'baked', object: 0, percent: 100 });
    });
    await motion.click();
    await status.getByText('Motion: preparing tail…', { exact: true }).waitFor();
    await capture('motion-preparing-1280x720');
    await status.getByText('Motion: ready', { exact: true }).waitFor();
    const bakeNote = page.getByText('Motion bypasses baked tables.', { exact: true });
    await bakeNote.waitFor({ state: 'attached' });
    await bakeNote.scrollIntoViewIfNeeded();
    assert.equal(await page.locator('[data-bake-path="0"]').isDisabled(), true);
    assert.equal(await page.getByRole('button', { name: 'Creative', exact: true }).getAttribute('aria-pressed'), 'true');
    await page.evaluate(() => window.verbRoom.bridge.setRebuilding(true));
    assert.equal(await page.locator('.viewport-rebuilding').isVisible(), false);
    assert.equal(await page.evaluate(() => window.verbRoom.store.telemetry['status.load'].pathBake.state), 'baked');
    await capture('motion-ready-1280x720');

    await page.getByRole('button', { name: /Undo.*Toggle Motion/ }).click();
    await waitMotion(false);
    assert.equal(await page.locator('[data-bake-path="0"]').isDisabled(), false);
    await page.getByRole('button', { name: /Redo.*Toggle Motion/ }).click();
    await waitMotion(true);
    await status.getByText('Motion: ready', { exact: true }).waitFor();
    await page.setViewportSize({ width: 1100, height: 560 });
    const fit = await page.evaluate(() => {
      const header = document.querySelector('.header');
      const footer = document.querySelector('.footer');
      const last = header.querySelector('.header-actions > :last-child');
      return header.scrollWidth <= header.clientWidth + 1
        && last.getBoundingClientRect().right <= innerWidth
        && footer.scrollWidth <= footer.clientWidth + 1;
    });
    assert.ok(fit, 'Motion header and status fit the 1100 × 560 viewport');
    await capture('motion-ready-1100x560');
    assert.deepEqual(errors, []);
  } finally {
    await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
});

