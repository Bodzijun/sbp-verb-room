import { withPathPoses } from '../viewport/movement-path.js';
export const FORMATS = ['Mono', 'Stereo', 'LCR', 'Quad', '5.0', '5.1', '7.1', 'Ring'];
export const COUNTS = [1, 2, 3, 4, 5, 6, 8];
export const LAYOUTS = ['Match track', 'Mono', 'Stereo (ORTF)', 'Stereo (XY)', 'LCR', 'Quad', '5.1', '7.1', '7.1.4', 'Ring', 'AmbiX (1st order)', 'AmbiX (2nd order)', 'AmbiX (3rd order)'];
export const MICS = ['Layout default', 'Omni', 'Subcardioid', 'Cardioid', 'Supercardioid', 'Hypercardioid', 'Figure-8', 'Human ears'];
export const BANDS = ['125', '250', '500', '1k', '2k', '4k', '8k'];
export const SOURCES_OUTPUT_HINT =
  'M / S / Gain act on what the plug-in outputs. Channels the plug-in does not output are passed on by the host unchanged.';
export const POSITIONED_DRY_MULTICHANNEL_WARNING =
  'Positioned dry is off: raw inputs are mixed into output channels k → k, which is not meaningful for Ambisonics/surround. Keep Positioned dry on.';
export function positionedDryWarning(state) {
  if (!state?.parameters?.values || !state?.document?.output || !state?.host) return '';
  if (Number(state.parameters.values.spatialDry ?? 1) >= 0.5) return '';
  const output = state.document.output;
  const effLayout = output.layout >= 0 ? output.layout : state.host.effectiveLayout;
  const rendered = state.host.renderedChannels ?? 0;
  const isAmbi = effLayout >= 9 && effLayout <= 11;
  const isMultiSurround = (effLayout >= 3 && effLayout <= 7) || (effLayout === 8 && (state.host.outputChannels ?? 0) > 2);
  return (isAmbi || isMultiSurround || rendered > 2) ? POSITIONED_DRY_MULTICHANNEL_WARNING : '';
}
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
export const WALL_CORNER_TOLERANCE = 1e-4;
export function translatedWallLinks(scene, refs, wallLayerMode) {
  const links = new Map();
  if (wallLayerMode) return links;
  const moved = new Set(refs.filter(r => r.kind === 'wall' && scene.walls[r.index]).map(r => r.index));
  if (!moved.size) return links;
  const corners = [...moved].flatMap(index => [scene.walls[index].a, scene.walls[index].b]);
  scene.walls.forEach((wall, index) => {
    if (moved.has(index) || wall.editor.hidden || wall.editor.locked) return;
    const ends = ['a', 'b'].filter(end => corners.some(([x, y]) =>
      Math.hypot(wall[end][0] - x, wall[end][1] - y) <= WALL_CORNER_TOLERANCE));
    if (ends.length) links.set(index, ends);
  });
  return links;
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
