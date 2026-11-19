import { withPathPoses } from '../viewport/movement-path.js';
export const FORMATS = ['Mono', 'Stereo', 'LCR', 'Quad', '5.0', '5.1', '7.1', 'Ring'];
export const COUNTS = [1, 2, 3, 4, 5, 6, 8];
export const LAYOUTS = ['Match track', 'Mono', 'Stereo (ORTF)', 'Stereo (XY)', 'LCR', 'Quad', '5.1', '7.1', '7.1.4', 'Ring', 'AmbiX (1st order)', 'AmbiX (2nd order)', 'AmbiX (3rd order)'];
export const MICS = ['Layout default', 'Omni', 'Subcardioid', 'Cardioid', 'Supercardioid', 'Hypercardioid', 'Figure-8', 'Human ears'];
export const BANDS = ['125', '250', '500', '1k', '2k', '4k', '8k'];
export const channels = source => source.format === 7 ? source.channels : COUNTS[source.format];
export const pointCount = source => source.sum ? 1 : channels(source) - ([5, 6].includes(source.format) ? 1 : 0);
export function budget(document) {
  const ranges = document.sources.map(s => [s.firstIn, s.firstIn + channels(s)]);
  return { inputs: Math.max(0, ...ranges.map(r => r[1])), points: document.sources.reduce((n, s) => n + pointCount(s), 0),
    overlap: ranges.some((a, i) => ranges.some((b, j) => i !== j && a[0] < b[1] && b[0] < a[1])) };
}
export function surface(scene, ref) {
  if (ref.kind === 'wall') return scene.walls[ref.index];
  if (ref.kind === 'floorZone') return scene.floorZones[ref.index];
  if (ref.kind === 'ceilingZone') return scene.ceilingZones[ref.index];
  return { material: scene[`${ref.kind}Material`], editor: scene[`${ref.kind}Editor`] };
}
export function surfaceRefs(scene) {
  return [{ kind: 'floor', index: 0 }, { kind: 'ceiling', index: 0 }, ...scene.walls.map((_, index) => ({ kind: 'wall', index })),
    ...scene.floorZones.map((_, index) => ({ kind: 'floorZone', index })), ...scene.ceilingZones.map((_, index) => ({ kind: 'ceilingZone', index }))];
}
export function expandedRefs(scene, refs, includeGroups = true) {
  const groups = includeGroups ? refs.map(r => surface(scene, r)?.editor?.group).filter(Boolean) : [];
  return surfaceRefs(scene).filter(r => refs.some(s => s.kind === r.kind && s.index === r.index) || groups.includes(surface(scene, r)?.editor?.group));
}
export function layerKey(scene, ref) {
  const object = surface(scene, ref);
  if (ref.kind === 'wall') return 'walls';
  if (object?.heightM != null) return 'plates';
  return ref.kind.startsWith('floor') ? 'floors' : 'ceilings';
}
export function effectiveValue(state, id) {
  const e = state.parameters.effective, values = state.parameters.values;
  const key = { size:'sizePercent',predelay:'predelayMs',stereoWidth:'stereoWidthPercent',freeze:'freeze',duck:'duckDb',duckRelease:'duckReleaseMs',early: 'earlyDb', tail: 'tailDb', doppler: 'doppler', diffusion: 'diffusion' }[id];
  if (key) return e[key];
  if (id.startsWith('decay')) return e.decay[['decayLow', 'decayMid', 'decayHigh'].indexOf(id)];
  if (/^(s\d+az|laz)$/.test(id)) return withPathPoses(state).parameters.values[id];  // E19: azimuth after Along path / Look at
  if (/^(s\d+z|lz)$/.test(id)) return e.zM?.[id] ?? Math.min(values[id], state.document.scene.ceilingMaterial === null ? 30 : state.document.scene.heightM);
  return values[id];
}
