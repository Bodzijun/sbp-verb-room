import { clone } from './contract.js';
import { budget, channels, surface, surfaceRefs, layerKey, translatedWallLinks, WALL_CORNER_TOLERANCE } from '../state/selectors.js';
import { layoutOffset } from '../viewport/transducers.js';
import { normalizeSourceColor } from '../viewport/colour.js';
import { cornerXY, movedVertexCorners, sameCorner } from '../viewport/corner-snap.js';
import { joinCorners } from '../state/join-corners.js';
export function reject(code, message) { throw Object.assign(new Error(message), { code }); }
export const tag = () => ({ name: '', group: '', hidden: false, locked: false });
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
    s.walls.forEach(w => { w.a = w.a.map(v => v * command.factor); w.b = w.b.map(v => v * command.factor); });
    if (command.includeObjects) {
      s.heightM *= command.factor;
      Object.keys(state.parameters.values).filter(k => /^(s\d+[xyz]|l[xyz])$/.test(k)).forEach(k => { state.parameters.values[k] *= command.factor; });
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
  } else if (op === 'source.add') {
    if (d.sources.length >= state.limits.maxSources) reject('budgetExceeded', 'Maximum source count');
    d.sources.push({ name: `Source ${d.sources.length + 1}`, format: 0, channels: 1, firstIn: budget(d).inputs, sum: false, spreadM: 0.5, pattern: 0, freePoints: false, offsetsM: [] });
  } else if (op === 'source.remove') {
    if (d.sources.length <= 1 || !d.sources[command.index]) reject('invalid', 'At least one source must remain');
    d.sources.splice(command.index, 1);
    for (let i = command.index + 1; i <= d.sources.length; i++) for (const k of ['x', 'y', 'z', 'az', 'g', 'mute', 'solo']) state.parameters.values[`s${i}${k}`] = state.parameters.values[`s${i + 1}${k}`];
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
        delete next.start;delete next.end;delete next.times;delete next.easings;
        if('times'in command)next.times=command.times.map(t=>t??null);
        if('easings'in command)next.easings=command.easings.slice();
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
      if(op==='path.set'||op==='path.setFlags'){
        for(const k of ['enabled','closed','smooth','orient','laps','start','end'])if(k in command)next[k]=command[k];
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
      normalizeTiming(next);
      d.paths[command.object]=next;
    }
  }
  else if (op === 'output.autoMakeup') { state.parameters.values.makeup = 12; } // Synthetic mock estimate, never engine evidence.
  else if (op === 'listener.set') { for (const [key,id] of [['xM','lx'],['yM','ly'],['zM','lz'],['yawDeg','laz']]) if (key in command) state.parameters.values[id] = command[key]; }
  else if (op === 'layers.set') { d.editorFlags.layerModes ||= {walls:0,floors:0,ceilings:0,plates:0}; d.editorFlags.layerModes[['walls','floors','ceilings','plates'][command.layer]] = command.mode; }
  else if (op === 'editor.setFlags') Object.assign(d.editorFlags, command.flags);
  else if (op === 'tail.setQuality') ['baseRays', 'adaptive'].forEach(k => { if (k in command) d.tail[k] = command[k]; });
  else reject('unsupported', `Command ${op} is unavailable in the mock`);
  updateHost(state);
  updateEffective(state);
}
