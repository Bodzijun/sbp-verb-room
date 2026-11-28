import { clampOffsetToRoom } from './room-bounds.js';

export const pathId = o => o === 32 ? 'lpath' : `s${o+1}path`;
const mix=(a,b,u)=>a.map((x,i)=>x+(b[i]-x)*u);
const distance=(a,b)=>Math.hypot(...a.map((x,i)=>x-b[i]));
// Centripetal Barry-Goldman; open end spans use the three-point knot parabola
// without a phantom, matching the neighbouring interior tangent and C++ pathCurve.
export function pathCurve(p,s,u) {
 const n=p.points.length, at=i=>p.closed?p.points[(i%n+n)%n]:i<0?mix(p.points[1],p.points[0],2):i>=n?mix(p.points[n-2],p.points[n-1],2):p.points[i];
 const a=at(s-1),b=at(s),c=at(s+1),d=at(s+2);
 if(!p.smooth||(!p.closed&&n===2))return mix(b,c,u);if(distance(b,c)<1e-10)return b;
 if(!p.closed&&s===0){const t1=0,t2=Math.max(1e-5,Math.sqrt(distance(b,c))),t3=t2+Math.max(1e-5,Math.sqrt(distance(c,d))),t=t1+u*(t2-t1),blend=(x,y,lo,hi)=>mix(x,y,(t-lo)/(hi-lo));return blend(blend(b,c,t1,t2),blend(c,d,t2,t3),t1,t3);}
 const t0=0,t1=Math.max(1e-5,Math.sqrt(distance(a,b))),t2=t1+Math.max(1e-5,Math.sqrt(distance(b,c))),t3=t2+Math.max(1e-5,Math.sqrt(distance(c,d))),t=t1+u*(t2-t1);
 const blend=(x,y,lo,hi)=>mix(x,y,(t-lo)/(hi-lo));
 if(!p.closed&&s===n-2)return blend(blend(a,b,t0,t1),blend(b,c,t1,t2),t0,t2);
 return blend(blend(blend(a,b,t0,t1),blend(b,c,t1,t2),t0,t2),blend(blend(b,c,t1,t2),blend(c,d,t2,t3),t1,t3),t1,t2);
}
export const MIN_WINDOW_GAP = 0.01;
export const DEFAULT_ENDLESS_PERIOD_S = 8;
export const MIN_ENDLESS_PERIOD_S = 0.25;
export const MAX_ENDLESS_PERIOD_S = 3600;
export const PATH_EASINGS = ['linear', 'easeIn', 'easeOut', 'easeInOut'];

export function endlessTransportPhase(samplePos, sampleRate, periodS = DEFAULT_ENDLESS_PERIOD_S) {
 if (!(sampleRate > 0) || !(periodS > 0) || !Number.isFinite(sampleRate) || !Number.isFinite(periodS)) return 0;
 const periodSamples = periodS * sampleRate;
 if (!(periodSamples > 0) || !Number.isFinite(periodSamples)) return 0;
 let rem = samplePos % periodSamples;
 if (rem < 0) rem += periodSamples;
 let phase = rem / periodSamples;
 if (phase >= 1 || phase < 0 || !Number.isFinite(phase)) phase = 0;
 return phase;
}

export function endlessEffectiveU(p, uBase, transportPhase) {
 const u0 = evaluateTimeMap(p, uBase);
 if (!p?.closed || !p?.endless) return u0;
 let u = u0 + transportPhase;
 u -= Math.floor(u);
 if (u >= 1 || u < 0 || !Number.isFinite(u)) u = 0;
 return u;
}

export function wanderEndEnvelope(u, closed, pinEnds = true) {
 if (closed || !pinEnds) return 1;
 const uc = Math.max(0, Math.min(1, Number.isFinite(u) ? u : 0));
 const edge = Math.min(uc, 1 - uc);
 const s = Math.max(0, Math.min(1, edge / 0.15));
 return s * s * (3 - 2 * s);
}

export function applyEasing(e, s) {
 const x = Math.max(0, Math.min(1, s));
 if (e === 'easeIn') return x * x;
 if (e === 'easeOut') return 1 - (1 - x) * (1 - x);
 if (e === 'easeInOut') return x * x * (3 - 2 * x);
 return x;
}

export function invertEasing(e, r) {
 const y = Math.max(0, Math.min(1, r));
 if (e === 'easeIn') return Math.sqrt(y);
 if (e === 'easeOut') return 1 - Math.sqrt(Math.max(0, 1 - y));
 if (e === 'easeInOut') return Math.max(0, Math.min(1, 0.5 - Math.sin(Math.asin(Math.max(-1, Math.min(1, 1 - 2 * y))) / 3)));
 return y;
}

export function hasCustomTiming(p) {
 const start = p?.start ?? 0, end = p?.end ?? 100;
 if (start !== 0 || end !== 100) return true;
 if (p?.times?.some(t => t !== null && t !== undefined)) return true;
 if (p?.easings?.some(e => e && e !== 'linear')) return true;
 return false;
}

export function isPointKey(p, index) {
 const n = p?.points?.length ?? 0;
 if (n < 2 || index < 0 || index >= n) return false;
 if (index === 0) return true;
 if (!p.closed && index === n - 1) return true;
 return p.times?.[index] !== null && p.times?.[index] !== undefined;
}

export function canPointHaveEasing(p, index) {
 const n = p?.points?.length ?? 0;
 if (n < 2 || index < 0 || index >= n) return false;
 if (!p.closed && index === n - 1) return false;
 return index === 0 || (p.times?.[index] !== null && p.times?.[index] !== undefined);
}

export function pointTimeBounds(p, index) {
 const n = p?.points?.length ?? 0;
 let min = 0, max = 100;
 for (let k = index - 1; k >= 1; k--) {
  const t = p?.times?.[k];
  if (t !== null && t !== undefined && Number.isFinite(t)) { min = t; break; }
 }
 for (let k = index + 1; k < n; k++) {
  if (!p?.closed && k === n - 1) break;
  const t = p?.times?.[k];
  if (t !== null && t !== undefined && Number.isFinite(t)) { max = t; break; }
 }
 return { min, max };
}

export function clampPointTime(p, index, raw) {
 const { min, max } = pointTimeBounds(p, index);
 return Math.max(min, Math.min(max, raw));
}

const cache=new WeakMap();
export function compilePath(p) {
 if(cache.has(p))return cache.get(p);
 const points=[],lengths=[],steps=p.smooth?128:1;let length=0;
 const n = p.points?.length ?? 0;
 const segs = Math.max(0, n - (p.closed ? 0 : 1));
 if(n>=2){points.push(p.points[0]);lengths.push(0);
 for(let s=0;s<segs;s++)for(let j=1;j<=steps;j++){const q=(p.closed&&s===segs-1&&j===steps)?p.points[0]:pathCurve(p,s,j/steps);length+=distance(points.at(-1),q);points.push(q);lengths.push(length);}}
 const knotFrac = k => {
  if (k <= 0) return 0;
  if (k >= segs) return 1;
  return length > 0 ? lengths[k * steps] / length : k / Math.max(1, segs);
 };
 const easingOf = k => p.easings?.[k] || 'linear';
 const timeKeys = [];
 const keyKnots = [];
 if (n >= 2) {
  timeKeys.push({ t: 0, f: 0, easing: easingOf(0) });
  keyKnots.push(0);
  for (let k = 1; k < n; k++) {
   if (!p.closed && k === n - 1) break;
   const tv = p.times?.[k];
   if (tv !== null && tv !== undefined && Number.isFinite(tv)) {
    timeKeys.push({ t: Math.max(0, Math.min(1, tv * 0.01)), f: knotFrac(k), easing: easingOf(k) });
    keyKnots.push(k);
   }
  }
  timeKeys.push({ t: 1, f: 1, easing: 'linear' });
  keyKnots.push(segs);
 }
 const computedTimes = Array.from({ length: n }, (_, k) => {
  if (n < 2 || k === 0) return 0;
  if (!p.closed && k === n - 1) return 100;
  const tv = p.times?.[k];
  if (tv !== null && tv !== undefined && Number.isFinite(tv)) return tv;
  let ki = 0;
  while (ki + 1 < keyKnots.length && keyKnots[ki + 1] < k) ki++;
  const ka = keyKnots[ki], kb = keyKnots[ki + 1];
  const a = timeKeys[ki], b = timeKeys[ki + 1];
  const df = b.f - a.f;
  let s = 0;
  if (df > 1e-12) s = invertEasing(a.easing, (knotFrac(k) - a.f) / df);
  else if (kb > ka) s = (k - ka) / (kb - ka);
  return (a.t + s * (b.t - a.t)) * 100;
 });
 const startNorm = Math.max(0, Math.min(1, (p.start ?? 0) * 0.01));
 const endNorm = Math.max(0, Math.min(1, (p.end ?? 100) * 0.01));
 const result={points,lengths,length,steps,timeKeys,computedTimes,startNorm,endNorm,hasTiming:hasCustomTiming(p)};
 cache.set(p,result);return result;
}
export function evaluateTimeMap(p, u) {
 const clamped = Math.max(0, Math.min(1, u));
 const t = compilePath(p);
 if (!t.hasTiming) return clamped;
 if (clamped <= t.startNorm) return 0;
 if (clamped >= t.endNorm) return 1;
 const wSpan = t.endNorm - t.startNorm;
 const w = wSpan > 0 ? Math.max(0, Math.min(1, (clamped - t.startNorm) / wSpan)) : 1;
 if (w <= 0) return 0;
 if (w >= 1) return 1;
 const keys = t.timeKeys;
 if (keys.length < 2) return w;
 let hi = 1;
 while (hi + 1 < keys.length && keys[hi].t <= w) hi++;
 const lo = hi - 1;
 const tSpan = keys[hi].t - keys[lo].t;
 const s = tSpan > 0 ? (w - keys[lo].t) / tSpan : 0;
 const e = applyEasing(keys[lo].easing, s);
 return keys[lo].f + e * (keys[hi].f - keys[lo].f);
}
function closedFrac(p,u){const clamped=evaluateTimeMap(p,u);if(!p.closed)return clamped;const laps=p.endless?1:Math.max(1,Math.min(64,Math.round(p.laps??1)));const phase=clamped*laps;return phase-Math.floor(phase);}
export function evaluateEndlessPath(p, uBase, transportPhase) {
 const { points, lengths, length } = compilePath(p);
 if (!points.length) return [0, 0, 0];
 const frac = endlessEffectiveU(p, uBase, transportPhase);
 if (p.closed && frac === 0) return points[0].slice();
 const d = frac * length;
 let lo = 1, hi = points.length - 1;
 while (lo < hi) { const mid = (lo + hi) >> 1; if (lengths[mid] <= d) lo = mid + 1; else hi = mid; }
 const span = lengths[lo] - lengths[lo - 1];
 return mix(points[lo - 1], points[lo], span > 1e-12 ? (d - lengths[lo - 1]) / span : 0);
}
export function evaluatePath(p,u) {
 const {points,lengths,length}=compilePath(p);if(!points.length)return [0,0,0];
 const frac=closedFrac(p,u);
 if(p.closed&&frac===0)return points[0].slice();
 const d=frac*length;
 let lo=1,hi=points.length-1;while(lo<hi){const mid=(lo+hi)>>1;if(lengths[mid]<=d)lo=mid+1;else hi=mid;}
 const span=lengths[lo]-lengths[lo-1];return mix(points[lo-1],points[lo],span>1e-12?(d-lengths[lo-1])/span:0);
}
export function remainder360(x) {
 const q = x / 360;
 const n = Math.floor(q);
 const f = q - n;
 const k = f < 0.5 ? n : (f > 0.5 ? n + 1 : ((n % 2 === 0) ? n : n + 1));
 const r = x - k * 360;
 return Object.is(r, -0) ? 0 : r;
}
export function orientationMode(p){return p?.lookAt?'lookAt':p?.orient?'along':'manual';}
export function hasActiveAzimuthKeys(p) {
 return !!(p?.enabled && (p.points?.length ?? 0) >= 2 && orientationMode(p) === 'manual' && Array.isArray(p.azKeys) && p.azKeys.length > 0);
}
export function evaluateAzimuthKeys(keys, u) {
 if (!Array.isArray(keys) || keys.length === 0) return null;
 const uc = Number.isFinite(u) ? Math.max(0, Math.min(1, u)) : 0;
 const tPct = uc * 100;
 if (tPct <= keys[0].t) return keys[0].az === 0 ? 0 : keys[0].az;
 if (tPct >= keys[keys.length - 1].t) {
  const last = keys[keys.length - 1].az;
  return last === 0 ? 0 : last;
 }
 let loIdx = 0, hiIdx = keys.length;
 while (loIdx < hiIdx) {
  const mid = (loIdx + hiIdx) >> 1;
  if (tPct < keys[mid].t) hiIdx = mid;
  else loIdx = mid + 1;
 }
 const hi = Math.max(1, Math.min(keys.length - 1, loIdx));
 const lo = hi - 1;
 const kLo = keys[lo], kHi = keys[hi];
 if (kLo.easing === 'hold' || kHi.t <= kLo.t) return kLo.az === 0 ? 0 : kLo.az;
 const s = (tPct - kLo.t) / (kHi.t - kLo.t);
 const e = applyEasing(kLo.easing, s);
 const delta = remainder360(kHi.az - kLo.az);
 return remainder360(kLo.az + delta * e);
}
export function pathPositionLocked(state,id) {
 const m=/^(s(\d+)|l)(x|y|z|az|path)$/.exec(id);if(!m)return false;
 const p=state.document.paths?.[m[2]?Number(m[2])-1:32];
 if(isFollower(p)){  // V04d: a follower's pose and Position come from its target
  const r=resolveFollowPath(state.document.paths,m[2]?Number(m[2])-1:32);
  if(m[3]==='path')return true;
  if(!r?.enabled||(r.points?.length??0)<2)return false;
  if(m[3]==='az')return !!(orientationMode(r)==='manual'&&Array.isArray(r.azKeys)&&r.azKeys.length>0);
  return true;
 }
 if(!p?.enabled || (p.points?.length??0)<2)return false;
 if(m[3]==='path')return !!(p.closed && p.endless);
 if(m[3]==='az')return hasActiveAzimuthKeys(p);
 return true;
}
export function pathYaw(p,u) {
 const t=compilePath(p);if(t.points.length<2)return 0;
 const frac=closedFrac(p,u);
 let i=1;
 if(p.closed&&frac===0){while(i+1<t.points.length&&t.lengths[i]-t.lengths[i-1]<=1e-12)i++;}
 else{const d=frac*t.length;while(i+1<t.points.length && t.lengths[i]<=d)i++;}
 let ti=i;
 if(t.lengths[ti]-t.lengths[ti-1]<=1e-12){
  while(ti>1&&t.lengths[ti]-t.lengths[ti-1]<=1e-12)ti--;
  if(t.lengths[ti]-t.lengths[ti-1]<=1e-12){ti=i;while(ti+1<t.points.length&&t.lengths[ti]-t.lengths[ti-1]<=1e-12)ti++;}
 }
 if(t.lengths[ti]-t.lengths[ti-1]<=1e-12)return 0;
 const a=t.points[ti-1],b=t.points[ti];return Math.atan2(b[1]-a[1],b[0]-a[0])*180/Math.PI;
}
export function pathGroupDriver(paths, object) {
 const p = paths?.[object];
 const g = p?.group ?? -1;
 if (g < 0 || !p?.enabled || (p.points?.length ?? 0) < 2) return object;
 for (let o = 0; o < 32; o++) {
  const c = paths[o];
  if (c?.enabled && c.points?.length >= 2 && (c.group ?? -1) === g) return o;
 }
 return object;
}
export function pathGroupMembers(paths, group) {
 if (!Array.isArray(paths) || group < 0) return [];
 const out = [];
 for (let o = 0; o <= 32; o++) {
  const c = paths[o];
  if (c?.enabled && c.points?.length >= 2 && (c.group ?? -1) === group) out.push(o);
 }
 return out;
}

// ---- V04d: Link to path (rig). Mirrors C++ resolveFollowerPath / CompiledPath::followYawAtSingleLap / followOffsetDelta. ----
export const MAX_FOLLOW_OFFSET_M = 50;
export function isFollower(p) { return Number.isInteger(p?.follow?.target) && p.follow.target >= 0; }
export function followTargetOf(p) { return isFollower(p) ? p.follow.target : -1; }
export function followersOf(paths, target) {
 const out = [];
 if (!Array.isArray(paths)) return out;
 for (let o = 0; o < 32; o++) if (paths[o]?.follow?.target === target && o !== target) out.push(o);
 return out;
}
// A usable link target: enabled path with >= 2 points that is not itself a follower.
export function canBeFollowTarget(p) { return !!(p?.enabled !== false && (p?.points?.length ?? 0) >= 2 && !isFollower(p)); }
const resolvedCache = new WeakMap();
const EMPTY_TARGET = {};
// The path a follower runs: the target's geometry/timing/Laps/Endless/LFO (+ wander when followWander), the follower's
// own enable / orientation / azimuth keys. Unusable target -> no points (the follower stays static).
export function resolveFollowPath(paths, o) {
 const f = paths?.[o];
 if (!isFollower(f)) return f;
 const t = f.follow.target === o ? null : paths?.[f.follow.target];
 const key = t || EMPTY_TARGET;
 let byTarget = resolvedCache.get(f);
 if (!byTarget) { byTarget = new WeakMap(); resolvedCache.set(f, byTarget); }
 if (byTarget.has(key)) return byTarget.get(key);
 const own = { enabled: f.enabled !== false, orient: !!f.orient, lookAt: !!f.lookAt, lookTarget: f.lookTarget, lookPos: f.lookPos, orientOffset: f.orientOffset || 0, azKeys: f.azKeys, follow: f.follow };
 let r;
 if (!t || !canBeFollowTarget(t)) r = { ...own, enabled: false, points: [] };
 else r = { ...own, points: t.points, closed: !!t.closed, smooth: !!t.smooth, laps: t.laps ?? 1, endless: !!t.endless, endlessPeriodS: t.endlessPeriodS,
  start: t.start, end: t.end, times: t.times, easings: t.easings, lfo: t.lfo, wander: f.follow.followWander !== false ? t.wander : undefined, group: -1 };
 byTarget.set(key, r);
 return r;
}
const followYawCache = new WeakMap();
// Heading (deg) of every compiled segment; horizontally degenerate segments hold the previous valid heading
// (the next one before the first valid; closed paths wrap from the last valid).
export function followYawTable(table, closed) {
 const cached = followYawCache.get(table);
 if (cached) return cached;
 const { points } = table, n = points.length, out = new Array(n).fill(NaN);
 let first = -1, last = -1;
 for (let i = 1; i < n; i++) {
  const dx = points[i][0] - points[i - 1][0], dy = points[i][1] - points[i - 1][1];
  if (Math.hypot(dx, dy) > 1e-9) { out[i] = Math.atan2(dy, dx) * 180 / Math.PI; if (first < 0) first = i; last = i; }
 }
 if (first < 0) out.fill(0);
 else {
  let held = closed ? out[last] : out[first];
  for (let i = 1; i < n; i++) { if (Number.isNaN(out[i])) out[i] = held; else held = out[i]; }
  out[0] = out[1] ?? 0;
 }
 followYawCache.set(table, out);
 return out;
}
export function followYawAtSingleLap(p, frac) {
 const t = compilePath(p);
 if (t.points.length < 2) return 0;
 const yt = followYawTable(t, !!p.closed);
 let f = Math.max(0, Math.min(1, Number.isFinite(frac) ? frac : 0));
 if (p.closed) f -= Math.floor(f);
 if (p.closed && f === 0) return yt[1];
 const d = f * t.length;
 // first vertex at or beyond d (lower_bound): a stop on a vertex holds the incoming heading
 let lo = 1, hi = t.points.length - 1;
 while (lo < hi) { const mid = (lo + hi) >> 1; if (t.lengths[mid] < d) lo = mid + 1; else hi = mid; }
 return yt[lo];
}
export function followOffsetDelta(yawDeg, off) {
 const r = (Number.isFinite(yawDeg) ? yawDeg : 0) * Math.PI / 180, c = Math.cos(r), s = Math.sin(r);
 const f = Number(off?.[0]) || 0, l = Number(off?.[1]) || 0, u = Number(off?.[2]) || 0;
 return [c * f - s * l, s * f + c * l, u];
}
export function singleLapFrac(p, u) { return closedFrac(p, u); }
// Drawn pose of a follower. The optional scene applies room clearances to the preview.
export function followerPose(paths, o, uBase, transportPhase = 0, zTop = 30, scene = null) {
 const r = resolveFollowPath(paths, o);
 if (!r || (r.points?.length ?? 0) < 2) return null;
 let frac, base;
 if (r.closed && r.endless) { frac = endlessEffectiveU(r, uBase, transportPhase); base = evaluateEndlessPath(r, uBase, transportPhase); }
 else { frac = closedFrac(r, uBase); base = evaluatePath(r, uBase); }
 const yaw = followYawAtSingleLap(r, frac);
 const d = followOffsetDelta(yaw, r.follow.offset);
 const bz = Math.max(0, Math.min(zTop, base[2]));
 const origin = [base[0], base[1], bz];
 const proposed = [base[0] + d[0], base[1] + d[1], bz + d[2]];
 const pos = scene ? clampOffsetToRoom(scene, origin, proposed, zTop) :
   [proposed[0], proposed[1], Math.max(0, Math.min(zTop, proposed[2]))];
 return { pos, base: origin, yaw, frac };
}
// The follower's offset curve over one lap of the target's geometry (drawing / Unlink preview).
export function followerCurve(paths, o, samples = 96, zTop = 30) {
 const r = resolveFollowPath(paths, o);
 if (!r || (r.points?.length ?? 0) < 2) return [];
 const t = compilePath(r), yt = followYawTable(t, !!r.closed), off = r.follow.offset, pts = [];
 for (let i = 0; i < t.points.length; i++) {
  const q = t.points[i];
  // both headings at a vertex: the incoming and the outgoing segment (sharp corners show their real jump)
  const yaws = i === 0 ? [yt[1]] : (i + 1 < t.points.length && Math.abs(remainder360(yt[i + 1] - yt[i])) > 1e-6 ? [yt[i], yt[i + 1]] : [yt[i]]);
  for (const y of yaws) { const d = followOffsetDelta(y, off); pts.push([q[0] + d[0], q[1] + d[1], Math.max(0, Math.min(zTop, Math.max(0, Math.min(zTop, q[2])) + d[2]))]); }
 }
 if (pts.length <= samples * 4) return pts;
 const stride = Math.ceil(pts.length / (samples * 4)), out = [];
 for (let i = 0; i < pts.length; i += stride) out.push(pts[i]);
 if (out.at(-1) !== pts.at(-1)) out.push(pts.at(-1));
 return out;
}
// Point at arc fraction `frac` of one lap of the compiled geometry (closed: wraps, open: clamped).
export function pathArcPoint(p, frac) {
 const t = compilePath(p), { points, lengths, length } = t;
 if (!points.length) return [0, 0, 0];
 let u = Number.isFinite(frac) ? frac : 0;
 if (p.closed) { u -= Math.floor(u); if (u === 0) return points[0].slice(); } else u = Math.max(0, Math.min(1, u));
 const d = u * length;
 let lo = 1, hi = points.length - 1;
 while (lo < hi) { const mid = (lo + hi) >> 1; if (lengths[mid] <= d) lo = mid + 1; else hi = mid; }
 const span = lengths[lo] - lengths[lo - 1], k = span > 1e-12 ? (d - lengths[lo - 1]) / span : 0, a = points[lo - 1], b = points[lo];
 return [a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k];
}
// Unlink: the follower's current offset curve as an own smooth path (n samples uniform in the target's arc fraction),
// with the target's Laps / Endless / Start-End / LFO (+ wander when Follow wander is on) and the follower's orientation.
export function followerToOwnPath(paths, o, zTop = 30, samples = 64) {
 const f = paths?.[o];
 const own = { points: [], enabled: f?.enabled !== false, closed: false, smooth: false, orient: !!f?.orient, laps: 1 };
 for (const k of ['lookAt', 'lookTarget', 'lookPos', 'orientOffset']) if (f && k in f) own[k] = f[k];
 const r = resolveFollowPath(paths, o);
 if (!isFollower(f) || !r || (r.points?.length ?? 0) < 2) return own;
 const t = paths[f.follow.target], n = Math.max(2, Math.min(64, samples)), zMax = Math.min(30, zTop);
 for (let k = 0; k < n; k++) {
  const fr = t.closed ? k / n : k / (n - 1), q = pathArcPoint(r, fr), d = followOffsetDelta(followYawAtSingleLap(r, fr), f.follow.offset);
  own.points.push([Math.max(-200, Math.min(200, q[0] + d[0])), Math.max(-200, Math.min(200, q[1] + d[1])), Math.max(0, Math.min(zMax, Math.max(0, Math.min(zTop, q[2])) + d[2]))]);
 }
 own.closed = !!t.closed; own.smooth = true; own.laps = t.laps ?? 1;
 if (t.endless) { own.endless = true; own.endlessPeriodS = t.endlessPeriodS ?? 8; }
 if (t.start !== undefined && t.start !== 0) own.start = t.start;
 if (t.end !== undefined && t.end !== 100) own.end = t.end;
 if (t.lfo) own.lfo = JSON.parse(JSON.stringify(t.lfo));
 if (f.follow.followWander !== false && t.wander) own.wander = JSON.parse(JSON.stringify(t.wander));
 if (Array.isArray(f.azKeys) && f.azKeys.length) own.azKeys = JSON.parse(JSON.stringify(f.azKeys));
 return own;
}
export function withPathPoses(state) {
 if(!state.document.paths?.some(p=>(p?.enabled&&p.points?.length>=2)||p?.lookAt||isFollower(p)))return state;
 const values={...state.parameters.values};
 const nsAll=state.document.sources?.length??32,zTopAll=Math.max(0.5,Math.min(30,Number(state.document.scene?.heightM)||30));
 state.document.paths.forEach((p,o)=>{
 if(isFollower(p)){  // V04d: target's parameter (its group driver) + offset in the path frame
  if(o===32||p.follow.target>=nsAll)return;
  const r=resolveFollowPath(state.document.paths,o);if(!r?.enabled||(r.points?.length??0)<2)return;
  const prefix=`s${o+1}`,driver=pathGroupDriver(state.document.paths,p.follow.target),u=values[pathId(driver)]||0,fp=followerPose(state.document.paths,o,u,0,zTopAll,state.document.scene);
  if(!fp)return;
  ['x','y','z'].forEach((axis,k)=>values[prefix+axis]=fp.pos[k]);
  if(r.orient&&!r.lookAt)values[prefix+'az']+=fp.yaw+(r.orientOffset||0);
  else if(!r.lookAt&&Array.isArray(r.azKeys)&&r.azKeys.length>0){const keyAz=evaluateAzimuthKeys(r.azKeys,u);if(keyAz!==null)values[prefix+'az']=keyAz;}
  return;
 }
 if(!p?.enabled||p.points?.length<2)return;const prefix=o===32?'l':`s${o+1}`,driver=pathGroupDriver(state.document.paths,o),u=values[pathId(driver)]||0,q=evaluatePath(p,u);
 ['x','y','z'].forEach((axis,k)=>values[prefix+axis]=q[k]);
 if(p.orient&&!p.lookAt)values[prefix+'az']+=pathYaw(p,u)+(p.orientOffset||0);
 else if(!p.lookAt&&Array.isArray(p.azKeys)&&p.azKeys.length>0){const keyAz=evaluateAzimuthKeys(p.azKeys,u);if(keyAz!==null)values[prefix+'az']=keyAz;}});
 // E19 Look at: azimuth from the positions just resolved (same function as the engine: atan2 + azimuth + offset, wrapped; target missing or coincident keeps the manual value)
 const ns=state.document.sources?.length??32,pre=o=>o===32?'l':`s${o+1}`;
 state.document.paths.forEach((p,o)=>{
  if(!p?.lookAt||(o!==32&&o>=ns))return;
  let tx,ty;
  if(Array.isArray(p.lookPos)){tx=p.lookPos[0];ty=p.lookPos[1];}
  else if(Number.isInteger(p.lookTarget)&&p.lookTarget!==o&&(p.lookTarget===32||p.lookTarget<ns)){tx=values[pre(p.lookTarget)+'x'];ty=values[pre(p.lookTarget)+'y'];}
  else return;
  const dx=tx-values[pre(o)+'x'],dy=ty-values[pre(o)+'y'];
  if(!(Math.hypot(dx,dy)>=1e-3))return;
  const a=Math.atan2(dy,dx)*180/Math.PI+values[pre(o)+'az']+(p.orientOffset||0);
  values[pre(o)+'az']=a-360*Math.round(a/360);
 });
 return {...state,parameters:{...state.parameters,values}};
}
