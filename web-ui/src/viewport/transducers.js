import { channels } from '../state/selectors.js';
const radians = degrees => degrees * Math.PI / 180;
export function rotateOffset(offset, yaw) { const a = radians(yaw); return [offset[0] * Math.cos(a) - offset[1] * Math.sin(a), offset[0] * Math.sin(a) + offset[1] * Math.cos(a), offset[2]]; }
export function layoutOffset(source, channel) {
  const angles = [[0], [30, -30], [30, -30, 0], [45, -45, 135, -135], [30, -30, 0, 110, -110], [30, -30, 0, 0, 110, -110], [30, -30, 0, 0, 90, -90, 150, -150]];
  if (channels(source) === 1 || source.sum) return [0, 0, 0];
  const angle = radians(source.format === 7 ? channel * 360 / channels(source) : angles[source.format][channel]);
  return [source.spreadM * Math.cos(angle), source.spreadM * Math.sin(angle), 0];
}
export function sourcePoints(state, index) {
  const source = state.document.sources[index], v = state.parameters.values, prefix = `s${index + 1}`, origin = ['x', 'y', 'z'].map(k => v[`${prefix}${k}`]);
  const count = source.sum ? 1 : channels(source), result = [];
  for (let channel = 0; channel < count; channel++) {
    if (!source.sum && [5, 6].includes(source.format) && channel === 3) continue;
    const local = source.sum ? [0, 0, 0] : source.freePoints ? source.offsetsM[channel] || layoutOffset(source, channel) : layoutOffset(source, channel);
    const offset = rotateOffset(local, v[`${prefix}az`]), position = origin.map((n, i) => n + offset[i]);
    position[2] = Math.max(0, Math.min(position[2], state.document.scene.ceilingMaterial === null ? 30*(state.geometryScale??1) : state.document.scene.heightM));
    result.push({ channel, local, position });
  }
  return result;
}
// Geometry and channel names ported from layouts.hpp; only rendering, no acoustic calculation.
export function receivers(state) {
  const output = state.document.output, layout = output.layout >= 0 ? output.layout : state.host.effectiveLayout, v = state.parameters.values;
  const centre = [v.lx, v.ly, Math.min(v.lz, state.document.scene.ceilingMaterial === null ? 30*(state.geometryScale??1) : state.document.scene.heightM)];
  const defs = [
    [['M', 0, 0]],
    [['L', 55, 0], ['R', -55, 0]],
    [['L', 45, 0], ['R', -45, 0]],
    [['L', 30, 0], ['R', -30, 0], ['C', 0, 0]],
    [['FL', 45, 0], ['FR', -45, 0], ['RL', 135, 0], ['RR', -135, 0]],
    [['L', 30, 0], ['R', -30, 0], ['C', 0, 0], ['LFE', 0, 0, true], ['Ls', 110, 0], ['Rs', -110, 0]],
    [['L', 30, 0], ['R', -30, 0], ['C', 0, 0], ['LFE', 0, 0, true], ['Lss', 90, 0], ['Rss', -90, 0], ['Lrs', 150, 0], ['Rrs', -150, 0]]
  ];
  defs[7] = [...defs[6], ['Ltf', 45, 45], ['Rtf', -45, 45], ['Ltr', 135, 45], ['Rtr', -135, 45]];
  defs[9] = [['W', 0, 0], ['Y', 90, 0], ['Z', 0, 90], ['X', 0, 0]];
  const ringCount = Math.min(state.host.outputChannels, 16);
  const ambi = layout >= 9 && layout <= 11, sh = layout === 10 || layout === 11;  // AmbiX 1st..3rd order
  if (sh) defs[layout] = Array.from({ length: layout === 10 ? 9 : 16 }, (_, i) => [`ACN${i}`, 0, 0]);
  const speakers = layout === 8 ? Array.from({ length: ringCount }, (_, i) => [`Ch${i + 1}`, i * 360 / ringCount, 0]) : defs[layout] || defs[1];
  return speakers.map(([name, azimuth, elevation, lfe], index) => {
    const a = radians(azimuth), e = radians(elevation), radius = (layout === 0 || ambi) ? 0 : output.radiusM;
    let local = [radius * Math.cos(e) * Math.cos(a), radius * Math.cos(e) * Math.sin(a), radius * Math.sin(e)];
    if (layout === 1) local = [0, index === 0 ? 0.085 : -0.085, 0];
    if (layout === 2) local = [0, 0, 0];
    if (!ambi && output.micPattern === 7 && speakers.length === 2) { local = [0, index === 0 ? radius : -radius, 0]; azimuth = index === 0 ? 90 : -90; }
    const rotated = rotateOffset(local, v.laz);
    const pattern = sh ? 0 : layout === 9 ? (index === 0 ? 0 : 1) : output.micPattern === 0 ? layout === 0 ? 0 : 0.5 : [0, 0, 0.3, 0.5, 0.63, 0.75, 1, 0][output.micPattern];
    return { name: name || String(index + 1), position: centre.map((n, i) => n + rotated[i]), azimuth: azimuth + v.laz, pattern, lfe: !!lfe, rendered: index < state.host.renderedChannels };
  });
}
