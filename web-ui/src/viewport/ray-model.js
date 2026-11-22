import { reflectionPlanes, imageSourcePaths } from './rays.js';
// ── Ray model shared by the mock adapter and the future engine feed ──────────────
// path = { id, order, points:[[x,y,z]…], lengthM?, delayMs, levelDb | levels[7] (dB re direct),
//          occluded?, diffracted?, surfaces?[], synthetic? }.  See POLISH_REQUESTS.md #2.
export const SPEED_OF_SOUND = 343;
export function levelOf(path) {
  if (Number.isFinite(path.levelDb)) return path.levelDb;
  if (Array.isArray(path.levels) && path.levels.length) return 10 * Math.log10(path.levels.reduce((n, v) => n + Math.pow(10, v / 10), 0) / path.levels.length);
  return null;
}
// Mock level: inverse distance relative to the direct path, no absorption; flagged synthetic.
export function mockRays(source, listener, scene, maxOrder = 2) {
  const paths = imageSourcePaths(source, listener, reflectionPlanes(scene), maxOrder), direct = paths.find(p => p.order === 0)?.lengthM || 1;
  return paths.map((p, i) => ({ id: `mock-${i}`, order: p.order, points: p.points, lengthM: p.lengthM, delayMs: p.lengthM / SPEED_OF_SOUND * 1000, levelDb: 20 * Math.log10(direct / Math.max(p.lengthM, 1e-3)), surfaces: p.surfaces, occluded: false, diffracted: false, synthetic: true }));
}
// Engine event → paths. Unknown / malformed paths are dropped, never guessed.
export function enginePaths(event) {
  return (event?.paths || []).filter(p => Array.isArray(p.points) && p.points.length >= 2 && p.points.every(q => Array.isArray(q) && q.length === 3 && q.every(Number.isFinite))).map((p, i) => ({
    id: p.id ?? `engine-${i}`, pointIndex:p.pointIndex, channelIndex:p.channelIndex, order: p.order ?? Math.max(0, p.points.length - 2), points: p.points, delayMs: p.delayMs ?? null, levelDb: p.levelDb, levels: p.levels, occluded: !!p.occluded, diffracted: !!p.diffracted, surfaces: p.surfaces || [], synthetic: false }));
}
// Visual encoding: brightness + width by level (−48…0 dB), occluded dotted / hidden, diffracted dash-dot.
export function rayStyle(path, { showOccluded = false } = {}) {
  if (path.occluded && !showOccluded) return null;
  const level = levelOf(path), t = level === null ? (path.order === 0 ? 1 : .55) : Math.max(0, Math.min(1, (level + 48) / 48));
  return { alpha: path.occluded ? .2 : .1 + .7 * t, width: (path.order===0?.45:.3)+.25*t, dash: path.occluded ? [2, 5] : path.diffracted ? [7, 2, 1, 2] : path.order === 0 ? [5, 4] : [] };
}
export function describeRay(path) {
  const level = levelOf(path), name = path.order === 0 ? 'Direct' : `${path.order}${['st', 'nd', 'rd'][path.order - 1] || 'th'} order`;
  const bands = Array.isArray(path.levels) && path.levels.length === 7 ? ` · LF ${path.levels[0].toFixed(1)} / MF ${path.levels[3].toFixed(1)} / HF ${path.levels[6].toFixed(1)} dB` : '';
  return `${path.synthetic ? 'MOCK ' : ''}${path.channelIndex>=0?`Ch ${path.channelIndex+1} · `:path.pointIndex!==undefined?'Sum / mono · ':''}${name}${path.occluded ? ' · occluded' : ''}${path.diffracted ? ' · diffracted' : ''} · ${Number.isFinite(path.delayMs) ? `${path.delayMs.toFixed(1)} ms` : 'delay n/a'} · ${level === null ? 'level n/a' : `${level.toFixed(1)} dB`}${bands}`;
}
