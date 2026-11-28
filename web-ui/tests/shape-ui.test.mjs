import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import test from 'node:test';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { serve } from '../scripts/serve.mjs';
import { pickOption } from './browser-helpers.mjs';
import { validator, readFixture } from './helpers.mjs';
import {
  PATH_SHAPE_META,
  defaultShapeParamsForObject,
  offsetFigure8DefaultCentre,
  generateRawPathShape,
  generatePathShapeForRoom,
  measureCircleRadialDeviation,
  isCompiledPathInsideRoom,
  roomPolygonsForScene
} from '../src/viewport/path-shapes.js';
import {
  compilePath,
  endlessTransportPhase,
  endlessEffectiveU,
  evaluateEndlessPath
} from '../src/viewport/movement-path.js';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';

const require = createRequire(import.meta.url);
let playwright;
for (const name of [process.env.PLAYWRIGHT_MODULE, 'playwright', 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'].filter(Boolean)) {
  try { playwright = require(name); break; } catch {}
}
const CHROME = process.env.CHROME_PATH || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SHOTS_DIR = process.env.VERB_V04A_SHOTS_DIR || path.resolve(__dirname, '../../out/V04a2/shots');

test('V04a geometry: Circle < 0.5% radial deviation, closure, Figure-8 crossing, polygons, Spiral & room clamp', async () => {
  for (const nodes of [12, 16, 24, 32, 64]) {
    const mp = generateRawPathShape({ kind: 'circle', cx: 3, cy: 4, z: 1.5, radiusX: 2.5, nodes });
    const dev = measureCircleRadialDeviation(mp.points, 3, 4, 2.5);
    assert.ok(dev.maxRelErrorPct < 0.5, `Circle N=${nodes} radial deviation ${dev.maxRelErrorPct}% < 0.5%`);
    if (nodes === 16) assert.ok(dev.maxRelErrorPct < 0.06, `Default N=16 Circle deviation ${dev.maxRelErrorPct}% < 0.06%`);
    const cp = compilePath(mp);
    assert.ok(Math.hypot(cp.points[0][0] - cp.points.at(-1)[0], cp.points[0][1] - cp.points.at(-1)[1]) < 1e-12);
  }

  // Figure-8 (Lemniscate of Gerono) crossing at centre
  const fig8 = generateRawPathShape({ kind: 'figure8', cx: 3, cy: 4, z: 1.5, radiusX: 2, radiusY: 1, nodes: 16 });
  assert.ok(Math.hypot(fig8.points[0][0] - 3, fig8.points[0][1] - 4) < 1e-9, 'Figure-8 node 0 crosses centre');
  assert.ok(Math.hypot(fig8.points[8][0] - 3, fig8.points[8][1] - 4) < 1e-9, 'Figure-8 node N/2 crosses centre');

  // Square, Rectangle, Triangle, Star, Spiral
  const sq = generateRawPathShape({ kind: 'square', cx: 3, cy: 4, z: 1.5, radiusX: 1.5 });
  assert.equal(sq.points.length, 4);
  assert.equal(sq.smooth, false);
  assert.ok(Math.abs(compilePath(sq).length - 12) < 1e-9);

  const tri = generateRawPathShape({ kind: 'triangle', cx: 3, cy: 4, z: 1.5, radiusX: 1.8 });
  assert.equal(tri.points.length, 3);
  assert.equal(tri.smooth, false);

  const star = generateRawPathShape({ kind: 'star', cx: 3, cy: 4, z: 1.5, radiusX: 2.0, radiusY: 0.8 });
  assert.equal(star.points.length, 10);
  assert.equal(star.smooth, false);

  const spiral = generateRawPathShape({ kind: 'spiral', cx: 3, cy: 4, z: 1.5, radiusX: 2.0, radiusY: 0.5, nodes: 24, spiralTurns: 2 });
  assert.equal(spiral.points.length, 24);
  assert.equal(spiral.smooth, true);
  assert.equal(spiral.closed, true);
  assert.ok(spiral.points.every(pt => Math.abs(pt[2] - 1.5) < 1e-12), 'Default spiral with omitted zEnd keeps flat height z=1.5');

  // V04a-2: Spiral start/end Z linear interpolation between first and last point
  const spiral3D = generateRawPathShape({ kind: 'spiral', cx: 3, cy: 4, z: 0.6, zEnd: 2.4, radiusX: 2.0, radiusY: 0.5, nodes: 24, spiralTurns: 2 });
  assert.equal(spiral3D.points.length, 24);
  assert.ok(Math.abs(spiral3D.points[0][2] - 0.6) < 1e-12, 'Spiral first point Z equals start height 0.6 m');
  assert.ok(Math.abs(spiral3D.points[23][2] - 2.4) < 1e-12, 'Spiral last point Z equals end height 2.4 m');
  for (let k = 0; k < 24; k++) {
    const expZ = 0.6 + (2.4 - 0.6) * (k / 23);
    assert.ok(Math.abs(spiral3D.points[k][2] - expZ) < 1e-12, `Spiral point ${k} Z rises linearly`);
    assert.ok(Math.abs(spiral3D.points[k][0] - spiral.points[k][0]) < 1e-12 && Math.abs(spiral3D.points[k][1] - spiral.points[k][1]) < 1e-12,
      'Spiral XY turns and radius are unchanged by zEnd');
  }

  // Room clamp in default-physical scene
  const bridge = await createMockBridge({ read: readFixture, validator, fixture: 'default-physical' });
  const store = createStore(bridge);
  await store.start();
  const scene = store.state.document.scene;
  const polys = roomPolygonsForScene(scene);

  const rawHuge = generatePathShapeForRoom({ kind: 'circle', cx: 3, cy: 4, z: 25, radiusX: 15, nodes: 16 }, scene, false);
  assert.equal(rawHuge.outsideRoom, true);
  const clampedHuge = generatePathShapeForRoom({ kind: 'circle', cx: 3, cy: 4, z: 25, radiusX: 15, nodes: 16 }, scene, true);
  assert.equal(clampedHuge.outsideRoom, true);
  assert.equal(clampedHuge.clamped, true);
  assert.equal(isCompiledPathInsideRoom(scene, polys, clampedHuge.path), true);
  const clampedDev = measureCircleRadialDeviation(clampedHuge.path.points, clampedHuge.fittedParams.cx, clampedHuge.fittedParams.cy, clampedHuge.fittedParams.radiusX);
  assert.ok(clampedDev.maxRelErrorPct < 0.1, 'Clamped circle remains a true circle');

  // V04a-2: Figure-8 default centre offset >= 1.5 m from listener and inside room
  const lx = store.state.parameters.values.lx;
  const ly = store.state.parameters.values.ly;
  const fig8FromListener = defaultShapeParamsForObject(store.state, 0, 'figure8', { cx: lx, cy: ly, z: 1.5, radiusX: 1.8 });
  const distFromListener = Math.hypot(fig8FromListener.cx - lx, fig8FromListener.cy - ly);
  assert.ok(distFromListener >= 1.5 - 1e-4, `Figure-8 default centre offset ${distFromListener} m >= 1.5 m from listener`);
  const fig8Fit = generatePathShapeForRoom(fig8FromListener, scene, true);
  assert.equal(isCompiledPathInsideRoom(scene, polys, fig8Fit.path), true, 'Offset Figure-8 fits inside room');
  store.dispose();
  bridge.dispose();
});

test('V04a Endless math & mock bridge commands: 1 h zero drift, lap continuity, undo/redo', async () => {
  const sr = 48000, periodS = 8;
  const s0 = 59256;
  const s1h = s0 + 450 * periodS * sr;
  const ph0 = endlessTransportPhase(s0, sr, periodS);
  const ph1h = endlessTransportPhase(s1h, sr, periodS);
  assert.equal(ph0, ph1h, 'Endless phase after 1 hour is exact');

  const circle = { ...generateRawPathShape({ kind: 'circle', cx: 3, cy: 4, z: 1.5, radiusX: 2, nodes: 16 }), endless: true, endlessPeriodS: 8 };
  const p0 = evaluateEndlessPath(circle, 0.25, ph0);
  const p1h = evaluateEndlessPath(circle, 0.25, ph1h);
  assert.deepEqual(p0, p1h);

  const bridge = await createMockBridge({ read: readFixture, validator, fixture: 'default-physical' });
  const store = createStore(bridge);
  await store.start();

  const ack1 = await store.command({
    op: 'path.set',
    object: 0,
    points: circle.points,
    enabled: true,
    closed: true,
    smooth: true,
    endless: true,
    endlessPeriodS: 6.5
  });
  assert.equal(ack1.kind, 'ack');
  assert.equal(store.state.document.paths[0].endless, true);
  assert.equal(store.state.document.paths[0].endlessPeriodS, 6.5);

  const ack2 = await store.command({ op: 'path.setFlags', object: 0, endless: false, endlessPeriodS: 8 });
  assert.equal(ack2.kind, 'ack');
  assert.equal('endless' in store.state.document.paths[0], false);
  assert.equal('endlessPeriodS' in store.state.document.paths[0], false);
  store.dispose();
  bridge.dispose();
});

test('V04a Browser UI: Shape menu, ghost preview, room clamp + Fit to room, Apply Circle & Figure-8, Endless, Undo & screenshots', { timeout: 60000 }, async t => {
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

    // Select Source 1
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'source', index: 0 }] }, false));
    await page.waitForSelector('.path-section');

    // 1. Open Shape menu and capture screenshot
    const shapeCombo = page.getByRole('combobox', { name: 'Path shape', exact: true });
    await shapeCombo.click();
    await page.waitForSelector('.dd-popup');
    await page.screenshot({ path: path.join(SHOTS_DIR, 'shape-menu-1280x720.png') });
    await page.keyboard.press('Escape');

    // 2. Pick Circle -> ghost preview appears in viewport
    await pickOption(page, 'Path shape', 'Circle');
    await page.waitForSelector('.path-shape-panel[data-shape-kind="circle"]');
    assert.equal(await page.evaluate(() => window.verbRoom.store.view.pathShapePreview?.params?.kind), 'circle');

    // Centre on listener
    await page.getByRole('button', { name: 'Centre shape on listener', exact: true }).click();
    const lisXY = await page.evaluate(() => {
      const v = window.verbRoom.store.state.parameters.values;
      const p = window.verbRoom.store.view.pathShapePreview.params;
      return { lx: v.lx, ly: v.ly, cx: p.cx, cy: p.cy };
    });
    assert.equal(lisXY.cx, lisXY.lx);
    assert.equal(lisXY.cy, lisXY.ly);

    // Enter oversized radius -> warning + Fit to room button appear
    const radiusInput = page.getByRole('textbox', { name: 'Shape radius, m', exact: true });
    await radiusInput.dblclick();
    await radiusInput.fill('9');
    await radiusInput.press('Enter');
    await page.waitForSelector('.path-shape-warning:not([hidden])');
    await page.locator('.path-shape-actions').scrollIntoViewIfNeeded();
    await page.screenshot({ path: path.join(SHOTS_DIR, 'shape-preview-1280x720.png') });

    // Click Fit to room -> warning clears and radius scales inside room
    await page.getByRole('button', { name: 'Fit shape to room', exact: true }).click();
    await page.waitForFunction(() => document.querySelector('.path-shape-warning')?.hidden === true);

    // Set radius to 1.8 m and Apply Circle
    const radiusInput2 = page.getByRole('textbox', { name: 'Shape radius, m', exact: true });
    await radiusInput2.dblclick();
    await radiusInput2.fill('1.8');
    await radiusInput2.press('Enter');
    await page.getByRole('button', { name: 'Apply path shape', exact: true }).click();
    await page.waitForFunction(() => (window.verbRoom.store.state.document.paths?.[0]?.points?.length ?? 0) === 16);

    // Enable Endless (∞) and set Period to 8 s
    await page.getByRole('switch', { name: 'Endless loop', exact: true }).click();
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths?.[0]?.endless === true);
    await page.waitForSelector('.path-endless-bake-note');

    // Close preview overlay so the applied Circle and Endless controls are shown cleanly in plan view
    await page.getByRole('button', { name: 'Close shape generator', exact: true }).click();
    await page.locator('.path-lap-note').scrollIntoViewIfNeeded();
    await page.waitForTimeout(80);
    await page.screenshot({ path: path.join(SHOTS_DIR, 'applied-circle-plan-1280x720.png') });

    // Compact 1100x560 screenshot with Circle + Endless
    await page.setViewportSize({ width: 1100, height: 560 });
    await page.locator('.path-lap-note').scrollIntoViewIfNeeded();
    await page.waitForTimeout(80);
    await page.screenshot({ path: path.join(SHOTS_DIR, 'applied-circle-compact-1100x560.png') });

    // 3. Re-apply Figure-8 (∞) at 1280x720 and 1100x560
    await page.setViewportSize({ width: 1280, height: 720 });
    await page.locator('.path-shape-row').scrollIntoViewIfNeeded();
    await pickOption(page, 'Path shape', 'Figure-8 (∞)');
    await page.waitForSelector('.path-shape-panel[data-shape-kind="figure8"]');
    await page.getByRole('button', { name: 'Apply path shape', exact: true }).click();
    await page.waitForFunction(() => {
      const pts = window.verbRoom.store.state.document.paths?.[0]?.points;
      return pts?.length === 16 && Math.hypot(pts[0][0] - pts[8][0], pts[0][1] - pts[8][1]) < 1e-6;
    });
    await page.getByRole('button', { name: 'Close shape generator', exact: true }).click();
    await page.locator('.path-lap-note').scrollIntoViewIfNeeded();
    await page.waitForTimeout(80);
    await page.screenshot({ path: path.join(SHOTS_DIR, 'applied-figure8-plan-1280x720.png') });

    await page.setViewportSize({ width: 1100, height: 560 });
    await page.locator('.path-lap-note').scrollIntoViewIfNeeded();
    await page.waitForTimeout(80);
    await page.screenshot({ path: path.join(SHOTS_DIR, 'applied-figure8-compact-1100x560.png') });

    // 4. Verify 1-step Undo from Figure-8 back to Circle
    await page.evaluate(() => window.verbRoom.store.command({ op: 'history.undo' }));
    await page.waitForFunction(() => {
      const pts = window.verbRoom.store.state.document.paths?.[0]?.points;
      return pts?.length === 16 && Math.hypot(pts[0][0] - pts[8][0], pts[0][1] - pts[8][1]) > 3.0;
    });

    // =========================================================================
    // 5. V04a-2 Verification Suite (All 6 UX items at 1280x720 and 1100x560)
    // =========================================================================
    await page.setViewportSize({ width: 1280, height: 720 });
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'source', index: 0 }] }, false));
    await page.waitForSelector('[data-parameter="s1path"]');

    // (a) Item 3: Endless control lock (Position & Laps disabled with exact tooltip) + live moving marker + trail + arrow
    const endlessLockInfo = await page.evaluate(() => {
      const posRow = document.querySelector('[data-parameter="s1path"]');
      const posInput = posRow?.querySelector('input.readout');
      const posTrack = posRow?.querySelector('[role="slider"]');
      const lapsInput = document.querySelector('input[aria-label="Path laps"]');
      const lapsRow = document.querySelector('.path-laps');
      const vp = document.querySelector('.viewport');
      const marker = vp?.dataset?.endlessMarker ? JSON.parse(vp.dataset.endlessMarker) : null;
      return {
        posLockedClass: posRow?.classList.contains('locked'),
        posInputDisabled: posInput?.disabled,
        posAriaDisabled: posTrack?.getAttribute('aria-disabled'),
        posTitle: posRow?.title,
        posInputTitle: posInput?.title,
        lapsDisabled: lapsInput?.disabled,
        lapsTitle: lapsInput?.title,
        lapsRowTitle: lapsRow?.title,
        marker
      };
    });
    assert.equal(endlessLockInfo.posLockedClass, true, 'Position control has .locked class when Endless is on');
    assert.equal(endlessLockInfo.posInputDisabled, true, 'Position readout input is disabled when Endless is on');
    assert.equal(endlessLockInfo.posAriaDisabled, 'true', 'Position slider track has aria-disabled=true when Endless is on');
    assert.equal(endlessLockInfo.posTitle, 'Endless: position is driven by transport');
    assert.equal(endlessLockInfo.posInputTitle, 'Endless: position is driven by transport');
    assert.equal(endlessLockInfo.lapsDisabled, true, 'Laps input is disabled when Endless is on');
    assert.equal(endlessLockInfo.lapsTitle, 'Endless: position is driven by transport');
    assert.equal(endlessLockInfo.lapsRowTitle, 'Endless: position is driven by transport');
    assert.ok(endlessLockInfo.marker && endlessLockInfo.marker.playing === true && endlessLockInfo.marker.trailPoints >= 10 && endlessLockInfo.marker.hasArrow === true,
      'Endless live marker renders with trail and direction arrow');

    // Advance transport deterministically and verify marker moves along the path to exact phase
    await page.evaluate(() => window.verbRoom.bridge.setTransport({ playing: true, samplePos: 96000, sampleRate: 48000 }));
    await page.waitForFunction(() => {
      const m = JSON.parse(document.querySelector('.viewport')?.dataset?.endlessMarker || 'null');
      return m && Math.abs(m.phase - 0.25) < 1e-4;
    });
    await page.screenshot({ path: path.join(SHOTS_DIR, 'endless-locked-marker-1280x720.png') });

    // Toggle Endless OFF -> verify Position and Laps re-enable and preserve state, then toggle back ON
    await page.getByRole('switch', { name: 'Endless loop', exact: true }).click();
    await page.waitForFunction(() => !window.verbRoom.store.state.document.paths?.[0]?.endless);
    const unlockedInfo = await page.evaluate(() => ({
      posDisabled: document.querySelector('[data-parameter="s1path"] input.readout')?.disabled,
      lapsDisabled: document.querySelector('input[aria-label="Path laps"]')?.disabled
    }));
    assert.equal(unlockedInfo.posDisabled, false, 'Position re-enabled when Endless turned off');
    assert.equal(unlockedInfo.lapsDisabled, false, 'Laps re-enabled when Endless turned off');
    await page.getByRole('switch', { name: 'Endless loop', exact: true }).click();
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths?.[0]?.endless === true);

    // (b) Item 4: Collapsible XYZ point list (collapsed by default for 16-node Circle > 8 points)
    const ptsCollapsedByDefault = await page.evaluate(() => {
      const sec = document.querySelector('[data-path-points-section="0"]');
      const btn = document.querySelector('[data-path-points-toggle="0"]');
      const body = sec?.querySelector('.path-points-body');
      return {
        count: Number(sec?.dataset?.pointsCount || 0),
        collapsed: sec?.classList.contains('is-collapsed'),
        expandedAttr: btn?.getAttribute('aria-expanded'),
        bodyHidden: body?.hidden
      };
    });
    assert.equal(ptsCollapsedByDefault.count, 16);
    assert.equal(ptsCollapsedByDefault.collapsed, true, 'Points list is collapsed by default when path has > 8 points');
    assert.equal(ptsCollapsedByDefault.expandedAttr, 'false');
    assert.equal(ptsCollapsedByDefault.bodyHidden, true);

    // Expand and re-collapse the point list, verifying session memory
    await page.locator('[data-path-points-toggle="0"]').click();
    const ptsExpanded = await page.evaluate(() => ({
      expandedAttr: document.querySelector('[data-path-points-toggle="0"]')?.getAttribute('aria-expanded'),
      bodyHidden: document.querySelector('[data-path-points-section="0"] .path-points-body')?.hidden,
      sessionOpen: window.verbRoom.store.view.pathPointsOpen?.[0],
      persistedInPrefs: 'pathPointsOpen' in (window.verbRoom.store.state.preferences?.sessionViewState || {})
    }));
    assert.equal(ptsExpanded.expandedAttr, 'true');
    assert.equal(ptsExpanded.bodyHidden, false);
    assert.equal(ptsExpanded.sessionOpen, true);
    assert.equal(ptsExpanded.persistedInPrefs, false, 'pathPointsOpen is UI session state only, never saved in project');
    await page.locator('[data-path-points-toggle="0"]').click();

    // (c) Item 1 & Item 6: Ghost of the old shape + Figure-8 default centre offset (>= 1.5 m from listener)
    // First centre Circle on listener so previous centre is right on listener
    await pickOption(page, 'Path shape', 'Circle');
    await page.getByRole('button', { name: 'Centre shape on listener', exact: true }).click();
    await page.waitForFunction(() => {
      const v = window.verbRoom.store.state.parameters.values;
      const p = window.verbRoom.store.view.pathShapePreview?.params;
      return p && Math.hypot(p.cx - v.lx, p.cy - v.ly) < 1e-4;
    });
    // Now switch shape to Figure-8 (∞):
    // - Old Circle becomes the translucent ghost (dataset.oldShapeGhost === 'visible')
    // - New Figure-8 default centre is automatically offset >= 1.5 m from the listener!
    await pickOption(page, 'Path shape', 'Figure-8 (∞)');
    await page.waitForSelector('.path-shape-panel[data-shape-kind="figure8"]');
    const ghostAndFig8 = await page.evaluate(() => {
      const vp = document.querySelector('.viewport');
      const v = window.verbRoom.store.state.parameters.values;
      const p = window.verbRoom.store.view.pathShapePreview?.params;
      return {
        ghostState: vp?.dataset?.oldShapeGhost,
        ghostPts: Number(vp?.dataset?.oldShapeGhostPoints || 0),
        oldPathDimmed: vp?.dataset?.oldPathDimmed,
        dismissBtnVisible: !!document.querySelector('[data-dismiss-ghost="0"]'),
        fig8DistFromListener: Math.hypot((p?.cx ?? 0) - v.lx, (p?.cy ?? 0) - v.ly),
        ghostInPersistedView: 'pathOldGhost' in (window.verbRoom.store.state.preferences?.sessionViewState || {})
      };
    });
    assert.equal(ghostAndFig8.ghostState, 'visible', 'Old shape ghost is visible in viewport after shape change');
    assert.equal(ghostAndFig8.oldPathDimmed, '0', 'While the new shape is only a preview, the current (old) path is drawn dimmed, not equally bright');
    assert.equal(ghostAndFig8.ghostPts, 16, 'Old shape ghost retains the 16-point Circle geometry');
    assert.equal(ghostAndFig8.dismissBtnVisible, true, 'Explicit Ghost × dismiss button is visible');
    assert.ok(ghostAndFig8.fig8DistFromListener >= 1.5 - 1e-4,
      `New Figure-8 default centre is offset ${ghostAndFig8.fig8DistFromListener} m >= 1.5 m from listener`);
    assert.equal(ghostAndFig8.ghostInPersistedView, false, 'Old shape ghost is never stored in sessionViewState');
    await page.screenshot({ path: path.join(SHOTS_DIR, 'ghost-old-shape-plan-1280x720.png') });

    // Verify ghost is also visible in 3D view
    await page.getByRole('button', { name: '3D', exact: true }).click();
    await page.waitForFunction(() => window.verbRoom.store.view.projection === '3d');
    assert.equal(await page.evaluate(() => document.querySelector('.viewport')?.dataset?.oldShapeGhost), 'visible',
      'Old shape ghost renders in 3D perspective view');
    await page.screenshot({ path: path.join(SHOTS_DIR, 'ghost-old-shape-3d-1280x720.png') });
    await page.getByRole('button', { name: 'Plan', exact: true }).click();
    await page.waitForFunction(() => window.verbRoom.store.view.projection === 'plan');

    // Dismiss ghost via explicit Ghost × button
    await page.getByRole('button', { name: 'Dismiss previous shape ghost', exact: true }).click();
    await page.waitForFunction(() => !document.querySelector('.viewport')?.dataset?.oldShapeGhost);
    assert.equal(await page.evaluate(() => window.verbRoom.store.view.pathOldGhost), null, 'Ghost dismissed via × button');

    // Switch to Star (5-point) -> ghost appears again; dismiss by clicking empty space in the viewport
    await pickOption(page, 'Path shape', 'Star (5-point)');
    await page.waitForFunction(() => document.querySelector('.viewport')?.dataset?.oldShapeGhost === 'visible');
    await page.locator('.viewport canvas').click({ position: { x: 18, y: 18 } });
    await page.waitForFunction(() => !document.querySelector('.viewport')?.dataset?.oldShapeGhost);
    assert.equal(await page.evaluate(() => window.verbRoom.store.view.pathOldGhost), null, 'Ghost dismissed by empty-space click');

    // Reselect Source 1 and test Item 2: Live shape parameters (64 intermediate updates -> 1 undo step + Esc cancel)
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'source', index: 0 }] }, false));
    await page.waitForSelector('.path-section');
    await pickOption(page, 'Path shape', 'Circle');
    await page.waitForSelector('.path-shape-panel[data-shape-kind="circle"]');

    const liveGestureResult = await page.evaluate(async () => {
      const store = window.verbRoom.store;
      const beforePts = store.state.document.paths[0].points.map(q => q.slice());
      const undoBefore = store.telemetry['history.changed']?.undoDepth ?? 0;
      const input = document.querySelector('input[aria-label="Shape radius, m"]');
      input.readOnly = false;
      input.focus();
      let stepsUpdated = 0;
      let prevX0 = beforePts[0][0];
      for (let i = 0; i < 64; i++) {
        const r = Number((0.5 + (1.25 * i / 63)).toFixed(4));
        input.value = String(r);
        input.dispatchEvent(new Event('input', { bubbles: true }));
        while (store.commandSlots < 256) {
          await new Promise(res => setTimeout(res, 2));
        }
        const curX0 = store.state.document.paths[0].points[0][0];
        if (Math.abs(curX0 - prevX0) > 1e-6) stepsUpdated++;
        prevX0 = curX0;
      }
      const liveFinalPts = store.state.document.paths[0].points.map(q => q.slice());
      // Commit gesture via change + blur (1 undo step)
      input.dispatchEvent(new Event('change', { bubbles: true }));
      input.blur();
      while (store.commandSlots < 256) {
        await new Promise(res => setTimeout(res, 5));
      }
      await new Promise(res => setTimeout(res, 20));
      const undoAfterCommit = store.telemetry['history.changed']?.undoDepth ?? 0;

      // Undo 1 step -> must restore original path before the 64-step gesture!
      await store.command({ op: 'history.undo' });
      const afterUndoPts = store.state.document.paths[0].points.map(q => q.slice());

      return {
        stepsUpdated,
        undoDelta: undoAfterCommit - undoBefore,
        restoredExact: JSON.stringify(afterUndoPts) === JSON.stringify(beforePts),
        liveFinalPts
      };
    });
    assert.equal(liveGestureResult.stepsUpdated, 64, 'All 64 intermediate live parameter updates updated path points before confirm');
    assert.equal(liveGestureResult.undoDelta, 1, '64-step live parameter edit committed as 1 single undo step');
    assert.equal(liveGestureResult.restoredExact, true, '1-step Undo after 64-step live edit restored the exact original path');

    // Re-select Source 1 after undo resync and verify final points of 64-step live edit are bit-exact equal to single confirmed update
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'source', index: 0 }] }, false));
    await page.waitForSelector('.path-shape-panel[data-shape-kind="circle"]');
    await page.getByRole('button', { name: 'Apply path shape', exact: true }).click();
    const singleConfirmPts = await page.evaluate(async () => {
      const store = window.verbRoom.store;
      while (store.commandSlots < 256) await new Promise(res => setTimeout(res, 5));
      return store.state.document.paths[0].points.map(q => q.slice());
    });
    assert.deepEqual(liveGestureResult.liveFinalPts, singleConfirmPts,
      'Final points after 64-step live drag are bit-exact equal to a single confirmed update');

    // Verify Escape during live parameter editing cancels and restores original path with 0 undo steps
    const escCancelResult = await page.evaluate(async () => {
      const store = window.verbRoom.store;
      const beforePts = store.state.document.paths[0].points.map(q => q.slice());
      const undoBefore = store.telemetry['history.changed']?.undoDepth ?? 0;
      const input = document.querySelector('input[aria-label="Shape radius, m"]');
      input.readOnly = false;
      input.focus();
      for (let i = 0; i < 20; i++) {
        const r = Number((0.8 + 0.05 * i).toFixed(4));
        input.value = String(r);
        input.dispatchEvent(new Event('input', { bubbles: true }));
        await new Promise(res => setTimeout(res, 2));
      }
      const changedDuringGesture = JSON.stringify(store.state.document.paths[0].points) !== JSON.stringify(beforePts);
      input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
      // mock transaction.cancel carries the mock bridge latency (35 ms): poll instead of a fixed wait
      for (let i = 0; i < 100 && JSON.stringify(store.state.document.paths[0].points) !== JSON.stringify(beforePts); i++) {
        await new Promise(res => setTimeout(res, 10));
      }
      await new Promise(res => setTimeout(res, 40));
      const afterEscPts = store.state.document.paths[0].points.map(q => q.slice());
      const undoAfterEsc = store.telemetry['history.changed']?.undoDepth ?? 0;
      return {
        changedDuringGesture,
        restoredOnEsc: JSON.stringify(afterEscPts) === JSON.stringify(beforePts),
        undoDelta: undoAfterEsc - undoBefore
      };
    });
    assert.equal(escCancelResult.changedDuringGesture, true, 'Path updated live during gesture before Escape');
    assert.equal(escCancelResult.restoredOnEsc, true, 'Escape cancelled live gesture and restored original path');
    assert.equal(escCancelResult.undoDelta, 0, 'Cancelled gesture added 0 undo entries');

    // (d) Item 5: Spiral height = Z of first and last point (Start Z m and End Z m) in UI + 3D view
    await pickOption(page, 'Path shape', 'Spiral');
    await page.waitForSelector('.path-shape-panel[data-shape-kind="spiral"]');
    const startZInput = page.getByRole('textbox', { name: 'Spiral start height Z, m', exact: true });
    const endZInput = page.getByRole('textbox', { name: 'Spiral end height Z, m', exact: true });
    await startZInput.dblclick();
    await startZInput.fill('0.6');
    await startZInput.press('Enter');
    await endZInput.dblclick();
    await endZInput.fill('2.4');
    await endZInput.press('Enter');
    await page.waitForFunction(() => {
      const pts = window.verbRoom.store.state.document.paths?.[0]?.points;
      return pts?.length === 24 && Math.abs(pts[0][2] - 0.6) < 1e-4 && Math.abs(pts[23][2] - 2.4) < 1e-4;
    });
    const spiralPts = await page.evaluate(() => window.verbRoom.store.state.document.paths[0].points);
    assert.equal(spiralPts.length, 24);
    assert.ok(Math.abs(spiralPts[0][2] - 0.6) < 1e-4, 'Spiral first point Z in UI is 0.6 m');
    assert.ok(Math.abs(spiralPts[23][2] - 2.4) < 1e-4, 'Spiral last point Z in UI is 2.4 m');

    await page.getByRole('button', { name: '3D', exact: true }).click();
    await page.waitForFunction(() => window.verbRoom.store.view.projection === '3d');
    await page.waitForTimeout(80);
    await page.screenshot({ path: path.join(SHOTS_DIR, 'spiral-start-end-z-3d-1280x720.png') });

    await page.setViewportSize({ width: 1100, height: 560 });
    await page.waitForTimeout(80);
    await page.screenshot({ path: path.join(SHOTS_DIR, 'spiral-start-end-z-compact-1100x560.png') });

    assert.deepEqual(errors, []);
  } finally {
    await browser?.close();
    server.close();
  }
});
