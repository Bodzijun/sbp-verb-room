import { clone, loadContract } from './contract.js';
import { canonical } from './parameters.js';

// One native call per structural request, one ordered batch per parameter turn.
// Values/texts come from C++; incoming automation is never sent back to the host.
export function createJuceBridge(options = {}) {
  const listeners = new Set(), paramListeners = new Set(), removers = [];
  let contract = options.validator, native, backend, keys, timer, closed = false, seq = 0;
  let lastLoad = null, values = {}, effective = {}, texts = {}, descriptors = [], batch = [], scheduled = false;
  const gestures = new Set(), snapshotWaiters = new Set();
  const validator = { assert(...args) { if (!contract) throw new Error('Bridge contract is loading'); return contract.assert(...args); } };
  function reportError(error) {
    const detail={code:'internal',message:error?.message||String(error)};
    options.onError?.(detail);
    for(const id of [...gestures]){gestures.delete(id);try{enqueue({type:'end',id});}catch{}}
    if(globalThis.window?.dispatchEvent&&typeof CustomEvent!=='undefined')window.dispatchEvent(new CustomEvent('verb-bridge-error',{detail}));
  }
  const deliver=(callbacks,value)=>{for(const fn of callbacks)try{fn(value);}catch(error){reportError(error);}};
  function receive(message) {
    try { receiveMessage(message); } catch(error) { reportError(error); }
  }
  function receiveMessage(message) {
    if (closed) return;
    validator.assert(message);
    if (message.kind === 'snapshot') {
      if (lastLoad !== null && lastLoad !== message.payload.loadSerial) {
        // A source/output rebuild replaces the native parameter gesture lifetime.
        // Cancel the local pointer before clearing the matching JS gesture ids.
        if(globalThis.window?.dispatchEvent)window.dispatchEvent(new Event('verb-gestures-reset'));
        gestures.clear();
      }
      lastLoad = message.payload.loadSerial;
      ({ values, effective } = clone(message.payload.parameters));
      texts = clone(message.payload.parameters.texts || {});
      descriptors = message.payload.parameters.descriptors?.parameters || descriptors;
      snapshotWaiters.forEach(w => { clearTimeout(w.timer); w.resolve(message); }); snapshotWaiters.clear();
    }
    if(message.kind==='error')reportError(new Error(message.payload.message));
    deliver(listeners,message);
  }
  function relay(update) {try {
    if (closed) return;
    contract.assert(update, 'parameters', 'Update');
    values = clone(update.values); effective = clone(update.effective); texts = clone(update.texts || {});
    deliver(paramListeners,{values,effective,texts});
  }catch(error){reportError(error);}}
  async function initialise() {
    contract ||= await loadContract(options.readSchema);
    if (closed) return;
    if (options.native) { native = options.native; backend = options.backend; }
    else {
      if (!globalThis.window?.__JUCE__?.backend) throw new Error('Native JUCE backend is unavailable');
      const { getNativeFunction } = await import('./native/juce/index.js');
      backend = window.__JUCE__.backend; native = getNativeFunction;
      const { installHostKeys } = await import('./native/host-keys.js');
      keys = installHostKeys({ editorOwnsKeys:true, send: payload => native('hostKey')(payload) });
    }
    const on = (name, fn) => { const token = backend?.addEventListener(name, fn); if (token !== undefined) removers.push(() => backend.removeEventListener(token)); };
    on('verb.message', receive); on('verb.parameters', relay);
    const ping = async () => {try {
      if (closed || globalThis.document?.hidden) return;
      const n = ++seq, reply = await native('ping')({ seq: n });
      backend?.emitEvent('verb.pingDone', { seq: n, ok: reply?.seq === n });
    }catch(error){reportError(error);}};
    on('verb.pingRequest', ping);
    on('verb.hello', hello => { keys?.setEnabled(hello.capabilities?.hostKeys !== false); clearInterval(timer); timer = setInterval(ping, hello.heartbeatMs || 2000); });
    const size = () => backend?.emitEvent('verb.pageSize', { w: globalThis.innerWidth || 0, h: globalThis.innerHeight || 0 });
    if (!options.native) { window.addEventListener('resize', size); removers.push(() => window.removeEventListener('resize', size)); }
    await native('ready')({ pageVersion: 't4-real-ui', supportedSchemaVersions: [1] }); size();
  }
  const initialised = initialise();
  let transport = Promise.resolve();
  function enqueue(action) {
    if (closed) throw new Error('Bridge disposed');
    batch.push(action);
    if (!scheduled) { scheduled = true; queueMicrotask(() => { scheduled = false; const actions = batch; batch = []; transport = transport.then(async () => { await initialised; if (!closed) { validator.assert(actions, 'parameters', 'GestureBatch'); const reply = await native('verbParameters')(actions); if (reply?.error) throw new Error(reply.error); } }).catch(async e => {
      // One rejected gesture must not poison every later parameter and command.
      for (const id of new Set(actions.map(a=>a.id))) {
        gestures.delete(id);
        try { if(!closed) await native('verbParameters')([{type:'end',id}]); } catch {}
      }
      reportError(e);
    }); }); }
  }
  const bridge = {
    mode: 'juce', validator,
    subscribe(fn) { listeners.add(fn); return () => listeners.delete(fn); },
    parameters: {
      subscribe(fn) { paramListeners.add(fn); return () => paramListeners.delete(fn); },
      get texts() { return texts; },
      isActive(id) { return gestures.has(id); },
      begin(id) { if (gestures.has(id)) throw new Error('Gesture already open'); gestures.add(id); enqueue({ type: 'begin', id }); },
      change(id, value) { if (!gestures.has(id) || !Number.isFinite(value)) throw new Error('Invalid parameter gesture'); const descriptor=descriptors.find(d=>d.id===id); enqueue({ type: 'change', id, value:descriptor?canonical(descriptor,value):value }); },
      end(id) { if (gestures.delete(id)) enqueue({ type: 'end', id }); }
    },
    async send(message) {
      await initialised;
      if (closed) throw new Error('Bridge disposed'); validator.assert(message);
      // Preserve parameter-before-command ordering (e.g. move source then Analyze).
      await Promise.resolve(); await transport;
      let pending, waiter;
      if (message.kind === 'hello') pending = new Promise((resolve,reject) => {
        waiter = {resolve,reject,timer:null}; waiter.timer=setTimeout(()=>{snapshotWaiters.delete(waiter);reject(new Error('Timed out waiting for the initial room snapshot'));},20000);snapshotWaiters.add(waiter);
      });
      let messages;
      try {
        messages = await native('verbMessage')(message);
        if (!Array.isArray(messages)) throw new Error('Invalid native bridge reply');
        for (const reply of messages) receive(reply);
      } catch (error) {
        if (waiter) { clearTimeout(waiter.timer); snapshotWaiters.delete(waiter); waiter.resolve(null); }
        throw error;
      }
      if (pending) {
        const failure=messages.find(m=>m.kind==='error');
        if (failure) { snapshotWaiters.forEach(w=>{clearTimeout(w.timer);w.resolve(null);});snapshotWaiters.clear();throw new Error(failure.payload.message); }
        return pending;
      }
      return messages.find(m => m.kind === 'ack' || m.kind === 'error' || m.kind === 'snapshot') || null;
    },
    async readResource(url) {
      if (!/^\/analysis\/[a-zA-Z0-9-]+\/(full|band\/(?:all|[0-6]))\.bin$/.test(url)) throw new Error('Invalid analysis resource URL');
      const response = await (options.fetch || fetch)(url); if (!response.ok) throw new Error('Analysis resource unavailable'); return response.arrayBuffer();
    },
    readDiagnostics() { return initialised.then(()=>native('verbDiagnostics')()); },
    diagnostic(error) { return initialised.then(()=>native('verbUiError')(String(error?.message||error))).catch(()=>{}); },
    useNativeUi() { return initialised.then(() => native('useClassicUi')()); },
    dispose() { if (closed) return; closed = true; snapshotWaiters.forEach(w=>{clearTimeout(w.timer);w.reject(new Error('Bridge disposed'));});snapshotWaiters.clear(); clearInterval(timer); keys?.dispose(); removers.forEach(fn => fn()); listeners.clear(); paramListeners.clear(); batch = []; }
  };
  return bridge;
}
