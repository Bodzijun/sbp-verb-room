// Mock stand-in for the engine's `diagnostics.clutter` event. The real placement comes from the C++ engine's
// modeled scene (EditorBridge::clutterObjects); this only makes plausible, deterministic objects inside the room
// outline so the viewport and the show/hide control can be exercised without the plugin. Flagged source:'mock'.
function mulberry32(seed) { let a = seed >>> 0; return () => { a = (a + 0x6D2B79F5) >>> 0; let t = a; t = Math.imul(t ^ t >>> 15, t | 1); t ^= t + Math.imul(t ^ t >>> 7, t | 61); return ((t ^ t >>> 14) >>> 0) / 4294967296; }; }
const insidePolygon = (p, walls) => { let c = 0; for (const w of walls) if ((w.a[1] > p[1]) !== (w.b[1] > p[1]) && w.a[0] + (p[1] - w.a[1]) * (w.b[0] - w.a[0]) / (w.b[1] - w.a[1]) > p[0]) c++; return c % 2 === 1; };
const wallDistance = (p, walls) => Math.min(...walls.map(w => { const dx = w.b[0] - w.a[0], dy = w.b[1] - w.a[1], l2 = dx * dx + dy * dy, t = l2 ? Math.max(0, Math.min(1, ((p[0] - w.a[0]) * dx + (p[1] - w.a[1]) * dy) / l2)) : 0; return Math.hypot(p[0] - w.a[0] - t * dx, p[1] - w.a[1] - t * dy); }));
export function mockClutterEvent(state) {
  const scene = state.document.scene, cfg = scene.clutter || { count: 0 }, walls = scene.walls, objects = [];
  if (cfg.count > 0 && walls.length) {
    const xs = walls.flatMap(w => [w.a[0], w.b[0]]), ys = walls.flatMap(w => [w.a[1], w.b[1]]), x0 = Math.min(...xs), x1 = Math.max(...xs), y0 = Math.min(...ys), y1 = Math.max(...ys);
    const rand = mulberry32((cfg.seed || 1) * 2654435761), U = (a, b) => a + (b - a) * rand();
    for (let tries = 0; objects.length < Math.min(cfg.count, 128) && tries < 80 * cfg.count; tries++) {
      const p = [U(x0, x1), U(y0, y1)], len = U(cfg.minLenM ?? cfg.minLen ?? 0.3, Math.max(cfg.minLenM ?? cfg.minLen ?? 0.3, cfg.maxLenM ?? cfg.maxLen ?? 1.2)), ang = U(0, Math.PI);
      const a = [p[0] - 0.5 * len * Math.cos(ang), p[1] - 0.5 * len * Math.sin(ang)], b = [p[0] + 0.5 * len * Math.cos(ang), p[1] + 0.5 * len * Math.sin(ang)];
      if (![p, a, b].every(q => insidePolygon(q, walls)) || wallDistance(p, walls) < 0.25) continue;
      const hMin = cfg.minHeightM ?? cfg.hMin ?? 0.4, hMax = Math.max(hMin, cfg.maxHeightM ?? cfg.hMax ?? 1.5);
      objects.push({ a, b, z0: 0, z1: Math.min(scene.heightM, U(hMin, hMax)) });
    }
  }
  return { event: 'diagnostics.clutter', revision: state.revision, sceneSerial: state.sceneSerial || '0', requested: cfg.count || 0, seed: cfg.seed || 0, objects, stale: false, source: 'mock' };
}
export const mockClutterSignature = state => JSON.stringify([state.document.scene.clutter, state.document.scene.walls, state.document.scene.heightM]);
