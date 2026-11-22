import test from 'node:test';
import assert from 'node:assert/strict';
import { validator, readFixture } from './helpers.mjs';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';
import { budget } from '../src/state/selectors.js';
import { applySourceBatch, canSum, planSourceFormat, planSourceSum, sumState } from '../src/pages/source-actions.js';

const source = (format, channels, firstIn, sum = false) => ({ format, channels, firstIn, sum });

async function setup(t, fixture) {
  const bridge = await createMockBridge({ read: readFixture, validator, fixture, delayMs: 0 });
  const store = createStore(bridge);
  await store.start();
  t.after(() => { store.dispose(); bridge.dispose(); });
  return { store, bridge };
}

function batchCommands(bridge) {
  return bridge.audit.filter(entry => entry.direction === 'out' && entry.message.transactionId)
    .map(entry => entry.message.payload);
}

test('F5 format preserves skipped routes and never places a new source over them', () => {
  const sources = [source(0, 1, 0), source(0, 1, 3), source(0, 1, 4)];
  const plan = planSourceFormat(sources, 1, 5, 64);
  assert.deepEqual(plan, {
    assignments: [{ index: 0, firstIn: 0, format: 1 }, { index: 1, firstIn: 2, format: 1 }],
    skippedSources: 1, missingChannels: 1, excessPoints: 0
  });
  sources[2].firstIn = 2;
  assert.equal(planSourceFormat(sources, 1, 5, 64).assignments[1].firstIn, 3);
});

test('F5 format counts points and applies reductions before increases', () => {
  const sources = [source(0, 1, 0), source(3, 4, 1)];
  const plan = planSourceFormat(sources, 1, 8, 5);
  assert.deepEqual(plan.assignments, [{ index: 1, firstIn: 2, format: 1 }, { index: 0, firstIn: 0, format: 1 }]);
  assert.equal(planSourceFormat(sources, 6, 64, 5).excessPoints, 9);
  const ring = planSourceFormat(sources, 7, 8, 5);
  assert.deepEqual(ring.assignments, [{ index: 0, firstIn: 0, format: 7 }, { index: 1, firstIn: 1, format: 7 }]);
  assert.equal(canSum({ format: 7, channels: 1 }), false);
});

test('F5 format for all uses one transaction and one undo, even with Keep I/O', async t => {
  const { store, bridge } = await setup(t, 'large-hall');
  await store.flags({ keepRouting: true });
  const before = store.state.document;
  const plan = planSourceFormat(before.sources, 0, store.state.host.inputChannels, store.state.limits.maxPoints);
  assert.equal(await applySourceBatch(store, 'All formats', plan.assignments), '');
  assert.deepEqual(batchCommands(bridge).map(p => p.op), [
    'transaction.begin', ...Array(32).fill('source.setConfig'), 'transaction.commit'
  ]);
  assert.deepEqual(budget(store.state.document), { inputs: 32, points: 32, overlap: false });
  assert.deepEqual(store.state.document.sources.map(s => s.firstIn), Array.from({ length: 32 }, (_, i) => i));
  assert.equal(store.state.document.editorFlags.keepRouting, true);
  assert.equal(store.telemetry['history.changed'].undoDepth, 1);
  await store.command({ op: 'history.undo' });
  assert.deepEqual(store.state.document, before);
});

test('F5 rejected format update cancels earlier writes before an unrelated command', async t => {
  const { store, bridge } = await setup(t, 'large-hall');
  const before = store.state.document;
  const result = applySourceBatch(store, 'All formats', [
    { index: 0, format: 0, firstIn: 0 }, { index: 31, format: 1, firstIn: 63 }
  ]);
  const queued = store.command({ op: 'source.setConfig', index: 0, name: 'Later edit' });
  assert.match(await result, /All formats cancelled.*budget/i);
  assert.equal((await queued).kind, 'ack');
  assert.deepEqual(batchCommands(bridge).map(p => p.op), [
    'transaction.begin', 'source.setConfig', 'source.setConfig', 'transaction.cancel'
  ]);
  assert.equal(store.state.document.sources[0].format, before.sources[0].format);
  assert.equal(store.state.document.sources[0].name, 'Later edit');
  assert.equal(store.telemetry['history.changed'].undoDepth, 1);
  await store.command({ op: 'history.undo' });
  assert.deepEqual(store.state.document, before);
});

test('F5 missing acknowledgement prevents commit and cancels the batch', async t => {
  const { store, bridge } = await setup(t, 'legacy-creative');
  const before = store.state.document;
  const send = bridge.send;
  bridge.send = message => {
    if (message.payload.op === 'source.setConfig' && message.payload.index === 1) return Promise.resolve(null);
    return send(message);
  };
  assert.match(await applySourceBatch(store, 'All formats', [
    { index: 0, format: 1, firstIn: 0 }, { index: 1, format: 1, firstIn: 2 }
  ]), /All formats cancelled/);
  assert.equal(batchCommands(bridge).at(-1).op, 'transaction.cancel');
  assert.deepEqual(store.state.document, before);
});

test('F5 mixed Sum becomes all on and restores mixed state with one undo', async t => {
  const { store, bridge } = await setup(t, 'large-hall');
  await store.command({ op: 'source.setConfig', index: 0, format: 0 });
  await store.command({ op: 'source.setConfig', index: 1, sum: false, freePoints: true });
  const before = store.state.document.sources;
  const depth = store.telemetry['history.changed'].undoDepth;
  assert.equal(sumState(before), 'mixed');
  assert.equal(await applySourceBatch(store, 'Sum all on', planSourceSum(before, true)), '');
  assert.deepEqual(batchCommands(bridge)[1], { op: 'source.setConfig', index: 1, sum: true });
  const after = store.state.document.sources;
  assert.equal(sumState(after), 'on');
  assert.deepEqual(after[0], before[0]);
  assert.deepEqual(after[1], { ...before[1], sum: true });
  assert.equal(store.telemetry['history.changed'].undoDepth, depth + 1);
  await store.command({ op: 'history.undo' });
  assert.deepEqual(store.state.document.sources, before);
  assert.equal(sumState(store.state.document.sources), 'mixed');
  assert.equal(await applySourceBatch(store, 'Sum all off', planSourceSum(before, false)), '');
  assert.equal(sumState(store.state.document.sources), 'off');
  assert.deepEqual(store.state.document.sources[0], before[0]);
});
