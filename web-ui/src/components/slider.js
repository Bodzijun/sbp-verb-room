import { element, row } from './dom.js';
import { displayNumber } from '../bridge/parameters.js';
import { valueField } from './value-field.js';
export function numericField(label,value,min,max,step,commit) {
  let current=value;
  const input=element('input',{type:'text',className:'readout','aria-label':label,'data-focus-key':label});
  const submit=()=>{const n=Number(input.value);if(input.value.trim()&&Number.isFinite(n)&&n>=min&&n<=max){current=n;input.setCustomValidity('');commit(n);}else{input.setCustomValidity(`Enter ${min}…${max}`);input.reportValidity();}};
  input.addEventListener('change',submit);
  input.addEventListener('keydown',e=>{if(e.key==='Enter'){submit();input.blur();}if(e.key==='Escape'){input.value=String(current);input.blur();}});
  input.dispose=valueField(input,{get:()=>current,min,max,step,display:n=>String(Number(n.toFixed(3))),commit:n=>{current=n;commit(n);}});
  return input;
}
// Structural numeric controls submit on commit; ranges are from command schemas.
// Custom track (no native <input type=range>) so every slider shares the instrument look.
export function slider(label, value, min, max, step, commit, unit = '', {live=false,locked=false,reason=''}={}) {
  let current = value;
  const track = element('div', { className: 'track', role: 'slider', tabIndex: locked ? -1 : 0, 'aria-label': label, 'aria-valuemin': min, 'aria-valuemax': max, 'aria-valuenow': value, ...(locked ? { 'aria-disabled': 'true' } : {}) }, element('i'));
  const text = element('input', { type: 'text', className: 'readout', value: displayNumber(value, ''), 'aria-label': `${label} value`, 'data-focus-key': label, disabled: locked });
  const norm = v => max > min ? Math.max(0, Math.min(1, (v - min) / (max - min))) : 0;
  const snap = v => Number((Math.round((Math.max(min, Math.min(max, v)) - min) / step) * step + min).toFixed(6));
  const paint = v => { track.style.setProperty('--normal', norm(v)); track.setAttribute('aria-valuenow', v); };
  paint(value);
  const submit = raw => {
    if (locked) return;
    const n = Number(raw); if (!String(raw).trim() || !Number.isFinite(n) || n < min || n > max) { text.setCustomValidity(`Enter ${min}…${max}`); text.reportValidity(); return; }
    text.setCustomValidity(''); current = snap(n); paint(current); commit(current);
  };
  let pointer = null, preview = value,lastPreview=0;
  const fromEvent = e => { const r = track.getBoundingClientRect(); return snap(min + Math.max(0, Math.min(1, (e.clientX - r.left) / Math.max(1, r.width))) * (max - min)); };
  track.addEventListener('pointerdown', e => { if (locked || e.button !== 0) return; e.preventDefault();if(live)track.setAttribute('data-gesture-active','true');track.focus(); pointer = e.pointerId; track.setPointerCapture(pointer); preview = fromEvent(e); paint(preview); text.value = displayNumber(preview, ''); });
  track.addEventListener('pointermove', e => { if (e.pointerId !== pointer) return; preview = fromEvent(e); paint(preview); text.value = displayNumber(preview, '');if(live&&performance.now()-lastPreview>=50){lastPreview=performance.now();commit(preview);} });
  const done = e => { if (e.pointerId !== pointer) return; pointer = null; submit(preview);track.removeAttribute('data-gesture-active');window.dispatchEvent(new Event('verb-control-end')); };
  track.addEventListener('pointerup', done); track.addEventListener('pointercancel', () => { pointer = null;track.removeAttribute('data-gesture-active');window.dispatchEvent(new Event('verb-control-end'));paint(current); text.value = displayNumber(current, ''); });
  track.addEventListener('keydown', e => {
    if (locked) return;
    const dir = ['ArrowRight', 'ArrowUp'].includes(e.key) ? 1 : ['ArrowLeft', 'ArrowDown'].includes(e.key) ? -1 : 0;
    if (dir) { e.preventDefault(); const n = snap(current + dir * step * (e.shiftKey ? 1 : 10)); text.value = displayNumber(n, ''); submit(n); }
  });
  text.addEventListener('change', () => submit(text.value));
  text.addEventListener('keydown', e => { if (e.key === 'Enter') { submit(text.value); text.blur(); } if (e.key === 'Escape') { text.value = displayNumber(current, ''); text.blur(); } });
  text.dispose = valueField(text, { get: () => current, min, max, step, display: n => displayNumber(n, ''), commit: n => { current = n; paint(n); submit(n); } });
  const root = row(unit ? `${label}, ${unit}` : label, element('div', { className: 'numeric' }, [track, text]));
  root.classList.add('value-slider');
  if (locked) {
    root.classList.add('locked');
    root.dataset.lockReason = reason;
    if (reason) {
      root.firstElementChild.append(element('small', { className: 'lock-reason', text: reason }));
      root.title = `${label} · ${displayNumber(value, unit)} · Locked (${reason}); stored value is preserved`;
      track.title = root.title;
      text.title = root.title;
    }
  }
  return root;
}
