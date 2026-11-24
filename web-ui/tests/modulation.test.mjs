import test from 'node:test';
import assert from 'node:assert/strict';
import { validator, readFixture } from './helpers.mjs';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';

const sampleRoute = (overrides = {}) => ({
  enabled: true,
  object: 0,
  driver: 'distanceToListener',
  curve: {
    inputRange: [0.5, 10],
    outputRange: [20000, 800],
    invert: false,
    clamp: true,
    nodes: [
      { x: 0, y: 0, smooth: false },
      { x: 0.5, y: 0.4, smooth: true },
      { x: 1, y: 1, smooth: false },
    ],
  },
  targetObject: -1,
  target: 'sourceLowPass',
  attackMs: 15,
  releaseMs: 80,
  ...overrides,
});

test('G4 mock bridge: modulation routes, budgets, path.setLfo, bake non-invalidation, undo, source.remove & Keep I/O', async t => {
  const bridge = await createMockBridge({ read: readFixture, validator, delayMs: 0, bakeDurationMs: 25 });
  const store = createStore(bridge);
  t.after(() => { store.dispose(); bridge.dispose(); });
  await store.start();

  // Default state omits modulation
  assert.equal(store.state.document.modulation, undefined);

  // Add route & edit route
  let res = await store.command({ op: 'modulation.addRoute', route: sampleRoute() });
  assert.equal(res?.kind, 'ack');
  assert.equal(store.state.document.modulation.routes.length, 1);
  assert.equal(store.state.document.modulation.routes[0].target, 'sourceLowPass');

  res = await store.command({
    op: 'modulation.setRoute',
    index: 0,
    route: sampleRoute({ target: 'sourceGain', curve: { inputRange: [0, 10], outputRange: [0, -18], nodes: [{ x: 0, y: 0 }, { x: 1, y: 1 }] } }),
  });
  assert.equal(res?.kind, 'ack');
  assert.equal(store.state.document.modulation.routes[0].target, 'sourceGain');

  // Undo setRoute -> restores sourceLowPass
  res = await store.command({ op: 'history.undo' });
  assert.equal(res?.kind, 'ack');
  assert.equal(store.state.document.modulation.routes[0].target, 'sourceLowPass');

  // Validation guards: invalid targetObject for global target, bad rateHz, LFO on empty path
  const badGlobalTarget = await store.command({
    op: 'modulation.addRoute',
    route: sampleRoute({ target: 'mix', targetObject: 0, curve: { inputRange: [0, 10], outputRange: [0, 100], nodes: [{ x: 0, y: 0 }, { x: 1, y: 1 }] } }),
  });
  assert.equal(badGlobalTarget?.kind, 'error');

  const badEmptyLfo = await store.command({
    op: 'path.setLfo',
    object: 0,
    lfo: { enabled: true, shape: 'sine', rateHz: 2, depth: 0.25 },
  });
  assert.equal(badEmptyLfo?.kind, 'error');

  await assert.rejects(() => store.command({
    op: 'path.set',
    object: 0,
    points: [[1, 1, 1.5], [4, 1, 1.5]],
    lfo: { enabled: true, shape: 'sine', rateHz: 9, depth: 0.25 },
  }), /Contract validation/);

  // Set path & bake -> LFO and modulation must NOT mark bake outdated
  res = await store.command({ op: 'path.set', object: 0, points: [[1, 1, 1.5], [4, 1, 1.5]] });
  assert.equal(res?.kind, 'ack');
  res = await store.command({ op: 'path.bake', object: 0 });
  assert.equal(res?.kind, 'ack');
  const deadline = Date.now() + 2000;
  while (Date.now() < deadline && store.telemetry['status.load']?.pathBake?.state !== 'baked') {
    await new Promise(r => setTimeout(r, 10));
  }
  assert.equal(store.telemetry['status.load']?.pathBake?.state, 'baked');

  res = await store.command({
    op: 'path.setLfo',
    object: 0,
    lfo: { enabled: true, shape: 'triangle', rateHz: 2.5, depth: 0.3, phaseDeg: 45, seed: 7 },
  });
  assert.equal(res?.kind, 'ack');
  assert.equal(store.state.document.paths[0].lfo.shape, 'triangle');
  assert.equal(store.telemetry['status.load']?.pathBake?.state, 'baked');

  res = await store.command({ op: 'modulation.addRoute', route: sampleRoute({ object: 32, target: 'mix', targetObject: -1 }) });
  assert.equal(res?.kind, 'ack');
  assert.equal(store.telemetry['status.load']?.pathBake?.state, 'baked');

  // Budget cap: 64 routes max
  const sixtyFour = Array.from({ length: 64 }, () => sampleRoute());
  res = await store.command({ op: 'modulation.setRoutes', routes: sixtyFour });
  assert.equal(res?.kind, 'ack');
  assert.equal(store.state.document.modulation.routes.length, 64);

  const overBudget = await store.command({ op: 'modulation.addRoute', route: sampleRoute() });
  assert.equal(overBudget?.kind, 'error');
  assert.equal(overBudget?.payload?.code, 'budgetExceeded');

  // source.remove reindexing: source 0 deleted -> routes on source 0 removed, routes on source 1 shift to 0
  res = await store.command({ op: 'source.add' });
  assert.equal(res?.kind, 'ack');
  res = await store.command({
    op: 'modulation.setRoutes',
    routes: [
      sampleRoute({ object: 0, target: 'sourceGain' }),
      sampleRoute({ object: 1, target: 'sourceHighPass' }),
      sampleRoute({ object: 32, targetObject: 1, target: 'sourceLowShelf' }),
      sampleRoute({ object: 32, targetObject: -1, target: 'earlyLevel' }),
    ],
  });
  assert.equal(res?.kind, 'ack');
  res = await store.command({ op: 'source.remove', index: 0 });
  assert.equal(res?.kind, 'ack');
  assert.deepEqual(
    store.state.document.modulation.routes.map(r => [r.object, r.targetObject, r.target]),
    [
      [0, -1, 'sourceHighPass'],
      [32, 0, 'sourceLowShelf'],
      [32, -1, 'earlyLevel'],
    ]
  );

  // Undo source.remove restores source & routes
  res = await store.command({ op: 'history.undo' });
  assert.equal(res?.kind, 'ack');
  assert.equal(store.state.document.sources.length, 2);
  assert.equal(store.state.document.modulation.routes.length, 4);

  // Keep I/O preserves local source routes, drops local global routes across preset.load
  res = await store.command({ op: 'editor.setFlags', flags: { keepRouting: true } });
  assert.equal(res?.kind, 'ack');
  res = await store.command({ op: 'preset.load', source: 'builtin', id: '0' });
  assert.equal(res?.kind, 'ack');
  assert.deepEqual(
    store.state.document.modulation.routes.map(r => [r.object, r.targetObject, r.target]),
    [
      [0, -1, 'sourceGain'],
      [1, -1, 'sourceHighPass'],
      [32, 1, 'sourceLowShelf'],
    ]
  );

  // Clear routes & LFO
  res = await store.command({ op: 'modulation.removeRoute', index: 0 });
  assert.equal(res?.kind, 'ack');
  res = await store.command({ op: 'modulation.setRoutes', routes: [] });
  assert.equal(res?.kind, 'ack');
  assert.equal(store.state.document.modulation, undefined);

  res = await store.command({ op: 'path.clear', object: 0 });
  assert.equal(res?.kind, 'ack');
  assert.equal(store.state.document.paths[0].lfo, undefined);

  // G8: target 'none' ("Host output only"), hostOut 0..16 validation, Keep I/O & source.remove preservation
  res = await store.command({ op: 'source.add' });
  assert.equal(res?.kind, 'ack');
  res = await store.command({
    op: 'modulation.setRoutes',
    routes: [
      sampleRoute({ object: 0, target: 'sourceGain', hostOut: 3 }),
      sampleRoute({ object: 1, target: 'none', targetObject: -1, hostOut: 5, curve: { inputRange: [0, 10], outputRange: [0, 1], nodes: [{ x: 0, y: 0 }, { x: 1, y: 1 }] } }),
      sampleRoute({ object: 32, target: 'mix', targetObject: -1, hostOut: 7 })
    ]
  });
  assert.equal(res?.kind, 'ack');
  assert.equal(store.state.document.modulation.routes[1].target, 'none');
  assert.equal(store.state.document.modulation.routes[1].hostOut, 5);

  const badNoneTgtObj = await store.command({
    op: 'modulation.addRoute',
    route: sampleRoute({ target: 'none', targetObject: 0, hostOut: 1 })
  });
  assert.equal(badNoneTgtObj?.kind, 'error');

  await assert.rejects(() => store.command({
    op: 'modulation.addRoute',
    route: sampleRoute({ target: 'none', targetObject: -1, hostOut: 17 })
  }), /Contract validation/);

  // source.remove 0 drops route on 0, shifts route on 1 -> 0 while preserving hostOut=5
  res = await store.command({ op: 'source.remove', index: 0 });
  assert.equal(res?.kind, 'ack');
  assert.deepEqual(
    store.state.document.modulation.routes.map(r => [r.object, r.target, r.hostOut || 0]),
    [[0, 'none', 5], [32, 'mix', 7]]
  );
  res = await store.command({ op: 'history.undo' });
  assert.equal(res?.kind, 'ack');
  assert.equal(store.state.document.modulation.routes.length, 3);

  // Keep I/O preserves local source* (hostOut=3) + local none (hostOut=5), drops local room-global mix (hostOut=7)
  res = await store.command({ op: 'editor.setFlags', flags: { keepRouting: true } });
  assert.equal(res?.kind, 'ack');
  res = await store.command({ op: 'preset.load', source: 'builtin', id: '0' });
  assert.equal(res?.kind, 'ack');
  assert.deepEqual(
    store.state.document.modulation.routes.map(r => [r.object, r.target, r.hostOut || 0]),
    [[0, 'sourceGain', 3], [1, 'none', 5]]
  );

  res = await store.command({ op: 'modulation.setRoutes', routes: [] });
  assert.equal(res?.kind, 'ack');
});
