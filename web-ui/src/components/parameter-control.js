import { pathPositionLocked, hasActiveAzimuthKeys } from '../viewport/movement-path.js';
import { element } from './dom.js';
import { range, canonical, toNormalised, fromNormalised, toText, fromText, displayText } from '../bridge/parameters.js';
import { effectiveValue } from '../state/selectors.js';
import { valueField } from './value-field.js';
// One gesture implementation shared by rotary and slider controls.
export function parameterControl(store, bridge, id, { knob = false, label } = {}) {
  const fallbackD = store.state.parameters.descriptors.parameters.find(p => p.id === id);
  if (!fallbackD) throw new Error(`Descriptor missing: ${id}`);
  const desc = () => store.state.parameters.descriptors.parameters.find(p => p.id === id) || fallbackD;
  const root = element('div', { className: knob ? 'parameter knob' : 'parameter slider-row', 'data-parameter': id, 'data-parameter-index': fallbackD.index });
  const name = element('span', { className: 'parameter-name', text: `${label || fallbackD.name}${!knob && fallbackD.unit ? `, ${fallbackD.unit}` : ''}` });
  const input = element('input', { className: 'readout', type: 'text', 'aria-label': `${label || fallbackD.name} value`, 'data-focus-key': id, inputMode: 'decimal' });
  const control = element('div', { className: knob ? 'rotary' : 'track', role: 'slider', tabIndex: 0, 'aria-label': label || fallbackD.name });
  const indicator = element('i'); control.append(indicator);
  if(knob) control.append(element('i',{className:'effective-pointer','aria-hidden':'true'}));
  const eff = element('small', { className: 'effective' });
  let textCommitted=false;
  let active = false, pointer = null, startY = 0, startX = 0, startNormal = 0, pending = null, frame = null, lastSent = 0;
  const current = () => store.state.parameters.values[id];
  const text = value => displayText(desc(), value);
  function disabled() { return !store.ready || pathPositionLocked(store.state,id) || id==='stereoWidth'&&store.state.parameters.effective.widthAvailable===false || desc().modeClass === 'creativeOnly' && store.state.parameters.values.mode === 0; }
  function begin() { if (!active && !disabled()) { active = true; root.setAttribute('data-gesture-active','true'); bridge.parameters.begin(id); } }
  function flush() { if (pending !== null && active) { bridge.parameters.change(id, pending); pending = null; lastSent = performance.now(); } }
  function send(value, final = false) {
    if (!active) return; pending = canonical(desc(), value);
    if (final || performance.now() - lastSent >= 1000 / 60) flush();
    else if (frame === null) frame = requestAnimationFrame(() => { frame = null; flush(); });
  }
  function end() { const wasActive=active; if (active) { flush(); bridge.parameters.end(id); active = false; } const capture=pointer;pointer = null; if(capture!==null&&control.hasPointerCapture(capture))control.releasePointerCapture(capture); root.removeAttribute('data-gesture-active'); if(wasActive) window.dispatchEvent(new Event('verb-control-end')); }
  function single(value) { begin(); send(value, true); end(); update(); }
  function displayedState() {
    const value = current(), locked = disabled();
    const isAz = /^(s\d+|l)az$/.test(id);
    const azObj = isAz ? (id === 'laz' ? 32 : Number(id.slice(1, -2)) - 1) : -1;
    const lockedByAzKeys = locked && isAz && hasActiveAzimuthKeys(store.state.document.paths?.[azObj]);
    const effective = effectiveValue(store.state, id);
    const displayVal = lockedByAzKeys ? effective : value;
    return { value, locked, isAz, lockedByAzKeys, effective, displayVal };
  }
  function update() {
    const d = desc();
    const { value, locked, isAz, lockedByAzKeys, effective, displayVal } = displayedState();
    const normal = toNormalised(d, displayVal);
    root.classList.toggle('locked', locked); input.disabled = locked; control.setAttribute('aria-disabled', String(locked));
    const [lo, hi] = range(d); control.setAttribute('aria-valuemin', lo); control.setAttribute('aria-valuemax', hi); control.setAttribute('aria-valuenow', displayVal); control.setAttribute('aria-valuetext', text(displayVal));
    root.style.setProperty('--normal', normal); root.style.setProperty('--angle', `${-135 + 270 * normal}deg`);
    if (document.activeElement !== input || input.readOnly && !active) input.value = text(displayVal);
    if (locked && isAz && !lockedByAzKeys && document.activeElement !== input) input.value = text(effective);  // E19: non-manual orientation shows the live computed azimuth
    root.style.setProperty('--effective-angle',`${-135+270*toNormalised(d,effective)}deg`);
    root.classList.toggle('has-effective',Math.abs(effective-value)>0.0005);
    const absorption = store.state.parameters.effective.absorption;
    eff.textContent = id === 'absorption' ? `applied ${Number.isFinite(absorption.applied) ? displayText(d, absorption.applied) : 'pending'} · ${absorption.tailCurrent ? 'tail current' : 'transition'}` : lockedByAzKeys ? `keys ${displayText(d, effective)}` : (locked || Math.abs(effective - value) > 0.0005) ? `effective ${displayText(d, effective)}` : '';
    if(id==='size')eff.textContent=store.state.load.tailCurrent?`Size ${effective} %`:`applied ${effective} % · Rebuilding…`;
    if(id==='stereoWidth'&&store.state.parameters.effective.widthAvailable===false)eff.textContent='unavailable · mono output';
    root.title = lockedByAzKeys
      ? `${label || d.name} · ${text(effective)} · Locked: driven by Azimuth keys in Path timeline (clear keys or switch Orientation to unlock)`
      : `${label || d.name} · ${text(value)}${locked && d.modeClass === 'creativeOnly' && store.state.parameters.values.mode===0 ? ' · Locked in Physical: switch to Creative; stored value is preserved' : ''}${eff.textContent ? ` · ${eff.textContent}` : ''}`;
    if(id==='size')root.title+=' · Debounced room rebuild; Edit room uses unscaled geometry';
    if(id==='stereoWidth'&&store.state.parameters.effective.widthAvailable===false)root.title+=' · Unavailable for mono output';
    control.title = root.title;
    if (lockedByAzKeys) input.title = root.title;
    if (id === 'mix' && store.state.parameters.effective.mixEffective.ignored) eff.textContent = 'ignored · Solo';
    else if (id === 'mix' && store.state.parameters.values.spatialDry) eff.textContent = 'positioned dry';
    if (/^s\d+g$/.test(id) && value <= -60) eff.textContent = '−∞ / silence';
  }
  control.addEventListener('pointerdown', e => {
    if (e.button !== 0 || disabled()) return;
    const d = desc();
    e.preventDefault(); begin(); pointer = e.pointerId; startY = e.clientY; startX = e.clientX; startNormal = toNormalised(d, current()); control.setPointerCapture(pointer);
    if (!knob) { const rect = control.getBoundingClientRect(); startNormal = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width)); send(fromNormalised(d, startNormal), true); }
  });
  control.addEventListener('pointermove', e => {
    if (e.pointerId !== pointer) return;
    const d = desc();
    const rect = control.getBoundingClientRect(), delta = knob ? (startY - e.clientY) / 130 : (e.clientX - startX) / rect.width;
    send(fromNormalised(d, startNormal + delta * (e.shiftKey ? 0.1 : 1)));
  });
  for (const event of ['pointerup', 'pointercancel', 'lostpointercapture']) control.addEventListener(event, end);
  control.addEventListener('dblclick', () => single(desc().default));
  control.addEventListener('wheel', e => { if (disabled() || !(document.documentElement.dataset.wheelValues === 'on' || e.altKey)) return; e.preventDefault(); const d = desc(); single(fromNormalised(d, toNormalised(d, current()) - Math.sign(e.deltaY) * (e.shiftKey ? 0.001 : 0.01))); }, { passive: false });
  control.addEventListener('keydown', e => {
    if (['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(e.key)) {
      e.preventDefault(); const d = desc(), dir = ['ArrowRight', 'ArrowUp'].includes(e.key) ? 1 : -1;
      single(e.key === 'Home' ? range(d)[0] : e.key === 'End' ? range(d)[1] : current() + dir * (d.step || (d.type === 'float' ? 0.001 : 1)) * (e.shiftKey ? 1 : 10));
    }
  });
  input.addEventListener('input',()=>{textCommitted=false;});
  input.addEventListener('change', () => { if(textCommitted)return; try { const value=fromText(desc(),input.value);textCommitted=true;single(value); input.setCustomValidity(''); } catch (e) { input.setCustomValidity(e.message); input.reportValidity(); } });
  input.addEventListener('blur', update);
  input.addEventListener('keydown', e => { if (e.key === 'Enter') { input.dispatchEvent(new Event('change')); input.blur(); } if (e.key === 'Escape') { textCommitted=true;input.value = toText(desc(), current()); input.setCustomValidity(''); input.blur(); } });
  const abort = () => { pending=null;if(frame!==null){cancelAnimationFrame(frame);frame=null;}
    const capture=pointer;const wasActive=active;active=false;pointer=null;root.removeAttribute('data-gesture-active');
    try{bridge.parameters.end(id);}catch{} if(capture!==null&&control.hasPointerCapture(capture))control.releasePointerCapture(capture);
    if(wasActive)window.dispatchEvent(new Event('verb-control-end'));
  };
  for(const event of ['verb-bridge-error','verb-gestures-reset','verb-ui-error'])window.addEventListener(event,abort);
  const cancel = () => end(); window.addEventListener('blur', cancel);
  const visibility = () => { if (document.hidden) end(); }; document.addEventListener('visibilitychange', visibility);
  const off = store.subscribe(({reason}) => { if (['parameters','snapshot','patch','resync'].includes(reason)) update(); }); root.append(...(knob ? [control, input, name, eff] : [name, control, input, eff]));
  const offField=valueField(input,{get:()=>{const s=displayedState();return s.locked&&s.isAz?s.effective:s.value;},min:()=>range(desc())[0],max:()=>range(desc())[1],step:()=>{const d=desc();return d.step||(d.type==='float'?0.001:1);},display:text,edit:v=>toText(desc(),v),start:begin,preview:v=>send(v),commit:v=>send(v,true),end});
  update();
  root.dispose = () => { for(const event of ['verb-bridge-error','verb-gestures-reset','verb-ui-error'])window.removeEventListener(event,abort); offField(); end(); if (frame !== null) cancelAnimationFrame(frame); off(); window.removeEventListener('blur', cancel); document.removeEventListener('visibilitychange', visibility); };
  return root;
}
