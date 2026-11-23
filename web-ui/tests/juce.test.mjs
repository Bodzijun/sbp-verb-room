import test from 'node:test';
import assert from 'node:assert/strict';
import { createJuceBridge } from '../src/bridge/juce.js';
import { createStore } from '../src/state/store.js';
import { createMockBridge } from '../src/bridge/mock.js';
import { validator, readFixture } from './helpers.mjs';
const fixture = await readFixture('snapshot.default-physical');
const snapshot = () => structuredClone(fixture.messages.find(m => m.kind === 'snapshot'));
function fake() {
  const calls=[], listeners=new Map(); let token=0, state=snapshot();
  state.payload.parameters.descriptors = null;
  const backend={ addEventListener(name,fn){const id=++token;listeners.set(id,{name,fn});return id;}, removeEventListener(id){listeners.delete(id);}, emitEvent(){} };
  const emit=(name,value)=>{for(const l of listeners.values())if(l.name===name)l.fn(structuredClone(value));};
  const native=name=>async value=>{
    calls.push({name,value:structuredClone(value)});
    if(name==='ready')return {ok:true};
    if(name==='verbParameters')return {ok:true};
    if(name==='ping')return value;
    if(name==='verbMessage') {
      if(value.kind==='hello'){state.sessionId=value.sessionId;return [structuredClone(state)];}
      return [{schemaVersion:1,sessionId:value.sessionId,kind:'ack',requestId:value.requestId,payload:{requestId:value.requestId,revision:state.payload.revision,status:'accepted'}}];
    }
    return true;
  };
  return {native,backend,calls,listeners,emit,state};
}
test('native handshake precedes hello; synchronous interface loads validator before delivery',async t=>{
 const f=fake(), b=createJuceBridge({native:f.native,backend:f.backend,validator});t.after(()=>b.dispose());
 const received=[];b.subscribe(m=>received.push(m));await b.send(fixture.messages.find(m=>m.kind==='hello'));
 assert.deepEqual(f.calls.map(c=>c.name),['ready','verbMessage']);assert.equal(received[0].kind,'snapshot');
 received.forEach(m=>validator.assert(m));assert.equal(b.mode,'juce');
});
test('F7 native snapshots preserve exact wall and zone coordinates in replies and events', async t => {
  const f = fake();
  const exactX = 0.000012345678901234567, exactY = 53.123456789012345;
  const withExactCorners = reply => {
    const { walls, floorZones, ceilingZones } = reply.payload.document.scene;
    const points = [...walls.flatMap(w => [w.a, w.b]), ...[...floorZones, ...ceilingZones].flatMap(z => z.vertices)];
    points[0] = [exactX, exactY];
    return { ...reply, exactSceneCorners: JSON.stringify(points) };
  };
  const native = name => name === 'verbMessage' ? async message => (await f.native(name)(message))
    .map(reply => reply.kind === 'snapshot' ? withExactCorners(reply) : reply) : f.native(name);
  const b = createJuceBridge({ native, backend: f.backend, validator }), s = createStore(b);
  t.after(() => { s.dispose(); b.dispose(); });
  await s.start();
  assert.deepEqual(s.state.document.scene.walls[0].a, [exactX, exactY]);
  const message = withExactCorners(f.state), points = JSON.parse(message.exactSceneCorners);
  points[1] = [exactY, exactX];
  f.emit('verb.message', { ...message, exactSceneCorners: JSON.stringify(points) });
  assert.deepEqual(s.state.document.scene.walls[0].b, [exactY, exactX]);
  assert.ok(s.ready);
});
test('ordered parameter gestures batch once; automation has no outgoing echo and carries plugin texts',async t=>{
 const f=fake(),b=createJuceBridge({native:f.native,backend:f.backend,validator});t.after(()=>b.dispose());await b.send(fixture.messages[0]);
 b.parameters.begin('mix');b.parameters.change('mix',37.5);b.parameters.end('mix');
 await b.send({...fixture.messages[0],kind:'command',requestId:'after-gesture',baseRevision:'1',payload:{op:'preset.list'}});
 const batches=f.calls.filter(c=>c.name==='verbParameters');assert.equal(batches.length,1);assert.deepEqual(batches[0].value.map(a=>a.type),['begin','change','end']);
 let update;b.parameters.subscribe(v=>update=v);const before=f.calls.length;
 f.emit('verb.parameters',{values:{...f.state.payload.parameters.values,mix:20},effective:f.state.payload.parameters.effective,texts:{mix:'20.000'}});
 assert.equal(update.values.mix,20);assert.equal(update.texts.mix,'20.000');assert.equal(f.calls.length,before);
 assert.throws(()=>b.parameters.change('mix',40));b.parameters.begin('mix');assert.throws(()=>b.parameters.begin('mix'));b.parameters.end('mix');
});
test('store resynchronises a forced patch gap and resync.required without waiting for another command',async t=>{
 const f=fake(),b=createJuceBridge({native:f.native,backend:f.backend,validator}),s=createStore(b);t.after(()=>{s.dispose();b.dispose();});await s.start();
 f.emit('verb.message',{schemaVersion:1,sessionId:'fake',kind:'patch',payload:{fromRevision:'999',toRevision:'1000',ops:[{op:'replace',path:'/document/scene/heightM',value:4}]}});
 await new Promise(r=>setTimeout(r,10));assert.equal(s.ready,true);assert.equal(f.calls.filter(c=>c.name==='verbMessage'&&c.value.kind==='hello').length,2);
 f.emit('verb.message',{schemaVersion:1,sessionId:'fake',kind:'event',payload:{event:'resync.required',reason:'redo',currentRevision:'2'}});
 await new Promise(r=>setTimeout(r,10));assert.equal(s.ready,true);assert.equal(f.calls.filter(c=>c.name==='verbMessage'&&c.value.kind==='hello').length,3);
});
test('native document flags use typed commands; layer mask adapter preserves frontend interface',async t=>{
 const f=fake(),b=createJuceBridge({native:f.native,backend:f.backend,validator}),s=createStore(b);t.after(()=>{s.dispose();b.dispose();});await s.start();
 await s.flags({navLocked:true,layerModes:{walls:1,plates:2}});
 assert.deepEqual(f.calls.filter(c=>c.name==='verbMessage'&&!['telemetry.subscribe','catalogs.list'].includes(c.value.payload.op)&&c.value.kind==='command').map(c=>c.value.payload.op),['editor.setFlags','layers.set','layers.set']);
});
test('native resource access permits only local typed result URLs and disposal releases listeners',async()=>{
 const f=fake(),b=createJuceBridge({native:f.native,backend:f.backend,validator,fetch:async()=>({ok:true,arrayBuffer:async()=>new ArrayBuffer(8)})});await b.send(fixture.messages[0]);
 assert.equal((await b.readResource('/analysis/energy-1/full.bin')).byteLength,8);await assert.rejects(b.readResource('https://evil.example/full.bin'));
 b.dispose();assert.equal(f.listeners.size,0);await assert.rejects(b.send(fixture.messages[0]),/disposed/);
});
test('v1.1 additions validate and the mock accepts typed state/catalog commands',async t=>{
 const b=await createMockBridge({validator,read:readFixture,delayMs:0});const s=createStore(b);t.after(()=>{s.dispose();b.dispose();});await s.start();
 await s.command({op:'listener.set',xM:2,yM:3});assert.equal(s.state.parameters.values.lx,2);
 await s.command({op:'editor.setFlags',flags:{navLocked:true}});assert.equal(s.state.document.editorFlags.navLocked,true);
 await s.command({op:'layers.set',layer:2,mode:1});assert.equal(s.state.document.editorFlags.layerModes.ceilings,1);
 await s.command({op:'room.setClutter',minDistM:0.8});assert.equal(s.state.document.scene.clutter.minDistM,0.8);
 await s.command({op:'catalogs.list'});assert.ok(s.telemetry['catalogs.list'].materials.length);
 await s.command({op:'reflections.query',sourceIndex:0,maxOrder:1,maxPaths:20});assert.ok(s.telemetry['diagnostics.paths']);
 b.audit.filter(x=>x.message?.schemaVersion).forEach(x=>validator.assert(x.message));
});

test('initial asynchronous room load waits for a real snapshot event',async t=>{
 const f=fake(),native=name=>name==='verbMessage'?async message=>{if(message.kind==='hello'){setTimeout(()=>f.emit('verb.message',{...f.state,sessionId:message.sessionId}),10);return [];}return f.native(name)(message);}:f.native(name);
 const b=createJuceBridge({native,backend:f.backend,validator});t.after(()=>b.dispose());const result=await b.send(fixture.messages[0]);assert.equal(result.kind,'snapshot');
});

test('parameter telemetry retains document identity and authoritative texts for selective rendering',async t=>{
 const f=fake(),b=createJuceBridge({native:f.native,backend:f.backend,validator}),s=createStore(b);t.after(()=>{s.dispose();b.dispose();});await s.start();
 const document=s.state.document,preferences=s.state.preferences;
 f.emit('verb.parameters',{values:{...f.state.payload.parameters.values,mix:26},effective:f.state.payload.parameters.effective,texts:{mix:'Plugin 26'}});
 assert.equal(s.state.document,document);assert.equal(s.state.preferences,preferences);assert.equal(s.state.parameters.texts.mix,'Plugin 26');assert.ok(Object.isFrozen(s.state.parameters));
});
test('stale view persistence retries once using fresh revision and request id; geometry is never replayed',async t=>{
 const f=fake();let rejectOp='session.setViewState';
 const native=name=>name!=='verbMessage'?f.native(name):async message=>{
   if(message.kind==='command'&&message.payload.op===rejectOp){rejectOp=null;f.calls.push({name,value:structuredClone(message)});f.state.payload.revision=String(Number(f.state.payload.revision)+1);
     return [{schemaVersion:1,sessionId:message.sessionId,kind:'error',requestId:message.requestId,payload:{code:'staleRevision',message:'Changed',requestId:message.requestId,currentRevision:f.state.payload.revision,resync:true}},structuredClone(f.state)];}
   return f.native(name)(message);
 };
 const b=createJuceBridge({native,backend:f.backend,validator}),s=createStore(b);t.after(()=>{s.dispose();b.dispose();});await s.start();
 const result=await s.setView({tab:'sources'});assert.equal(result.kind,'ack');assert.equal(s.error,null);
 const views=f.calls.filter(c=>c.value?.payload?.op==='session.setViewState');assert.equal(views.length,2);assert.notEqual(views[0].value.requestId,views[1].value.requestId);assert.deepEqual(views.map(c=>c.value.baseRevision),['1','2']);
 rejectOp='preset.load';assert.equal((await s.command({op:'preset.load',source:'builtin',id:0})).kind,'ack');assert.equal(f.calls.filter(c=>c.value?.payload?.op==='preset.load').length,2);assert.equal(s.error,null);
 rejectOp='room.setHeight';await s.command({op:rejectOp,heightM:4});assert.equal(f.calls.filter(c=>c.value?.payload?.op==='room.setHeight').length,1);
});

test('session view restores on first snapshot; subsequent resync cannot overwrite a newer local page',async t=>{
 const f=fake();f.state.payload.preferences={sessionViewState:{page:'edit',projection:'front'}};
 const b=createJuceBridge({native:f.native,backend:f.backend,validator}),s=createStore(b);t.after(()=>{s.dispose();b.dispose();});await s.start();assert.equal(s.view.page,'edit');assert.equal(s.view.projection,'front');
 s.setView({page:'main',projection:'plan'},false);f.emit('verb.message',f.state);assert.equal(s.view.page,'main');assert.equal(s.view.projection,'plan');
});

test('T6 a rejected native parameter batch does not poison subsequent gestures and commands',async t=>{
 const f=fake();let reject=true;const errors=[];
 const native=name=>name!=='verbParameters'?f.native(name):async actions=>{f.calls.push({name,value:actions});if(reject){reject=false;return{error:'mode changed during gesture'};}return{ok:true};};
 const b=createJuceBridge({native,backend:f.backend,validator,onError:e=>errors.push(e.message)});t.after(()=>b.dispose());await b.send(fixture.messages[0]);
 b.parameters.begin('early');b.parameters.change('early',-5);b.parameters.end('early');
 await b.send({...fixture.messages[0],kind:'command',requestId:'recovered-1',baseRevision:'1',payload:{op:'preset.list'}});
 b.parameters.begin('mix');b.parameters.change('mix',30);b.parameters.end('mix');
 const result=await b.send({...fixture.messages[0],kind:'command',requestId:'recovered-2',baseRevision:'1',payload:{op:'preset.list'}});
 assert.equal(result.kind,'ack');assert.deepEqual(errors,['mode changed during gesture']);assert.ok(f.calls.some(c=>c.name==='verbParameters'&&c.value.some(a=>a.id==='mix'&&a.type==='change')));
});

test('E11 rejected batch ends gestures and transport accepts next mode gesture',async t=>{
 const f=fake();let reject=true;
 const native=name=>async value=>{if(name==='verbParameters'&&reject&&value.some(a=>a.type==='change')){reject=false;f.calls.push({name,value});return {error:'Parameter out of range'};}return f.native(name)(value);};
 let errors=0;const b=createJuceBridge({native,backend:f.backend,validator,onError:()=>errors++});t.after(()=>b.dispose());
 await b.send(fixture.messages[0]);b.parameters.begin('mix');b.parameters.change('mix',20);
 await new Promise(r=>setTimeout(r,20));assert.equal(b.parameters.isActive('mix'),false);assert.equal(errors,1);
 assert.ok(f.calls.some(c=>c.name==='verbParameters'&&c.value.some(a=>a.id==='mix'&&a.type==='end')));
 b.parameters.begin('mode');b.parameters.change('mode',1);b.parameters.end('mode');await new Promise(r=>setTimeout(r,20));assert.equal(errors,1);
});
