import { button, element } from './dom.js';
export function tabs(items, active, change, label = 'Tabs') {
  const buttons = items.map(([key, text]) => button(text, () => change(key), { role: 'tab', id: `tab-${key}`, 'data-focus-key': `tab-${key}`, 'aria-selected': String(key === active), tabIndex: key === active ? 0 : -1, className: key === active ? 'active' : '' }));
  buttons.forEach((b, i) => b.addEventListener('keydown', e => {
    if (['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(e.key)) {
      e.preventDefault(); const index = e.key === 'Home' ? 0 : e.key === 'End' ? items.length - 1 : (i + (e.key === 'ArrowRight' ? 1 : -1) + items.length) % items.length;
      change(items[index][0]);
      requestAnimationFrame(() => document.getElementById(`tab-${items[index][0]}`)?.focus());
    }
  }));
  return element('div', { className: 'tabs', role: 'tablist', 'aria-label': label }, buttons);
}
