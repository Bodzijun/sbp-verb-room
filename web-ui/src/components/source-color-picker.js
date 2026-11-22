import { element, button, closeDropdowns } from './dom.js';
import {
  SOURCE_COLOUR_PALETTE,
  AUTO_STRIP_COLOURS,
  isValidSourceColorHex,
  normalizeSourceColor,
  contrastTextColour
} from '../viewport/colour.js';

let activePopoverClose = null;
export function closeSourceColorPopovers() {
  if (activePopoverClose) activePopoverClose(false);
}

export function openSourceColorPopover({ anchor, source, index, onSelect }) {
  closeDropdowns();
  closeSourceColorPopovers();

  const current = isValidSourceColorHex(source?.color) ? source.color.toLowerCase() : '';
  const autoHex = AUTO_STRIP_COLOURS[((index % AUTO_STRIP_COLOURS.length) + AUTO_STRIP_COLOURS.length) % AUTO_STRIP_COLOURS.length];

  const popover = element('div', {
    className: 'source-color-popover',
    role: 'dialog',
    'aria-label': `Source ${index + 1} colour`
  });

  let closed = false;
  const close = (restoreFocus = false) => {
    if (closed) return;
    closed = true;
    popover.remove();
    if (activePopoverClose === close) activePopoverClose = null;
    document.removeEventListener('pointerdown', onOutside, true);
    document.removeEventListener('keydown', onKeyDown, true);
    window.removeEventListener('blur', onBlur);
    window.removeEventListener('resize', onResize);
    if (restoreFocus && anchor?.isConnected) anchor.focus({ preventScroll: true });
  };
  const pick = hex => {
    close(false);
    onSelect?.(hex);
  };

  const title = element('span', { className: 'source-color-title', text: `Source ${String(index + 1).padStart(2, '0')}` });
  const autoBtn = button('', () => pick(''), {
    className: `source-color-auto${!current ? ' active' : ''}`,
    'aria-label': 'Auto (index)',
    'aria-pressed': String(!current),
    title: 'Use automatic index-based colour'
  });
  autoBtn.append(
    element('i', { className: 'source-color-auto-dot', style: `background:${autoHex}`, 'aria-hidden': 'true' }),
    element('span', { text: 'Auto' })
  );
  const head = element('div', { className: 'source-color-head' }, [title, autoBtn]);

  const grid = element('div', { className: 'source-color-grid', role: 'group', 'aria-label': 'Palette swatches' },
    SOURCE_COLOUR_PALETTE.map(item => {
      const selected = current === item.hex;
      const fg = contrastTextColour(item.hex);
      return button(selected ? '✓' : '', () => pick(item.hex), {
        className: `source-color-swatch${selected ? ' active' : ''}`,
        style: `background:${item.hex};color:${fg}`,
        title: `${item.name} · ${item.hex}`,
        'aria-label': `${item.name} (${item.hex})`,
        'aria-pressed': String(selected),
        'data-color': item.hex
      });
    })
  );

  const hexInput = element('input', {
    type: 'text',
    className: 'source-color-hex',
    placeholder: '#rrggbb',
    maxLength: 7,
    value: current,
    'aria-label': 'Hex colour'
  });
  const nativePicker = element('input', {
    type: 'color',
    className: 'swatch source-color-native',
    value: current || autoHex,
    'aria-label': 'Custom colour picker',
    title: 'Pick custom colour',
    onchange: e => {
      const norm = normalizeSourceColor(e.target.value);
      if (norm) pick(norm);
    }
  });
  const errorNote = element('div', { className: 'note warning source-color-error', role: 'alert', hidden: true });

  const submitHex = () => {
    const raw = hexInput.value.trim();
    const norm = normalizeSourceColor(raw);
    if (norm === null) {
      errorNote.textContent = 'Enter #rrggbb or leave empty for Auto';
      errorNote.hidden = false;
      hexInput.focus();
      return;
    }
    errorNote.hidden = true;
    pick(norm);
  };

  hexInput.addEventListener('input', () => {
    errorNote.hidden = true;
    const norm = normalizeSourceColor(hexInput.value.trim());
    if (norm) nativePicker.value = norm;
  });
  hexInput.addEventListener('keydown', e => {
    if (e.key === 'Enter') {
      e.preventDefault();
      e.stopPropagation();
      submitHex();
    }
  });

  const applyBtn = button('Apply', submitHex, { className: 'source-color-apply', 'aria-label': 'Apply hex colour' });
  const customRow = element('div', { className: 'source-color-custom' }, [nativePicker, hexInput, applyBtn]);

  popover.append(head, grid, customRow, errorNote);
  document.body.append(popover);

  const r = anchor?.getBoundingClientRect?.() || { left: 40, right: 60, top: 40, bottom: 60 };
  const pw = popover.offsetWidth || 216;
  const ph = popover.offsetHeight || 152;
  const vw = globalThis.innerWidth || 1280;
  const vh = globalThis.innerHeight || 800;
  const left = Math.max(4, Math.min(r.left, vw - pw - 4));
  const below = vh - r.bottom - 6;
  const top = below < ph && r.top > below ? Math.max(4, r.top - ph - 4) : Math.max(4, Math.min(r.bottom + 4, vh - ph - 4));
  popover.style.left = `${left}px`;
  popover.style.top = `${top}px`;

  function onOutside(e) {
    if (!popover.contains(e.target) && !anchor?.contains?.(e.target)) close(false);
  }
  function onKeyDown(e) {
    if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      close(true);
    }
  }
  const onBlur = () => close(false);
  const onResize = () => close(false);

  activePopoverClose = close;
  document.addEventListener('pointerdown', onOutside, true);
  document.addEventListener('keydown', onKeyDown, true);
  window.addEventListener('blur', onBlur);
  window.addEventListener('resize', onResize);

  const focusTarget = popover.querySelector('.source-color-swatch[aria-pressed="true"]') || popover.querySelector('.source-color-auto');
  focusTarget?.focus?.({ preventScroll: true });

  return { element: popover, close };
}
