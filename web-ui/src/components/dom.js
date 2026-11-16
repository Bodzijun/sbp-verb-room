export function element(tag, attrs = {}, children = []) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else if (key === 'className') node.className = value;
    else if (key === 'text') node.textContent = value;
    else if (key in node && !key.startsWith('aria') && key !== 'role') node[key] = value;
    else node.setAttribute(key, value);
  }
  for (const child of [children].flat(Infinity)) if (child !== null && child !== undefined) node.append(typeof child === 'string' ? document.createTextNode(child) : child);
  return node;
}
export const button = (label, action, attrs = {}) => element('button', { type: 'button', text: label, onclick: action, ...attrs });

// ── Custom dark dropdown. Native <select> popups are white and OS-styled, so the
// control is a button + listbox portal. Keeps role=combobox for assistive tech.
let openDropdown = null;
export function closeDropdowns() { if (openDropdown) openDropdown(); }
export function select(label, values, value, change, attrs = {}) {
  const { reselect = false, ...attributes } = attrs;
  const items = values.map((v, i) => { const [key, text] = Array.isArray(v) ? v : [i, v]; return { key: String(key), text: String(text) }; });
  let current = String(value), popup = null, highlighted = -1, search = null, closeFn = null;
  const labelOf = key => items.find(i => i.key === key)?.text ?? '';
  const caption = element('span', { className: 'dd-text', text: labelOf(current) }), caret = element('i', { className: 'dd-caret', 'aria-hidden': 'true' });
  const root = element('button', { type: 'button', className: 'dd', role: 'combobox', 'aria-label': label, 'aria-haspopup': 'listbox', 'aria-expanded': 'false', ...attributes }, [caption, caret]);
  Object.defineProperty(root, 'value', { get: () => current, set: v => { current = String(v); caption.textContent = labelOf(current); } });
  const visible = () => popup ? [...popup.querySelectorAll('[role=option]:not([hidden])')] : [];
  function paint() { visible().forEach((o, i) => o.classList.toggle('hl', i === highlighted)); const o = visible()[highlighted]; o?.scrollIntoView?.({ block: 'nearest' }); }
  function close() {
    if (!popup) return; popup.remove(); popup = null; closeFn = null; openDropdown = null; root.setAttribute('aria-expanded', 'false');
    document.removeEventListener('pointerdown', outside, true); window.removeEventListener('blur', close); window.removeEventListener('resize', close);
  }
  function outside(e) { if (!popup?.contains(e.target) && !root.contains(e.target)) close(); }
  function choose(key) { close(); if (key === current && !reselect) return; current = key; caption.textContent = labelOf(key); change(key); }
  function open() {
    closeDropdowns(); if (root.disabled) return;
    popup = element('div', { className: 'dd-popup', role: 'listbox', 'aria-label': label });
    if (items.length > 10) { search = element('input', { type: 'text', className: 'dd-search', placeholder: 'Search…', 'aria-label': `${label} search` }); popup.append(search);
      search.addEventListener('input', () => { for (const o of popup.querySelectorAll('[role=option]')) o.hidden = !o.textContent.toLowerCase().includes(search.value.toLowerCase()); highlighted = 0; paint(); });
      search.addEventListener('keydown', keys); }
    const list = element('div', { className: 'dd-list scroll' });
    items.forEach(item => list.append(element('div', { className: `dd-option${item.key === current ? ' sel' : ''}`, role: 'option', 'data-value':item.key, 'aria-selected': String(item.key === current), text: item.text, onpointerdown: e => { e.preventDefault(); choose(item.key); } })));
    popup.append(list); document.body.append(popup);
    const r = root.getBoundingClientRect(), h = Math.min(popup.offsetHeight, 260), below = innerHeight - r.bottom - 6, up = below < h && r.top > below;
    popup.style.minWidth = `${Math.max(r.width, 96)}px`; popup.style.left = `${Math.max(4, Math.min(r.left, innerWidth - popup.offsetWidth - 4))}px`;
    popup.style.top = up ? `${Math.max(4, r.top - h - 2)}px` : `${r.bottom + 2}px`; popup.style.maxHeight = `${Math.max(80, up ? r.top - 8 : below)}px`;
    highlighted = Math.max(0, items.findIndex(i => i.key === current)); paint();
    root.setAttribute('aria-expanded', 'true'); closeFn = close; openDropdown = close;
    document.addEventListener('pointerdown', outside, true); window.addEventListener('blur', close); window.addEventListener('resize', close);
    search?.focus({ preventScroll: true });
  }
  function keys(e) {
    if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close(); root.focus({ preventScroll: true }); return; }
    if (!popup) { if (['ArrowDown', 'ArrowUp', 'Enter', ' '].includes(e.key)) { e.preventDefault(); open(); } return; }
    const list = visible();
    if (e.key === 'ArrowDown') { e.preventDefault(); highlighted = Math.min(list.length - 1, highlighted + 1); paint(); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); highlighted = Math.max(0, highlighted - 1); paint(); }
    else if (e.key === 'Home' || e.key === 'End') { if (e.target === root) { e.preventDefault(); highlighted = e.key === 'Home' ? 0 : list.length - 1; paint(); } }
    else if (e.key === 'Enter' || (e.key === ' ' && e.target === root)) { e.preventDefault(); const o = list[highlighted]; if (o) choose(items[[...popup.querySelectorAll('[role=option]')].indexOf(o)].key); root.focus({ preventScroll: true }); }
    else if (e.key === 'Tab') close();
  }
  root.addEventListener('click', () => (popup ? close() : open())); root.addEventListener('keydown', keys);
  root.dispose = () => close();
  return root;
}

// ── Square state switch (no native checkbox). The element itself is the switch.
export function check(label, value, change, attrs = {}) {
  const lamp = element('i', { className: 'led', 'aria-hidden': 'true' });
  const root = element('button', { type: 'button', className: `check ${value ? 'is-on' : 'is-off'}`, role: 'switch', 'aria-checked': String(!!value), ...attrs }, [lamp, element('span', { text: label })]);
  const set = on => { root.setAttribute('aria-checked', String(!!on)); root.classList.toggle('is-on', !!on); root.classList.toggle('is-off', !on); };
  Object.defineProperty(root, 'checked', { get: () => root.getAttribute('aria-checked') === 'true', set });
  root.setChecked = set;
  root.addEventListener('click', () => { if (root.disabled) return; const on = !root.checked; set(on); change(on); });
  return root;
}
export const row = (label, control) => element('div', { className: 'row' }, [element('span', { text: label }), control]);
export const note = text => element('p', { className: 'note', text });
export function unavailable(state, key, label = key) {
  const capability = state.capabilities.features[key];
  return element('span', { className: 'unavailable', title: capability?.reasonCode || key }, [label, element('span', { className: 'availability', text: ` · ${capability?.status || 'unavailable'}` })]);
}

// ── Slim scroll: native scrollbars are hidden; a 4 px overlay thumb matches the
// chrome. Only used where content can genuinely be long (32 sources, outliner,
// materials). The scrolling element keeps the `scroll` class so app.js restores it.
export function scrollArea(children, attrs = {}) {
  const { className = '', rootClass = '', ...rest } = attrs;
  const view = element('div', { className: `sa-view scroll ${className}`.trim(), ...rest }, children), thumb = element('div', { className: 'sa-thumb' });
  const track = element('div', { className: 'sa-track', 'aria-hidden': 'true' }, thumb), root = element('div', { className: `sa ${rootClass}`.trim() }, [view, track]);
  const update = () => {
    const { scrollTop, scrollHeight, clientHeight } = view, extra = scrollHeight - clientHeight > 1;
    track.hidden = !extra; if (!extra) return;
    const room = track.clientHeight, size = Math.max(22, room * clientHeight / scrollHeight);
    thumb.style.height = `${size}px`; thumb.style.transform = `translateY(${(room - size) * scrollTop / (scrollHeight - clientHeight)}px)`;
  };
  view.addEventListener('scroll', update, { passive: true });
  let drag = null;
  thumb.addEventListener('pointerdown', e => { e.preventDefault(); thumb.setPointerCapture(e.pointerId); drag = { y: e.clientY, top: view.scrollTop }; });
  thumb.addEventListener('pointermove', e => { if (!drag) return; const room = track.clientHeight - thumb.offsetHeight; if (room > 0) view.scrollTop = drag.top + (e.clientY - drag.y) * (view.scrollHeight - view.clientHeight) / room; });
  thumb.addEventListener('pointerup', () => { drag = null; }); thumb.addEventListener('pointercancel', () => { drag = null; });
  track.addEventListener('pointerdown', e => { if (e.target === track) view.scrollTop += (e.offsetY > thumb.offsetTop ? 1 : -1) * view.clientHeight * 0.9; });
  const observer = typeof ResizeObserver === 'function' ? new ResizeObserver(update) : null;
  observer?.observe(view); for (const child of view.children) observer?.observe(child);
  requestAnimationFrame(update);
  root.view = view; root.update = update; root.dispose = () => observer?.disconnect();
  return root;
}
