import { withPathPoses } from './movement-path.js';
// Main is a view of effective geometry; Edit always owns the original document.
// The processor remains authoritative: Size here only changes drawing coordinates.
const cache = new WeakMap();
export const positionId = id => /^(s\d+|l)[xyz]$/.test(id);
export function viewportState(state, edit = false) {
  state=withPathPoses(state);
  const f = (state.parameters.effective.sizePercent ?? state.parameters.values.size ?? 100) / 100;
  if (edit || f === 1) return state;
  if (cache.has(state)) return cache.get(state);
  const result = structuredClone(state), s = result.document.scene, v = result.parameters.values;
  result.geometryScale=f;
  s.heightM *= f;
  if(s.ceilingFootprint)s.ceilingFootprint=s.ceilingFootprint.map(poly=>poly.map(p=>p.map(x=>x*f)));
  for (const w of s.walls) { w.a=w.a.map(x=>x*f);w.b=w.b.map(x=>x*f);w.zBottomM*=f;if(w.zTopM!==null)w.zTopM*=f; }
  for (const z of [...s.floorZones,...s.ceilingZones]) {z.vertices=z.vertices.map(p=>p.map(x=>x*f));if(z.heightM!==null)z.heightM*=f;}
  for (const [id,value] of Object.entries(v)) if(positionId(id))v[id]=value*f;
  for (const d of result.parameters.descriptors?.parameters || [])if(positionId(d.id)){d.min*=f;d.max*=f;d.step*=f;}
  for (const source of result.document.sources){source.spreadM*=f;source.offsetsM=source.offsetsM.map(p=>p.map(x=>x*f));}
  // output.radiusM / ORTF spacing / ears radius remain physical sizes.
  cache.set(state,result);return result;
}
