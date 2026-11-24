import { evaluatePath, evaluateAzimuthKeys, pathGroupDriver, pathId, pathYaw, remainder360 } from '../viewport/movement-path.js';

export const MOD_MAX_ROUTES = 64;
export const MOD_MAX_HOST_OUTS = 16;
export const MOD_MIN_NODES = 2;
export const MOD_MAX_NODES = 8;

export const MOD_DRIVERS = [
  { id: 'distanceToListener', label: 'Distance to listener', unit: 'm', defaultIn: [1, 15], step: 0.1, desc: '3D Euclidean distance between object and listener' },
  { id: 'distanceToPoint', label: 'Distance to point', unit: 'm', defaultIn: [0, 15], step: 0.1, hasPoint: true, desc: '3D Euclidean distance from object to reference XYZ point' },
  { id: 'azimuthToListener', label: 'Azimuth to listener', unit: '°', defaultIn: [-180, 180], step: 1, noListener: true, desc: 'Horizontal angle relative to listener heading (0° front, ±180° back)' },
  { id: 'elevation', label: 'Elevation angle', unit: '°', defaultIn: [-45, 45], step: 1, noListener: true, desc: 'Vertical angle from listener (+90° above, −90° below)' },
  { id: 'x', label: 'X position', unit: 'm', defaultIn: [-10, 10], step: 0.1, desc: 'World X coordinate in metres' },
  { id: 'y', label: 'Y position', unit: 'm', defaultIn: [-10, 10], step: 0.1, desc: 'World Y coordinate in metres' },
  { id: 'z', label: 'Z height', unit: 'm', defaultIn: [0, 5], step: 0.1, desc: 'World Z height above floor in metres' },
  { id: 'speed', label: 'Speed', unit: 'm/s', defaultIn: [0, 10], step: 0.1, desc: 'Smoothed 3D movement speed (~30 ms window)' },
  { id: 'radialSpeed', label: 'Radial speed', unit: 'm/s', defaultIn: [-10, 10], step: 0.1, noListener: true, desc: 'Signed speed away from (+) or toward (−) listener' },
  { id: 'pathPosition', label: 'Path position', unit: '', inLabel: 'In (path 0–1)', defaultIn: [0, 1], step: 0.01, desc: 'Path progress: 0 = start, 1 = end (= Position 0–100 %, including Path LFO)' },
  { id: 'distanceToNearestWall', label: 'Distance to nearest wall', unit: 'm', defaultIn: [0, 5], step: 0.1, desc: 'Shortest distance from object to any wall segment' }
];

export const MOD_TARGETS = [
  { id: 'sourceGain', label: 'Source gain Δ', shortLabel: 'Gain Δ', unit: 'dB', min: -60, max: 12, defaultOut: [0, -24], step: 0.5, global: false },
  { id: 'sourceLowPass', label: 'Source low-pass', shortLabel: 'Low-pass', unit: 'Hz', min: 20, max: 20000, defaultOut: [20000, 1400], step: 10, global: false, isFreq: true },
  { id: 'sourceHighPass', label: 'Source high-pass', shortLabel: 'High-pass', unit: 'Hz', min: 20, max: 20000, defaultOut: [20, 500], step: 10, global: false, isFreq: true },
  { id: 'sourceLowShelf', label: 'Source low shelf', shortLabel: 'Low shelf', unit: 'dB', min: -12, max: 12, defaultOut: [0, -6], step: 0.5, global: false },
  { id: 'sourceHighShelf', label: 'Source high shelf', shortLabel: 'High shelf', unit: 'dB', min: -12, max: 12, defaultOut: [0, -6], step: 0.5, global: false },
  { id: 'earlyLevel', label: 'Early level Δ', shortLabel: 'Early level Δ', unit: 'dB', min: -60, max: 6, defaultOut: [0, -6], step: 0.5, global: true },
  { id: 'tailLevel', label: 'Tail level Δ', shortLabel: 'Tail level Δ', unit: 'dB', min: -48, max: 12, defaultOut: [0, -6], step: 0.5, global: true },
  { id: 'mix', label: 'Wet / dry mix Δ', shortLabel: 'Mix Δ', unit: '%', min: -100, max: 100, defaultOut: [0, -25], step: 1, global: true },
  { id: 'none', label: 'Host output only', shortLabel: 'Host out', unit: '', min: 0, max: 1, defaultOut: [0, 1], step: 0.01, global: true, hostOnly: true }
];

export const DRIVER_MAP = new Map(MOD_DRIVERS.map(d => [d.id, d]));
export const TARGET_MAP = new Map(MOD_TARGETS.map(t => [t.id, t]));
export const driverMeta = id => DRIVER_MAP.get(id) || MOD_DRIVERS[0];
export const targetMeta = id => TARGET_MAP.get(id) || MOD_TARGETS[0];
export const isGlobalTarget = id => !!targetMeta(id).global;

export const LFO_SHAPES = [
  { id: 'sine', label: 'Sine' },
  { id: 'triangle', label: 'Triangle' },
  { id: 'saw', label: 'Saw' },
  { id: 'square', label: 'Square' },
  { id: 'smoothRandom', label: 'Smooth random' }
];

export const DEFAULT_LFO = Object.freeze({
  enabled: false,
  shape: 'sine',
  rateHz: 0.5,
  depth: 0,
  phaseDeg: 0,
  seed: 1
});

export function normalizeLfo(lfo) {
  const src = lfo && typeof lfo === 'object' ? lfo : {};
  const shape = LFO_SHAPES.some(s => s.id === src.shape) ? src.shape : 'sine';
  const rateHz = Number.isFinite(src.rateHz) ? Math.max(0, Math.min(8, src.rateHz)) : 0.5;
  const depth = Number.isFinite(src.depth) ? Math.max(0, Math.min(1, src.depth)) : 0;
  const phaseDeg = Number.isFinite(src.phaseDeg) ? Math.max(0, Math.min(360, src.phaseDeg)) : 0;
  const seed = Number.isInteger(src.seed) ? Math.max(1, Math.min(999999, src.seed)) : 1;
  return { enabled: !!src.enabled, shape, rateHz, depth, phaseDeg, seed };
}

export function isDefaultLfo(lfo) {
  const n = normalizeLfo(lfo);
  return !n.enabled && n.shape === 'sine' && n.rateHz === 0.5 && n.depth === 0 && n.phaseDeg === 0 && n.seed === 1;
}

export const MOD_PRESETS = [
  {
    id: 'dist-lpf',
    label: 'Distance → low-pass',
    desc: 'Rolls off high frequencies smoothly with distance',
    build: object => ({
      enabled: true,
      object: object === 32 ? 0 : object,
      driver: 'distanceToListener',
      curve: {
        inputRange: [1, 15],
        outputRange: [20000, 1200],
        invert: false,
        clamp: true,
        nodes: [{ x: 0, y: 0, smooth: true }, { x: 1, y: 1, smooth: false }]
      },
      targetObject: -1,
      target: 'sourceLowPass',
      attackMs: 15,
      releaseMs: 80
    })
  },
  {
    id: 'dist-gain',
    label: 'Distance → gain',
    desc: 'Attenuates source gain as distance increases',
    build: object => ({
      enabled: true,
      object: object === 32 ? 0 : object,
      driver: 'distanceToListener',
      curve: {
        inputRange: [1, 15],
        outputRange: [0, -12],
        invert: false,
        clamp: true,
        nodes: [{ x: 0, y: 0, smooth: false }, { x: 1, y: 1, smooth: false }]
      },
      targetObject: -1,
      target: 'sourceGain',
      attackMs: 15,
      releaseMs: 80
    })
  },
  {
    id: 'behind-lpf',
    label: 'Behind the listener → low-pass',
    desc: 'Keeps front full-band (0°) and softens rear sound (±180°)',
    build: object => ({
      enabled: true,
      object: object === 32 ? 0 : object,
      driver: 'azimuthToListener',
      curve: {
        inputRange: [-180, 180],
        outputRange: [20000, 1400],
        invert: false,
        clamp: true,
        nodes: [
          { x: 0, y: 1, smooth: true },
          { x: 0.25, y: 0.25, smooth: true },
          { x: 0.5, y: 0, smooth: true },
          { x: 0.75, y: 0.25, smooth: true },
          { x: 1, y: 1, smooth: false }
        ]
      },
      targetObject: -1,
      target: 'sourceLowPass',
      attackMs: 20,
      releaseMs: 100
    })
  },
  {
    id: 'speed-hshelf',
    label: 'Speed → high shelf',
    desc: 'Adds high-frequency presence when moving faster',
    build: object => ({
      enabled: true,
      object,
      driver: 'speed',
      curve: {
        inputRange: [0, 8],
        outputRange: [0, 4.5],
        invert: false,
        clamp: true,
        nodes: [{ x: 0, y: 0, smooth: true }, { x: 1, y: 1, smooth: false }]
      },
      targetObject: object === 32 ? 0 : -1,
      target: 'sourceHighShelf',
      attackMs: 25,
      releaseMs: 140
    })
  },
  {
    id: 'path-gain',
    label: 'Path position → gain',
    desc: 'Fades a moving source in along the centre of its path',
    build: object => ({
      enabled: true,
      object: object === 32 ? 0 : object,
      driver: 'pathPosition',
      curve: {
        inputRange: [0, 1],
        outputRange: [-12, 0],
        invert: false,
        clamp: true,
        nodes: [
          { x: 0, y: 0, smooth: true },
          { x: 0.5, y: 1, smooth: true },
          { x: 1, y: 0, smooth: false }
        ]
      },
      targetObject: -1,
      target: 'sourceGain',
      attackMs: 15,
      releaseMs: 80
    })
  }
];

export function createDefaultRoute(object = 0, target = null, numSources = 1) {
  const safeObj = object === 32 ? 32 : Math.max(0, Math.min(Math.max(0, numSources - 1), object));
  const resolvedTarget = target || (safeObj === 32 ? 'mix' : 'sourceLowPass');
  const tm = targetMeta(resolvedTarget);
  const drv = resolvedTarget === 'mix' ? 'distanceToListener' : 'distanceToListener';
  const dm = driverMeta(drv);
  return {
    enabled: true,
    object: safeObj,
    driver: drv,
    curve: {
      inputRange: [...dm.defaultIn],
      outputRange: [...tm.defaultOut],
      invert: false,
      clamp: true,
      nodes: [{ x: 0, y: 0, smooth: false }, { x: 1, y: 1, smooth: false }]
    },
    targetObject: tm.global ? -1 : (safeObj === 32 ? 0 : -1),
    target: resolvedTarget,
    attackMs: 15,
    releaseMs: 80
  };
}

export function normalizeRoute(raw, numSources = 32) {
  const r = raw && typeof raw === 'object' ? raw : {};
  const object = r.object === 32 ? 32 : Math.max(0, Math.min(Math.max(0, numSources - 1), Number.isInteger(r.object) ? r.object : 0));
  const driver = DRIVER_MAP.has(r.driver) ? r.driver : 'distanceToListener';
  const target = TARGET_MAP.has(r.target) ? r.target : 'sourceGain';
  const dm = driverMeta(driver);
  const tm = targetMeta(target);
  const clampFin = (v, lo, hi, fb) => Number.isFinite(v) ? Math.max(lo, Math.min(hi, v)) : fb;
  const inMin = clampFin(r.curve?.inputRange?.[0], -1e6, 1e6, dm.defaultIn[0]);
  const inMax = clampFin(r.curve?.inputRange?.[1], -1e6, 1e6, dm.defaultIn[1]);
  const outMin = clampFin(r.curve?.outputRange?.[0], tm.min, tm.max, tm.defaultOut[0]);
  const outMax = clampFin(r.curve?.outputRange?.[1], tm.min, tm.max, tm.defaultOut[1]);
  const rawNodes = Array.isArray(r.curve?.nodes) && r.curve.nodes.length >= MOD_MIN_NODES
    ? r.curve.nodes.slice(0, MOD_MAX_NODES)
    : [{ x: 0, y: 0, smooth: false }, { x: 1, y: 1, smooth: false }];
  let prevX = 0;
  const nodes = rawNodes.map((n, i) => {
    const x = clampFin(Number(n?.x), prevX, 1, i / Math.max(1, rawNodes.length - 1));
    prevX = x;
    const y = clampFin(Number(n?.y), 0, 1, x);
    return { x, y, smooth: !!n?.smooth };
  });
  let targetObject = tm.global ? -1 : (Number.isInteger(r.targetObject) ? r.targetObject : -1);
  if (!tm.global) {
    if (targetObject < -1 || targetObject >= numSources) targetObject = -1;
    if (targetObject === -1 && object === 32) targetObject = 0;
  }
  const hostOut = Number.isInteger(r.hostOut) ? Math.max(0, Math.min(MOD_MAX_HOST_OUTS, r.hostOut)) : 0;
  const route = {
    enabled: r.enabled !== false,
    object,
    driver,
    ...(driver === 'distanceToPoint'
      ? {
          point: [
            clampFin(Number(r.point?.[0]), -200, 200, 0),
            clampFin(Number(r.point?.[1]), -200, 200, 0),
            clampFin(Number(r.point?.[2]), 0, 30, 0)
          ]
        }
      : {}),
    curve: {
      inputRange: [inMin, inMax],
      outputRange: [outMin, outMax],
      invert: !!r.curve?.invert,
      clamp: r.curve?.clamp !== false,
      nodes
    },
    targetObject,
    target,
    ...(hostOut > 0 ? { hostOut } : {}),
    attackMs: clampFin(Number(r.attackMs), 0, 2000, 15),
    releaseMs: clampFin(Number(r.releaseMs), 0, 5000, 80)
  };
  return route;
}

export function evaluateModCurveNormalized(curve, raw) {
  const inMin = curve?.inputRange?.[0] ?? 0;
  const inMax = curve?.inputRange?.[1] ?? 10;
  const span = inMax - inMin;
  let t = Math.abs(span) > 1e-12 ? (raw - inMin) / span : 0;
  if (!Number.isFinite(t)) t = 0;
  const clamp = curve?.clamp !== false;
  if (clamp) t = Math.max(0, Math.min(1, t));
  if (curve?.invert) t = 1 - t;
  const nodes = Array.isArray(curve?.nodes) && curve.nodes.length >= 2
    ? curve.nodes
    : [{ x: 0, y: 0, smooth: false }, { x: 1, y: 1, smooth: false }];
  const last = nodes.length - 1;
  if (t <= nodes[0].x) {
    if (!clamp && nodes.length >= 2 && nodes[1].x > nodes[0].x + 1e-12) {
      const slope = (nodes[1].y - nodes[0].y) / (nodes[1].x - nodes[0].x);
      return nodes[0].y + slope * (t - nodes[0].x);
    }
    return nodes[0].y;
  }
  if (t >= nodes[last].x) {
    if (!clamp && nodes.length >= 2 && nodes[last].x > nodes[last - 1].x + 1e-12) {
      const slope = (nodes[last].y - nodes[last - 1].y) / (nodes[last].x - nodes[last - 1].x);
      return nodes[last].y + slope * (t - nodes[last].x);
    }
    return nodes[last].y;
  }
  for (let i = 1; i < nodes.length; i++) {
    if (t <= nodes[i].x) {
      const a = nodes[i - 1], b = nodes[i];
      const dx = b.x - a.x;
      if (dx <= 1e-12) return b.y;
      const s = Math.max(0, Math.min(1, (t - a.x) / dx));
      const shaped = a.smooth ? (s * s * (3 - 2 * s)) : s;
      return a.y + (b.y - a.y) * shaped;
    }
  }
  return nodes[last].y;
}

export function evaluateModCurve(curve, raw) {
  const outMin = curve?.outputRange?.[0] ?? 0;
  const outMax = curve?.outputRange?.[1] ?? 1;
  const u = evaluateModCurveNormalized(curve, raw);
  let val = outMin + u * (outMax - outMin);
  if (!Number.isFinite(val)) val = outMin;
  if (curve?.clamp !== false) {
    const lo = Math.min(outMin, outMax), hi = Math.max(outMin, outMax);
    val = Math.max(lo, Math.min(hi, val));
  }
  return val;
}

const U64_MASK = (1n << 64n) - 1n;
const C1 = 0x9E3779B97F4A7C15n;
const C2 = 0xBF58476D1CE4E5B9n;
const C3 = 0x94D049BB133111EBn;

function lfoHashNode(seed, idx) {
  const s = BigInt(Math.max(1, Math.trunc(seed || 1)));
  const i = BigInt.asUintN(64, BigInt(Math.trunc(idx)));
  let z = (BigInt.asUintN(64, s * C1) ^ BigInt.asUintN(64, i + C2)) & U64_MASK;
  z = BigInt.asUintN(64, (z ^ (z >> 30n)) * C2);
  z = BigInt.asUintN(64, (z ^ (z >> 27n)) * C3);
  z = z ^ (z >> 31n);
  const u = Number(z >> 11n) * (1.0 / 9007199254740992.0);
  return u * 2.0 - 1.0;
}

export function evaluateLfoWave(lfo, tSec = 0) {
  const shape = lfo?.shape || 'sine';
  const rateHz = Number.isFinite(lfo?.rateHz) ? lfo.rateHz : 0.5;
  const phaseDeg = Number.isFinite(lfo?.phaseDeg) ? lfo.phaseDeg : 0;
  const seed = Number.isInteger(lfo?.seed) ? lfo.seed : 1;
  const cycles = Math.max(0, tSec) * rateHz + phaseDeg / 360.0;
  const baseIdx = Math.floor(cycles);
  const frac = cycles - baseIdx;
  switch (shape) {
  case 'sine':
    return Math.sin(2.0 * Math.PI * frac);
  case 'triangle':
    if (frac < 0.25) return 4.0 * frac;
    if (frac < 0.75) return 2.0 - 4.0 * frac;
    return 4.0 * frac - 4.0;
  case 'saw':
    return 2.0 * frac - 1.0;
  case 'square':
    return frac < 0.5 ? 1.0 : -1.0;
  case 'smoothRandom': {
    const p0 = lfoHashNode(seed, baseIdx - 1);
    const p1 = lfoHashNode(seed, baseIdx);
    const p2 = lfoHashNode(seed, baseIdx + 1);
    const p3 = lfoHashNode(seed, baseIdx + 2);
    const t = frac, t2 = t * t, t3 = t2 * t;
    const v = 0.5 * (2.0 * p1 + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
    return Math.max(-1.0, Math.min(1.0, v));
  }
  default:
    return 0.0;
  }
}

export function applyPathLfo(lfo, uBase, tSec = 0, closed = false, depthScale = 1.0) {
  const base = Math.max(0, Math.min(1, Number(uBase) || 0));
  if (!lfo?.enabled || !(lfo.depth > 0) || !(depthScale > 0)) return base;
  const uRaw = base + (lfo.depth * depthScale) * evaluateLfoWave(lfo, tSec);
  if (closed) {
    const wrapped = uRaw - Math.floor(uRaw);
    return wrapped >= 1.0 || wrapped < 0.0 ? 0.0 : wrapped;
  }
  return Math.max(0, Math.min(1, uRaw));
}

export function distanceToWallSegments(scene, px, py, pz) {
  const walls = scene?.walls;
  if (!Array.isArray(walls) || !walls.length) return 0;
  const defaultTop = scene?.ceilingMaterial === null ? 1e9 : (Number.isFinite(scene?.heightM) ? scene.heightM : 4);
  let best = 1e12;
  for (const w of walls) {
    const ax = w.a?.[0] ?? 0, ay = w.a?.[1] ?? 0, bx = w.b?.[0] ?? 0, by = w.b?.[1] ?? 0;
    const z0 = Number.isFinite(w.zBottomM) ? w.zBottomM : 0;
    const z1 = w.zTopM === null || w.zTopM === undefined ? defaultTop : w.zTopM;
    const vx = bx - ax, vy = by - ay;
    const len2 = vx * vx + vy * vy;
    const t = len2 > 1e-12 ? Math.max(0, Math.min(1, ((px - ax) * vx + (py - ay) * vy) / len2)) : 0;
    const dxy = Math.hypot(px - (ax + t * vx), py - (ay + t * vy));
    const dz = pz < z0 ? (z0 - pz) : (pz > z1 ? (pz - z1) : 0);
    const d = Math.hypot(dxy, dz);
    if (d < best) best = d;
  }
  return best >= 1e11 ? 0 : best;
}

export function resolveModulatedPoseState(state, tSec = 0, { includeLfo = true } = {}) {
  const values = { ...(state?.parameters?.values || {}) };
  const paths = state?.document?.paths || [];
  const ns = state?.document?.sources?.length ?? 0;
  const creative = (values.mode ?? 0) >= 0.5;
  const uMap = new Array(33).fill(0);
  const velMap = new Array(33).fill([0, 0, 0]);
  const prefix = o => o === 32 ? 'l' : `s${o + 1}`;
  const activeIndices = [...Array.from({ length: ns }, (_, i) => i), 32];

  const evalObjAt = (o, timeSec) => {
    const p = paths[o];
    if (!p?.enabled || (p.points?.length ?? 0) < 2) {
      const pre = prefix(o);
      return {
        u: Math.max(0, Math.min(1, values[pathId(o)] ?? 0)),
        pos: [values[pre + 'x'] ?? 0, values[pre + 'y'] ?? 0, values[pre + 'z'] ?? 0]
      };
    }
    const driver = pathGroupDriver(paths, o);
    const uBase = Math.max(0, Math.min(1, values[pathId(driver)] ?? 0));
    const lfo = paths[driver]?.lfo;
    const uEff = (creative && includeLfo && lfo?.enabled && lfo.depth > 0)
      ? applyPathLfo(lfo, uBase, timeSec, !!p.closed, 1.0)
      : uBase;
    return { u: uEff, pos: evaluatePath(p, uEff) };
  };

  for (const o of activeIndices) {
    const pre = prefix(o);
    const p = paths[o];
    const cur = evalObjAt(o, tSec);
    uMap[o] = cur.u;
    values[pre + 'x'] = cur.pos[0];
    values[pre + 'y'] = cur.pos[1];
    values[pre + 'z'] = cur.pos[2];
    if (p?.enabled && (p.points?.length ?? 0) >= 2) {
      if (p.orient && !p.lookAt) values[pre + 'az'] = (values[pre + 'az'] ?? 0) + pathYaw(p, cur.u) + (p.orientOffset || 0);
      else if (!p.lookAt && Array.isArray(p.azKeys) && p.azKeys.length > 0) {
        const keyAz = evaluateAzimuthKeys(p.azKeys, cur.u);
        if (keyAz !== null) values[pre + 'az'] = keyAz;
      }
      const driver = pathGroupDriver(paths, o);
      const lfo = paths[driver]?.lfo;
      if (creative && includeLfo && lfo?.enabled && lfo.depth > 0 && lfo.rateHz > 0) {
        const dt = 0.005;
        const prev = evalObjAt(o, Math.max(0, tSec - dt));
        velMap[o] = [
          (cur.pos[0] - prev.pos[0]) / dt,
          (cur.pos[1] - prev.pos[1]) / dt,
          (cur.pos[2] - prev.pos[2]) / dt
        ];
      }
    }
  }
  for (const o of activeIndices) {
    const p = paths[o];
    if (!p?.lookAt) continue;
    let tx, ty;
    if (Array.isArray(p.lookPos)) { tx = p.lookPos[0]; ty = p.lookPos[1]; }
    else if (Number.isInteger(p.lookTarget) && p.lookTarget !== o && (p.lookTarget === 32 || p.lookTarget < ns)) {
      tx = values[prefix(p.lookTarget) + 'x'];
      ty = values[prefix(p.lookTarget) + 'y'];
    } else continue;
    const dx = tx - values[prefix(o) + 'x'], dy = ty - values[prefix(o) + 'y'];
    if (!(Math.hypot(dx, dy) >= 1e-3)) continue;
    values[prefix(o) + 'az'] = remainder360(Math.atan2(dy, dx) * 180 / Math.PI + (values[prefix(o) + 'az'] ?? 0) + (p.orientOffset || 0));
  }
  return { values, uMap, velMap };
}

export function previewRouteDriverRaw(state, route, tSec = 0, poseCache = null, velocities = null) {
  if (!state || !route) return 0;
  const ns = state.document?.sources?.length ?? 0;
  const o = route.object;
  if (o < 0 || (o >= ns && o !== 32)) return 0;
  const pose = poseCache || resolveModulatedPoseState(state, tSec, { includeLfo: true });
  const v = pose.values;
  const pre = o === 32 ? 'l' : `s${o + 1}`;
  const ox = v[pre + 'x'] ?? 0, oy = v[pre + 'y'] ?? 0, oz = v[pre + 'z'] ?? 0;
  const lx = v.lx ?? 0, ly = v.ly ?? 0, lz = v.lz ?? 0, lyaw = v.laz ?? 0;

  switch (route.driver) {
  case 'distanceToListener':
    return Math.hypot(ox - lx, oy - ly, oz - lz);
  case 'distanceToPoint': {
    const px = route.point?.[0] ?? 0, py = route.point?.[1] ?? 0, pz = route.point?.[2] ?? 0;
    return Math.hypot(ox - px, oy - py, oz - pz);
  }
  case 'azimuthToListener': {
    if (o === 32) return 0;
    const dx = ox - lx, dy = oy - ly;
    if (Math.hypot(dx, dy) < 1e-6) return 0;
    const bearing = Math.atan2(dy, dx) * 180 / Math.PI;
    return remainder360(bearing - lyaw);
  }
  case 'elevation': {
    if (o === 32) return 0;
    const dxy = Math.hypot(ox - lx, oy - ly), dz = oz - lz;
    if (dxy < 1e-6 && Math.abs(dz) < 1e-6) return 0;
    return Math.atan2(dz, dxy) * 180 / Math.PI;
  }
  case 'x': return ox;
  case 'y': return oy;
  case 'z': return oz;
  case 'speed': {
    const vel = velocities?.[o] || pose.velMap[o] || [0, 0, 0];
    return Math.hypot(vel[0], vel[1], vel[2]);
  }
  case 'radialSpeed': {
    if (o === 32) return 0;
    const d = Math.hypot(ox - lx, oy - ly, oz - lz);
    if (d < 1e-6) return 0;
    const vel = velocities?.[o] || pose.velMap[o] || [0, 0, 0];
    const lvel = velocities?.[32] || pose.velMap[32] || [0, 0, 0];
    const rvx = vel[0] - lvel[0], rvy = vel[1] - lvel[1], rvz = vel[2] - lvel[2];
    return ((ox - lx) * rvx + (oy - ly) * rvy + (oz - lz) * rvz) / d;
  }
  case 'pathPosition':
    return Math.max(0, Math.min(1, pose.uMap[o] ?? 0));
  case 'distanceToNearestWall':
    return distanceToWallSegments(state.document?.scene, ox, oy, oz);
  default:
    return 0;
  }
}

export function computeModulationTelemetry(state, tSec = 0, velocities = null) {
  const routes = state?.document?.modulation?.routes || [];
  if (!routes.length) return { routes: [], previews: [] };
  const ns = state.document?.sources?.length ?? 0;
  const creative = (state.parameters?.values?.mode ?? 0) >= 0.5;
  const poseCache = resolveModulatedPoseState(state, tSec, { includeLfo: true });
  const wireRoutes = [];
  const previews = [];
  for (const r of routes) {
    const srcValid = r.object >= 0 && (r.object < ns || r.object === 32);
    const isGlobal = isGlobalTarget(r.target);
    const tgtSrc = isGlobal ? -1 : (r.targetObject >= 0 ? r.targetObject : (r.object < ns ? r.object : -1));
    const tgtValid = isGlobal || (tgtSrc >= 0 && tgtSrc < ns);
    const active = !!r.enabled && srcValid && tgtValid && creative;
    const raw = previewRouteDriverRaw(state, r, tSec, poseCache, velocities);
    const normVal = Math.max(0, Math.min(1, evaluateModCurveNormalized(r.curve, raw)));
    const value = r.target === 'none' ? normVal : evaluateModCurve(r.curve, raw);
    const cleanRaw = Number.isFinite(raw) ? raw : 0;
    const cleanVal = Number.isFinite(value) ? value : 0;
    previews.push({ raw: cleanRaw, value: cleanVal, hostOutValue: active ? normVal : 0.5, active });
    wireRoutes.push({
      raw: active ? cleanRaw : 0,
      value: active ? cleanVal : 0,
      active
    });
  }
  return { routes: wireRoutes, previews };
}

export function formatModValue(val, unit, isFreq = false) {
  if (!Number.isFinite(val)) return '—';
  if (isFreq) {
    return val >= 1000 ? `${(val / 1000).toFixed(val >= 10000 ? 1 : 2)} kHz` : `${Math.round(val)} Hz`;
  }
  const abs = Math.abs(val);
  const digits = unit === '' ? 2 : abs >= 100 ? 0 : abs >= 10 ? 1 : 2;
  const text = Number(val.toFixed(digits)) === 0 ? '0' : val.toFixed(digits);
  return unit ? `${text} ${unit}` : text;
}

export function routeMatchesObject(route, selObject) {
  if (selObject == null) return true;
  if (route.object === selObject) return true;
  if (isGlobalTarget(route.target)) return true;
  if (route.targetObject === selObject) return true;
  return false;
}

/** Compute effective Path LFO swing around current Position (0..1) and safe depth limit. */
export function computePathLfoSwing(pos01, depth01, closed = false) {
  const posPct = Math.max(0, Math.min(100, (Number(pos01) || 0) * 100));
  const depthPct = Math.max(0, Math.min(100, (Number(depth01) || 0) * 100));
  const rawLo = posPct - depthPct;
  const rawHi = posPct + depthPct;
  const leavesEnds = depthPct > 1e-6 && (rawLo < -1e-6 || rawHi > 100 + 1e-6);
  const maxSafeDepthPct = Math.min(posPct, 100 - posPct);
  const safeDepth = Math.floor(maxSafeDepthPct + 1e-9) / 100;
  const fmt = v => String(Number(v.toFixed(1)));
  let text;
  if (!leavesEnds || !closed) {
    text = `Swing ${fmt(Math.max(0, rawLo))}–${fmt(Math.min(100, rawHi))} %`;
  } else {
    const wrapMod = v => ((v % 100) + 100) % 100;
    const span = rawHi - rawLo;
    text = span >= 100 - 1e-6
      ? 'Swing 0–100 % (full loop)'
      : `Swing ${fmt(wrapMod(rawLo))}–${fmt(wrapMod(rawHi))} % (wraps)`;
  }
  return { posPct, depthPct, rawLo, rawHi, leavesEnds, maxSafeDepthPct, safeDepth, text };
}
