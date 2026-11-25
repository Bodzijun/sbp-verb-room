import { element } from './dom.js';
const lastPx = new WeakMap();
// Opaque per-view state, shared with camera/selection/accordion persistence.
export function inspectorSplitter(rack, store) {
  const bar = element('div', { className: 'inspector-splitter', role: 'separator', tabIndex: 0,
    'aria-label': 'Inspector / tabs split', 'aria-orientation': 'horizontal', 'data-focus-key': 'inspector-split',
    title: 'Drag to resize · Arrow Up/Down · double-click or Home resets' });
  let share = store.view.inspectorSplit ?? null, drag = null;
  if (lastPx.has(store)) rack.style.setProperty('--inspector-h', `${lastPx.get(store)}px`);
  const limits = () => {
    const macro = rack.querySelector('.macro-row')?.getBoundingClientRect().height || 76;
    return { min: 176, max: Math.max(176, rack.clientHeight - macro - 18 - 170) };
  };
  const apply = value => {
    const { min, max } = limits();
    // Default proportion: 70 % of the space shared by the inspector and the tabs goes to the inspector (top), 30 % to the tabs.
    const shared = rack.clientHeight - (rack.querySelector('.macro-row')?.getBoundingClientRect().height || 76) - 18;
    const px = Math.max(min, Math.min(max, value == null ? 0.7 * shared : value * rack.clientHeight));
    lastPx.set(store, px);
    rack.style.setProperty('--inspector-h', `${px}px`);
    bar.setAttribute('aria-valuemin', min); bar.setAttribute('aria-valuemax', max); bar.setAttribute('aria-valuenow', Math.round(px));
  };
  const commit = value => { share = value; apply(value); store.setView({ inspectorSplit: value }); };
  bar.addEventListener('pointerdown', e => {
    if (e.button !== 0) return;
    drag = { y: e.clientY, height: Number(bar.getAttribute('aria-valuenow')) };
    bar.setAttribute('data-gesture-active', 'true'); bar.setPointerCapture(e.pointerId); bar.classList.add('dragging'); e.preventDefault();
  });
  bar.addEventListener('pointermove', e => { if (drag) apply((drag.height + e.clientY - drag.y) / rack.clientHeight); });
  const end = e => {
    if (!drag) return;
    drag = null; bar.removeAttribute('data-gesture-active'); bar.classList.remove('dragging');
    if (e.type === 'pointerup') commit(Number(bar.getAttribute('aria-valuenow')) / rack.clientHeight);
    else apply(share);
    window.dispatchEvent(new Event('verb-control-end'));
  };
  bar.addEventListener('pointerup', end); bar.addEventListener('pointercancel', end);
  bar.addEventListener('dblclick', () => commit(null));
  bar.addEventListener('keydown', e => {
    if (!['ArrowUp', 'ArrowDown', 'Home'].includes(e.key)) return;
    e.preventDefault();
    if (e.key === 'Home') commit(null);
    else { apply((Number(bar.getAttribute('aria-valuenow')) + (e.key === 'ArrowUp' ? -16 : 16)) / rack.clientHeight); commit(Number(bar.getAttribute('aria-valuenow')) / rack.clientHeight); }
  });
  const observer = new ResizeObserver(() => apply(share)); observer.observe(rack);
  bar.dispose = () => observer.disconnect();
  return bar;
}

