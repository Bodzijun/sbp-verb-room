// T12: pure geometry & scale-bar helpers for Main / Edit room viewports and inspectors.
import { displayNumber } from '../bridge/parameters.js';
import { pixelsPerMetre, autoStep, stepLabel } from './snap.js';
import { gridStep } from './grid.js';

export const SCALE_BAR_SERIES = [
  0.01, 0.02, 0.05,
  0.1, 0.2, 0.5,
  1, 2, 5,
  10, 20, 50,
  100, 200, 500
];

export function formatScaleValue(metres) {
  if (!Number.isFinite(metres) || metres <= 0) return '—';
  const rounded = Number(metres.toFixed(4));
  return `${rounded} m`;
}

export function chooseScaleBar(ppm, minPx = 44, maxPx = 120) {
  if (!Number.isFinite(ppm) || ppm <= 0) return { metres: 1, pixels: 60, label: '1 m' };
  const targetPx = (minPx + maxPx) / 2;
  const inRange = SCALE_BAR_SERIES.filter(m => m * ppm >= minPx && m * ppm <= maxPx);
  const candidates = inRange.length ? inRange : SCALE_BAR_SERIES;
  let best = candidates[0], bestDiff = Math.abs(best * ppm - targetPx);
  for (const m of candidates) {
    const diff = Math.abs(m * ppm - targetPx);
    if (diff < bestDiff) { best = m; bestDiff = diff; }
  }
  return {
    metres: best,
    pixels: Math.round(best * ppm),
    label: formatScaleValue(best)
  };
}

export function activeGridStepM(bounds, camera, height) {
  const coarse = gridStep(bounds, camera.view);
  if (camera.view === '3d') return coarse;
  const fine = autoStep(camera, height);
  const ppm = pixelsPerMetre(camera, height);
  if (fine * ppm >= 6 && fine < 1) return fine;
  for (const s of SCALE_BAR_SERIES) {
    if (s * ppm >= 18) return s;
  }
  return coarse;
}

export function scaleIndicatorState(bounds, camera, height) {
  const ppm = pixelsPerMetre(camera, height);
  const bar = chooseScaleBar(ppm);
  const stepM = activeGridStepM(bounds, camera, height);
  const gridLabel = `Grid ${stepLabel(stepM)}`;
  const is3d = camera.view === '3d';
  return {
    ...bar,
    stepM,
    gridLabel,
    is3d,
    caption: is3d ? `${bar.label} (target) · ${gridLabel}` : `${bar.label} · ${gridLabel}`
  };
}

export function wallLengthM(wall) {
  if (!wall || !Array.isArray(wall.a) || !Array.isArray(wall.b)) return 0;
  return Math.hypot(wall.b[0] - wall.a[0], wall.b[1] - wall.a[1]);
}

export function wallHeightSpanM(wall, roomHeightM = 3) {
  const z0 = Number.isFinite(wall?.zBottomM) ? wall.zBottomM : 0;
  const rawTop = wall?.zTopM;
  const z1 = rawTop !== null && rawTop !== undefined && rawTop < 1e5 ? rawTop : roomHeightM;
  return { z0, z1, spanM: Math.max(0, z1 - z0), fullHeight: rawTop === null || rawTop === undefined || rawTop >= 1e5 };
}

export function formatMetres2(value) {
  if (!Number.isFinite(value)) return '—';
  return `${Number(value).toFixed(2)} m`;
}

export function formatWallLengthLabel(lengthM, angleDeg = null) {
  const lenText = formatMetres2(lengthM);
  if (angleDeg === null || !Number.isFinite(angleDeg)) return lenText;
  const norm = Math.abs(angleDeg) < 0.05 ? 0 : angleDeg;
  return `${lenText} · ${norm < 0 ? '-' : ''}${Math.abs(norm).toFixed(1)}°`;
}

export function wallSelectionSummary(scene, refs = []) {
  const walls = scene?.walls || [];
  const heightM = Number.isFinite(scene?.heightM) ? scene.heightM : 3;
  const wallRefs = refs.filter(r => r && r.kind === 'wall' && walls[r.index]);
  if (!wallRefs.length) return null;
  const wallObjs = wallRefs.map(r => walls[r.index]);
  if (refs.length === 1 && wallObjs.length === 1) {
    const lenM = wallLengthM(wallObjs[0]);
    const hSpan = wallHeightSpanM(wallObjs[0], heightM);
    const heightText = hSpan.z0 > 0
      ? `${formatMetres2(hSpan.spanM)} (${formatMetres2(hSpan.z0)}–${formatMetres2(hSpan.z1)})`
      : `${formatMetres2(hSpan.spanM)}${hSpan.fullHeight ? ' · full height' : ''}`;
    return {
      count: 1,
      lengthLabel: 'Length',
      lengthM: lenM,
      lengthText: formatMetres2(lenM),
      heightSpanM: hSpan.spanM,
      heightText
    };
  }
  const totalLenM = wallObjs.reduce((sum, w) => sum + wallLengthM(w), 0);
  const spans = wallObjs.map(w => wallHeightSpanM(w, heightM));
  const sameHeight = spans.every(h => Math.abs(h.spanM - spans[0].spanM) < 1e-4 && Math.abs(h.z0 - spans[0].z0) < 1e-4);
  const heightText = sameHeight
    ? formatMetres2(spans[0].spanM)
    : `${formatMetres2(Math.min(...spans.map(h => h.spanM)))}–${formatMetres2(Math.max(...spans.map(h => h.spanM)))} (mixed)`;
  return {
    count: wallObjs.length,
    lengthLabel: wallObjs.length > 1 ? 'Total length' : 'Wall length',
    lengthM: totalLenM,
    lengthText: `${formatMetres2(totalLenM)} · ${wallObjs.length} wall${wallObjs.length > 1 ? 's' : ''}`,
    heightSpanM: spans[0].spanM,
    heightText
  };
}


export function polygonArea2D(vertices) {
  if (!Array.isArray(vertices) || vertices.length < 3) return 0;
  let sum = 0;
  for (let i = 0; i < vertices.length; i++) {
    const a = vertices[i], b = vertices[(i + 1) % vertices.length];
    sum += a[0] * b[1] - b[0] * a[1];
  }
  return Math.abs(sum) * 0.5;
}

// Trace closed simple loops from 2D wall segments [a, b].
export function traceWallLoops(walls, eps = 1e-3) {
  const valid = (walls || []).filter(w => Array.isArray(w?.a) && Array.isArray(w?.b) && wallLengthM(w) > eps);
  if (!valid.length) return { closed: false, loops: [], openCount: 0 };
  const vertices = [];
  const findOrAdd = pt => {
    for (let i = 0; i < vertices.length; i++) {
      if (Math.hypot(vertices[i][0] - pt[0], vertices[i][1] - pt[1]) <= eps) return i;
    }
    vertices.push([pt[0], pt[1]]);
    return vertices.length - 1;
  };
  const adj = new Map();
  const edges = [];
  for (const w of valid) {
    const u = findOrAdd(w.a), v = findOrAdd(w.b);
    if (u === v) continue;
    const edgeIdx = edges.length;
    edges.push([u, v]);
    if (!adj.has(u)) adj.set(u, []);
    if (!adj.has(v)) adj.set(v, []);
    adj.get(u).push({ to: v, edge: edgeIdx });
    adj.get(v).push({ to: u, edge: edgeIdx });
  }
  if (!edges.length) return { closed: false, loops: [], openCount: valid.length };

  // Check if all vertices have even degree >= 2 and trace simple cycles
  const visitedEdges = new Set();
  const loops = [];
  let openCount = 0;

  for (let startEdge = 0; startEdge < edges.length; startEdge++) {
    if (visitedEdges.has(startEdge)) continue;
    // Collect connected component of edges/vertices
    const compVertices = new Set();
    const compEdges = new Set();
    const queue = [edges[startEdge][0]];
    while (queue.length) {
      const curr = queue.pop();
      if (compVertices.has(curr)) continue;
      compVertices.add(curr);
      for (const { to, edge } of adj.get(curr) || []) {
        compEdges.add(edge);
        if (!compVertices.has(to)) queue.push(to);
      }
    }
    for (const e of compEdges) visitedEdges.add(e);
    const isSimpleCycle = compVertices.size >= 3 && compEdges.size === compVertices.size && [...compVertices].every(v => (adj.get(v)?.length || 0) === 2);
    if (isSimpleCycle) {
      const ring = [];
      let prevEdge = -1, curr = edges[startEdge][0];
      const startV = curr;
      for (let step = 0; step < compVertices.size; step++) {
        ring.push(vertices[curr]);
        const next = (adj.get(curr) || []).find(n => n.edge !== prevEdge);
        if (!next) break;
        prevEdge = next.edge;
        curr = next.to;
        if (curr === startV) break;
      }
      if (ring.length >= 3 && curr === startV) {
        loops.push({ vertices: ring, areaM2: polygonArea2D(ring) });
      } else {
        openCount += compEdges.size;
      }
    } else {
      openCount += compEdges.size;
    }
  }

  return {
    closed: loops.length > 0 && openCount === 0,
    loops,
    openCount
  };
}

export function computeRoomMetrics(scene) {
  if (!scene) return null;
  const walls = scene.walls || [];
  const floorZones = scene.floorZones || [];
  const ceilingZones = scene.ceilingZones || [];
  const heightM = Number.isFinite(scene.heightM) ? scene.heightM : 3;

  // Gather XY points from walls and zones for bounding box
  const xyPoints = [
    ...walls.flatMap(w => [w.a, w.b]),
    ...floorZones.flatMap(z => z.vertices || []),
    ...ceilingZones.flatMap(z => z.vertices || [])
  ].filter(p => Array.isArray(p) && Number.isFinite(p[0]) && Number.isFinite(p[1]));

  let lengthM = 0, widthM = 0;
  if (xyPoints.length >= 2) {
    const xs = xyPoints.map(p => p[0]), ys = xyPoints.map(p => p[1]);
    const spanX = Math.max(...xs) - Math.min(...xs);
    const spanY = Math.max(...ys) - Math.min(...ys);
    lengthM = Math.max(spanX, spanY);
    widthM = Math.min(spanX, spanY);
  }

  const bboxAreaM2 = lengthM * widthM;
  const traced = traceWallLoops(walls);
  let areaM2 = 0, closedOutline = false, areaApproximate = false;

  if (traced.loops.length > 0 && traced.openCount === 0) {
    // Sort loops by area descending; if smaller loops are inside the outer loop or disjoint,
    // use the outer loop area (or sum disjoint loops). In room authoring, multiple closed loops
    // are either disjoint rooms or inner pillars; checking point-in-polygon distinguishes them.
    const sorted = traced.loops.slice().sort((a, b) => b.areaM2 - a.areaM2);
    let total = sorted[0].areaM2;
    for (let i = 1; i < sorted.length; i++) {
      const sample = sorted[i].vertices[0];
      const insideOuter = pointInPolygon2D(sample, sorted[0].vertices);
      total += insideOuter ? -sorted[i].areaM2 : sorted[i].areaM2;
    }
    areaM2 = Math.max(0, total);
    closedOutline = true;
  } else if (traced.loops.length > 0) {
    // Mixed closed loop + interior open partitions: use the largest closed outer loop if it encloses the rest, else bbox
    const largest = traced.loops.slice().sort((a, b) => b.areaM2 - a.areaM2)[0];
    areaM2 = largest.areaM2 > 0 ? largest.areaM2 : bboxAreaM2;
    closedOutline = false;
    areaApproximate = true;
  } else {
    areaM2 = bboxAreaM2;
    closedOutline = false;
    areaApproximate = xyPoints.length > 0;
  }

  const openSky = scene.ceilingMaterial === null;
  const volumeM3 = areaM2 * heightM;

  const fmtDim = v => displayNumber(v, '').replace(/\s+$/, '');
  const dimensionsText = `${fmtDim(lengthM)} × ${fmtDim(widthM)} × ${fmtDim(heightM)} m`;
  const fmtArea = v => `${Number(v.toFixed(v >= 100 ? 1 : 2))} m²`;
  const fmtVol = v => `${Number(v.toFixed(v >= 100 ? 1 : 2))} m³`;

  const areaText = xyPoints.length === 0 ? '0 m²' : closedOutline ? fmtArea(areaM2) : `≈ ${fmtArea(areaM2)} (bounding box)`;
  const volumeText = xyPoints.length === 0 ? '0 m³' : `${ !closedOutline || openSky ? '≈ ' : '' }${fmtVol(volumeM3)}${openSky ? ' (open sky)' : !closedOutline ? ' (bounding box)' : ''}`;

  return {
    lengthM,
    widthM,
    heightM,
    areaM2,
    volumeM3,
    closedOutline,
    areaApproximate,
    openSky,
    dimensionsText,
    areaText,
    volumeText
  };
}

function pointInPolygon2D(p, poly) {
  let inside = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const a = poly[i], b = poly[j];
    if ((a[1] > p[1]) !== (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) inside = !inside;
  }
  return inside;
}
