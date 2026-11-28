// V04d: Link to path (rig). Geometry mirrors MovementPath.h (followerPoseAt / followerToOwnPath), mock commands mirror
// SceneCommands.cpp (path.link / path.setFollow / path.unlink, no chains, delete-target policy, one undo step), and the
// browser UI: inspector button + picker (multi-select), follower panel (target, jump, F/L/U offsets with live gesture,
// Esc cancel, Follow wander, Unlink, locked controls) and the viewport rig drawing (offset ghost, dashed link, Paths toggle).
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
import {
  compilePath, followerPose, followerCurve, followerToOwnPath, followOffsetDelta, followYawAtSingleLap, followersOf,
  canBeFollowTarget, isFollower, resolveFollowPath, withPathPoses, pathArcPoint
} from '../src/viewport/movement-path.js';

const require = createRequire(import.meta.url);
let playwright;
for (const name of [process.env.PLAYWRIGHT_MODULE, 'playwright', 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'].filter(Boolean)) {
  try { playwright = require(name); break; } catch {}
}
const CHROME = process.env.CHROME_PATH || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const __dirname = path.dirname(fileURLToPath(import.meta.url));
// Screenshots never go to web-ui/shots: default out/V04d/shots, override with VERB_V04D_SHOTS_DIR.
const SHOTS_DIR = process.env.VERB_V04D_SHOTS_DIR || path.resolve(__dirname, '../../out/V04d/shots');

const circle = (cx, cy, r, z, n = 16) => Array.from({ length: n }, (_, k) => [cx + r * Math.cos(2 * Math.PI * k / n), cy + r * Math.sin(2 * Math.PI * k / n), z]);
const follower = (target, offset, followWander = true) => ({ points: [], enabled: true, closed: false, smooth: false, orient: false, laps: 1, follow: { target, offset, followWander, frame: 'path' } });

test('V04d path frame: forward = horizontal tangent, left = horizontal perpendicular, up = world Z', () => {
  assert.deepEqual(followOffsetDelta(0, [1, 0, 0]).map(v => +v.toFixed(12)), [1, 0, 0]);
  assert.deepEqual(followOffsetDelta(0, [0, 1, 0]).map(v => +v.toFixed(12)), [0, 1, 0]);
  assert.deepEqual(followOffsetDelta(90, [1, 0, 0]).map(v => +v.toFixed(12)), [0, 1, 0]);
  assert.deepEqual(followOffsetDelta(90, [0, 1, 0]).map(v => +v.toFixed(12)), [-1, 0, 0]);
  assert.deepEqual(followOffsetDelta(37, [0, 0, 0.4]).map(v => +v.toFixed(12)), [0, 0, 0.4]);
  // Circle, counter-clockwise: Left points to the centre, Up is world Z (the same radius rule as V04dTest.h).
  const paths = [{ points: circle(3, 4, 2, 1.5), enabled: true, closed: true, smooth: true, orient: false, laps: 1 }, follower(0, [0, 0.5, 0.3])];
  let maxErr = 0;
  for (let k = 0; k <= 200; k++) {
    const u = k / 200, fp = followerPose(paths, 1, u, 0, 3);
    const rT = Math.hypot(fp.base[0] - 3, fp.base[1] - 4), rF = Math.hypot(fp.pos[0] - 3, fp.pos[1] - 4);
    maxErr = Math.max(maxErr, Math.abs(rF - (rT - 0.5)), Math.abs(fp.pos[2] - 1.8));
  }
  assert.ok(maxErr < 3e-3, `circle radius error ${maxErr}`);
  // Vertical segment holds the previous heading (no NaN, no flip); a stop on a vertex holds the incoming heading.
  const climb = { points: [[1, 1, 1], [3, 1, 1], [3, 1, 2.5], [3, 3, 2.5]], enabled: true, closed: false, smooth: false, orient: false, laps: 1 };
  const t = compilePath(climb);
  const fracAtVertex2 = t.lengths[2] / t.length;
  assert.equal(followYawAtSingleLap(climb, fracAtVertex2 - 1e-6), 0);
  assert.equal(followYawAtSingleLap(climb, (t.lengths[1] + t.lengths[2]) / 2 / t.length), 0, 'vertical segment holds the heading');
  assert.equal(Math.round(followYawAtSingleLap(climb, 0.99)), 90);
  // Clamp: z stays in 0..zTop.
  const low = [climb, follower(0, [0, 0, -5])];
  assert.equal(followerPose(low, 1, 0.1, 0, 3).pos[2], 0);
  const high = [climb, follower(0, [0, 0, 5])];
  assert.equal(followerPose(high, 1, 0.9, 0, 3).pos[2], 3);
});

test('V04d model helpers: resolve, followers, usable targets, Unlink sampling', () => {
  const tgt = { points: circle(3, 4, 2, 1.5), enabled: true, closed: true, smooth: true, orient: true, laps: 3, endless: true, endlessPeriodS: 6, lfo: { enabled: true, shape: 'sine', rateHz: 0.5, depth: 0.1, phaseDeg: 0, seed: 1 }, wander: { enabled: true, ampHorizM: 0.4, ampVertM: 0.1, rateHz: 0.5, smoothness: 0.8, pinEnds: true, horizOnly: false, seed: 7 } };
  const paths = [tgt, follower(0, [0.1, 0.2, 0.3]), follower(0, [0, 0, -1], false), { points: [], enabled: true, closed: false, smooth: false, orient: false, laps: 1 }];
  assert.deepEqual(followersOf(paths, 0), [1, 2]);
  assert.equal(canBeFollowTarget(paths[0]), true);
  assert.equal(canBeFollowTarget(paths[1]), false, 'a follower is never a target (no chains)');
  assert.equal(canBeFollowTarget(paths[3]), false, 'no points');
  const r1 = resolveFollowPath(paths, 1), r2 = resolveFollowPath(paths, 2);
  assert.equal(r1.points, tgt.points);
  assert.equal(r1.laps, 3); assert.equal(r1.endless, true); assert.equal(r1.lfo, tgt.lfo); assert.equal(r1.wander, tgt.wander); assert.equal(r1.group, -1);
  assert.equal(r2.wander, undefined, 'Follow wander OFF');
  assert.equal(resolveFollowPath(paths, 1), r1, 'cached per (follower, target) object');
  const own = followerToOwnPath(paths, 1, 3);
  assert.equal(own.points.length, 64); assert.equal(own.closed, true); assert.equal(own.smooth, true);
  assert.equal(own.laps, 3); assert.equal(own.endless, true); assert.equal(own.endlessPeriodS, 6); assert.ok(own.lfo && own.wander);
  assert.equal(isFollower(own), false);
  assert.equal(followerToOwnPath(paths, 2, 3).wander, undefined);
  // The own path runs within ~1 cm of the follower's offset curve (16-node circle, smooth).
  let worst = 0;
  for (let k = 0; k < 64; k++) {
    const fr = k / 64, q = pathArcPoint(r1, fr), d = followOffsetDelta(followYawAtSingleLap(r1, fr), [0.1, 0.2, 0.3]);
    worst = Math.max(worst, Math.hypot(own.points[k][0] - q[0] - d[0], own.points[k][1] - q[1] - d[1], own.points[k][2] - q[2] - d[2]));
  }
  assert.ok(worst < 1e-9, `own points = offset curve samples (${worst})`);
  assert.ok(followerCurve(paths, 1, 96, 3).length >= 64);
});

test('V04d mock: path.link (multi-select) / setFollow gesture / Esc / unlink / no chains / locked controls / delete-target policy', async () => {
  const bridge = await createMockBridge({ read: readFixture, validator, fixture: 'rig-walk', delayMs: 0 });
  const store = createStore(bridge);
  try {
    await store.start();
    const hist = () => store.telemetry['history.changed'] || { undoDepth: 0, undoLabel: '' };
    const doc = () => store.state.document;
    assert.deepEqual(followersOf(doc().paths, 0), [1, 2, 3]);
    // Fixture poses (u = 0 on the oval: heading nearly +Y): Head 5 cm ahead + 35 cm up, Hands 30 cm to the left (-X), Feet on the floor.
    const v = withPathPoses(store.state).parameters.values;
    assert.ok(Math.abs(v.s2y - 4.05) < 0.001 && Math.abs(v.s2z - 1.65) < 1e-9);
    assert.ok(Math.abs(v.s3x - 4.3) < 0.001 && Math.abs(v.s3z - 1.0) < 1e-9);
    assert.ok(Math.abs(v.s4z - 0.1) < 1e-9);
    const farOffset = structuredClone(store.state);
    farOffset.document.paths[1].follow.offset = [0, -10, 8];
    const clamped = withPathPoses(farOffset).parameters.values;
    assert.ok(clamped.s2x < 6 && clamped.s2z < farOffset.document.scene.heightM,
              'the preview stops at the wall and ceiling');
    // Offset gesture: transaction with live steps = one undo step; cancel restores.
    const d0 = hist().undoDepth;
    const tx = 'tx-link-1';
    assert.equal((await store.command({ op: 'transaction.begin', label: 'Edit link offset' }, tx)).kind, 'ack');
    for (let k = 1; k <= 10; k++) await store.command({ op: 'path.setFollow', object: 1, offset: [0.05, 0, 0.35 + 0.01 * k] }, tx);
    assert.ok(Math.abs(doc().paths[1].follow.offset[2] - 0.45) < 1e-12, 'live update inside the gesture');
    await store.command({ op: 'transaction.commit' }, tx);
    assert.equal(hist().undoDepth, d0 + 1); assert.equal(hist().undoLabel, 'Edit link offset');
    const before = JSON.stringify(doc().paths[2]);
    await store.command({ op: 'transaction.begin', label: 'Edit link offset' }, 'tx-link-2');
    await store.command({ op: 'path.setFollow', object: 2, offset: [0, -0.4, -0.3] }, 'tx-link-2');
    await store.command({ op: 'transaction.cancel' }, 'tx-link-2');
    assert.equal(JSON.stringify(doc().paths[2]), before, 'Esc restores the offset');
    assert.equal(hist().undoDepth, d0 + 1);
    // No chains, self link, bad offsets, locked controls.
    assert.match((await store.command({ op: 'path.link', object: 0, target: 1 })).payload.message, /no chains/);
    assert.equal((await store.command({ op: 'path.link', object: 1, target: 1 })).kind, 'error');
    await assert.rejects(store.command({ op: 'path.setFollow', object: 1, offset: [0, 0, 51] }), /Contract validation/);
    assert.match((await store.command({ op: 'path.setFlags', object: 3, laps: 2 })).payload.message, /unlink first/);
    assert.match((await store.command({ op: 'path.setLfo', object: 3, lfo: { enabled: true, shape: 'sine', rateHz: 1, depth: 0.2, phaseDeg: 0, seed: 1 } })).payload.message, /unlink first/);
    assert.equal((await store.command({ op: 'path.setFlags', object: 3, orient: false })).kind, 'ack', 'orientation stays editable');
    assert.equal((await store.command({ op: 'path.setFollow', object: 3, followWander: true })).kind, 'ack');
    assert.equal(hist().undoLabel, 'Follow wander');
    // Unlink -> own 64-point path; undo restores the link.
    await store.command({ op: 'path.unlink', object: 2 });
    assert.equal(doc().paths[2].points.length, 64); assert.equal(isFollower(doc().paths[2]), false); assert.equal(hist().undoLabel, 'Unlink path');
    await store.command({ op: 'history.undo' });
    assert.equal(doc().paths[2].follow.target, 0);
    // Re-link with multi-select: one step.
    await store.command({ op: 'path.unlink', object: 2 });
    await store.command({ op: 'path.unlink', object: 3 });
    const d1 = hist().undoDepth;
    assert.equal((await store.command({ op: 'path.link', objects: [2, 3], target: 0, offset: [0, 0, -0.5] })).kind, 'ack');
    assert.equal(hist().undoDepth, d1 + 1); assert.equal(hist().undoLabel, 'Link 2 sources to path');
    assert.deepEqual(followersOf(doc().paths, 0), [1, 2, 3]);
    // Delete-target policy: clearing / removing the target keeps every follower's motion as an own path (one undo step).
    const d2 = hist().undoDepth;
    await store.command({ op: 'path.clear', object: 0 });
    assert.equal(hist().undoDepth, d2 + 1);
    assert.deepEqual([1, 2, 3].map(o => doc().paths[o].points.length), [64, 64, 64]);
    await store.command({ op: 'history.undo' });
    assert.deepEqual([1, 2, 3].map(o => doc().paths[o].follow?.target), [0, 0, 0]);
    await store.command({ op: 'source.remove', index: 0 });
    assert.equal(doc().sources.length, 3);
    assert.deepEqual([0, 1, 2].map(o => isFollower(doc().paths[o])), [false, false, false]);
    await store.command({ op: 'history.undo' });
    assert.deepEqual(followersOf(doc().paths, 0), [1, 2, 3]);
    // Remove a source before nothing / after the target: indexes shift with the sources.
    await store.command({ op: 'source.remove', index: 1 });
    assert.deepEqual(followersOf(doc().paths, 0), [1, 2]);
    await store.command({ op: 'history.undo' });
    // Room scale with objects scales offsets.
    const offset = doc().paths[2].follow.offset.slice();
    assert.equal((await store.command({ op: 'room.scale', factor: 2, includeObjects: true })).kind, 'ack');
    assert.deepEqual(doc().paths[2].follow.offset, offset.map(value => value * 2));
    await store.command({ op: 'history.undo' });
  } finally { store.dispose(); bridge.dispose(); }
});

test('V04d Browser UI: Link to path… picker (multi-select), follower panel, live offset + Esc, Unlink, viewport rig drawing, screenshots', { timeout: 120000 }, async t => {
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
    await page.goto(`${url}/?fixture=rig-walk&dev=0`);
    await page.waitForFunction(() => window.verbRoom?.store?.ready);
    const shot = async name => { await page.waitForTimeout(150); await page.screenshot({ path: path.join(SHOTS_DIR, name) }); };
    const vp = 'section.viewport';
    const undoInfo = () => page.evaluate(() => ({ depth: window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0, label: window.verbRoom.store.telemetry['history.changed']?.undoLabel ?? '' }));
    const paths = () => page.evaluate(() => window.verbRoom.store.state.document.paths);
    const rig = () => page.evaluate(sel => JSON.parse(document.querySelector(sel)?.dataset.rigLinks || '[]'), vp);

    // Viewport: 3 followers drawn (offset ghost + dashed link) in plan and 3D.
    await page.evaluate(() => window.verbRoom.store.setView({ projection: 'plan', selection: [{ kind: 'source', index: 0 }] }, false));
    await page.waitForFunction(sel => JSON.parse(document.querySelector(sel)?.dataset.rigLinks || '[]').length === 3, vp);
    const r0 = await rig();
    assert.deepEqual(r0.map(x => [x.follower, x.target]), [[1, 0], [2, 0], [3, 0]]);
    assert.ok(r0.every(x => x.curve >= 12 && Array.isArray(x.link)));
    await page.waitForSelector('[data-link-followed="0"]');
    assert.match(await page.textContent('[data-link-followed="0"]'), /Head, Hands, Feet/);
    await shot('link-target-plan-1280x720.png');
    await page.evaluate(async () => {
      const { store, bridge } = window.verbRoom;
      await store.command({ op: 'path.setFlags', object: 0, endless: true, endlessPeriodS: 8 });
      bridge.setTransport({ playing: true, timeSec: 2 });
    });
    await page.waitForFunction(({ vp, original }) => {
      const current = JSON.parse(document.querySelector(vp)?.dataset.rigLinks || '[]');
      return current.length === 3 && current.every((link, i) =>
        link.link?.[0] !== original[i].link?.[0] && link.link?.[2] !== original[i].link?.[2]);
    }, { vp, original: r0 });
    await page.evaluate(() => window.verbRoom.store.setView({ projection: '3d' }, false));
    await page.waitForFunction(sel => JSON.parse(document.querySelector(sel)?.dataset.rigLinks || '[]').length === 3, vp);
    await shot('link-rig-3d-1280x720.png');
    // Paths toggle hides the rig drawing unless the follower or its target is selected.
    await page.evaluate(() => window.verbRoom.store.setView({ projection: 'plan', selection: [] }, false));
    await page.evaluate(() => window.verbRoom.store.command({ op: 'preferences.set', patch: { showPaths: false } }));
    await page.waitForFunction(sel => !document.querySelector(sel)?.dataset.rigLinks, vp);
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'source', index: 2 }] }, false));
    await page.waitForFunction(sel => JSON.parse(document.querySelector(sel)?.dataset.rigLinks || '[]').length === 1, vp);
    await page.evaluate(() => window.verbRoom.store.command({ op: 'preferences.set', patch: { showPaths: true } }));
    await page.waitForFunction(sel => JSON.parse(document.querySelector(sel)?.dataset.rigLinks || '[]').length === 3, vp);

    // Follower panel (Hands): target row, F/L/U offsets, Follow wander, locked controls, bake note.
    await page.waitForSelector('[data-link-follower="2"]');
    for (const name of ['Position', 'Laps', 'Endless', 'Path LFO', 'Path wander', 'Coupling']) assert.ok(await page.$(`[data-link-locked="${name}"]`), name);
    assert.equal(await page.$('.path-draw-btn'), null, 'no Draw on a follower');
    assert.equal(await page.$('[data-path-link="2"]'), null, 'no Link button on a follower');
    await page.locator('[data-link-follower="2"]').evaluate(el => el.scrollIntoView({ block: 'start' }));
    await shot('link-follower-panel-1280x720.png');
    // Live typed offset: Left 0.30 -> 0.55 (one undo step, live update while typing).
    const d0 = (await undoInfo()).depth;
    const left = page.getByRole('textbox', { name: 'Offset Left, m', exact: true });
    await left.dblclick();
    await left.fill('0.55');
    await page.waitForFunction(() => Math.abs(window.verbRoom.store.state.document.paths[2].follow.offset[1] - 0.55) < 1e-9);
    await left.press('Enter');
    await page.waitForFunction(d => (window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0) === d + 1, d0);
    assert.equal((await undoInfo()).label, 'Edit link offset');
    // Esc cancels a typed gesture.
    const up = page.getByRole('textbox', { name: 'Offset Up, m', exact: true });
    await up.dblclick();
    await up.fill('-0.9');
    await page.waitForFunction(() => Math.abs(window.verbRoom.store.state.document.paths[2].follow.offset[2] + 0.9) < 1e-9);
    await up.press('Escape');
    await page.waitForFunction(() => Math.abs(window.verbRoom.store.state.document.paths[2].follow.offset[2] + 0.3) < 1e-9);
    assert.equal((await undoInfo()).depth, d0 + 1, 'cancelled gesture adds no undo step');
    // Vertical drag on Forward = one undo step.
    const fwd = page.getByRole('textbox', { name: 'Offset Forward, m', exact: true });
    const box = await fwd.boundingBox();
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    await page.mouse.down();
    for (let k = 1; k <= 8; k++) await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2 - 4 * k);
    await page.mouse.up();
    await page.waitForFunction(d => (window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0) === d + 2, d0);
    assert.ok((await paths())[2].follow.offset[0] > 0.15, 'drag up increases Forward');
    // Follow wander switch.
    await page.getByRole('switch', { name: 'Follow wander', exact: true }).click();
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[2].follow.followWander === false);
    // Jump to the target.
    await page.click('[data-link-jump="2"]');
    await page.waitForFunction(() => window.verbRoom.store.view.selection[0]?.index === 0);
    await page.waitForSelector('[data-link-followed="0"]');

    // Unlink Feet -> own path; undo -> follower again.
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'source', index: 3 }] }, false));
    await page.waitForSelector('[data-link-unlink="3"]');
    await page.click('[data-link-unlink="3"]');
    await page.waitForFunction(() => (window.verbRoom.store.state.document.paths[3].points?.length ?? 0) === 64);
    await page.waitForSelector('.path-draw-btn');
    assert.equal((await undoInfo()).label, 'Unlink path');
    await shot('link-unlinked-feet-1280x720.png');

    // Link Feet again with the picker, multi-select Hands as well (Hands is a follower already -> re-link replaces it).
    await page.click('[data-path-link="3"]');
    await page.waitForSelector('[data-link-picker="3"]');
    assert.ok(await page.$('[data-link-target="0"]'), 'Body is offered as target');
    assert.equal(await page.$('[data-link-target="1"]'), null, 'followers are never offered as targets (no chains)');
    await shot('link-picker-1280x720.png');
    await page.click('[data-link-extra="2"]');
    const d1 = (await undoInfo()).depth;
    await page.click('[data-link-apply="3"]');
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[3].follow?.target === 0);
    const u1 = await undoInfo();
    assert.equal(u1.depth, d1 + 1); assert.equal(u1.label, 'Link 2 sources to path');
    assert.deepEqual((await paths())[2].follow.offset, [0, 0, 0], 'multi-select links with offset 0 (set it afterwards)');

    // Delete-target policy in the UI: Clear on Body keeps followers moving as own paths; one undo restores the rig.
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'source', index: 0 }] }, false));
    await page.getByRole('button', { name: 'Clear path', exact: true }).click();
    await page.waitForFunction(() => [1, 2, 3].every(o => (window.verbRoom.store.state.document.paths[o].points?.length ?? 0) === 64));
    await page.evaluate(() => window.verbRoom.store.command({ op: 'history.undo' }));
    await page.waitForFunction(() => [1, 2, 3].every(o => window.verbRoom.store.state.document.paths[o].follow?.target === 0));

    // Compact viewports.
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'source', index: 1 }], projection: 'plan' }, false));
    await page.waitForSelector('[data-link-follower="1"]');
    await page.setViewportSize({ width: 1100, height: 560 });
    await page.locator('[data-link-follower="1"]').evaluate(el => el.scrollIntoView({ block: 'start' }));
    await shot('link-follower-compact-1100x560.png');
    await page.evaluate(() => window.verbRoom.store.setView({ projection: '3d' }, false));
    await shot('link-rig-3d-compact-1100x560.png');
    await page.setViewportSize({ width: 1280, height: 720 });
    await page.evaluate(() => window.verbRoom.store.setView({ projection: 'plan' }, false));
    await shot('link-follower-plan-1280x720.png');
    // Edit page: same inspector.
    await page.evaluate(() => window.verbRoom.store.setView({ page: 'edit', selection: [{ kind: 'source', index: 2 }] }, false));
    await page.waitForSelector('[data-link-follower="2"]');
    await shot('link-edit-page-1280x720.png');
    assert.deepEqual(errors, []);
  } finally {
    await browser?.close();
    server.close();
  }
});
