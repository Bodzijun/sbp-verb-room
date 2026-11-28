import { compilePath, withPathPoses } from './movement-path.js';
import { isPointInsideRoom, roomPolygonsForScene, roomVerticalBoundsAt, segmentsIntersect2D } from './room-bounds.js';

export { isPointInsideRoom, roomPolygonsForScene, roomVerticalBoundsAt } from './room-bounds.js';

export const PATH_SHAPE_OPTIONS = [
  ['', 'Shape…'],
  ['circle', 'Circle'],
  ['ellipse', 'Ellipse'],
  ['figure8', 'Figure-8 (∞)'],
  ['square', 'Square'],
  ['rectangle', 'Rectangle'],
  ['triangle', 'Triangle'],
  ['star', 'Star (5-point)'],
  ['spiral', 'Spiral']
];

export const PATH_SHAPE_META = {
  circle: {
    label: 'Circle',
    smooth: true,
    hasRadiusY: false,
    radiusXLabel: 'Radius',
    hasNodes: true,
    minNodes: 12,
    maxNodes: 64,
    defaultNodes: 16
  },
  ellipse: {
    label: 'Ellipse',
    smooth: true,
    hasRadiusY: true,
    radiusXLabel: 'Semi-axis X',
    radiusYLabel: 'Semi-axis Y',
    defaultRadiusYRatio: 0.6,
    hasNodes: true,
    minNodes: 12,
    maxNodes: 64,
    defaultNodes: 16
  },
  figure8: {
    label: 'Figure-8 (∞)',
    smooth: true,
    hasRadiusY: true,
    radiusXLabel: 'Half-length X',
    radiusYLabel: 'Lobe width Y',
    defaultRadiusYRatio: 0.5,
    hasNodes: true,
    minNodes: 12,
    maxNodes: 64,
    defaultNodes: 16,
    evenNodes: true
  },
  square: {
    label: 'Square',
    smooth: false,
    hasRadiusY: false,
    radiusXLabel: 'Half-side',
    hasNodes: false,
    defaultNodes: 4
  },
  rectangle: {
    label: 'Rectangle',
    smooth: false,
    hasRadiusY: true,
    radiusXLabel: 'Half-width X',
    radiusYLabel: 'Half-height Y',
    defaultRadiusYRatio: 0.6,
    hasNodes: false,
    defaultNodes: 4
  },
  triangle: {
    label: 'Triangle',
    smooth: false,
    hasRadiusY: false,
    radiusXLabel: 'Radius',
    hasNodes: false,
    defaultNodes: 3
  },
  star: {
    label: 'Star (5-point)',
    smooth: false,
    hasRadiusY: true,
    radiusXLabel: 'Outer radius',
    radiusYLabel: 'Inner radius',
    defaultRadiusYRatio: 0.3819660112501051,
    hasNodes: false,
    defaultNodes: 10
  },
  spiral: {
    label: 'Spiral',
    smooth: true,
    hasRadiusY: true,
    radiusXLabel: 'Outer radius',
    radiusYLabel: 'Inner radius',
    defaultRadiusYRatio: 0.25,
    hasNodes: true,
    minNodes: 16,
    maxNodes: 64,
    defaultNodes: 24,
    evenNodes: true,
    isSpiral: true
  }
};

const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const r4 = v => Number(v.toFixed(4));
const dist3 = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
const mix3 = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];

export function offsetFigure8DefaultCentre(initial, lx, ly, scene = null, minDistanceM = 1.5, preferredX = NaN, preferredY = NaN) {
  const p = { ...initial, kind: 'figure8' };
  const curDist = Math.hypot(p.cx - lx, p.cy - ly);
  if (curDist >= minDistanceM - 1e-6) {
    if (scene) {
      const fit = generatePathShapeForRoom(p, scene, true);
      if (Math.hypot(fit.fittedParams.cx - lx, fit.fittedParams.cy - ly) >= minDistanceM - 1e-6) {
        return fit.fittedParams;
      }
    } else {
      return p;
    }
  }

  const dirs = [];
  const pushDir = (dx, dy) => {
    const len = Math.hypot(dx, dy);
    if (len > 1e-6) dirs.push([dx / len, dy / len]);
  };
  if (Number.isFinite(preferredX) && Number.isFinite(preferredY)) pushDir(preferredX - lx, preferredY - ly);
  pushDir(p.cx - lx, p.cy - ly);
  if (scene?.walls?.length > 0) {
    const xs = scene.walls.flatMap(w => [w.a[0], w.b[0]]);
    const ys = scene.walls.flatMap(w => [w.a[1], w.b[1]]);
    pushDir(0.5 * (Math.min(...xs) + Math.max(...xs)) - lx, 0.5 * (Math.min(...ys) + Math.max(...ys)) - ly);
  }
  pushDir(0, 1);
  pushDir(0, -1);
  pushDir(1, 0);
  pushDir(-1, 0);
  pushDir(0.70710678, 0.70710678);
  pushDir(-0.70710678, 0.70710678);
  pushDir(0.70710678, -0.70710678);
  pushDir(-0.70710678, -0.70710678);

  const targetDist = Math.max(minDistanceM, 1.6);
  if (!scene) {
    const d = dirs[0] || [0, 1];
    return {
      ...p,
      cx: r4(lx + d[0] * targetDist),
      cy: r4(ly + d[1] * targetDist)
    };
  }

  const polygons = roomPolygonsForScene(scene);
  let best = p;
  let bestScore = -1e30;
  for (const dist of [targetDist, minDistanceM, 1.8, 2.0]) {
    for (const [ux, uy] of dirs) {
      const cand = {
        ...p,
        cx: r4(lx + ux * dist),
        cy: r4(ly + uy * dist)
      };
      if (!isPointInsideRoom(scene, polygons, cand.cx, cand.cy, cand.z, 0.20, 0.05)) continue;
      const fit = generatePathShapeForRoom(cand, scene, true);
      const actualDist = Math.hypot(fit.fittedParams.cx - lx, fit.fittedParams.cy - ly);
      if (actualDist < minDistanceM - 1e-4) continue;
      const score = fit.appliedScale * 10 - Math.abs(actualDist - targetDist);
      if (score > bestScore) {
        bestScore = score;
        best = fit.fittedParams;
      }
    }
    if (bestScore > -1e20) break;
  }
  if (bestScore > -1e20) return best;

  let maxDist = -1;
  for (const [ux, uy] of dirs) {
    const cand = {
      ...p,
      cx: r4(lx + ux * targetDist),
      cy: r4(ly + uy * targetDist)
    };
    const fit = generatePathShapeForRoom(cand, scene, true);
    const actualDist = Math.hypot(fit.fittedParams.cx - lx, fit.fittedParams.cy - ly);
    if (actualDist > maxDist) {
      maxDist = actualDist;
      best = fit.fittedParams;
    }
  }
  return best;
}

export function defaultShapeParamsForObject(state, object, kind = 'circle', prev = null) {
  const meta = PATH_SHAPE_META[kind] || PATH_SHAPE_META.circle;
  const vals = withPathPoses(state).parameters?.values || {};
  const rawVals = state?.parameters?.values || vals;
  const prefix = object === 32 ? 'l' : `s${object + 1}`;
  const existing = state?.document?.paths?.[object];
  const baseObjX = Number(rawVals[prefix + 'x'] ?? vals[prefix + 'x'] ?? 0);
  const baseObjY = Number(rawVals[prefix + 'y'] ?? vals[prefix + 'y'] ?? 0);
  let cx = Number(vals[prefix + 'x'] ?? 0);
  let cy = Number(vals[prefix + 'y'] ?? 0);
  let z = Number(vals[prefix + 'z'] ?? 1.5);
  let zEnd = z;
  if (existing?.points?.length >= 2) {
    const xs = existing.points.map(q => q[0]);
    const ys = existing.points.map(q => q[1]);
    cx = 0.5 * (Math.min(...xs) + Math.max(...xs));
    cy = 0.5 * (Math.min(...ys) + Math.max(...ys));
    z = existing.points[0][2];
    zEnd = existing.points[existing.points.length - 1][2];
  }
  const radiusX = Number.isFinite(prev?.radiusX) && prev.radiusX >= 0.05 ? prev.radiusX : 2.0;
  const ratio = meta.defaultRadiusYRatio ?? 0.6;
  const radiusY = r4(Math.max(0.05, radiusX * ratio));
  const baseNodes = (prev?.kind === kind && Number.isFinite(prev?.nodes)) ? prev.nodes : meta.defaultNodes;
  const nodes = meta.hasNodes
    ? clamp(
        meta.evenNodes && baseNodes % 2 !== 0
          ? baseNodes + 1
          : baseNodes,
        meta.minNodes,
        meta.maxNodes
      )
    : meta.defaultNodes;
  const resolvedZ = r4(Number.isFinite(prev?.z) ? prev.z : z);
  const resolvedZEnd = r4(Number.isFinite(prev?.zEnd) ? prev.zEnd : (kind === 'spiral' && existing?.points?.length >= 2 && !Number.isFinite(prev?.z) ? zEnd : resolvedZ));
  const out = {
    kind,
    cx: r4(Number.isFinite(prev?.cx) ? prev.cx : cx),
    cy: r4(Number.isFinite(prev?.cy) ? prev.cy : cy),
    z: resolvedZ,
    zEnd: resolvedZEnd,
    radiusX: r4(radiusX),
    radiusY,
    rotationDeg: Number.isFinite(prev?.rotationDeg) ? prev.rotationDeg : 0,
    startPhaseDeg: Number.isFinite(prev?.startPhaseDeg) ? prev.startPhaseDeg : 0,
    clockwise: typeof prev?.clockwise === 'boolean' ? prev.clockwise : false,
    nodes,
    spiralTurns: clamp(Math.round(prev?.spiralTurns ?? 2), 1, 6),
    spiralOutIn: typeof prev?.spiralOutIn === 'boolean' ? prev.spiralOutIn : true
  };
  if (kind === 'figure8') {
    const lx = Number(vals.lx ?? rawVals.lx ?? 0);
    const ly = Number(vals.ly ?? rawVals.ly ?? 0);
    const offset = offsetFigure8DefaultCentre(out, lx, ly, state?.document?.scene || null, 1.5, baseObjX, baseObjY);
    out.cx = r4(offset.cx);
    out.cy = r4(offset.cy);
  }
  return out;
}

export function centreShapeOnListener(state, params) {
  const vals = withPathPoses(state).parameters?.values || {};
  return {
    ...params,
    cx: r4(Number(vals.lx ?? 0)),
    cy: r4(Number(vals.ly ?? 0))
  };
}

function applyPolygonStartPhase(corners, startPhaseDeg) {
  const m = corners.length;
  if (m < 3) return corners;
  let norm = (startPhaseDeg / 360) - Math.floor(startPhaseDeg / 360);
  if (!Number.isFinite(norm) || norm < 0 || norm >= 1) norm = 0;
  if (norm < 1e-7 || 1 - norm < 1e-7) return corners;

  const edgeLen = [];
  let perim = 0;
  for (let i = 0; i < m; i++) {
    const l = dist3(corners[i], corners[(i + 1) % m]);
    edgeLen.push(l);
    perim += l;
  }
  if (perim <= 1e-9) return corners;

  const targetDist = norm * perim;
  let acc = 0, edgeIdx = 0, localT = 0;
  for (let i = 0; i < m; i++) {
    const nextAcc = acc + edgeLen[i];
    if (targetDist <= nextAcc || i + 1 === m) {
      edgeIdx = i;
      localT = edgeLen[i] > 1e-12 ? clamp((targetDist - acc) / edgeLen[i], 0, 1) : 0;
      break;
    }
    acc = nextAcc;
  }

  if (localT < 1e-5 || 1 - localT < 1e-5) {
    const startCorner = (1 - localT < 1e-5) ? ((edgeIdx + 1) % m) : edgeIdx;
    return Array.from({ length: m }, (_, k) => corners[(startCorner + k) % m]);
  }

  const out = [mix3(corners[edgeIdx], corners[(edgeIdx + 1) % m], localT)];
  for (let k = 1; k <= m; k++) out.push(corners[(edgeIdx + k) % m]);
  return out;
}

export function generateRawPathShape(p) {
  const kind = p?.kind || 'circle';
  const z = clamp(Number.isFinite(p?.z) ? p.z : 1.5, 0, 30);
  const cx = clamp(Number.isFinite(p?.cx) ? p.cx : 0, -200, 200);
  const cy = clamp(Number.isFinite(p?.cy) ? p.cy : 0, -200, 200);
  const dir = p?.clockwise ? -1 : 1;
  const rot = (Number.isFinite(p?.rotationDeg) ? p.rotationDeg : 0) * Math.PI / 180;
  const cr = Math.cos(rot), sr = Math.sin(rot);
  const phaseDeg = Number.isFinite(p?.startPhaseDeg) ? p.startPhaseDeg : 0;
  const phaseRad = phaseDeg * Math.PI / 180;

  let smooth = true;
  let points = [];

  if (kind === 'circle') {
    smooth = true;
    const n = clamp(Math.round(p?.nodes ?? 16), 12, 64);
    const r = Math.max(0.05, Number.isFinite(p?.radiusX) ? p.radiusX : 2.0);
    const a0 = rot + phaseRad;
    for (let k = 0; k < n; k++) {
      const a = a0 + dir * (2 * Math.PI * k / n);
      points.push([cx + r * Math.cos(a), cy + r * Math.sin(a), z]);
    }
  } else if (kind === 'ellipse') {
    smooth = true;
    const n = clamp(Math.round(p?.nodes ?? 16), 12, 64);
    const rx = Math.max(0.05, Number.isFinite(p?.radiusX) ? p.radiusX : 2.5);
    const ry = Math.max(0.05, Number.isFinite(p?.radiusY) && p.radiusY > 0 ? p.radiusY : 0.6 * rx);
    for (let k = 0; k < n; k++) {
      const phi = phaseRad + dir * (2 * Math.PI * k / n);
      const u = rx * Math.cos(phi), v = ry * Math.sin(phi);
      points.push([cx + u * cr - v * sr, cy + u * sr + v * cr, z]);
    }
  } else if (kind === 'figure8') {
    // Lemniscate of Gerono: u = a * sin(phi), v = b * sin(2 * phi)
    smooth = true;
    let n = clamp(Math.round(p?.nodes ?? 16), 12, 64);
    if (n % 2 !== 0) n = n < 64 ? n + 1 : 64;
    const rx = Math.max(0.05, Number.isFinite(p?.radiusX) ? p.radiusX : 2.0);
    const ry = Math.max(0.05, Number.isFinite(p?.radiusY) && p.radiusY > 0 ? p.radiusY : 0.5 * rx);
    for (let k = 0; k < n; k++) {
      const phi = phaseRad + dir * (2 * Math.PI * k / n);
      const u = rx * Math.sin(phi), v = ry * Math.sin(2 * phi);
      points.push([cx + u * cr - v * sr, cy + u * sr + v * cr, z]);
    }
  } else if (kind === 'square' || kind === 'rectangle') {
    smooth = false;
    const rx = Math.max(0.05, Number.isFinite(p?.radiusX) ? p.radiusX : 2.0);
    const ry = kind === 'square'
      ? rx
      : Math.max(0.05, Number.isFinite(p?.radiusY) && p.radiusY > 0 ? p.radiusY : 0.6 * rx);
    const local = p?.clockwise
      ? [[rx, ry], [rx, -ry], [-rx, -ry], [-rx, ry]]
      : [[rx, ry], [-rx, ry], [-rx, -ry], [rx, -ry]];
    const corners = local.map(([u, v]) => [cx + u * cr - v * sr, cy + u * sr + v * cr, z]);
    points = applyPolygonStartPhase(corners, phaseDeg);
  } else if (kind === 'triangle') {
    smooth = false;
    const r = Math.max(0.05, Number.isFinite(p?.radiusX) ? p.radiusX : 2.0);
    const corners = [0, 1, 2].map(k => {
      const a = rot + dir * (2 * Math.PI * k / 3);
      return [cx + r * Math.cos(a), cy + r * Math.sin(a), z];
    });
    points = applyPolygonStartPhase(corners, phaseDeg);
  } else if (kind === 'star') {
    smooth = false;
    const rOut = Math.max(0.05, Number.isFinite(p?.radiusX) ? p.radiusX : 2.0);
    const rIn = Number.isFinite(p?.radiusY) && p.radiusY > 0
      ? clamp(p.radiusY, 0.05 * rOut, 0.95 * rOut)
      : 0.3819660112501051 * rOut;
    const corners = Array.from({ length: 10 }, (_, k) => {
      const rad = k % 2 === 0 ? rOut : rIn;
      const a = rot + dir * (Math.PI * k / 5);
      return [cx + rad * Math.cos(a), cy + rad * Math.sin(a), z];
    });
    points = applyPolygonStartPhase(corners, phaseDeg);
  } else if (kind === 'spiral') {
    smooth = true;
    let n = clamp(Math.round(p?.nodes ?? 24), 16, 64);
    if (n % 2 !== 0) n = n < 64 ? n + 1 : 64;
    const turns = clamp(Math.round(p?.spiralTurns ?? 2), 1, 6);
    const rOut = Math.max(0.1, Number.isFinite(p?.radiusX) ? p.radiusX : 2.0);
    const rIn = Number.isFinite(p?.radiusY) && p.radiusY > 0
      ? clamp(p.radiusY, 0.02, 0.92 * rOut)
      : 0.25 * rOut;
    const zStart = z;
    const zEndVal = clamp(Number.isFinite(p?.zEnd) ? p.zEnd : zStart, 0, 30);
    const outIn = p?.spiralOutIn !== false;
    let phase0 = (phaseDeg / 360) - Math.floor(phaseDeg / 360);
    if (!Number.isFinite(phase0) || phase0 < 0 || phase0 >= 1) phase0 = 0;
    for (let k = 0; k < n; k++) {
      let s = phase0 + k / n;
      s -= Math.floor(s);
      const w = 0.5 * (1 - Math.cos(2 * Math.PI * s));
      const rad = outIn ? (rIn + (rOut - rIn) * w) : (rOut - (rOut - rIn) * w);
      const a = rot + dir * (2 * Math.PI * turns * s);
      const tZ = n > 1 ? (k / (n - 1)) : 0;
      const zk = zStart + (zEndVal - zStart) * tZ;
      points.push([cx + rad * Math.cos(a), cy + rad * Math.sin(a), zk]);
    }
  }

  points = points.map(pt => [
    clamp(pt[0], -200, 200),
    clamp(pt[1], -200, 200),
    clamp(pt[2], 0, 30)
  ]);

  return {
    points,
    enabled: true,
    closed: true,
    smooth
  };
}

export function isCompiledPathInsideRoom(scene, polygons, mp, clearanceXY = 0.08, clearanceZ = 0.05) {
  if (!mp?.points || mp.points.length < 2) return false;
  for (const q of mp.points) {
    if (!isPointInsideRoom(scene, polygons, q[0], q[1], q[2], clearanceXY, clearanceZ)) return false;
  }
  const compiled = compilePath(mp);
  const roomH = Number(scene?.heightM ?? 3);
  for (let i = 0; i < compiled.points.length; i++) {
    const q = compiled.points[i];
    if (!isPointInsideRoom(scene, polygons, q[0], q[1], q[2], clearanceXY, clearanceZ)) return false;
    if (i > 0) {
      const prev = compiled.points[i - 1];
      const zMid = 0.5 * (prev[2] + q[2]);
      for (const w of scene?.walls || []) {
        const z0 = Number.isFinite(w.zBottomM) ? w.zBottomM : 0;
        const z1 = w.zTopM === null || w.zTopM === undefined || w.zTopM >= 1e5 ? roomH : w.zTopM;
        if (zMid < z0 - clearanceZ || zMid > z1 + clearanceZ) continue;
        if (segmentsIntersect2D(prev[0], prev[1], q[0], q[1], w.a[0], w.a[1], w.b[0], w.b[1])) return false;
      }
    }
  }
  return true;
}

export function generatePathShapeForRoom(params, scene = null, clampToRoom = true, clearanceXY = 0.08, clearanceZ = 0.05) {
  const rawPath = generateRawPathShape(params);
  if (!scene) {
    return { path: rawPath, fittedParams: { ...params }, outsideRoom: false, clamped: false, appliedScale: 1 };
  }
  const polygons = roomPolygonsForScene(scene);
  if (isCompiledPathInsideRoom(scene, polygons, rawPath, clearanceXY, clearanceZ)) {
    return { path: rawPath, fittedParams: { ...params }, outsideRoom: false, clamped: false, appliedScale: 1 };
  }

  // Always compute the fitted parameters so the UI can show the exact fitted scale & offer "Fit to room"
  const fit = { ...params };
  let { zFloor, zCeil } = roomVerticalBoundsAt(scene, fit.cx, fit.cy);
  if (zCeil - zFloor <= 2 * clearanceZ) {
    fit.z = 0.5 * (zFloor + zCeil);
    if (Number.isFinite(fit.zEnd)) fit.zEnd = fit.z;
  } else {
    fit.z = clamp(fit.z, zFloor + clearanceZ, zCeil - clearanceZ);
    if (Number.isFinite(fit.zEnd)) fit.zEnd = clamp(fit.zEnd, zFloor + clearanceZ, zCeil - clearanceZ);
  }

  const centreClearance = Math.max(0.20, clearanceXY * 2);
  if (!isPointInsideRoom(scene, polygons, fit.cx, fit.cy, fit.z, centreClearance, clearanceZ)) {
    let anchorX = fit.cx, anchorY = fit.cy, foundAnchor = false;
    if (scene.home && isPointInsideRoom(scene, polygons, scene.home.x, scene.home.y, fit.z, centreClearance, clearanceZ)) {
      anchorX = scene.home.x; anchorY = scene.home.y; foundAnchor = true;
    }
    if (!foundAnchor && polygons.length > 0 && polygons[0].length > 0) {
      let sx = 0, sy = 0;
      for (const pt of polygons[0]) { sx += pt[0]; sy += pt[1]; }
      const mx = sx / polygons[0].length, my = sy / polygons[0].length;
      if (isPointInsideRoom(scene, polygons, mx, my, fit.z, centreClearance, clearanceZ)) {
        anchorX = mx; anchorY = my; foundAnchor = true;
      }
    }
    if (!foundAnchor && scene.walls?.length > 0) {
      const xs = scene.walls.flatMap(w => [w.a[0], w.b[0]]);
      const ys = scene.walls.flatMap(w => [w.a[1], w.b[1]]);
      const mx = 0.5 * (Math.min(...xs) + Math.max(...xs));
      const my = 0.5 * (Math.min(...ys) + Math.max(...ys));
      if (isPointInsideRoom(scene, polygons, mx, my, fit.z, clearanceXY, clearanceZ)) {
        anchorX = mx; anchorY = my; foundAnchor = true;
      }
    }
    if (foundAnchor) {
      let lo = 0, hi = 1;
      for (let it = 0; it < 20; it++) {
        const mid = 0.5 * (lo + hi);
        const tx = anchorX + (fit.cx - anchorX) * mid;
        const ty = anchorY + (fit.cy - anchorY) * mid;
        if (isPointInsideRoom(scene, polygons, tx, ty, fit.z, centreClearance, clearanceZ)) lo = mid;
        else hi = mid;
      }
      fit.cx = r4(anchorX + (fit.cx - anchorX) * lo);
      fit.cy = r4(anchorY + (fit.cy - anchorY) * lo);
      ({ zFloor, zCeil } = roomVerticalBoundsAt(scene, fit.cx, fit.cy));
      if (zCeil - zFloor > 2 * clearanceZ) {
        fit.z = r4(clamp(fit.z, zFloor + clearanceZ, zCeil - clearanceZ));
        if (Number.isFinite(fit.zEnd)) fit.zEnd = r4(clamp(fit.zEnd, zFloor + clearanceZ, zCeil - clearanceZ));
      }
    }
  }

  const meta = PATH_SHAPE_META[params.kind] || PATH_SHAPE_META.circle;
  const baseRx = Math.max(0.05, Number.isFinite(params.radiusX) ? params.radiusX : 2.0);
  const baseRy = Number.isFinite(params.radiusY) && params.radiusY > 0
    ? params.radiusY
    : baseRx * (meta.defaultRadiusYRatio ?? 1);

  const makeScaled = s => ({
    ...fit,
    radiusX: r4(Math.max(0.05, baseRx * s)),
    radiusY: r4(Math.max(0.02, baseRy * s))
  });

  let appliedScale = 1;
  let fittedParams = makeScaled(1);
  if (!isCompiledPathInsideRoom(scene, polygons, generateRawPathShape(fittedParams), clearanceXY, clearanceZ)) {
    let lo = 0.02, hi = 1.0;
    for (let it = 0; it < 24; it++) {
      const mid = 0.5 * (lo + hi);
      if (isCompiledPathInsideRoom(scene, polygons, generateRawPathShape(makeScaled(mid)), clearanceXY, clearanceZ)) lo = mid;
      else hi = mid;
    }
    appliedScale = lo;
    fittedParams = makeScaled(lo);
  }
  const fittedPath = generateRawPathShape(fittedParams);

  return {
    path: clampToRoom ? fittedPath : rawPath,
    rawPath,
    fittedPath,
    fittedParams,
    outsideRoom: true,
    clamped: clampToRoom,
    appliedScale
  };
}

export function measureCircleRadialDeviation(points, cx, cy, radius) {
  const compiled = compilePath({ points, closed: true, smooth: true });
  let maxAbsErrorM = 0;
  for (const q of compiled.points) {
    const r = Math.hypot(q[0] - cx, q[1] - cy);
    maxAbsErrorM = Math.max(maxAbsErrorM, Math.abs(r - radius));
  }
  return {
    maxAbsErrorM,
    maxRelErrorPct: radius > 0 ? (maxAbsErrorM / radius) * 100 : 0
  };
}
