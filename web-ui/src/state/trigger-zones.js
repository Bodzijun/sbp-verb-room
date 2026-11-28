// V04c trigger zones: named areas of the room (box or polygon in plan, between zMin and zMax). Zones are part of the
// scene document (document.zones); the processor evaluates occupancy per control period with exit hysteresis and a
// swept test. This module mirrors the plugin's TriggerZones.h geometry for the editor (drawing, validation, mock and
// stateless previews). Ids are stable integers 1..9999; list order = Zone index priority.
export const ZONE_MAX = 16;
export const ZONE_MAX_VERTS = 32;
export const ZONE_DEFAULT_HYSTERESIS = 0.05;
export const ZONE_MAX_EDGE = 10;
export const ZONE_MAX_HYSTERESIS = 2;
export const ZONE_FAR = 1000;
export const ZONE_OBJECT_LISTENER = 32;
// Distinct, readable on dark and light backgrounds; used when a zone has no explicit colour.
export const ZONE_PALETTE = ['#22c55e', '#38bdf8', '#f59e0b', '#e879f9', '#f97316', '#a3e635', '#2dd4bf', '#f472b6'];

export const zoneColour = (zone, index = 0) => (typeof zone?.colour === 'string' && /^#[0-9a-f]{6}$/i.test(zone.colour)) ? zone.colour.toLowerCase() : ZONE_PALETTE[((zone?.id ?? index + 1) - 1) % ZONE_PALETTE.length];
export const zoneList = state => Array.isArray(state?.document?.zones) ? state.document.zones : [];
export const zoneById = (state, id) => zoneList(state).find(z => z.id === id) || null;
export const zoneLabel = zone => zone ? `${zone.name || `Zone ${zone.id}`}` : 'Missing zone';

// Plan outline (counter-clockwise not required). A box stores two opposite corners.
export function zoneOutline(zone) {
  const p = zone?.points || [];
  if (zone?.kind === 'box') {
    if (p.length < 2) return [];
    const [a, b] = p, x0 = Math.min(a[0], b[0]), x1 = Math.max(a[0], b[0]), y0 = Math.min(a[1], b[1]), y1 = Math.max(a[1], b[1]);
    return [[x0, y0], [x1, y0], [x1, y1], [x0, y1]];
  }
  return p.map(q => [q[0], q[1]]);
}

function cross(o, a, b) { return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]); }
function onSeg(p, q, r) { return Math.min(p[0], r[0]) - 1e-12 <= q[0] && q[0] <= Math.max(p[0], r[0]) + 1e-12 && Math.min(p[1], r[1]) - 1e-12 <= q[1] && q[1] <= Math.max(p[1], r[1]) + 1e-12; }
export function segmentsCross(p1, p2, p3, p4) {
  const d1 = cross(p3, p4, p1), d2 = cross(p3, p4, p2), d3 = cross(p1, p2, p3), d4 = cross(p1, p2, p4);
  if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) return true;
  const eps = 1e-12;
  return (Math.abs(d1) < eps && onSeg(p3, p1, p4)) || (Math.abs(d2) < eps && onSeg(p3, p2, p4)) || (Math.abs(d3) < eps && onSeg(p1, p3, p2)) || (Math.abs(d4) < eps && onSeg(p1, p4, p2));
}
export function polygonArea(pts) { let a = 0; for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) a += pts[j][0] * pts[i][1] - pts[i][0] * pts[j][1]; return a / 2; }
// Simple polygon: >= 3 vertices, non-zero area, no two non-adjacent edges touching.
export function polygonSimple(pts) {
  const n = pts.length;
  if (n < 3 || Math.abs(polygonArea(pts)) < 1e-6) return false;
  for (let i = 0; i < n; i++) for (let j = i + 1; j < n; j++) {
    if (j === i + 1 || (i === 0 && j === n - 1)) continue;
    if (segmentsCross(pts[i], pts[(i + 1) % n], pts[j], pts[(j + 1) % n])) return false;
  }
  return true;
}
// Returns null when valid, else a short message (same wording as the plugin where it matters).
export function zoneProblem(zone) {
  if (!zone) return 'Missing zone';
  const pts = zone.points || [];
  if (zone.kind === 'box') {
    if (pts.length !== 2) return 'A box zone needs two opposite corners';
    if (Math.abs(pts[0][0] - pts[1][0]) < 0.01 || Math.abs(pts[0][1] - pts[1][1]) < 0.01) return 'A box zone must be at least 1 cm in each direction (inside the room)';
  } else {
    if (pts.length < 3 || pts.length > ZONE_MAX_VERTS) return 'A polygon zone needs 3 to 32 vertices';
    if (!polygonSimple(pts)) return 'The zone outline crosses itself (or has zero area); a zone must be a simple polygon';
  }
  if (!(zone.zMax - zone.zMin >= 0.01)) return 'zMax must be above zMin (inside floor..ceiling)';
  if (zone.edge < 0 || zone.edge > ZONE_MAX_EDGE) return 'edge is out of range';
  if (zone.hysteresis < 0 || zone.hysteresis > ZONE_MAX_HYSTERESIS) return 'hysteresis is out of range';
  return null;
}

export function pointInPolygon(p, poly) {
  let yes = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const a = poly[i], b = poly[j];
    if ((a[1] > p[1]) !== (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) yes = !yes;
  }
  return yes;
}
function segDist(p, a, b) {
  const dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
  const t = l2 > 0 ? Math.max(0, Math.min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2)) : 0;
  return Math.hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy);
}
// Signed plan distance: negative inside (distance to the nearest edge).
export function planSignedDistance(p, outline) {
  let best = Infinity;
  for (let i = 0, j = outline.length - 1; i < outline.length; j = i++) best = Math.min(best, segDist(p, outline[j], outline[i]));
  return pointInPolygon(p, outline) ? -best : best;
}
// Signed 3D distance to the prism (outline x [zMin, zMax]); negative inside (distance to the nearest face).
export function zoneSignedDistance(zone, pos, outline = zoneOutline(zone)) {
  if (!outline.length) return ZONE_FAR;
  const dp = planSignedDistance(pos, outline), lo = Math.min(zone.zMin, zone.zMax), hi = Math.max(zone.zMin, zone.zMax);
  const dz = Math.max(lo - pos[2], pos[2] - hi);
  if (dp <= 0 && dz <= 0) return Math.max(dp, dz);
  return Math.hypot(Math.max(dp, 0), Math.max(dz, 0));
}
// Soft edge: 0 at the boundary, 1 once `edge` metres inside (smoothstep); hard edge = inside ? 1 : 0.
export function zoneSoftValue(zone, d) {
  if (!(zone.edge > 0)) return d <= 0 ? 1 : 0;
  const t = Math.max(0, Math.min(1, -d / zone.edge));
  return t * t * (3 - 2 * t);
}
export const zoneEnabled = z => z && z.enabled !== false;

// Stateless occupancy for previews and the mock (no hysteresis / sweep; the processor adds both).
export function objectPositions(values, numSources) {
  const out = [];
  for (let o = 0; o < numSources; o++) out.push([o, [values[`s${o + 1}x`] ?? 0, values[`s${o + 1}y`] ?? 0, values[`s${o + 1}z`] ?? 0]]);
  out.push([ZONE_OBJECT_LISTENER, [values.lx ?? 0, values.ly ?? 0, values.lz ?? 0]]);
  return out;
}
export function computeZoneOccupancy(zones, values, numSources) {
  const objs = objectPositions(values, numSources);
  return (zones || []).map(z => ({ id: z.id, objects: zoneEnabled(z) ? objs.filter(([, pos]) => zoneSignedDistance(z, pos) <= 0).map(([o]) => o) : [] }));
}
// Driver values (same definitions as the processor, without hysteresis).
export function zoneDriverRaw(route, zones, pos) {
  const list = zones || [];
  if (route.driver === 'zoneIndex') {
    for (let i = 0; i < list.length; i++) if (zoneEnabled(list[i]) && zoneSignedDistance(list[i], pos) <= 0) return i + 1;
    return 0;
  }
  const z = list.find(q => q.id === route.zoneId);
  if (!z || !zoneEnabled(z)) return route.driver === 'inZone' ? 0 : ZONE_FAR;
  const d = zoneSignedDistance(z, pos);
  return route.driver === 'inZone' ? zoneSoftValue(z, d) : d;
}
export function nextZoneId(zones) { return (zones || []).reduce((m, z) => Math.max(m, z.id || 0), 0) + 1; }
// Occupancy from live telemetry (native) or computed (mock / no audio): {id -> objects[]}.
export function liveOccupancy(state, telemetry) {
  const wire = telemetry?.['status.load']?.zones?.occupancy;
  if (Array.isArray(wire)) return new Map(wire.map(o => [o.id, o.objects || []]));
  return null;
}
