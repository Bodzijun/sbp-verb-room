import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import test from 'node:test';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { serve } from '../scripts/serve.mjs';
import { pickOption } from './browser-helpers.mjs';
import { validator, readFixture } from './helpers.mjs';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';
import { normalizeRoute, computeModulationTelemetry, MOD_DRIVERS } from '../src/components/modulation-model.js';
import { zoneOutline, polygonSimple, zoneSignedDistance, zoneSoftValue, zoneDriverRaw, computeZoneOccupancy, zoneProblem, ZONE_FAR } from '../src/state/trigger-zones.js';

const require = createRequire(import.meta.url);
let playwright;
for (const name of [process.env.PLAYWRIGHT_MODULE, 'playwright', 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'].filter(Boolean)) {
  try { playwright = require(name); break; } catch {}
}
const CHROME = process.env.CHROME_PATH || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const __dirname = path.dirname(fileURLToPath(import.meta.url));
// Screenshots never go to web-ui/shots: default out/V04c/shots, override with VERB_V04C_SHOTS_DIR.
const SHOTS_DIR = process.env.VERB_V04C_SHOTS_DIR || path.resolve(__dirname, '../../out/V04c/shots');

test('V04c zone geometry mirrors the processor (prism distance, soft edge, simple polygon, drivers)', () => {
  const box = { id: 1, kind: 'box', points: [[4, 3], [2, 1]], zMin: 0, zMax: 3, edge: 0, hysteresis: 0.05, enabled: true };
  assert.deepEqual(zoneOutline(box), [[2, 1], [4, 1], [4, 3], [2, 3]]);
  assert.equal(zoneSignedDistance(box, [3, 2, 1.5]), -1);            // nearest face: the sides 1 m away
  assert.ok(Math.abs(zoneSignedDistance(box, [3, 2, 2.8]) + 0.2) < 1e-12); // top 0.2 m away
  assert.ok(Math.abs(zoneSignedDistance(box, [5, 2, 1.5]) - 1) < 1e-12);
  assert.ok(Math.abs(zoneSignedDistance(box, [5, 4, 4]) - Math.sqrt(3)) < 1e-12);
  const soft = { ...box, edge: 0.4 };
  assert.equal(zoneSoftValue(soft, 0), 0);
  assert.equal(zoneSoftValue(soft, -0.1), 0.15625);                  // same as V04cTest (edge / 4)
  assert.equal(zoneSoftValue(soft, -0.2), 0.5);
  assert.equal(zoneSoftValue(soft, -0.4), 1);
  assert.equal(polygonSimple([[0, 0], [2, 0], [2, 2], [1, 1], [0, 2]]), true);   // concave is fine
  assert.equal(polygonSimple([[0.5, 0.5], [2, 2], [2, 0.5], [0.5, 2]]), false);  // bow-tie
  assert.match(zoneProblem({ ...box, kind: 'polygon', points: [[0.5, 0.5], [2, 2], [2, 0.5], [0.5, 2]] }), /crosses itself/);
  const poly = { id: 2, kind: 'polygon', points: [[5, 1], [7, 1], [7, 3]], zMin: 0, zMax: 3, edge: 0, hysteresis: 0.05, enabled: true };
  const zones = [box, poly];
  assert.equal(zoneDriverRaw({ driver: 'inZone', zoneId: 1 }, zones, [3, 2, 1]), 1);
  assert.equal(zoneDriverRaw({ driver: 'inZone', zoneId: 2 }, zones, [3, 2, 1]), 0);
  assert.equal(zoneDriverRaw({ driver: 'zoneIndex' }, zones, [6.5, 1.5, 1]), 2);
  assert.equal(zoneDriverRaw({ driver: 'zoneIndex' }, zones, [0, 0, 1]), 0);
  assert.equal(zoneDriverRaw({ driver: 'distanceToZone', zoneId: 9 }, zones, [0, 0, 1]), ZONE_FAR);
  assert.equal(zoneDriverRaw({ driver: 'inZone', zoneId: 1 }, [{ ...box, enabled: false }], [3, 2, 1]), 0);
  assert.deepEqual(computeZoneOccupancy(zones, { s1x: 3, s1y: 2, s1z: 1, lx: 6.5, ly: 1.5, lz: 1 }, 1), [{ id: 1, objects: [0] }, { id: 2, objects: [32] }]);
  // route model: the three drivers exist; zoneId kept for In zone / Distance to zone only
  for (const id of ['inZone', 'zoneIndex', 'distanceToZone']) assert.ok(MOD_DRIVERS.some(d => d.id === id), id);
  assert.equal(normalizeRoute({ object: 0, driver: 'inZone', zoneId: 7, target: 'none', hostOut: 1 }, 1).zoneId, 7);
  assert.equal('zoneId' in normalizeRoute({ object: 0, driver: 'zoneIndex', zoneId: 7, target: 'none' }, 1), false);
  assert.equal('zoneId' in normalizeRoute({ object: 0, driver: 'x', zoneId: 7, target: 'none' }, 1), false);
});

test('V04c mock: zone.* commands, schema validation, gesture = one undo step, cancel, rejections, occupancy telemetry', async () => {
  const bridge = await createMockBridge({ read: readFixture, validator, fixture: 'zones-creative', delayMs: 0 });
  const store = createStore(bridge);
  try {
    await store.start();
    const zones = () => store.state.document.zones || [];
    assert.equal(zones().length, 2);
    assert.deepEqual(zones().map(z => z.name), ['Stage', 'Tiles']);
    // Creative fixture: In zone route on Source 1 reads 1 (Source 1 at 3,6 inside Stage); listener outside every zone.
    const tel = computeModulationTelemetry(store.state, 0);
    assert.equal(tel.previews[0].raw, 1);
    assert.equal(tel.previews[1].raw, 0);
    const created = await store.command({ op: 'zone.create', kind: 'box', points: [[1, 4], [2.5, 7.5]], name: 'Door' });
    assert.equal(created.kind, 'ack');
    assert.equal(created.payload.details.zoneId, 3);
    assert.equal(zones()[2].hysteresis, 0.05);
    assert.equal(zones()[2].zMax, store.state.document.scene.heightM);
    const depth = () => store.history?.undoDepth ?? store.telemetry['history.changed']?.undoDepth;
    const d0 = depth();
    await store.command({ op: 'transaction.begin', label: 'Move zone vertex' }, 'tx-z');
    for (let k = 1; k <= 12; k++) await store.command({ op: 'zone.set', id: 3, points: [[1, 4], [2.5 + 0.05 * k, 7.5]] }, 'tx-z');
    assert.equal(zones()[2].points[1][0], 3.1);                          // live while the gesture runs
    await store.command({ op: 'transaction.commit' }, 'tx-z');
    assert.equal(depth(), d0 + 1, 'one undo step per gesture');
    await store.command({ op: 'transaction.begin', label: 'Move zone' }, 'tx-c');
    await store.command({ op: 'zone.set', id: 3, points: [[0.5, 4], [2, 7.5]] }, 'tx-c');
    await store.command({ op: 'transaction.cancel' }, 'tx-c');
    assert.deepEqual(zones()[2].points, [[1, 4], [3.1, 7.5]], 'cancel restores');
    assert.equal(depth(), d0 + 1, 'cancel adds no step');
    await store.command({ op: 'history.undo' });
    assert.deepEqual(zones()[2].points, [[1, 4], [2.5, 7.5]], 'one undo reverts the whole drag');
    const bow = await store.command({ op: 'zone.create', kind: 'polygon', points: [[0.5, 0.5], [2, 2], [2, 0.5], [0.5, 2]] });
    assert.equal(bow.kind, 'error');
    assert.match(bow.payload.message, /crosses itself/);
    const noZone = await store.command({ op: 'modulation.addRoute', route: { object: 0, driver: 'inZone', curve: { inputRange: [0, 1], outputRange: [0, 1], nodes: [{ x: 0, y: 0 }, { x: 1, y: 1 }] }, target: 'none', hostOut: 3 } });
    assert.equal(noZone.kind, 'error');
    for (let k = zones().length; k < 16; k++) assert.equal((await store.command({ op: 'zone.create', kind: 'box', points: [[0.5, 0.5], [1, 1]] })).kind, 'ack');
    const over = await store.command({ op: 'zone.create', kind: 'box', points: [[0.5, 0.5], [1, 1]] });
    assert.equal(over.kind, 'error');
    assert.equal(over.payload.code, 'budgetExceeded');
    assert.equal((await store.command({ op: 'zone.move', id: 2, index: 0 })).kind, 'ack');
    assert.equal(zones()[0].id, 2);
    assert.equal((await store.command({ op: 'zone.delete', id: 1 })).kind, 'ack');
    assert.equal(store.state.document.modulation.routes[0].zoneId, 1, 'routes keep a deleted zone id (read outside)');
    // Occupancy telemetry from the mock status events (stateless emulation of the processor)
    await store.command({ op: 'history.undo' });
    await new Promise(r => setTimeout(r, 30));
    const occ = store.telemetry['status.load']?.zones?.occupancy;
    assert.ok(Array.isArray(occ) && occ.find(o => o.id === 1)?.objects.includes(0), JSON.stringify(occ));
    bridge.audit.filter(a => ['in', 'out'].includes(a.direction)).forEach(a => validator.assert(a.message));
  } finally {
    store.dispose();
    bridge.dispose();
  }
});

test('V04c Browser UI: Zones section, draw box / polygon, live vertex edit, Esc cancel, 1 undo per gesture, lit zone, driver pick, screenshots', { timeout: 90000 }, async t => {
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
    await page.goto(`${url}/?fixture=zones-creative&dev=0`);
    await page.waitForFunction(() => window.verbRoom?.store?.ready);
    const shot = async name => { await page.waitForTimeout(120); await page.screenshot({ path: path.join(SHOTS_DIR, name) }); };
    const vp = 'section.viewport';
    const zones = () => page.evaluate(() => window.verbRoom.store.state.document.zones || []);
    const undoInfo = () => page.evaluate(() => ({ depth: window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0, label: window.verbRoom.store.telemetry['history.changed']?.undoLabel ?? '' }));

    // Main page, Plan: Stage is lit (Source 1 inside), Tiles is not.
    await page.evaluate(() => window.verbRoom.store.setView({ projection: 'plan' }));
    await page.waitForFunction(sel => document.querySelector(sel)?.dataset.zonesLit === '1', vp);
    await shot('zones-main-plan-lit-1280x720.png');
    await page.evaluate(() => window.verbRoom.store.setView({ projection: '3d' }));
    await page.waitForFunction(sel => document.querySelector(sel)?.dataset.zonesDrawn === '2', vp);
    await shot('zones-main-3d-lit-1280x720.png');
    await page.evaluate(() => window.verbRoom.store.setView({ projection: 'front' }));
    await shot('zones-main-front-1280x720.png');

    // Edit page → Scene → Zones
    await page.evaluate(() => window.verbRoom.store.setView({ page: 'edit', projection: 'plan', inspectorTab: 'scene', selection: [] }));
    await page.locator('[data-acc-id="scene.zones"]').click();
    await page.waitForSelector('.zones-panel .zone-item[data-zone-id="1"].lit');
    assert.equal(await page.locator('.zones-panel .zone-item').count(), 2);
    await shot('zones-edit-panel-1280x720.png');

    // Draw a box zone (one undo step 'Add zone'), it becomes the edited zone with handles.
    const before = await undoInfo();
    await page.getByRole('button', { name: 'Draw box zone', exact: true }).click();
    const box = await page.locator(`${vp} canvas`).first().boundingBox();
    const cx = box.x + box.width * 0.42, cy = box.y + box.height * 0.5;
    await page.mouse.move(cx, cy); await page.mouse.down(); await page.mouse.move(cx + 60, cy + 40, { steps: 6 }); await page.mouse.move(cx + 110, cy + 70, { steps: 6 });
    await shot('zones-draw-box-1280x720.png');
    await page.mouse.up();
    await page.waitForFunction(() => (window.verbRoom.store.state.document.zones || []).length === 3);
    const z3 = (await zones())[2];
    assert.equal(z3.kind, 'box');
    let u = await undoInfo();
    assert.equal(u.depth, before.depth + 1);
    assert.equal(u.label, 'Add zone');
    await page.waitForFunction(id => window.verbRoom.store.view.zoneEdit === id, z3.id);
    await page.waitForFunction(sel => document.querySelector(sel).zoneHandleScreens().length === 8, vp);

    // Live vertex edit: drag the 3rd outline vertex; the zone updates during the drag; one undo step after release.
    const handles = await page.evaluate(sel => document.querySelector(sel).zoneHandleScreens(), vp);
    const v2 = handles.find(h => h.kind === 'vertex' && h.index === 2).screen;
    const d1 = (await undoInfo()).depth;
    await page.mouse.move(box.x + v2[0], box.y + v2[1]); await page.mouse.down();
    await page.mouse.move(box.x + v2[0] + 25, box.y + v2[1] + 15, { steps: 5 });
    await page.mouse.move(box.x + v2[0] + 50, box.y + v2[1] + 30, { steps: 5 });
    await page.waitForTimeout(80);
    const during = (await zones())[2];
    assert.notDeepEqual(during.points, z3.points, 'zone geometry follows the drag (live)');
    await shot('zones-vertex-drag-live-1280x720.png');
    await page.mouse.up();
    await page.waitForFunction(d => (window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0) === d, d1 + 1).catch(() => {});
    u = await undoInfo();
    assert.equal(u.depth, d1 + 1, 'vertex drag = one undo step');
    assert.equal(u.label, 'Move zone vertex');
    const afterDrag = (await zones())[2];

    // Esc cancels a running drag: the zone is restored, no undo step.
    const h2 = await page.evaluate(sel => document.querySelector(sel).zoneHandleScreens(), vp);
    const v0 = h2.find(h => h.kind === 'vertex' && h.index === 0).screen;
    await page.mouse.move(box.x + v0[0], box.y + v0[1]); await page.mouse.down();
    await page.mouse.move(box.x + v0[0] - 40, box.y + v0[1] - 20, { steps: 6 });
    await page.waitForTimeout(60);
    await page.keyboard.press('Escape');
    await page.mouse.up();
    await page.waitForTimeout(80);
    assert.deepEqual((await zones())[2].points, afterDrag.points, 'Esc restores the zone');
    assert.equal((await undoInfo()).depth, d1 + 1, 'Esc adds no undo step');

    // Ctrl+Z once undoes the whole vertex drag.
    await page.locator(`${vp} canvas`).first().focus();
    await page.keyboard.press('Control+z');
    await page.waitForFunction(pts => JSON.stringify(window.verbRoom.store.state.document.zones[2].points) === JSON.stringify(pts), z3.points);

    // Polygon tool: click 3 vertices, double-click the 4th → 4-vertex polygon. Esc on a second draft cancels it.
    await page.getByRole('button', { name: 'Draw polygon zone', exact: true }).click();
    const P = [[0.32, 0.2], [0.41, 0.2], [0.41, 0.33], [0.32, 0.36]].map(([fx, fy]) => [box.x + box.width * fx, box.y + box.height * fy]);
    await page.mouse.click(...P[0]); await page.mouse.click(...P[1]); await page.mouse.click(...P[2]);
    await shot('zones-draw-polygon-1280x720.png');
    await page.mouse.dblclick(...P[3]);
    await page.waitForFunction(() => (window.verbRoom.store.state.document.zones || []).length === 4);
    const z4 = (await zones())[3];
    assert.equal(z4.kind, 'polygon');
    assert.equal(z4.points.length, 4);
    await page.getByRole('button', { name: 'Draw polygon zone', exact: true }).click();
    await page.mouse.click(box.x + box.width * 0.6, box.y + box.height * 0.2); await page.mouse.click(box.x + box.width * 0.7, box.y + box.height * 0.25);
    await page.keyboard.press('Escape');
    await page.waitForTimeout(80);
    assert.equal((await zones()).length, 4, 'Esc cancels the polygon draft');
    assert.equal(await page.evaluate(sel => document.querySelector(sel).dataset.draftPoints, vp), '0');

    // Move Source 1 into the new polygon: it lights up in the viewport and in the panel (mock emulates telemetry).
    const c4 = z4.points.reduce((a, q) => [a[0] + q[0] / 4, a[1] + q[1] / 4], [0, 0]);
    await page.evaluate(([x, y]) => { window.verbRoom.bridge.automate('s1x', x); window.verbRoom.bridge.automate('s1y', y); }, c4);
    await page.waitForFunction(([sel, id]) => document.querySelector(sel)?.dataset.zonesLit.split(',').includes(String(id)), [vp, z4.id]);
    await page.waitForSelector(`.zones-panel .zone-item[data-zone-id="${z4.id}"].lit`);
    await page.evaluate(id => window.verbRoom.store.setView({ zoneEdit: id }), z4.id);
    await shot('zones-edit-polygon-lit-1280x720.png');
    await page.evaluate(() => window.verbRoom.store.setView({ projection: '3d' }));
    await shot('zones-edit-3d-lit-1280x720.png');
    await page.evaluate(() => window.verbRoom.store.setView({ projection: 'plan' }));

    // Compact layout
    await page.setViewportSize({ width: 1100, height: 560 });
    await shot('zones-edit-compact-1100x560.png');
    await page.setViewportSize({ width: 1280, height: 720 });

    // Route editor: pick the zone for the In zone route; live readout shows inside/outside.
    await page.evaluate(() => window.verbRoom.store.setView({ page: 'main', tab: 'modulation', modFilter: 'all', modRoute: 0, modEditorOpen: true }));
    await page.waitForSelector('.mod-zone-row');
    await pickOption(page, 'Zone for driver', 'Tiles');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].zoneId === 2);
    await page.waitForSelector('.mod-zone-live');
    await shot('zones-route-editor-1280x720.png');
    await pickOption(page, 'Zone for driver', z4.name);
    await page.waitForFunction(id => window.verbRoom.store.state.document.modulation.routes[0].zoneId === id, z4.id);
    await page.waitForSelector('.mod-zone-live.lit');
    await shot('zones-route-editor-lit-1280x720.png');
    await page.setViewportSize({ width: 1100, height: 560 });
    await shot('zones-route-editor-compact-1100x560.png');

    assert.deepEqual(errors, []);
  } finally {
    await browser?.close();
    server.close();
  }
});
