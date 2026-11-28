// U2 (0.2.1): UI-A scrollLeft=0 on inspector numeric entry; UI-B drag projection & ghosts; UI-C rebuilding banner; post-gain source meters; Redraw dimmed old path.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { serve } from '../scripts/serve.mjs';
const require = createRequire(import.meta.url);

async function withPage(fn, viewport = { width: 1050, height: 700 }) {
  const pw = require(process.env.PLAYWRIGHT_MODULE || 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package');
  const { server, url } = await serve({ port: 0 });
  const browser = await pw.chromium.launch({ headless: true, executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe' });
  const errors = [];
  try {
    const page = await browser.newPage({ viewport });
    page.on('pageerror', e => errors.push(e.message));
    await page.goto(url);
    await page.waitForFunction(() => window.verbRoom?.store?.ready);
    await fn(page);
    assert.deepEqual(errors, []);
  } finally {
    await browser.close();
    server.close?.();
  }
}

test('F1 Randomize is a keyboard-accessible Seed control and sends a different in-range seed', async () => {
  await withPage(async page => {
    await page.evaluate(async () => {
      const { store } = window.verbRoom;
      await store.setView({ page: 'edit', inspectorTab: 'scene' });
      await store.command({ op: 'room.setClutter', seed: 1 });
    });
    await page.getByRole('complementary').getByRole('button', { name: 'Clutter', exact: true }).click();
    const randomize = page.getByRole('button', { name: 'Randomize clutter seed' });
    const seedField = page.getByRole('textbox', { name: 'Seed value' });
    const readout = await seedField.boundingBox(), button = await randomize.boundingBox();
    assert.ok(button.x >= readout.x + readout.width && Math.abs(button.y - readout.y) < 5);
    const slider = await page.getByRole('slider', { name: 'Seed', exact: true }).boundingBox();
    assert.ok(slider.width > 10, 'Seed track stays usable next to Randomize');

    for (const [current, random, expected, key] of [[1, 0, 2, 'Enter'], [9999, 0.9999999, 9998, 'Space']]) {
      await page.evaluate(async ({ current, random }) => {
        await window.verbRoom.store.command({ op: 'room.setClutter', seed: current });
        window.Math.random = () => random;
      }, { current, random });
      await seedField.focus();
      await page.keyboard.press('Tab');
      assert.ok(await randomize.evaluate(el => document.activeElement === el));
      await page.keyboard.press(key);
      await page.waitForFunction(seed => window.verbRoom.store.state.document.scene.clutter.seed === seed, expected);
      const result = await page.evaluate(() => {
        const { store, bridge } = window.verbRoom;
        const last = bridge.audit.findLast(e => e.direction === 'out' && e.message.payload?.op === 'room.setClutter');
        return { command: last.message.payload, roughSeed: store.state.document.scene.roughSeed };
      });
      assert.deepEqual(result, { command: { op: 'room.setClutter', seed: expected }, roughSeed: expected });
      assert.equal(await seedField.inputValue(), String(expected));
    }
  });
});

test('F1 wall drag sends autoLink on and off, previews both connected endpoints and undoes as one transaction', async () => {
  await withPage(async page => {
    await page.evaluate(() => window.verbRoom.store.setView({ page: 'edit', projection: 'plan', tool: 'move' }));
    await page.waitForFunction(() => document.querySelector('.viewport')?.liveCamera?.view === 'plan' && document.querySelector('.edit-inspector'));
    // V04e (flaky fix): after switching to Edit the viewport canvas catches up with the page layout over a few hundred
    // ms. Wait until its on-screen rect AND backing size are unchanged for 10 consecutive animation frames (no sleeps).
    const layoutSettled = () => page.waitForFunction(() => {
      const c = document.querySelector('.viewport canvas'); if (!c) return false;
      const r = c.getBoundingClientRect(), k = [r.x, r.y, r.width, r.height, c.width, c.height].map(v => Math.round(v * 10)).join(',');
      if (window.__u2LayoutKey === k) window.__u2Stable = (window.__u2Stable || 0) + 1; else { window.__u2LayoutKey = k; window.__u2Stable = 0; }
      return r.width > 0 && r.height > 0 && window.__u2Stable >= 10;
    }, null, { polling: 'raf' });
    for (const enabled of [true, false]) {
      if (!enabled) await page.getByRole('switch', { name: 'Auto-link corners' }).click();
      await layoutSettled();
      const start = await page.evaluate(async () => {
        const { project } = await import('/src/viewport/camera.js');
        const { store, bridge } = window.verbRoom;
        const walls = store.state.document.scene.walls;
        const vp = document.querySelector('.viewport'), canvas = vp.querySelector('canvas');
        const rect = canvas.getBoundingClientRect();
        const worldToLocal = ([x, y]) => project([x, y, 0], vp.liveCamera, rect.width, rect.height).slice(0, 2);
        const points = walls.map(w => [worldToLocal(w.a), worldToLocal(w.b)]);
        const [a, b] = points[1];
        const ctx = canvas.getContext('2d');
        let path = [];
        window.f1Ghosts = [];
        if (!canvas.dataset.f1Hooked) for (const method of ['beginPath', 'moveTo', 'lineTo', 'stroke']) {
          const original = ctx[method];
          ctx[method] = function (...args) {
            if (method === 'beginPath') path = [];
            else if (method === 'moveTo' || method === 'lineTo') path.push(args);
            else if (this.lineWidth === 5.5 && this.globalAlpha === 0.35) window.f1Ghosts.push(path);
            return original.apply(this, args);
          };
        }
        canvas.dataset.f1Hooked = 'true';
        return { x: rect.x + (a[0] + b[0]) / 2, y: rect.y + (a[1] + b[1]) / 2, points, walls, audit: bridge.audit.length };
      });
      await page.mouse.move(start.x, start.y);
      await page.mouse.down();
      await page.mouse.move(start.x, start.y - 30);
      const count = enabled ? 3 : 1;
      await page.waitForFunction(n => Number(document.querySelector('.viewport').dataset.dragGhostCount) === n, count);
      const ghosts = await page.evaluate(n => window.f1Ghosts.slice(-n), count);
      const shifted = ([x, y]) => [x, y - 30];
      const expected = [[shifted(start.points[1][0]), shifted(start.points[1][1])]];
      if (enabled) expected.push([start.points[0][0], shifted(start.points[0][1])], [shifted(start.points[2][0]), start.points[2][1]]);
      assert.equal(ghosts.length, count);
      for (const segment of expected) assert.ok(ghosts.some(line => line.length === 2 &&
        segment.every(([x, y], i) => Math.hypot(line[i][0] - x, line[i][1] - y) < 1)), JSON.stringify({ segment, ghosts }));

      await page.mouse.up();
      await page.waitForFunction(audit => window.verbRoom.bridge.audit.slice(audit)
        .some(e => e.direction === 'out' && e.message.payload?.op === 'transaction.commit'), start.audit);
      const end = await page.evaluate(audit => {
        const { store, bridge } = window.verbRoom;
        const messages = bridge.audit.slice(audit).filter(e => e.direction === 'out').map(e => e.message);
        const translate = messages.find(m => m.payload?.op === 'surface.translate');
        const ops = messages.filter(m => m.transactionId === translate.transactionId).map(m => m.payload.op);
        return { translate: translate.payload, ops, walls: store.state.document.scene.walls };
      }, start.audit);
      assert.equal(end.translate.autoLink, enabled);
      assert.deepEqual(end.translate.refs, [{ kind: 'wall', index: 1 }]);
      assert.deepEqual(end.ops, ['transaction.begin', 'surface.translate', 'transaction.commit']);
      assert.notDeepEqual(end.walls[1], start.walls[1]);
      assert.deepEqual(end.walls[0].b, enabled ? end.walls[1].a : start.walls[0].b);
      assert.deepEqual(end.walls[2].a, enabled ? end.walls[1].b : start.walls[2].a);
      assert.deepEqual(end.walls[0].a, start.walls[0].a);
      assert.deepEqual(end.walls[2].b, start.walls[2].b);
      const status = await page.evaluate(async () => {
        const { store } = window.verbRoom;
        await store.command({ op: 'history.undo' });
        return (await store.command({ op: 'history.undo' })).payload.status;
      });
      assert.equal(status, 'noChange');
      assert.deepEqual(await page.evaluate(() => window.verbRoom.store.state.document.scene.walls), start.walls);
    }
  });
});

test('F1 mock replays replacement deltas from the start and rejects a collapsed neighbour atomically', async () => {
  await withPage(async page => {
    const result = await page.evaluate(async () => {
      const { store } = window.verbRoom;
      const walls = () => store.state.document.scene.walls;
      const command = (dy, autoLink) => ({ op: 'surface.translate', refs: [{ kind: 'wall', index: 1 }], deltaM: [0, dy], autoLink });
      const initial = walls();
      await store.command({ op: 'transaction.begin', label: 'Translate wall' }, 'f1');
      await store.command(command(1, true), 'f1');
      const linked = walls();
      await store.command(command(2, false), 'f1');
      const unlinked = walls();
      await store.command(command(0.5, true), 'f1');
      const relinked = walls();
      await store.command({ op: 'transaction.commit' }, 'f1');
      await store.command({ op: 'history.undo' });
      const reject = await store.command(command(8, true));
      return { initial, linked, unlinked, relinked, reject: reject.payload.code, afterReject: walls() };
    });
    assert.deepEqual(result.linked[0].b, [0, 1]);
    assert.deepEqual(result.unlinked[0].b, [0, 0]);
    assert.deepEqual(result.relinked[0].b, [0, 0.5]);
    assert.equal(result.reject, 'invalid');
    assert.deepEqual(result.afterReject, result.initial);
  });
});

test('U2 UI-A: focusing, typing, and committing numeric fields in Main Inspector leaves every ancestor scrollLeft at 0', async () => {
  await withPage(async page => {
    for (const selection of [[{ kind: 'listener', index: 0 }], [{ kind: 'source', index: 0 }]]) {
      await page.evaluate(sel => window.verbRoom.store.setView({ page: 'main', selection: sel }), selection);
      await page.waitForSelector('.pose-column input.readout');
      const count = await page.locator('.selected-card input.readout').count();
      assert.ok(count >= 4);
      for (let i = 0; i < count; i++) {
        const inp = page.locator('.selected-card input.readout').nth(i);
        if (await inp.isDisabled()) continue;
        await inp.dblclick();
        await page.keyboard.type('1.25');
        const whileFocused = await page.evaluate(() => {
          const bad = [];
          let cur = document.activeElement?.parentElement;
          while (cur && cur !== document.documentElement) {
            if (cur.scrollLeft !== 0) bad.push({ tag: cur.tagName, cls: cur.className, scrollLeft: cur.scrollLeft });
            cur = cur.parentElement;
          }
          return bad;
        });
        assert.deepEqual(whileFocused, [], `${selection[0].kind} field ${i} scrolled ancestor while focused`);
        await page.keyboard.press('Enter');
        await page.waitForTimeout(20);
        const afterCommit = await page.evaluate(() => {
          const bad = [];
          for (const el of document.querySelectorAll('#app, #app *')) {
            if (el.tagName !== 'INPUT' && el.scrollLeft !== 0) bad.push({ tag: el.tagName, cls: el.className, scrollLeft: el.scrollLeft });
          }
          return bad;
        });
        assert.deepEqual(afterCommit, [], `${selection[0].kind} field ${i} left ancestor scrollLeft != 0 after commit`);
      }
    }
  }, { width: 1050, height: 700 });
});

test('U2 UI-B: 3D surface unprojection at hit height stays under cursor (< 1 mm) across camera angles', async () => {
  const { createCamera, frame, project, pointOnHeightPlane, hitHeightForSurface } = await import('../src/viewport/camera.js');
  const wall = { a: [1.0, 0.5], b: [5.0, 2.5], zBottomM: 0, zTopM: 3.2 };
  const plate = { vertices: [[1, 1], [3, 1], [3, 3], [1, 3]], heightM: 1.8 };
  const floorZone = { vertices: [[0, 0], [2, 0], [2, 2], [0, 2]], heightM: null };
  const bounds = { min: [0, 0, 0], max: [6, 4, 3.2] };
  for (const [w, h] of [[600, 400], [1000, 700]]) {
    for (const yaw of [-2.4, -0.75, 0.4, 1.9]) {
      for (const pitch of [0.25, 0.55, 1.0]) {
        const cam = frame({ ...createCamera('3d'), yaw, pitch }, bounds, w, h);
        const dy = pitch < 0.35 ? 24 : -24;
        for (const z of [0.3, 1.6, 2.9]) {
          const clickWorld = [3.0, 1.5, z]; // lies directly on `wall`
          const startScreen = project(clickWorld, cam, w, h);
          const hitZ = hitHeightForSurface(startScreen, cam, w, h, wall, 'wall', 3.2);
          assert.ok(Math.abs(hitZ - z) < 1e-3, `wall hitZ error ${Math.abs(hitZ - z)} >= 1 mm`);
          const targetScreen = [startScreen[0] + 45, startScreen[1] + dy];
          const a = pointOnHeightPlane(startScreen, cam, w, h, hitZ);
          const b = pointOnHeightPlane(targetScreen, cam, w, h, hitZ);
          assert.ok(a && b);
          const movedWorld = [clickWorld[0] + (b[0] - a[0]), clickWorld[1] + (b[1] - a[1]), clickWorld[2] + (b[2] - a[2])];
          const reprojected = project(movedWorld, cam, w, h);
          assert.ok(Math.hypot(reprojected[0] - targetScreen[0], reprojected[1] - targetScreen[1]) < 1e-6);
          const unprojMoved = pointOnHeightPlane(reprojected, cam, w, h, hitZ);
          assert.ok(Math.hypot(movedWorld[0] - unprojMoved[0], movedWorld[1] - unprojMoved[1], movedWorld[2] - unprojMoved[2]) < 1e-3);
        }
        for (const [kind, obj, expectedZ] of [['floorZone', plate, 1.8], ['floorZone', floorZone, 0], ['ceilingZone', floorZone, 3.2]]) {
          const hitZ = hitHeightForSurface([w / 2, h / 2], cam, w, h, obj, kind, 3.2);
          assert.ok(Math.abs(hitZ - expectedZ) < 1e-6);
        }
      }
    }
  }
});

test('U2 UI-B: dragging zones and linked rooms shows ghosts for all moving surfaces and applies cleanly on release', async () => {
  await withPage(async page => {
    const beforeDraw = await page.evaluate(async () => {
      const n = Number(document.querySelector('.viewport')?.dataset.drawCount || 0);
      const s = window.verbRoom.store;
      await s.setView({ page: 'edit', projection: 'plan', tool: 'move', selection: [] });
      await s.command({ op: 'surface.create', kind: 'floorZone', vertices: [[0.4, 3.5], [1.8, 3.5], [1.8, 4.8], [0.4, 4.8]], heightM: 1.0 });
      return n;
    });
    await page.waitForFunction(n => Number(document.querySelector('.viewport')?.dataset.drawCount || 0) > n && window.verbRoom.store.state.document.scene.floorZones.length === 1, beforeDraw);
    await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
    // 1. Drag a zone / raised plate in Edit room
    const zoneScreen = await page.evaluate(async () => {
      const { project } = await import('/src/viewport/camera.js');
      const s = window.verbRoom.store, z = s.state.document.scene.floorZones.at(-1);
      const cx = z.vertices.reduce((n, p) => n + p[0], 0) / z.vertices.length;
      const cy = z.vertices.reduce((n, p) => n + p[1], 0) / z.vertices.length;
      const vp = document.querySelector('.viewport'), rect = vp.querySelector('canvas').getBoundingClientRect();
      const pt = project([cx, cy, z.heightM ?? 0], vp.liveCamera, rect.width, rect.height);
      return { x: rect.left + pt[0], y: rect.top + pt[1], localX: pt[0], localY: pt[1], origX: z.vertices[0][0], index: s.state.document.scene.floorZones.length - 1 };
    });
    await page.mouse.move(zoneScreen.x, zoneScreen.y);
    await page.mouse.down();
    await page.mouse.move(zoneScreen.x + 120, zoneScreen.y - 30, { steps: 6 });
    await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
    const duringZoneDrag = await page.evaluate(({ idx, lx, ly }) => {
      const c = document.querySelector('.viewport canvas');
      const ctx = c.getContext('2d');
      const dpr = window.devicePixelRatio || 1;
      const bg = [...ctx.getImageData(Math.round(10 * dpr), Math.round(10 * dpr), 1, 1).data];
      const ghostPx = [...ctx.getImageData(Math.round((lx + 120) * dpr), Math.round((ly - 30) * dpr), 1, 1).data];
      return {
        sel: window.verbRoom.store.view.selection,
        ghosts: Number(document.querySelector('.viewport').dataset.dragGhostCount || 0),
        vertX: window.verbRoom.store.state.document.scene.floorZones[idx].vertices[0][0],
        bg,
        ghostPx
      };
    }, { idx: zoneScreen.index, lx: zoneScreen.localX, ly: zoneScreen.localY });
    assert.equal(duringZoneDrag.sel[0]?.kind, 'floorZone', 'picked the floorZone');
    assert.equal(duringZoneDrag.ghosts, 1, 'zone gets a ghost during drag');
    assert.notDeepEqual(duringZoneDrag.ghostPx, duringZoneDrag.bg, 'ghost fill actually painted at dragged zone position');
    assert.equal(duringZoneDrag.vertX, zoneScreen.origX, 'original zone stays put while dragging ghost');
    await page.mouse.up();
    await page.waitForFunction(idx => Number(document.querySelector('.viewport').dataset.dragGhostCount || 0) === 0 && window.verbRoom.store.state.document.scene.floorZones[idx].vertices[0][0] !== 0.4, zoneScreen.index);
    const afterZoneDrag = await page.evaluate(idx => ({
      ghosts: Number(document.querySelector('.viewport').dataset.dragGhostCount || 0),
      vertX: window.verbRoom.store.state.document.scene.floorZones[idx].vertices[0][0]
    }), zoneScreen.index);
    assert.equal(afterZoneDrag.ghosts, 0);
    assert.ok(Math.abs(afterZoneDrag.vertX - zoneScreen.origX) > 0.2, 'zone moves cleanly on release');

    // 2. Drag one wall of a linked group in 3D view: all linked group members get ghosts and move identically
    const before3d = await page.evaluate(async () => {
      const n = Number(document.querySelector('.viewport')?.dataset.drawCount || 0);
      const s = window.verbRoom.store;
      await s.command({ op: 'surface.link', refs: [{ kind: 'wall', index: 0 }, { kind: 'wall', index: 1 }, { kind: 'wall', index: 2 }], groupName: 'LinkTest' });
      await s.setView({ projection: '3d', selection: [], autoLink: false });
      return n;
    });
    await page.waitForFunction(n => Number(document.querySelector('.viewport')?.dataset.drawCount || 0) > n && document.querySelector('.viewport')?.liveCamera?.view === '3d', before3d);
    await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
    const wallScreen = await page.evaluate(async () => {
      const { project } = await import('/src/viewport/camera.js');
      const s = window.verbRoom.store, w0 = s.state.document.scene.walls[0];
      const mx = (w0.a[0] + w0.b[0]) / 2, my = (w0.a[1] + w0.b[1]) / 2, mz = 1.5;
      const vp = document.querySelector('.viewport'), rect = vp.querySelector('canvas').getBoundingClientRect();
      const pt = project([mx, my, mz], vp.liveCamera, rect.width, rect.height);
      return { x: rect.left + pt[0], y: rect.top + pt[1], before: s.state.document.scene.walls.slice(0, 4).map(w => w.a.slice()) };
    });
    await page.mouse.move(wallScreen.x, wallScreen.y);
    await page.mouse.down();
    await page.mouse.move(wallScreen.x + 50, wallScreen.y - 25, { steps: 6 });
    await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
    const duringLinkDrag = await page.evaluate(() => ({
      ghosts: Number(document.querySelector('.viewport').dataset.dragGhostCount || 0),
      a0: window.verbRoom.store.state.document.scene.walls[0].a.slice()
    }));
    assert.equal(duringLinkDrag.ghosts, 3, 'all 3 linked walls render ghosts together');
    assert.deepEqual(duringLinkDrag.a0, wallScreen.before[0], 'linked originals stay unmutated until release');
    await page.mouse.up();
    await page.waitForFunction(prevX => Number(document.querySelector('.viewport').dataset.dragGhostCount || 0) === 0 && window.verbRoom.store.state.document.scene.walls[0].a[0] !== prevX, wallScreen.before[0][0]);
    const afterLinkDrag = await page.evaluate(() => window.verbRoom.store.state.document.scene.walls.slice(0, 4).map(w => w.a.slice()));
    const deltas = [0, 1, 2].map(i => [afterLinkDrag[i][0] - wallScreen.before[i][0], afterLinkDrag[i][1] - wallScreen.before[i][1]]);
    assert.ok(Math.hypot(...deltas[0]) > 0.1, 'dragged wall moved');
    assert.ok(Math.hypot(deltas[1][0] - deltas[0][0], deltas[1][1] - deltas[0][1]) < 1e-6, 'linked wall 1 matched delta');
    assert.ok(Math.hypot(deltas[2][0] - deltas[0][0], deltas[2][1] - deltas[0][1]) < 1e-6, 'linked wall 2 matched delta');
    assert.ok(Math.hypot(afterLinkDrag[3][0] - wallScreen.before[3][0], afterLinkDrag[3][1] - wallScreen.before[3][1]) < 1e-9, 'unlinked wall 3 stayed put');
  }, { width: 1050, height: 700 });
});

test('U2 UI-C: prominent amber Rebuilding banner debounces flicker, shows while rebuilding, does not block input, and hides when off', async () => {
  await withPage(async page => {
    const state = () => page.evaluate(() => {
      const el = document.querySelector('.viewport-rebuilding');
      const cs = el ? getComputedStyle(el) : null;
      return {
        exists: !!el,
        hidden: el?.hidden ?? true,
        display: cs?.display,
        pointerEvents: cs?.pointerEvents,
        borderColor: cs?.borderTopColor,
        color: cs?.color,
        text: el?.textContent?.trim() || ''
      };
    });
    const init = await state();
    assert.equal(init.exists, true);
    assert.equal(init.hidden, true);
    assert.equal(init.display, 'none');

    const clockStart = Date.UTC(2026, 0, 1);
    await page.clock.install({ time: new Date(clockStart) });
    await page.clock.pauseAt(new Date(clockStart + 1000));

    // Short (< 150 ms) spike must NOT flicker the banner
    await page.evaluate(() => window.verbRoom.bridge.setRebuilding(true));
    await page.clock.runFor(60);
    await page.evaluate(() => window.verbRoom.bridge.setRebuilding(false));
    await page.clock.runFor(150);
    assert.equal((await state()).hidden, true, 'brief <150ms rebuild should not flicker banner');

    // Sustained rebuild: appears after ~150 ms, non-blocking, amber/warning styling
    await page.evaluate(() => window.verbRoom.bridge.setRebuilding(true));
    await page.clock.runFor(150);
    const visible = await state();
    assert.equal(visible.hidden, false);
    assert.notEqual(visible.display, 'none');
    assert.equal(visible.pointerEvents, 'none');
    assert.match(visible.text, /Rebuilding/);
    assert.notEqual(visible.borderColor, 'rgb(33, 185, 155)', 'uses amber/warning token rather than emerald accent');

    // Turning off immediately after appear still respects minimum visible window (~400 ms), then hides
    await page.evaluate(() => window.verbRoom.bridge.setRebuilding(false));
    await page.clock.runFor(399);
    assert.equal((await state()).hidden, false, 'stays visible during 400ms minimum window');
    await page.clock.runFor(1);
    assert.equal((await state()).hidden, true);
  }, { width: 1050, height: 700 });
});

test('U2 Item 4: source meter is post-gain (tracks gain dB, zeros on Mute or another source Solo) and has post-gain tooltip', async () => {
  const { sourcePostGain } = await import('../src/components/meters.js');
  assert.equal(sourcePostGain({ s1g: 0 }, 2, 0), 1);
  assert.ok(Math.abs(sourcePostGain({ s1g: 6 }, 2, 0) - Math.pow(10, 6 / 20)) < 1e-6);
  assert.equal(sourcePostGain({ s1g: -60 }, 2, 0), 0);
  assert.equal(sourcePostGain({ s1g: 0, s1mute: 1, s1solo: 1 }, 2, 0), 0);
  assert.equal(sourcePostGain({ s1g: 0, s2solo: 1 }, 2, 0), 0);
  assert.equal(sourcePostGain({ s1g: 0, s2solo: 1 }, 2, 1), 1);
  assert.equal(sourcePostGain({ s1g: 0, s3solo: 1 }, 2, 0), 1, 'inactive source solo beyond count ignored');

  await withPage(async page => {
    await page.evaluate(async () => {
      const s = window.verbRoom.store;
      await s.command({ op: 'source.add' });
      await s.setView({ page: 'main', tab: 'sources' });
    });
    await page.waitForSelector('.mix-row .hmeter');
    await page.waitForFunction(() => {
      const bars = document.querySelectorAll('.mix-row .hmeter');
      return bars.length >= 2 && parseFloat(bars[0].firstChild.style.left || '0') > 10 && parseFloat(bars[1].firstChild.style.left || '0') > 10;
    }, null, { timeout: 3000 });

    const info = await page.evaluate(() => [...document.querySelectorAll('.mix-row .hmeter')].map(el => ({
      title: el.title,
      pct: parseFloat(el.firstChild.style.left || '0')
    })));
    assert.match(info[0].title, /post-gain/);
    assert.ok(info[0].pct > 10);

    // Lower s1g to -24 dB -> bar drops noticeably
    await page.evaluate(() => {
      const b = window.verbRoom.bridge;
      b.parameters.begin('s1g'); b.parameters.change('s1g', -24); b.parameters.end('s1g');
    });
    await page.waitForTimeout(160);
    const lowPct = await page.evaluate(() => parseFloat(document.querySelectorAll('.mix-row .hmeter')[0].firstChild.style.left || '0'));
    assert.ok(lowPct < info[0].pct - 20, `expected -24 dB bar (${lowPct}%) much lower than 0 dB (${info[0].pct}%)`);

    // Raise s1g back to 0 dB, wait for level recovery, then Mute -> drops to 0%
    await page.evaluate(() => {
      const b = window.verbRoom.bridge;
      b.parameters.begin('s1g'); b.parameters.change('s1g', 0); b.parameters.end('s1g');
    });
    await page.waitForFunction(() => parseFloat(document.querySelectorAll('.mix-row .hmeter')[0].firstChild.style.left || '0') > 20, null, { timeout: 2000 });
    await page.evaluate(() => {
      const b = window.verbRoom.bridge;
      b.parameters.begin('s1mute'); b.parameters.change('s1mute', 1); b.parameters.end('s1mute');
    });
    await page.waitForTimeout(150);
    const mutedState = await page.evaluate(() => {
      const el = document.querySelectorAll('.mix-row .hmeter')[0];
      return { pct: parseFloat(el.firstChild.style.left || '0'), holdHidden: el.lastChild.hidden };
    });
    assert.equal(mutedState.pct, 0, 'muted source meter must be 0%');
    assert.equal(mutedState.holdHidden, true, 'muted source hold tick must hide');

    // Unmute source 1, Solo source 2 -> source 1 meter zeros, source 2 meter stays active
    await page.evaluate(() => {
      const b = window.verbRoom.bridge;
      b.parameters.begin('s1mute'); b.parameters.change('s1mute', 0); b.parameters.end('s1mute');
    });
    await page.waitForFunction(() => parseFloat(document.querySelectorAll('.mix-row .hmeter')[0].firstChild.style.left || '0') > 20, null, { timeout: 2000 });
    await page.evaluate(() => {
      const b = window.verbRoom.bridge;
      b.parameters.begin('s2solo'); b.parameters.change('s2solo', 1); b.parameters.end('s2solo');
    });
    await page.waitForTimeout(150);
    const soloState = await page.evaluate(() => [...document.querySelectorAll('.mix-row .hmeter')].map(el => ({
      pct: parseFloat(el.firstChild.style.left || '0'),
      holdHidden: el.lastChild.hidden
    })));
    assert.equal(soloState[0].pct, 0, 'non-soloed source 1 must drop to 0% when source 2 is soloed');
    assert.equal(soloState[0].holdHidden, true);
    assert.ok(soloState[1].pct > 10, 'soloed source 2 must remain active');
  }, { width: 1050, height: 700 });
});

test('U2 Item 5: Redraw draws old path with pathDimmed dashed stroke, excludes its nodes from hit-testing, restores on Esc, and clears on Finish', async () => {
  await withPage(async page => {
    const oldPts = [[1.5, 2.0, 1.5], [4.5, 2.0, 1.5], [4.5, 5.5, 1.5]];
    await page.evaluate(async pts => {
      const s = window.verbRoom.store;
      await s.command({ op: 'path.set', object: 0, points: pts, enabled: true });
      await s.setView({ page: 'main', projection: 'plan', selection: [{ kind: 'source', index: 0 }] });
    }, oldPts);
    await page.waitForSelector('.path-draw-btn');
    assert.equal(await page.locator('.path-draw-btn').textContent(), 'Redraw');

    // Enter Redraw mode by clicking the Redraw button
    await page.locator('.path-draw-btn').click();
    await page.waitForFunction(() => window.verbRoom.store.view.pathDrawing === 0);
    await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));

    // Spy on canvas 2D strokes while forcing a repaint during Redraw
    const strokes = await page.evaluate(async () => {
      const { viewportPalette } = await import('/src/viewport/palette.js');
      const roles = viewportPalette(window.verbRoom.store.state.preferences || {});
      const canvas = document.querySelector('.viewport canvas');
      const ctx = canvas.getContext('2d');
      const origStroke = ctx.stroke.bind(ctx);
      const origDash = ctx.setLineDash.bind(ctx);
      let curDash = [];
      const seen = [];
      ctx.setLineDash = arr => { curDash = [...arr]; origDash(arr); };
      ctx.stroke = function () {
        seen.push({ strokeStyle: ctx.strokeStyle, lineWidth: ctx.lineWidth, alpha: ctx.globalAlpha, dash: curDash.slice() });
        return origStroke();
      };
      // Trigger a viewport redraw via a store view notification (draws synchronously on next rAF)
      window.verbRoom.store.setView({ pathDraft: window.verbRoom.store.view.pathDraft || null }, false);
      await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
      ctx.stroke = origStroke;
      ctx.setLineDash = origDash;
      return { expectedDimmed: roles.pathDimmed.toLowerCase(), seen };
    });
    const ghostStroke = strokes.seen.find(s =>
      s.strokeStyle.toLowerCase() === strokes.expectedDimmed && s.dash.length === 2 && s.lineWidth <= 1.2 && s.alpha <= 0.6
    );
    assert.ok(ghostStroke, `expected a thin dashed pathDimmed (${strokes.expectedDimmed}) stroke while redrawing (saw ${strokes.seen.length} strokes: ${JSON.stringify(strokes.seen.filter(x => x.dash.length))})`);

    // Project middle old node [4.5, 2.0, 1.5] and click/drag on it AND right-click it:
    // old path must NOT move or lose a point; instead left-click adds a new draft point.
    const nodePos = await page.evaluate(async pt => {
      const { project } = await import('/src/viewport/camera.js');
      const vp = document.querySelector('.viewport'), rect = vp.querySelector('canvas').getBoundingClientRect();
      const sPt = project(pt, vp.liveCamera, rect.width, rect.height);
      return { x: rect.left + sPt[0], y: rect.top + sPt[1] };
    }, oldPts[1]);

    await page.mouse.move(nodePos.x, nodePos.y);
    await page.mouse.down();
    await page.mouse.move(nodePos.x + 35, nodePos.y + 35, { steps: 4 });
    await page.mouse.up();
    await page.mouse.click(nodePos.x, nodePos.y, { button: 'right' });
    await page.waitForTimeout(80);

    const duringRedraw = await page.evaluate(() => ({
      oldPoints: window.verbRoom.store.state.document.paths[0].points,
      draftLen: window.verbRoom.store.view.pathDraft?.length || 0,
      pathDrawing: window.verbRoom.store.view.pathDrawing
    }));
    assert.deepEqual(duringRedraw.oldPoints, oldPts, 'clicking/dragging old node must not modify old path');
    assert.equal(duringRedraw.draftLen, 2, 'click on old node added a new draft point instead of dragging old node');
    assert.equal(duringRedraw.pathDrawing, 0);

    // Esc cancels redraw and keeps old path unchanged
    await page.keyboard.press('Escape');
    await page.waitForFunction(() => window.verbRoom.store.view.pathDrawing === null);
    const afterEsc = await page.evaluate(() => window.verbRoom.store.state.document.paths[0].points);
    assert.deepEqual(afterEsc, oldPts, 'Esc restores original path unchanged');

    // Redraw again and Finish replaces the old path with the new draft
    await page.locator('.path-draw-btn').click();
    await page.waitForFunction(() => window.verbRoom.store.view.pathDrawing === 0);
    await page.mouse.click(nodePos.x + 40, nodePos.y - 40);
    await page.locator('.path-draw-btn').click(); // Drawing... (Finish)
    await page.waitForFunction(() => window.verbRoom.store.view.pathDrawing === null && window.verbRoom.store.state.document.paths[0].points.length === 2);
    const afterFinish = await page.evaluate(() => window.verbRoom.store.state.document.paths[0].points);
    assert.equal(afterFinish.length, 2);
    assert.notDeepEqual(afterFinish[1], oldPts[1], 'Finish replaced old path with new draft');
  }, { width: 1050, height: 700 });
});





// U3: source meter ballistics (peak vs smooth)
import { stepSourceMeter, METER_MODES } from '../src/components/meters.js';
test('U3 source meter: peak mode falls faster than smooth and holds ~1.2 s', () => {
  const run = mode => { const n = { level: 0, hold: 0, holdAt: 0 }; let t = 0; stepSourceMeter(n, 1, 33, t, mode); for (let i = 0; i < 15; i++) { t += 33; stepSourceMeter(n, 0, 33, t, mode); } return { ...n, t }; };
  const pk = run('peak'), sm = run('smooth');
  assert.ok(pk.level < sm.level * 0.8, `peak level ${pk.level} should be well below smooth ${sm.level}`);
  assert.equal(pk.hold, 1, 'peak hold marker stays for 1.2 s');
  assert.ok(sm.hold < 1, 'smooth hold decays continuously');
  const n = { level: 0, hold: 0, holdAt: 0 }; stepSourceMeter(n, 0.5, 33, 0, 'peak'); assert.equal(n.level, 0.5, 'instant attack');
  assert.ok(METER_MODES.peak && METER_MODES.smooth);
});
