import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { createRequire } from 'node:module';
import { serve } from '../scripts/serve.mjs';
import { pickOption } from './browser-helpers.mjs';

const require = createRequire(import.meta.url);
const locations = [process.env.PLAYWRIGHT_MODULE, 'playwright', 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'];
let playwright;
for (const location of locations.filter(Boolean)) {
  try { playwright = require(location); break; } catch {}
}
assert.ok(playwright, 'Set PLAYWRIGHT_MODULE to a Playwright installation');
const executablePath = process.env.CHROME_PATH || [
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
].find(existsSync);
const { server, url } = await serve();
let browser;
try {
  browser = await playwright.chromium.launch({ headless: true, executablePath });
  const page = await browser.newPage({ viewport: { width: 1024, height: 660 } });
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(`${url}/?fixture=default-physical#edit`);
  await page.waitForFunction(() => window.verbRoom?.store.ready && document.querySelector('.viewport canvas'));

  const startingWalls = [[[0, 0], [3, 0]], [[3, 0], [3, 3]], [[5.123456789012345, .7543210987654321], [5.5, 3.5]]];
  const captureWalls = () => page.evaluate(() => window.verbRoom.store.state.document.scene.walls.map(({ a, b }) => [a, b]));
  const openScene = async walls => {
    await page.evaluate(async input => {
      const { store } = window.verbRoom;
      await store.command({ op: 'room.clear' });
      for (const [a, b] of input) await store.command({ op: 'surface.create', kind: 'wall', a, b });
      store.setView({ page: 'edit', tool: 'move', projection: 'plan', selection: [{ kind: 'wall', index: 0 }] }, false);
    }, walls);
    await page.waitForFunction(() => window.verbRoom.store.ready && window.verbRoom.store.view.camera?.view === 'plan');
    await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
    assert.deepEqual(await captureWalls(), walls);
  };
  const option = patch => page.evaluate(p => window.verbRoom.store.setView(p, false), patch);
  const toScreen = ([x, y, z = 0]) => page.evaluate(async point => {
    const { project } = await import('/src/viewport/camera.js');
    const canvas = document.querySelector('.viewport canvas');
    const rect = canvas.getBoundingClientRect();
    const p = project(point, window.verbRoom.store.view.camera, canvas.clientWidth, canvas.clientHeight);
    return [rect.x + p[0], rect.y + p[1]];
  }, [x, y, z]);
  const historyDepth = () => page.evaluate(() => window.verbRoom.store.telemetry['history.changed'].undoDepth);
  const selectFirstWall = () => option({ selection: [{ kind: 'wall', index: 0 }] });
  const undo = async (walls, depth, reselect = true) => {
    await page.evaluate(() => window.verbRoom.store.command({ op: 'history.undo' }));
    await page.waitForFunction(() => window.verbRoom.store.ready);
    assert.deepEqual(await captureWalls(), walls);
    assert.equal(await historyDepth(), depth);
    if (reselect) await selectFirstWall();
    await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
  };
  const readout = page.locator('.viewport .drawing-readout');
  const latest = op => page.evaluate(type => window.verbRoom.bridge.audit.findLast(e => e.direction === 'out' && e.message.payload.op === type)?.message.payload, op);
  const drag = async (from, to, { alt = false, magnetic = true } = {}) => {
    const depth = await historyDepth();
    await page.mouse.move(...from);
    await page.mouse.down();
    if (alt) await page.keyboard.down('Alt');
    await page.mouse.move(...to, { steps: 5 });
    if (magnetic) {
      try { await readout.waitFor({ state: 'visible', timeout: 2500 }); }
      catch (error) {
        console.error({ from, to, details: await page.evaluate(() => {
          const { view, state } = window.verbRoom.store;
          const canvas = document.querySelector('.viewport canvas');
          return { selection: view.selection, camera: view.camera, rect: canvas.getBoundingClientRect().toJSON(),
            poses: ['s1x', 's1y', 's2x', 's2y', 'lx', 'ly'].map(k => state.parameters.values[k]),
            ready: window.verbRoom.store.ready, ghosts: document.querySelector('.viewport').dataset.dragGhostCount };
        }) });
        throw error;
      }
      assert.match(await readout.textContent(), /corner$/);
      await page.waitForFunction(() => Number(document.querySelector('.viewport').dataset.dragGhostCount) > 0);
    } else assert.ok(!(await readout.isVisible()) || !(await readout.textContent()).endsWith('corner'));
    await page.mouse.up();
    if (alt) await page.keyboard.up('Alt');
    await page.waitForFunction(n => window.verbRoom.store.telemetry['history.changed']?.undoDepth === n + 1, depth);
    return depth;
  };

  await openScene(startingWalls);
  const [targetX, targetY] = startingWalls[2][0];
  let from = await toScreen(startingWalls[0][1]), to = await toScreen(startingWalls[2][0]);
  to = [to[0] - 4, to[1] + 3];
  let depth = await drag(from, to);
  let current = await captureWalls();
  assert.deepEqual(current[0][1], [targetX, targetY]);
  assert.deepEqual(current[1][0], [targetX, targetY]);
  assert.deepEqual((await latest('surface.moveVertex')).snapTarget, { kind: 'wall', index: 2, vertex: 0 });
  await undo(startingWalls, depth);
  console.log('PASS linked corner drag uses exact target, ghost, and one undo');

  await option({ snap: false, autoLink: false });
  depth = await drag(from, to);
  current = await captureWalls();
  assert.deepEqual(current[0][1], [targetX, targetY]);
  assert.deepEqual(current[1][0], startingWalls[1][0]);
  await undo(startingWalls, depth);
  depth = await drag(from, to, { alt: true, magnetic: false });
  current = await captureWalls();
  assert.notDeepEqual(current[0][1], [targetX, targetY]);
  assert.equal((await latest('surface.moveVertex')).snapTarget, undefined);
  await undo(startingWalls, depth);
  console.log('PASS magnet with grid snap off and auto-link off; Alt bypasses vertex magnet');

  const delta = [targetX - 3, targetY];
  from = await toScreen([1.4, 0]);
  to = await toScreen([1.4 + delta[0], delta[1]]);
  to = [to[0] - 4, to[1] + 3];
  await option({ autoLink: true });
  depth = await drag(from, to);
  current = await captureWalls();
  assert.deepEqual(current[0][1], [targetX, targetY]);
  assert.deepEqual(current[1][0], [targetX, targetY]);
  const translation = await latest('surface.translate');
  assert.deepEqual(translation.snapFrom, { kind: 'wall', index: 0, vertex: 1 });
  assert.deepEqual(translation.deltaM, delta);
  await undo(startingWalls, depth);
  depth = await drag(from, to, { alt: true, magnetic: false });
  assert.notDeepEqual((await captureWalls())[0][1], [targetX, targetY]);
  assert.equal((await latest('surface.translate')).snapTarget, undefined);
  await undo(startingWalls, depth);
  console.log('PASS wall translate snaps with grid off, preserves links, and Alt bypasses it');

  const grouped = [startingWalls[0], startingWalls[1], [[targetX, 3 + targetY], [6, 4.5]]];
  await openScene(grouped);
  await page.evaluate(() => window.verbRoom.store.command({ op: 'surface.link',
    refs: [{ kind: 'wall', index: 0 }, { kind: 'wall', index: 1 }], groupName: 'F7 pair' }));
  await option({ autoLink: false });
  from = await toScreen([1.4, 0]);
  to = await toScreen([1.4 + delta[0], delta[1]]);
  depth = await drag(from, [to[0] - 4, to[1] + 3]);
  current = await captureWalls();
  assert.deepEqual(current[1][1], grouped[2][0]);
  assert.deepEqual((await latest('surface.translate')).snapFrom, { kind: 'wall', index: 1, vertex: 1 });
  await undo(grouped, depth);
  console.log('PASS a wall moved through its group can supply the snapping endpoint');

  for (const view of ['front', 'side', '3d']) {
    const xy = ([x, y]) => view === 'side' ? [y, x] : [x, y];
    const walls = startingWalls.map(w => w.map(xy));
    await openScene(walls);
    await option({ projection: view, autoLink: true });
    await page.waitForFunction(name => window.verbRoom.store.view.camera?.view === name, view);
    await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
    from = await toScreen([...xy([1.4, 0]), 1.5]);
    to = await toScreen([...xy([1.4 + delta[0], delta[1]]), 1.5]);
    depth = await drag(from, [to[0] - 4, to[1]]);
    current = await captureWalls();
    assert.deepEqual(current[0][1], walls[2][0]);
    assert.deepEqual(current[1][0], walls[2][0]);
    await undo(walls, depth);
    console.log(`PASS wall translation snaps in ${view}`);
  }

  const gap = [[[0, 0], [targetX, 0]], [[targetX, .03], [targetX, 4]], [[targetX, 4], [0, 4]], [[0, 4], [0, 0]]];
  await page.setViewportSize({ width: 900, height: 600 });
  await openScene(gap);
  await page.getByRole('tab', { name: 'Scene', exact: true }).click();
  await page.getByText('Open corners: 1', { exact: true }).waitFor();
  const inspector = page.locator('.edit-inspector .context-body');
  const join = page.getByRole('button', { name: 'Join corners', exact: true });
  await join.scrollIntoViewIfNeeded();
  assert.equal(await inspector.evaluate(el => el.scrollWidth <= el.clientWidth && el.getBoundingClientRect().bottom <= innerHeight), true);
  assert.match(await page.getByRole('combobox', { name: 'Corner tolerance' }).textContent(), /5 cm/);
  await pickOption(page, 'Corner tolerance', '1 cm');
  await join.click();
  await page.getByText('Joined 0, skipped 0', { exact: true }).waitFor();
  assert.deepEqual(await captureWalls(), gap);
  depth = await historyDepth();
  await pickOption(page, 'Corner tolerance', '5 cm');
  await join.click();
  await page.getByText('Joined 1, skipped 0', { exact: true }).waitFor();
  assert.equal(await page.getByText('Open corners: 1', { exact: true }).count(), 0);
  assert.deepEqual((await captureWalls())[1][0], gap[0][1]);
  assert.equal(await historyDepth(), depth + 1);
  await undo(gap, depth, false);
  await page.getByText('Open corners: 1', { exact: true }).waitFor();
  assert.deepEqual(errors, []);
  console.log('PASS small window, tolerance choices, 3 cm gap count 1 → 0, status and one undo');
} finally {
  await browser?.close();
  server.close();
}
