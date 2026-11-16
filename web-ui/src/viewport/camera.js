import { clamp } from '../bridge/parameters.js';
export const add = (a, b) => a.map((v, i) => v + b[i]);
export const sub = (a, b) => a.map((v, i) => v - b[i]);
export const mul = (a, n) => a.map(v => v * n);
export const dot = (a, b) => a.reduce((n, v, i) => n + v * b[i], 0);
export function basis(camera) {
  if (camera.view === 'plan') return { right: [1, 0, 0], up: [0, 1, 0], back: [0, 0, 1] };
  if (camera.view === 'front') return { right: [1, 0, 0], up: [0, 0, 1], back: [0, -1, 0] };
  if (camera.view === 'side') return { right: [0, 1, 0], up: [0, 0, 1], back: [1, 0, 0] };
  const y = camera.yaw, p = camera.pitch;
  return { right: [-Math.sin(y), Math.cos(y), 0], up: [-Math.cos(y) * Math.sin(p), -Math.sin(y) * Math.sin(p), Math.cos(p)], back: [Math.cos(y) * Math.cos(p), Math.sin(y) * Math.cos(p), Math.sin(p)] };
}
export function createCamera(view = '3d') { return { view, target: [0, 0, 0], yaw: -0.75, pitch: 0.5, distance: 20, fov: Math.PI / 3, referenceDistance: 20 }; }
export function project(point, camera, width, height) {
  const b = basis(camera), p = sub(point, camera.target), depth = camera.distance - dot(p, b.back);
  const factor = height / (2 * Math.tan(camera.fov / 2)) / (camera.view === '3d' ? depth : camera.distance);
  return [width / 2 + dot(p, b.right) * factor, height / 2 - dot(p, b.up) * factor, depth];
}
export function pointOnTargetPlane(screen, camera, width, height) {
  const b = basis(camera), units = camera.distance * 2 * Math.tan(camera.fov / 2) / height;
  return add(camera.target, add(mul(b.right, (screen[0] - width / 2) * units), mul(b.up, -(screen[1] - height / 2) * units)));
}
// In perspective, pose drags keep Z fixed: intersect the actual cursor ray with
// that horizontal plane. Target-plane XY deltas drift away from the pointer.
export function pointOnHeightPlane(screen, camera, width, height, z) {
  const b = basis(camera), scale = 2 * Math.tan(camera.fov / 2) / height;
  const origin = add(camera.target, mul(b.back, camera.distance));
  const direction = add(mul(b.back, -1), add(mul(b.right, (screen[0] - width / 2) * scale), mul(b.up, -(screen[1] - height / 2) * scale)));
  if (Math.abs(direction[2]) < 1e-5) return null;
  const distance = (z - origin[2]) / direction[2];
  return distance > 0 ? add(origin, mul(direction, distance)) : null;
}
export function pan(camera, from, to, width, height) {
  const a = pointOnTargetPlane(from, camera, width, height), b = pointOnTargetPlane(to, camera, width, height);
  return { ...camera, target: add(camera.target, sub(a, b)) };
}
export function zoomAt(camera, screen, factor, width, height) {
  const before = pointOnTargetPlane(screen, camera, width, height);
  // frameSelection must never make the outward limit tighter than the full scene.
  const reference = Math.max(camera.referenceDistance, camera.sceneDistance || 0);
  const next = { ...camera, distance: clamp(camera.distance * factor, camera.referenceDistance / 64, reference * 4) };
  const after = pointOnTargetPlane(screen, next, width, height); next.target = add(next.target, sub(before, after)); return next;
}
export function corners(bounds) {
  const out = []; for (const x of [bounds.min[0], bounds.max[0]]) for (const y of [bounds.min[1], bounds.max[1]]) for (const z of [bounds.min[2], bounds.max[2]]) out.push([x, y, z]); return out;
}
export function boundsOf(points) {
  if (!points.length) return { min: [-1, -1, 0], max: [1, 1, 2] };
  return { min: [0, 1, 2].map(i => Math.min(...points.map(p => p[i]))), max: [0, 1, 2].map(i => Math.max(...points.map(p => p[i]))) };
}
export function frame(camera, bounds, width, height) {
  const next = { ...camera, target: add(bounds.min, mul(sub(bounds.max, bounds.min), 0.5)) }, b = basis(next);
  const t = Math.tan(next.fov / 2), aspect = width / height;
  let distance = 0.5;
  corners(bounds).forEach(point => {
    const p = sub(point, next.target), depth = next.view === '3d' ? dot(p, b.back) : 0;
    distance = Math.max(distance, dot(p, b.back) + 0.1, depth + Math.abs(dot(p, b.right)) / (t * aspect) * 1.22, depth + Math.abs(dot(p, b.up)) / t * 1.22);
  });
  next.distance = distance; next.referenceDistance = distance; return next;
}
// T11: screen-space extent of a point set; `fits` = every point inside the viewport with a small margin.
export function screenExtent(points, camera, width, height, margin = 4) {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity, behind = false;
  for (const p of points) { const [x, y, depth] = project(p, camera, width, height); if (depth <= 0) behind = true; x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); }
  if (!points.length) return { fits: true, fillX: 0, fillY: 0, fill: 0 };
  const fits = !behind && x0 >= margin && y0 >= margin && x1 <= width - margin && y1 <= height - margin;
  return { fits, fillX: (x1 - x0) / width, fillY: (y1 - y0) / height, fill: Math.max((x1 - x0) / width, (y1 - y0) / height), box: [x0, y0, x1, y1] };
}
export function scenePoints(state) {
  const s = state.document.scene, values = state.parameters.values;
  return [...s.walls.flatMap(w => { const topZ = (w.zTopM ?? s.heightM) < 1e5 ? (w.zTopM ?? s.heightM) : s.heightM; return [w.a, w.b].flatMap(p => [[...p, w.zBottomM], [...p, topZ]]); }),
    ...s.floorZones.flatMap(z => z.vertices.map(v => [...v, z.heightM ?? 0])), ...s.ceilingZones.flatMap(z => z.vertices.map(v => [...v, z.heightM ?? s.heightM])),
    ...state.document.sources.map((_, i) => ['x', 'y', 'z'].map(k => values[`s${i + 1}${k}`])), ['lx', 'ly', 'lz'].map(k => values[k])];
}
