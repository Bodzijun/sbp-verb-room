import { clone } from '../bridge/contract.js';
function freeze(value) { if (value && typeof value === 'object' && !Object.isFrozen(value)) { Object.freeze(value); Object.values(value).forEach(freeze); } return value; }
export function createStore(bridge) {
  const listeners = new Set(), sessionId = `web-${Date.now()}-${Math.random().toString(36).slice(2)}`, transactions = new Map();
  let state = null, serial = 0, queue = Promise.resolve(), ready = false, epoch = 0, queued = 0;
  let telemetry = {}, error = null, cornerJoinResult = null;
  const view = { page: 'main', tab: 'decay', projection: 'plan', selection: [], energy: false, rays: false, tool: 'move', snap: true, autoLink: true, bands: [true, true, true, true, true, true, true] };
  const notify = reason => listeners.forEach(fn => fn({ state, telemetry, error, ready, view, reason }));
  let resyncPending = false;
  const requestSnapshot = () => {
    if (resyncPending) return; resyncPending = true;
    queueMicrotask(async () => { try { if (!ready) await bridge.send({ schemaVersion: 1, sessionId, kind: 'hello', payload: { supportedVersions: [1], ready: true } }); } catch (e) { error={code:'internal',message:e.message};notify('error'); } finally { resyncPending = false; } });
  };
  const resync = () => { ready = false; epoch++; transactions.clear(); notify('resync'); requestSnapshot(); };
  const receive = message => {
    bridge.validator.assert(message);
    const p = message.payload;
    if (message.kind === 'snapshot') {
      const first = state === null;
      if (state?.loadSerial !== p.loadSerial) cornerJoinResult = null;
      state = freeze(clone(p)); ready = true;
      if (first && p.preferences?.sessionViewState) Object.assign(view, clone(p.preferences.sessionViewState));
      notify('snapshot');
    } else if (message.kind === 'patch') {
      if (!state || p.fromRevision !== state.revision) { resync(); return; }
      const next = clone(state);
      for (const op of p.ops) {
        const keys = op.path.split('/').slice(1).map(k => k.replace(/~1/g, '/').replace(/~0/g, '~'));
        if (keys.some(k => ['__proto__', 'prototype', 'constructor'].includes(k))) throw new Error('Unsafe patch path');
        const parent = keys.slice(0, -1).reduce((obj, k) => obj[k], next), key = keys[keys.length - 1];
        if (op.op === 'remove') { if (Array.isArray(parent)) parent.splice(Number(key), 1); else delete parent[key]; }
        else parent[key] = clone(op.value);
      }
      next.revision = p.toRevision; state = freeze(next); notify('patch');
    } else if (message.kind === 'error') { error = p; if (p.resync) resync(); notify('error'); }
    else if (message.kind === 'event') {
      if (p.event === 'resync.required') { view.selection = []; resync(); }
      else {
        telemetry = { ...telemetry, [p.event]: clone(p) };
        if(p.event==='analysis.cleared') {
          delete telemetry['analysis.result'];
          delete telemetry['analysis.progress'];
          delete telemetry['analysis.stale'];
        }
        if(p.event==='analysis.result') {
          // A quick Direct-only job can finish before its first progress tick.
          // The previous job's 100% must not keep the new result "running".
          delete telemetry['analysis.progress'];
          if(p.outcome==='done')delete telemetry['analysis.stale'];
        }
        if (p.event === 'catalogs.list') telemetry['materials.catalogue'] = { event:'materials.catalogue', version:1, materials:p.materials.map(m=>({...m,source:'library'})) };
        if (p.event === 'diagnostics.paths') {
          const directFor=point=>p.paths.find(path=>path.order===0&&!path.diffracted&&path.pointIndex===point);
          telemetry['rays.paths'] = { event:'rays.paths',source:p.sourceIndex,revision:p.revision,stale:p.stale,truncated:p.truncated,pointCount:p.pointCount,budgetExceeded:p.budgetExceeded,
            paths:p.paths.map((path,i)=>({ ...path,id:`engine-${p.sourceIndex}-${i}`,points:path.pointsM,delayMs:path.delayS*1000,
              levels:path.bandEnergy.map((energy,b)=>10*Math.log10(Math.max(1e-30,energy)/Math.max(1e-30,directFor(path.pointIndex)?.bandEnergy[b] ?? 1))),
              surfaces:path.surfaces.map(s=>`${s.kind}:${s.index}`),synthetic:false })) };
        }
        notify(p.event);
      }
    }
  };
  const off = bridge.subscribe(receive);
  const offParams = bridge.parameters.subscribe(update => {
    if (!state) return;
    const next = { ...state, parameters: { ...state.parameters, values:update.values, effective:update.effective, ...(update.texts ? { texts:update.texts } : {}) } };
    state = freeze(next); notify('parameters');
  });
  const store = {
    mode: bridge.mode,
    get state() { return state; }, get ready() { return ready; }, get view() { return view; }, get telemetry() { return telemetry; }, get error() { return error; },
    get commandSlots() { return 256 - queued; },
    cornerJoinTolerance: .05,
    get cornerJoinResult() { return cornerJoinResult; },
    async joinCorners() {
      const loadSerial = state.loadSerial;
      const reply = await store.command({ op: 'scene.joinCorners', toleranceM: store.cornerJoinTolerance });
      if (reply?.kind === 'ack' && reply.payload.details && loadSerial === state.loadSerial) {
        cornerJoinResult = reply.payload.details;
        notify('view');
      }
    },
    subscribe(fn) { listeners.add(fn); return () => listeners.delete(fn); },
    async start() { await bridge.send({ schemaVersion: 1, sessionId, kind: 'hello', payload: { supportedVersions: [1], client: 'verb-room-t2', ready: true } }); await store.command({ op: 'telemetry.subscribe', topics: ['meters', 'tail', 'status', 'analysis'] }); if (bridge.mode === 'juce') await store.command({op:'catalogs.list'}); },
    command(payload, transactionId) {
      if (queued >= 256) { error = { code: 'notReady', message: 'Command queue is full; wait for the bridge' }; notify('error'); return Promise.resolve(null); }
      queued++;
      const begin = payload.op === 'transaction.begin', closing = ['transaction.commit','transaction.cancel'].includes(payload.op);
      if (begin) transactions.set(transactionId, { open:false, failed:false });
      const transaction = transactionId ? transactions.get(transactionId) : null;
      if (transaction && payload.op === 'transaction.cancel') transaction.cancelQueued = true;
      const commandEpoch = epoch;
      const work = async () => {
        if (!ready || !state || commandEpoch !== epoch) return null;
        // A queued update/end cannot outrun a rejected begin, or a resync.
        if (transactionId && !begin && (!transaction || transactions.get(transactionId) !== transaction || !transaction.open || transaction.failed && payload.op !== 'transaction.cancel')) return null;
        let recoveredRequest = null;
        for (let attempt = 0; attempt < 2; attempt++) {
          const message = { schemaVersion: 1, sessionId, kind: 'command', requestId: `${sessionId}-${++serial}`, baseRevision: state.revision, ...(transactionId ? { transactionId } : {}), payload };
          let reply;
          try {
            bridge.validator.assert(message);
            reply = await bridge.send(message);
          } catch (error) {
            if (transaction) transaction.failed = true;
            if (begin) transactions.delete(transactionId);
            throw error;
          }
          if (transaction) {
            if (begin) {
              transaction.open = reply?.kind === 'ack';
              if (!transaction.open) transactions.delete(transactionId);
            }
            if (closing && (reply?.kind === 'ack' || payload.op === 'transaction.cancel')) {
              transaction.open = false; transactions.delete(transactionId);
            }
            if (reply?.kind !== 'ack') {
              transaction.failed = true;
              if (!begin && !closing && !reply?.payload?.resync && !transaction.cancelQueued) store.command({op:'transaction.cancel'}, transactionId);
              notify('transactionfailed');
            }
          }
          // Retry queries, explicit preset/file actions and opaque view persistence after
          // a rejected stale request (the backend guarantees it has not executed them).
          // A geometry edit is never replayed against a different document.
          if (attempt === 0 && bridge.mode === 'juce' && ['session.setViewState','reflections.query','catalogs.list','telemetry.subscribe','preset.load','preset.save','ir.export','source.loadDirectivityFile'].includes(payload.op) && reply?.kind === 'error' && reply.payload.code === 'staleRevision' && ready) { recoveredRequest = message.requestId; continue; }
          if (recoveredRequest && reply?.kind !== 'error' && error?.requestId === recoveredRequest) { error = null; notify('dismiss'); }
          return reply;
        }
      };
      const result = queue.then(work).finally(() => { queued--; }); queue = result.catch(e => { error = { code: 'internal', message: e.message }; notify('error'); }); return result;
    },
    setView(patch, persist = true) {
      patch={...patch};
      if(patch.energy===true&&!view.energy&&view.projection!=='plan'){patch.energyPreviousView=view.projection;patch.projection='plan';}
      else if(patch.energy===false&&view.energy){if(view.energyPreviousView&&view.projection==='plan')patch.projection=view.energyPreviousView;patch.energyPreviousView=null;}
      else if('projection' in patch&&patch.projection!==view.projection)patch.energyPreviousView=null;
      Object.assign(view, patch);
      // Sticky ray source: clearing the selection (orbit, click on empty space, selecting the listener) keeps the rays of the last selected source.
      const selectedSource=view.selection.find(s=>s.kind==='source'), raySourceChanged=!!selectedSource&&selectedSource.index!==store.raySource;
      if(selectedSource)store.raySource=selectedSource.index;
      notify('view'); if (bridge.mode === 'juce' && ready && (raySourceChanged || 'rayOrder' in patch || patch.rays)) store.command({op:'reflections.query',sourceIndex:Math.min(store.raySource ?? 0,Math.max(0,(state?.document?.sources?.length ?? 1)-1)),maxOrder:Math.min(1,view.rayOrder ?? 1),maxPaths:128}); if (persist && ready) return store.command({ op: 'session.setViewState', viewState: clone(view) }); },
    async flags(patch) {
      if (bridge.mode === 'mock') return store.command({ op: 'preferences.set', patch: { editorFlags: patch } });
      const { layerModes, ...flags } = patch;
      let reply;
      if (Object.keys(flags).length) reply = await store.command({ op: 'editor.setFlags', flags });
      if (layerModes) for (const [key, mode] of Object.entries(layerModes)) reply = await store.command({ op: 'layers.set', layer: ['walls','floors','ceilings','plates'].indexOf(key), mode });
      return reply;
    },
    recover(error) {
      for(const [id,tx] of transactions)if(tx.open)store.command({op:'transaction.cancel'},id);
      view.selection=[];notify('transactionfailed');
      store.command({op:'session.setViewState',viewState:clone(view)});
    },
    clearError() { error = null; notify('dismiss'); },
    async fixture(name) { epoch++; ready = false; telemetry = {}; error = null; view.selection = []; view.camera = null; await bridge.selectFixture(name); await store.command({ op: 'telemetry.subscribe', topics: ['meters', 'tail', 'status', 'analysis'] }); },
    dispose() { off(); offParams(); listeners.clear(); }
  };
  return store;
}
