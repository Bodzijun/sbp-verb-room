import { element, row } from './dom.js';
import { displayNumber } from '../bridge/parameters.js';
import { valueField } from './value-field.js';

export const tryCapture = (el, e) => {
  try { el.setPointerCapture?.(e.pointerId); return true; } catch { return false; }
};

export function createTxGesture(store, label, { throttleMs = 33 } = {}) {
  const tx = `tx-${Date.now()}-${Math.random().toString(36).slice(2, 6)}`;
  let began = false, last = 0, pending = null, sentKey = '', timer = null;
  const flush = () => {
    if (timer) { clearTimeout(timer); timer = null; }
    if (!pending) return;
    const cmds = pending; pending = null; last = performance.now();
    if (!began) { began = true; store.command({ op: 'transaction.begin', label }, tx); }
    for (const c of cmds) store.command(c, tx);
  };
  return {
    get txId() { return tx; },
    get isActive() { return began; },
    send(cmdOrCmds) {
      const cmds = Array.isArray(cmdOrCmds) ? cmdOrCmds : [cmdOrCmds];
      if (!cmds.length) return;
      const key = JSON.stringify(cmds);
      if (key === sentKey) return;
      sentKey = key;
      pending = cmds;
      if (throttleMs <= 0 || performance.now() - last >= throttleMs) flush();
      else if (!timer) timer = setTimeout(flush, throttleMs);
    },
    async end() {
      flush();
      if (began) await store.command({ op: 'transaction.commit' }, tx);
      began = false;
    },
    cancel() {
      if (timer) { clearTimeout(timer); timer = null; }
      pending = null;
      if (began) store.command({ op: 'transaction.cancel' }, tx);
      began = false;
    }
  };
}

export function txNumericField({
  store, label, txLabel = label, value, min, max, step = 0.1,
  disabled = false, title = '', className = 'readout', clampOnSubmit = false,
  liveInput = false, throttleMs = 33,
  display, edit, getCommitCmd, onPreview, onCancel
}) {
  let current = value, fg = null, committed = false, skipChange = false;
  let typingFg = null, typingInitial = value, rafInput = null;
  const fmt = display || (n => String(Number(n.toFixed(3))));
  const input = element('input', {
    type: 'text',
    className,
    disabled,
    'aria-label': label,
    'data-focus-key': label
  });
  const clampVal = v => {
    const lo = typeof min === 'function' ? min() : min;
    const hi = typeof max === 'function' ? max() : max;
    return Math.max(lo, Math.min(hi, v));
  };
  const closeTypingTx = async cancelled => {
    if (rafInput !== null) { cancelAnimationFrame(rafInput); rafInput = null; }
    const g = typingFg;
    typingFg = null;
    if (!g) return;
    if (cancelled) {
      g.cancel();
      current = typingInitial;
      onCancel?.(typingInitial);
    } else {
      await g.end();
    }
    input.removeAttribute('data-gesture-active');
    window.dispatchEvent(new Event('verb-control-end'));
  };
  const submit = () => {
    if (disabled || skipChange) return;
    const raw = input.value.trim();
    const n = Number(raw);
    const lo = typeof min === 'function' ? min() : min;
    const hi = typeof max === 'function' ? max() : max;
    if (!raw || !Number.isFinite(n)) {
      input.setCustomValidity(`Enter ${lo}…${hi}`);
      input.reportValidity();
      return;
    }
    if (n < lo || n > hi) {
      if (!clampOnSubmit) {
        input.setCustomValidity(`Enter ${lo}…${hi}`);
        input.reportValidity();
        return;
      }
      const clamped = Math.max(lo, Math.min(hi, n));
      input.setCustomValidity('');
      input.value = fmt(clamped);
      if (typingFg) {
        current = clamped;
        onPreview?.(clamped);
        typingFg.send(getCommitCmd(clamped));
        closeTypingTx(false);
      } else if (Math.abs(clamped - current) > 1e-9) {
        current = clamped;
        onPreview?.(clamped);
        store.command(getCommitCmd(clamped));
      }
      return;
    }
    input.setCustomValidity('');
    if (typingFg) {
      current = n;
      onPreview?.(n);
      typingFg.send(getCommitCmd(n));
      closeTypingTx(false);
    } else if (Math.abs(n - current) > 1e-9) {
      current = n;
      onPreview?.(n);
      store.command(getCommitCmd(n));
    }
  };
  input.addEventListener('input', () => {
    skipChange = false;
    if (!liveInput || disabled) return;
    const raw = input.value.trim();
    const n = Number(raw);
    const lo = typeof min === 'function' ? min() : min;
    const hi = typeof max === 'function' ? max() : max;
    if (!raw || !Number.isFinite(n) || n < lo || n > hi) return;
    input.setCustomValidity('');
    if (!typingFg) {
      typingInitial = current;
      typingFg = createTxGesture(store, txLabel, { throttleMs });
      input.setAttribute('data-gesture-active', 'true');
    }
    current = n;
    onPreview?.(n);
    typingFg.send(getCommitCmd(n));
  });
  input.addEventListener('change', submit);
  input.addEventListener('blur', () => {
    if (typingFg && !skipChange) closeTypingTx(false);
  });
  input.addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); submit(); input.blur(); }
    if (e.key === 'Escape') {
      e.preventDefault();
      skipChange = true;
      input.setCustomValidity('');
      if (typingFg) {
        closeTypingTx(true);
      }
      input.value = fmt(current);
      input.blur();
    }
  });
  const offVf = valueField(input, {
    get: () => current,
    min,
    max,
    step,
    display: fmt,
    edit: edit || (n => String(Number(n.toFixed(4)))),
    start: () => {
      committed = false;
      typingInitial = current;
      fg = createTxGesture(store, txLabel, { throttleMs });
    },
    preview: v => {
      const c = clampVal(v);
      onPreview?.(c);
      fg?.send(getCommitCmd(c));
    },
    commit: v => {
      const c = clampVal(v);
      current = c;
      if (fg) {
        committed = true;
        fg.send(getCommitCmd(c));
      } else {
        onPreview?.(c);
        store.command(getCommitCmd(c));
      }
    },
    end: async cancelled => {
      const g = fg; fg = null;
      if (g) {
        if (committed && !cancelled) await g.end();
        else {
          g.cancel();
          current = typingInitial;
          onCancel?.(typingInitial);
        }
      }
    }
  });
  if (title) input.title = title;
  input.dispose = () => {
    if (rafInput !== null) { cancelAnimationFrame(rafInput); rafInput = null; }
    if (typingFg) { typingFg.cancel(); typingFg = null; }
    offVf();
  };
  return input;
}

export function txSlider({
  store, label, txLabel = label, value, min, max, step,
  unit = '', disabled = false, title = '',
  display, getCommitCmd, onPreview
}) {
  let current = Math.max(min, Math.min(max, Number.isFinite(value) ? value : min));
  const norm = v => max > min ? Math.max(0, Math.min(1, (v - min) / (max - min))) : 0;
  const snap = v => Number((Math.round((Math.max(min, Math.min(max, v)) - min) / step) * step + min).toFixed(6));
  const fmt = display || (v => displayNumber(v, ''));

  const track = element('div', {
    className: 'tx-track',
    role: 'slider',
    tabIndex: disabled ? -1 : 0,
    'aria-label': label,
    'aria-valuemin': min,
    'aria-valuemax': max,
    'aria-valuenow': current,
    ...(disabled ? { 'aria-disabled': 'true' } : {}),
    ...(title ? { title } : {})
  }, element('i'));

  const field = txNumericField({
    store,
    label: `${label} value`,
    txLabel,
    value: current,
    min,
    max,
    step,
    disabled,
    title: title || `${label} · drag vertically · Shift fine · double-click to type`,
    display: fmt,
    getCommitCmd: v => getCommitCmd(snap(v)),
    onPreview: v => {
      current = snap(v);
      track.style.setProperty('--normal', norm(current));
      track.setAttribute('aria-valuenow', current);
      onPreview?.(current);
    }
  });

  const paint = v => {
    current = snap(v);
    track.style.setProperty('--normal', norm(current));
    track.setAttribute('aria-valuenow', current);
    if (document.activeElement !== field || field.readOnly) field.value = fmt(current);
  };
  paint(current);

  const fromEvent = e => {
    const r = track.getBoundingClientRect();
    return snap(min + Math.max(0, Math.min(1, (e.clientX - r.left) / Math.max(1, r.width))) * (max - min));
  };

  track.addEventListener('pointerdown', e => {
    if (disabled || e.button !== 0 || !store.ready) return;
    e.preventDefault();
    if (!tryCapture(track, e)) return;
    track.setAttribute('data-gesture-active', 'true');
    track.focus({ preventScroll: true });
    const g = createTxGesture(store, txLabel);
    let active = true;
    const apply = v => {
      paint(v);
      onPreview?.(v);
      g.send(getCommitCmd(v));
    };
    apply(fromEvent(e));
    const move = ev => { if (active && ev.pointerId === e.pointerId) apply(fromEvent(ev)); };
    const finish = async cancelled => {
      if (!active) return;
      active = false;
      track.removeEventListener('pointermove', move);
      track.removeEventListener('pointerup', up);
      track.removeEventListener('pointercancel', cancel);
      track.removeEventListener('lostpointercapture', cancel);
      window.removeEventListener('verb-gestures-reset', cancel);
      window.removeEventListener('verb-ui-error', cancel);
      if (cancelled) g.cancel();
      else await g.end();
      track.removeAttribute('data-gesture-active');
      window.dispatchEvent(new Event('verb-control-end'));
    };
    const up = () => finish(false), cancel = () => finish(true);
    track.addEventListener('pointermove', move);
    track.addEventListener('pointerup', up);
    track.addEventListener('pointercancel', cancel);
    track.addEventListener('lostpointercapture', cancel);
    window.addEventListener('verb-gestures-reset', cancel);
    window.addEventListener('verb-ui-error', cancel);
  });

  track.addEventListener('keydown', e => {
    if (disabled) return;
    const dir = ['ArrowRight', 'ArrowUp'].includes(e.key) ? 1 : ['ArrowLeft', 'ArrowDown'].includes(e.key) ? -1 : 0;
    if (!dir) return;
    e.preventDefault();
    const mult = e.shiftKey ? 1 : 10;
    const next = snap(current + dir * step * mult);
    if (Math.abs(next - current) > 1e-9) {
      paint(next);
      onPreview?.(next);
      store.command(getCommitCmd(next));
    }
  });

  const root = row(unit ? `${label}, ${unit}` : label, element('div', { className: 'numeric' }, [track, field]));
  root.classList.add('value-slider');
  if (disabled) root.classList.add('locked');
  if (title) root.firstElementChild.title = title;
  root.dispose = () => field.dispose?.();
  return root;
}
