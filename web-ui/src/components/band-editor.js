import { element, note } from './dom.js';
import { BANDS } from '../state/selectors.js';
export function bandEditor(store, refs, material) {
  const root = element('div', { className: 'band-editor', 'aria-label': 'Material absorption by octave band' });
  BANDS.forEach((label, band) => {
    const value = material.alpha[band], field = element('input', { type: 'text', className: 'readout', value: value.toFixed(3), 'aria-label': `${label} Hz absorption value`, 'data-focus-key': `alpha-${band}` });
    const bar = element('div', { className: 'absorption-bar', role: 'slider', tabIndex: 0, 'aria-label': `${label} Hz absorption`, 'aria-valuemin': 0, 'aria-valuemax': 1, 'aria-valuenow': value }, element('i'));
    const commands = cleanRefs => ({ op: 'surface.setAbsorption', refs: cleanRefs, bands: [{ band, alpha: Number(field.value) }] });
    const cleanRefs = refs.map(({ kind, index }) => ({ kind, index }));
    let dragging = false, initialY = 0, tx = null, last = 0;
    const preview = n => { n = Math.max(0, Math.min(1, n)); field.value = n.toFixed(3); bar.style.setProperty('--alpha', n); bar.setAttribute('aria-valuenow', n); };
    const send = () => store.command(commands(cleanRefs), tx);
    const end = cancel => {
      if (!dragging) return; if (!cancel) send(); store.command({ op: cancel ? 'transaction.cancel' : 'transaction.commit' }, tx); dragging = false; tx = null;
      bar.removeAttribute('data-gesture-active'); window.dispatchEvent(new Event('verb-control-end'));
    };
    preview(value);
    bar.addEventListener('pointerdown', e => { if (e.button !== 0) return; e.preventDefault(); dragging = true; bar.setAttribute('data-gesture-active', 'true'); initialY = e.clientY; tx = `alpha-${band}-${Date.now()}`; store.command({ op: 'transaction.begin', label: 'Material absorption' }, tx); bar.setPointerCapture(e.pointerId); });
    bar.addEventListener('pointermove', e => { if (!dragging) return; const rect = bar.getBoundingClientRect(); preview(value + (initialY - e.clientY) / rect.height * (e.shiftKey ? 0.1 : 1)); if (performance.now() - last >= 50) { last = performance.now(); send(); } });
    bar.addEventListener('pointerup', () => end(false)); bar.addEventListener('pointercancel', () => end(true)); bar.addEventListener('lostpointercapture', () => end(true));
    bar.addEventListener('keydown', e => { if (['ArrowUp', 'ArrowDown'].includes(e.key)) { e.preventDefault(); preview(Number(field.value) + (e.key === 'ArrowUp' ? 1 : -1) * (e.shiftKey ? 0.001 : 0.01)); send(); } });
    field.addEventListener('change', () => { const n = Number(field.value); if (field.value.trim() && Number.isFinite(n) && n >= 0 && n <= 1) { preview(n); send(); } else { field.setCustomValidity('Enter 0…1'); field.reportValidity(); } });
    const column = element('div', { className: 'band-column' }, [field, bar, element('small', { text: label })]);
    const blur = () => end(true), visibility = () => { if (document.hidden) end(true); };
    window.addEventListener('blur', blur); document.addEventListener('visibilitychange', visibility);
    const off = store.subscribe(({reason}) => { if (reason === 'resync' || reason === 'transactionfailed') end(true); });
    column.dispose = () => { end(true); off(); window.removeEventListener('blur', blur); document.removeEventListener('visibilitychange', visibility); }; root.append(column);
  }); return root;
}
