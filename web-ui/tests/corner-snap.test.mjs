import assert from 'node:assert/strict';
import test from 'node:test';
import { movedVertexCorners, sameCorner, snapTranslation, snapVertex, visibleCorners, wallCorners } from '../src/viewport/corner-snap.js';
import { joinCorners, openCornerCount } from '../src/state/join-corners.js';

const wall = (a, b, editor = {}) => ({ a, b, zBottomM: 0, zTopM: null, editor });
const scene = (walls, floorZones = [], ceilingZones = []) => ({ heightM: 3, walls, floorZones, ceilingZones });
const plan = ([x, y]) => [x * 100, y * 100];

test('F7 vertex prefers a visible corner over the grid, including when grid snap is off', () => {
  const xyz = [1.1234567890123457, .012345678901234567];
  const s = scene([wall(xyz, [2, 0])], [{ vertices: [[1.16, 0]], editor: { hidden: true } }]);
  const corners = visibleCorners(s, {}), raw = [1.177, .049];
  for (const step of [0, .1]) {
    const hit = snapVertex(raw, plan(raw), corners, plan, step, false);
    assert.equal(hit.snap, 'corner');
    assert.deepEqual(hit.world, xyz);
    assert.deepEqual(hit.target, { kind: 'wall', index: 0, vertex: 0 });
  }
  assert.deepEqual(snapVertex(raw, plan(raw), corners, plan, .1, true).world, raw);
  const distant = [1.235, .239];
  assert.deepEqual(snapVertex(distant, plan(distant), corners, plan, .1, false).world, [1.2, .2]);
  assert.deepEqual(snapVertex(distant, plan(distant), corners, plan, 0, false).world, distant);
});

test('F7 only moving endpoints are excluded; hidden and locked neighbours do not follow auto-link', () => {
  const s = scene([wall([0, 0], [1, 0]), wall([1, 0], [2, 1]), wall([1, 0], [2, 2], { locked: true }),
    wall([1, 0], [3, 3], { hidden: true })], [{ vertices: [[1, 0]], editor: {} }]);
  const dragged = { kind: 'wall', index: 0, vertex: 1 };
  const linked = movedVertexCorners(s, dragged, true, 0);
  assert.deepEqual(linked, [dragged, { kind: 'wall', index: 1, vertex: 0 }]);
  assert.deepEqual(movedVertexCorners(s, dragged, false, 0), [dragged]);
  const remaining = visibleCorners(s, {}).filter(c => !linked.some(ref => sameCorner(ref, c.ref)));
  assert.deepEqual(remaining.filter(c => c.xy[0] === 1 && c.xy[1] === 0).map(c => c.ref), [
    { kind: 'wall', index: 2, vertex: 0 }, { kind: 'floorZone', index: 0, vertex: 0 }
  ]);
  assert.deepEqual(visibleCorners(s, { walls: 2, floors: 2 }), []);
});

test('F7 translation picks the closest target across both walls and preserves vertical delta', () => {
  const moved = [wall([0, 1], [1, 1]), wall([1, 2], [2, 2])].flatMap((w, i) => wallCorners(w, i, 3));
  const targets = visibleCorners(scene([wall([2.06, 1], [3.02, 2]), wall([9, 1], [10, 2])]), {});
  const hit = snapTranslation([1, 0, .25], moved, targets, plan, false);
  assert.deepEqual(hit.from, { kind: 'wall', index: 1, vertex: 1 });
  assert.deepEqual(hit.target, { kind: 'wall', index: 0, vertex: 1 });
  assert.deepEqual(hit.delta, [3.02 - 2, 0, .25]);
  assert.deepEqual(hit.world, [3.02, 2]);
  assert.equal(snapTranslation([1, 0], moved, targets, plan, true), null);
  assert.equal(snapTranslation([.5, 0], moved, targets, plan, false), null);
});

test('F7 corner count counts transitive groups and requires a real gap between different walls', () => {
  const walls = [wall([0, 0], [0, 3]), wall([.03, 0], [9, 3], { hidden: true }),
    wall([.11, 0], [8, 1]), wall([6, 0], [6.03, 0]), wall([0, 3], [2, 4]),
    wall([8.03, 1], [7, 7]), wall([7.00008, 7], [4, 2])];
  assert.equal(openCornerCount(walls), 2);
  assert.equal(openCornerCount(walls, 1), 0);
  assert.equal(openCornerCount(walls.slice(3, 4)), 0);
  assert.equal(openCornerCount([wall([0, 0], [0, 1]), wall([.0001, 0], [1, 1])]), 0);
  assert.equal(openCornerCount([wall([0, 0], [0, 1]), wall([.1, 0], [1, 1])]), 0);
});

test('F7 mock repair is anchored, reports skipped groups, and does not collapse walls', () => {
  const anchored = [wall([0, 0], [1, 1]), wall([.04, 0], [2, 2], { locked: true }), wall([.08, 0], [3, 3], { hidden: true })];
  assert.deepEqual(joinCorners(anchored, .05), { joined: 1, skipped: 0 });
  assert.deepEqual(anchored.map(w => w.a), [[.04, 0], [.04, 0], [.04, 0]]);
  assert.equal(openCornerCount(anchored), 0);
  const collapsed = [wall([0, 0], [.04, 0]), wall([.02, 0], [3, 3])];
  assert.deepEqual(joinCorners(collapsed, .05), { joined: 0, skipped: 1 });
  assert.deepEqual(collapsed[0].b, [.04, 0]);
  assert.deepEqual(joinCorners([wall([0, 0], [1, 1]), wall([.04, 0], [2, 2]), wall([.08, 0], [3, 3])], .05), { joined: 0, skipped: 1 });
  assert.deepEqual(joinCorners([wall([0, 0], [1, 1], { locked: true }), wall([.03, 0], [2, 2], { locked: true })], .05), { joined: 0, skipped: 1 });
});
