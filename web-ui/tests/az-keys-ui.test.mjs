import assert from 'node:assert/strict';
import test from 'node:test';
import { createRequire } from 'node:module';
import { mkdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { serve } from '../scripts/serve.mjs';
import {
  timelineSections, pathRow, FULL_VIEW,
  AZ_EASINGS, AZ_AT_PLAYHEAD_TOL, azKeyTimeBounds, clampAzKeyTime, newAzKeyTime, azAngleAt, azimuthCurvePath
} from '../src/components/timeline-model.js';

const require = createRequire(import.meta.url);
const { chromium } = require('C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package');
const CHROME = 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const REPO_ROOT = path.resolve(fileURLToPath(new URL('../..', import.meta.url)));
const SHOTS_DIR = path.join(REPO_ROOT, 'out', 'G2', 'shots');

function stateWithPath(patch = {}) {
  const paths = Array.from({ length: 33 }, () => ({
    points: [], enabled: false, closed: false, smooth: false, orient: false,
    orientMode: 'manual', lookTarget: -1, lookPos: [0, 0, 0], orientOffset: 0,
    laps: 1, group: -1, start: 0, end: 100, times: null, easings: null, azKeys: []
  }));
  paths[0] = {
    points: [[1, 1, 1], [3, 2, 1], [4, 2, 1]],
    enabled: true,
    closed: false,
    smooth: false,
    orient: false,
    orientMode: 'manual',
    lookTarget: -1,
    lookPos: [0, 0, 0],
    orientOffset: 0,
    laps: 1,
    group: -1,
    start: 0,
    end: 100,
    times: null,
    easings: null,
    azKeys: [],
    ...patch
  };
  return {
    document: {
      sources: [{ name: 'Voice', colour: '#6aa4c8', muted: false, solo: false }],
      listener: { name: 'Listener' },
      paths
    },
    parameters: { values: { s1path: 0, s1x: 1, s1y: 1, s1z: 1, s1az: 0, lx: 2, ly: 2, lz: 1.5, laz: 0 } }
  };
}

test('G2 unit: AZ_EASINGS, azKeyTimeBounds, clampAzKeyTime, newAzKeyTime', { timeout: 10000 }, () => {
  assert.deepEqual(AZ_EASINGS, ['linear', 'easeIn', 'easeOut', 'easeInOut', 'hold']);
  assert.equal(AZ_AT_PLAYHEAD_TOL, 0.5);

  const p = { azKeys: [{ t: 10, az: 0, easing: 'linear' }, { t: 40, az: 90, easing: 'easeIn' }, { t: 80, az: -90, easing: 'hold' }] };
  assert.deepEqual(azKeyTimeBounds(p, 0), { min: 0, max: 39.95 });
  assert.deepEqual(azKeyTimeBounds(p, 1), { min: 10.05, max: 79.95 });
  assert.deepEqual(azKeyTimeBounds(p, 2), { min: 40.05, max: 100 });

  assert.equal(clampAzKeyTime(p, 1, 5), 10.05);
  assert.equal(clampAzKeyTime(p, 1, 95), 79.95);
  assert.equal(newAzKeyTime(p, 1, 33.7, false), 33.5);
  assert.equal(newAzKeyTime(p, 1, 33.73, true), 33.75);
});

test('G2 unit: pathRow, azAngleAt & azimuthCurvePath (hold, easings, ±180 seam, zoom)', { timeout: 10000 }, () => {
  const s = stateWithPath({
    azKeys: [
      { t: 10, az: 170, easing: 'linear' },
      { t: 50, az: -170, easing: 'hold' },
      { t: 90, az: 45, easing: 'easeInOut' }
    ]
  });
  s.parameters.values.s1path = 0.3;
  const row = pathRow(s, 0);
  assert.equal(row.azMode, 'manual');
  assert.equal(row.azActive, true);
  assert.equal(row.azKeys.length, 3);
  assert.ok(Math.abs(Math.abs(row.azCurrent) - 180) < 0.01);

  assert.equal(azAngleAt(s, 0, 5), 170);
  assert.equal(azAngleAt(s, 0, 60), -170);
  assert.equal(azAngleAt(s, 0, 95), 45);

  const dFull = azimuthCurvePath(row.azKeys, FULL_VIEW, 1000, 24);
  const moveCount = (dFull.match(/M/g) || []).length;
  assert.ok(moveCount >= 2, `expected seam split with >=2 M commands, got: ${dFull}`);
  assert.match(dFull, /L 900 20\.5 L 900 9\.75\b/, `expected hold step in SVG path, got: ${dFull}`);

  const dEase = azimuthCurvePath([
    { t: 0, az: -45, easing: 'easeIn' },
    { t: 100, az: 45, easing: 'linear' }
  ], { lo: 20, hi: 80 }, 600, 24);
  assert.equal((dEase.match(/M/g) || []).length, 1);

  assert.equal(azimuthCurvePath([], FULL_VIEW, 1000, 24), '');

  const secs = timelineSections(s);
  assert.equal(secs.length, 1);
  assert.equal(secs[0].rows[0].azActive, true);

  // Stationary path returns azActive: false
  const stat = stateWithPath({ points: [] });
  assert.equal(pathRow(stat, 0).azActive, false);
});

test('G2 browser E2E: azimuth strip, editor, RMB rotate, modes, slider lock, inspector, keyboard & 8-obj 1100x560 layout', { timeout: 60000 }, async () => {
  await mkdir(SHOTS_DIR, { recursive: true });
  let server = null, browser = null;
  try {
    server = await serve({ port: 0 });
    browser = await chromium.launch({ executablePath: CHROME });
    const page = await browser.newPage({ viewport: { width: 1280, height: 760 } });
    page.setDefaultTimeout(5000);
    const errors = [];
    page.on('pageerror', e => errors.push(`pageerror: ${e.message}`));
    page.on('console', m => { if (m.type() === 'error') errors.push(`console.error: ${m.text()}`); });

    const settle = () => page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));

    await page.goto(server.url);
    await page.waitForFunction(() => window.verbRoom?.store?.ready);

    // 1. Add a path to Voice (object 0), open Sources tab and Timeline
    await page.evaluate(async () => {
      const s = window.verbRoom.store;
      await s.command({ op: 'path.set', object: 0, points: [[1, 1, 1], [4, 2, 1], [6, 3, 1]], enabled: true, start: 0, end: 100 });
      s.setView({ selection: [{ kind: 'source', index: 0 }], timelineOpen: true });
    });
    await page.click('[data-focus-key="tab-sources"]');
    await page.waitForSelector('.timeline-panel');
    await page.waitForSelector('.tl-az-row[data-az-object="0"]');
    await settle();

    // Shot 1: timeline open with moving source and 0 azimuth keys
    await page.screenshot({ path: path.join(SHOTS_DIR, '01_default.png') });

    // Stationary object (listener = 32) must NOT appear in timeline / azimuth rows
    assert.equal(await page.locator('.tl-row[data-object="32"]').count(), 0);
    assert.equal(await page.locator('.tl-az-row[data-az-object="32"]').count(), 0);

    // Focus button and count in inspector
    assert.equal(await page.locator('[data-az-keys-info="0"] .az-keys-label').textContent(), 'Azimuth keys: 0');
    assert.equal(await page.locator('[data-parameter="s1az"].locked').count(), 0);

    // 2. Add key via + Key at playhead (0%), add via double-click at 40%, add 3rd at 80%
    await page.click('.tl-az-row[data-az-object="0"] .tl-az-add');
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].azKeys?.length === 1);
    await settle();
    assert.equal(await page.evaluate(() => window.verbRoom.store.state.document.paths[0].azKeys.length), 1);
    assert.equal(await page.locator('[data-az-keys-info="0"] .az-keys-label').textContent(), 'Azimuth keys: 1');

    // Slider must now be locked and show lock tooltip explaining Azimuth keys
    const azLockTitle = await page.getAttribute('[data-parameter="s1az"]', 'title');
    assert.match(azLockTitle || '', /Locked: driven by Azimuth keys/i);
    assert.equal(await page.locator('[data-parameter="s1az"].locked').count(), 1);

    // Double-click empty az track at 40% to add key #2
    const azTrackBox = await page.locator('.tl-az-row[data-az-object="0"] .tl-az-track .tl-inner').boundingBox();
    await page.mouse.dblclick(azTrackBox.x + azTrackBox.width * 0.4, azTrackBox.y + azTrackBox.height * 0.5);
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].azKeys?.length === 2);
    await settle();
    await page.evaluate(async () => {
      const s = window.verbRoom.store;
      await s.command({ op: 'path.setAzimuthKey', object: 0, index: 0, az: 0 });
      await s.command({ op: 'path.setAzimuthKey', object: 0, index: 1, az: 90 });
      await s.command({ op: 'path.setAzimuthKey', object: 0, t: 80, az: -150 });
    });
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].azKeys?.length === 3);
    await settle();
    assert.equal(await page.locator('.tl-az-row[data-az-object="0"] .tl-az-key').count(), 3);
    assert.equal(await page.locator('[data-az-keys-info="0"] .az-keys-label').textContent(), 'Azimuth keys: 3');

    // Scrub playhead to 40% -> key 1 (.tl-az-key[data-az-key="1"]) gets .at-ph, live readout shows 90°
    await page.evaluate(() => {
      const b = window.verbRoom.bridge;
      b.parameters.begin('s1path'); b.parameters.change('s1path', 0.4); b.parameters.end('s1path');
    });
    await settle();
    assert.equal(await page.locator('.tl-az-row[data-az-object="0"] .tl-az-key[data-az-key="1"].at-ph').count(), 1);
    assert.equal(await page.locator('.tl-az-row[data-az-object="0"] [data-az-readout="0"]').textContent(), '90°');

    // Scrub to 20% -> readout shows interpolated 45°, slider aria-valuetext shows 45°, key 1 loses .at-ph
    await page.evaluate(() => {
      const b = window.verbRoom.bridge;
      b.parameters.begin('s1path'); b.parameters.change('s1path', 0.2); b.parameters.end('s1path');
    });
    await settle();
    assert.equal(await page.locator('.tl-az-row[data-az-object="0"] .tl-az-key[data-az-key="1"].at-ph').count(), 0);
    assert.equal(await page.locator('.tl-az-row[data-az-object="0"] [data-az-readout="0"]').textContent(), '45°');
    assert.match(await page.locator('[data-parameter="s1az"] [role="slider"]').getAttribute('aria-valuetext') || '', /45/);
    assert.match(await page.locator('[data-parameter="s1az"] input.readout').inputValue(), /45/);

    // Clear selection so Shot 2 shows clean multi-key curve
    await page.evaluate(() => window.verbRoom.store.setView({ azKey: null }, false));
    await settle();
    await page.screenshot({ path: path.join(SHOTS_DIR, '02_with_keys.png') });

    // 3. Drag key #1 horizontally in time: 1 undo step, clamped inside neighbors, 0.5% snap
    const depthBeforeDrag = await page.evaluate(() => window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0);
    const k1Box = await page.locator('.tl-az-row[data-az-object="0"] .tl-az-key[data-az-key="1"]').boundingBox();
    await page.mouse.move(k1Box.x + k1Box.width / 2, k1Box.y + k1Box.height / 2);
    await page.mouse.down();
    await page.mouse.move(azTrackBox.x + azTrackBox.width * 0.52, k1Box.y + k1Box.height / 2, { steps: 6 });
    await page.mouse.move(azTrackBox.x + azTrackBox.width * 0.6, k1Box.y + k1Box.height / 2, { steps: 6 });
    await page.mouse.up();
    await page.waitForFunction(d => (window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0) === d + 1 && Math.abs(window.verbRoom.store.state.document.paths[0].azKeys[1].t - 60) < 1, depthBeforeDrag);
    await settle();
    const afterDrag = await page.evaluate(() => ({
      t: window.verbRoom.store.state.document.paths[0].azKeys[1].t,
      depth: window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0
    }));
    assert.ok(afterDrag.t > 55 && afterDrag.t < 65, `expected key 1 moved near 60%, got ${afterDrag.t}`);
    assert.equal(afterDrag.depth, depthBeforeDrag + 1, 'dragging key in time must add exactly 1 undo step');
    await page.evaluate(() => window.verbRoom.store.command({ op: 'history.undo' }));
    await page.waitForFunction(() => Math.abs(window.verbRoom.store.state.document.paths[0].azKeys[1].t - 40) < 0.01);
    await settle();

    // 4. Context menu -> Hold changes curve SVG path
    const dBeforeHold = await page.locator('.tl-az-row[data-az-object="0"] .tl-az-path').getAttribute('d');
    await page.locator('.tl-az-row[data-az-object="0"] .tl-az-key[data-az-key="1"]').click({ button: 'right' });
    await page.waitForSelector('.tl-menu');
    const menuItems = await page.locator('.tl-menu button').allTextContents();
    assert.deepEqual(menuItems, ['Linear', 'Ease in', 'Ease out', 'Ease in-out', 'Hold', 'Delete key']);
    await page.locator('.tl-menu button:has-text("Hold")').click();
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].azKeys[1].easing === 'hold');
    await settle();
    const dAfterHold = await page.locator('.tl-az-row[data-az-object="0"] .tl-az-path').getAttribute('d');
    assert.notEqual(dAfterHold, dBeforeHold);
    assert.equal(await page.locator('.tl-az-row[data-az-object="0"] .tl-az-key[data-az-key="1"]').getAttribute('data-ease'), 'hold');
    await page.screenshot({ path: path.join(SHOTS_DIR, '03_hold.png') });

    // 5. Select key #1 -> angle editor slider & numeric field
    await page.locator('.tl-az-row[data-az-object="0"] .tl-az-key[data-az-key="1"]').click();
    await page.waitForSelector('.tl-az-editor');
    await page.screenshot({ path: path.join(SHOTS_DIR, '04_selected_key.png') });

    const depthBeforeSlider = await page.evaluate(() => window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0);
    const edSlider = await page.locator('.tl-az-editor .tl-az-slider').boundingBox();
    await page.mouse.move(edSlider.x + edSlider.width * 0.25, edSlider.y + edSlider.height / 2);
    await page.mouse.down();
    await page.mouse.move(edSlider.x + edSlider.width * 0.1, edSlider.y + edSlider.height / 2, { steps: 5 });
    await page.mouse.up();
    await page.waitForFunction(d => (window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0) === d + 1 && window.verbRoom.store.state.document.paths[0].azKeys[1].az < -100, depthBeforeSlider);
    await settle();
    const afterSlider = await page.evaluate(() => ({
      az: window.verbRoom.store.state.document.paths[0].azKeys[1].az,
      depth: window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0
    }));
    assert.ok(afterSlider.az < -100, `expected angle slider left side < -100°, got ${afterSlider.az}`);
    assert.equal(afterSlider.depth, depthBeforeSlider + 1, 'angle slider drag must be 1 undo step');

    // Numeric field input (valueField opens edit mode on double-click)
    const angField = page.locator('.tl-az-editor .tl-az-angle-input');
    await angField.dblclick();
    await angField.fill('-60');
    await angField.press('Enter');
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].azKeys[1].az === -60);
    await settle();
    assert.equal(await page.evaluate(() => window.verbRoom.store.state.document.paths[0].azKeys[1].az), -60);

    // Keyboard arrow & Shift+Arrow on marker retains focus
    const k1 = page.locator('.tl-az-row[data-az-object="0"] .tl-az-key[data-az-key="1"]');
    await k1.focus();
    assert.equal(await k1.getAttribute('tabindex'), '0');
    assert.match((await k1.getAttribute('aria-label')) || '', /-60° at 40 percent/);
    await page.keyboard.press('ArrowRight');
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].azKeys[1].t === 40.5);
    await settle();
    assert.equal(await page.evaluate(() => window.verbRoom.store.state.document.paths[0].azKeys[1].t), 40.5);
    assert.equal(await page.evaluate(() => document.activeElement?.getAttribute('data-az-key')), '1');
    await page.keyboard.down('Shift');
    await page.keyboard.press('ArrowRight');
    await page.keyboard.up('Shift');
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].azKeys[1].t === 40.55);
    await settle();
    assert.equal(await page.evaluate(() => window.verbRoom.store.state.document.paths[0].azKeys[1].t), 40.55);

    // 6. Viewport RMB rotation behavior
    const canvasBox = await page.locator('canvas').first().boundingBox();
    // 6a. Active keys, NO selected key, playhead at 20% (> 0.5% away from any key) -> BLOCKED!
    await page.evaluate(() => window.verbRoom.store.setView({ azKey: null }, false));
    await settle();
    const beforeBlocked = await page.evaluate(() => ({
      keys: window.verbRoom.store.state.document.paths[0].azKeys.map(k => ({ ...k })),
      s1az: window.verbRoom.store.state.parameters.values.s1az
    }));
    await page.mouse.move(canvasBox.x + canvasBox.width * 0.78, canvasBox.y + canvasBox.height * 0.3);
    await page.mouse.down({ button: 'right' });
    await page.mouse.move(canvasBox.x + canvasBox.width * 0.2, canvasBox.y + canvasBox.height * 0.75, { steps: 4 });
    await page.mouse.up({ button: 'right' });
    await settle();
    const afterBlocked = await page.evaluate(() => ({
      keys: window.verbRoom.store.state.document.paths[0].azKeys,
      s1az: window.verbRoom.store.state.parameters.values.s1az,
      hint: document.querySelector('.viewport-hover')?.textContent
    }));
    assert.deepEqual(afterBlocked.keys, beforeBlocked.keys);
    assert.equal(afterBlocked.s1az, beforeBlocked.s1az);
    assert.equal(afterBlocked.hint, 'Select a key or move the playhead onto a key to rotate; use + Key to add one');

    // 6b. Playhead at 40.55% (within 0.5% of key 1) WITHOUT azKey selected -> rotates key 1 in 1 undo step
    await page.evaluate(() => {
      const b = window.verbRoom.bridge;
      b.parameters.begin('s1path'); b.parameters.change('s1path', 0.4055); b.parameters.end('s1path');
    });
    await settle();
    const depthBeforeRmb = await page.evaluate(() => window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0);
    await page.mouse.move(canvasBox.x + canvasBox.width * 0.85, canvasBox.y + canvasBox.height * 0.25);
    await page.mouse.down({ button: 'right' });
    await page.mouse.move(canvasBox.x + canvasBox.width * 0.25, canvasBox.y + canvasBox.height * 0.75, { steps: 5 });
    await page.mouse.up({ button: 'right' });
    await page.waitForFunction(d => (window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0) === d + 1, depthBeforeRmb);
    await settle();
    const afterRmb = await page.evaluate(() => ({
      az: window.verbRoom.store.state.document.paths[0].azKeys[1].az,
      depth: window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0
    }));
    assert.notEqual(afterRmb.az, -60);
    assert.equal(afterRmb.depth, depthBeforeRmb + 1, 'viewport RMB rotation must commit 1 undo step');

    // 6c. Moving object WITHOUT azimuth keys -> RMB rotates host parameter s2az normally
    await page.evaluate(async () => {
      const s = window.verbRoom.store;
      await s.command({ op: 'source.add' });
      await s.command({ op: 'path.set', object: 1, points: [[2, 1, 1], [3, 2, 1]], enabled: true, start: 0, end: 100 });
      s.setView({ selection: [{ kind: 'source', index: 1 }], azKey: null });
    });
    await settle();
    const s2Before = await page.evaluate(() => window.verbRoom.store.state.parameters.values.s2az);
    await page.mouse.move(canvasBox.x + canvasBox.width * 0.85, canvasBox.y + canvasBox.height * 0.5);
    await page.mouse.down({ button: 'right' });
    await page.mouse.move(canvasBox.x + canvasBox.width * 0.25, canvasBox.y + canvasBox.height * 0.25, { steps: 4 });
    await page.mouse.up({ button: 'right' });
    await page.waitForFunction(b => window.verbRoom.store.state.parameters.values.s2az !== b, s2Before);
    await settle();
    const s2After = await page.evaluate(() => window.verbRoom.store.state.parameters.values.s2az);
    assert.notEqual(s2After, s2Before, 'moving object with 0 azimuth keys should still rotate host azimuth');

    // 7. Along path & Look at dim strip with hint, keep keys
    await page.evaluate(async () => {
      const s = window.verbRoom.store;
      s.setView({ selection: [{ kind: 'source', index: 0 }] });
      await s.command({ op: 'path.setFlags', object: 0, orientMode: 'along' });
    });
    await page.waitForSelector('.tl-az-row[data-az-object="0"].dim.az-inactive');
    await settle();
    assert.equal(await page.locator('.tl-az-row[data-az-object="0"].dim.az-inactive').count(), 1);
    assert.equal(await page.locator('.tl-az-row[data-az-object="0"] .tl-az-hint').textContent(), 'Angle is driven by Along path');
    assert.equal(await page.locator('.az-keys-dim-hint').textContent(), 'Angle is driven by Along path (keys kept)');
    assert.equal(await page.evaluate(() => window.verbRoom.store.state.document.paths[0].azKeys.length), 3);
    await page.screenshot({ path: path.join(SHOTS_DIR, '05_dimmed_along_look.png') });

    await page.evaluate(() => window.verbRoom.store.command({ op: 'path.setFlags', object: 0, orientMode: 'lookAt' }));
    await page.waitForFunction(() => document.querySelector('.tl-az-row[data-az-object="0"] .tl-az-hint')?.textContent === 'Angle is driven by Look at');
    await settle();
    assert.equal(await page.locator('.tl-az-row[data-az-object="0"] .tl-az-hint').textContent(), 'Angle is driven by Look at');
    assert.equal(await page.locator('.az-keys-dim-hint').textContent(), 'Angle is driven by Look at (keys kept)');
    await page.evaluate(() => window.verbRoom.store.command({ op: 'path.setFlags', object: 0, orientMode: 'manual' }));
    await page.waitForFunction(() => !document.querySelector('.tl-az-row[data-az-object="0"]')?.classList.contains('az-inactive'));
    await settle();

    // Delete key via menu & Del key
    await page.locator('.tl-az-row[data-az-object="0"] .tl-az-key[data-az-key="2"]').click({ button: 'right' });
    await page.locator('.tl-menu button:has-text("Delete key")').click();
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].azKeys.length === 2);
    await settle();
    assert.equal(await page.evaluate(() => window.verbRoom.store.state.document.paths[0].azKeys.length), 2);
    await page.locator('.tl-az-row[data-az-object="0"] .tl-az-key[data-az-key="1"]').click();
    await page.keyboard.press('Delete');
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].azKeys.length === 1);
    await settle();
    assert.equal(await page.evaluate(() => window.verbRoom.store.state.document.paths[0].azKeys.length), 1);

    // Focus button from inspector opens timeline and focuses strip
    await page.evaluate(() => window.verbRoom.store.setView({ timelineOpen: false }, false));
    await settle();
    await page.click('[data-az-focus="0"]');
    await page.waitForSelector('.timeline-panel');
    await page.waitForTimeout(80);
    const focusedInStrip = await page.evaluate(() => document.querySelector('.tl-az-row[data-az-object="0"]')?.contains(document.activeElement));
    assert.equal(focusedInStrip, true);

    // 8. 8 moving objects (including grouped rows) at 1100x560: alignment, scrollArea, >= 11px font, zero clipping
    await page.setViewportSize({ width: 1100, height: 560 });
    await page.evaluate(async () => {
      const s = window.verbRoom.store;
      while (s.state.document.sources.length < 7) await s.command({ op: 'source.add' });
      for (let o = 0; o < 7; o++) {
        await s.command({ op: 'path.set', object: o, points: [[1, 1 + o * 0.2, 1], [3, 1 + o * 0.2, 1]], enabled: true, start: 5 + o * 2, end: 90, group: o < 2 ? 0 : -1 });
        await s.command({ op: 'path.setAzimuthKeys', object: o, keys: [{ t: 15, az: -45, easing: 'linear' }, { t: 75, az: 60, easing: 'easeOut' }] });
      }
      await s.command({ op: 'path.set', object: 32, points: [[2, 2, 1.5], [4, 3, 1.5]], enabled: true, start: 10, end: 95, group: -1 });
      await s.command({ op: 'path.setAzimuthKeys', object: 32, keys: [{ t: 25, az: 30, easing: 'hold' }, { t: 80, az: -30, easing: 'linear' }] });
      s.setView({ selection: [{ kind: 'source', index: 0 }], azKey: { object: 0, index: 1 }, timelineOpen: true });
    });
    await settle();
    await page.waitForTimeout(100);
    await page.screenshot({ path: path.join(SHOTS_DIR, '06_small_1100x560_8objects.png') });

    const audit = await page.evaluate(() => {
      const panel = document.querySelector('.timeline-panel');
      const body = panel?.querySelector('.tl-body');
      const pathTrack = panel?.querySelector('.tl-row[data-object="0"] .tl-track')?.getBoundingClientRect();
      const azTrack = panel?.querySelector('.tl-az-row[data-az-object="0"] .tl-az-track')?.getBoundingClientRect();
      const grpTrack = panel?.querySelector('.tl-gbar .tl-track')?.getBoundingClientRect();
      const smallFonts = [];
      const w = document.createTreeWalker(panel, NodeFilter.SHOW_TEXT);
      for (let n = w.nextNode(); n; n = w.nextNode()) {
        if (!n.nodeValue?.trim()) continue;
        const el = n.parentElement;
        if (!el || el.offsetParent === null) continue;
        const fs = parseFloat(getComputedStyle(el).fontSize);
        if (fs < 11) smallFonts.push({ text: n.nodeValue.trim(), fs, cls: el.className });
      }
      const clipped = [];
      for (const el of panel.querySelectorAll('button, input, [role="slider"], .tl-az-caption, .tl-az-readout, .tl-az-hint, .tl-az-editor-info')) {
        if (el.offsetParent === null) continue;
        if (el.scrollWidth > el.clientWidth + 2 && getComputedStyle(el).textOverflow !== 'ellipsis') {
          clipped.push({ text: el.textContent?.trim(), cls: el.className, sw: el.scrollWidth, cw: el.clientWidth });
        }
        const r = el.getBoundingClientRect();
        const pr = panel.getBoundingClientRect();
        if (r.right > pr.right + 1 || r.left < pr.left - 1) clipped.push({ outOfPanel: true, cls: el.className, r, pr });
      }
      const saScrolls = body && body.scrollHeight > body.clientHeight;
      if (body) body.scrollTop = body.scrollHeight;
      return {
        rows: panel?.querySelectorAll('.tl-row[data-object]').length,
        azRows: panel?.querySelectorAll('.tl-az-row').length,
        alignTrackDiff: Math.abs((pathTrack?.left ?? 0) - (azTrack?.left ?? 999)),
        alignWidthDiff: Math.abs((pathTrack?.width ?? 0) - (azTrack?.width ?? 999)),
        alignGroupDiff: Math.abs((grpTrack?.left ?? 0) - (azTrack?.left ?? 999)),
        smallFonts,
        clipped,
        saScrolls,
        scrolledTop: body?.scrollTop ?? 0
      };
    });
    assert.equal(audit.rows, 8);
    assert.equal(audit.azRows, 8);
    assert.ok(audit.alignTrackDiff <= 1, `track left mismatch: ${audit.alignTrackDiff}`);
    assert.ok(audit.alignWidthDiff <= 1, `track width mismatch: ${audit.alignWidthDiff}`);
    assert.ok(audit.alignGroupDiff <= 1, `group track left mismatch: ${audit.alignGroupDiff}`);
    assert.deepEqual(audit.smallFonts, [], `found fonts < 11px: ${JSON.stringify(audit.smallFonts)}`);
    assert.deepEqual(audit.clipped, [], `found clipped controls: ${JSON.stringify(audit.clipped)}`);
    assert.equal(audit.saScrolls, true, '8 moving objects at 1100x560 must activate scrollArea');
    assert.ok(audit.scrolledTop > 0, 'scrollArea must scroll vertically');

    assert.deepEqual(errors, [], `unexpected runtime errors: ${errors.join('\n')}`);
  } finally {
    await browser?.close().catch(() => {});
    server?.server?.closeAllConnections?.();
    if (server?.server) await new Promise(r => server.server.close(r));
  }
});
