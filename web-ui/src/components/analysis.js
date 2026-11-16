import { ENERGY_PALETTES } from '../viewport/energy-layer.js';
import { element, button, select, check, row, note, unavailable } from './dom.js';
import { slider } from './slider.js';
import { BANDS } from '../state/selectors.js';
export function analysisPanel(ctx) {
  const { store } = ctx, state = store.state;
  const o = { source: -1, heightM: 1.2, durationS: 20, resolution: 24, rays: 48000, components: 7, band: -1, range: 60, auto: true, ...store.view.analysis };
  const set = patch => store.setView({ analysis: { ...o, ...patch } });
  const advanced = element('details', { className: 'energy-advanced' }, [element('summary', { text: 'Probe / tracing', title: 'Source, components, height, quality, duration' }), element('div', { className: 'energy-advanced-body panel' }, [
    row('Source', select('Energy source', [[-1, 'All audible'], ...state.document.sources.map((s, i) => [i, s.name])], o.source, source => set({ source: Number(source) }))),
    slider('Probe height', o.heightM, 0.1, 30, 0.1, heightM => set({ heightM }), 'm'),
    row('Quality', select('Energy quality', [[12000, 'Preview · 12,000'], [48000, 'Mix · 48,000'], [192000, 'Render · 192,000']], o.rays, rays => set({ rays: Number(rays) }))),
    slider('Duration', o.durationS, 1, 20, 0.1, durationS => set({ durationS }), 's'),
    note(`${ctx.bridge.mode === 'mock' ? 'SYNTHETIC TEST DATA · not engine acoustics. ' : ''}Hover cells for X/Y and dB relative to peak. Band/components/contrast do not retrace.`), unavailable(state, 'energy.sliceFrontSide', 'Front / Side Energy')])]);
  const root = element('div', { className: 'analysis panel' }, [element('div', { className: 'energy-strip' }, [

    element('div',{className:'energy-components inline',role:'group','aria-label':'Energy components'},['Direct','Early','Tail'].map((label,i)=>button(label,()=>{const components=o.components^(1<<i);if(components)set({components});},{'aria-label':`Energy ${label}`,'aria-pressed':String(!!(o.components&(1<<i))),className:o.components&(1<<i)?'active':''}))),
    row('Band', select('Energy band', [[-1, 'Broadband'], ...BANDS.map((v, i) => [i, `${v} Hz`])], o.band, band => set({ band: Number(band) }))),
    row('Palette', select('Energy colour theme', ENERGY_PALETTES.map(v=>[v,v]), state.preferences?.energyPalette || 'Inferno', energyPalette=>store.command({op:'preferences.set',patch:{energyPalette}}))),
    row('Cells', select('Energy resolution', [16, 24, 32, 48, 64, 96].map(v => [v, v]), o.resolution, resolution => set({ resolution: Number(resolution) }))),
    ]),
    element('div', { className: 'energy-line' }, [slider('Range', o.range, 0.5, 120, 0.5, range => set({ range,auto:false }), 'dB'), check('Auto',o.auto,auto=>set({auto}),{title:'Fit range to 95% of lit cells'}), advanced]),
    element('div',{className:'energy-legend',role:'status'}),
    element('div', { className: 'energy-actions inline' }, [button('Analyze', () => {
      const { source, heightM, durationS, resolution, rays, components } = o;
      store.command({ op: 'analysis.start', source, heightM, durationS, resolution, rays, components:7 });
    }, { 'data-action': 'analyze' }), button('Cancel', () => store.command({ op: 'analysis.cancel' }), { 'data-action': 'cancel-analysis' }),
    element('progress', { max: 1, value: 0, 'aria-label': 'Energy analysis progress' }), element('div', { className: 'analysis-status', role: 'status' })])]);
  const head=element('div',{className:'energy-head'},[element('strong',{text:'Energy · XY'}),button(store.view.energyCollapsed?'+':'−',()=>store.setView({energyCollapsed:!store.view.energyCollapsed}),{'aria-label':'Collapse Energy panel','aria-expanded':String(!store.view.energyCollapsed)})]);
  root.prepend(head);root.classList.toggle('collapsed',!!store.view.energyCollapsed);
  const pos=store.view.energyPosition||{x:8,y:30};
  root.style.left=`${pos.x}px`;root.style.top=`${pos.y}px`;
  // The remembered position was made in another window size: keep the whole panel inside the viewport whenever it resizes.
  const place=()=>{const parent=root.parentElement;if(!parent||drag)return;const x=Math.max(0,Math.min(parent.clientWidth-root.offsetWidth,pos.x)),y=Math.max(0,Math.min(parent.clientHeight-root.offsetHeight,pos.y));root.style.left=`${x}px`;root.style.top=`${y}px`;};
  const fit=new ResizeObserver(place);let fitFrame=requestAnimationFrame(()=>{fitFrame=0;place();if(root.parentElement)fit.observe(root.parentElement);});
  let drag=null;
  head.addEventListener('pointerdown',e=>{if(e.button!==0||e.target.closest('button'))return;e.preventDefault();drag={x:e.clientX,y:e.clientY,left:root.offsetLeft,top:root.offsetTop};root.setAttribute('data-gesture-active','true');head.setPointerCapture(e.pointerId);});
  head.addEventListener('pointermove',e=>{if(!drag)return;root.style.left=`${Math.max(0,Math.min(root.parentElement.clientWidth-root.offsetWidth,drag.left+e.clientX-drag.x))}px`;root.style.top=`${Math.max(0,Math.min(root.parentElement.clientHeight-root.offsetHeight,drag.top+e.clientY-drag.y))}px`;});
  const finish=()=>{if(!drag)return;drag=null;root.removeAttribute('data-gesture-active');store.setView({energyPosition:{x:root.offsetLeft,y:root.offsetTop}});window.dispatchEvent(new Event('verb-control-end'));};
  head.addEventListener('pointerup',finish);head.addEventListener('pointercancel',finish);head.addEventListener('lostpointercapture',finish);
  const update = () => {
    const progress = store.telemetry['analysis.progress'], result = store.telemetry['analysis.result'], stale = store.telemetry['analysis.stale'];
    const running = progress && (!result || result.jobId !== progress.jobId);
    root.querySelector('progress').value = running ? progress.progress : result?.outcome === 'done' ? 1 : 0;
    root.querySelector('[data-action=analyze]').disabled = !!running || !state.capabilities.features['energy.map'].available;
    root.querySelector('[data-action=cancel-analysis]').disabled = !running;
    const changed = result?.result && ['source', 'heightM', 'durationS', 'resolution', 'rays'].some(k => (k === 'heightM' ? result.result.heightM : result.result.options[k]) !== o[k]);
    const missing=result?.result && (o.components & ~(result.result.options.components??7));
    const shownSource=result?.result?.options.source??o.source;
    root.querySelector('.energy-legend').textContent=`${['Direct','Early','Tail'].filter((_,i)=>o.components&(1<<i)).join(' + ')} · ${o.band<0?'Broadband':`${BANDS[o.band]} Hz`} · ${shownSource<0?'All audible':state.document.sources[shownSource]?.name}${missing?' · STALE: component was not analyzed; Analyze to include it':changed?' · STALE: probe settings changed; Analyze to update':''}`;
    root.querySelector('.analysis-status').textContent = running ? `Analyzing ${Math.round(progress.progress * 100)}%` : result?.outcome === 'done' ? `${stale?.stale || changed ? 'STALE · ' : ''}${result.result.nx} × ${result.result.ny} · ${result.result.raysUsed.toLocaleString()} rays${ctx.bridge.mode === 'mock' ? ' · synthetic' : ''}` : result?.message || result?.outcome || 'No result · Analyze to start';
  };
  const off = store.subscribe(({reason}) => { if (reason.startsWith('analysis.')) update(); }); root.dispose = () => { off(); fit.disconnect(); if (fitFrame) cancelAnimationFrame(fitFrame); }; update(); return root;
}
