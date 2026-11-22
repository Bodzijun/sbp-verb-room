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
export const PATH_EASINGS = ['linear', 'easeIn', 'easeOut', 'easeInOut'];

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
function closedFrac(p,u){const clamped=evaluateTimeMap(p,u);if(!p.closed)return clamped;const laps=Math.max(1,Math.min(64,Math.round(p.laps??1)));const phase=clamped*laps;return phase-Math.floor(phase);}
export function evaluatePath(p,u) {
 const {points,lengths,length}=compilePath(p);if(!points.length)return [0,0,0];
 const frac=closedFrac(p,u);
 if(p.closed&&frac===0)return points[0].slice();
 const d=frac*length;
 let lo=1,hi=points.length-1;while(lo<hi){const mid=(lo+hi)>>1;if(lengths[mid]<=d)lo=mid+1;else hi=mid;}
 const span=lengths[lo]-lengths[lo-1];return mix(points[lo-1],points[lo],span>1e-12?(d-lengths[lo-1])/span:0);
}
export function pathPositionLocked(state,id) {
 const m=/^(s(\d+)|l)[xyz]$/.exec(id);if(!m)return false;
 const p=state.document.paths?.[m[2]?Number(m[2])-1:32];
 return !!(p?.enabled && p.points?.length>=2);  // azimuth stays editable in every orientation mode: it is the offset
}
export const orientationMode=p=>p?.lookAt?'lookAt':p?.orient?'along':'manual';
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
export function withPathPoses(state) {
 if(!state.document.paths?.some(p=>(p?.enabled&&p.points?.length>=2)||p?.lookAt))return state;
 const values={...state.parameters.values};
 state.document.paths.forEach((p,o)=>{if(!p?.enabled||p.points?.length<2)return;const prefix=o===32?'l':`s${o+1}`,driver=pathGroupDriver(state.document.paths,o),u=values[pathId(driver)]||0,q=evaluatePath(p,u);
 ['x','y','z'].forEach((axis,k)=>values[prefix+axis]=q[k]);
 if(p.orient&&!p.lookAt)values[prefix+'az']+=pathYaw(p,u)+(p.orientOffset||0);});
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
