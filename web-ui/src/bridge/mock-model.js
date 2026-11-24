import { clone } from './contract.js';
import { budget, channels, surface, surfaceRefs, layerKey, translatedWallLinks, WALL_CORNER_TOLERANCE } from '../state/selectors.js';
import { layoutOffset } from '../viewport/transducers.js';
import { normalizeSourceColor } from '../viewport/colour.js';
import { cornerXY, movedVertexCorners, sameCorner } from '../viewport/corner-snap.js';
import { joinCorners } from '../state/join-corners.js';
export function reject(code, message) { throw Object.assign(new Error(message), { code }); }
export const tag = () => ({ name: '', group: '', hidden: false, locked: false });

export function defaultAutomationRange() {
  return { xM: [-200, 200], yM: [-200, 200], zM: [0, 30] };
}
export function sceneAutomationRange(scene) {
  return scene?.automationRange ? sanitiseAutomationRange(scene.automationRange) : defaultAutomationRange();
}
export function isDefaultAutomationRange(r) {
  if (!r) return true;
  return Math.abs(r.xM[0] + 200) < 1e-6 && Math.abs(r.xM[1] - 200) < 1e-6
    && Math.abs(r.yM[0] + 200) < 1e-6 && Math.abs(r.yM[1] - 200) < 1e-6
    && Math.abs(r.zM[0]) < 1e-6 && Math.abs(r.zM[1] - 30) < 1e-6;
}
export function sanitiseAutomationRange(raw) {
  const def = defaultAutomationRange();
  const fixAxis = (arr, gLo, gHi, minSpan, dLo, dHi) => {
    let lo = Array.isArray(arr) && Number.isFinite(arr[0]) ? arr[0] : dLo;
    let hi = Array.isArray(arr) && Number.isFinite(arr[1]) ? arr[1] : dHi;
    if (lo > hi) [lo, hi] = [hi, lo];
    lo = Math.max(gLo, Math.min(gHi, lo));
    hi = Math.max(gLo, Math.min(gHi, hi));
    if (hi - lo < minSpan) {
      const mid = 0.5 * (lo + hi);
      lo = mid - 0.5 * minSpan;
      hi = mid + 0.5 * minSpan;
      if (lo < gLo) { lo = gLo; hi = Math.min(gHi, lo + minSpan); }
      if (hi > gHi) { hi = gHi; lo = Math.max(gLo, hi - minSpan); }
    }
    return [lo, hi];
  };
  return {
    xM: fixAxis(raw?.xM, -200, 200, 1, def.xM[0], def.xM[1]),
    yM: fixAxis(raw?.yM, -200, 200, 1, def.yM[0], def.yM[1]),
    zM: fixAxis(raw?.zM, 0, 30, 0.5, def.zM[0], def.zM[1]),
  };
}
export function syncDescriptorsWithRange(state) {
  const ar = sceneAutomationRange(state.document?.scene);
  for (const d of state.parameters.descriptors?.parameters || []) {
    const m = /^(?:s(?:[1-9]|[12]\d|3[0-2])|l)([xyz])$/.exec(d.id);
    if (!m) continue;
    const [lo, hi] = m[1] === 'x' ? ar.xM : m[1] === 'y' ? ar.yM : ar.zM;
    d.min = lo;
    d.max = hi;
    const baseDef = m[1] === 'z' ? 1.5 : 0;
    d.default = Math.max(lo, Math.min(hi, baseDef));
  }
}
function clampPositionValuesToRange(state, ar) {
  const vals = state.parameters.values;
  for (const id of Object.keys(vals)) {
    const m = /^(?:s(?:[1-9]|[12]\d|3[0-2])|l)([xyz])$/.exec(id);
    if (!m) continue;
    const [lo, hi] = m[1] === 'x' ? ar.xM : m[1] === 'y' ? ar.yM : ar.zM;
    vals[id] = Math.round(Math.max(lo, Math.min(hi, vals[id])) * 1000) / 1000;
  }
}
export function expandRangeWithPoint(ar, x, y, z) {
  if (Number.isFinite(x)) { const c = Math.max(-200, Math.min(200, x)); ar.xM = [Math.min(ar.xM[0], c), Math.max(ar.xM[1], c)]; }
  if (Number.isFinite(y)) { const c = Math.max(-200, Math.min(200, y)); ar.yM = [Math.min(ar.yM[0], c), Math.max(ar.yM[1], c)]; }
  if (Number.isFinite(z)) { const c = Math.max(0, Math.min(30, z)); ar.zM = [Math.min(ar.zM[0], c), Math.max(ar.zM[1], c)]; }
}
export function computeFitRange(state, marginM = -1) {
  const s = state.document.scene, sources = state.document.sources || [], paths = state.document.paths || [], vals = state.parameters.values, out = state.document.output || {};
  let rawXMin = Infinity, rawXMax = -Infinity, rawYMin = Infinity, rawYMax = -Infinity, rawZMin = Infinity, rawZMax = -Infinity;
  let hasXY = false, hasZ = false, hasGeom = false;
  const accXY = (x, y) => { if (!Number.isFinite(x) || !Number.isFinite(y)) return; rawXMin = Math.min(rawXMin, x); rawXMax = Math.max(rawXMax, x); rawYMin = Math.min(rawYMin, y); rawYMax = Math.max(rawYMax, y); hasXY = true; };
  const accZ = z => { if (!Number.isFinite(z)) return; rawZMin = Math.min(rawZMin, z); rawZMax = Math.max(rawZMax, z); hasZ = true; };
  for (const w of s.walls || []) { hasGeom = true; accXY(w.a[0], w.a[1]); accXY(w.b[0], w.b[1]); accZ(0); accZ(w.zBottomM ?? 0); accZ(w.zTopM === null || w.zTopM === undefined ? s.heightM : w.zTopM); }
  for (const z of s.floorZones || []) { if (z.vertices?.length) hasGeom = true; for (const pt of z.vertices || []) accXY(pt[0], pt[1]); accZ(z.heightM ?? 0); }
  for (const z of s.ceilingZones || []) { if (z.vertices?.length) hasGeom = true; for (const pt of z.vertices || []) accXY(pt[0], pt[1]); accZ(z.heightM ?? s.heightM); }
  if (hasGeom) { accZ(0); accZ(s.heightM); }
  for (let i = 0; i < sources.length; i++) {
    const cfg = sources[i];
    const sx = vals[`s${i + 1}x`] ?? 0, sy = vals[`s${i + 1}y`] ?? 0, sz = vals[`s${i + 1}z`] ?? 1.5, az = ((vals[`s${i + 1}az`] ?? 0) * Math.PI) / 180;
    accXY(sx, sy); accZ(sz);
    if (!cfg.sum) {
      if (cfg.freePoints && Array.isArray(cfg.offsetsM)) {
        const cs = Math.cos(az), sn = Math.sin(az);
        for (const off of cfg.offsetsM) { accXY(sx + off[0] * cs - off[1] * sn, sy + off[0] * sn + off[1] * cs); accZ(sz + off[2]); }
      } else if ((cfg.channels ?? 1) > 1 && (cfg.spreadM ?? 0) > 0) {
        accXY(sx - cfg.spreadM, sy - cfg.spreadM); accXY(sx + cfg.spreadM, sy + cfg.spreadM);
      }
    }
    const lp = paths[i];
    if (lp) { for (const pt of lp.points || []) { accXY(pt[0], pt[1]); accZ(pt[2]); } if (lp.lookAt && Array.isArray(lp.lookPos)) { accXY(lp.lookPos[0], lp.lookPos[1]); accZ(lp.lookPos[2]); } }
  }
  const lx = vals.lx ?? 0, ly = vals.ly ?? 0, lz = vals.lz ?? 1.5;
  accXY(lx, ly); accZ(lz);
  if ((out.radiusM ?? 0) > 0 && out.layout !== 0 && !(out.layout >= 9 && out.layout <= 11)) {
    accXY(lx - out.radiusM, ly - out.radiusM); accXY(lx + out.radiusM, ly + out.radiusM);
  }
  const lpLis = paths[32];
  if (lpLis) { for (const pt of lpLis.points || []) { accXY(pt[0], pt[1]); accZ(pt[2]); } if (lpLis.lookAt && Array.isArray(lpLis.lookPos)) { accXY(lpLis.lookPos[0], lpLis.lookPos[1]); accZ(lpLis.lookPos[2]); } }
  if (!hasXY) { rawXMin = -5; rawXMax = 5; rawYMin = -5; rawYMax = 5; }
  if (!hasZ) { rawZMin = 0; rawZMax = Math.max(3, s.heightM); }
  const span = Math.max(rawXMax - rawXMin, rawYMax - rawYMin);
  const mXY = marginM > 0 ? marginM : Math.max(1, Math.min(5, 0.1 * span));
  const floorHalf = v => Math.floor(v * 2) * 0.5, ceilHalf = v => Math.ceil(v * 2) * 0.5;
  let xMin = floorHalf(rawXMin - mXY), xMax = ceilHalf(rawXMax + mXY), yMin = floorHalf(rawYMin - mXY), yMax = ceilHalf(rawYMax + mXY);
  if (!hasGeom) { xMin = Math.min(xMin, -5); xMax = Math.max(xMax, 5); yMin = Math.min(yMin, -5); yMax = Math.max(yMax, 5); }
  const zBase = Math.max(rawZMax, hasGeom ? s.heightM : Math.max(3, s.heightM));
  const zMax = ceilHalf(zBase + (s.ceilingMaterial === null ? 2 : 1));
  return sanitiseAutomationRange({ xM: [xMin, xMax], yM: [yMin, yMax], zM: [0, zMax] });
}
const distance = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);
function cross(a, b, c) { return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]); }
export function polygonValid(vertices) {
  let area = 0;
  for (let i = 0; i < vertices.length; i++) {
    const a = vertices[i], b = vertices[(i + 1) % vertices.length];
    if (distance(a, b) < 1e-6) return false;
    area += a[0] * b[1] - b[0] * a[1];
    for (let j = i + 2; j < vertices.length; j++) {
      if (i === 0 && j === vertices.length - 1) continue;
      const c = vertices[j], d = vertices[(j + 1) % vertices.length];
      if (cross(a, b, c) * cross(a, b, d) <= 0 && cross(c, d, a) * cross(c, d, b) <= 0 &&
        Math.max(Math.min(a[0], b[0]), Math.min(c[0], d[0])) <= Math.min(Math.max(a[0], b[0]), Math.max(c[0], d[0])) &&
        Math.max(Math.min(a[1], b[1]), Math.min(c[1], d[1])) <= Math.min(Math.max(a[1], b[1]), Math.max(c[1], d[1]))) return false;
    }
  }
  return Math.abs(area) > 1e-6;
}
export function validateModel(state, validator) {
  validator.assert(state, 'state', 'SnapshotPayload');
  for (const d of state.parameters.descriptors?.parameters || []) {
    const value = state.parameters.values[d.id], min = d.min ?? 0, max = d.max ?? (d.type === 'bool' ? 1 : d.choices.length - 1);
    if (!Number.isFinite(value) || value < min - 1e-6 || value > max + 1e-6) reject('invalid', `Parameter ${d.id} outside descriptor range`);
  }
  const { scene, sources } = state.document, limits = state.limits, usage = budget(state.document);
  if (usage.inputs > limits.maxInputs || usage.points > limits.maxPoints || sources.length > limits.maxSources) reject('budgetExceeded', 'Source channel / point budget exceeded');
  if (scene.walls.length > limits.maxWalls || scene.floorZones.length > limits.maxFloorZones || scene.ceilingZones.length > limits.maxCeilingZones ||
      [...scene.floorZones, ...scene.ceilingZones].filter(z => z.heightM !== null).length > limits.maxPlates) reject('budgetExceeded', 'Surface budget exceeded');
  scene.materials.forEach(m => m.alpha.forEach((a, i) => { if (a + m.tau[i] > 1 + 1e-10) reject('invalid', 'Absorption + transmission exceeds one'); }));
  surfaceRefs(scene).forEach(ref => {
    const obj = surface(scene, ref);
    if (obj.material !== null && !scene.materials[obj.material]) reject('invalid', 'Invalid material reference');
    if (ref.kind === 'wall' && (distance(obj.a, obj.b) < 1e-6 || (obj.zTopM ?? scene.heightM) <= obj.zBottomM)) reject('invalid', 'Zero wall length or invalid heights');
    if (obj.vertices && (!polygonValid(obj.vertices) || (obj.heightM !== null && (obj.heightM <= 0 || obj.heightM > scene.heightM)))) reject('invalid', 'Invalid zone geometry or height');
  });
  for (const s of sources) if (s.firstIn + channels(s) > limits.maxInputs) reject('budgetExceeded', 'Input route exceeds available contract channels');
  const c = scene.clutter;
  if (c.minLenM > c.maxLenM || c.minHeightM > c.maxHeightM || c.tlMinDb > c.tlMaxDb) reject('invalid', 'Minimum exceeds maximum');
}
export function updateEffective(state) {
  const v = state.parameters.values, creative = v.mode === 1;
  const solo = ['soloDirect', 'soloEarly', 'soloTail'].some(k => v[k] >= 0.5);
  state.parameters.effective = { sizePercent:v.size??100,sizeRequested:v.size??100,predelayMs:creative?(v.predelay??0):0,stereoWidthPercent:creative&&state.host.renderedChannels>1?(v.stereoWidth??100):100,widthAvailable:state.host.renderedChannels>1,freeze:creative&&v.freeze>=.5,duckDb:creative?(v.duck??0):0,duckReleaseMs:creative?(v.duckRelease??300):300,duckReductionDb:0,lockReason:creative?'':'Locked in Physical: switch to Creative; stored values are preserved',mode: creative ? 'creative' : 'physical', earlyDb: creative ? v.early : 0,
    tailDb: creative ? v.tail : 0, doppler: creative ? v.doppler : 1, diffusion: v.diffusion,
    decay: ['decayLow', 'decayMid', 'decayHigh'].map(k => creative ? v[k] : 1),
    absorption: { requested: v.absorption, tailCurrent: state.load.tailCurrent },
    mixEffective: { ignored: solo, reason: solo ? 'solo' : v.spatialDry ? 'positionedDry' : 'none' } };
}
export function updateHost(state) {
  const output = state.document.output;
  output.ambisonicsNormalization ??= 0;
  output.ambisonicsLevelDb ??= 0;
  state.host.numPoints = budget(state.document).points;
  const byChannels = { 1: 0, 2: 1, 3: 3, 4: 4, 6: 5, 8: 6, 12: 7 };
  state.host.effectiveLayout = output.layout === -1 ? byChannels[state.host.outputChannels] ?? 8 : output.layout;
  const configured = [1, 2, 2, 3, 4, 6, 8, 12, state.host.outputChannels, 4, 9, 16][state.host.effectiveLayout];
  state.host.renderedChannels = Math.min(configured, state.host.outputChannels);
}
export function expandedRefs(scene, refs, includeGroups = true) {
  const groups = includeGroups ? refs.map(r => surface(scene, r)?.editor.group).filter(Boolean) : [];
  return surfaceRefs(scene).filter(r => refs.some(s => s.kind === r.kind && s.index === r.index) || groups.includes(surface(scene, r)?.editor.group));
}
export function applyMutation(state, command, original = state, presetMaterials = []) {
  const d = state.document, s = d.scene, op = command.op;
  let refs = command.refs || (command.ref ? [command.ref] : []);
  refs.forEach(r => { if (!surface(s, r)?.editor) reject('invalid', 'Surface does not exist'); });
  if (['surface.translate', 'surface.delete'].includes(op)) refs = expandedRefs(s, refs, command.includeGroups !== false);
  if (op.startsWith('surface.') && !['surface.setMetadata', 'surface.unlink'].includes(op)) for (const r of refs) {
    if (surface(s, r).editor.locked || (d.editorFlags.layerModes?.[layerKey(s, r)] || 0) !== 0) reject('locked', 'Selection contains a locked surface or layer');
  }
  const assignMaterial = (r, index) => { if (['floor', 'ceiling'].includes(r.kind)) s[`${r.kind}Material`] = index; else surface(s, r).material = index; };
  const objects = refs.map(r => surface(s, r));
  if (op === 'scene.joinCorners') {
    if (command.refs?.some(ref => ref.kind !== 'wall')) reject('invalid', 'Join corners accepts wall references only');
    return joinCorners(s.walls, command.toleranceM ?? .05, d.editorFlags.layerModes?.walls, command.refs);
  }
  if (op === 'surface.translate') {
    const start = original.document.scene, delta = command.deltaM.slice();
    const mode = original.document.editorFlags.layerModes?.walls || d.editorFlags.layerModes?.walls;
    const links = command.autoLink === true ? translatedWallLinks(start, refs, mode) : new Map();
    const hasRef = ref => refs.some(r => r.kind === ref.kind && r.index === ref.index);
    const { snapFrom, snapTarget } = command;
    let source, target;
    if (snapFrom || snapTarget) {
      source = snapFrom && cornerXY(start, snapFrom);
      target = snapTarget && cornerXY(start, snapTarget);
      if (!source || !target || snapFrom.kind !== 'wall' || !hasRef(snapFrom) || hasRef(snapTarget) ||
          (snapTarget.kind === 'wall' && links.get(snapTarget.index)?.includes(['a', 'b'][snapTarget.vertex])))
        reject('invalid', 'Snap needs a moved wall end and a stationary corner');
      const exact = target.map((v, i) => v - source[i]);
      if (distance(exact, delta) > 1e-9) reject('invalid', 'Snap corners do not match the translation');
      [delta[0], delta[1]] = exact;
    }
    const moveXY = (point, wall) => wall && source && distance(source, point) === 0 ? target.slice() : point.map((v, i) => v + delta[i]);
    refs.forEach(r => {
      const obj = surface(s, r), initial = surface(start, r);
      if (!initial || !['wall', 'floorZone', 'ceilingZone'].includes(r.kind)) reject('invalid', 'Base floor / ceiling cannot move');
      if (r.kind === 'wall') {
        obj.a = moveXY(initial.a, true); obj.b = moveXY(initial.b, true);
        if (source && distance(obj.a, obj.b) < WALL_CORNER_TOLERANCE) reject('invalid', 'the move would make a wall of zero length');
      } else obj.vertices = initial.vertices.map(point => moveXY(point, false));
      if (delta[2]) {
        if (r.kind === 'wall') { obj.zBottomM = initial.zBottomM + delta[2]; obj.zTopM = (initial.zTopM ?? s.heightM) + delta[2]; }
        else if (initial.heightM !== null) obj.heightM = initial.heightM + delta[2];
      }
    });
    if (command.autoLink === true && (delta[0] || delta[1])) {
      for (const [index, ends] of links) {
        const wall = s.walls[index];
        if (!wall || wall.editor.hidden || wall.editor.locked) continue;
        for (const end of ends) wall[end] = moveXY(start.walls[index][end], true);
        if (distance(wall.a, wall.b) < WALL_CORNER_TOLERANCE) reject('invalid', 'the move would make a wall of zero length');
      }
    }
  }
  else if (op === 'surface.moveVertex') {
    const ref = { ...command.ref, vertex: command.vertex };
    if (!cornerXY(s, ref)) reject('invalid', 'Vertex does not exist');
    const moved = movedVertexCorners(s, ref, command.autoLink !== false, d.editorFlags.layerModes?.walls);
    let position = command.positionM;
    if (command.snapTarget) {
      position = cornerXY(original.document.scene, command.snapTarget);
      if (!position || distance(position, command.positionM) > 1e-9 || moved.some(c => sameCorner(c, command.snapTarget)))
        reject('invalid', 'Snap target must be a stationary corner at the requested position');
    }
    for (const r of moved) {
      const obj = surface(s, r);
      if (r.kind === 'wall') obj[['a', 'b'][r.vertex]] = position.slice();
      else obj.vertices[r.vertex] = position.slice();
    }
    if (ref.kind === 'wall' && moved.some(({ index }) => distance(s.walls[index].a, s.walls[index].b) < WALL_CORNER_TOLERANCE))
      reject('invalid', 'the move would make a wall of zero length');
  } else if (op === 'surface.create') {
    if (command.kind === 'wall') {
      if (!command.a || !command.b) reject('invalid', 'Wall needs two endpoints');
      s.walls.push({ a: command.a, b: command.b, zBottomM: command.zBottomM ?? 0, zTopM: command.zTopM ?? null, material: command.material ?? 0, editor: {...tag(),group:command.groupName||''} });
    } else {
      if (!command.vertices || command.vertices.length < 3) reject('invalid', 'Zone needs at least three vertices');
      s[command.kind === 'floorZone' ? 'floorZones' : 'ceilingZones'].push({ vertices: command.vertices, heightM: command.heightM ?? null, material: command.material ?? 0, editor: {...tag(),group:command.groupName||''} });
    }
  } else if (op === 'surface.delete') {
    if (refs.some(r => ['floor', 'ceiling'].includes(r.kind))) reject('invalid', 'Base surfaces cannot be deleted');
    for (const [kind, key] of [['wall', 'walls'], ['floorZone', 'floorZones'], ['ceilingZone', 'ceilingZones']]) s[key] = s[key].filter((_, i) => !refs.some(r => r.kind === kind && r.index === i));
  } else if (op === 'surface.setMetadata') objects.forEach(o => ['name', 'hidden', 'locked'].forEach(k => { if (k in command) o.editor[k] = command[k]; }));
  else if (op === 'surface.link' || op === 'surface.unlink') {
    if (op === 'surface.link' && (objects.length < 2 || refs.some(r => ['floor', 'ceiling'].includes(r.kind)))) reject('invalid', 'Link needs two movable surfaces');
    const used = new Set(surfaceRefs(s).filter(r => !refs.some(q => q.kind === r.kind && q.index === r.index)).map(r => surface(s, r).editor.group));
    let group = command.groupName || 'Group', suffix = 2;
    while (used.has(group)) group = `${command.groupName || 'Group'} ${suffix++}`;
    objects.forEach(o => { o.editor.group = op === 'surface.link' ? group : ''; });
  } else if (['surface.applyPreset', 'surface.setAbsorption', 'surface.setTransmission', 'surface.setScatter'].includes(op)) refs.forEach(r => {
    const old = s.materials[surface(s, r).material];
    if (!old) reject('invalid', 'Opening has no material');
    let mat = clone(old);
    if (op === 'surface.applyPreset') { mat = clone(presetMaterials.find(m => m.name === command.preset) || reject('invalid', 'Unknown fixture material')); }
    else {
      mat.name = old.name.startsWith('Custom') ? old.name : `Custom · ${old.name}`; mat.classId = 10;
      if (command.bands) command.bands.forEach(b => { if ('alpha' in b) mat.alpha[b.band] = Math.min(b.alpha, 1 - mat.tau[b.band]); else mat.tau[b.band] = Math.min(b.tau, 1 - mat.alpha[b.band]); });
      ['scatterLF', 'scatterHF'].forEach(k => { if (k in command) mat[k] = command[k]; });
    }
    s.materials.push(mat); assignMaterial(r, s.materials.length - 1);
  });
  else if (op === 'surface.setHeights') objects.forEach(o => ['zBottomM', 'zTopM', 'heightM'].forEach(k => { if (k in command && k in o) o[k] = command[k]; }));
  else if (op === 'room.setHeight') { if(command.heightM!==undefined)s.heightM = command.heightM; if(command.ceilingExtent!==undefined)s.ceilingExtent=command.ceilingExtent; }
  else if (op === 'room.setOpening') {
    if (!['ceiling', 'ceilingZone'].includes(command.ref.kind)) reject('invalid', 'Only ceiling surfaces can open');
    assignMaterial(command.ref, command.open ? null : s.floorMaterial);
  } else if (op === 'room.setAir') ['tempC', 'humidityPct'].forEach(k => { if (k in command) s[k] = command[k]; });
  else if (op === 'room.setClutter') { Object.keys(s.clutter).forEach(k => { if (k in command) s.clutter[k] = command[k]; }); s.roughSeed = s.clutter.seed; }
  else if (op === 'room.setRoughness') { if ('roughness' in command) s.roughness = command.roughness; if ('seed' in command) s.roughSeed = command.seed; }
  else if (op === 'room.setDiffraction') s.diffraction = command.enabled;
  else if (op === 'room.clear') { s.walls = []; s.floorZones = []; s.ceilingZones = []; s.clutter.count = 0; s.roughness = 0; }
  else if (op === 'room.scale') {
    // Frozen v1 behavior: zones are deliberately NOT scaled; capability is partial.
    let cx = 0, cy = 0;
    if (s.walls.length) {
      let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
      for (const w of s.walls) { x0 = Math.min(x0, w.a[0], w.b[0]); x1 = Math.max(x1, w.a[0], w.b[0]); y0 = Math.min(y0, w.a[1], w.b[1]); y1 = Math.max(y1, w.a[1], w.b[1]); }
      cx = 0.5 * (x0 + x1); cy = 0.5 * (y0 + y1);
    }
    const curAr = sceneAutomationRange(s);
    if (!isDefaultAutomationRange(curAr)) {
      const f = command.factor;
      const nextAr = sanitiseAutomationRange({
        xM: [cx + (curAr.xM[0] - cx) * f, cx + (curAr.xM[1] - cx) * f],
        yM: [cy + (curAr.yM[0] - cy) * f, cy + (curAr.yM[1] - cy) * f],
        zM: command.includeObjects ? [curAr.zM[0] * f, curAr.zM[1] * f] : curAr.zM,
      });
      if (!command.includeObjects) {
        const vals = state.parameters.values;
        for (let i = 0; i < (state.document.sources || []).length; i++) expandRangeWithPoint(nextAr, vals[`s${i + 1}x`], vals[`s${i + 1}y`], vals[`s${i + 1}z`]);
        expandRangeWithPoint(nextAr, vals.lx, vals.ly, vals.lz);
      }
      s.automationRange = sanitiseAutomationRange(nextAr);
      syncDescriptorsWithRange(state);
    }
    s.walls.forEach(w => { w.a = w.a.map(v => v * command.factor); w.b = w.b.map(v => v * command.factor); });
    if (command.includeObjects) {
      s.heightM *= command.factor;
      Object.keys(state.parameters.values).filter(k => /^(s\d+[xyz]|l[xyz])$/.test(k)).forEach(k => { state.parameters.values[k] *= command.factor; });
      if (!isDefaultAutomationRange(sceneAutomationRange(s))) clampPositionValuesToRange(state, sceneAutomationRange(s));
      if (Array.isArray(state.document.paths)) {
        for (const path of state.document.paths) {
          if (path?.points?.length) {
            path.points = path.points.map(pt => [
              Math.max(-200, Math.min(200, pt[0] * command.factor)),
              Math.max(-200, Math.min(200, pt[1] * command.factor)),
              Math.max(0, Math.min(30, pt[2] * command.factor))
            ]);
          }
        }
      }
    }
  } else if (op === 'room.fitAutomationRange') {
    const fitted = computeFitRange(state, command.marginM ?? -1);
    s.automationRange = fitted;
    syncDescriptorsWithRange(state);
    clampPositionValuesToRange(state, fitted);
  } else if (op === 'room.setAutomationRange') {
    let next = clone(sceneAutomationRange(s));
    if (command.reset) next = defaultAutomationRange();
    if ('xM' in command) {
      if (!Array.isArray(command.xM) || command.xM.length !== 2 || command.xM[0] < -200 || command.xM[1] > 200 || command.xM[1] - command.xM[0] < 1 - 1e-9)
        reject('invalid', 'X range must satisfy -200 <= min, max <= 200 and span >= 1.0 m.');
      next.xM = [...command.xM];
    }
    if ('yM' in command) {
      if (!Array.isArray(command.yM) || command.yM.length !== 2 || command.yM[0] < -200 || command.yM[1] > 200 || command.yM[1] - command.yM[0] < 1 - 1e-9)
        reject('invalid', 'Y range must satisfy -200 <= min, max <= 200 and span >= 1.0 m.');
      next.yM = [...command.yM];
    }
    if ('zM' in command) {
      if (!Array.isArray(command.zM) || command.zM.length !== 2 || command.zM[0] < 0 || command.zM[1] > 30 || command.zM[1] - command.zM[0] < 0.5 - 1e-9)
        reject('invalid', 'Z range must satisfy 0 <= min, max <= 30 and span >= 0.5 m.');
      next.zM = [...command.zM];
    }
    s.automationRange = sanitiseAutomationRange(next);
    syncDescriptorsWithRange(state);
    clampPositionValuesToRange(state, s.automationRange);
  } else if (op === 'source.add') {
    if (d.sources.length >= state.limits.maxSources) reject('budgetExceeded', 'Maximum source count');
    d.sources.push({ name: `Source ${d.sources.length + 1}`, format: 0, channels: 1, firstIn: budget(d).inputs, sum: false, spreadM: 0.5, pattern: 0, freePoints: false, offsetsM: [] });
  } else if (op === 'source.remove') {
    if (d.sources.length <= 1 || !d.sources[command.index]) reject('invalid', 'At least one source must remain');
    d.sources.splice(command.index, 1);
    for (let i = command.index + 1; i <= d.sources.length; i++) for (const k of ['x', 'y', 'z', 'az', 'g', 'mute', 'solo']) state.parameters.values[`s${i}${k}`] = state.parameters.values[`s${i + 1}${k}`];
    if (Array.isArray(d.paths)) {
      for (let i = command.index; i < d.sources.length; i++) d.paths[i] = clone(d.paths[i + 1]);
      d.paths[d.sources.length] = { points: [], enabled: true, closed: false, smooth: false, orient: false, laps: 1 };
      for (const lp of d.paths) {
        if (!lp || !('lookTarget' in lp) || !Number.isInteger(lp.lookTarget) || lp.lookTarget >= 32) continue;
        if (lp.lookTarget === command.index) delete lp.lookTarget;
        else if (lp.lookTarget > command.index) lp.lookTarget -= 1;
      }
    }
    if (Array.isArray(d.modulation?.routes)) {
      const idx = command.index;
      const kept = [];
      for (const route of d.modulation.routes) {
        const r = clone(route);
        if (r.object === idx || r.targetObject === idx) continue;
        if (r.object > idx && r.object < 32) r.object -= 1;
        if (r.targetObject > idx && r.targetObject < 32) r.targetObject -= 1;
        kept.push(r);
      }
      if (kept.length) d.modulation.routes = kept;
      else delete d.modulation;
    }
  } else if (op.startsWith('source.')) {
    const source = d.sources[command.index]; if (!source) reject('invalid', 'Source does not exist');
    if (op === 'source.setConfig') {
      const freeze = command.freePoints === true && !source.freePoints;
      ['name', 'format', 'channels', 'firstIn', 'sum', 'freePoints'].forEach(k => { if (k in command) source[k] = command[k]; }); source.channels = channels(source);
      if ('color' in command) {
        const norm = normalizeSourceColor(command.color);
        if (norm === null) reject('invalid', 'color must be empty, null, or #rrggbb');
        if (norm === '') delete source.color;
        else source.color = norm;
      }
      if (source.freePoints && (freeze || source.offsetsM.length !== channels(source))) {
        const layout = { ...source, sum: false };
        source.offsetsM = Array.from({ length: channels(source) }, (_, channel) => layoutOffset(layout, channel));
      }
    }
    else if (op === 'source.setPattern') source.pattern = command.pattern;
    else if (op === 'source.setSpread') source.spreadM = command.spreadM;
    else if (op === 'source.setDirectivity' || op === 'source.setInstrument') {
      const prevInst = source.instrument || 'none';
      const nextInst = 'instrument' in command ? (command.instrument || 'none') : prevInst;
      const amount = Math.max(0, Math.min(1, Number('amount' in command ? command.amount : (source.instrumentAmount ?? 1))));
      const numOpt = (key, min, max) => {
        if (!(key in command)) return undefined;
        const v = Number(command[key]);
        if (!Number.isFinite(v) || v < min || v > max) reject('invalid', `Invalid ${key}`);
        return v;
      };
      const nextAz = numOpt('aimAzDeg', -180, 180);
      const nextEl = numOpt('aimElDeg', -90, 90);
      const nextRoll = numOpt('aimRollDeg', -180, 180);
      if (nextInst === 'none') {
        if ((nextAz && nextAz !== 0) || (nextEl && nextEl !== 0) || (nextRoll && nextRoll !== 0)) {
          reject('invalid', 'Aim correction requires an active directivity profile');
        }
        if (prevInst !== 'none') source.pattern = 0;
        source.instrument = nextInst;
        source.instrumentAmount = amount;
        source.customDirectivityName = '';
        source.directivityWarning = '';
        delete source.aimAzDeg;
        delete source.aimElDeg;
        delete source.aimRollDeg;
        delete source.polarSlices;
      } else {
        source.instrument = nextInst;
        source.instrumentAmount = amount;
        if ('customName' in command) source.customDirectivityName = command.customName || '';
        source.directivityWarning = '';
        const az = nextAz ?? (source.aimAzDeg ?? 0);
        const el = nextEl ?? (source.aimElDeg ?? 0);
        const roll = nextRoll ?? (source.aimRollDeg ?? 0);
        if (az !== 0) source.aimAzDeg = az; else delete source.aimAzDeg;
        if (el !== 0) source.aimElDeg = el; else delete source.aimElDeg;
        if (roll !== 0) source.aimRollDeg = roll; else delete source.aimRollDeg;
        const deg = Math.PI / 180;
        const ca = Math.cos(az * deg), sa = Math.sin(az * deg);
        const ce = Math.cos(el * deg), se = Math.sin(el * deg);
        const cr = Math.cos(roll * deg), sr = Math.sin(roll * deg);
        const fitted = nextInst.startsWith('brass_') ? 0.56 : nextInst === 'genelec_8030c' ? 0.52 : 0.42;
        source.pattern = Math.max(0, Math.min(1, amount * fitted));
        source.polarSlices = [0.25, 0.50, 0.72].map(p => Array.from({ length: 72 }, (_, k) => {
          const phi = k * 2 * Math.PI / 72;
          let ux = Math.cos(phi), uy = Math.sin(phi), uz = 0;
          if (az !== 0) { const nx = ux * ca + uy * sa, ny = -ux * sa + uy * ca; ux = nx; uy = ny; }
          if (el !== 0) { const nx = ux * ce + uz * se, nz = -ux * se + uz * ce; ux = nx; uz = nz; }
          if (roll !== 0) { const ny = uy * cr + uz * sr, nz = -uy * sr + uz * cr; uy = ny; uz = nz; }
          const c = ux + 0.15 * uz;
          const g = Math.max(0.05, (1 - p) + p * c);
          return (1 - amount) + amount * g;
        }));
      }
    }
    else if (op === 'source.setPointOffset') {
      if (source.sum || !source.freePoints || command.channel >= channels(source) || ([5, 6].includes(source.format) && command.channel === 3)) reject('invalid', 'Point is not editable (Sum / LFE / channel)');
      while (source.offsetsM.length < channels(source)) source.offsetsM.push([0, 0, 0]);
      source.offsetsM[command.channel] = command.offsetM;
    }
  } else if (op === 'output.set') { ['layout', 'radiusM', 'micPattern', 'ambisonicsNormalization', 'ambisonicsLevelDb'].forEach(k => { if (k in command) d.output[k] = command[k]; }); if (command.micPattern === 7) d.output.radiusM = 0.0875; }
  else if(op.startsWith('path.')) {
    const emptyPath=()=>({points:[],enabled:true,closed:false,smooth:false,orient:false,laps:1});
    const validEasings=new Set(['linear','easeIn','easeOut','easeInOut']);
    const kMaxAzKeys=128,kAzKeyMinGap=1e-6;
    const validAzEasings=new Set(['linear','easeIn','easeOut','easeInOut','hold']);
    const validLfoShapes=new Set(['sine','triangle','saw','square','smoothRandom']);
    const defaultLfo=()=>({enabled:false,shape:'sine',rateHz:0.5,depth:0,phaseDeg:0,seed:1});
    const isDefaultLfo=l=>!l||(l.enabled===false&&l.shape==='sine'&&l.rateHz===0.5&&l.depth===0&&l.phaseDeg===0&&l.seed===1);
    const parseLfo=(item,base=defaultLfo())=>{
      if(!item||typeof item!=='object'||Array.isArray(item))reject('invalid','Expected LFO object');
      const out={...base};
      if('enabled'in item){if(typeof item.enabled!=='boolean')reject('invalid','lfo.enabled must be boolean');out.enabled=item.enabled;}
      if('shape'in item){if(!validLfoShapes.has(item.shape))reject('invalid','Invalid lfo.shape');out.shape=item.shape;}
      if('rateHz'in item){if(!Number.isFinite(item.rateHz)||item.rateHz<0||item.rateHz>8)reject('invalid','lfo.rateHz out of range [0, 8]');out.rateHz=item.rateHz;}
      if('depth'in item){if(!Number.isFinite(item.depth)||item.depth<0||item.depth>1)reject('invalid','lfo.depth out of range [0, 1]');out.depth=item.depth;}
      if('phaseDeg'in item){if(!Number.isFinite(item.phaseDeg)||item.phaseDeg<0||item.phaseDeg>360)reject('invalid','lfo.phaseDeg out of range [0, 360]');out.phaseDeg=item.phaseDeg;}
      if('seed'in item){if(!Number.isInteger(item.seed)||item.seed<1||item.seed>999999)reject('invalid','lfo.seed out of range [1, 999999]');out.seed=item.seed;}
      return out;
    };
    const parseAzKey=item=>{
      if(!item||typeof item!=='object'||Array.isArray(item))reject('invalid','Expected azimuth key object');
      const t=item.t,az=item.az,easing='easing'in item?item.easing:'linear';
      if(!Number.isFinite(t)||t<0||t>100)reject('invalid','Azimuth key time out of range [0, 100]');
      if(!Number.isFinite(az)||az<-180||az>180)reject('invalid','Azimuth key angle out of range [-180, 180]');
      if(!validAzEasings.has(easing))reject('invalid','Invalid azimuth key easing');
      return {t:t===0?0:t,az:az===0?0:az,easing};
    };
    const normalizeAzKeys=p=>{
      const n=p.points?.length??0;
      if(n<2||!Array.isArray(p.azKeys)||p.azKeys.length===0){delete p.azKeys;return;}
      const sorted=p.azKeys.slice().sort((a,b)=>{
        const af=Number.isFinite(a.t),bf=Number.isFinite(b.t);
        if(af!==bf)return bf-af;
        return af?(a.t-b.t):0;
      });
      const dedup=[];
      for(const item of sorted){
        const k={t:item.t===0?0:item.t,az:item.az===0?0:item.az,easing:item.easing||'linear'};
        if(dedup.length&&Number.isFinite(k.t)&&Number.isFinite(dedup[dedup.length-1].t)&&Math.abs(k.t-dedup[dedup.length-1].t)<kAzKeyMinGap)
          dedup[dedup.length-1]=k;
        else dedup.push(k);
      }
      p.azKeys=dedup;
    };
    const validateAzKeys=p=>{
      if(!('azKeys'in p))return;
      if(!Array.isArray(p.azKeys)||p.azKeys.length>kMaxAzKeys)reject('invalid','Expected 0..128 azimuth keys');
      if((p.points?.length??0)<2&&p.azKeys.length>0)reject('invalid','Path has fewer than two points');
      for(let i=0;i<p.azKeys.length;i++){
        const k=p.azKeys[i];
        if(!Number.isFinite(k.t)||k.t<0||k.t>100)reject('invalid','Azimuth key time out of range');
        if(!Number.isFinite(k.az)||k.az<-180||k.az>180)reject('invalid','Azimuth key angle out of range');
        if(!validAzEasings.has(k.easing))reject('invalid','Invalid azimuth key easing');
        if(i>0&&!(k.t>p.azKeys[i-1].t))reject('invalid','Azimuth key times must be strictly increasing');
      }
    };
    const normalizeTiming=p=>{
      const n=p.points?.length??0;
      if(n<2){delete p.start;delete p.end;delete p.times;delete p.easings;return;}
      if(p.start===0)delete p.start;
      if(p.end===100)delete p.end;
      if(Array.isArray(p.times)){
        while(p.times.length<n)p.times.push(null);
        p.times.length=n;
        p.times[0]=null;
        if(!p.closed)p.times[n-1]=null;
        if(p.times.every(t=>t===null||t===undefined))delete p.times;
      }
      if(Array.isArray(p.easings)){
        while(p.easings.length<n)p.easings.push('linear');
        p.easings.length=n;
        for(let i=0;i<n;i++){
          const isKey=i===0||(p.times?.[i]!==null&&p.times?.[i]!==undefined);
          const isLast=!p.closed&&i+1===n;
          if(!isKey||isLast)p.easings[i]='linear';
        }
        if(p.easings.every(e=>!e||e==='linear'))delete p.easings;
      }
    };
    const validateTiming=p=>{
      const start=p.start??0,end=p.end??100;
      if(!Number.isFinite(start)||!Number.isFinite(end)||start<0||end>100||end-start<0.01)reject('invalid','Invalid path timing window');
      const n=p.points?.length??0;
      if(n<2){if('times'in p||'easings'in p)reject('invalid','Invalid path timing');return;}
      if('times'in p){
        if(!Array.isArray(p.times)||p.times.length!==n)reject('invalid','Expected times matching point count');
        let prev=0;
        for(let i=0;i<n;i++){
          const t=p.times[i];
          if(t===null||t===undefined)continue;
          if(!Number.isFinite(t)||t<0||t>100)reject('invalid','Point time out of range');
          if(i===0&&t!==0)reject('invalid','First point time must be 0%');
          if(!p.closed&&i+1===n&&t!==100)reject('invalid','Last point time must be 100%');
          if(t<prev)reject('invalid','Point times must be non-decreasing');
          prev=t;
        }
      }
      if('easings'in p){
        if(!Array.isArray(p.easings)||p.easings.length!==n)reject('invalid','Expected easings matching point count');
        for(const e of p.easings)if(!validEasings.has(e))reject('invalid','Invalid path easing');
      }
    };
    d.paths ||= Array.from({length:33},emptyPath);
    if(command.object<0||command.object>32||(command.object<32&&!d.sources[command.object]))reject('invalid','Path object does not exist');
    let p=d.paths[command.object]||(d.paths[command.object]=emptyPath());
    if(op==='path.clear'){const keep=d.paths[command.object]||{};d.paths[command.object]=emptyPath();for(const k of ['lookAt','lookTarget','lookPos','orientOffset'])if(k in keep)d.paths[command.object][k]=keep[k];p=d.paths[command.object];}
    else{
      const next=clone(p);
      if(op==='path.set'){
        next.points=command.points.map(q=>q.slice());
        delete next.start;delete next.end;delete next.times;delete next.easings;delete next.lfo;
        if('times'in command)next.times=command.times.map(t=>t??null);
        if('easings'in command)next.easings=command.easings.slice();
        if('azKeys'in command){
          if(!Array.isArray(command.azKeys)||command.azKeys.length>kMaxAzKeys)reject('invalid','Expected 0..128 azimuth keys');
          next.azKeys=command.azKeys.map(parseAzKey);
          normalizeAzKeys(next);
        }
        if('lfo'in command){const l=parseLfo(command.lfo);if(!isDefaultLfo(l))next.lfo=l;}
      }
      if(op==='path.addPoint'){
        if(next.points.length>=64)reject('invalid','Path exceeds 64 points');
        const idx='index' in command?command.index:next.points.length;
        if(idx<0||idx>next.points.length)reject('invalid','Invalid point index');
        next.points.splice(idx,0,command.point.slice());
        if(Array.isArray(next.times))next.times.splice(idx,0,null);
        if(Array.isArray(next.easings))next.easings.splice(idx,0,'linear');
      }
      if(op==='path.movePoint'){
        if(!next.points[command.index])reject('invalid','Point does not exist');
        next.points[command.index]=command.point.slice();
      }
      if(op==='path.removePoint'){
        if(!next.points[command.index])reject('invalid','Point does not exist');
        if(next.points.length<=2)reject('invalid','A path requires two points; use clear');
        next.points.splice(command.index,1);
        if(Array.isArray(next.times))next.times.splice(command.index,1);
        if(Array.isArray(next.easings))next.easings.splice(command.index,1);
        if(Array.isArray(next.times)){if(command.index===0)next.times[0]=null;if(!next.closed)next.times[next.times.length-1]=null;}
      }
      if(op==='path.setPointTiming'){
        const i=command.index;
        if(!next.points[i]||next.points.length<2)reject('invalid','Point does not exist');
        if(!('time'in command)&&!('easing'in command))reject('invalid','Expected time and/or easing');
        if('time'in command){
          if(command.time===null||command.time===undefined){if(Array.isArray(next.times))next.times[i]=null;}
          else{
            if(!Number.isFinite(command.time)||command.time<0||command.time>100)reject('invalid','Point time out of range');
            next.times||=Array(next.points.length).fill(null);
            next.times[i]=command.time;
          }
        }
        if('easing'in command){
          if(!validEasings.has(command.easing))reject('invalid','Invalid path easing');
          const isKey=i===0||(next.times?.[i]!==null&&next.times?.[i]!==undefined);
          const isLast=!next.closed&&i+1===next.points.length;
          if(command.easing!=='linear'&&(!isKey||isLast))reject('invalid','Only key points before the end can have easing');
          if(!next.easings&&command.easing!=='linear')next.easings=Array(next.points.length).fill('linear');
          if(next.easings)next.easings[i]=command.easing;
        }
      }
      if(op==='path.setAzimuthKey'){
        if((next.points?.length??0)<2)reject('invalid','Path has fewer than two points');
        const keys=Array.isArray(next.azKeys)?next.azKeys.slice():[];
        if('index'in command){
          const i=command.index;
          if(!Number.isInteger(i)||i<0||i>=keys.length)reject('invalid','Azimuth key does not exist');
          if(!('t'in command)&&!('az'in command)&&!('easing'in command))reject('invalid','Expected t, az, and/or easing');
          const ak={...keys[i]};
          if('t'in command){if(!Number.isFinite(command.t)||command.t<0||command.t>100)reject('invalid','Azimuth key time out of range');ak.t=command.t;}
          if('az'in command){if(!Number.isFinite(command.az)||command.az<-180||command.az>180)reject('invalid','Azimuth key angle out of range');ak.az=command.az;}
          if('easing'in command){if(!validAzEasings.has(command.easing))reject('invalid','Invalid azimuth key easing');ak.easing=command.easing;}
          keys.splice(i,1);keys.push(ak);
          next.azKeys=keys;normalizeAzKeys(next);
        }else{
          const hasEase='easing'in command;
          const ak=parseAzKey({t:command.t,az:command.az,...(hasEase?{easing:command.easing}:{})});
          const hit=keys.findIndex(k=>Math.abs(k.t-ak.t)<kAzKeyMinGap);
          if(hit>=0){
            keys[hit]={t:ak.t,az:ak.az,easing:hasEase?ak.easing:keys[hit].easing};
          }else{
            if(keys.length>=kMaxAzKeys)reject('invalid','Maximum 128 azimuth keys');
            keys.push(ak);
          }
          next.azKeys=keys;normalizeAzKeys(next);
        }
      }
      if(op==='path.removeAzimuthKey'){
        if((next.points?.length??0)<2)reject('invalid','Path has fewer than two points');
        if(!Array.isArray(next.azKeys)||!Number.isInteger(command.index)||!next.azKeys[command.index])reject('invalid','Azimuth key does not exist');
        next.azKeys.splice(command.index,1);
      }
      if(op==='path.setAzimuthKeys'){
        if(!Array.isArray(command.keys)||command.keys.length>kMaxAzKeys)reject('invalid','Expected 0..128 azimuth keys');
        if(command.keys.length>0&&(next.points?.length??0)<2)reject('invalid','Path has fewer than two points');
        next.azKeys=command.keys.map(parseAzKey);
        normalizeAzKeys(next);
      }
      if(op==='path.setLfo'){
        if(!('lfo'in command))reject('invalid','lfo is required');
        const l=parseLfo(command.lfo,next.lfo||defaultLfo());
        if((next.points?.length??0)<2&&!isDefaultLfo(l))reject('invalid','Path has fewer than two points');
        if(isDefaultLfo(l))delete next.lfo;else next.lfo=l;
      }
      if(op==='path.set'||op==='path.setFlags'){
        for(const k of ['enabled','closed','smooth','orient','laps','start','end'])if(k in command)next[k]=command[k];
        if(op==='path.setFlags'&&'lfo'in command){
          const l=parseLfo(command.lfo,next.lfo||defaultLfo());
          if((next.points?.length??0)<2&&!isDefaultLfo(l))reject('invalid','Path has fewer than two points');
          if(isDefaultLfo(l))delete next.lfo;else next.lfo=l;
        }
        if(command.orient===true)delete next.lookAt;
        if('orientMode'in command){
          if(command.orientMode==='manual'){next.orient=false;delete next.lookAt;}
          else if(command.orientMode==='along'){next.orient=true;delete next.lookAt;}
          else if(command.orientMode==='lookAt'){next.orient=false;next.lookAt=true;}
          else reject('invalid','Expected manual, along or lookAt');
        }
        if('lookTarget'in command){
          if(command.lookTarget===command.object)reject('invalid','An object cannot look at itself');
          if(command.lookTarget!==32&&command.lookTarget>=d.sources.length)reject('invalid','Source does not exist');
          next.lookTarget=command.lookTarget;delete next.lookPos;
        }
        if('lookPos'in command){next.lookPos=command.lookPos.slice(0,3);delete next.lookTarget;}
        if('orientOffset'in command){if(command.orientOffset===0)delete next.orientOffset;else next.orientOffset=command.orientOffset;}
        if(op==='path.setFlags'&&'closed'in command&&!next.closed&&Array.isArray(next.times))next.times[next.times.length-1]=null;
        if('group' in command){if(command.group<0)delete next.group;else next.group=command.group;}
      }
      validateTiming(next);
      validateAzKeys(next);
      normalizeTiming(next);
      normalizeAzKeys(next);
      d.paths[command.object]=next;
    }
  } else if (op.startsWith('modulation.')) {
    const kMaxModRoutes = 64;
    const validDrivers = new Set(['distanceToListener', 'distanceToPoint', 'azimuthToListener', 'elevation', 'x', 'y', 'z', 'speed', 'radialSpeed', 'pathPosition', 'distanceToNearestWall']);
    const validTargets = new Set(['sourceGain', 'sourceLowPass', 'sourceHighPass', 'sourceLowShelf', 'sourceHighShelf', 'earlyLevel', 'tailLevel', 'mix', 'none']);
    const globalTargets = new Set(['earlyLevel', 'tailLevel', 'mix', 'none']);
    const targetBounds = {
      sourceGain: [-60, 12],
      sourceLowPass: [20, 20000],
      sourceHighPass: [20, 20000],
      sourceLowShelf: [-12, 12],
      sourceHighShelf: [-12, 12],
      earlyLevel: [-60, 6],
      tailLevel: [-48, 12],
      mix: [-100, 100],
      none: [0, 1]
    };
    const parseRoute = item => {
      if (!item || typeof item !== 'object' || Array.isArray(item)) reject('invalid', 'Expected modulation route object');
      const enabled = 'enabled' in item ? item.enabled : true;
      if (typeof enabled !== 'boolean') reject('invalid', 'enabled must be a boolean');
      if (!Number.isInteger(item.object) || item.object < 0 || item.object > 32 || (item.object < 32 && item.object >= d.sources.length)) reject('invalid', 'object does not exist');
      if (!validDrivers.has(item.driver)) reject('invalid', 'Invalid modulation driver');
      if (!validTargets.has(item.target)) reject('invalid', 'Invalid modulation target');
      let point;
      if ('point' in item) {
        if (!Array.isArray(item.point) || item.point.length !== 3 || item.point.some(v => !Number.isFinite(v))) reject('invalid', 'point must be [x, y, z]');
        if (item.point[0] < -200 || item.point[0] > 200 || item.point[1] < -200 || item.point[1] > 200 || item.point[2] < 0 || item.point[2] > 30) reject('invalid', 'point coordinates out of range');
        point = item.point.slice();
      }
      const targetObject = 'targetObject' in item ? item.targetObject : -1;
      if (!Number.isInteger(targetObject) || targetObject < -1 || targetObject > 31) reject('invalid', 'targetObject out of range');
      if (globalTargets.has(item.target)) {
        if (targetObject !== -1) reject('invalid', 'Global targets require targetObject -1');
      } else if (targetObject >= d.sources.length || (targetObject === -1 && item.object === 32)) {
        reject('invalid', 'targetObject does not refer to a valid source');
      }
      const hostOut = 'hostOut' in item ? item.hostOut : 0;
      if (!Number.isInteger(hostOut) || hostOut < 0 || hostOut > 16) reject('invalid', 'hostOut out of range [0, 16]');
      const attackMs = 'attackMs' in item ? item.attackMs : 15;
      const releaseMs = 'releaseMs' in item ? item.releaseMs : 80;
      if (!Number.isFinite(attackMs) || attackMs < 0 || attackMs > 2000) reject('invalid', 'attackMs out of range [0, 2000]');
      if (!Number.isFinite(releaseMs) || releaseMs < 0 || releaseMs > 5000) reject('invalid', 'releaseMs out of range [0, 5000]');
      const cv = item.curve;
      if (!cv || typeof cv !== 'object' || Array.isArray(cv)) reject('invalid', 'curve object is required');
      for (const rk of ['inputRange', 'outputRange']) {
        if (!Array.isArray(cv[rk]) || cv[rk].length !== 2 || !Number.isFinite(cv[rk][0]) || !Number.isFinite(cv[rk][1]) || Math.abs(cv[rk][0]) > 1e6 || Math.abs(cv[rk][1]) > 1e6) {
          reject('invalid', `${rk} must be [min, max]`);
        }
      }
      const invert = 'invert' in cv ? cv.invert : false;
      const clamp = 'clamp' in cv ? cv.clamp : true;
      if (typeof invert !== 'boolean' || typeof clamp !== 'boolean') reject('invalid', 'curve.invert and curve.clamp must be boolean');
      if (!Array.isArray(cv.nodes) || cv.nodes.length < 2 || cv.nodes.length > 8) reject('invalid', 'curve.nodes must have 2..8 points');
      const nodes = cv.nodes.map((nd, ni) => {
        if (!nd || typeof nd !== 'object' || !Number.isFinite(nd.x) || !Number.isFinite(nd.y) || nd.x < 0 || nd.x > 1 || nd.y < 0 || nd.y > 1) reject('invalid', 'Curve node x/y out of range [0, 1]');
        if (ni > 0 && nd.x < cv.nodes[ni - 1].x) reject('invalid', 'Curve node x values must be non-decreasing');
        const smooth = 'smooth' in nd ? nd.smooth : false;
        if (typeof smooth !== 'boolean') reject('invalid', 'Curve node smooth must be boolean');
        return { x: nd.x, y: nd.y, smooth };
      });
      const [tgtLo, tgtHi] = targetBounds[item.target] || [-60, 12];
      const clampedOutRange = [
        Math.max(tgtLo, Math.min(tgtHi, cv.outputRange[0])),
        Math.max(tgtLo, Math.min(tgtHi, cv.outputRange[1]))
      ];
      const out = { enabled, object: item.object, driver: item.driver, curve: { inputRange: cv.inputRange.slice(), outputRange: clampedOutRange, invert, clamp, nodes }, targetObject, target: item.target, ...(hostOut > 0 ? { hostOut } : {}), attackMs, releaseMs };
      if (point && item.driver === 'distanceToPoint') out.point = point;
      else if (item.driver === 'distanceToPoint') out.point = [0, 0, 0];
      return out;
    };
    const routes = Array.isArray(d.modulation?.routes) ? d.modulation.routes.map(clone) : [];
    if (op === 'modulation.addRoute') {
      if (routes.length >= kMaxModRoutes) reject('budgetExceeded', 'At most 64 modulation routes.');
      routes.push(parseRoute(command.route));
      d.modulation = { routes };
    } else if (op === 'modulation.setRoute') {
      if (!Number.isInteger(command.index) || command.index < 0 || command.index >= routes.length) reject('invalid', 'Modulation route does not exist');
      routes[command.index] = parseRoute(command.route);
      d.modulation = { routes };
    } else if (op === 'modulation.removeRoute') {
      if (!Number.isInteger(command.index) || command.index < 0 || command.index >= routes.length) reject('invalid', 'Modulation route does not exist');
      routes.splice(command.index, 1);
      if (routes.length) d.modulation = { routes }; else delete d.modulation;
    } else if (op === 'modulation.setRoutes') {
      if (!Array.isArray(command.routes)) reject('invalid', 'routes array is required');
      if (command.routes.length > kMaxModRoutes) reject('budgetExceeded', 'At most 64 modulation routes.');
      const nextRoutes = command.routes.map(parseRoute);
      if (nextRoutes.length) d.modulation = { routes: nextRoutes }; else delete d.modulation;
    } else reject('unsupported', `Unknown modulation operation ${op}`);
  }
  else if (op === 'output.autoMakeup') { state.parameters.values.makeup = 12; } // Synthetic mock estimate, never engine evidence.
  else if (op === 'listener.set') {
    const ar = sceneAutomationRange(s);
    for (const [key, id, bounds] of [['xM', 'lx', ar.xM], ['yM', 'ly', ar.yM], ['zM', 'lz', ar.zM], ['yawDeg', 'laz', [-180, 180]]]) {
      if (key in command) {
        if (command[key] < bounds[0] - 1e-6 || command[key] > bounds[1] + 1e-6) reject('invalid', `${key} outside range`);
        state.parameters.values[id] = Math.max(bounds[0], Math.min(bounds[1], command[key]));
      }
    }
  }
  else if (op === 'layers.set') { d.editorFlags.layerModes ||= {walls:0,floors:0,ceilings:0,plates:0}; d.editorFlags.layerModes[['walls','floors','ceilings','plates'][command.layer]] = command.mode; }
  else if (op === 'editor.setFlags') Object.assign(d.editorFlags, command.flags);
  else if (op === 'tail.setQuality') ['baseRays', 'adaptive'].forEach(k => { if (k in command) d.tail[k] = command[k]; });
  else reject('unsupported', `Command ${op} is unavailable in the mock`);
  updateHost(state);
  updateEffective(state);
}
