// E20: path timeline panel. Horizontal axis = group Position 0..100 % (the engine's u). Everything shown comes from the store;
// every edit is an existing command (path.setFlags start/end, path.setPointTiming), so the inspector and the timeline never disagree.
import { element, button, scrollArea } from './dom.js';
import { valueField } from './value-field.js';
import { pathId, evaluateAzimuthKeys, orientationMode, withPathPoses } from '../viewport/movement-path.js';
import {
  timelineSections, pathRow, objectRef, newStart, newEnd, newKeyTime, shiftWindows, alignCommands, holdCommands, clamp, snap,
  FULL_VIEW, viewFrac, viewValue, zoomView, panView, rulerTicks, timeToAxis, PATH_EASINGS,
  AZ_EASINGS, AZ_AT_PLAYHEAD_TOL, newAzKeyTime, clampAzKeyTime, azAngleAt, azimuthCurvePath
} from './timeline-model.js';

const EASE_LABEL = { linear: 'Linear', easeIn: 'Ease in', easeOut: 'Ease out', easeInOut: 'Ease in-out' };
const AZ_EASE_LABEL = { linear: 'Linear', easeIn: 'Ease in', easeOut: 'Ease out', easeInOut: 'Ease in-out', hold: 'Hold' };
const SVG_NS = 'http://www.w3.org/2000/svg';
const pct = f => `${(f * 100).toFixed(3)}%`;
const fmt = v => `${Number(v.toFixed(2))}`;
const fmtAz = v => { const n = Number((v ?? 0).toFixed(1)); return `${n === 0 ? 0 : n}°`; };
let live = null;   // the panel currently shown (module-level keys/handlers talk to the newest one)
let pointerInside = false; // last pointer interaction was inside the panel (survives re-renders)
let lastTap = null; // survives the re-render between the two clicks of a double click
let pendingFocusAzKey = null; // { o, index } restored after keyboard arrow step
let pendingFocusAzSlider = false;

export function timelinePanel(ctx) {
  const { store, bridge } = ctx;
  const state = store.state;
  const view = store.view.timelineView && store.view.timelineView.hi > store.view.timelineView.lo ? store.view.timelineView : FULL_VIEW;
  const collapsed = store.view.timelineCollapsed || {};
  const sections = timelineSections(state);
  const root = element('section', { className: 'timeline-panel panel', role: 'region', 'aria-label': 'Path timeline', 'data-timeline': 'open' });
  const selectedObjects = new Set(store.view.selection.filter(r => r.kind === 'source' || r.kind === 'listener').map(r => r.kind === 'listener' ? 32 : r.index));
  const selPoint = store.view.pathPoint;
  let selAzKey = store.view.azKey;
  if (selAzKey) {
    const sp = state.document.paths?.[selAzKey.object];
    if (!sp || (sp.points?.length ?? 0) < 2 || !Array.isArray(sp.azKeys) || selAzKey.index < 0 || selAzKey.index >= sp.azKeys.length) {
      selAzKey = null;
      store.view.azKey = null;
    }
  }
  const refs = new Map();           // object -> { row, parts }  (live repaint during a drag)
  const sectionRefs = [];           // group header bar painters
  let tipEl = null, menu = null;

  // ---- helpers -----------------------------------------------------------------------------------------------------------------
  const zoomed = view.lo > 0 || view.hi < 100;
  const innerOf = el => el.closest('.tl-track').querySelector('.tl-inner');
  const valueAt = (e, el) => { const r = innerOf(el).getBoundingClientRect(); return viewValue(view, clamp((e.clientX - r.left) / Math.max(1, r.width), 0, 1)); };
  const setLeft = (el, v) => { el.style.left = pct(viewFrac(view, v)); };
  const setBar = (el, a, b) => { el.style.left = pct(viewFrac(view, a)); el.style.width = pct(Math.max(0, (b - a) / (view.hi - view.lo))); };
  const selectObject = (o, extra = {}) => store.setView({ selection: [objectRef(o)], pathPoint: store.view.pathPoint?.object === o ? store.view.pathPoint : null, azKey: store.view.azKey?.object === o ? store.view.azKey : null, ...extra }, false);
  const selectKey = (o, index) => store.setView({ selection: [objectRef(o)], pathPoint: { object: o, index, point: store.state.document.paths[o].points[index] }, azKey: null }, false);
  const selectAzKey = (o, index) => store.setView({ selection: [objectRef(o)], azKey: { object: o, index }, pathPoint: null }, false);
  const showTip = (el, v, text) => {
    const inner = innerOf(el);
    if (!tipEl || tipEl.parentNode !== inner) { tipEl?.remove(); tipEl = element('div', { className: 'tl-tip', role: 'status' }); inner.append(tipEl); }
    tipEl.textContent = text; setLeft(tipEl, clamp(v, view.lo, view.hi));
  };
  const hideTip = () => { tipEl?.remove(); tipEl = null; };
  const withPatch = (o, patch) => {
    const paths = store.state.document.paths.slice(), p = { ...store.state.document.paths[o], ...patch };
    paths[o] = p; return pathRow({ ...store.state, document: { ...store.state.document, paths } }, o);
  };

  // One gesture = one transaction = one undo step. Commands go out when the value changes, at most every 33 ms; the last one always goes.
  function gesture(label) {
    const tx = `tl-${Date.now()}-${Math.random().toString(36).slice(2, 6)}`;
    let began = false, last = 0, pending = null, sentKey = '', timer = null;
    const flush = () => {
      if (timer) { clearTimeout(timer); timer = null; }
      if (!pending) return;
      const cmds = pending; pending = null; last = performance.now();
      if (!began) { began = true; store.command({ op: 'transaction.begin', label }, tx); }
      for (const c of cmds) store.command(c, tx);
    };
    return {
      send(cmds) {
        const key = JSON.stringify(cmds); if (key === sentKey) return; sentKey = key; pending = cmds;
        if (performance.now() - last >= 33) flush(); else if (!timer) timer = setTimeout(flush, 33);
      },
      async end() { flush(); if (began) await store.command({ op: 'transaction.commit' }, tx); },
      cancel() { if (timer) clearTimeout(timer); pending = null; if (began) store.command({ op: 'transaction.cancel' }, tx); began = false; }
    };
  }
  // setPointerCapture throws InvalidStateError if the pointer is already gone (slow frame, released button); a throw here would leave data-gesture-active set.
  const tryCapture = (el, e) => { try { el.setPointerCapture?.(e.pointerId); return true; } catch { return false; } };
  // Generic pointer drag with gesture marker (the app defers its re-render while data-gesture-active is set).
  function drag(e, el, { onMove, label, onClick, onEnd }) {
    if (e.button !== 0 || !store.ready) return;
    if (!tryCapture(el, e)) return;
    root.setAttribute('data-gesture-active', 'true'); pointerInside = true;
    const g = label ? gesture(label) : null, start = { x: e.clientX, y: e.clientY };
    let moved = false, done = false;
    const finish = async cancelled => {
      if (done) return; done = true;
      el.removeEventListener('pointermove', move); el.removeEventListener('pointerup', up); el.removeEventListener('pointercancel', cancel); el.removeEventListener('lostpointercapture', cancel);
      window.removeEventListener('verb-gestures-reset', cancel); window.removeEventListener('verb-ui-error', cancel);
      hideTip();
      if (g) { if (cancelled) g.cancel(); else await g.end(); }
      if (!moved && !cancelled) onClick?.(e);
      onEnd?.(cancelled, moved);
      root.removeAttribute('data-gesture-active'); window.dispatchEvent(new Event('verb-control-end'));
    };
    const move = ev => { if (ev.pointerId !== e.pointerId) return; if (!moved && Math.hypot(ev.clientX - start.x, ev.clientY - start.y) < 3) return; moved = true; onMove(ev, g); };
    const up = () => finish(false), cancel = () => finish(true);
    el.addEventListener('pointermove', move); el.addEventListener('pointerup', up); el.addEventListener('pointercancel', cancel); el.addEventListener('lostpointercapture', cancel);
    window.addEventListener('verb-gestures-reset', cancel); window.addEventListener('verb-ui-error', cancel);
  }

  // ---- playhead scrub (same begin/change/end protocol as the Position slider) ----------------------------------------------------
  const paintDriverPosition = (driver, u) => {
    const pos = u * 100;
    for (const r of root.querySelectorAll(`[data-driver="${driver}"]`)) setLeft(r, pos);
    for (const [, entry] of refs) {
      if (entry.row.driver !== driver) continue;
      entry.current = { ...entry.current, position: pos };
      entry.paintAzPosition?.(pos, u);
      entry.paintLfoBand?.(pos);
    }
  };
  function scrub(e, el, driver) {
    if (e.button !== 0 || !store.ready) return;
    const id = pathId(driver);
    if (!tryCapture(el, e)) return; root.setAttribute('data-gesture-active', 'true');
    let active = true, pendingU = null, frame = null;
    bridge.parameters.begin(id);
    const apply = ev => {
      const u = Number(clamp(valueAt(ev, el) / 100, 0, 1).toFixed(4)); pendingU = u;
      paintDriverPosition(driver, u);
      if (frame === null) frame = requestAnimationFrame(() => { frame = null; if (pendingU !== null && active) { bridge.parameters.change(id, pendingU); pendingU = null; } });
    };
    apply(e);
    const move = ev => { if (ev.pointerId === e.pointerId) apply(ev); };
    const end = () => {
      if (!active) return; active = false;
      el.removeEventListener('pointermove', move); el.removeEventListener('pointerup', end); el.removeEventListener('pointercancel', end); el.removeEventListener('lostpointercapture', end);
      if (frame !== null) cancelAnimationFrame(frame);
      if (pendingU !== null) bridge.parameters.change(id, pendingU);
      bridge.parameters.end(id);
      root.removeAttribute('data-gesture-active'); window.dispatchEvent(new Event('verb-control-end'));
    };
    el.addEventListener('pointermove', move); el.addEventListener('pointerup', end); el.addEventListener('pointercancel', end); el.addEventListener('lostpointercapture', end);
  }

  // ---- easing menu / key commands --------------------------------------------------------------------------------------------------
  const closeMenu = () => { if (menu) { menu.remove(); menu = null; window.removeEventListener('pointerdown', outside, true); window.removeEventListener('keydown', menuKey, true); } };
  const outside = e => { if (menu && !menu.contains(e.target)) closeMenu(); };
  const menuKey = e => { if (e.key === 'Escape') { e.preventDefault(); e.stopImmediatePropagation(); closeMenu(); } };
  function setEasing(o, index, easing) {
    const p = store.state.document.paths[o], explicit = index === 0 || (p.times?.[index] !== null && p.times?.[index] !== undefined);
    if (explicit) { if ((p.easings?.[index] || 'linear') !== easing) store.command({ op: 'path.setPointTiming', object: o, index, easing }); return; }
    if (easing === 'linear') return;                                          // an Auto point stays Auto; non-linear promotes it to a key at its computed time
    const r = pathRow(store.state, o), time = newKeyTime(p, index, timeToAxis(r.start, r.end, r.autos.find(a => a.index === index)?.time ?? 0), true);
    store.command({ op: 'path.setPointTiming', object: o, index, time, easing });
  }
  // Hold plan for a point. An Auto point first gets the explicit time it already has (what a sideways drag would do), then the hold copy: one gesture.
  function holdPlan(o, index) {
    const p = store.state.document.paths[o];
    const explicit = index === 0 || (p.times?.[index] !== null && p.times?.[index] !== undefined);
    if (explicit) { const cmds = holdCommands(p, index, o); return cmds ? { cmds } : { reason: 'No room for a pause here (last point, or the next key is too close in time)' }; }
    if (!p.closed && index >= p.points.length - 1) return { reason: 'The last point is the End handle: nothing follows it to pause before' };
    const r = pathRow(store.state, o), time = newKeyTime(p, index, timeToAxis(r.start, r.end, r.autos.find(a => a.index === index)?.time ?? 0), true);
    const times = p.times ? p.times.slice() : Array(p.points.length).fill(null); times[index] = time;
    const cmds = holdCommands({ ...p, times }, index, o);
    return cmds ? { cmds: [{ op: 'path.setPointTiming', object: o, index, time }, ...cmds] } : { reason: 'No room for a pause here (the next key is too close in time)' };
  }
  function addHold(o, index) {
    const plan = holdPlan(o, index); if (!plan.cmds) return;
    const g = gesture('Hold at point ' + (index + 1)); g.send(plan.cmds); g.end();
  }
  function openMenu(e, o, index, current, canHold) {
    e.preventDefault(); closeMenu();
    const items = PATH_EASINGS.map(name => button(EASE_LABEL[name], () => { closeMenu(); setEasing(o, index, name); },
      { role: 'menuitemradio', 'aria-checked': String(name === current), className: name === current ? 'on' : '' }));
    if (canHold) { const plan = holdPlan(o, index); items.push(element('hr', { className: 'tl-menu-sep' }), button('Hold here (pause)', () => { closeMenu(); addHold(o, index); },
      { role: 'menuitem', 'data-hold': plan.cmds ? 'on' : 'disabled', disabled: !plan.cmds, title: plan.cmds ? 'Adds a copy of this point a little later in time: the object rests at this place in between' : plan.reason })); }
    menu = element('div', { className: 'tl-menu', role: 'menu', 'aria-label': 'Key easing' }, items);
    document.body.append(menu);                       // outside the panel: never clipped by it
    const w = menu.offsetWidth, h = menu.offsetHeight;
    menu.style.left = `${Math.max(2, Math.min(e.clientX, window.innerWidth - w - 2))}px`;
    menu.style.top = `${Math.max(2, Math.min(e.clientY - 4, window.innerHeight - h - 2))}px`;
    menu.querySelector('button')?.focus({ preventScroll: true });
    window.addEventListener('pointerdown', outside, true); window.addEventListener('keydown', menuKey, true);
  }
  const clearKeyTime = (o, index) => store.command({ op: 'path.setPointTiming', object: o, index, time: null });

  async function addAzKeyAt(o, t) {
    const rawAz = azAngleAt(store.state, o, t);
    const azNorm = Number(clamp(rawAz, -180, 180).toFixed(4));
    const az = azNorm === 0 ? 0 : azNorm;
    const reply = await store.command({ op: 'path.setAzimuthKey', object: o, t, az });
    if (reply?.kind === 'ack') {
      const keys = store.state.document.paths?.[o]?.azKeys || [];
      const idx = keys.findIndex(k => Math.abs(k.t - t) < 1e-3);
      if (idx >= 0) selectAzKey(o, idx);
    }
  }
  function removeAzKey(o, index) {
    if (store.view.azKey?.object === o) store.setView({ azKey: null }, false);
    store.command({ op: 'path.removeAzimuthKey', object: o, index });
  }
  function openAzMenu(e, o, index, current) {
    e.preventDefault(); closeMenu();
    const p = store.state.document.paths?.[o];
    if (!p || orientationMode(p) !== 'manual') return;
    const items = AZ_EASINGS.map(name => button(AZ_EASE_LABEL[name], () => {
      closeMenu();
      if ((p.azKeys?.[index]?.easing || 'linear') !== name) {
        store.command({ op: 'path.setAzimuthKey', object: o, index, easing: name });
      }
    }, { role: 'menuitemradio', 'aria-checked': String(name === current), className: name === current ? 'on' : '' }));
    items.push(
      element('hr', { className: 'tl-menu-sep' }),
      button('Delete key', () => { closeMenu(); removeAzKey(o, index); }, { role: 'menuitem' })
    );
    menu = element('div', { className: 'tl-menu', role: 'menu', 'aria-label': 'Azimuth key easing' }, items);
    document.body.append(menu);
    const w = menu.offsetWidth, h = menu.offsetHeight;
    menu.style.left = `${Math.max(2, Math.min(e.clientX, window.innerWidth - w - 2))}px`;
    menu.style.top = `${Math.max(2, Math.min(e.clientY - 4, window.innerHeight - h - 2))}px`;
    menu.querySelector('button')?.focus({ preventScroll: true });
    window.addEventListener('pointerdown', outside, true); window.addEventListener('keydown', menuKey, true);
  }

  // ---- building one row -----------------------------------------------------------------------------------------------------------
  function buildRow(row, inGroup) {
    const o = row.object, selected = selectedObjects.has(o);
    const parts = { keyEls: new Map(), autoEls: new Map(), lapEls: [], holdEls: [] };
    const label = button('', () => selectObject(o), { className: 'tl-name', 'aria-pressed': String(selected), 'aria-label': `Select ${row.label}`, title: `${row.label}${row.enabled ? '' : ' (path disabled)'}` });
    label.append(element('i', { className: 'tl-swatch', style: `--c:${row.colour}` }), element('span', { text: row.label }));
    const rowEl = element('div', { className: `tl-row${selected ? ' selected' : ''}${row.enabled ? '' : ' dim'}${inGroup ? ' in-group' : ''}`, 'data-object': String(o), style: `--c:${row.colour}` });
    const inner = element('div', { className: 'tl-inner' });
    const grid = element('div', { className: 'tl-grid' });
    const restL = element('div', { className: 'tl-rest' }), restR = element('div', { className: 'tl-rest' });
    const bar = element('div', { className: 'tl-bar', title: `${row.label}: ${fmt(row.start)} - ${fmt(row.end)} % (drag to shift the window)`, 'aria-label': `${row.label} window` });
    const hStart = element('div', { className: 'tl-h tl-h-start', role: 'slider', tabIndex: 0, 'aria-label': `${row.label} Start %`, 'aria-valuemin': 0, 'aria-valuemax': 100, 'data-handle': 'start' });
    const hEnd = element('div', { className: 'tl-h tl-h-end', role: 'slider', tabIndex: 0, 'aria-label': `${row.label} End %`, 'aria-valuemin': 0, 'aria-valuemax': 100, 'data-handle': 'end' });
    const lapsLabel = row.closed && row.laps > 1 ? element('span', { className: 'tl-laps', text: `×${row.laps} laps` }) : null;
    const ph = element('div', { className: 'tl-ph', 'data-driver': String(row.driver) });
    const lfoCfg = store.state.document.paths?.[row.driver]?.lfo;
    const lfoDepthPct = lfoCfg?.enabled && lfoCfg.depth > 0 ? lfoCfg.depth * 100 : 0;
    const lfoCreative = (store.state.parameters?.values?.mode ?? 0) >= 0.5;
    const lfoBand = lfoDepthPct > 0
      ? element('div', { className: `tl-lfo-band${lfoCreative ? '' : ' dim'}`, title: `Path LFO ±${Math.round(lfoDepthPct)}% (${lfoCfg.shape}, ${Number((lfoCfg.rateHz ?? 0.5).toFixed(2))} Hz · Creative mode only)` })
      : null;
    inner.append(grid, restL, restR, bar, ...(lfoBand ? [lfoBand] : []), ph);
    row.holds.forEach(() => { const d = element('i', { className: 'tl-hold', title: 'Hold: the object rests here' }); parts.holdEls.push(d); inner.append(d); });
    row.lapMarks.forEach(() => { const d = element('i', { className: 'tl-lap' }); parts.lapEls.push(d); inner.append(d); });
    for (const k of row.autos) { const d = element('div', { className: 'tl-auto', tabIndex: 0, role: 'button', 'data-point': String(k.index) }); parts.autoEls.set(k.index, d); inner.append(d); }
    for (const k of row.keys) { if (k.index === 0) continue; const d = element('div', { className: 'tl-key', tabIndex: 0, role: 'button', 'data-point': String(k.index) }); parts.keyEls.set(k.index, d); inner.append(d); }
    inner.append(hStart, hEnd);
    if (lapsLabel) bar.append(lapsLabel);
    const clip = element('div', { className: 'tl-clip' }, [inner]);
    const track = element('div', { className: 'tl-track', role: 'presentation' }, [clip]);
    rowEl.append(label, track);

    const paint = r => {
      setBar(bar, r.start, r.end);
      restL.style.left = '0'; restL.style.width = pct(Math.max(0, viewFrac(view, r.start)));
      restR.style.left = pct(viewFrac(view, r.end)); restR.style.width = pct(Math.max(0, 1 - viewFrac(view, r.end)));
      setLeft(hStart, r.start); setLeft(hEnd, r.end); setLeft(ph, r.position);
      if (lfoBand) setBar(lfoBand, Math.max(0, r.position - lfoDepthPct), Math.min(100, r.position + lfoDepthPct));
      hStart.setAttribute('aria-valuenow', String(r.start)); hEnd.setAttribute('aria-valuenow', String(r.end));
      hStart.title = `Start ${fmt(r.start)} % · drag to change · right-click: easing of point 1 (${EASE_LABEL[r.keys[0]?.easing || 'linear']})`;
      hEnd.title = `End ${fmt(r.end)} %`;
      r.lapMarks.forEach((v, i) => { if (parts.lapEls[i]) setLeft(parts.lapEls[i], v); });
      r.holds.forEach((h, i) => { if (parts.holdEls[i]) setBar(parts.holdEls[i], h.from, h.to); });
      for (const k of r.keys) {
        const d = parts.keyEls.get(k.index);
        if (k.index === 0) { hStart.dataset.ease = k.easing; continue; }
        if (!d) continue;
        setLeft(d, k.x); d.dataset.ease = k.easing; d.classList.toggle('sel', selPoint?.object === o && selPoint.index === k.index);
        d.title = `Point ${k.index + 1} · ${fmt(k.time)} % of window · ${EASE_LABEL[k.easing]}`;
        d.setAttribute('aria-label', `${row.label} point ${k.index + 1} key at ${fmt(k.time)} percent, ${EASE_LABEL[k.easing]}`);
      }
      for (const k of r.autos) {
        const d = parts.autoEls.get(k.index); if (!d) continue;
        setLeft(d, k.x); d.classList.toggle('sel', selPoint?.object === o && selPoint.index === k.index);
        d.title = `Point ${k.index + 1} · auto ${fmt(k.time)} % · drag to make it a key`;
        d.setAttribute('aria-label', `${row.label} point ${k.index + 1} automatic at ${fmt(k.time)} percent`);
      }
    };
    paint(row);
    const paintLfoBand = pos => { if (lfoBand) setBar(lfoBand, Math.max(0, pos - lfoDepthPct), Math.min(100, pos + lfoDepthPct)); };
    refs.set(o, { row, paint, paintLfoBand, parts, current: row });

    // empty track = scrub this row's Position driver
    track.addEventListener('pointerdown', e => {
      if (e.target.closest('.tl-bar,.tl-h,.tl-key,.tl-auto')) return;
      selectObject(o); scrub(e, track, row.driver);
    });
    // window shift by the bar body
    bar.addEventListener('pointerdown', e => {
      if (e.target.closest('.tl-key,.tl-auto,.tl-h')) return;
      const p0 = store.state.document.paths[o], origin = valueAt(e, bar), base = { start: p0.start ?? 0, end: p0.end ?? 100 };
      drag(e, bar, { label: `Shift ${row.label} window`, onClick: () => selectObject(o), onMove: (ev, g) => {
        const [w] = shiftWindows([{ ...base, object: o }], valueAt(ev, bar) - origin, ev.shiftKey);
        refs.get(o).current = withPatch(o, w); refs.get(o).paint(refs.get(o).current); paintSections();
        g.send([{ op: 'path.setFlags', object: o, start: w.start, end: w.end }]); showTip(bar, w.start, `${fmt(w.start)} - ${fmt(w.end)} %`);
      } });
    });
    const handle = (el, edge) => {
      el.addEventListener('pointerdown', e => {
        e.stopPropagation(); el.focus({ preventScroll: true });
        drag(e, el, { label: `${edge === 'start' ? 'Start' : 'End'} ${row.label}`, onClick: () => { selectObject(o); if (edge === 'start') selectKey(o, 0); }, onMove: (ev, g) => {
          const r = refs.get(o).current, v = edge === 'start' ? newStart(r, valueAt(ev, el), ev.shiftKey) : newEnd(r, valueAt(ev, el), ev.shiftKey);
          const next = withPatch(o, { [edge]: v }); refs.get(o).current = next; refs.get(o).paint(next); paintSections();
          g.send([{ op: 'path.setFlags', object: o, [edge]: v }]); showTip(el, v, `${edge === 'start' ? 'Start' : 'End'} ${fmt(v)} %`);
        } });
      });
      el.addEventListener('keydown', e => {
        const dir = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0; if (!dir) return;
        e.preventDefault(); e.stopPropagation();
        const r = refs.get(o).current, step = e.shiftKey ? 0.05 : 0.5;
        const v = edge === 'start' ? newStart(r, r.start + dir * step, true) : newEnd(r, r.end + dir * step, true);
        if (v !== r[edge]) store.command({ op: 'path.setFlags', object: o, [edge]: Number(v.toFixed(4)) });
      });
    };
    handle(hStart, 'start'); handle(hEnd, 'end');
    hStart.addEventListener('contextmenu', e => openMenu(e, o, 0, row.keys[0]?.easing || 'linear', true));
    const pointEl = (d, index, auto) => {
      d.addEventListener('pointerdown', e => {
        e.stopPropagation(); d.focus({ preventScroll: true });
        const p0 = store.state.document.paths[o];
        const isDouble = lastTap && lastTap.o === o && lastTap.index === index && performance.now() - lastTap.t < 400;
        lastTap = null;
        if (isDouble && !auto) { clearKeyTime(o, index); return; }
        drag(e, d, { label: `Time of point ${index + 1}`, onClick: () => { lastTap = { o, index, t: performance.now() }; selectKey(o, index); }, onMove: (ev, g) => {
          const t = newKeyTime(store.state.document.paths[o], index, valueAt(ev, d), ev.shiftKey);
          const times = (p0.times ? p0.times.slice() : Array(p0.points.length).fill(null)); times[index] = t;
          refs.get(o).current = withPatch(o, { times }); refs.get(o).paint(refs.get(o).current);
          g.send([{ op: 'path.setPointTiming', object: o, index, time: t }]);
          const r = refs.get(o).current; showTip(d, timeToAxis(r.start, r.end, t), `Point ${index + 1} · ${fmt(t)} %`);
        } });
      });
      d.addEventListener('contextmenu', e => openMenu(e, o, index, auto ? 'linear' : (store.state.document.paths[o].easings?.[index] || 'linear'), true));
      d.addEventListener('keydown', e => {
        const p = store.state.document.paths[o];
        if (e.key === 'Delete' || e.key === 'Backspace') { e.preventDefault(); e.stopPropagation(); if (!auto) clearKeyTime(o, index); return; }
        if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); e.stopPropagation(); selectKey(o, index); return; }
        const dir = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0; if (!dir) return;
        e.preventDefault(); e.stopPropagation();
        const r = pathRow(store.state, o), item = [...r.keys, ...r.autos].find(k => k.index === index); if (!item) return;
        const t = newKeyTime(p, index, timeToAxis(r.start, r.end, item.time) + dir * (e.shiftKey ? 0.05 : 0.5) * ((r.end - r.start) / 100 || 1), true);
        if (Math.abs(t - item.time) > 1e-9) store.command({ op: 'path.setPointTiming', object: o, index, time: t });
      });
    };
    parts.keyEls.forEach((d, i) => pointEl(d, i, false)); parts.autoEls.forEach((d, i) => pointEl(d, i, true));

    // ---- azimuth keyframe sub-row ------------------------------------------------------------------------------------------------
    const azDim = row.azMode !== 'manual' || !row.enabled;
    const azInactive = row.azMode !== 'manual';
    const azRowEl = element('div', {
      className: `tl-row tl-az-row${selected ? ' az-obj-selected' : ''}${azDim ? ' dim' : ''}${azInactive ? ' az-inactive' : ''}${inGroup ? ' in-group' : ''}`,
      'data-az-object': String(o),
      style: `--c:${row.colour}`
    });
    const azReadout = element('span', { className: 'tl-az-readout', 'data-az-readout': String(o), text: fmtAz(row.azCurrent) });
    const azAddBtn = button('+ Key', async e => {
      e.stopPropagation();
      if (!store.ready) return;
      const curRow = refs.get(o)?.current || row;
      if (curRow.azMode !== 'manual') return;
      const t = Number(clamp(curRow.position, 0, 100).toFixed(4));
      await addAzKeyAt(o, t);
    }, {
      className: 'tl-az-add',
      disabled: azInactive,
      'aria-label': `Add azimuth key at playhead for ${row.label}`,
      title: azInactive
        ? `Angle is driven by ${row.azMode === 'along' ? 'Along path' : 'Look at'}`
        : 'Add an azimuth key at the playhead using the current angle'
    });
    const azLeft = element('div', { className: 'tl-az-name' }, [
      element('span', { className: 'tl-az-caption', text: 'Azimuth', title: `${row.label} azimuth keys` }),
      azReadout,
      azAddBtn
    ]);

    const azSvg = document.createElementNS(SVG_NS, 'svg');
    azSvg.setAttribute('class', 'tl-az-curve');
    azSvg.setAttribute('viewBox', '0 0 1000 24');
    azSvg.setAttribute('preserveAspectRatio', 'none');
    azSvg.setAttribute('aria-hidden', 'true');
    const azZero = document.createElementNS(SVG_NS, 'line');
    azZero.setAttribute('class', 'tl-az-zero');
    azZero.setAttribute('x1', '0'); azZero.setAttribute('y1', '12');
    azZero.setAttribute('x2', '1000'); azZero.setAttribute('y2', '12');
    const azCurve = document.createElementNS(SVG_NS, 'path');
    azCurve.setAttribute('class', 'tl-az-path');
    azSvg.append(azZero, azCurve);

    const azPh = element('div', { className: 'tl-ph', 'data-driver': String(row.driver) });
    const azInner = element('div', { className: 'tl-inner' }, [element('div', { className: 'tl-grid' }), azSvg, azPh]);
    if (azInactive) {
      const hintText = row.azMode === 'along' ? 'Angle is driven by Along path' : 'Angle is driven by Look at';
      azInner.append(element('span', { className: 'tl-az-hint', text: hintText, title: `${hintText} · stored keys are kept` }));
    }

    const azKeyEls = new Map();
    for (const k of row.azKeys) {
      const kd = element('div', {
        className: 'tl-az-key',
        tabIndex: 0,
        role: 'button',
        'data-az-key': String(k.index),
        'data-focus-key': `tl-az-key-${o}-${k.index}`
      });
      azKeyEls.set(k.index, kd);
      azInner.append(kd);
    }
    const azTrack = element('div', {
      className: 'tl-track tl-az-track',
      tabIndex: -1,
      role: 'group',
      'aria-label': `${row.label} azimuth strip`,
      title: azInactive
        ? `Angle is driven by ${row.azMode === 'along' ? 'Along path' : 'Look at'} · stored keys are kept`
        : 'Double-click to add an azimuth key · drag key to move · right-click for easing/delete'
    }, [element('div', { className: 'tl-clip' }, [azInner])]);
    azRowEl.append(azLeft, azTrack);

    const yPct = az => ((12 - (clamp(az, -180, 180) / 180) * 9) / 24) * 100;
    const paintAz = r => {
      setLeft(azPh, r.position);
      azReadout.textContent = fmtAz(r.azCurrent);
      azCurve.setAttribute('d', azimuthCurvePath(r.azKeys, view, 1000, 24));
      for (const k of r.azKeys) {
        const kd = azKeyEls.get(k.index);
        if (!kd) continue;
        setLeft(kd, k.t);
        kd.style.top = `${yPct(k.az).toFixed(2)}%`;
        kd.dataset.ease = k.easing;
        kd.classList.toggle('sel', selAzKey?.object === o && selAzKey.index === k.index);
        kd.classList.toggle('at-ph', Math.abs(k.t - r.position) <= AZ_AT_PLAYHEAD_TOL);
        kd.title = `Azimuth key ${k.index + 1} · ${fmtAz(k.az)} @ ${fmt(k.t)} % · ${AZ_EASE_LABEL[k.easing]}`;
        kd.setAttribute('aria-label', `${r.label} azimuth key ${k.index + 1}, ${fmtAz(k.az)} at ${fmt(k.t)} percent, ${AZ_EASE_LABEL[k.easing]}`);
      }
    };
    const paintAzPosition = (pos, u) => {
      setLeft(azPh, pos);
      const p = store.state.document.paths?.[o];
      let curAz = row.azCurrent;
      if (p) {
        const prefix = o === 32 ? 'l' : `s${o + 1}`;
        if (orientationMode(p) === 'manual') {
          const evAz = p.enabled && p.azKeys?.length ? evaluateAzimuthKeys(p.azKeys, u) : null;
          curAz = evAz !== null ? evAz : (store.state.parameters?.values?.[prefix + 'az'] ?? 0);
        } else {
          const patched = {
            ...store.state,
            parameters: {
              ...store.state.parameters,
              values: { ...store.state.parameters.values, [pathId(row.driver)]: u }
            }
          };
          curAz = withPathPoses(patched).parameters.values[prefix + 'az'] ?? 0;
        }
      }
      azReadout.textContent = fmtAz(curAz);
      for (const k of refs.get(o).current.azKeys) {
        const kd = azKeyEls.get(k.index);
        if (kd) kd.classList.toggle('at-ph', Math.abs(k.t - pos) <= AZ_AT_PLAYHEAD_TOL);
      }
    };

    paintAz(row);
    const entry = refs.get(o);
    entry.paint = r => { paint(r); paintAz(r); };
    entry.paintAzPosition = paintAzPosition;
    entry.azRowEl = azRowEl;
    entry.azKeyEls = azKeyEls;

    azTrack.addEventListener('pointerdown', e => {
      if (e.target.closest('.tl-az-key')) return;
      selectObject(o);
      scrub(e, azTrack, row.driver);
    });
    azTrack.addEventListener('dblclick', async e => {
      if (e.target.closest('.tl-az-key') || !store.ready) return;
      const p = store.state.document.paths?.[o];
      if (!p || orientationMode(p) !== 'manual') return;
      const rawT = clamp(valueAt(e, azTrack), 0, 100);
      let t = snap(rawT, e.shiftKey);
      if (p.azKeys?.some(k => Math.abs(k.t - t) < 0.01)) {
        t = Number(rawT.toFixed(4));
        if (p.azKeys.some(k => Math.abs(k.t - t) < 1e-4)) return;
      }
      await addAzKeyAt(o, t);
    });

    azKeyEls.forEach((kd, keyIndex) => {
      kd.addEventListener('dblclick', e => { e.stopPropagation(); });
      kd.addEventListener('pointerdown', e => {
        e.stopPropagation(); kd.focus({ preventScroll: true });
        const p0 = store.state.document.paths[o];
        if (orientationMode(p0) !== 'manual') { selectObject(o); return; }
        let moved = false;
        drag(e, kd, {
          label: `Move ${row.label} azimuth key`,
          onClick: () => selectAzKey(o, keyIndex),
          onMove: (ev, g) => {
            moved = true;
            const t = newAzKeyTime(p0, keyIndex, valueAt(ev, kd), ev.shiftKey);
            const azKeys = (p0.azKeys || []).map((ak, i) => i === keyIndex ? { ...ak, t } : ak);
            const nextRow = withPatch(o, { azKeys });
            refs.get(o).current = nextRow;
            refs.get(o).paint(nextRow);
            g.send([{ op: 'path.setAzimuthKey', object: o, index: keyIndex, t }]);
            showTip(kd, t, `${fmt(t)} % · ${fmtAz(p0.azKeys[keyIndex].az)}`);
          },
          onEnd: cancelled => {
            if (moved && !cancelled) selectAzKey(o, keyIndex);
          }
        });
      });
      kd.addEventListener('contextmenu', e => {
        const curKey = store.state.document.paths?.[o]?.azKeys?.[keyIndex];
        if (curKey) openAzMenu(e, o, keyIndex, curKey.easing || 'linear');
      });
      kd.addEventListener('keydown', e => {
        const p = store.state.document.paths?.[o];
        if (!p || orientationMode(p) !== 'manual') return;
        if (e.key === 'Delete' || e.key === 'Backspace') {
          e.preventDefault(); e.stopPropagation();
          removeAzKey(o, keyIndex);
          return;
        }
        if (e.key === 'Enter' || e.key === ' ') {
          e.preventDefault(); e.stopPropagation();
          selectAzKey(o, keyIndex);
          return;
        }
        const dir = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0;
        if (!dir) return;
        e.preventDefault(); e.stopPropagation();
        const curKey = p.azKeys?.[keyIndex];
        if (!curKey) return;
        const step = e.shiftKey ? 0.05 : 0.5;
        const t = clampAzKeyTime(p, keyIndex, curKey.t + dir * step);
        if (Math.abs(t - curKey.t) > 1e-9) {
          pendingFocusAzKey = { o, index: keyIndex };
          store.command({ op: 'path.setAzimuthKey', object: o, index: keyIndex, t });
        }
      });
    });

    return [rowEl, azRowEl];
  }

  // ---- group header ---------------------------------------------------------------------------------------------------------------
  const paintSections = () => { for (const s of sectionRefs) s.paint(); };
  function buildGroup(sec) {
    const isCollapsed = !!collapsed[sec.group];
    const toggle = button(isCollapsed ? '▸' : '▾', () => store.setView({ timelineCollapsed: { ...collapsed, [sec.group]: !isCollapsed } }, false), { className: 'tl-chev', 'aria-expanded': String(!isCollapsed), 'aria-label': `${isCollapsed ? 'Expand' : 'Collapse'} ${sec.label}` });
    const refFor = () => { const sel = sec.rows.find(r => selectedObjects.has(r.object)); return sel ? sel.object : sec.driver; };
    const rowsNow = () => sec.rows.map(r => refs.get(r.object)?.current || r);
    const align = kind => {
      const cmds = alignCommands(rowsNow(), kind, refFor()); if (!cmds.length) return;
      const g = gesture(kind === 'starts' ? 'Align starts' : 'Align ends'); g.send(cmds); g.end();
    };
    const head = element('div', { className: 'tl-row tl-head' }, [
      element('div', { className: 'tl-name tl-group-name' }, [toggle, element('b', { text: sec.label })]),
      element('div', { className: 'tl-head-main' }, [
        element('span', { className: 'tl-note', text: `driven by ${sec.driverLabel} Position` }),
        element('span', { className: 'tl-spacer' }),
        button('Align starts', () => align('starts'), { className: 'tl-align', title: 'Every member takes the Start of the selected member (or the driver)' }),
        button('Align ends', () => align('ends'), { className: 'tl-align', title: 'Every member takes the End of the selected member (or the driver)' })
      ])
    ]);
    const bar = element('div', { className: 'tl-bar tl-group-bar', 'aria-label': `${sec.label} window (drag to shift all members)`, title: 'Drag to shift Start/End of all members together · Shift = fine' });
    const ph = element('div', { className: 'tl-ph', 'data-driver': String(sec.driver) });
    const inner = element('div', { className: 'tl-inner' }, [element('div', { className: 'tl-grid' }), bar, ph]);
    const gtrack = element('div', { className: 'tl-row tl-gbar' }, [element('div', { className: 'tl-name tl-spacer-cell' }), element('div', { className: 'tl-track' }, [element('div', { className: 'tl-clip' }, [inner])])]);
    const paint = () => {
      const rs = rowsNow(); setBar(bar, Math.min(...rs.map(r => r.start)), Math.max(...rs.map(r => r.end))); setLeft(ph, sec.position);
    };
    sectionRefs.push({ paint }); paint();
    gtrack.querySelector('.tl-track').addEventListener('pointerdown', e => {
      if (e.target.closest('.tl-bar')) return; scrub(e, gtrack.querySelector('.tl-track'), sec.driver);
    });
    bar.addEventListener('pointerdown', e => {
      const bases = rowsNow().map(r => ({ object: r.object, start: r.start, end: r.end })), origin = valueAt(e, bar);
      drag(e, bar, { label: `Shift ${sec.label}`, onMove: (ev, g) => {
        const w = shiftWindows(bases, valueAt(ev, bar) - origin, ev.shiftKey);
        w.forEach((win, i) => { const o = bases[i].object; refs.get(o).current = withPatch(o, win); refs.get(o).paint(refs.get(o).current); });
        paintSections();
        g.send(w.map((win, i) => ({ op: 'path.setFlags', object: bases[i].object, start: win.start, end: win.end })));
        showTip(bar, w[0].start, `shift ${fmt(w[0].start - bases[0].start)} %`);
      } });
    });
    return [head, gtrack];
  }

  // ---- assemble -----------------------------------------------------------------------------------------------------------------
  const ticks = rulerTicks(view);
  const ruler = element('div', { className: 'tl-row tl-ruler' }, [
    element('div', { className: 'tl-name tl-ruler-label', text: 'Position %' }),
    element('div', { className: 'tl-track' }, [element('div', { className: 'tl-clip' }, [element('div', { className: 'tl-inner' },
      ticks.map(v => element('span', { className: 'tl-tick', style: `left:${pct(viewFrac(view, v))}`, text: String(v) })))])])
  ]);
  ruler.querySelector('.tl-track').addEventListener('pointerdown', e => {
    const o = [...selectedObjects].find(x => refs.has(x)) ?? [...refs.keys()][0]; if (o === undefined) return;
    scrub(e, ruler.querySelector('.tl-track'), refs.get(o).row.driver);
  });
  const bodyScroll = scrollArea([], { className: 'tl-body' });
  const body = bodyScroll.view;
  if (!sections.length) body.append(element('p', { className: 'note tl-empty', text: 'No moving objects yet. Draw a path for a source or the listener, then it appears here.' }));
  for (const sec of sections) {
    if (sec.type === 'group') {
      const [head, gbar] = buildGroup(sec); body.append(head);
      if (!collapsed[sec.group]) { body.append(gbar); sec.rows.forEach(r => body.append(...buildRow(r, true))); }
      else { body.append(gbar); }
    } else {
      body.append(element('div', { className: 'tl-row tl-head' }, [element('div', { className: 'tl-name tl-group-name' }, [element('b', { text: sec.label })]), element('div', { className: 'tl-head-main' }, [element('span', { className: 'tl-note', text: 'each path follows its own Position' })])]));
      sec.rows.forEach(r => body.append(...buildRow(r, false)));
    }
  }
  // ---- selected azimuth key angle editor ----------------------------------------------------------------------------------------
  function buildAzEditor() {
    if (!selAzKey) return null;
    const o = selAzKey.object, idx = selAzKey.index;
    const r = refs.get(o)?.row || pathRow(state, o);
    const k = r?.azKeys?.[idx];
    if (!r || !k) return null;
    const disabled = r.azMode !== 'manual';
    const editor = element('div', { className: `tl-az-editor${disabled ? ' dim' : ''}`, 'data-az-editor': `${o}:${idx}` });
    editor.append(element('span', {
      className: 'tl-az-editor-info',
      text: `${r.label} · Key ${idx + 1} @ ${fmt(k.t)} %`,
      title: `${r.label} · Azimuth key ${idx + 1} at ${fmt(k.t)} %`
    }));
    if (disabled) {
      editor.append(element('span', {
        className: 'tl-az-editor-dim',
        text: r.azMode === 'along' ? 'Angle is driven by Along path' : 'Angle is driven by Look at'
      }));
      return editor;
    }
    const currentAz = () => store.state.document.paths?.[o]?.azKeys?.[idx]?.az ?? k.az;
    const snapAz = (v, fine) => {
      const step = fine ? 0.1 : 1;
      const s = clamp(Math.round(v / step) * step, -180, 180);
      const n = Number(s.toFixed(1));
      return n === 0 ? 0 : n;
    };
    const sliderEl = element('div', {
      className: 'track tl-az-slider',
      role: 'slider',
      tabIndex: 0,
      'aria-label': 'Selected azimuth key angle',
      'aria-valuemin': -180,
      'aria-valuemax': 180,
      'data-focus-key': 'tl-az-slider',
      title: 'Drag to set angle (−180…180°) · Shift = fine 0.1°'
    }, [element('i')]);
    const fieldEl = element('input', {
      type: 'text',
      className: 'readout tl-az-angle-input',
      'aria-label': 'Selected azimuth key angle value',
      'data-focus-key': 'tl-az-angle',
      inputMode: 'decimal'
    });
    const paintEditor = az => {
      const norm = clamp((az + 180) / 360, 0, 1);
      sliderEl.style.setProperty('--normal', String(norm));
      sliderEl.setAttribute('aria-valuenow', String(az));
      sliderEl.setAttribute('aria-valuetext', fmtAz(az));
      if (document.activeElement !== fieldEl || fieldEl.readOnly) fieldEl.value = fmtAz(az);
      const entry = refs.get(o);
      if (entry) {
        const p0 = store.state.document.paths?.[o];
        if (p0?.azKeys?.[idx]) {
          const azKeys = p0.azKeys.map((ak, i) => i === idx ? { ...ak, az } : ak);
          const nextRow = withPatch(o, { azKeys });
          entry.current = nextRow;
          entry.paint(nextRow);
        }
      }
    };
    paintEditor(k.az);

    sliderEl.addEventListener('pointerdown', e => {
      if (e.button !== 0 || !store.ready) return;
      e.preventDefault(); sliderEl.focus({ preventScroll: true });
      if (!tryCapture(sliderEl, e)) return;
      root.setAttribute('data-gesture-active', 'true'); pointerInside = true;
      const g = gesture(`Edit ${r.label} key angle`);
      const rect = sliderEl.getBoundingClientRect();
      const startX = e.clientX;
      const startNorm = e.shiftKey ? clamp((currentAz() + 180) / 360, 0, 1) : clamp((e.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
      let active = true;
      const applyAngle = (ev, fine) => {
        const delta = (ev.clientX - startX) / Math.max(1, rect.width);
        const norm = clamp(startNorm + (ev === e ? 0 : delta * (fine ? 0.1 : 1)), 0, 1);
        const az = snapAz(-180 + norm * 360, fine);
        paintEditor(az);
        g.send([{ op: 'path.setAzimuthKey', object: o, index: idx, az }]);
      };
      if (!e.shiftKey) applyAngle(e, false);
      const move = ev => { if (active && ev.pointerId === e.pointerId) applyAngle(ev, ev.shiftKey); };
      const finish = async cancelled => {
        if (!active) return; active = false;
        sliderEl.removeEventListener('pointermove', move);
        sliderEl.removeEventListener('pointerup', up);
        sliderEl.removeEventListener('pointercancel', cancel);
        sliderEl.removeEventListener('lostpointercapture', cancel);
        window.removeEventListener('verb-gestures-reset', cancel);
        window.removeEventListener('verb-ui-error', cancel);
        if (cancelled) g.cancel(); else await g.end();
        root.removeAttribute('data-gesture-active');
        window.dispatchEvent(new Event('verb-control-end'));
      };
      const up = () => finish(false), cancel = () => finish(true);
      sliderEl.addEventListener('pointermove', move);
      sliderEl.addEventListener('pointerup', up);
      sliderEl.addEventListener('pointercancel', cancel);
      sliderEl.addEventListener('lostpointercapture', cancel);
      window.addEventListener('verb-gestures-reset', cancel);
      window.addEventListener('verb-ui-error', cancel);
    });
    sliderEl.addEventListener('keydown', e => {
      if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(e.key)) return;
      e.preventDefault(); e.stopPropagation();
      const dir = ['ArrowRight', 'ArrowUp'].includes(e.key) ? 1 : -1;
      const step = e.shiftKey ? 0.1 : 1;
      const next = e.key === 'Home' ? -180 : e.key === 'End' ? 180 : snapAz(currentAz() + dir * step, e.shiftKey);
      if (Math.abs(next - currentAz()) > 1e-6) {
        pendingFocusAzSlider = true;
        store.command({ op: 'path.setAzimuthKey', object: o, index: idx, az: next });
      }
    });

    let textCommitted = false, fieldG = null, fieldCommitted = false;
    const submitField = () => {
      if (textCommitted) return;
      const raw = fieldEl.value.trim().replace(/°$/, '').trim();
      const n = Number(raw);
      if (!raw || !Number.isFinite(n) || n < -180 || n > 180) {
        fieldEl.setCustomValidity('Enter −180…180');
        fieldEl.reportValidity();
        return;
      }
      textCommitted = true;
      fieldEl.setCustomValidity('');
      const az = Number(n.toFixed(3)) === 0 ? 0 : Number(n.toFixed(3));
      paintEditor(az);
      if (Math.abs(az - currentAz()) > 1e-6) {
        store.command({ op: 'path.setAzimuthKey', object: o, index: idx, az });
      }
    };
    fieldEl.addEventListener('input', () => { textCommitted = false; });
    fieldEl.addEventListener('change', submitField);
    fieldEl.addEventListener('keydown', e => {
      if (e.key === 'Enter') { e.preventDefault(); submitField(); fieldEl.blur(); }
      if (e.key === 'Escape') { e.preventDefault(); textCommitted = true; fieldEl.setCustomValidity(''); fieldEl.value = fmtAz(currentAz()); fieldEl.blur(); }
    });
    const offField = valueField(fieldEl, {
      get: currentAz,
      min: -180,
      max: 180,
      step: 0.1,
      display: fmtAz,
      edit: v => String(Number(v.toFixed(2))),
      start: () => {
        fieldCommitted = false;
        root.setAttribute('data-gesture-active', 'true');
        fieldG = gesture(`Edit ${r.label} key angle`);
      },
      preview: v => {
        const az = Number(clamp(v, -180, 180).toFixed(2));
        const clean = az === 0 ? 0 : az;
        paintEditor(clean);
        fieldG?.send([{ op: 'path.setAzimuthKey', object: o, index: idx, az: clean }]);
      },
      commit: v => {
        const az = Number(clamp(v, -180, 180).toFixed(2));
        const clean = az === 0 ? 0 : az;
        if (fieldG) {
          fieldCommitted = true;
          fieldG.send([{ op: 'path.setAzimuthKey', object: o, index: idx, az: clean }]);
        } else if (Math.abs(clean - currentAz()) > 1e-6) {
          store.command({ op: 'path.setAzimuthKey', object: o, index: idx, az: clean });
        }
      },
      end: async () => {
        const fg = fieldG; fieldG = null;
        if (fg) { if (fieldCommitted) await fg.end(); else fg.cancel(); }
        root.removeAttribute('data-gesture-active');
        window.dispatchEvent(new Event('verb-control-end'));
      }
    });
    const delBtn = button('Delete', () => removeAzKey(o, idx), {
      className: 'tl-az-del',
      'aria-label': 'Delete selected azimuth key',
      title: 'Delete selected azimuth key (Del)'
    });
    editor.append(element('span', { className: 'tl-az-editor-lbl', text: 'Angle' }), sliderEl, fieldEl, delBtn);
    editor.dispose = offField;
    return editor;
  }
  const azEditor = buildAzEditor();
  // ruler is built before the rows exist in `refs`, so its scrub looks them up lazily (done above).
  const header = element('div', { className: 'tl-header' }, [
    element('b', { text: 'Path timeline' }),
    element('span', { className: 'tl-note', text: 'Position 0-100 %' }),
    element('span', { className: 'tl-spacer' }),
    ...(zoomed ? [button('Fit', () => store.setView({ timelineView: null }, false), { title: 'Show Position 0-100 %', 'aria-label': 'Fit timeline' })] : []),
    button(store.view.timelineMin ? '+' : '−', () => store.setView({ timelineMin: !store.view.timelineMin }, false), { className: 'tl-min', 'aria-label': 'Collapse timeline', 'aria-expanded': String(!store.view.timelineMin), title: 'Collapse / expand' }),
    button('×', () => store.setView({ timelineOpen: false }, false), { className: 'tl-close', 'aria-label': 'Close timeline', title: 'Close (Esc)' })
  ]);
  const bounds = () => root.parentElement?.getBoundingClientRect() || { left: 0, top: 0, right: window.innerWidth, bottom: window.innerHeight };
  const grip = element('div', { className: 'tl-grip', 'aria-label': 'Resize timeline', title: 'Drag to resize' });
  root.append(header, ruler, bodyScroll, ...(azEditor ? [azEditor] : []), grip);
  root.classList.toggle('collapsed', !!store.view.timelineMin);
  // move by the header, resize by the corner grip; double-click the header to reset. Offsets are kept in the view state.
  const box = { dx: 0, dy: 0, w: 0, h: 0, ...(store.view.timelineBox || {}) };
  const applyBox = b => {
    root.style.transform = b.dx || b.dy ? `translate(${b.dx}px, ${b.dy}px)` : '';
    if (b.w) { root.style.right = 'auto'; root.style.width = `${b.w}px`; } else { root.style.right = ''; root.style.width = ''; }
    if (b.h) { root.style.maxHeight = 'none'; root.style.height = `${b.h}px`; } else { root.style.maxHeight = ''; root.style.height = ''; }
  };
  applyBox(box);
  const panelDrag = (e, el, onMove) => {
    if (e.button !== 0) return;
    e.preventDefault(); if (!tryCapture(el, e)) return; root.setAttribute('data-gesture-active', 'true'); pointerInside = true;
    const x0 = e.clientX, y0 = e.clientY, r0 = root.getBoundingClientRect(), b0 = { ...box };
    const move = ev => { if (ev.pointerId === e.pointerId) onMove(ev.clientX - x0, ev.clientY - y0, r0, b0); };
    const end = () => {
      el.removeEventListener('pointermove', move); el.removeEventListener('pointerup', end); el.removeEventListener('pointercancel', end); el.removeEventListener('lostpointercapture', end);
      store.setView({ timelineBox: { ...box } }, false);
      root.removeAttribute('data-gesture-active'); window.dispatchEvent(new Event('verb-control-end'));
    };
    el.addEventListener('pointermove', move); el.addEventListener('pointerup', end); el.addEventListener('pointercancel', end); el.addEventListener('lostpointercapture', end);
  };
  header.addEventListener('pointerdown', e => {
    if (e.target.closest('button')) return;
    panelDrag(e, header, (ddx, ddy, r0, b0) => {
      const P = bounds(), cx = clamp(ddx, P.left - r0.left, P.right - r0.right), cy = clamp(ddy, P.top - r0.top, P.bottom - r0.bottom);
      box.dx = Math.round(b0.dx + cx); box.dy = Math.round(b0.dy + cy); applyBox(box);
    });
  });
  header.addEventListener('dblclick', e => { if (!e.target.closest('button')) store.setView({ timelineBox: null }, false); });
  grip.addEventListener('pointerdown', e => panelDrag(e, grip, (ddx, ddy, r0, b0) => {
    const P = bounds(); box.w = Math.round(clamp(r0.width + ddx, 380, P.right - r0.left)); box.h = Math.round(clamp(r0.height - ddy, 110, r0.bottom - P.top));
    box.dx = b0.dx; box.dy = b0.dy; applyBox(box);
  }));
  // Keep the whole panel inside the viewport (preview) area, also after the window is resized (same rule as the Energy panel).
  const fit = () => {
    if (!root.isConnected) return;
    const P = bounds(); let r = root.getBoundingClientRect();
    if (r.width > P.right - P.left - 2 && !box.w) return;
    if (r.width > P.right - P.left - 2) { box.w = Math.max(380, Math.floor(P.right - P.left - 16)); applyBox(box); r = root.getBoundingClientRect(); }
    if (box.h && r.height > P.bottom - P.top - 2) { box.h = Math.max(110, Math.floor(P.bottom - P.top - 16)); applyBox(box); r = root.getBoundingClientRect(); }
    const dx = r.left < P.left ? P.left - r.left : r.right > P.right ? P.right - r.right : 0;
    const dy = r.top < P.top ? P.top - r.top : r.bottom > P.bottom ? P.bottom - r.bottom : 0;
    if (dx || dy) { box.dx += Math.round(dx); box.dy += Math.round(dy); applyBox(box); }
    store.view.timelineBox = box.dx || box.dy || box.w || box.h ? { ...box } : null;
  };
  let resizeObs = null;
  const focusTarget = store.view.timelineFocusAz;
  if (focusTarget != null) store.view.timelineFocusAz = null;
  requestAnimationFrame(() => {
    if (!root.isConnected) return;
    fit();
    bodyScroll.update?.();
    if (typeof ResizeObserver !== 'undefined' && root.parentElement) { resizeObs = new ResizeObserver(() => { fit(); bodyScroll.update?.(); }); resizeObs.observe(root.parentElement); }
    if (pendingFocusAzKey) {
      const target = refs.get(pendingFocusAzKey.o)?.azKeyEls?.get(pendingFocusAzKey.index);
      pendingFocusAzKey = null;
      target?.focus({ preventScroll: true });
    } else if (pendingFocusAzSlider) {
      pendingFocusAzSlider = false;
      azEditor?.querySelector('.tl-az-slider')?.focus({ preventScroll: true });
    } else if (focusTarget != null) {
      const entry = refs.get(focusTarget);
      if (entry?.azRowEl) {
        entry.azRowEl.scrollIntoView({ block: 'nearest' });
        const target = entry.azRowEl.querySelector('.tl-az-key.sel')
          || entry.azRowEl.querySelector('.tl-az-key')
          || entry.azRowEl.querySelector('.tl-az-add:not(:disabled)')
          || entry.azRowEl.querySelector('.tl-az-track');
        target?.focus({ preventScroll: true });
      }
    }
  });
  const offParams = store.subscribe(({ reason }) => {
    if (reason !== 'parameters' || !root.isConnected || !store.state) return;
    const seen = new Set();
    for (const [, entry] of refs) {
      const d = entry.row.driver;
      if (seen.has(d)) continue;
      seen.add(d);
      const u = clamp(Number(store.state.parameters?.values?.[pathId(d)]) || 0, 0, 1);
      paintDriverPosition(d, u);
    }
  });
  // wheel = zoom around the cursor (Shift = pan); never below a 5 % view
  root.addEventListener('wheel', e => {
    const track = e.target.closest?.('.tl-track'); if (!track) return;
    e.preventDefault();
    const r = track.querySelector('.tl-inner').getBoundingClientRect(), frac = clamp((e.clientX - r.left) / Math.max(1, r.width), 0, 1);
    const next = e.shiftKey ? panView(view, Math.sign(e.deltaY || e.deltaX) * (view.hi - view.lo) * 0.1) : zoomView(view, frac, e.deltaY < 0 ? 0.8 : 1.25);
    store.setView({ timelineView: next.lo <= 0 && next.hi >= 100 ? null : next }, false);
  }, { passive: false });
  root.addEventListener('pointerdown', () => { pointerInside = true; }, true);
  root.dispose = () => { offParams(); bodyScroll.dispose?.(); azEditor?.dispose?.(); resizeObs?.disconnect(); closeMenu(); if (live?.root === root) live = null; };
  live = { root, store };
  return root;
}

// Delete on a key the user just picked in the timeline clears its explicit time or removes the selected azimuth key (never deletes the geometry point).
if (typeof window !== 'undefined' && !window.__verbTimelineKeys) {
  window.__verbTimelineKeys = true;
  window.addEventListener('pointerdown', e => { if (live && !live.root.contains(e.target)) pointerInside = false; }, true);
  window.addEventListener('keydown', e => {
    if (!live || (!pointerInside && !live.root.contains(document.activeElement)) || !['Delete', 'Backspace'].includes(e.key) || /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName)) return;
    const { store } = live;
    const azKey = store.view.azKey;
    if (azKey) {
      const p = store.state.document.paths?.[azKey.object];
      if (p && orientationMode(p) === 'manual' && p.azKeys?.[azKey.index]) {
        e.preventDefault(); e.stopImmediatePropagation();
        store.setView({ azKey: null }, false);
        store.command({ op: 'path.removeAzimuthKey', object: azKey.object, index: azKey.index });
      }
      return;
    }
    const pt = store.view.pathPoint, p = pt && store.state.document.paths?.[pt.object];
    if (!p || pt.index <= 0) return;
    e.preventDefault(); e.stopImmediatePropagation();
    if (p.times?.[pt.index] !== null && p.times?.[pt.index] !== undefined) store.command({ op: 'path.setPointTiming', object: pt.object, index: pt.index, time: null });
  }, true);
}
