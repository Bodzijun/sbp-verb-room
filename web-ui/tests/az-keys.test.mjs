import test from 'node:test';
import assert from 'node:assert/strict';
import {
  evaluateAzimuthKeys,
  hasActiveAzimuthKeys,
  pathPositionLocked,
  withPathPoses,
} from '../src/viewport/movement-path.js';
import { validator, readFixture } from './helpers.mjs';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';

const emptyPath = () => ({ points: [], enabled: true, closed: false, smooth: false, orient: false, laps: 1 });
const baseState = () => ({
  document: {
    output: { layout: 1, radiusM: 0.1, micPattern: 0 },
    sources: [
      { name: 'A', format: 0, firstIn: 0, sum: false },
      { name: 'B', format: 0, firstIn: 1, sum: false },
    ],
    paths: Array.from({ length: 33 }, emptyPath),
    scene: { heightM: 3 },
  },
  parameters: {
    values: {
      lx: 4, ly: 4, lz: 1.5, laz: 10,
      s1x: 7, s1y: 8, s1z: 1.5, s1az: 33,
      s2x: 1, s2y: 4, s2z: 1.5, s2az: -25,
      lpath: 0, s1path: 0, s2path: 0,
    },
    effective: {},
    descriptors: { parameters: [] },
  },
});

test('G1 JS evaluateAzimuthKeys matches C++ semantics incl. Hold, easings, ±180° wrap and ties-to-even', () => {
  assert.equal(evaluateAzimuthKeys([], 0.5), null);
  assert.equal(evaluateAzimuthKeys([{ t: 40, az: 75, easing: 'hold' }], 0), 75);
  assert.equal(evaluateAzimuthKeys([{ t: 40, az: 75, easing: 'hold' }], 1), 75);

  const keys = [
    { t: 20, az: -30, easing: 'hold' },
    { t: 40, az: 90, easing: 'linear' },
    { t: 60, az: 170, easing: 'easeInOut' },
    { t: 80, az: -170, easing: 'easeIn' },
    { t: 100, az: -90, easing: 'easeOut' },
  ];
  assert.equal(evaluateAzimuthKeys(keys, 0.0), -30);
  assert.equal(evaluateAzimuthKeys(keys, 0.3999), -30);
  assert.equal(evaluateAzimuthKeys(keys, 0.4), 90);
  assert.equal(evaluateAzimuthKeys(keys, 0.5), 130);
  assert.equal(evaluateAzimuthKeys(keys, 0.7), 180);
  const s = 0.25, eInOut = s * s * (3 - 2 * s);
  assert.ok(Math.abs(evaluateAzimuthKeys(keys, 0.65) - (170 + 20 * eInOut)) < 1e-12);
  assert.ok(Math.abs(evaluateAzimuthKeys(keys, 0.9) - (-170 + 80 * 0.25)) < 1e-12);

  // IEEE 754 std::remainder ties-to-even for 0 -> 180 midpoint (+180 -> +90, not -90)
  const halfTurn = [{ t: 0, az: 0, easing: 'linear' }, { t: 100, az: 180, easing: 'linear' }];
  assert.equal(evaluateAzimuthKeys(halfTurn, 0.5), 90);
});

test('G1 pathPositionLocked and withPathPoses respect Manual + moving path vs AlongPath/LookAt/disabled', () => {
  const s = baseState();
  s.document.paths[0] = {
    ...emptyPath(),
    points: [[1, 1, 1.5], [5, 1, 1.5]],
    azKeys: [{ t: 0, az: -60, easing: 'linear' }, { t: 100, az: 60, easing: 'linear' }],
  };
  s.parameters.values.s1path = 0.5;
  assert.equal(hasActiveAzimuthKeys(s.document.paths[0]), true);
  assert.equal(pathPositionLocked(s, 's1x'), true);
  assert.equal(pathPositionLocked(s, 's1az'), true);
  assert.equal(withPathPoses(s).parameters.values.s1az, 0);

  // Coupled group follower evaluates at driver's path parameter
  s.document.paths[0].group = 0;
  s.document.paths[1] = {
    ...emptyPath(),
    points: [[1, 3, 1.5], [5, 3, 1.5]],
    group: 0,
    azKeys: [{ t: 0, az: 170, easing: 'linear' }, { t: 100, az: -170, easing: 'linear' }],
  };
  s.parameters.values.s2path = 0.0;
  assert.equal(withPathPoses(s).parameters.values.s2az, 180);

  // AlongPath & LookAt store keys but leave az unlocked and use host az as offset
  s.document.paths[0].orient = true;
  assert.equal(hasActiveAzimuthKeys(s.document.paths[0]), false);
  assert.equal(pathPositionLocked(s, 's1az'), false);
  assert.equal(withPathPoses(s).parameters.values.s1az, 33);

  s.document.paths[0].orient = false;
  s.document.paths[0].lookAt = true;
  s.document.paths[0].lookPos = [3, 5, 1.5];
  assert.equal(pathPositionLocked(s, 's1az'), false);
  assert.ok(Math.abs(withPathPoses(s).parameters.values.s1az - (90 + 33)) < 1e-9);

  // Disabled path keeps azKeys stored but unlocked & inactive
  s.document.paths[0].lookAt = false;
  s.document.paths[0].enabled = false;
  assert.equal(pathPositionLocked(s, 's1az'), false);
  assert.equal(withPathPoses(s).parameters.values.s1az, 33);
});

test('G1 mock bridge: azKeys commands, schema validation, bake invalidation, tx undo & source.remove', async t => {
  const bridge = await createMockBridge({ read: readFixture, validator, delayMs: 0, bakeDurationMs: 25 });
  const store = createStore(bridge);
  t.after(() => { store.dispose(); bridge.dispose(); });
  await store.start();
  const path = o => store.state.document.paths[o];

  const badNoPath = await store.command({ op: 'path.setAzimuthKey', object: 0, t: 25, az: 45 });
  assert.equal(badNoPath?.kind, 'error');

  const waitReady = async () => {
    const t0 = Date.now();
    while (Date.now() < t0 + 2000 && !store.ready) await new Promise(r => setTimeout(r, 5));
    assert.equal(store.ready, true);
  };

  let res = await store.command({ op: 'path.set', object: 0, points: [[1, 1, 1.5], [4, 1, 1.5]] });
  assert.equal(res?.kind, 'ack');
  res = await store.command({ op: 'path.setAzimuthKey', object: 0, t: 75, az: -90, easing: 'easeInOut' });
  assert.equal(res?.kind, 'ack');
  res = await store.command({ op: 'path.setAzimuthKey', object: 0, t: 25, az: 45, easing: 'hold' });
  assert.equal(res?.kind, 'ack');
  assert.deepEqual(path(0).azKeys, [
    { t: 25, az: 45, easing: 'hold' },
    { t: 75, az: -90, easing: 'easeInOut' },
  ]);

  // Bake then edit azKey -> outdated
  res = await store.command({ op: 'path.bake', object: 0 });
  assert.equal(res?.kind, 'ack');
  const deadline = Date.now() + 2000;
  while (Date.now() < deadline && store.telemetry['status.load']?.pathBake?.state !== 'baked') {
    await new Promise(r => setTimeout(r, 10));
  }
  assert.equal(store.telemetry['status.load']?.pathBake?.state, 'baked');

  res = await store.command({ op: 'transaction.begin', label: 'Edit azKey' }, 'tx-az');
  assert.equal(res?.kind, 'ack');
  res = await store.command({ op: 'path.setAzimuthKey', object: 0, index: 0, az: 90 }, 'tx-az');
  assert.equal(res?.kind, 'ack');
  res = await store.command({ op: 'path.setAzimuthKey', object: 0, index: 0, az: 120 }, 'tx-az');
  assert.equal(res?.kind, 'ack');
  res = await store.command({ op: 'transaction.commit' }, 'tx-az');
  assert.equal(res?.kind, 'ack');
  assert.equal(path(0).azKeys[0].az, 120);
  assert.equal(store.telemetry['status.load']?.pathBake?.state, 'outdated');

  res = await store.command({ op: 'history.undo' });
  assert.equal(res?.kind, 'ack');
  assert.equal(path(0).azKeys[0].az, 45);

  res = await store.command({ op: 'path.removeAzimuthKey', object: 0, index: 0 });
  assert.equal(res?.kind, 'ack');
  assert.equal(path(0).azKeys.length, 1);

  res = await store.command({ op: 'path.setAzimuthKeys', object: 0, keys: [{ t: 10, az: 30 }, { t: 90, az: -30 }] });
  assert.equal(res?.kind, 'ack');
  res = await store.command({ op: 'path.clear', object: 0 });
  assert.equal(res?.kind, 'ack');
  assert.equal(path(0).azKeys, undefined);

  // source.add -> 2 sources -> source.remove shifts azKeys cleanly
  res = await store.command({ op: 'source.add' });
  assert.equal(res?.kind, 'ack');
  await waitReady();
  res = await store.command({ op: 'path.set', object: 1, points: [[2, 2, 1.5], [3, 2, 1.5]], azKeys: [{ t: 50, az: 111, easing: 'hold' }] });
  assert.equal(res?.kind, 'ack');
  res = await store.command({ op: 'source.remove', index: 0 });
  assert.equal(res?.kind, 'ack');
  await waitReady();
  assert.deepEqual(path(0).azKeys, [{ t: 50, az: 111, easing: 'hold' }]);
});
