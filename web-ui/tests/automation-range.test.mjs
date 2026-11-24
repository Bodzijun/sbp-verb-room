import test from 'node:test';
import assert from 'node:assert/strict';
import { validator, readFixture } from './helpers.mjs';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';
import { computeFitRange, isDefaultAutomationRange } from '../src/bridge/mock-model.js';

const setup = async (name = 'default-physical') => {
  const bridge = await createMockBridge({ read: readFixture, validator, fixture: name, delayMs: 0, bakeDurationMs: 40 });
  const store = createStore(bridge);
  await store.start();
  return { bridge, store };
};

test('G3 fitAutomationRange computes margin, includes outside path points, preserves values and BakeCache', async t => {
  const { bridge, store } = await setup();
  t.after(() => { store.dispose(); bridge.dispose(); });

  await store.command({ op: 'room.clear' });
  await store.command({ op: 'room.setHeight', heightM: 4 });
  for (const [a, b] of [[[0, 0], [10, 0]], [[10, 0], [10, 8]], [[10, 8], [0, 8]], [[0, 8], [0, 0]]]) {
    await store.command({ op: 'surface.create', kind: 'wall', a, b });
  }
  await store.command({ op: 'listener.set', xM: 7, yM: 4, zM: 1.5 });
  bridge.parameters.begin('s1x'); bridge.parameters.change('s1x', 3); bridge.parameters.end('s1x');
  bridge.parameters.begin('s1y'); bridge.parameters.change('s1y', 4); bridge.parameters.end('s1y');
  await store.command({ op: 'path.set', object: 0, points: [[2, 2, 1.5], [12, 4, 1.5]], enabled: true });
  await store.command({ op: 'path.bake', object: 0 });
  await new Promise(r => setTimeout(r, 120));
  assert.equal(store.telemetry['status.load']?.pathBake?.state, 'baked');

  const preview = computeFitRange(store.state);
  assert.deepEqual(preview, { xM: [-1.5, 13.5], yM: [-1.5, 9.5], zM: [0, 5] });

  await store.command({ op: 'room.fitAutomationRange' });
  assert.deepEqual(store.state.document.scene.automationRange, { xM: [-1.5, 13.5], yM: [-1.5, 9.5], zM: [0, 5] });
  assert.equal(store.state.parameters.values.s1x, 3);
  assert.equal(store.state.parameters.values.lx, 7);
  const s1xDesc = store.state.parameters.descriptors.parameters.find(d => d.id === 's1x');
  assert.equal(s1xDesc.min, -1.5);
  assert.equal(s1xDesc.max, 13.5);
  assert.equal(store.telemetry['status.load']?.pathBake?.state, 'baked', 'Fit without clamping keeps bake current');
  bridge.audit.filter(a => ['in', 'out'].includes(a.direction)).forEach(a => validator.assert(a.message));
});

test('G3 narrowing clamps objects, single Undo restores range + coordinates, scale scales range', async t => {
  const { bridge, store } = await setup();
  t.after(() => { store.dispose(); bridge.dispose(); });

  await store.command({ op: 'listener.set', xM: 15, yM: 12, zM: 2 });
  bridge.parameters.begin('s1x'); bridge.parameters.change('s1x', 14); bridge.parameters.end('s1x');

  await store.command({ op: 'room.setAutomationRange', xM: [0, 8], yM: [0, 8], zM: [0, 4] });
  assert.deepEqual(store.state.document.scene.automationRange, { xM: [0, 8], yM: [0, 8], zM: [0, 4] });
  assert.equal(store.state.parameters.values.s1x, 8);
  assert.equal(store.state.parameters.values.lx, 8);
  assert.equal(store.state.parameters.values.ly, 8);

  const rejected = await store.command({ op: 'listener.set', xM: 12 });
  assert.equal(rejected.kind, 'error');

  await store.command({ op: 'history.undo' });
  assert.equal(isDefaultAutomationRange(store.state.document.scene.automationRange), true);
  assert.equal(store.state.parameters.values.s1x, 14);
  assert.equal(store.state.parameters.values.lx, 15);

  // room.scale scales non-default automationRange
  await store.command({ op: 'room.setAutomationRange', xM: [-2, 10], yM: [-2, 10], zM: [0, 5] });
  await store.command({ op: 'room.scale', factor: 2, includeObjects: true });
  const arAfter = store.state.document.scene.automationRange;
  assert.equal(arAfter.zM[1], 10);
  assert.ok(arAfter.xM[1] > 10);
});
