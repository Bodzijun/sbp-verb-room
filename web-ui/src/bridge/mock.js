import { clone, loadContract } from './contract.js';
import { canonical } from './parameters.js';
import { applyMutation, validateModel, updateEffective, updateHost, reject } from './mock-model.js';
import { makeSyntheticEnergy } from '../viewport/energy-layer.js';
import { mockClutterEvent, mockClutterSignature } from './mock-clutter.js';
export const FIXTURES = ['default-physical', 'legacy-creative', 'small-room', 'large-hall', 'energy-result', 'error-resync'];
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
export async function createMockBridge(options = {}) {
  const read = options.read || (async name => {
    const response = await fetch(new URL(`../../fixtures/${name}.json`, import.meta.url));
    if (!response.ok) throw new Error(`Cannot read fixture ${name}`); return response.json();
  });
  const validator = options.validator || await loadContract();
  const data = new Map(await Promise.all(['parameters.default', ...FIXTURES.map(n => n.startsWith('energy') || n.startsWith('error') ? n : `snapshot.${n}`)].map(async n => [n, await read(n)])));
  data.forEach((value, key) => validator.assert(value, key === 'parameters.default' ? 'fixture-parameters' : 'fixture'));
  const descriptors = data.get('parameters.default').descriptors, listeners = new Set(), parameterListeners = new Set();
  const audit = [], resources = new Map(), history = [], seen = new Map(), timers = new Set(), gestures = new Set();
  let state, selected = 'default-physical', sessionId = 'editor-mock', generation = 0, job = null, tx = null, closed = false, seq = 0, meterToken = 0, targetToken = 0, tailSubscribed = false, tailTimer = null;
  const latency = options.delayMs ?? 35;
  const emptyBake=()=>({state:'notBaked',object:-1,name:'',percent:0,reason:'',done:0,total:0,workers:0,paths:[]});
  const enabledPaths=doc=>(doc.paths||[]).map((path,object)=>({path,object})).filter(x=>x.path?.enabled&&x.path.points.length>=2);
  const pathGroupDriver=(paths,object)=>{const p=paths?.[object],g=p?.group??-1;if(g<0||!p?.enabled||(p.points?.length??0)<2)return object;for(let o=0;o<32;o++)if(paths[o]?.enabled&&paths[o].points?.length>=2&&(paths[o].group??-1)===g)return o;return object;};
  let bakeStatus=emptyBake(),bakeToken=0;
  function bakeEvent(){event({event:'status.load',state:state.load.state,status:state.load.status,sceneSerial:state.sceneSerial||'0',loadSerial:state.loadSerial||'0',tailCurrent:state.load.tailCurrent,rebuilding:false,pathBake:bakeStatus});}
  function emit(kind, payload, requestId) {
    const message = { schemaVersion: 1, sessionId, kind, ...(requestId ? { requestId } : {}), payload: clone(payload) };
    validator.assert(message); audit.push({ direction: 'in', message: clone(message) });
    if (audit.length > 4096) audit.splice(0, audit.length - 4096);
    listeners.forEach(fn => fn(message)); return message;
  }
  function event(payload) { return emit('event', payload); }
  function later(fn, ms) {
    const gen = generation; const id = setTimeout(() => { timers.delete(id); if (!closed && generation === gen) fn(); }, ms); timers.add(id);
  }
  function scheduleTail(ms = 30) {
    if (!tailSubscribed || closed) return;
    if (tailTimer) { clearTimeout(tailTimer); timers.delete(tailTimer); tailTimer = null; }
    const gen = generation;
    const id = setTimeout(() => { timers.delete(id); if (tailTimer === id) tailTimer = null; if (!closed && generation === gen) bridge.emitSyntheticTail(); }, ms);
    tailTimer = id; timers.add(id);
  }
  function stopJobs() { timers.forEach(clearTimeout); timers.clear(); tailTimer = null; job = null; meterToken++; }
  function historyEvent() { event({event:'history.changed',canUndo:history.length>0,undoDepth:history.length,canRedo:false,undoLabel:history.length ? (history.at(-1).undoLabel || 'Scene edit') : '',redoLabel:''}); }
  let clutterSignature = null;
  function clutterEvent(force = false) { const signature = mockClutterSignature(state); if (force || signature !== clutterSignature) { clutterSignature = signature; event(mockClutterEvent(state)); } }
  function snapshot() { const result = emit('snapshot', state); historyEvent(); clutterEvent(); return result; }
  function fresh(name, keepPreferences = false) {
    const old = state; generation++; stopJobs(); bakeToken++;bakeStatus=emptyBake();tx = null; gestures.clear(); history.length = 0; seen.clear(); resources.clear(); selected = name;
    clutterSignature = null;
    const fixture = data.get(name === 'error-resync' ? name : `snapshot.${name === 'energy-result' ? 'default-physical' : name}`);
    state = clone(fixture.messages.find(m => m.kind === 'snapshot').payload); state.parameters.descriptors = clone(descriptors);for(const d of descriptors.parameters)state.parameters.values[d.id] ??= d.default;
    state.capabilities = state.capabilities || {};
    state.capabilities.features = state.capabilities.features || {};
    state.capabilities.features['colours.source'] = { available: true, status: 'available' };
    if (keepPreferences && old) state.preferences = clone(old.preferences || {});
    if (name === 'error-resync') state.revision = '43';
    validateModel(state, validator);
  }
  function change(next, requestId, paths = ['document', 'host', 'parameters', 'preferences'], editedPathObject = null) {
    const docChanged=JSON.stringify(next.document)!==JSON.stringify(state.document);
    const bakeInvalid=docChanged&&(['baking','baked','approximate'].includes(bakeStatus.state)||(Array.isArray(bakeStatus.paths)&&bakeStatus.paths.some(x=>['baking','baked','approximate'].includes(x.state))));
    const oldPaths=state.document?.paths;
    const fromRevision = state.revision; next.revision = (BigInt(fromRevision) + 1n).toString();
    validateModel(next, validator); state = next;
    const ops = paths.filter(key => state[key] !== undefined).map(key => ({ op: 'replace', path: `/${key}`, value: clone(state[key]) }));
    emit('patch', { fromRevision, toRevision: state.revision, originRequestId: requestId, ops });
    clutterEvent();
    if(bakeInvalid){
      bakeToken++;
      const msg='Room, quality or path changed - bake again';
      let updatedPaths=bakeStatus.paths||[];
      if(Number.isInteger(editedPathObject)&&updatedPaths.length){
        const oldG=oldPaths?.[editedPathObject]?.group??-1;
        const newG=next.document?.paths?.[editedPathObject]?.group??-1;
        updatedPaths=updatedPaths.map(item=>(item.object===editedPathObject||(oldG>=0&&item.group===oldG)||(newG>=0&&item.group===newG))?{...item,state:'outdated',percent:0,reason:msg}:item);
      }else if(updatedPaths.length){
        updatedPaths=updatedPaths.map(item=>({...item,state:'outdated',percent:0,reason:msg}));
      }
      bakeStatus={...bakeStatus,state:'outdated',percent:0,reason:msg,paths:updatedPaths};
      bakeEvent();
    }
    return state.revision;
  }
  function replayEnergy() {
    data.get('energy-result').messages.forEach((m, index) => later(() => {
      if (m.payload.result) resources.set(m.payload.result.blob.fullUrl, makeSyntheticEnergy(m.payload.result));
      if (m.payload.event === 'analysis.result') job = null;
      event(m.payload);
    }, 80 + index * 180));
  }
  function relay(values = state.parameters.values, effective = state.parameters.effective) {
    validator.assert(values, 'parameters', 'Values'); validator.assert(effective, 'parameters', 'Effective');
    const message = { values: clone(values), effective: clone(effective) };
    audit.push({ direction: 'relay-in', message }); parameterListeners.forEach(fn => fn(message));
  }
  function parameterAction(type, id, value) {
    if (!state || closed) throw new Error('Bridge is not ready');
    const d = descriptors.parameters.find(p => p.id === id); if (!d) throw new Error('Unknown parameter');
    if (type === 'begin') { if (gestures.has(id)) throw new Error(`Gesture already open: ${id}`); gestures.add(id); }
    if (type === 'change') {
      if (!gestures.has(id)) throw new Error(`Change outside gesture: ${id}`);
      if (canonical(d, value) !== value) throw new Error(`Noncanonical parameter value: ${id}`);
      state.parameters.values[id] = value; updateEffective(state); relay();
      if (job?.resultId) event({ event: 'analysis.stale', resultId: job.resultId, stale: true });
      if (['size', 'absorption', 'decayLow', 'decayMid', 'decayHigh', 'mode'].includes(id)) scheduleTail(15);
    }
    if (type === 'end') gestures.delete(id);
    audit.push({ direction: 'relay-out', message: { type, id, ...(value !== undefined ? { value } : {}) } });
  }
  const bridge = {
    mode: 'mock', validator, audit,
    get selectedFixture() { return selected; },
    subscribe(fn) { listeners.add(fn); return () => listeners.delete(fn); },
    parameters: { subscribe(fn) { parameterListeners.add(fn); return () => parameterListeners.delete(fn); },
      begin: id => parameterAction('begin', id), change: (id, value) => parameterAction('change', id, value), end: id => parameterAction('end', id) },
    async send(message) {
      validator.assert(message); audit.push({ direction: 'out', message: clone(message) });
      if (closed) throw new Error('Bridge disposed');
      sessionId = message.sessionId; const gen = generation;
      await delay(latency);
      if (gen !== generation) return null;
      if (message.kind === 'hello') { if (!state) fresh(selected); const result = snapshot(); if (selected === 'energy-result') replayEnergy(); return result; }
      if (message.kind !== 'command') throw new Error('Only hello / command messages may be sent');
      if (seen.has(message.requestId)) return clone(seen.get(message.requestId));
      const p = message.payload, op = p.op;
      try {
        if (message.baseRevision && message.baseRevision !== state.revision) reject('staleRevision', 'Mock rejected an obsolete revision; requesting a fresh snapshot');
        const ack = extra => emit('ack', { requestId: message.requestId, revision: state.revision, status: 'accepted', ...extra }, message.requestId);
        let result;
        if(op==='path.recordPass'||op==='path.recordStop'){
          if(op==='path.recordPass'){
            const outdated=['baked','approximate'].includes(bakeStatus.state);
            bakeStatus={...bakeStatus,...(outdated?{state:'outdated',percent:0,reason:'Room, quality or recorded pass changed - bake again'}:{}),passState:1,passSamples:0};
          }else{
            bakeStatus={...bakeStatus,passState:3,passSamples:Math.max(bakeStatus.passSamples||0,16)};
          }
          bakeEvent();result=ack();
        }
        else if(op==='path.bakeClear'){
          bakeToken++;bakeStatus={...emptyBake(),passState:bakeStatus.passState??0,passSamples:bakeStatus.passSamples??0};bakeEvent();result=ack();
        }
        else if(op==='path.bake'||op==='path.bakeCancel'){
          const token=++bakeToken;
          const keepPass={passState:bakeStatus.passState??0,passSamples:bakeStatus.passSamples??0};
          if(op==='path.bakeCancel'){bakeStatus={...bakeStatus,...keepPass,state:'cancelled',percent:0,reason:'Cancelled',paths:(bakeStatus.paths||[]).map(x=>x.state==='baking'?{...x,state:'cancelled',percent:0,reason:'Cancelled'}:x)};bakeEvent();}
          else{
            const enabled=enabledPaths(state.document);
            const listenerActive=enabled.some(x=>x.object===32);
            const keyOf=x=>(x.path.group??-1)>=0?1000+x.path.group:(x.object===32?-1:x.object);
            const uniqueGroups=new Set(enabled.map(keyOf));
            const object=enabled.length===1?enabled[0].object:-1;
            const allowed=enabled.length>0&&(!listenerActive||uniqueGroups.size===1);
            const hasRecorded=keepPass.passState===3&&(keepPass.passSamples??0)>=2;
            if(!allowed&&!hasRecorded){
              const lGrp=state.document.paths?.[32]?.group??-1;
              const reason=enabled.length===0?'Enable a path or record a pass before baking':(listenerActive&&lGrp<0?'Cannot bake uncoupled moving listener with sources - assign them to one Group or record a pass':'Cannot bake uncoupled paths alongside a moving listener - couple all moving paths in one Group or record a pass');
              bakeStatus={...emptyBake(),...keepPass,state:'blocked',object,reason,paths:enabled.map(x=>({object:x.object,group:Number.isInteger(x.path.group)?x.path.group:-1,drivenBy:pathGroupDriver(state.document.paths,x.object),state:'blocked',percent:0,reason,done:0,total:0}))};
              bakeEvent();
            }else{
              const recordedMode=!allowed&&hasRecorded;
              const g0=enabled[0]?.path.group??-1;
              const name=recordedMode?'Recorded pass':(enabled.length===1?(object===32?'Listener':state.document.sources[object]?.name||`Source ${object+1}`):(uniqueGroups.size===1&&g0>=0?`Group ${g0+1} (${enabled.length} paths)`:`${enabled.length} paths`));
              const totalStations=64*Math.max(1,recordedMode?1:uniqueGroups.size);
              const items=enabled.map(x=>({object:x.object,group:Number.isInteger(x.path.group)?x.path.group:-1,drivenBy:pathGroupDriver(state.document.paths,x.object),state:'baking',percent:0,reason:'Tracing stations',done:0,total:64}));
              bakeStatus={...keepPass,state:'baking',object:recordedMode?-1:object,name,percent:0,reason:'Tracing stations',done:0,total:totalStations,workers:27,paths:items};
              bakeEvent();
              [15,45,72,100].forEach((percent,i)=>later(()=>{
                if(token!==bakeToken)return;
                const st=percent===100?'baked':'baking';
                const r=percent===100?'':percent<45?'Tracing stations':'Refining path';
                bakeStatus={...bakeStatus,state:st,percent,done:Math.round(percent*totalStations/100),reason:r,paths:bakeStatus.paths.map(x=>({...x,state:st,percent,done:Math.round(percent*.64),reason:r}))};
                bakeEvent();
              },(i+1)*(options.bakeDurationMs??1200)/4));
            }
          }
          result=ack();
        }
        else if (op === 'transaction.begin') { if (tx) reject('invalid', 'Another transaction is active'); if (!message.transactionId) reject('invalid', 'Transaction ID required'); tx = { id: message.transactionId, label: p.label, initial: clone(state), changed: false, commands: [] }; result = ack(); }
        else if (op === 'transaction.commit' || op === 'transaction.cancel') {
          if (!tx || tx.id !== message.transactionId) reject('invalid', 'Transaction not active');
          if (op === 'transaction.cancel' && tx.changed) change(clone(tx.initial), message.requestId);
          if (op === 'transaction.commit' && tx.changed) history.push({transactionDocument:clone(tx.initial.document), undoLabel:tx.label});
          tx = null; result = ack();
        } else if (op === 'telemetry.subscribe') {
          const token = ++meterToken;
          result = ack();
          if (p.topics.includes('meters')) {
            const tick = () => { if (token !== meterToken) return; event({ event: 'meters.input', seq: seq++, hostInputChannels: state.host.inputChannels, peaks: Array.from({ length: state.host.inputChannels }, (_, i) => 0.03 + 0.05 * (1 + Math.sin(seq / 8 + i))) }); later(tick, 40); };
            later(tick, 40);
          }
          if (p.topics.includes('tail')) { tailSubscribed = true; later(() => bridge.emitSyntheticTail(), 70); }
        } else if (op === 'preferences.set' || op === 'session.setViewState') {
          const next = clone(state); next.preferences = next.preferences || {};
          if (op === 'session.setViewState') {
            if (JSON.stringify(p.viewState).length > state.limits.maxViewStateBytes) reject('invalid', 'View state is too large');
            next.preferences.sessionViewState = p.viewState;
          } else {
            const { editorFlags, ...preferences } = p.patch;
            Object.assign(next.preferences, preferences); if (editorFlags) Object.assign(next.document.editorFlags, editorFlags);
          }
          change(next, message.requestId, ['preferences', 'document']); result = ack();
        } else if (op === 'analysis.start') {
          if (job?.running) reject('notReady', 'An analysis is already running');
          const id = `mock-job-${Date.now()}`, startRevision = state.revision;
          job = { id, running: true }; result = ack({ jobId: id });
          for (let i = 1; i <= 5; i++) later(() => {
            if (!job?.running || job.id !== id) return;
            event({ event: 'analysis.progress', jobId: id, state: 'running', progress: i / 5 });
            if (i === 5) {
              const descriptor = clone(data.get('energy-result').messages.find(m => m.payload.result).payload.result);
              const vertices = state.document.scene.walls.flatMap(w => [w.a, w.b]);
              const xs = vertices.map(v => v[0]), ys = vertices.map(v => v[1]);
              const width = Math.max(0.1, Math.max(1, ...xs) - Math.min(0, ...xs)), height = Math.max(0.1, Math.max(1, ...ys) - Math.min(0, ...ys));
              Object.assign(descriptor, { resultId: id, sceneRevision: startRevision, nx: Math.max(2, Math.ceil(p.resolution * width / Math.max(width, height))), ny: Math.max(2, Math.ceil(p.resolution * height / Math.max(width, height))), x0: Math.min(0, ...xs), y0: Math.min(0, ...ys), heightM: p.heightM, raysUsed: p.rays,
                options: { source: p.source, durationS: p.durationS, resolution: p.resolution, rays: p.rays, components: p.components, earlyDb: state.parameters.effective.earlyDb, tailDb: state.parameters.effective.tailDb } });
              descriptor.dx = width / descriptor.nx; descriptor.dy = height / descriptor.ny;
              const count = descriptor.nx * descriptor.ny, usable = p.heightM < state.document.scene.heightM || state.document.scene.ceilingMaterial === null;
              descriptor.validCells = usable ? count : 0;
              descriptor.blob = { layoutVersion: 1, fullUrl: `/analysis/${id}/full.bin`, bandUrlTemplate: `/analysis/${id}/band/{band}.bin`, maskByteLength: Math.ceil(count / 32) * 4, fullByteLength: Math.ceil(count / 32) * 4 + count * 84 };
              resources.set(descriptor.blob.fullUrl, makeSyntheticEnergy(descriptor, usable));
              job = { id, running: false, resultId: id }; event({ event: 'analysis.result', jobId: id, outcome: 'done', result: descriptor });
              if (state.revision !== startRevision) event({ event: 'analysis.stale', resultId: id, stale: true });
            }
          }, i * 140);
        } else if (op === 'analysis.cancel') { if (job?.running) { const id = job.id; job = null; event({ event: 'analysis.result', jobId: id, outcome: 'cancelled' }); } result = ack(); }
        else if (op === 'rt60.start') { const token = ++targetToken; result = ack({ jobId: 'mock-target' }); event({ event: 'status.rt60', state: 1, requestedS: p.targetS, achievedS: null }); later(() => { if (token === targetToken) event({ event: 'status.rt60', state: 3, requestedS: p.targetS, achievedS: null }); }, 400); }
        else if (op === 'rt60.cancel') { targetToken++; event({ event: 'status.rt60', state: 4 }); result = ack(); }
        else if (op === 'preset.load') {
          if (p.source !== 'builtin' || !/^\d$/.test(p.id || '') || Number(p.id) > 3) reject('ioError', 'Mock has only four fixture rooms; native FileChooser is not connected');
          const old = clone(state), name = FIXTURES[Number(p.id)]; fresh(name, true);
          delete state.preferences.sessionViewState;
          state.host = old.host;
          if (old.document.editorFlags.keepRouting) { state.document.sources = old.document.sources; state.document.output = old.document.output; }
          state.document.editorFlags = { ...state.document.editorFlags, ...Object.fromEntries(['keepRouting', 'keepProcessing', 'inputMeters', 'mixerCompact', 'navLocked', 'outlinerVisible'].filter(k => k in old.document.editorFlags).map(k => [k, old.document.editorFlags[k]])) };
          updateHost(state); validateModel(state, validator);
          state.revision = (BigInt(old.revision) + 1n).toString(); event({ event: 'resync.required', reason: 'asyncLoad', currentRevision: state.revision }); snapshot(); scheduleTail(35); result = ack();
        } else if (op === 'catalogs.list') {
          result = ack(); event({event:'catalogs.list',requestId:message.requestId,materials:clone(state.document.scene.materials),builtinRooms:FIXTURES.slice(0,4).map((name,id)=>({id,name})),userPresets:[]});
        } else if (op === 'reflections.query') { result = ack(); event({event:'diagnostics.paths',revision:state.revision,sceneSerial:state.sceneSerial || '0',sourceIndex:p.sourceIndex,paths:[],truncated:false,stale:false,engineOrder:1,receiver:'listener-centre'}); }
        else if (['preset.list', 'preset.save', 'ir.export', 'source.loadDirectivityFile'].includes(op)) reject('ioError', 'Mock only: native library / dialogs / file export are not connected');
        else if (op === 'history.redo' || op.startsWith('comparison.')) reject('unsupported', 'Capability unavailable in frozen v1');
        else if (op === 'history.undo') {
          if (!history.length) result = ack({ status: 'noChange' });
          else { const next = clone(state); const previous=history.pop(); if('makeupBefore' in previous) next.parameters.values.makeup=previous.makeupBefore; else if('pathObject' in previous) {next.document.paths[previous.pathObject]=previous.pathBefore;} else if(previous.transactionDocument) { next.document=previous.transactionDocument; if(previous.transactionParams) next.parameters.values=previous.transactionParams; } else next.document.scene=previous; change(next, message.requestId); event({ event: 'resync.required', reason: 'undo', currentRevision: state.revision }); snapshot(); scheduleTail(35); result = ack(); }
        } else {
          if (message.transactionId && tx?.id !== message.transactionId) reject('invalid', 'Unknown transaction');
          const next = clone(state), materials = [...data.values()].flatMap(f => f.messages?.find(m => m.kind === 'snapshot')?.payload.document.scene.materials || []);
          const commands = tx && message.transactionId ? [...tx.commands] : null;
          const slot = commands && op === 'surface.translate' ? commands.findIndex(command =>
            command.op === op && JSON.stringify(command.refs) === JSON.stringify(p.refs)) : -1;
          if (slot >= 0) {
            // Match native keyRefs coalescing even if Alt or autoLink changes during the drag.
            commands[slot] = p;
            next.document = clone(tx.initial.document);
            next.parameters.values = clone(tx.initial.parameters.values);
            for (const command of commands) applyMutation(next, command, tx.initial, materials);
          } else {
            applyMutation(next, p, tx?.initial || state, materials);
            commands?.push(p);
          }
          validateModel(next, validator);
          if (commands) tx.commands = commands;
          if (JSON.stringify(next) === JSON.stringify(state)) result = ack({ status: 'noChange' });
          else {
            if(!tx&&op.startsWith('path.'))history.push({pathObject:p.object,pathBefore:clone(state.document.paths?.[p.object]||{points:[],enabled:true,closed:false,smooth:false,orient:false,laps:1})});
            if (!tx && op === 'output.autoMakeup') history.push({makeupBefore:state.parameters.values.makeup});
            if (!tx && op === 'source.setConfig') history.push({ transactionDocument: clone(state.document), undoLabel: 'color' in p ? 'Source colour' : 'Edit source' });
            if (!tx && (op === 'room.scale' || op === 'source.setDirectivity' || op === 'source.setInstrument')) history.push({ transactionDocument: clone(state.document), transactionParams: clone(state.parameters.values) });
            else if (!tx && (op.startsWith('surface.') || ['room.setHeight', 'room.setOpening', 'room.clear'].includes(op))) history.push(clone(state.document.scene));
            history.splice(0, Math.max(0, history.length - 60)); if (tx) tx.changed = true;
            change(next, message.requestId, undefined, op.startsWith('path.') ? p.object : null);
            if (job?.resultId) event({ event: 'analysis.stale', resultId: job.resultId, stale: true });
            if (['surface.delete', 'surface.create', 'source.add', 'source.remove'].includes(op)) { event({ event: 'resync.required', reason: 'structuralChange', currentRevision: state.revision }); snapshot(); }
            if (/^(surface|room|tail)\./.test(op) || op === 'source.setDirectivity' || op === 'source.setInstrument') scheduleTail(35);
            result = ack();
          }
        }
        if ((/^(history|surface|room|transaction|path)\./.test(op)&&!['path.bake','path.bakeCancel','path.bakeClear'].includes(op)) || op === 'output.autoMakeup' || op === 'source.setDirectivity' || op === 'source.setInstrument' || (op === 'source.setConfig' && !tx)) historyEvent();
        seen.set(message.requestId, result); if (seen.size > 512) seen.delete(seen.keys().next().value); return result;
      } catch (error) {
        const result = emit('error', { requestId: message.requestId, code: error.code || 'invalid', message: error.message, currentRevision: state.revision, ...(error.code === 'staleRevision' ? { resync: true } : {}) }, message.requestId);
        if (error.code === 'staleRevision') { tx = null; event({ event: 'resync.required', reason: 'staleRevision', currentRevision: state.revision }); snapshot(); }
        seen.set(message.requestId, result); return result;
      }
    },
    async readResource(url) { await delay(latency); if (!resources.has(url)) throw new Error('Mock binary resource not available'); return resources.get(url).slice(0); },
    async selectFixture(name) { if (!FIXTURES.includes(name)) throw new Error('Unknown fixture'); fresh(name); snapshot(); if (name === 'energy-result') replayEnergy(); if (name === 'error-resync') later(() => bridge.injectResync(), 100); scheduleTail(35); },
    // Test hook: simulate a host channel-count / layout change; the next snapshot carries the new host block.
    setHost(patch) { Object.assign(state.host, patch); updateEffective(state); snapshot(); },
    injectResync() {
      const error = clone(data.get('error-resync').messages.find(m => m.kind === 'error').payload); error.currentRevision = state.revision;
      emit('error', error, error.requestId); event({ event: 'resync.required', reason: 'staleRevision', currentRevision: state.revision }); snapshot();
    },
    automate(id, value) { const d = descriptors.parameters.find(p => p.id === id); state.parameters.values[id] = canonical(d, value); updateEffective(state); relay(); if (['size', 'absorption', 'decayLow', 'decayMid', 'decayHigh', 'mode'].includes(id)) scheduleTail(15); },
    emitSyntheticTail() {
      const multipliers = state.parameters.effective.decay;
      const baseCurve = [2.8, 2.6, 2.2, 1.9, 1.6, 1.2, 0.9];
      const roomScale = selected === 'small-room' ? 0.2 : selected === 'large-hall' ? 2.4 : 1.0;
      const sizeScale = Math.max(0.2, (state.parameters.effective.sizePercent || 100) / 100);
      const absReq = Math.max(10, state.parameters.effective.absorption?.requested || 100) / 100;
      const scale = roomScale * sizeScale / Math.pow(absReq, 0.65);
      const physical = baseCurve.map(v => Math.max(0.08, Math.min(20, v * scale)));
      const factors = physical.map((_, b) => multipliers[b < 2 ? 0 : b < 5 ? 1 : 2]);
      const rt60 = physical.map((v, i) => Math.min(20, v * factors[i]));
      const durationS = Math.min(20, Math.max(1.5, Math.max(...rt60, ...physical) * 1.05));
      event({ event: 'diagnostics.tail', tailRevision: state.revision, durationS, points: 256,
        rt60, physicalRT60: physical, reliableSeconds: physical.map(v => v * 0.7), active: physical.map(() => true),
        multipliers, effectiveMultipliers: factors, reshapeStatus: rt60.map(v => v >= 20 ? 3 : 0),
        decayDb: rt60.map(v => Array.from({ length: 256 }, (_, j) => Math.max(-100, -60 * (j / 255 * durationS) / Math.max(0.05, v)))),
        physicalDecayDb: physical.map(v => Array.from({ length: 256 }, (_, j) => Math.max(-100, -60 * (j / 255 * durationS) / Math.max(0.05, v)))),
        raysUsed: state.document.tail.baseRays, rayLimit: state.document.tail.baseRays * 4, baseRays: state.document.tail.baseRays, shared: state.document.sources.length > 8, adaptive: state.document.tail.adaptive, needsMoreRays: false });
    },
    setCpuStatus(cpu) {
      event({ event: 'status.load', state: state.load.state, status: state.load.status, sceneSerial: state.sceneSerial || '0', loadSerial: state.loadSerial || '0', tailCurrent: state.load.tailCurrent, rebuilding: false, pathBake:bakeStatus,...(cpu ? { cpu } : {}) });
    },
    setRebuilding(rebuilding) {
      event({ event: 'status.load', state: state.load.state, status: state.load.status, sceneSerial: state.sceneSerial || '0', loadSerial: state.loadSerial || '0', tailCurrent: state.load.tailCurrent, rebuilding: !!rebuilding, pathBake: bakeStatus });
    },
    setBakeStatus(value){bakeToken++;bakeStatus={...emptyBake(),...value};bakeEvent();},
    dispose() { closed = true; stopJobs(); gestures.clear(); listeners.clear(); parameterListeners.clear(); },
    get openGestures() { return [...gestures]; }
  };
  fresh(options.fixture || 'default-physical'); return bridge;
}
