import test from 'node:test';
import assert from 'node:assert/strict';
import { readdir } from 'node:fs/promises';
import { validator, readFixture } from './helpers.mjs';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';
import { decodeEnergy, projectEnergy, makeSyntheticEnergy } from '../src/viewport/energy-layer.js';
const setup = async name => { const bridge = await createMockBridge({ read: readFixture, validator, fixture: name, delayMs: 0 }); const store = createStore(bridge); await store.start(); return { bridge, store }; };
test('all frozen fixtures and their messages validate offline', async () => {
  const files = await readdir(new URL('../fixtures', import.meta.url));
  for (const file of files) { const value = await readFixture(file.slice(0, -5)); validator.assert(value, file.startsWith('parameters') ? 'fixture-parameters' : 'fixture'); value.messages?.forEach(m => validator.assert(m)); }
});
test('AmbiX 2nd/3rd order layout indices validate in state and commands', async () => {
  const fixture = await readFixture('snapshot.default-physical');
  const msg = JSON.parse(JSON.stringify(fixture.messages.find(m => m.payload?.host?.effectiveLayout !== undefined)));
  for (const layout of [9, 10, 11]) { msg.payload.host.effectiveLayout = layout; msg.payload.document.output.layout = layout; validator.assert(msg); }
  msg.payload.host.effectiveLayout = 12; assert.throws(() => validator.assert(msg));
  const cmd = { schemaVersion: 1, sessionId: 'test', kind: 'command', requestId: 'x', baseRevision: '1', payload: { op: 'output.set', layout: 11 } };
  validator.assert(cmd); cmd.payload.layout = 12; assert.throws(() => validator.assert(cmd));
});
test('negative messages are rejected before exchange', () => {
  const envelope = { schemaVersion: 1, sessionId: 'test', kind: 'command', requestId: 'x', baseRevision: '1', payload: { op: 'room.setHeight', heightM: 0 } };
  assert.throws(() => validator.assert(envelope)); envelope.payload.heightM = 3; delete envelope.baseRevision; assert.throws(() => validator.assert(envelope));
  envelope.baseRevision = '1'; envelope.payload.extra = true; assert.throws(() => validator.assert(envelope));
});
test('canonical patches, immutable store, stale resync, idempotent commands', async t => {
  const { bridge, store } = await setup(); t.after(() => { store.dispose(); bridge.dispose(); });
  assert.throws(() => { store.state.document.scene.heightM = 4; });
  await store.command({ op: 'room.setHeight', heightM: 4 }); assert.equal(store.state.document.scene.heightM, 4); assert.equal(store.state.revision, '2');
  const message = { schemaVersion: 1, sessionId: 'test', kind: 'command', requestId: 'dedup', baseRevision: '2', payload: { op: 'source.add' } };
  await bridge.send(message); await bridge.send(message); assert.equal(store.state.document.sources.length, 2);
  const reply = await bridge.send({ ...message, requestId: 'stale', baseRevision: '0' }); assert.equal(reply.payload.code, 'staleRevision'); assert.equal(store.ready, true);
  bridge.audit.filter(a => ['in', 'out'].includes(a.direction)).forEach(a => validator.assert(a.message));
});
test('transaction deltas are from initial state; cancel restores it', async t => {
  const { bridge, store } = await setup(); t.after(() => { store.dispose(); bridge.dispose(); });
  const before = store.state.document.scene.walls[0].a[0];
  await store.command({ op: 'transaction.begin', label: 'Test' }, 'drag');
  await store.command({ op: 'surface.translate', refs: [{ kind: 'wall', index: 0 }], deltaM: [1, 0] }, 'drag');
  await store.command({ op: 'surface.translate', refs: [{ kind: 'wall', index: 0 }], deltaM: [2, 0] }, 'drag');
  assert.equal(store.state.document.scene.walls[0].a[0], before + 2);
  await store.command({ op: 'transaction.cancel' }, 'drag'); assert.equal(store.state.document.scene.walls[0].a[0], before);
});
test('locked, zero length, budgets reject atomically; metadata remains editable', async t => {
  const { bridge, store } = await setup('error-resync'); t.after(() => { store.dispose(); bridge.dispose(); });
  assert.equal((await store.command({ op: 'surface.translate', refs: [{ kind: 'wall', index: 0 }], deltaM: [1, 0] })).payload.code, 'locked');
  assert.equal((await store.command({ op: 'surface.create', kind: 'wall', a: [0, 0], b: [0, 0] })).payload.code, 'invalid');
  assert.equal((await store.command({ op: 'source.setConfig', index: 0, firstIn: 63, format: 1 })).payload.code, 'budgetExceeded');
  await store.command({ op: 'surface.setMetadata', refs: [{ kind: 'wall', index: 0 }], locked: false }); assert.equal(store.state.document.scene.walls[0].editor.locked, false);
});
test('parameter relays have gestures, automation no echo, Physical preserves Creative stored values', async t => {
  const { bridge, store } = await setup('legacy-creative'); t.after(() => { store.dispose(); bridge.dispose(); });
  const original = store.state.parameters.values.early;
  bridge.parameters.begin('mode'); bridge.parameters.change('mode', 0); bridge.parameters.end('mode');
  assert.equal(store.state.parameters.values.early, original); assert.equal(store.state.parameters.effective.earlyDb, 0);
  const before = bridge.audit.filter(v => v.direction === 'relay-out').length; bridge.automate('mix', 37.5);
  assert.equal(store.state.parameters.values.mix, 37.5); assert.equal(bridge.audit.filter(v => v.direction === 'relay-out').length, before);
  assert.deepEqual(bridge.openGestures, []); assert.throws(() => bridge.parameters.change('mix', 50));
});
test('binary energy decoder components, contrast, invalid cells and bad length', async () => {
  const result = (await readFixture('energy-result')).messages.find(m => m.payload.result).payload.result;
  const data = decodeEnergy(makeSyntheticEnergy(result), result), all = data.energy(30, -1, 7);
  assert.ok(Math.abs(all - (data.energy(30, -1, 1) + data.energy(30, -1, 2) + data.energy(30, -1, 4))) < 1e-6);
  assert.equal(Math.max(...projectEnergy(data, -1, 7).db), 0); assert.equal(decodeEnergy(makeSyntheticEnergy(result, false), result).energy(0), null);
  assert.throws(() => decodeEnergy(new ArrayBuffer(1), result));
});
test('preset mock Keep I/O preserves setup and actual host bus counts; Match track resolves from host', async t => {
  const { bridge, store } = await setup('legacy-creative'); t.after(() => { store.dispose(); bridge.dispose(); });
  await store.flags({ keepRouting: true });
  await store.command({ op: 'output.set', ambisonicsNormalization: 1, ambisonicsLevelDb: -3 });
  const sources = JSON.parse(JSON.stringify(store.state.document.sources)), output = JSON.parse(JSON.stringify(store.state.document.output));
  await store.command({ op: 'preset.load', source: 'builtin', id: '3' });
  assert.deepEqual(store.state.document.sources, sources); assert.deepEqual(store.state.document.output, output);
  assert.equal(store.state.host.inputChannels, 2); assert.equal(store.state.host.numPoints, 2);
  await store.command({ op: 'output.set', layout: 0 }); assert.equal(store.state.host.effectiveLayout, 0);
  await store.command({ op: 'output.set', layout: -1 }); assert.equal(store.state.host.effectiveLayout, 1);
});
test('material edits clone per surface, preserve other bands and clamp alpha + tau', async t => {
  const { bridge, store } = await setup(); t.after(() => { store.dispose(); bridge.dispose(); });
  const initial = JSON.parse(JSON.stringify(store.state.document.scene.materials[0]));
  await store.command({ op: 'surface.setAbsorption', refs: [{ kind: 'wall', index: 0 }], bands: [{ band: 0, alpha: 0.6 }] });
  await store.command({ op: 'surface.setTransmission', refs: [{ kind: 'wall', index: 0 }], bands: [{ band: 0, tau: 0.8 }] });
  const scene = store.state.document.scene, edited = scene.materials[scene.walls[0].material];
  assert.equal(edited.alpha[0], 0.6); assert.equal(edited.tau[0], 0.4); assert.deepEqual(edited.alpha.slice(1), initial.alpha.slice(1));
  assert.deepEqual(scene.materials[scene.walls[1].material], initial);
});
test('mock RT60 cancel suppresses late completion; unusable probe has a valid empty binary mask', async t => {
  const { bridge, store } = await setup(); t.after(() => { store.dispose(); bridge.dispose(); });
  await store.command({ op: 'rt60.start', targetS: 3 }); await store.command({ op: 'rt60.cancel' });
  await new Promise(resolve => setTimeout(resolve, 440)); assert.equal(store.telemetry['status.rt60'].state, 4);
  await store.command({ op: 'analysis.start', source: -1, heightM: 30, durationS: 20, resolution: 16, rays: 12000, components: 7 });
  await new Promise(resolve => setTimeout(resolve, 740));
  const result = store.telemetry['analysis.result'].result; assert.equal(result.validCells, 0);
  const decoded = decodeEnergy(await bridge.readResource(result.blob.fullUrl), result); assert.equal(decoded.energy(0), null);
});
