import { traceWallLoops } from './room-metrics.js';

const clamp = (value, min, max) => Math.min(max, Math.max(min, value));

function pointInPolygon2D(x, y, poly) {
  let inside = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const a = poly[i], b = poly[j];
    if ((a[1] > y) !== (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0])
      inside = !inside;
  }
  return inside;
}

function pointToSegmentDist2D(px, py, ax, ay, bx, by) {
  const dx = bx - ax, dy = by - ay;
  const len2 = dx * dx + dy * dy;
  const t = len2 > 1e-12 ? clamp(((px - ax) * dx + (py - ay) * dy) / len2, 0, 1) : 0;
  return Math.hypot(px - (ax + t * dx), py - (ay + t * dy));
}

function cross2D(ax, ay, bx, by, cx, cy) {
  return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

export function segmentsIntersect2D(ax, ay, bx, by, cx, cy, dx, dy) {
  if (Math.max(ax, bx) < Math.min(cx, dx) - 1e-9 || Math.max(cx, dx) < Math.min(ax, bx) - 1e-9 ||
      Math.max(ay, by) < Math.min(cy, dy) - 1e-9 || Math.max(cy, dy) < Math.min(ay, by) - 1e-9)
    return false;
  const d1 = cross2D(ax, ay, bx, by, cx, cy);
  const d2 = cross2D(ax, ay, bx, by, dx, dy);
  const d3 = cross2D(cx, cy, dx, dy, ax, ay);
  const d4 = cross2D(cx, cy, dx, dy, bx, by);
  return ((d1 <= 0 && d2 >= 0) || (d1 >= 0 && d2 <= 0)) &&
         ((d3 <= 0 && d4 >= 0) || (d3 >= 0 && d4 <= 0));
}

export function roomPolygonsForScene(scene) {
  if (!scene) return [];
  if (Array.isArray(scene.ceilingFootprint) && scene.ceilingFootprint.length > 0) {
    return scene.ceilingFootprint.filter(p => Array.isArray(p) && p.length >= 3);
  }
  const traced = traceWallLoops(scene.walls || []);
  return traced.loops.map(l => l.vertices).filter(p => p.length >= 3);
}

export function roomVerticalBoundsAt(scene, x, y) {
  let zFloor = 0;
  for (const fz of scene?.floorZones || []) {
    if (fz.heightM !== null && fz.heightM !== undefined && fz.heightM >= 0 && fz.vertices?.length >= 3 && pointInPolygon2D(x, y, fz.vertices)) {
      zFloor = Math.max(zFloor, fz.heightM);
    }
  }
  let zCeil = scene?.ceilingMaterial === null ? 30 : Math.max(1, Number(scene?.heightM ?? 3));
  for (const cz of scene?.ceilingZones || []) {
    if (cz.heightM !== null && cz.heightM !== undefined && cz.heightM > 0 && cz.vertices?.length >= 3 && pointInPolygon2D(x, y, cz.vertices)) {
      zCeil = Math.min(zCeil, cz.heightM);
    }
  }
  return { zFloor, zCeil };
}

export function isPointInsideRoom(scene, polygons, x, y, z, clearanceXY = 0.08, clearanceZ = 0.05) {
  if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) return false;
  if (x < -200 + clearanceXY || x > 200 - clearanceXY || y < -200 + clearanceXY || y > 200 - clearanceXY)
    return false;
  const { zFloor, zCeil } = roomVerticalBoundsAt(scene, x, y);
  if (z < zFloor + clearanceZ || z > zCeil - clearanceZ) return false;
  if (polygons.length > 0 && !polygons.some(poly => pointInPolygon2D(x, y, poly))) return false;
  const roomH = Number(scene?.heightM ?? 3);
  for (const w of scene?.walls || []) {
    const z0 = Number.isFinite(w.zBottomM) ? w.zBottomM : 0;
    const z1 = w.zTopM === null || w.zTopM === undefined || w.zTopM >= 1e5 ? roomH : w.zTopM;
    if (z < z0 - clearanceZ || z > z1 + clearanceZ) continue;
    if (pointToSegmentDist2D(x, y, w.a[0], w.a[1], w.b[0], w.b[1]) < clearanceXY) return false;
  }
  return true;
}

// Match the processor's wall and floor clearances for a stopped-transport preview.
export function clampOffsetToRoom(scene, base, proposed, zTop) {
  const clearanceXY = 0.08;
  const clearanceZ = 0.05;
  const clampZ = ([x, y, z]) => {
    const { zFloor, zCeil } = roomVerticalBoundsAt(scene, x, y);
    const height = zCeil - zFloor;
    const safeZ = height <= 2 * clearanceZ ? (zFloor + zCeil) / 2 : clamp(z, zFloor + clearanceZ, zCeil - clearanceZ);
    return [x, y, safeZ];
  };
  const from = clampZ(base);
  const to = clampZ([
    clamp(proposed[0], -200 + clearanceXY, 200 - clearanceXY),
    clamp(proposed[1], -200 + clearanceXY, 200 - clearanceXY),
    proposed[2]
  ]);
  const polygons = roomPolygonsForScene(scene);
  const insideRoom = point => isPointInsideRoom(scene, polygons, ...point, clearanceXY, clearanceZ);
  const baseInside = insideRoom(from);
  const crossesWall = (a, b) => {
    const zMid = (a[2] + b[2]) / 2;
    if (Math.hypot(a[0] - b[0], a[1] - b[1]) < 1e-9) return false;
    return (scene.walls || []).some(wall => {
      const low = wall.zBottomM ?? 0;
      const high = wall.zTopM ?? scene.heightM ?? 3;
      return zMid >= low - clearanceZ && zMid <= high + clearanceZ &&
        segmentsIntersect2D(a[0], a[1], b[0], b[1], ...wall.a, ...wall.b);
    });
  };
  const valid = point => (!baseInside || insideRoom(point)) && !crossesWall(from, point);
  let result = to;
  if (!valid(to)) {
    let low = 0;
    let high = 1;
    for (let step = 0; step < 16; step++) {
      const mid = (low + high) / 2;
      if (valid(clampZ(from.map((v, axis) => v + (to[axis] - v) * mid)))) low = mid;
      else high = mid;
    }
    result = clampZ(from.map((v, axis) => v + (to[axis] - v) * low));
  }
  result[2] = clamp(result[2], 0, zTop);
  return result;
}
