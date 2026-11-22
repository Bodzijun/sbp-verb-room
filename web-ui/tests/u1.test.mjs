// U1 (0.2.1): wheel does not change values by default; Hold on never-moved (Auto) keys; one gesture, one undo step.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { serve } from '../scripts/serve.mjs';
const require = createRequire(import.meta.url);

async function withPage(fn) {
  const pw = require(process.env.PLAYWRIGHT_MODULE || 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package');
  const { server, url } = await serve({ port: 0 });
  const browser = await pw.chromium.launch({ headless: true, executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe' });
  const errors = [];
  try {
    const page = await browser.newPage({ viewport: { width: 1160, height: 760 } });
    page.on('pageerror', e => errors.push(e.message));
    await page.goto(url); await page.waitForFunction(() => window.verbRoom?.store?.ready);
    await fn(page);
    assert.deepEqual(errors, []);
  } finally { await browser.close(); server.close?.(); }
}

test('U1 wheel: knob and value field ignore the wheel by default, obey it when the preference is on (and Alt)', async () => {
  await withPage(async page => {
    const probe = () => page.evaluate(() => {
      const root = document.querySelector('.parameter.knob'), id = root.dataset.parameter, s = window.verbRoom.store;
      const fire = (el, init) => { const ev = new WheelEvent('wheel', { deltaY: 100, bubbles: true, cancelable: true, ...init }); el.dispatchEvent(ev); return ev.defaultPrevented; };
      const before = s.state.parameters.values[id];
      const out = { id, before, knobPrevented: fire(root.querySelector('.rotary'), {}), fieldPrevented: fire(root.querySelector('input.readout'), {}) };
      out.alt = fire(root.querySelector('.rotary'), { altKey: true });
      return out;
    });
    // default OFF: nothing prevented, nothing changed
    let r = await probe();
    // Alt+wheel is allowed to change it, so read the value before the Alt event separately
    const idle = await page.evaluate(() => {
      const root = document.querySelector('.parameter.knob'), id = root.dataset.parameter, s = window.verbRoom.store;
      const before = s.state.parameters.values[id];
      const k = root.querySelector('.rotary'), f = root.querySelector('input.readout');
      const e1 = new WheelEvent('wheel', { deltaY: 100, bubbles: true, cancelable: true }); k.dispatchEvent(e1);
      const e2 = new WheelEvent('wheel', { deltaY: 100, bubbles: true, cancelable: true }); f.dispatchEvent(e2);
      return { id, before, after: s.state.parameters.values[id], p1: e1.defaultPrevented, p2: e2.defaultPrevented };
    });
    assert.equal(idle.p1, false); assert.equal(idle.p2, false); assert.equal(idle.after, idle.before);
    assert.equal(r.alt, true);                                           // Alt+wheel still acts
    // ON: preventDefault and the value moves
    await page.evaluate(() => window.verbRoom.store.command({ op: 'preferences.set', patch: { wheelChangesValues: true } }));
    await page.waitForFunction(() => document.documentElement.dataset.wheelValues === 'on');
    const on = await page.evaluate(async () => {
      const root = document.querySelector('.parameter.knob'), id = root.dataset.parameter, s = window.verbRoom.store;
      const before = s.state.parameters.values[id];
      const e = new WheelEvent('wheel', { deltaY: -100, bubbles: true, cancelable: true }); root.querySelector('.rotary').dispatchEvent(e);
      await new Promise(r => setTimeout(r, 200));
      const f = new WheelEvent('wheel', { deltaY: -100, bubbles: true, cancelable: true }); root.querySelector('input.readout').dispatchEvent(f);
      return { before, after: s.state.parameters.values[id], knob: e.defaultPrevented, field: f.defaultPrevented };
    });
    assert.equal(on.knob, true); assert.equal(on.field, true); assert.notEqual(on.after, on.before);
    // back OFF
    await page.evaluate(() => window.verbRoom.store.command({ op: 'preferences.set', patch: { wheelChangesValues: false } }));
    await page.waitForFunction(() => document.documentElement.dataset.wheelValues === 'off');
  });
});

test('U1 Hold: offered on an Auto key, one undo step, disabled with a reason where impossible', async () => {
  await withPage(async page => {
    await page.evaluate(async () => {
      const s = window.verbRoom.store, P = [[1, 1, 1.5], [3, 1, 1.5], [5, 3, 1.5], [7, 3, 1.5], [9, 1, 1.5]];
      await s.command({ op: 'path.set', object: 0, points: P, enabled: true });
      s.setView({ selection: [{ kind: 'source', index: 0 }], timelineOpen: true }, false);
    });
    await page.waitForSelector('.timeline-panel');
    const depth = () => page.evaluate(() => window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0);
    const path = () => page.evaluate(() => JSON.parse(JSON.stringify(window.verbRoom.store.state.document.paths[0])));
    const settle = d => page.waitForFunction(d => (window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0) === d && !document.querySelector('[data-gesture-active]'), d);
    const auto = page.locator('.tl-row[data-object="0"] .tl-auto').nth(1);       // point 2: never moved
    assert.ok(await auto.count() >= 1);
    const open = async () => { const b = await auto.boundingBox(); await page.mouse.click(b.x + b.width / 2, b.y + b.height / 2, { button: 'right' }); await page.waitForSelector('.tl-menu'); };
    await open();
    const item = page.getByRole('menuitem', { name: /Hold here/ });
    assert.equal(await item.count(), 1); assert.equal(await item.isDisabled(), false);
    const d0 = await depth(), before = await path();
    assert.ok(!before.times || before.times.every(t => t === null || t === undefined));
    await item.click(); await settle(d0 + 1);                                   // ONE undo step for key time + hold copy
    const after = await path();
    assert.equal(after.points.length, 6);
    assert.deepEqual(after.points[3], after.points[2]);
    assert.ok(Number.isFinite(after.times[2]) && after.times[3] > after.times[2]);
    await page.evaluate(() => window.verbRoom.store.command({ op: 'history.undo' })); await settle(d0);
    const undone = await path();
    assert.equal(undone.points.length, 5); assert.ok(!undone.times || undone.times.every(t => t === null || t === undefined));
    // redo is not available in the mock bridge (frozen v1); redo of the combined gesture is covered by the C++ bridge test
  });
});

test('U1 walls: Undo while drawing takes back the last click only (wall + its draft point); 4 clicks, 4 undos -> start state', async () => {
  await withPage(async page => {
    await page.evaluate(() => window.verbRoom.store.setView({ page: 'edit', projection: 'plan', tool: 'wall', selection: [], autoLink: true }));
    const viewport = page.locator('.viewport');
    await page.waitForFunction(() => document.querySelector('.viewport canvas')?.width > 150 &&
      document.querySelector('.viewport').dataset.draftPoints === '0');
    const walls = () => page.evaluate(() => window.verbRoom.store.state.document.scene.walls.length);
    const w0 = await walls();
    const box = await viewport.locator('canvas').first().boundingBox(), cx = box.x + box.width / 2, cy = box.y + box.height / 2;
    const waitForDraft = ([wallCount, draftCount]) => page.waitForFunction(([wallCount, draftCount]) =>
      window.verbRoom.store.state.document.scene.walls.length === wallCount &&
      Number(document.querySelector('.viewport').dataset.draftPoints) === draftCount, [wallCount, draftCount]);
    const pts = [[-90, -60], [90, -60], [90, 60], [-90, 60]];
    for (const [index, [dx, dy]] of pts.entries()) {
      await page.mouse.click(cx + dx, cy + dy);
      await waitForDraft([w0 + index, index + 1]);
    }
    for (const expected of [[w0 + 2, 3], [w0 + 1, 2], [w0, 1], [w0, 0]]) {
      await page.keyboard.press('Control+z');
      await waitForDraft(expected);
    }
  });
});
