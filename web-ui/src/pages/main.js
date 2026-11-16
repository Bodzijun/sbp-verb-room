import { autoNumberSources } from '../viewport/t13.js';
import { element, button, select, check, row, note, unavailable, scrollArea } from '../components/dom.js';
import { tabs } from '../components/tabs.js';
import { parameterToggle, outputParts, makeupControls, sourceParts, listenerParts, sceneFacts, parameterSwitch, heading } from '../components/inspector.js';
import { decayPanel } from '../components/decay.js';
import { outputMeters, sourceMeters } from '../components/meters.js';
import { slider, numericField } from '../components/slider.js';
import { FORMATS, LAYOUTS, MICS, channels, budget } from '../state/selectors.js';
import { targetKnob } from '../components/command-knob.js';
import { viewportState } from '../viewport/effective-scene.js';
import { computeRoomMetrics } from '../viewport/room-metrics.js';
// The main page only edits sources and the listener; surfaces belong to Edit room.
export const mainSelectable = ref => ref && ['source', 'listener'].includes(ref.kind);
export function macroRow(ctx) {
  const { store, param } = ctx;
  const target = targetKnob(store);
  const ghost = (key, label) => element('div', { className: 'ghost-macro' }, [element('span', { className: 'rotary unavailable-dial', text: '' }), element('b', { text: 'unavailable' }), unavailable(store.state, key, label)]);
  return element('section', { className: 'macro-row panel' }, [param('size', {knob:true,label:'Size'}), param('absorption', { knob: true, label: 'Absorption' }), target, param('predelay', {knob:true,label:'Predelay'}), param('stereoWidth', {knob:true,label:'Stereo width'}), param('gain', { knob: true, label: 'Output' })]);
}
// ── Context rack: [status | selected object or scene facts] ---------------------------------
const rtText = v => Number.isFinite(v) && v > 0 ? `${v.toFixed(2)} s` : '—';
function statusCard(ctx) {
  const { store } = ctx, state = store.state, diag = store.telemetry['diagnostics.tail'];
  const card = element('div', { className: 'card status-card' }, [element('div', { className: 'card-title' }, 'Reverb time')]);
  const body = element('div', { className: 'card-body' }, [element('div', { className: 'rt-large', text: diag ? rtText(diag.rt60[3]) : '—' }), element('p', { className: 'note rt-caption', text: ctx.bridge.mode === 'mock' ? 'SYNTHETIC · mock RT60' : 'Engine model · RT60', title: ctx.bridge.mode === 'mock' ? 'Test telemetry, not an engine estimate' : 'Modelled decay estimate' }),
    ...['Low', 'Mid', 'High'].map((n, i) => element('div', { className: 'rt-band', style: `--band-colour:${['#d49b75', '#bdb585', '#a99bc8'][i]}` }, [element('span', { text: n }), element('i', {}, element('b', { style: `width:${Math.min(100, (diag?.rt60[[0, 3, 6][i]] || 0) / 8 * 100)}%` })), element('span', { text: diag ? rtText(diag.rt60[[0, 3, 6][i]]) : '—' })])),
    element('div', { className: 'listen' }, [heading('Listen · solo'), element('div', { className: 'inline' }, [parameterToggle(ctx, 'soloDirect', 'Direct'), parameterToggle(ctx, 'soloEarly', 'Early'), parameterToggle(ctx, 'soloTail', 'Tail')]),
      element('div', { className: 'inline' }, [creativeButton(ctx, 'freeze', 'Freeze')])])]);
  card.append(body);
  card.dispose = store.subscribe(({ reason }) => {
    if (reason !== 'diagnostics.tail') return;
    const d = store.telemetry['diagnostics.tail'], large = body.querySelector('.rt-large'); if (large) large.textContent = rtText(d.rt60[3]);
    const rows = body.querySelectorAll('.rt-band'); [0, 3, 6].forEach((b, i) => { rows[i].lastChild.textContent = rtText(d.rt60[b]); rows[i].querySelector('b').style.width = `${Math.min(100, d.rt60[b] / 8 * 100)}%`; });
  });
  return card;
}
function selectedCard(ctx) {
  const { store } = ctx, state = store.state, ref = store.view.selection.find(mainSelectable);
  const title = !ref ? 'Scene' : ref.kind === 'source' ? `Source ${String(ref.index + 1).padStart(2, '0')} · ${state.document.sources[ref.index]?.name || ''}` : 'Listener';
  const head = element('div', { className: 'card-title' }, [element('span', { text: title })]);
  if (ref) head.append(button('×', () => store.setView({ selection: [] }), { className: 'card-close', 'aria-label': 'Clear selection', title: 'Clear selection' }));
  const body = element('div', { className: 'card-grid' });
  let disposeScene = null;
  if (ref?.kind === 'source') {
    const p = sourceParts(ctx, ref.index);
    if (p) body.append(element('div', { className: 'stack pose-column' }, [heading('Pose'), ...p.position]), element('div', { className: 'stack settings-column' }, [...p.identity, ...p.routing, ...p.level, ...p.points]));
  } else if (ref?.kind === 'listener') {
    const p = listenerParts(ctx);
    body.append(element('div', { className: 'stack pose-column' }, [heading('Pose'), ...p.position]), element('div', { className: 'stack settings-column' }, [heading('Output / microphones'), ...p.output.main]));
  } else {
    const usage = budget(state.document);
    const effScene = viewportState(state, false).document.scene;
    const metrics = computeRoomMetrics(effScene);
    const dimNode = element('span', { className: 'scene-dimensions', text: metrics.dimensionsText });
    const areaNode = element('span', { className: 'scene-area', text: metrics.areaText });
    const volNode = element('span', { className: 'scene-volume', text: metrics.volumeText });
    const modeRow = row('Mode', state.parameters.effective.mode);
    body.append(element('div', { className: 'stack wide' }, [element('div', { className: 'facts-line' }, [row('Sources', `${state.document.sources.length} / ${state.limits.maxSources}`), row('Points', `${usage.points} / ${state.limits.maxPoints}`), row('Walls', `${state.document.scene.walls.length} / ${state.limits.maxWalls}`)]),
      element('div', { className: 'facts-grid' }, [
        modeRow,
        Object.assign(row('L × W × H', dimNode), { title: 'Bounding dimensions (length × width × height)' }),
        row('Output', LAYOUTS[state.document.output.layout + 1]),
        row('Floor area', areaNode),
        row('Microphones', MICS[state.document.output.micPattern]),
        row('Volume', volNode)
      ]),
      element('p', { className: `note ${usage.overlap ? 'warning' : ''}`, text: usage.overlap ? 'Source routes overlap.' : 'Select a source or the Listener here or in the viewport. Walls and materials are edited in Edit room.' }),
      element('div', { className: 'pick-chips' }, [button('Listener', () => store.setView({ selection: [{ kind: 'listener', index: 0 }] })), ...state.document.sources.map((src, index) => button(`${String(index + 1).padStart(2, '0')} ${src.name}`, () => store.setView({ selection: [{ kind: 'source', index }] }), { title: src.name }))])]));
    disposeScene = store.subscribe(({ reason }) => {
      if (!['parameters', 'snapshot', 'patch'].includes(reason) || !store.state) return;
      const m = computeRoomMetrics(viewportState(store.state, false).document.scene);
      dimNode.textContent = m.dimensionsText;
      areaNode.textContent = m.areaText;
      volNode.textContent = m.volumeText;
      if (modeRow.lastChild) modeRow.lastChild.textContent = store.state.parameters.effective.mode;
    });
  }
  const card = element('div', { className: `card selected-card${ref ? ' has-selection' : ''}` }, [head, scrollArea(body, { className: 'card-body' })]);
  if (disposeScene) card.dispose = disposeScene;
  return card;
}
export function mainContext(ctx) {
  const root = element('section', { className: 'context panel main-context' }, [statusCard(ctx), selectedCard(ctx)]);
  return root;
}
// ── Sources: DAW-style mixer strips -------------------------------------------------------
export function sourcesPanel(ctx) {
  const { store, param } = ctx, state = store.state, flags = state.document.editorFlags, usage = budget(state.document);
  const root = element('div', { className: 'sources-panel' });
  root.append(element('div', { className: 'source-actions' }, [button('+ Source', () => store.command({ op: 'source.add' }), { disabled: state.document.sources.length >= state.limits.maxSources }),
    button('− Source', () => {
      const index = store.view.selection.find(r => r.kind === 'source')?.index ?? 0;
      if (window.confirm('Remove this source? Later source slots and index-based host automation shift down.')) store.command({ op: 'source.remove', index });
    }, { disabled: state.document.sources.length <= 1 }),
    button('Auto-number channels', async () => {
      const plan=autoNumberSources(store.state.document.sources,store.state.host.inputChannels),tx=`auto-inputs-${Date.now()}`;
      const begin=await store.command({op:'transaction.begin',label:'Auto-number channels'},tx);
      if(begin?.kind!=='ack')return;
      for(const assignment of plan.assignments){const reply=await store.command({op:'source.setConfig',...assignment},tx);if(reply?.kind!=='ack')return;}
      await store.command({op:'transaction.commit'},tx);
      if(plan.skippedSources){const warning=document.querySelector('.source-budget');if(warning){warning.classList.add('warning');warning.textContent+=` · ${plan.missingChannels} inputs / ${plan.skippedSources} sources did not fit (unchanged)`;}}
    }),
    check('Keep I/O', flags.keepRouting, keepRouting => store.flags({ keepRouting }), { title: 'Full presets preserve local Sources / Output; room, poses and mix import by index.' }),
    element('span', { className: `${usage.overlap || usage.inputs > state.host.inputChannels ? 'warning' : 'note'} source-budget`, text: `${state.document.sources.length}/${state.limits.maxSources} src · ${usage.inputs}/${state.host.inputChannels} inputs · ${usage.points}/${state.limits.maxPoints} pts${usage.overlap ? ' · overlapping routes' : ''}` })]));
  const head = element('div', { className: 'mix-row mix-head', role: 'row' }, ['', 'Source', 'Format', 'In', 'Ch', 'Gain dB', 'Level', 'M', 'S'].map((t, i) => element('span', { text: t, className: `c${i}` })));
  const list = element('div', { className: 'mix-list', role: 'rowgroup' });
  const meterSet = sourceMeters(store, state.document.sources.map(s => [s.firstIn, channels(s)]));
  state.document.sources.forEach((source, index) => {
    const config = patch => store.command({ op: 'source.setConfig', index, ...patch });
    const selected = store.view.selection.some(r => r.kind === 'source' && r.index === index);
    const colourIndex = index % 6;
    list.append(element('div', { className: `mix-row${selected ? ' selected' : ''}`, role: 'row', style: `--strip:var(--strip-${colourIndex})` }, [
      button(String(index + 1).padStart(2, '0'), () => store.setView({ selection: [{ kind: 'source', index }] }), { className: 'strip-id c0', 'aria-label': `Select source ${index + 1}`, 'aria-pressed': String(selected) }),
      element('input', { type: 'text', className: 'c1 strip-name', value: source.name, 'aria-label': `Source ${index + 1} name`, maxLength: 128, 'data-focus-key': `source-table-${index}`, onchange: e => config({ name: e.target.value }) }),
      select(`Source ${index + 1} format`, FORMATS, source.format, n => config({ format: Number(n) }), { className: 'dd c2' }),
      numericField(`Source ${index + 1} first input`, source.firstIn + 1, 1, 64, 1, n => config({ firstIn: n - 1 })),
      element('span', { className: 'c4 strip-ch', text: `${channels(source)}`, title: `In ${source.firstIn + 1}–${source.firstIn + channels(source)}` }),
      element('div', { className: 'c5 strip-gain' }, param(`s${index + 1}g`, { label: 'Gain' })),
      meterSet ? Object.assign(meterSet.nodes[index], { className: 'hmeter c6' }) : element('span', { className: 'c6' }),
      parameterSwitch(ctx, `s${index + 1}mute`, 'M', 'mute', `Source ${index + 1} mute`), parameterSwitch(ctx, `s${index + 1}solo`, 'S', 'solo', `Source ${index + 1} solo`)]));
  });
  if (meterSet) root.dispose = meterSet.dispose;
  const area = scrollArea(list, { className: 'mix-scroll' });
  root.append(element('div', { className: 'mixer' }, [head, area]));
  return root;
}
export function mixPanel(ctx) {
  const { param } = ctx;
  const block = (title, cls, children) => element('div', { className: `mix-block ${cls}` }, [heading(title), element('div', { className: 'mix-block-body' }, children)]);
  return element('div', { className: 'mix-panel' }, [
    element('div', { className: 'mix-columns' }, [
      block('Early', 'mix-early', [param('early', { knob: true, label: 'Early level' }), param('diffusion', { knob: true, label: 'Early diffusion' })]),
      block('Tail & motion', 'mix-tail', [param('tail', { knob: true, label: 'Tail level' }), param('doppler', { knob: true, label: 'Doppler' })]),
      block('Output', 'mix-output', [
        element('div', { className: 'mix-output-knobs' }, [param('mix', { knob: true, label: 'Mix' }), makeupControls(ctx)]),
        parameterToggle(ctx, 'spatialDry', 'Positioned dry'),
        note('Direct sound follows source and listener positions.')
      ]),
      block('Duck', 'mix-duck', [
        element('div', { className: 'duck-head' }, [creativeButton(ctx, 'duck', 'Duck'), duckMeter(ctx)]),
        param('duck', { knob: true, label: 'Amount' }),
        param('duckRelease', { knob: true, label: 'Release' })
      ])
    ])
  ]);
}
export function outputPanel(ctx) {
  const { store } = ctx, state = store.state, tail = state.document.tail, o = outputParts(ctx);
  const colOutput = element('div', { className: 'stack' }, [heading('Output / microphones'), ...o.main]);
  const colTail = element('div', { className: 'stack' }, [heading('Tail · convolution'),
    row('Quality', select('Tail quality', [[12000, 'Preview · 12,000'], [48000, 'Mix · 48,000'], [192000, 'Render · 192,000'], ...([12000,48000,192000].includes(tail.baseRays)?[]:[[tail.baseRays,'Custom']])], tail.baseRays, n => store.command({ op: 'tail.setQuality', baseRays: Number(n) }))),
    slider('Tail rays', tail.baseRays, 8000, 384000, 1, baseRays => store.command({ op: 'tail.setQuality', baseRays })),
    check('Adaptive', tail.adaptive, adaptive => store.command({ op: 'tail.setQuality', adaptive })),
    heading('Physics'), check('Edge diffraction', state.document.scene.diffraction, enabled => store.command({ op: 'room.setDiffraction', enabled })),
    ...[['lfRoomModes', 'LF room modes'], ['velvetTransition', 'Velvet transition']].map(([key, label]) => unavailable(state, key, label))]);
  const colArray = element('div', { className: 'stack' }, [heading('Array / host'), ...o.extra, state.capabilities.features['output.meters']?.available ? outputMeters(store) : unavailable(state, 'output.meters', 'Output meters')]);
  return scrollArea([colOutput, colTail, colArray], { className: 'output-panel' });
}
export function mainBottom(ctx) {
  const { store } = ctx, root = element('section', { className: `bottom panel bottom-${store.view.tab}` });
  root.append(tabs([['decay', 'Decay'], ['sources', 'Sources'], ['mix', 'Mix'], ['output', 'Settings']], store.view.tab, tab => store.setView({ tab })));
  const content = element('div', { className: 'bottom-content', role: 'tabpanel', 'aria-labelledby': `tab-${store.view.tab}` });
  content.append(({ decay: decayPanel, sources: sourcesPanel, mix: mixPanel, output: outputPanel })[store.view.tab](ctx)); root.append(content); return root;
}

// Both actions use the same begin/change/end protocol as every host-bound control.
const rememberedDucks=new WeakMap();
function creativeButton({store,bridge},id,label){
  let remembered=rememberedDucks.get(store)??6;
  const root=button(label,()=>{
    const value=store.state.parameters.values[id];if(id==='duck'&&value>0){remembered=value;rememberedDucks.set(store,value);}
    bridge.parameters.begin(id);bridge.parameters.change(id,value>0?0:id==='duck'?remembered:1);bridge.parameters.end(id);
  },{'data-parameter-button':id,'aria-label':label});
  function update(){const v=store.state.parameters.values[id],locked=store.state.parameters.values.mode===0;
    root.disabled=locked;root.classList.toggle('locked',locked);root.classList.toggle('on',v>0&&!locked);root.setAttribute('aria-pressed',String(v>0&&!locked));
    root.title=locked?'Locked in Physical: switch to Creative; stored value is preserved':id==='duck'?'Attenuate wet while dry input is present. Amount / release in Mix.':'Hold the current tail; new input does not enter the held layer';
  }
  root.dispose=store.subscribe(update);update();return root;
}
function duckMeter({store}){
  const root=element('small',{className:'duck-meter','aria-label':'Duck gain reduction'});
  const update=()=>{root.textContent=`−${(store.state.parameters.effective.duckReductionDb??0).toFixed(1)} dB`;};
  root.dispose=store.subscribe(update);update();return root;
}
