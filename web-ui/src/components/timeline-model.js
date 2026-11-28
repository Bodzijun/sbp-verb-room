// E20: pure model + drag math for the path timeline panel (no DOM; unit-tested in tests/e20.test.mjs).
// Axis = group Position 0..100 % (the engine's u). A key's "Time %" is WINDOW-relative: axis x = start + time * (end - start) / 100.
import {
  compilePath, evaluateTimeMap, pathGroupMembers, pathId, pointTimeBounds, clampPointTime,
  MIN_WINDOW_GAP, PATH_EASINGS, remainder360, applyEasing, evaluateAzimuthKeys,
  hasActiveAzimuthKeys, orientationMode, withPathPoses
} from '../viewport/movement-path.js';
export { MIN_WINDOW_GAP, PATH_EASINGS };
export const AZ_EASINGS = ['linear', 'easeIn', 'easeOut', 'easeInOut', 'hold'];
export const AZ_AT_PLAYHEAD_TOL = 0.5;
export const SNAP_STEP = 0.5, FINE_STEP = 0.05, MIN_VIEW = 5;
export const objectLabel = (state, o) => o === 32 ? 'Listener' : (state.document.sources[o]?.name || `Source ${o + 1}`);
export const objectColour = (o, state) => {
  if (o === 32) return 'var(--timeline-listener, #8ea6c6)';
  const c = state?.document?.sources?.[o]?.color;
  return /^#[0-9a-f]{6}$/i.test(c || '') ? c.toLowerCase() : `var(--strip-${o % 6}, #d9a05b)`;
};
export const objectRef = o => o === 32 ? { kind: 'listener', index: 0 } : { kind: 'source', index: o };
export const isMovingPath = p => (p?.points?.length ?? 0) >= 2;
const num = (v, d) => Number.isFinite(v) ? v : d;
const r4 = v => Number(v.toFixed(4));
export const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
// window-relative time <-> axis position
export const timeToAxis = (start, end, t) => start + t * (end - start) / 100;
export const axisToTime = (start, end, v) => end - start > 0 ? (v - start) / (end - start) * 100 : 0;

// Invert the time map: axis positions where a closed path completes lap 1..laps-1 (the map applies BEFORE laps wrap).
export function lapBoundaries(p) {
  if (p?.endless) return [];
  const laps = Math.max(1, Math.min(64, Math.round(p?.laps ?? 1)));
  if (!p?.closed || laps < 2 || !isMovingPath(p)) return [];
  const out = [];
  for (let k = 1; k < laps; k++) {
    let lo = 0, hi = 1;
    for (let i = 0; i < 40; i++) { const mid = (lo + hi) / 2; if (evaluateTimeMap(p, mid) < k / laps) lo = mid; else hi = mid; }
    out.push(r4(hi * 100));
  }
  return out;
}

// One row per moving path. keys: first point (pinned, hosts the Start handle) + every explicit Time %; autos: computed dots.
export function pathRow(state, o) {
  const p = state.document.paths?.[o];
  const start = num(p.start, 0), end = num(p.end, 100), n = p.points.length;
  const compiled = compilePath(p);
  const keys = [], autos = [], at = [];
  for (let k = 0; k < n; k++) {
    const last = !p.closed && k === n - 1;
    if (last) { at[k] = 100; continue; }                  // open end: the End handle
    const raw = p.times?.[k];
    const explicit = k > 0 && raw !== null && raw !== undefined && Number.isFinite(raw);
    const time = k === 0 ? 0 : explicit ? raw : compiled.computedTimes[k];
    at[k] = time;
    const item = { index: k, time: r4(time), x: r4(timeToAxis(start, end, time)), easing: p.easings?.[k] || 'linear', pinned: k === 0 };
    if (k === 0 || explicit) keys.push(item); else autos.push(item);
  }
  const holds = [];
  for (let k = 0; k + 1 < n; k++) {
    const a = p.points[k], b = p.points[k + 1];
    if (Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]) < 1e-9 && at[k + 1] - at[k] > 1e-9) holds.push({ index: k, from: r4(timeToAxis(start, end, at[k])), to: r4(timeToAxis(start, end, at[k + 1])) });
  }
  const driver = pathGroupMembers(state.document.paths, p.group ?? -1).length >= 2 ? driverOf(state.document.paths, o) : o;
  const position = clamp(num(state.parameters?.values?.[pathId(driver)], 0), 0, 1) * 100;
  const prefix = o === 32 ? 'l' : `s${o + 1}`;
  const hostAz = num(state.parameters?.values?.[prefix + 'az'], 0);
  const azMode = orientationMode(p);
  const azActive = hasActiveAzimuthKeys(p);
  const azKeys = (Array.isArray(p.azKeys) ? p.azKeys : []).map((k, index) => ({
    index,
    t: r4(k.t),
    az: r4(k.az),
    easing: k.easing || 'linear',
    atPlayhead: Math.abs(k.t - position) <= AZ_AT_PLAYHEAD_TOL
  }));
  let azCurrent = hostAz;
  if (state.document?.paths && state.parameters?.values) {
    azCurrent = num(withPathPoses(state).parameters?.values?.[prefix + 'az'], hostAz);
  } else if (azActive && azKeys.length) {
    const evaluated = evaluateAzimuthKeys(p.azKeys, position / 100);
    if (evaluated !== null) azCurrent = evaluated;
  }
  return {
    object: o, label: objectLabel(state, o), colour: objectColour(o, state), enabled: !!p.enabled, closed: !!p.closed,
    endless: !!(p.closed && p.endless), endlessPeriodS: Number.isFinite(p.endlessPeriodS) ? p.endlessPeriodS : 8,
    laps: p.closed ? Math.max(1, Math.round(p.laps ?? 1)) : 1,
    start, end, group: Number.isInteger(p.group) ? p.group : -1, driver, position,
    keys, autos, holds, lapMarks: lapBoundaries(p), points: n,
    azMode, azActive, azKeys, azCurrent: r4(azCurrent)
  };
}
function driverOf(paths, o) { const g = paths[o]?.group ?? -1; return pathGroupMembers(paths, g)[0] ?? o; }

// Sections: one per coupling group with >= 2 enabled members (header row + members), then every other moving path (own Position each).
export function timelineSections(state) {
  const paths = state.document.paths || [], sources = state.document.sources || [];
  const exists = o => o === 32 || o < sources.length;
  const sections = [], seen = new Set();
  const groups = new Set();
  paths.forEach((p, o) => { if (exists(o) && p?.enabled && isMovingPath(p) && (p.group ?? -1) >= 0) groups.add(p.group); });
  for (const g of [...groups].sort((a, b) => a - b)) {
    const members = pathGroupMembers(paths, g).filter(exists);
    if (members.length < 2) continue;
    const rows = members.map(o => pathRow(state, o));
    members.forEach(o => seen.add(o));
    sections.push({ type: 'group', group: g, label: `Group ${g + 1}`, driver: members[0], driverLabel: objectLabel(state, members[0]), rows,
      start: Math.min(...rows.map(r => r.start)), end: Math.max(...rows.map(r => r.end)), position: rows[0].position });
  }
  const free = [];
  paths.forEach((p, o) => { if (exists(o) && isMovingPath(p) && !seen.has(o)) free.push(pathRow(state, o)); });
  if (free.length) sections.push({ type: 'free', label: 'Uncoupled', rows: free });
  return sections;
}

// ---- drag math -------------------------------------------------------------------------------------------------------------------
export const stepFor = fine => fine ? FINE_STEP : SNAP_STEP;
export const snap = (v, fine) => r4(Math.round(v / stepFor(fine)) * stepFor(fine));
export const newStart = (row, v, fine) => r4(clamp(snap(clamp(v, 0, 100), fine), 0, row.end - MIN_WINDOW_GAP));
export const newEnd = (row, v, fine) => r4(clamp(snap(clamp(v, 0, 100), fine), row.start + MIN_WINDOW_GAP, 100));
// Key time from a pointer position: window-relative, snapped, then clamped between the neighbouring explicit times (as the inspector does).
export function newKeyTime(p, index, v, fine) {
  const start = num(p.start, 0), end = num(p.end, 100);
  return r4(clampPointTime(p, index, snap(clamp(axisToTime(start, end, v), 0, 100), fine)));
}
// Shift windows together: delta snapped, limited so that no member leaves 0..100. rows: [{start,end}]
export function shiftWindows(rows, delta, fine) {
  const lo = -Math.min(...rows.map(r => r.start)), hi = 100 - Math.max(...rows.map(r => r.end));
  const d = r4(clamp(snap(delta, fine), lo, hi));
  return rows.map(r => ({ start: r4(r.start + d), end: r4(r.end + d) }));
}
// Align: every member takes the reference's Start (or End), limited by its own window gap. Returns only members that change.
export function alignCommands(rows, kind, referenceObject) {
  const ref = rows.find(r => r.object === referenceObject) || rows[0];
  const out = [];
  for (const r of rows) {
    if (kind === 'starts') { const start = r4(Math.min(ref.start, r.end - MIN_WINDOW_GAP)); if (Math.abs(start - r.start) > 1e-9) out.push({ op: 'path.setFlags', object: r.object, start }); }
    else { const end = r4(Math.max(ref.end, r.start + MIN_WINDOW_GAP)); if (Math.abs(end - r.end) > 1e-9) out.push({ op: 'path.setFlags', object: r.object, end }); }
  }
  return out;
}

// ---- view (zoom) -----------------------------------------------------------------------------------------------------------------
export const FULL_VIEW = { lo: 0, hi: 100 };
export const viewFrac = (view, v) => (v - view.lo) / (view.hi - view.lo);
export const viewValue = (view, frac) => view.lo + frac * (view.hi - view.lo);
// Wheel zoom around the cursor (frac 0..1 of the track): factor < 1 zooms in. Span never below MIN_VIEW, never beyond 0..100.
export function zoomView(view, frac, factor) {
  const span = clamp((view.hi - view.lo) * factor, MIN_VIEW, 100), centre = viewValue(view, frac);
  let lo = centre - frac * span;
  lo = clamp(lo, 0, 100 - span);
  return { lo: r4(lo), hi: r4(lo + span) };
}
export function panView(view, deltaValue) {
  const span = view.hi - view.lo, lo = clamp(view.lo + deltaValue, 0, 100 - span);
  return { lo: r4(lo), hi: r4(lo + span) };
}
export function rulerTicks(view) {
  const span = view.hi - view.lo, steps = [0.5, 1, 2, 5, 10, 20, 25];
  const step = steps.find(s => span / s <= 8) ?? 25;
  const ticks = [];
  for (let v = Math.ceil(view.lo / step - 1e-9) * step; v <= view.hi + 1e-9; v += step) ticks.push(r4(v));
  return ticks;
}

// "Hold here": duplicate the key's point after it and give the copy a later time, so the object rests there between the two times.
// Returns the commands (one transaction) or null when there is no room before the next explicit time.
export function holdCommands(p, index, object) {
  const n = p.points.length;
  const t0 = index === 0 ? 0 : p.times?.[index];
  if (!Number.isFinite(t0) || (!p.closed && index >= n - 1)) return null;
  let hi = 100;
  for (let k = index + 1; k < n; k++) { if (!p.closed && k === n - 1) break; const t = p.times?.[k]; if (t !== null && t !== undefined && Number.isFinite(t)) { hi = t; break; } }
  if (hi - t0 < 0.1) return null;
  const len = Math.min(hi - t0, Math.max(SNAP_STEP, snap((hi - t0) * 0.25, false)));
  const time = r4(t0 + len), ease = p.easings?.[index] || 'linear';
  const cmds = [{ op: 'path.addPoint', object, index: index + 1, point: p.points[index].slice() }, { op: 'path.setPointTiming', object, index: index + 1, time, ...(ease !== 'linear' ? { easing: ease } : {}) }];
  if (ease !== 'linear') cmds.push({ op: 'path.setPointTiming', object, index, easing: 'linear' });   // the easing now belongs to the move that starts after the hold
  return cmds;
}

// ---- azimuth keys math & curve geometry ------------------------------------------------------------------------------------------
export function azKeyTimeBounds(p, index) {
  const keys = p?.azKeys || [];
  const prev = index > 0 && Number.isFinite(keys[index - 1]?.t) ? keys[index - 1].t : null;
  const next = index + 1 < keys.length && Number.isFinite(keys[index + 1]?.t) ? keys[index + 1].t : null;
  const loGap = prev !== null ? Math.min(FINE_STEP, Math.max(1e-4, ((next ?? 100) - prev) * 0.25)) : 0;
  const hiGap = next !== null ? Math.min(FINE_STEP, Math.max(1e-4, (next - (prev ?? 0)) * 0.25)) : 0;
  const min = prev !== null ? Math.min(100, prev + loGap) : 0;
  const max = next !== null ? Math.max(0, next - hiGap) : 100;
  const r5 = v => Number(v.toFixed(5));
  return { min: r5(min), max: r5(Math.max(min, max)) };
}

export function clampAzKeyTime(p, index, raw) {
  const { min, max } = azKeyTimeBounds(p, index);
  return Number(clamp(raw, min, max).toFixed(5));
}

export function newAzKeyTime(p, index, v, fine) {
  return clampAzKeyTime(p, index, snap(clamp(v, 0, 100), fine));
}

export function azAngleAt(state, o, tPct) {
  const p = state?.document?.paths?.[o];
  if (Array.isArray(p?.azKeys) && p.azKeys.length > 0) {
    const v = evaluateAzimuthKeys(p.azKeys, clamp(tPct, 0, 100) / 100);
    if (v !== null) return v === 0 ? 0 : v;
  }
  const id = o === 32 ? 'laz' : `s${o + 1}az`;
  const v = num(state?.parameters?.values?.[id], 0);
  return v === 0 ? 0 : v;
}

export function azimuthCurvePath(keys, view = FULL_VIEW, width = 1000, height = 24) {
  if (!Array.isArray(keys) || keys.length === 0) return '';
  const pad = 3, mid = height / 2, span = mid - pad;
  const xOf = t => Number((viewFrac(view, t) * width).toFixed(2));
  const yOf = az => Number((mid - (clamp(az, -180, 180) / 180) * span).toFixed(2));
  const cmds = [`M ${xOf(0)} ${yOf(keys[0].az)}`];
  if (keys[0].t > 0) cmds.push(`L ${xOf(keys[0].t)} ${yOf(keys[0].az)}`);
  for (let i = 0; i + 1 < keys.length; i++) {
    const a = keys[i], b = keys[i + 1];
    const ease = a.easing || 'linear';
    if (ease === 'hold' || b.t <= a.t) {
      cmds.push(`L ${xOf(b.t)} ${yOf(a.az)}`);
      cmds.push(`L ${xOf(b.t)} ${yOf(b.az)}`);
      continue;
    }
    const delta = remainder360(b.az - a.az);
    const steps = ease === 'linear' ? 1 : 24;
    let prevT = a.t, prevRaw = a.az, prevWrapped = a.az;
    for (let j = 1; j <= steps; j++) {
      const s = j / steps;
      const t = a.t + s * (b.t - a.t);
      const e = applyEasing(ease, s);
      const raw = a.az + delta * e;
      const wrapped = remainder360(raw);
      if (Math.abs(wrapped - prevWrapped) > 180) {
        const seam = raw > prevRaw ? 180 : -180;
        const prevCont = seam === 180 && prevWrapped < 0 ? prevWrapped + 360 : seam === -180 && prevWrapped > 0 ? prevWrapped - 360 : prevWrapped;
        const curCont = seam === 180 && wrapped < 0 ? wrapped + 360 : seam === -180 && wrapped > 0 ? wrapped - 360 : wrapped;
        const denom = curCont - prevCont;
        const f = Math.abs(denom) > 1e-12 ? clamp((seam - prevCont) / denom, 0, 1) : 0.5;
        const tCross = prevT + f * (t - prevT);
        cmds.push(`L ${xOf(tCross)} ${yOf(seam)}`);
        cmds.push(`M ${xOf(tCross)} ${yOf(-seam)}`);
      }
      cmds.push(`L ${xOf(t)} ${yOf(wrapped)}`);
      prevT = t;
      prevRaw = raw;
      prevWrapped = wrapped;
    }
  }
  const last = keys[keys.length - 1];
  if (last.t < 100) cmds.push(`L ${xOf(100)} ${yOf(last.az)}`);
  return cmds.join(' ');
}
