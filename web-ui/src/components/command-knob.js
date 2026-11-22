import { element, button } from './dom.js';
import { valueField } from './value-field.js';
// Non-APVTS macro: the command runs on commit, never masquerades as a parameter relay.
export function targetKnob(store) {
  const current = store.view.rt60TargetS || store.telemetry['status.rt60']?.requestedS || 2;
  const root = element('div', { className: 'macro-target' });
  const knob = element('div', { className: 'rotary', role: 'slider', tabIndex: 0, 'aria-label': 'RT60 target', 'aria-valuemin': 0.1, 'aria-valuemax': 20, 'aria-valuenow': current }, element('i'));
  const input = element('input', { type: 'text', value: String(current), className: 'readout', 'aria-label': 'RT60 target seconds' });
  const status = element('small', { className: 'target-status', text: 'target · mid 500–2k Hz' });
  let start = null, draft = current;
  const update = value => { draft = Math.max(0.1, value); input.value = `${draft.toFixed(2)} s`; knob.style.setProperty('--normal', Math.max(0, Math.min(1, Math.log(draft / 0.1) / Math.log(200)))); knob.style.setProperty('--angle', `${-135 + 270 * Math.max(0, Math.min(1, Math.log(draft / 0.1) / Math.log(200)))}deg`); knob.setAttribute('aria-valuenow', draft); };
  const commit = () => { const targetS = Number.parseFloat(input.value); if (Number.isFinite(targetS) && targetS > 0) { store.view.rt60TargetS=targetS; store.command({ op: 'rt60.start', targetS }); } };
  const release = () => { root.removeAttribute('data-gesture-active'); window.dispatchEvent(new Event('verb-control-end')); };
  knob.addEventListener('pointerdown', e => { if (e.button !== 0) return; root.setAttribute('data-gesture-active','true'); start = { y: e.clientY, value: draft }; knob.setPointerCapture(e.pointerId); });
  knob.addEventListener('pointermove', e => { if (start) update(Math.min(20, start.value * Math.exp((start.y - e.clientY) / (e.shiftKey ? 500 : 80)))); });
  knob.addEventListener('pointerup', () => { if (start) { start = null; commit(); release(); } });
  const cancel = () => { const active=!!start; start = null; update(store.view.rt60TargetS || current); if(active)release(); };
  knob.addEventListener('pointercancel', cancel); knob.addEventListener('lostpointercapture', cancel);
  knob.addEventListener('dblclick', () => { update(2); commit(); });
  knob.addEventListener('keydown', e => { if (e.key === 'ArrowUp' || e.key === 'ArrowDown') { e.preventDefault(); update(draft + (e.key === 'ArrowUp' ? 1 : -1) * (e.shiftKey ? 0.01 : 0.1)); commit(); } });
  input.addEventListener('change', commit); input.addEventListener('keydown', e => { if (e.key === 'Enter') { commit(); input.blur(); } if (e.key === 'Escape') { cancel(); input.blur(); } });
  const statusUpdate = () => { const e=store.telemetry['status.rt60']; if(e) status.textContent=`${['idle', 'working', 'reached', 'unreachable', 'cancelled/error', 'invalid'][e.state]} · achieved ${e.achievedS ?? 'unknown'} s`; };
  const off = store.subscribe(({ reason }) => { if(reason==='status.rt60')statusUpdate(); }); statusUpdate();
  const offField=valueField(input,{get:()=>draft,min:.1,max:20,step:.01,display:n=>`${n.toFixed(2)} s`,commit:n=>{update(n);commit();}});
  window.addEventListener('blur', cancel); root.dispose = () => { offField(); off(); window.removeEventListener('blur', cancel); start = null; };
  update(current); root.append(knob, input, button('RT60 target', commit), button('×', () => store.command({ op: 'rt60.cancel' }), { className: 'cancel-target', title: 'Cancel target', 'aria-label': 'Cancel target' }), status); return root;
}
