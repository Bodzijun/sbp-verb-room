import { modKey } from '../state/platform.js';
import { compilePath, evaluatePath, pathPositionLocked, withPathPoses } from './movement-path.js';
import { facingAngle, cursorEnergyLevel } from './t13.js';
import { viewportState, positionId } from './effective-scene.js';
import { element, button } from '../components/dom.js';
import { cleanSelection, toggleSelection, touchesBox, boxSelection, sameRef } from '../state/selection.js';
import { canonical } from '../bridge/parameters.js';
import { surface, surfaceRefs, expandedRefs, layerKey, translatedWallLinks } from '../state/selectors.js';
import { createCamera, scenePoints, boundsOf, frame, project, pointOnTargetPlane, pointOnHeightPlane, hitHeightForSurface, pan, zoomAt, add, screenExtent } from './camera.js';
import { createEnergyLoader, projectEnergy, energyColour } from './energy-layer.js';
import { sourcePoints, receivers, rotateOffset } from './transducers.js';
import { markerRadius, orientationTick, directivityTicks } from './marker.js';
import { viewportPalette } from './palette.js';
import { materialColour, rayColour, surfaceRole, sourceMarkerColour, sourceLabelColour, isValidSourceColorHex } from './colour.js';
import { skyEscapeGuides } from './rays.js';
import { mockRays, enginePaths, rayStyle, describeRay } from './ray-model.js';
import { finiteGrid, gridStep, projectedSegment, clipPolygonNear } from './grid.js';
import { effectiveStep, autoStep, pixelsPerMetre, visibleGrid } from './snap.js';
import { cornerXY, movedVertexCorners, sameCorner, snapTranslation, snapVertex, visibleCorners, wallCorners } from './corner-snap.js';
import { viewportEvent } from '../state/render-policy.js';
import { scaleIndicatorState, wallLengthM, formatMetres2 } from './room-metrics.js';
// Default material strokes stay inside the warm canvas palette. Explicit user
// colours and theme overrides are preserved; alpha never depends on class/type.
const same = (a, b) => a.kind === b.kind && a.index === b.index;
function segmentDistance(p, a, b) {
  const dx = b[0] - a[0], dy = b[1] - a[1], t = Math.max(0, Math.min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / (dx * dx + dy * dy || 1)));
  return Math.hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy);
}
function inside(p, poly) { let yes = false; for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) { const a = poly[i], b = poly[j]; if ((a[1] > p[1]) !== (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) yes = !yes; } return yes; }
export function createViewport(authorStore, bridge, {preview=false,draftProvider=null,liveState=null}={}) {
  const overlayFactor=()=>authorStore.view.page==='edit'?1/((authorStore.state.parameters.effective.sizePercent??100)/100):1;
  const overlayPoint=p=>p.map(x=>x*overlayFactor());
  const sizeFactor=()=>authorStore.view.page==='edit'?1:(authorStore.state.parameters.effective.sizePercent??100)/100;
  const store=new Proxy(authorStore,{get(target,key){
    if(key==='state')return viewportState(target.state,target.view.page==='edit');
    if(key==='command')return (command,...args)=>target.command(command.op==='source.setPointOffset'?{...command,offsetM:command.offsetM.map(x=>x/sizeFactor())}:command,...args);
    return target[key];
  }});
  const root = element('section', { className: 'viewport panel' });
  const canvas = element('canvas', { tabIndex: 0, 'aria-label': 'Room viewport. F frame all, Shift F frame selection, Home reset. Middle or Shift drag pans.' });
  const hover = element('div', { className: 'viewport-hover', role: 'status' });
  const hint = element('div', { className: 'viewport-hint', text: 'Drag objects · RMB face cursor · wheel zoom · M / Shift-drag pan · F frame · double-click centre' });
  const badge = element('div', { className: 'viewport-badge' });
  const rayLegend = element('div', { className: 'ray-legend' });
  const drawingReadout=element('div',{className:'drawing-readout',role:'status',hidden:true});
  const marquee=element('div',{className:'selection-marquee',hidden:true,'aria-hidden':'true'});
  const scale=element('div',{className:'energy-scale',hidden:true,'aria-label':'Energy colour scale'});
  const scaleBarLine = element('div', { className: 'viewport-scale-bar' });
  const scaleBarText = element('span', { className: 'viewport-scale-text' });
  const scaleIndicator = element('div', { className: 'viewport-scale', 'aria-label': 'Viewport grid scale' }, [scaleBarText, scaleBarLine]);
  const rebuildingBanner = preview ? null : element('div', { className: 'viewport-rebuilding', role: 'status', 'aria-live': 'polite', hidden: true }, [element('i', { className: 'viewport-rebuilding-dot', 'aria-hidden': 'true' }), element('span', { text: 'Rebuilding…' })]);
  root.append(canvas, hover, badge, hint, rayLegend, drawingReadout, marquee, scale, scaleIndicator, ...(rebuildingBanner ? [rebuildingBanner] : []));
  root.setRebuilding = on => { if (rebuildingBanner) rebuildingBanner.hidden = !on; };
  root.currentSnapStep = () => snapStep();
  let width = 600, height = 300, camera = store.view.camera || createCamera(store.view.projection), hitCache = [], raf = null, disposed = false;
  let poseTimer=null,persistTimer=null,cursorLevel=null;
  const mouseLocked=()=>store.view.page==='main' && !!store.state.preferences?.mainViewportLocked;
  function facePointer(p) {
    const prefix=drag.ref.kind==='listener'?'l':`s${drag.ref.index+1}`,v=store.state.parameters.values;
    const target=camera.view==='3d'?pointOnHeightPlane(p,camera,width,height,v[prefix+'z']):pointOnTargetPlane(p,camera,width,height);
    const angle=facingAngle([v[prefix+'x'],v[prefix+'y']],target);
    if(angle!==null){const id=prefix+'az',d=store.state.parameters.descriptors.parameters.find(d=>d.id===id);pendingPose={[id]:canonical(d,angle)};flushPose();}
  }
  let drag = null, pendingPose = null, lastPoseTime = 0, binary = null, energy = null, draft = [], cycle = { point: [-100, -100], index: 0 };
  let autoFrame = true, sceneKey = '', lastSize = null, lastPage = store.view.page, lastLoadState = null, pendingRoomFit = false;
  let draftCursor=null,draftTool=store.view.tool,chainGroup=null,previewViewport=null,previewStore=null,inset=null;
  let labelVisibility=new Map();
  let rayCache = { key:'', paths:[] }, gridCache = { key:'', lines:[] }, rayHits = [], pickedRay = null, lastPaths = [];
  const ctx = canvas.getContext('2d');
  const reportedErrors=new Set();
  function reportViewportError(error){
    const message=error?.message||String(error);
    resetPointer(false);
    if(reportedErrors.has(message))return;reportedErrors.add(message);
    console.error('Verb viewport:',error);root.dataset.renderError=message;
    root.dispatchEvent(new CustomEvent('verb-viewport-error',{bubbles:true,detail:{code:'internal',message:`Viewport: ${message}`}}));
  }
  function onCanvas(name,handler,options){canvas.addEventListener(name,e=>{try{handler(e);}catch(error){reportViewportError(error);}},options);}
  function schedule() { if (raf === null && !disposed) raf = requestAnimationFrame(() => { raf = null; try { draw(); } catch(error) {
    reportViewportError(error);
  } }); }
  const activeDrag=()=>preview?liveState?.():drag;
  const allPoints=()=>[...scenePoints(store.state),...(preview&&draftProvider?draftProvider().points:[])];
  function fit(selection = false) {
    let points = allPoints();
    if (selection && store.view.selection.length) points = store.view.selection.flatMap(r => pointsFor(r));
    const full = frame({ ...camera, view: store.view.projection }, boundsOf(allPoints()), width, height);
    camera = selection ? frame(full, boundsOf(points), width, height) : full;
    camera.sceneDistance = full.distance; autoFrame = !selection; store.view.camera = camera; schedule();
  }
  function basePointsFor(r, s = store.state.document.scene) {
    const state = store.state, v = state.parameters.values;
    if (r.kind === 'source') return r.channel !== undefined ? [sourcePoints(state, r.index).find(p => p.channel === r.channel)?.position || [0, 0, 0]] : [['x', 'y', 'z'].map(k => v[`s${r.index + 1}${k}`])];
    if (r.kind === 'listener') return [['lx', 'ly', 'lz'].map(k => v[k])];
    const obj = surface(s, r);
    if (!obj) return [];
    const topZ = (obj.zTopM ?? s.heightM) < 1e5 ? (obj.zTopM ?? s.heightM) : s.heightM;
    if (r.kind === 'wall') return [[...obj.a, obj.zBottomM], [...obj.a, topZ], [...obj.b, topZ], [...obj.b, obj.zBottomM]];
    if (obj.vertices) return obj.vertices.map(p => [...p, obj.heightM ?? (r.kind === 'floorZone' ? 0 : s.heightM)]);
    return [];
  }
  function movingSurfaceRefs(d) {
    if (!d) return [];
    return d.movingRefs || [];
  }
  function ghostPointsFor(r, s, d) {
    if (d?.type === 'surface' && d.liveDelta) {
      const points = basePointsFor(r, d.startScene), linkedEnds = r.kind === 'wall' && d.linkedWalls.get(r.index);
      return points.map((p, index) => {
        if (linkedEnds && !linkedEnds.includes(index < 2 ? 'a' : 'b')) return p;
        const z = p[2] + (linkedEnds ? 0 : d.liveDelta[2] || 0);
        if (r.kind === 'wall' && d.snapFrom && p[0] === d.snapFrom[0] && p[1] === d.snapFrom[1])
          return [...d.snapReadout.world, z];
        return [p[0] + d.liveDelta[0], p[1] + d.liveDelta[1], z];
      });
    }
    if (d?.type === 'vertex' && d.liveVertexPos) {
      const obj = surface(s, r);
      if (!obj) return [];
      const topZ = (obj.zTopM ?? s.heightM) < 1e5 ? (obj.zTopM ?? s.heightM) : s.heightM;
      if (r.kind === 'wall') {
        const moved = vertex => d.movedVertices.some(c => sameCorner(c, { ...r, vertex }));
        const a = moved(0) ? d.liveVertexPos : obj.a, b = moved(1) ? d.liveVertexPos : obj.b;
        return [[...a, obj.zBottomM], [...a, topZ], [...b, topZ], [...b, obj.zBottomM]];
      }
      if (obj.vertices) return obj.vertices.map((p, vi) => {
        const xy = d.ref?.kind === r.kind && d.ref?.index === r.index && d.ref?.vertex === vi ? d.liveVertexPos : p;
        return [...xy, obj.heightM ?? (r.kind === 'floorZone' ? 0 : s.heightM)];
      });
    }
    return basePointsFor(r, s);
  }
  function pointsFor(r) {
    const s = store.state.document.scene, d = activeDrag();
    if (movingSurfaceRefs(d).some(m => same(m, r))) return ghostPointsFor(r, s, d);
    return basePointsFor(r, s);
  }
  const energyLoader = createEnergyLoader(url => bridge.readResource(url), value => {
    if (disposed) return; binary = value; energy = null; schedule();
  }, error => { if (!disposed) hover.textContent = error.message; });
  function loadEnergy() { const result = store.telemetry['analysis.result']?.result; if (result) return energyLoader(result); }
  function draw() {
    if (!store.state) return;
    if(preview&&draftProvider){const d=draftProvider();draft=d.outline;draftCursor=null;}
    const wouldSelect=drag?.type==='box'?marqueeSelection():store.view.selection;
    const state = store.state, s = state.document.scene, preferences = state.preferences || {};
    const roles = viewportPalette(preferences);
    ctx.clearRect(0, 0, width, height); ctx.fillStyle = roles.background; ctx.fillRect(0, 0, width, height); hitCache = [];
    ctx.strokeStyle = roles.grid; ctx.lineWidth = 0.6;
    const bounds = boundsOf(scenePoints(state)), span = Math.max(bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1], 1);
    const gridKey = JSON.stringify([bounds,camera.view]);
    if (gridCache.key !== gridKey) gridCache = { key:gridKey, lines:finiteGrid(bounds,camera.view) };
    for (const {a,b,alpha} of gridCache.lines) { ctx.globalAlpha=alpha; worldLine(a,b); }
    drawFineGrid(bounds);
    ctx.globalAlpha = 1;
    drawMovementPaths(roles);
    publishSnapStep();
    if (store.view.energy && camera.view === 'plan' && binary) {
      const opts = store.view.analysis || {}, desc = binary.descriptor, signature = JSON.stringify([opts.band, opts.components, opts.auto, opts.range]);
      if (!energy || energy.signature !== signature) energy = { ...projectEnergy(binary, opts.band ?? -1, opts.components ?? 7, opts.auto !== false, opts.range ?? 60), signature };
      for (let y = 0; y < desc.ny; y++) for (let x = 0; x < desc.nx; x++) {
        const i = y * desc.nx + x; if (energy.db[i] === null) continue;
        const a = project(overlayPoint([desc.x0 + x * desc.dx, desc.y0 + y * desc.dy, 0]), camera, width, height), b = project(overlayPoint([desc.x0 + (x + 1) * desc.dx, desc.y0 + (y + 1) * desc.dy, 0]), camera, width, height);
        ctx.fillStyle = energyColour(1 + energy.db[i] / energy.range, preferences.energyPalette); ctx.globalAlpha = 0.8; ctx.fillRect(a[0], b[1], b[0] - a[0] + 0.5, a[1] - b[1] + 0.5);
      }
      ctx.globalAlpha = 1;
    }
    scale.hidden=!(store.view.energy&&energy&&binary&&camera.view==='plan');
    if(!scale.hidden){
      const stops=Array.from({length:9},(_,i)=>`${energyColour(1-i/8,preferences.energyPalette)} ${i*12.5}%`);
      scale.replaceChildren(element('span',{className:'energy-scale-title',text:'dB rel. peak'}),element('div',{className:'energy-scale-bar',style:`background:linear-gradient(to bottom,${stops.join(',')})`}),element('div',{className:'energy-scale-ticks'},Array.from({length:5},(_,i)=>element('span',{text:String(Number((-energy.range*i/4).toFixed(energy.range<5?2:1)))}))));
      if(cursorLevel!==null){const bar=scale.querySelector('.energy-scale-bar');bar.style.position='relative';bar.append(element('span',{className:'energy-cursor','data-level':`${Number.isFinite(cursorLevel)?cursorLevel.toFixed(1):'-inf'} dB`,style:`top:${Math.max(0,Math.min(1,-cursorLevel/energy.range))*100}%`,text:`${Number.isFinite(cursorLevel)?cursorLevel.toFixed(1):'-inf'} dB`}));}
      scale.dataset.range=String(energy.range);
    }
    // N1: the engine's actual clutter (same seeded objects the sound uses), lightly, under the walls. Never hit-testable.
    // Shown only while the toggle is on AND the document's clutter count is above zero (a stale event cannot resurrect it).
    if (preferences.showClutter !== false && (s.clutter?.count || 0) > 0) {
      const clutter = store.telemetry['diagnostics.clutter'];
      ctx.strokeStyle = roles.clutter; ctx.fillStyle = roles.clutter; ctx.lineWidth = 1;
      for (const o of clutter?.objects || []) {
        const quadWorld = clipPolygonNear([[o.a[0], o.a[1], o.z0], [o.b[0], o.b[1], o.z0], [o.b[0], o.b[1], o.z1], [o.a[0], o.a[1], o.z1]].map(overlayPoint), camera); if (!quadWorld) continue;
        const quad = quadWorld.map(p => project(p, camera, width, height));
        ctx.beginPath(); quad.forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1])); ctx.closePath();
        ctx.globalAlpha = clutter?.stale ? 0.025 : 0.07; ctx.fill(); ctx.globalAlpha = (clutter?.stale?.valueOf() ? .3 : 1)*(camera.view === 'plan' ? 0.7 : 0.42); ctx.lineWidth = camera.view === 'plan' ? 2 : 1; ctx.stroke();
      }
      ctx.globalAlpha = 1; ctx.lineWidth = 1; root.dataset.clutterDrawn = String(clutter?.objects?.length || 0);
    } else root.dataset.clutterDrawn = '0';
    const d = activeDrag();
    const moving = movingSurfaceRefs(d);
    const faces = surfaceRefs(s).filter(r => !['floor', 'ceiling'].includes(r.kind)).map(ref => ({ ref, points: basePointsFor(ref, s) }));
    faces.sort((a, b) => b.points.reduce((n, p) => n + project(p, camera, width, height)[2], 0) / b.points.length - a.points.reduce((n, p) => n + project(p, camera, width, height)[2], 0) / a.points.length);
    faces.forEach(({ ref, points }) => {
      const obj = surface(s, ref), layer = state.document.editorFlags.layerModes?.[layerKey(s, ref)] || 0;
      if (obj.editor.hidden || layer === 2) return;
      // Near-plane clipping: a face partly behind the camera is drawn (and hit-tested) by its visible part only.
      const visible = clipPolygonNear(points, camera); if (!visible) return;
      const clipped = visible !== points;
      const isMoving = moving.some(m => same(m, ref));
      const selected = !isMoving && wouldSelect.some(r => same(r, ref)), screen = visible.map(p => project(p, camera, width, height));
      const material = s.materials[obj.material], role = surfaceRole(ref, obj), colour = materialColour(preferences, material, roles, role);
      ctx.fillStyle = colour; ctx.setLineDash(obj.editor.locked || layer === 1 ? [5, 4] : []);
      ctx.beginPath(); screen.forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1])); ctx.closePath();
      ctx.globalAlpha = isMoving ? (role === 'wall' ? 0.02 : 0.04) : (role === 'wall' ? 0.05 : 0.11); ctx.fill();
      // Selection is a halo around the material colour, never a replacement for it.
      if (selected) { ctx.strokeStyle = roles.selection; ctx.lineWidth = 5.5; ctx.globalAlpha = 0.38; ctx.stroke(); }
      ctx.strokeStyle = colour; ctx.lineWidth = isMoving ? 1.1 : (selected ? 2.2 : 1.4); ctx.globalAlpha = isMoving ? 0.28 : 0.95; ctx.stroke(); ctx.globalAlpha = 1; ctx.setLineDash([]);
      if (store.view.page === 'edit') hitCache.push({ ref, screen, polygon: ref.kind !== 'wall' || camera.view === '3d' || camera.view !== 'plan' });
      if (selected && !clipped) screen.forEach((p, vertex) => { ctx.fillStyle = roles.selection; ctx.beginPath(); ctx.arc(p[0], p[1], 3, 0, 7); ctx.fill(); if (camera.view === 'plan' && (ref.kind !== 'wall' || vertex === 0 || vertex === 3)) hitCache.push({ ref: { ...ref, vertex: ref.kind === 'wall' ? vertex === 0 ? 0 : 1 : vertex }, screen: [p], radius: 6 }); });
    });
    let ghostCount = 0;
    if (moving.length) {
      const ghostFaces = moving.map(ref => ({ ref, points: ghostPointsFor(ref, s, d) }));
      ghostFaces.sort((a, b) => b.points.reduce((n, p) => n + project(p, camera, width, height)[2], 0) / b.points.length - a.points.reduce((n, p) => n + project(p, camera, width, height)[2], 0) / a.points.length);
      ghostFaces.forEach(({ ref, points }) => {
        const obj = surface(s, ref), layer = state.document.editorFlags.layerModes?.[layerKey(s, ref)] || 0;
        if (!obj || obj.editor.hidden || layer === 2) return;
        const visible = clipPolygonNear(points, camera); if (!visible) return;
        const clipped = visible !== points, screen = visible.map(p => project(p, camera, width, height));
        const material = s.materials[obj.material], role = surfaceRole(ref, obj), colour = materialColour(preferences, material, roles, role);
        const traceGhost = () => {
          ctx.beginPath();
          if (ref.kind === 'wall' && camera.view === 'plan' && !clipped && screen.length === 4) {
            ctx.moveTo(screen[0][0], screen[0][1]); ctx.lineTo(screen[3][0], screen[3][1]);
          } else {
            screen.forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1])); ctx.closePath();
          }
        };
        ctx.fillStyle = colour; ctx.setLineDash([]);
        ctx.beginPath(); screen.forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1])); ctx.closePath();
        ctx.globalAlpha = role === 'wall' ? 0.14 : 0.24; ctx.fill();
        traceGhost();
        ctx.strokeStyle = roles.selection; ctx.lineWidth = 5.5; ctx.globalAlpha = 0.35; ctx.stroke();
        ctx.setLineDash([7, 4]); ctx.strokeStyle = roles.selection; ctx.lineWidth = 2.2; ctx.globalAlpha = 1; ctx.stroke();
        ctx.setLineDash([]); ctx.globalAlpha = 1;
        if (wouldSelect.some(r => same(r, ref)) && !clipped) {
          screen.forEach(p => { ctx.fillStyle = roles.selection; ctx.beginPath(); ctx.arc(p[0], p[1], 3, 0, 7); ctx.fill(); });
        }
        ghostCount++;
      });
    }
    root.dataset.dragGhostCount = String(ghostCount);
    if(s.ceilingMaterial!==null && s.ceilingExtent!=='infinite' && (state.document.editorFlags.layerModes?.ceilings||0)!==2) {
      ctx.strokeStyle=roles.ceiling;ctx.fillStyle=roles.ceiling;ctx.setLineDash([5,4]);
      for(const poly of s.ceilingFootprint||[]) { const visible=clipPolygonNear(poly.map(p=>[...p,s.heightM]),camera);if(!visible)continue;
        ctx.beginPath();visible.map(p=>project(p,camera,width,height)).forEach((p,i)=>i?ctx.lineTo(p[0],p[1]):ctx.moveTo(p[0],p[1]));ctx.closePath();ctx.globalAlpha=.035;ctx.fill();ctx.globalAlpha=.3;ctx.stroke();
      }ctx.globalAlpha=1;ctx.setLineDash([]);
    }
    rayLegend.hidden = !store.view.rays; rayHits = [];
    if (store.view.rays) {
      // The ray source stays on the last selected source when the selection is cleared (e.g. by orbiting the camera).
      const index = Math.min(store.view.selection.find(r => r.kind === 'source')?.index ?? store.raySource ?? 0, Math.max(0, state.document.sources.length - 1)), order = store.view.rayOrder ?? 2, engine = store.telemetry['rays.paths'];
      let paths = [], label = '';
      if (engine && bridge.mode !== 'mock' && engine.source === index) {
        paths = enginePaths(engine).map(p=>({...p,points:p.points.map(overlayPoint)}));
        label = `${paths.length ? 'ENGINE RAYS' : 'No engine paths'} · ${state.document.sources[index]?.name} · first order · live poses${engine.pointCount>1?` · all ${engine.pointCount} points`:''}${engine.truncated ? ' · truncated' : ''}${engine.stale ? ' · stale' : ''}`;
      }
      else if (bridge.mode === 'mock') {
        const source = pointsFor({ kind: 'source', index })[0], listener = pointsFor({ kind: 'listener', index: 0 })[0], key = JSON.stringify([s, source, listener]);
        if (rayCache.key !== key) rayCache = { key, paths: mockRays(source, listener, s) };
        paths = rayCache.paths; label = `MOCK RAYS · ${state.document.sources[index]?.name} · direct${order >= 1 ? ' / 1st' : ''}${order >= 2 ? ' / 2nd' : ''}`;
      } else label = 'Reflection paths unavailable · waiting for engine';
      paths.filter(p => p.order <= order).slice().reverse().forEach(path => {
        const style = rayStyle(path, { showOccluded: !!store.view.raysOccluded }); if (!style) return;
        const picked = pickedRay === path.id;
        ctx.strokeStyle = picked ? roles.selection : rayColour(path.order, roles,path.pointIndex); ctx.lineWidth = style.width + (picked ? .65 : 0); ctx.globalAlpha = picked ? 1 : style.alpha; ctx.setLineDash(style.dash);
        path.points.slice(1).forEach((p, i) => { const segment = projectedSegment(path.points[i], p, camera, width, height); if (segment) { line(...segment); rayHits.push({ path, a: segment[0], b: segment[1] }); } });
      });
      const escape=skyEscapeGuides(pointsFor({kind:'source',index})[0],s);
      ctx.strokeStyle=roles.opening;ctx.setLineDash([3,4]);ctx.globalAlpha=.65;ctx.lineWidth=1;
      for(const guide of escape)for(let j=1;j<guide.length;++j){const segment=projectedSegment(guide[j-1],guide[j],camera,width,height);if(segment)line(...segment);}
      if(escape.length)label+=' - sky escape guides (geometry)';
      ctx.setLineDash([]); ctx.globalAlpha = 1; rayLegend.textContent = label;
      if (pickedRay && !paths.some(p => p.id === pickedRay)) pickedRay = null;
      lastPaths = paths;
    }
    const labelBoxes=[],nextLabels=new Map();
    const pose = (ref, colour, text, labelColour = roles.labels, custom = false) => {
      const world = pointsFor(ref)[0], p = project(world, camera, width, height), yaw = state.parameters.values[ref.kind === 'listener' ? 'laz' : `s${ref.index + 1}az`] * Math.PI / 180;
      const isSelected = store.view.selection.some(r => same(r, ref));
      // A marker always stays at the projected physical point. Threshold-based
      // collision displacement made dense preset markers jump by 16 px in orbit.
      const marker = p;
      if(camera.view!=='plan') {ctx.strokeStyle=colour;ctx.globalAlpha=.55;ctx.lineWidth=1;ctx.setLineDash([4,4]);worldLine(world,[world[0],world[1],0]);ctx.setLineDash([]);ctx.globalAlpha=1;}
      if (isSelected && custom) {
        ctx.save(); ctx.strokeStyle = roles.selection; ctx.lineWidth = 5; ctx.globalAlpha = 0.35;
        ctx.beginPath(); ctx.arc(marker[0], marker[1], markerRadius(ref.kind, state.document.sources.length), 0, 7); ctx.stroke();
        ctx.restore();
      }
      ctx.strokeStyle = colour; ctx.fillStyle = colour; ctx.lineWidth = isSelected ? 2.5 : 1;
      ctx.beginPath(); ctx.arc(marker[0], marker[1], markerRadius(ref.kind, state.document.sources.length), 0, 7); ctx.stroke();
      ctx.beginPath(); ctx.arc(marker[0], marker[1], 3.5, 0, 7); ctx.fill();
      {// A source with an instrument profile always shows its full-length heading: the profile is aimed by it whatever the fitted analytic pattern is.
        const tickSource=ref.kind==='source'?state.document.sources[ref.index]:null,profiled=tickSource&&(tickSource.instrument||'none')!=='none';
        const ticks=tickSource&&!profiled?directivityTicks(camera,yaw,tickSource.pattern):{forward:orientationTick(camera,yaw),backward:null};
        for(const tick of [ticks.forward,ticks.backward])if(tick)line(marker,[marker[0]+tick[0],marker[1]+tick[1]]);
        // Profile mark: a filled dot on the tip of the heading tells a profiled source from an analytic one.
        if(profiled){ctx.save();ctx.fillStyle=ctx.strokeStyle;ctx.beginPath();ctx.arc(marker[0]+ticks.forward[0],marker[1]+ticks.forward[1],3,0,Math.PI*2);ctx.fill();ctx.restore();}
        if(ref.kind==='listener'&&state.document.output.micPattern===7){
          // A symbolic head stays readable at any zoom; ear capsules below remain
          // at the true receiver coordinates when the to-scale option is enabled.
          ctx.beginPath();ctx.ellipse(marker[0],marker[1],6,8,0,0,Math.PI*2);ctx.stroke();
          const yawScreen=Math.atan2(ticks.forward[1],ticks.forward[0]);ctx.save();ctx.translate(marker[0],marker[1]);ctx.rotate(yawScreen);
          ctx.beginPath();ctx.moveTo(6,-2);ctx.lineTo(11,0);ctx.lineTo(6,2);ctx.stroke();
          for(const side of [-1,1]){ctx.beginPath();ctx.ellipse(0,side*8,3,2,0,0,Math.PI*2);ctx.stroke();}ctx.restore();
        }}
      ctx.font = '11px Segoe UI, -apple-system, BlinkMacSystemFont, sans-serif'; ctx.fillStyle = labelColour;
      const label=[marker[0]+11,marker[1]-12];
      const box={x:label[0],y:label[1]-11,w:ctx.measureText(text).width+6,h:15};
      // Freeze collision visibility for an orbit gesture; labels may overlap
      // temporarily but never blink at a threshold while the camera is moving.
      const overlaps=labelBoxes.some(b=>box.x<b.x+b.w&&box.x+box.w>b.x&&box.y<b.y+b.h&&box.y+box.h>b.y);
      const labelKey=`${ref.kind}:${ref.index}`;
      const visible=isSelected||(['orbit','pan'].includes(drag?.type)?(drag.labels.get(labelKey)??!overlaps):!overlaps);
      nextLabels.set(labelKey,visible);
      if(visible) { ctx.fillText(text,...label); labelBoxes.push(box); }
      hitCache.push({ ref, screen: [marker], radius: 11 });
    };
    pose({ kind: 'listener', index: 0 }, roles.listener, 'Listener');
    state.document.sources.map((source,index)=>({source,index})).sort((a,b)=>Number(store.view.selection.some(r=>r.kind==='source'&&r.index===b.index))-Number(store.view.selection.some(r=>r.kind==='source'&&r.index===a.index))).forEach(({source,index})=>pose({kind:'source',index},sourceMarkerColour(source,roles),source.name,sourceLabelColour(source,roles),isValidSourceColorHex(source.color)));
    labelVisibility=nextLabels;
    state.document.sources.forEach((source, index) => {
      if (source.sum || source.channels === 1) return;
      const origin = project(pointsFor({ kind: 'source', index })[0], camera, width, height);
      const pointColour = sourceMarkerColour(source, roles);
      sourcePoints(state, index).forEach(point => {
        const p = project(point.position, camera, width, height);
        ctx.strokeStyle = pointColour; ctx.globalAlpha = 0.4; line(origin, p); ctx.globalAlpha = 1; ctx.fillStyle = pointColour; ctx.beginPath(); ctx.arc(p[0], p[1], 3, 0, 7); ctx.fill();
        if (source.freePoints) hitCache.push({ ref: { kind: 'source', index, channel: point.channel }, screen: [p], radius: 6 });
      });
    });
    if (store.view.showMic) {
      const mics = receivers(state), projected = mics.map((mic, index) => ({ mic, index, p: project(mic.position, camera, width, height) }));
      projected.forEach(({ mic, index, p }) => {
        ctx.strokeStyle = mic.rendered ? roles.listener : '#687375'; ctx.fillStyle = ctx.strokeStyle;
        if (mic.lfe) ctx.strokeRect(p[0] - 3, p[1] - 3, 6, 6); else { ctx.beginPath(); ctx.arc(p[0], p[1], 3, 0, 7); ctx.stroke(); }
        if (state.document.output.micPattern === 7 && !mic.lfe) { ctx.beginPath(); ctx.arc(p[0], p[1], 4, index === 0 ? .5 : Math.PI + .5, index === 0 ? Math.PI * 1.5 : Math.PI * 2.5); ctx.stroke(); }
        if (!mic.lfe && state.document.output.micPattern !== 7 && !(/^ACN\d+$/.test(mic.name) && index > 0)) {  // identical omni rings of coincident ACN capsules are drawn once
          ctx.beginPath();
          for (let i = 0; i <= 60; i++) {
            const angle = i / 60 * Math.PI * 2, gain = Math.abs(1 - mic.pattern + mic.pattern * Math.cos(angle)), yaw = mic.azimuth * Math.PI / 180 + angle;
            const q = project(add(mic.position, [Math.cos(yaw) * gain * 0.25, Math.sin(yaw) * gain * 0.25, 0]), camera, width, height);
            i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1]);
          } ctx.stroke();
        }
        // AmbiX 2nd/3rd order: horizontal-plane sections of the sectorial harmonics, cos(n*phi) and sin(n*phi) = 2n petals per
        // order (order 1: the two figure-8 above is the 4-petal pair's n=1 case; order 2: clover; order 3: six-petal rosette).
        // Lower orders are drawn thinner and fainter so the top order reads first.
        if (/^ACN\d+$/.test(mic.name) && index === 0 && mics.length >= 9) {
          const top = Math.round(Math.sqrt(mics.length)) - 1, savedAlpha = ctx.globalAlpha, savedWidth = ctx.lineWidth;
          for (let n = 1; n <= top; n++) {
            ctx.globalAlpha = savedAlpha * (n === top ? 1 : 0.35); ctx.lineWidth = n === top ? 1 : 0.7;
            for (const phase of [0, Math.PI / (2 * n)]) {
              ctx.beginPath();
              for (let i = 0; i <= 120; i++) {
                const angle = i / 120 * Math.PI * 2, gain = Math.abs(Math.cos(n * (angle - phase))), yaw = mic.azimuth * Math.PI / 180 + angle;
                const q = project(add(mic.position, [Math.cos(yaw) * gain * 0.25, Math.sin(yaw) * gain * 0.25, 0]), camera, width, height);
                i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1]);
              } ctx.stroke();
            }
          }
          ctx.globalAlpha = savedAlpha; ctx.lineWidth = savedWidth;
        }
      });
      // T11 item 6: group coincident capsules (within 2.5 px in 3D/screen space, e.g. C · LFE or XY L · R)
      // and place labels around capsules without overlapping each other or existing pose labels.
      const groups = [];
      for (const item of projected) {
        const g = groups.find(g => Math.hypot(g.p[0] - item.p[0], g.p[1] - item.p[1]) < 2.5 && Math.hypot(...g.world.map((v, i) => v - item.mic.position[i])) < 1e-3);
        if (g) { g.names.push(item.mic.name); g.rendered ||= item.mic.rendered; }
        else groups.push({ p: item.p, world: item.mic.position, names: [item.mic.name], rendered: item.mic.rendered });
      }
      const listenerScreen = project(pointsFor({ kind: 'listener', index: 0 })[0], camera, width, height);
      ctx.font = '11px Segoe UI, -apple-system, BlinkMacSystemFont, sans-serif';
      const micTexts = [], micBoxes = [];
      const overlaps = (a, b) => a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
      const angleSteps = [0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 7, -7, 8].map(k => k * Math.PI / 8);
      groups.forEach((g, gi) => {
        const acn = g.names.length > 3 && g.names.every(n => /^ACN\d+$/.test(n));  // AmbiX 2nd/3rd order: one short label (order and channel count) for the whole coincident set
        const text = acn ? `AmbiX ${g.names.length === 9 ? '2nd' : '3rd'} order · ${g.names.length} ch` : g.names.join(' · '), tw = ctx.measureText(text).width, th = 13;
        const dx = g.p[0] - listenerScreen[0], dy = g.p[1] - listenerScreen[1], dist = Math.hypot(dx, dy);
        const baseAngle = dist > 1.5 ? Math.atan2(dy, dx) : -Math.PI / 2 + gi * (2 * Math.PI / Math.max(1, groups.length));
        const candidates = [];
        for (const r of [9, 18, 28, 38, 50, 62, 76, 92, 110]) {
          for (const da of angleSteps) {
            const a = baseAngle + da, cx = g.p[0] + Math.cos(a) * r, cy = g.p[1] + Math.sin(a) * r;
            const lx = Math.cos(a) >= 0.2 ? cx : Math.cos(a) <= -0.2 ? cx - tw : cx - tw / 2;
            const ly = cy + 4;
            candidates.push({ x: lx, y: ly - 10, w: tw + 4, h: th, tx: lx, ty: ly });
          }
        }
        const chosen = candidates.find(c => c.x >= 2 && c.x + c.w <= width - 2 && c.y >= 2 && c.y + c.h <= height - 2 && !labelBoxes.some(b => overlaps(c, b)))
          || candidates.find(c => !labelBoxes.some(b => overlaps(c, b))) || candidates[0];
        ctx.fillStyle = g.rendered ? roles.listener : '#687375';
        ctx.fillText(text, chosen.tx, chosen.ty);
        labelBoxes.push(chosen);
        micBoxes.push(chosen);
        micTexts.push(text);
      });
      let micOverlapCount = 0;
      for (let i = 0; i < micBoxes.length; i++) for (let j = i + 1; j < micBoxes.length; j++) if (overlaps(micBoxes[i], micBoxes[j])) micOverlapCount++;
      root.dataset.micLabels = JSON.stringify(micTexts);
      root.dataset.micLabelOverlaps = String(micOverlapCount);
    } else {
      delete root.dataset.micLabels;
      delete root.dataset.micLabelOverlaps;
    }
      // T12 item 2: wall segment length label(s) in Edit room (selected wall(s) and live drag / vertex move).
    if (store.view.page === 'edit' && !preview) {
      const selectedWalls = [...wouldSelect, ...moving].filter(r => r.kind === 'wall');
      const liveWalls = new Map();
      for (const r of selectedWalls) {
        const w = surface(s, r);
        if (!w || w.editor.hidden) continue;
        if (moving.some(m => same(m, r))) {
          const [a, top, , b] = ghostPointsFor(r, s, d);
          liveWalls.set(r.index, { a: a.slice(0, 2), b: b.slice(0, 2), zBottomM: Math.max(0, a[2]), zTopM: top[2] });
        } else liveWalls.set(r.index, w);
      }
      const wallLabelTexts = [];
      ctx.font = '600 11px Segoe UI, -apple-system, BlinkMacSystemFont, sans-serif';
      for (const [, w] of liveWalls) {
        const lenM = wallLengthM(w);
        const text = formatMetres2(lenM);
        const topZ = (w.zTopM ?? s.heightM) < 1e5 ? (w.zTopM ?? s.heightM) : s.heightM;
        const midZ = camera.view === 'plan' ? 0 : (w.zBottomM + topZ) * 0.5;
        const pa = project([w.a[0], w.a[1], midZ], camera, width, height);
        const pb = project([w.b[0], w.b[1], midZ], camera, width, height);
        if (pa[2] <= 0 && pb[2] <= 0) continue;
        const mx = (pa[0] + pb[0]) * 0.5, my = (pa[1] + pb[1]) * 0.5;
        const sdx = pb[0] - pa[0], sdy = pb[1] - pa[1], slen = Math.hypot(sdx, sdy) || 1;
        const nx = -sdy / slen, ny = sdx / slen;
        const tw = ctx.measureText(text).width, padX = 5, bh = 16, bw = tw + padX * 2;
        const lx = Math.max(4, Math.min(width - bw - 4, mx + nx * 14 - bw * 0.5));
        const ly = Math.max(26, Math.min(height - bh - 26, my + ny * 14 - bh * 0.5));
        ctx.fillStyle = '#0d1519e6';
        ctx.strokeStyle = roles.selection;
        ctx.lineWidth = 1;
        ctx.fillRect(lx, ly, bw, bh);
        ctx.strokeRect(lx + 0.5, ly + 0.5, bw - 1, bh - 1);
        ctx.fillStyle = '#eff8fb';
        ctx.fillText(text, lx + padX, ly + 11.5);
        wallLabelTexts.push(text);
      }
      if (selectedWalls.length > 1) {
        const totalLen = selectedWalls.reduce((sum, r) => sum + wallLengthM(liveWalls.get(r.index) || surface(s, r)), 0);
        root.dataset.selectedWallSummary = `${formatMetres2(totalLen)} · ${selectedWalls.length} walls`;
      } else if (selectedWalls.length === 1) {
        const only = liveWalls.get(selectedWalls[0].index) || surface(s, selectedWalls[0]);
        root.dataset.selectedWallSummary = formatMetres2(wallLengthM(only));
      } else {
        delete root.dataset.selectedWallSummary;
      }
      root.dataset.wallLabels = JSON.stringify(wallLabelTexts);
      // Update inspector readout live while dragging a vertex
      if (drag?.type === 'vertex' && drag.ref?.kind === 'wall') {
        const readout = document.querySelector('.edit-inspector .wall-length-readout');
        if (readout && liveWalls.has(drag.ref.index)) {
          const liveLen = wallLengthM(liveWalls.get(drag.ref.index));
          readout.textContent = formatMetres2(liveLen);
          readout.dataset.wallLength = liveLen.toFixed(4);
        }
      }
    } else {
      delete root.dataset.selectedWallSummary;
      delete root.dataset.wallLabels;
    }
    marquee.hidden=drag?.type!=='box';
    if(!marquee.hidden){const lo=drag.start.map((v,i)=>Math.min(v,drag.last[i]));Object.assign(marquee.style,{left:`${lo[0]}px`,top:`${lo[1]}px`,width:`${Math.abs(drag.last[0]-drag.start[0])}px`,height:`${Math.abs(drag.last[1]-drag.start[1])}px`});root.dataset.marqueeCount=String(wouldSelect.length);}
    root.dataset.draftPoints=String(draft.length);
    if (draft.length) {
      ctx.strokeStyle=roles.selection;ctx.fillStyle=roles.background;ctx.lineWidth=2;
      const points=store.view.tool==='room'&&draftCursor?rectangle(draft[0],draftCursor.world):draft;
      ctx.beginPath();points.forEach((p,i)=>{const q=project([...p,0],camera,width,height);i?ctx.lineTo(q[0],q[1]):ctx.moveTo(q[0],q[1]);});
      if(store.view.tool==='room')ctx.closePath();ctx.stroke();
      if(draftCursor&&store.view.tool!=='room'){
        ctx.setLineDash([5,3]);worldLine([...draft.at(-1),0],[...draftCursor.world,0]);
        if(draft.length>=2&&store.view.tool!=='wall')worldLine([...draftCursor.world,0],[...draft[0],0]);ctx.setLineDash([]);
      }
      if(preview){const top=store.view.tool==='plate'?1:s.heightM;ctx.setLineDash([4,3]);for(const p of points)worldLine([...p,0],[...p,top]);for(let i=1;i<points.length;i++)worldLine([...points[i-1],top],[...points[i],top]);if(['room','floorZone','ceilingZone','plate'].includes(store.view.tool)&&points.length>2)worldLine([...points.at(-1),top],[...points[0],top]);ctx.setLineDash([]);}
      for(const p of points){const q=project([...p,0],camera,width,height);ctx.beginPath();ctx.arc(q[0],q[1],5,0,Math.PI*2);ctx.fill();ctx.stroke();}
    }
    const readout = drag?.snapReadout || (isDrawing() && camera.view === 'plan' ? draftCursor : null);
    drawingReadout.hidden = !readout;
    if(!drawingReadout.hidden){
      const q=readout,from=drag?.snapReadout ? q.from : draft.at(-1),dx=from?q.world[0]-from[0]:0,dy=from?q.world[1]-from[1]:0;
      if (!drag?.snapReadout && store.view.tool === 'room' && from) {
        drawingReadout.textContent = `${Math.abs(dx).toFixed(2)} × ${Math.abs(dy).toFixed(2)} m · ${q.snap}`;
      } else {
        drawingReadout.textContent=`${Math.hypot(dx,dy).toFixed(2)} m · ${Math.atan2(dy,dx)*180/Math.PI<0?'-':''}${Math.abs(Math.atan2(dy,dx)*180/Math.PI).toFixed(1)}° · ${q.snap}`;
      }
      drawingReadout.style.left=`${Math.max(4,Math.min(width-245,q.screen[0]+14))}px`;drawingReadout.style.top=`${Math.max(30,Math.min(height-45,q.screen[1]+18))}px`;
      if(q.snap!=='free'){const p=project([...q.world,q.z ?? 0],camera,width,height);ctx.strokeStyle=roles.selection;ctx.lineWidth=1;ctx.strokeRect(p[0]-8,p[1]-8,16,16);}
    }
    // T12 item 1: grid scale indicator (scale bar + grid step) in bottom-right corner.
    if (!preview) {
      const sc = scaleIndicatorState(bounds, camera, height);
      scaleBarLine.style.width = `${sc.pixels}px`;
      scaleBarText.textContent = sc.caption;
      scaleIndicator.dataset.scaleMetres = String(sc.metres);
      scaleIndicator.dataset.scalePixels = String(sc.pixels);
      scaleIndicator.dataset.gridStep = String(sc.stepM);
      scaleIndicator.classList.toggle('above-ray-legend', !rayLegend.hidden);
    }
    badge.textContent = `${camera.view.toUpperCase()} / METRES · ${s.walls.length} walls · ${store.view.page==='edit'?`Unscaled · Size ${authorStore.state.parameters.values.size??100} % applied on Main`:`Size ${authorStore.state.parameters.effective.sizePercent??100} %`}${store.view.energy ? camera.view !== 'plan' ? ' · Energy unavailable in this view' : bridge.mode === 'mock' ? ' · SYNTHETIC ENERGY (mock)' : ' · Energy' : ''}`;
    root.dataset.drawCount = String(Number(root.dataset.drawCount || 0) + 1);
    if(!preview)syncPreview();
  }
  // T6 B4: the grid follows zoom with the same step the auto snap uses (1 m / 50 cm / 10 cm / 5 cm / 1 cm).
  function drawFineGrid(bounds) {
    if (camera.view === '3d') return;
    const step = autoStep(camera, height);
    if (step >= gridStep(bounds, camera.view) || step * pixelsPerMetre(camera, height) < 6) return;
    const axes = camera.view === 'front' ? [0, 2] : camera.view === 'side' ? [1, 2] : [0, 1];
    const p0 = pointOnTargetPlane([0, 0], camera, width, height), p1 = pointOnTargetPlane([width, height], camera, width, height);
    ctx.lineWidth = 0.5;
    for (const { a, b, major } of visibleGrid(camera.view, [p0[axes[0]], p0[axes[1]]], [p1[axes[0]], p1[axes[1]]], step)) { ctx.globalAlpha = major ? 0.3 : 0.15; worldLine(a, b); }
  }
  let reportedStep = null;
  const snapStep = () => effectiveStep(store.view, camera, height);
  function publishSnapStep() { const step = snapStep(); if (step !== reportedStep) { reportedStep = step; root.dataset.snapStep = String(step); root.dispatchEvent(new CustomEvent('snapstep', { detail: step })); } }
  const isDrawing=()=>store.view.page==='edit'&&['room','wall','floorZone','ceilingZone','plate'].includes(store.view.tool);
  const rectangle=(a,b)=>[[a[0],a[1]],[b[0],a[1]],[b[0],b[1]],[a[0],b[1]]];
  function drawingPoint(p,e){
    const raw=pointOnTargetPlane(p,camera,width,height).slice(0,2);
    const { scene, editorFlags } = store.state.document;
    const corners = [...visibleCorners(scene, editorFlags.layerModes || {}), ...draft.map(xy => ({ xy, heights: [0] }))];
    return snapVertex(raw, p, corners, q => project(q, camera, width, height), snapStep(), e.altKey);
  }
  function surfaceDragPlane(d, p) {
    if (d.camera.view === '3d' && d.hitZ != null) {
      const a = pointOnHeightPlane(d.start, d.camera, width, height, d.hitZ);
      const b = pointOnHeightPlane(p, d.camera, width, height, d.hitZ);
      if (a && b) return [a, b, true];
    }
    return [d.start, p].map(q => pointOnTargetPlane(q, d.camera, width, height));
  }
  function updateGeometryDrag(d, p, e, a, b, onHeightPlane) {
    const scene = d.startScene, modes = store.state.document.editorFlags.layerModes || {};
    const view = d.camera.view, autoLink = store.view.autoLink !== false;
    const free = e.altKey || (view === '3d' && !onHeightPlane);
    const projectCorner = q => {
      const point = project(view === '3d' ? [q[0], q[1], d.hitZ] : q, d.camera, width, height);
      return view === '3d' && point[2] <= .1 ? [Infinity, Infinity] : point;
    };
    const candidates = free ? [] : visibleCorners(scene, modes);
    const unlocked = refs => !refs.some(r => surface(scene, r).editor.locked || modes[layerKey(scene, r)]);
    const { kind, index, vertex } = d.ref;
    if (d.type === 'vertex') {
      const ref = { kind, index, vertex };
      d.movedVertices = movedVertexCorners(scene, ref, autoLink, modes.walls);
      const available = candidates.filter(c => !d.movedVertices.some(r => sameCorner(c.ref, r)));
      const result = snapVertex(b.slice(0, 2), p, available, projectCorner, snapStep(), free);
      d.movingRefs = unlocked([ref]) ? d.movedVertices : [];
      d.liveVertexPos = result.world;
      d.snapReadout = { ...result, z: view === '3d' ? d.hitZ : result.z, from: cornerXY(scene, ref) };
      d.final = { op: 'surface.moveVertex', ref: { kind, index }, vertex, positionM: result.world, autoLink,
        ...(result.target ? { snapTarget: result.target } : {}) };
      return;
    }
    const refs = d.refs.map(({ kind, index }) => ({ kind, index })), includeGroups = !e.altKey;
    const moved = expandedRefs(scene, refs, includeGroups).filter(r => !['floor', 'ceiling'].includes(r.kind));
    const allowed = unlocked(moved);
    const links = allowed && autoLink ? translatedWallLinks(scene, moved, modes.walls) : new Map();
    const delta = b.map((v, i) => v - a[i]).slice(0, view === 'plan' || view === '3d' ? 2 : 3);
    const movingWalls = allowed && !free ? moved.filter(r => r.kind === 'wall').flatMap(r => wallCorners(scene.walls[r.index], r.index, scene.heightM)) : [];
    const available = candidates.filter(({ ref }) => !moved.some(r => same(r, ref)) &&
      !(ref.kind === 'wall' && links.get(ref.index)?.includes(ref.vertex === 0 ? 'a' : 'b')));
    const hit = snapTranslation(delta, movingWalls, available, projectCorner, free);
    d.liveDelta = hit?.delta ?? delta;
    d.snapFrom = hit ? cornerXY(scene, hit.from) : null;
    d.snapReadout = hit ? { world: hit.world, screen: p, z: view === '3d' ? d.hitZ : hit.z, snap: 'corner', from: d.snapFrom } : null;
    d.linkedWalls = d.liveDelta[0] || d.liveDelta[1] ? links : new Map();
    d.movingRefs = allowed && d.liveDelta.some(v => Math.abs(v) > 1e-6) ?
      [...moved, ...[...d.linkedWalls.keys()].map(index => ({ kind: 'wall', index }))] : [];
    d.final = { op: 'surface.translate', refs, deltaM: d.liveDelta, includeGroups, autoLink,
      ...(hit ? { snapFrom: hit.from, snapTarget: hit.target } : {}) };
  }
  function endDraft(){chainGroup=null;draft=[];draftCursor=null;drawingReadout.hidden=true;schedule();}
  // Undo while a chain is being drawn takes back the LAST CLICK only: the wall that click created (history) and its draft point, so the
  // next click continues from the previous point; a lone first point is just dropped. Returns true when it consumed the shortcut.
  function undoDraft(){
    if(!isDrawing()||store.view.tool==='room'||!draft.length)return false;
    if(store.view.tool==='wall'&&draft.length>=2)store.command({op:'history.undo'});
    draft.pop();draftCursor=null;if(!draft.length)endDraft();else schedule();return true;
  }
  function closeShape(){
    if(store.view.tool==='wall'){
      if(draft.length>=3)store.command(wallCommand(draft.at(-1),draft[0]));
    }else if(draft.length>=3)store.command({op:'surface.create',kind:store.view.tool==='plate'?'floorZone':store.view.tool,vertices:draft,heightM:store.view.tool==='plate'?1:null});
    endDraft();
  }
  function drawGroup(){
    if(!store.view.autoLink)return undefined;
    if(!chainGroup){const groups=new Set(surfaceRefs(store.state.document.scene).map(r=>surface(store.state.document.scene,r).editor.group));let name='Construction',i=2;while(groups.has(name))name=`Construction ${i++}`;chainGroup=name;}
    return chainGroup;
  }
  function wallCommand(a,b){const groupName=drawGroup();return {op:'surface.create',kind:'wall',a,b,...(groupName?{groupName}:{})};}
  function marqueeSelection(){
    const hits=hitCache.filter(h=>h.ref.vertex===undefined&&h.ref.channel===undefined&&canSelect(h.ref)&&touchesBox(h.screen,drag.start,drag.last,h.polygon));
    return boxSelection(drag.base,hits.map(h=>h.ref),drag.modifiers);
  }
  function canSelect(ref){if(['source','listener'].includes(ref.kind))return true;const obj=surface(store.state.document.scene,ref);return obj&&!obj.editor.hidden&&!obj.editor.locked&&!(store.state.document.editorFlags.layerModes?.[layerKey(store.state.document.scene,ref)]||0);}
  function syncPreview(){
    const show=store.view.page==='edit'&&camera.view!=='3d'&&(store.view.perspectivePreview??isDrawing());
    if(!show){if(previewViewport){previewViewport.dispose();inset.remove();previewViewport=previewStore=inset=null;}return;}
    if(!previewViewport){
      const local={...store.view,page:'preview',projection:'3d',selection:[],energy:false,rays:false,camera:store.view.previewCamera||null};
      previewStore={get state(){return store.state;},get ready(){return store.ready;},get telemetry(){return store.telemetry;},view:local,subscribe:fn=>authorStore.subscribe(fn),command:()=>Promise.resolve(),setView(patch){Object.assign(local,patch);if(patch.camera)store.setView({previewCamera:patch.camera},true);}};
      inset=element('div',{className:'perspective-inset'});
      previewViewport=createViewport(previewStore,bridge,{preview:true,liveState:()=>drag,draftProvider:()=>{
        let outline=store.view.tool==='room'&&draftCursor&&draft.length?rectangle(draft[0],draftCursor.world):[...draft,...(draftCursor&&draft.length?[draftCursor.world]:[])];
        const top=store.view.tool==='plate'?1:store.state.document.scene.heightM;
        return {outline,points:outline.flatMap(p=>[[...p,0],[...p,top]])};
      }});inset.append(previewViewport);root.append(inset);
    }
    previewStore.view.tool=store.view.tool;previewViewport.redraw();
  }
  function line(a, b) { ctx.beginPath(); ctx.moveTo(a[0], a[1]); ctx.lineTo(b[0], b[1]); ctx.stroke(); }
  function worldLine(a,b) { const segment=projectedSegment(a,b,camera,width,height); if(segment) line(...segment); }
  function rayAt(p) { let best = null, dist = 5; for (const h of rayHits) { const d = segmentDistance(p, h.a, h.b); if (d < dist) { dist = d; best = h; } } return best; }
  function screen(e) { const r = canvas.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; }
  function pick(p, cycling = false) {
    const priority = h => ['source','listener'].includes(h.ref.kind) ? 4 : h.ref.vertex !== undefined ? 3 : h.ref.kind === 'wall' ? 2 : 1;
    let hits = hitCache.slice().reverse().filter(h => {
      if (!['source','listener'].includes(h.ref.kind)) {
        const obj=surface(store.state.document.scene,h.ref), layer=store.state.document.editorFlags.layerModes?.[layerKey(store.state.document.scene,h.ref)] || 0;
        if(!obj || obj.editor.hidden || obj.editor.locked || layer!==0)return false;
      }
      return h.radius ? Math.hypot(p[0] - h.screen[0][0], p[1] - h.screen[0][1]) <= (priority(h)===4 ? Math.max(16,h.radius) : h.radius) : h.polygon && inside(p, h.screen) || h.screen.some((a, i) => segmentDistance(p, a, h.screen[(i + 1) % h.screen.length]) < 6);
    });
    if (!hits.length) return null;
    const top=Math.max(...hits.map(priority)); hits=hits.filter(h=>priority(h)===top);
    if(top===4) return hits.sort((a,b)=>Math.hypot(p[0]-a.screen[0][0],p[1]-a.screen[0][1])-Math.hypot(p[0]-b.screen[0][0],p[1]-b.screen[0][1]))[0].ref;
    if (cycling && Math.hypot(p[0] - cycle.point[0], p[1] - cycle.point[1]) < 4) cycle.index = (cycle.index + 1) % hits.length; else cycle.index = 0;
    cycle.point = p; return hits[cycle.index % hits.length].ref;
  }
  function persistCamera() { if (persistTimer !== null) { clearTimeout(persistTimer); persistTimer = null; } store.setView({ camera }, true); }
  function persistCameraDebounced() { if (persistTimer !== null) clearTimeout(persistTimer); persistTimer = setTimeout(() => { persistTimer = null; if (!disposed) store.setView({ camera }, true); }, 120); }
  function flushPose() {
    if(poseTimer!==null){clearTimeout(poseTimer);poseTimer=null;}
    if(!pendingPose||!drag?.ids)return;
    const pose=pendingPose;pendingPose=null;
    if(drag.ids.some(id=>bridge.parameters.isActive?.(id)===false))return;
    for(const [id,value] of Object.entries(pose))bridge.parameters.change(id,positionId(id)?canonical(authorStore.state.parameters.descriptors.parameters.find(d=>d.id===id),value/sizeFactor()):value);
    lastPoseTime=performance.now();
  }
  function finish(cancel=false,restore=true){
    if(!drag)return;
    const ending=drag;
    try{
      const selection=ending.type==='box'&&!cancel?marqueeSelection():null;
      if(cancel&&ending.ids)pendingPose=restore?Object.fromEntries(ending.ids.map(id=>[id,ending.initial[id]])):null;
      flushPose();
      if(ending.tx)store.command({op:cancel?'transaction.cancel':'transaction.commit'},ending.tx);
      if(selection)store.setView({selection});
      if(['pan','orbit'].includes(ending.type))persistCamera();
    }finally{
      drag=null;pendingPose=null;
      for(const id of ending.ids||[])bridge.parameters.end(id);
      schedule();root.dispatchEvent(new Event('interactionend'));
    }
  }
  function resetPointer(restore=false){try{finish(true,restore);}catch(error){drag=null;pendingPose=null;console.error('Verb gesture reset:',error);}schedule();}
  let pathHits=[], pathSegHits=[], draftNodeHits=[];
  function updateHint(){
    if(preview)return;
    const drawing=store.view.pathDrawing!=null && !mouseLocked();
    const editing=store.view.pathEditing!=null && !mouseLocked();
    root.classList.toggle('is-drawing-path', drawing);
    canvas.style.cursor=drawing?'crosshair':'';
    hint.textContent=mouseLocked()?'Locked · select / camera only'
      :drawing?(width<520?'Click add · Drag node move · Ctrl+Z/Y · Dbl-click/Enter finish · Esc cancel':'Click empty space to add point · Drag node to move · Ctrl+Z/Y point · Double-click / Enter finishes · Esc cancels')
      :editing?(width<520?'Drag node/curve · Click empty add · RMB/Del remove · Esc done':'Drag node to move · Drag curve to insert · Click empty space to append · Right-click/Del removes · Esc done')
      :width<450?'RMB face cursor · wheel zoom · Shift pan':'Drag objects · RMB face cursor · wheel zoom · M / Shift-drag pan · F frame · double-click centre';
  }
  function drawArrowHead(tip, from, size = 8) {
    if (!tip || !from || (camera.view === '3d' && (tip[2] <= 0 || from[2] <= 0))) return;
    const dx = tip[0] - from[0], dy = tip[1] - from[1], len = Math.hypot(dx, dy);
    if (len < 1.5) return;
    const ang = Math.atan2(dy, dx), spread = 0.44;
    ctx.beginPath();
    ctx.moveTo(tip[0], tip[1]);
    ctx.lineTo(tip[0] - Math.cos(ang - spread) * size, tip[1] - Math.sin(ang - spread) * size);
    ctx.lineTo(tip[0] - Math.cos(ang + spread) * size, tip[1] - Math.sin(ang + spread) * size);
    ctx.closePath();
    ctx.fill();
  }
  function effectivePathForDraw(p, o) {
    const d = activeDrag();
    if (!p || d?.type !== 'pathPoint' || d.object !== o || !d.livePoint) return p;
    const pts = p.points ? p.points.map(q => q.slice()) : [];
    if (d.isNew && pts.length === d.baseCount) pts.splice(d.index, 0, d.livePoint.slice());
    else if (d.index >= 0 && d.index < pts.length) pts[d.index] = d.livePoint.slice();
    return { ...p, points: pts };
  }
  function drawMovementPaths(roles) {
    pathHits=[]; pathSegHits=[]; draftNodeHits=[];
    const paths=authorStore.state.document.paths||[];
    const selObj=store.view.selection.find(r=>r.kind==='source'||r.kind==='listener');
    const activeObj=store.view.pathDrawing??store.view.pathEditing??(selObj?(selObj.kind==='listener'?32:selObj.index):null);
    const drawPath=(raw,o,isDraft=false)=>{
      const p=isDraft?raw:effectivePathForDraw(raw,o);
      if(!p?.points?.length || (!isDraft && p.points.length<2))return;
      const isOldRedraw=!isDraft && store.view.pathDrawing===o;
      const isActive=isDraft || (!isOldRedraw && (o===activeObj || o===store.view.pathEditing));
      const table=compilePath(p),f=sizeFactor();
      const lineCol=isActive?(roles.pathLine||(o===32?roles.listener:roles.source)):(roles.pathDimmed||roles.grid);
      const nodeCol=roles.pathNode||lineCol;
      const arrowCol=isActive?(roles.pathArrow||lineCol):(roles.pathDimmed||lineCol);
      const markerCol=isOldRedraw?(roles.pathDimmed||lineCol):(roles.pathMarker||roles.selection);
      const screenPts=table.points.map(q=>project(q.map(x=>x*f),camera,width,height));
      ctx.save();
      ctx.strokeStyle=ctx.fillStyle=lineCol;
      ctx.globalAlpha=isActive?0.95:(isOldRedraw?0.5:0.45);
      ctx.lineWidth=isActive?2.2:(isOldRedraw?1.1:1.15);
      if(isOldRedraw)ctx.setLineDash([5,4]);
      ctx.beginPath();screenPts.forEach((v,i)=>{i?ctx.lineTo(v[0],v[1]):ctx.moveTo(v[0],v[1]);});ctx.stroke();
      if(isOldRedraw)ctx.setLineDash([]);
      if(!isDraft && !isOldRedraw && !preview)pathSegHits.push({object:o,worldPts:table.points,screenPts,steps:table.steps,pointCount:p.points.length});
      if(screenPts.length>=2){
        ctx.fillStyle=arrowCol;
        if(!p.closed){
          const tip=screenPts.at(-1);
          let fromIdx=screenPts.length-2;
          while(fromIdx>0 && Math.hypot(tip[0]-screenPts[fromIdx][0],tip[1]-screenPts[fromIdx][1])<3)fromIdx--;
          drawArrowHead(tip,screenPts[fromIdx],isActive?18:13);
        }else{
          const segSamples=Math.max(1,table.steps||1);
          const midIdx=Math.min(screenPts.length-1,Math.max(1,Math.floor(segSamples*0.5)));
          let prevIdx=midIdx-1;
          while(prevIdx>0 && Math.hypot(screenPts[midIdx][0]-screenPts[prevIdx][0],screenPts[midIdx][1]-screenPts[prevIdx][1])<3)prevIdx--;
          drawArrowHead(screenPts[midIdx],screenPts[prevIdx],isActive?17:12);
          if(screenPts.length>segSamples*2){
            const idx2=Math.min(screenPts.length-1,Math.floor(screenPts.length*0.65));
            let p2=idx2-1;while(p2>0 && Math.hypot(screenPts[idx2][0]-screenPts[p2][0],screenPts[idx2][1]-screenPts[p2][1])<3)p2--;
            drawArrowHead(screenPts[idx2],screenPts[p2],isActive?16:11);
          }
        }
      }
      if(!isDraft && p.enabled && p.points.length>=2){
        const u=authorStore.state.parameters.values[o===32?'lpath':`s${o+1}path`]??0;
        const curWorld=evaluatePath(p,u).map(x=>x*f),cm=project(curWorld,camera,width,height);
        if(camera.view!=='3d'||cm[2]>0){
          ctx.save();ctx.strokeStyle=markerCol;ctx.fillStyle=markerCol;ctx.globalAlpha=isActive?0.95:0.45;ctx.lineWidth=1.6;
          ctx.beginPath();ctx.moveTo(cm[0],cm[1]-6);ctx.lineTo(cm[0]+6,cm[1]);ctx.lineTo(cm[0],cm[1]+6);ctx.lineTo(cm[0]-6,cm[1]);ctx.closePath();ctx.stroke();
          ctx.beginPath();ctx.arc(cm[0],cm[1],2.4,0,Math.PI*2);ctx.fill();
          ctx.restore();
        }
      }
      p.points.forEach((q,index)=>{
        const v=project(q.map(x=>x*f),camera,width,height);
        if(camera.view==='3d'&&v[2]<=0)return;
        if(!isActive){
          ctx.strokeStyle=lineCol;ctx.lineWidth=1;ctx.globalAlpha=0.45;
          ctx.strokeRect(v[0]-2.5,v[1]-2.5,5,5);
          return;
        }
        ctx.globalAlpha=1;ctx.strokeStyle=ctx.fillStyle=nodeCol;ctx.lineWidth=1.4;
        const isSel=!isDraft&&store.view.pathPoint?.object===o&&store.view.pathPoint?.index===index;
        if(isSel){ctx.save();ctx.strokeStyle=roles.selection;ctx.fillStyle=roles.selection;ctx.lineWidth=2;ctx.fillRect(v[0]-5,v[1]-5,10,10);ctx.strokeRect(v[0]-7,v[1]-7,14,14);ctx.restore();}
        else ctx.strokeRect(v[0]-4,v[1]-4,8,8);
        ctx.save();ctx.font='600 10px Segoe UI, -apple-system, BlinkMacSystemFont, sans-serif';ctx.fillStyle=isSel?roles.selection:nodeCol;ctx.fillText(String(index+1),v[0]+7,v[1]-6);ctx.restore();
        if(isDraft)draftNodeHits.push({index,screen:v,point:q});
        else pathHits.push({object:o,index,screen:v,point:q});
      });
      ctx.restore();
    };
    // Paths toggle (viewport toolbar): hidden paths are neither drawn nor hit-testable; the path being drawn or edited always stays visible.
    const showPaths=authorStore.state.preferences?.showPaths!==false;
    paths.forEach((p,o)=>{ if(showPaths || o===store.view.pathDrawing || o===store.view.pathEditing) drawPath(p,o); });
    if(store.view.pathDrawing!=null){
      const o=store.view.pathDrawing,prefix=o===32?'l':`s${o+1}`,v=withPathPoses(authorStore.state).parameters.values;
      const pts=store.view.pathDraft?.length?store.view.pathDraft:[['x','y','z'].map(k=>v[prefix+k])];
      drawPath({points:pts},o,true);
    }
  }
  function finishPath(cancel=false){
    const object=store.view.pathDrawing,points=store.view.pathDraft;
    const owner=object!=null?[{kind:object===32?'listener':'source',index:object===32?0:object}]:store.view.selection;
    if(!cancel&&points?.length>=2)store.command({op:'path.set',object,points,enabled:true});
    store.setView({pathDrawing:null,pathDraft:null,pathRedo:null,...(owner?.length?{selection:owner}:{})});schedule();
  }
  function closestCurveHit(p, allowedObject) {
    let best = null, bestDist = 8;
    for (const seg of pathSegHits) {
      if (allowedObject != null && seg.object !== allowedObject) continue;
      for (let k = 1; k < seg.screenPts.length; k++) {
        const a = seg.screenPts[k - 1], b = seg.screenPts[k];
        if (a[2] <= 0 && b[2] <= 0) continue;
        const dx = b[0] - a[0], dy = b[1] - a[1], len2 = dx * dx + dy * dy;
        const t = len2 > 1e-6 ? Math.max(0, Math.min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / len2)) : 0;
        const d = Math.hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy);
        if (d < bestDist) {
          bestDist = d;
          const wa = seg.worldPts[k - 1], wb = seg.worldPts[k];
          const point = wa.map((v, i) => v + (wb[i] - v) * t);
          const insertIndex = Math.min(seg.pointCount, Math.floor((k - 1) / Math.max(1, seg.steps)) + 1);
          best = { object: seg.object, insertIndex, point, baseCount: seg.pointCount };
        }
      }
    }
    return best;
  }
  function dragPathPointInView(p, d, e) {
    const f = sizeFactor(), maxZ = Math.min(30, store.state.document.scene.heightM || 30);
    const clampXY = x => Math.max(-200, Math.min(200, x));
    const clampZ = z => Math.max(0, Math.min(maxZ, z));
    if (camera.view === 'front') {
      const q = pointOnTargetPlane(p, camera, width, height);
      return [clampXY(q[0] / f), d.startWorld[1], clampZ(q[2] / f)];
    }
    if (camera.view === 'side') {
      const q = pointOnTargetPlane(p, camera, width, height);
      return [d.startWorld[0], clampXY(q[1] / f), clampZ(q[2] / f)];
    }
    if (camera.view === '3d' && e?.shiftKey && d.startTarget) {
      const q = pointOnTargetPlane(p, camera, width, height);
      return [d.startWorld[0], d.startWorld[1], clampZ(d.startWorld[2] + (q[2] - d.startTarget[2]) / f)];
    }
    const q = pointOnHeightPlane(p, camera, width, height, d.startZ);
    if (q && Math.hypot(q[0] / f - d.startWorld[0], q[1] / f - d.startWorld[1]) < 200) {
      return [clampXY(q[0] / f), clampXY(q[1] / f), d.startWorld[2]];
    }
    if (d.startTarget) {
      const tp = pointOnTargetPlane(p, camera, width, height);
      return [clampXY(d.startWorld[0] + (tp[0] - d.startTarget[0]) / f), clampXY(d.startWorld[1] + (tp[1] - d.startTarget[1]) / f), d.startWorld[2]];
    }
    return null;
  }
  function removePathPointWithFeedback(object, index) {
    const path = authorStore.state.document.paths?.[object];
    if (!path?.points?.length) return;
    if (path.points.length <= 2) {
      hover.textContent = 'A path needs at least 2 points — use Clear path to remove it';
      return;
    }
    const nextIdx = Math.min(index, path.points.length - 2);
    store.command({ op: 'path.removePoint', object, index });
    const nextPt = path.points[nextIdx === index ? index + 1 : nextIdx] || path.points[0];
    store.setView({ pathPoint: { object, index: nextIdx, point: nextPt } }, false);
  }
  onCanvas('pointerdown', e => {
    const p = screen(e);
    if(store.ready&&e.button===0&&store.view.pathDrawing!=null&&!mouseLocked()){
      if(e.detail>1)return;
      e.preventDefault();canvas.focus();
      const o=store.view.pathDrawing,prefix=o===32?'l':`s${o+1}`,v=withPathPoses(authorStore.state).parameters.values;
      const initial=['x','y','z'].map(k=>v[prefix+k]),pts=store.view.pathDraft||[initial];
      const f=sizeFactor();
      const draftHit=[...draftNodeHits].reverse().find(h=>Math.hypot(p[0]-h.screen[0],p[1]-h.screen[1])<9);
      if(draftHit){
        drag={type:'draftPoint',object:o,index:draftHit.index,start:p,startWorld:draftHit.point.slice(),startZ:draftHit.point[2]*f,startTarget:pointOnTargetPlane(p,camera,width,height),moved:false};
        canvas.setPointerCapture(e.pointerId);schedule();return;
      }
      const q=camera.view==='front'||camera.view==='side'?pointOnTargetPlane(p,camera,width,height):pointOnHeightPlane(p,camera,width,height,initial[2]*f);
      if(q&&pts.length<64){
        const pt=camera.view==='front'?[q[0]/f,initial[1],Math.max(0,Math.min(30,q[2]/f))]:camera.view==='side'?[initial[0],q[1]/f,Math.max(0,Math.min(30,q[2]/f))]:q.map(x=>x/f);
        const last=pts.at(-1);
        if(!last || Math.hypot(pt[0]-last[0],pt[1]-last[1],pt[2]-last[2])>=0.001){
          store.setView({pathDraft:[...pts,pt],pathRedo:null});
        }
      }
      schedule();return;
    }
    if (!store.ready) return; if (drag) finish(true); canvas.focus(); e.preventDefault();
    const ref = pick(p, true);
    const objMarkerHit = ref && ['source', 'listener'].includes(ref.kind) ? ref : null;
    if (!objMarkerHit) {
      const pathHit = mouseLocked() ? null : pathHits.find(h => Math.hypot(p[0] - h.screen[0], p[1] - h.screen[1]) < 9);
      if (pathHit && (e.button === 2 || (e.button === 0 && e.altKey))) {
        removePathPointWithFeedback(pathHit.object, pathHit.index);
        schedule(); return;
      }
      if (pathHit && e.button === 0) {
        const ownerRef = { kind: pathHit.object === 32 ? 'listener' : 'source', index: pathHit.object === 32 ? 0 : pathHit.object };
        store.setView({ pathPoint: pathHit, selection: [ownerRef] }, false);
        const tx = `path-${Date.now()}`, f = sizeFactor();
        store.command({ op: 'transaction.begin', label: 'Move path point' }, tx);
        drag = { type: 'pathPoint', ...pathHit, livePoint: pathHit.point.slice(), start: p, startWorld: pathHit.point.slice(), startZ: pathHit.point[2] * f, startTarget: pointOnTargetPlane(p, camera, width, height), tx, lastSent: 0 };
        canvas.setPointerCapture(e.pointerId); schedule(); return;
      }
      const selObj = store.view.selection.find(r => r.kind === 'source' || r.kind === 'listener');
      const selectedPathObj = selObj ? (selObj.kind === 'listener' ? 32 : selObj.index) : null;
      const curveAllowed = mouseLocked() ? null : store.view.pathEditing ?? (e.shiftKey ? selectedPathObj : null);
      const segHit = e.button === 0 && curveAllowed != null ? closestCurveHit(p, curveAllowed) : null;
      if (segHit) {
        const ownerRef = { kind: segHit.object === 32 ? 'listener' : 'source', index: segHit.object === 32 ? 0 : segHit.object };
        const tx = `path-insert-${Date.now()}`, f = sizeFactor();
        store.command({ op: 'transaction.begin', label: 'Insert path point' }, tx);
        store.command({ op: 'path.addPoint', object: segHit.object, index: segHit.insertIndex, point: segHit.point }, tx);
        const ptInfo = { object: segHit.object, index: segHit.insertIndex, point: segHit.point };
        store.setView({ pathPoint: ptInfo, selection: [ownerRef] }, false);
        drag = { type: 'pathPoint', object: segHit.object, index: segHit.insertIndex, point: segHit.point, livePoint: segHit.point.slice(), start: p, startWorld: segHit.point.slice(), startZ: segHit.point[2] * f, startTarget: pointOnTargetPlane(p, camera, width, height), tx, isNew: true, baseCount: segHit.baseCount, lastSent: 0 };
        canvas.setPointerCapture(e.pointerId); schedule(); return;
      }
      if (e.button === 0 && !ref) {
        const anyLineHit = closestCurveHit(p, null);
        if (anyLineHit) {
          const ownerRef = { kind: anyLineHit.object === 32 ? 'listener' : 'source', index: anyLineHit.object === 32 ? 0 : anyLineHit.object };
          store.setView({ selection: [ownerRef], pathPoint: null });
          schedule(); return;
        }
      }
    }
    if(ref&&['source','listener'].includes(ref.kind)&&pathPositionLocked(authorStore.state,(ref.kind==='listener'?'l':`s${ref.index+1}`)+'x')){store.setView({selection:[ref]});return;}
    if(isDrawing()&&e.button===2){endDraft();return;}
    if(e.button===2&&!preview){
      if(mouseLocked()||!['plan','3d'].includes(camera.view))return;
      const selected=store.view.selection.find(r=>['source','listener'].includes(r.kind))||{kind:'listener',index:0};
      const id=selected.kind==='listener'?'laz':`s${selected.index+1}az`;
      bridge.parameters.begin(id);drag={type:'face',ref:selected,ids:[id],initial:{...store.state.parameters.values},start:p,camera};facePointer(p);
    } else if(preview){drag={type:'orbit',start:p,camera,labels:new Map(labelVisibility)};}
    else if (e.button === 1 || e.shiftKey && e.button === 0 && store.view.tool!=='box' && !(store.view.page==='edit'&&ref&&!isDrawing())) {
      if (store.state.document.editorFlags.navLocked) return;
      drag = { type: 'pan', camera, start: p,labels:new Map(labelVisibility) };
    } else if (store.view.page === 'edit' && store.view.tool === 'room' && e.button === 0) {
      if (camera.view !== 'plan') { hover.textContent = 'Draw surfaces in Plan view'; return; }
      draftCursor=drawingPoint(p,e);if(!draft.length)draft=[draftCursor.world];
      drag = {type:'room',start:p,last:p,camera};schedule();
    } else if (store.view.page === 'edit' && ['wall', 'floorZone', 'ceilingZone','plate'].includes(store.view.tool) && e.button === 0) {
      if (camera.view !== 'plan') { hover.textContent = 'Draw surfaces in Plan view'; return; }
      if(e.detail>1)return;
      draftCursor=drawingPoint(p,e);const world=draftCursor.world;
      if(draft.length>=3&&Math.hypot(...project([...draft[0],0],camera,width,height).slice(0,2).map((v,i)=>v-p[i]))<9){closeShape();return;}
      if(draft.length&&Math.hypot(world[0]-draft.at(-1)[0],world[1]-draft.at(-1)[1])<.001)return;
      if(store.view.tool==='wall'&&draft.length){store.command(wallCommand(draft.at(-1),world));if(!store.view.autoLink){endDraft();return;}}
      draft.push(world);schedule();return;
    } else if (store.view.tool === 'box' && store.view.page === 'edit') drag = { type: 'box', start: p, last: p,base:cleanSelection(store.view.selection),modifiers:{shiftKey:e.shiftKey,ctrlKey:e.ctrlKey,altKey:e.altKey,metaKey:e.metaKey} };
    else if (ref && ['source', 'listener'].includes(ref.kind)) {
      if(mouseLocked()){store.setView({selection:[{kind:ref.kind,index:ref.index}]});return;}
      queueMicrotask(() => store.setView({ selection: [{ kind: ref.kind, index: ref.index }] }, false));
      if (ref.kind === 'source' && ref.channel !== undefined && e.button === 0) {
        const tx = `point-${Date.now()}`, pt = sourcePoints(store.state, ref.index).find(point => point.channel === ref.channel), local = pt.local;
        const hitZ = camera.view === '3d' ? pt.position[2] : null;
        drag = { type: 'point', ref, tx, start: p, camera, local, hitZ, lastSent: 0, inFlight: false }; store.command({ op: 'transaction.begin', label: 'Move channel point' }, tx);
      } else {
      const prefix = ref.kind === 'listener' ? 'l' : `s${ref.index + 1}`, yaw = ref.kind === 'listener' ? 'laz' : `${prefix}az`;
      const ids = e.button === 2 ? [yaw] : camera.view === 'front' ? [`${prefix}x`, `${prefix}z`] : camera.view === 'side' ? [`${prefix}y`, `${prefix}z`] : [`${prefix}x`, `${prefix}y`];
      ids.forEach(id => bridge.parameters.begin(id)); drag = { type: e.button === 2 ? 'rotate' : 'pose', ref, ids, start: p, camera, initial: { ...store.state.parameters.values } };
      }
    } else if (ref && store.view.page === 'edit') {
      const modified=e.shiftKey||modKey(e);
      const selection=modified?toggleSelection(store.view.selection,ref):store.view.selection.some(r=>sameRef(r,ref))?cleanSelection(store.view.selection):[{kind:ref.kind,index:ref.index}];
      store.setView({selection});
      if(modified){schedule();return;}
      if (e.button === 0) {
        const tx = `geometry-${Date.now()}`;
        const scene = store.state.document.scene, pickedObj = surface(scene, ref);
        const hitZ = hitHeightForSurface(p, camera, width, height, pickedObj, ref.kind, scene.heightM);
        drag = { type: ref.vertex !== undefined && selection.length===1 ? 'vertex' : 'surface', ref, refs: selection.filter(r=>!['source','listener'].includes(r.kind)), tx, start: p, camera, lastSent: 0, hitZ, startScene: scene };
        store.command({ op: 'transaction.begin', label: 'Move surface' }, tx);
      }
    } else if (store.view.pathEditing != null && e.button === 0 && !mouseLocked()) {
      drag = { type: 'pathAppendOrOrbit', object: store.view.pathEditing, start: p, camera, labels: new Map(labelVisibility), moved: false };
    } else if (camera.view === '3d' && e.button === 0) { drag = { type: 'orbit', start: p, camera,labels:new Map(labelVisibility) };store.setView({ selection: [] }, false); }
    else {
      // Rays are display-only (owner's decision): they are never picked, so they cannot steal clicks.
      store.setView({ selection: [] }); return;
    }
    if (drag) canvas.setPointerCapture(e.pointerId);
    if (drag && ['pan', 'orbit'].includes(drag.type)) autoFrame = false;
  });
  onCanvas('pointerleave',()=>{cursorLevel=null;hover.textContent='';schedule();});
  onCanvas('pointermove', e => {
    const p = screen(e);
    if(drag?.type==='draftPoint'){
      if(!drag.moved && Math.hypot(p[0]-drag.start[0],p[1]-drag.start[1])>=3)drag.moved=true;
      if(drag.moved){
        const nextPt=dragPathPointInView(p,drag,e);
        const o=drag.object,prefix=o===32?'l':`s${o+1}`,v=withPathPoses(authorStore.state).parameters.values;
        const base=(store.view.pathDraft||[['x','y','z'].map(k=>v[prefix+k])]).map(q=>q.slice());
        if(nextPt && drag.index>=0 && drag.index<base.length){
          base[drag.index]=nextPt;
          store.view.pathDraft=base;
          store.view.pathRedo=null;
        }
      }
      schedule();return;
    }
    if(drag?.type==='pathPoint'){
      const nextPt=dragPathPointInView(p,drag,e);
      if(nextPt){
        drag.livePoint=nextPt;
        drag.final={op:'path.movePoint',object:drag.object,index:drag.index,point:nextPt};
        if(performance.now()-drag.lastSent>=50){store.command(drag.final,drag.tx);drag.lastSent=performance.now();}
      }
      schedule();return;
    }
    if(drag?.type==='pathAppendOrOrbit'){
      if(Math.hypot(p[0]-drag.start[0],p[1]-drag.start[1])>=4){
        drag.moved=true;
        if(camera.view==='3d'){drag.type='orbit';autoFrame=false;camera={...drag.camera,yaw:drag.camera.yaw-(p[0]-drag.start[0])*0.008,pitch:Math.max(-0.2,Math.min(1.5,drag.camera.pitch+(p[1]-drag.start[1])*0.008))};}
      }
      store.view.camera=camera;schedule();return;
    }
    if(isDrawing()&&camera.view==='plan'){draftCursor=drawingPoint(p,e);schedule();if(!drag)return;}
    if (!drag) {
      const ref=pick(p);canvas.title=ref?.kind==='source'?store.state.document.sources[ref.index]?.name:ref?.kind==='listener'?'Listener':'';
      if (store.view.energy && energy && binary && camera.view === 'plan') {
        const world = pointOnTargetPlane(p, camera, width, height).map(x=>x/overlayFactor()), d = binary.descriptor, x = Math.floor((world[0] - d.x0) / d.dx), y = Math.floor((world[1] - d.y0) / d.dy);
        cursorLevel=cursorEnergyLevel(world,d,energy.db);schedule();
        const i = y * d.nx + x; hover.textContent = x < 0 || x >= d.nx || y < 0 || y >= d.ny ? '' : `${bridge.mode === 'mock' ? 'SYNTHETIC · ' : ''}X ${world[0].toFixed(2)} Y ${world[1].toFixed(2)} · ${energy.db[i] === null ? 'Outside usable height' : Number.isFinite(energy.db[i]) ? `${energy.db[i].toFixed(1)} dB rel. peak` : 'No energy detected'}`;
      } return;
    }
    if (drag.type === 'face') facePointer(p);
    else if (drag.type === 'pan') camera = pan(drag.camera, drag.start, p, width, height);
    else if (drag.type === 'orbit') camera = { ...drag.camera, yaw: drag.camera.yaw - (p[0] - drag.start[0]) * 0.008, pitch: Math.max(-0.2, Math.min(1.5, drag.camera.pitch + (p[1] - drag.start[1]) * 0.008)) };
    else if (drag.type === 'box' || drag.type === 'room') drag.last = p;
    else if (drag.ids) {
      const z = drag.initial[drag.ref.kind === 'listener' ? 'lz' : `s${drag.ref.index + 1}z`];
      const unproject = q => drag.camera.view === '3d' && drag.type === 'pose' ? pointOnHeightPlane(q, drag.camera, width, height, z) : pointOnTargetPlane(q, drag.camera, width, height);
      const a = unproject(drag.start), b = unproject(p); if (!a || !b) return;
      const delta = b.map((v, i) => v - a[i]);
      pendingPose = {};
      drag.ids.forEach(id => {
        const d = store.state.parameters.descriptors.parameters.find(v => v.id === id), axis = id.endsWith('x') ? 0 : id.endsWith('y') ? 1 : 2;
        let value = drag.type === 'rotate' ? drag.initial[id] + (p[0] - drag.start[0]) * (e.shiftKey ? 0.1 : 0.6) : drag.initial[id] + delta[axis];
        if (drag.type === 'rotate') value = ((value + 180) % 360 + 360) % 360 - 180;
        pendingPose[id] = canonical(d, value);
      });
      if (performance.now() - lastPoseTime >= 1000 / 60) flushPose();
      else if(poseTimer===null)poseTimer=setTimeout(flushPose,Math.max(0,1000/60-(performance.now()-lastPoseTime)));
    } else if (drag.tx) {
      const [a, b, onHeightPlane] = surfaceDragPlane(drag, p);
      if (drag.type === 'point') {
        const delta = b.map((v, i) => v - a[i]);
        const localDelta = rotateOffset(delta, -store.state.parameters.values[`s${drag.ref.index + 1}az`]);
        drag.final = { op: 'source.setPointOffset', index: drag.ref.index, channel: drag.ref.channel, offsetM: drag.local.map((n, i) => Math.max(-100, Math.min(100, n + localDelta[i]))) };
        if (!drag.inFlight && performance.now() - drag.lastSent >= 50) {
          drag.inFlight = true; drag.lastSent = performance.now();
          const curDrag = drag;
          Promise.resolve(store.command(drag.final, drag.tx)).finally(() => { curDrag.inFlight = false; });
        }
      } else {
        updateGeometryDrag(drag, p, e, a, b, onHeightPlane);
      }
    }
    store.view.camera = camera; schedule();
  });
  onCanvas('pointerup', e => {
    if (drag?.type === 'draftPoint') {
      if (drag.moved && store.view.pathDraft) store.setView({ pathDraft: store.view.pathDraft.map(q => q.slice()) });
      finish(); return;
    }
    if (drag?.type === 'pathAppendOrOrbit' && !drag.moved) {
      const p = screen(e), obj = drag.object, path = authorStore.state.document.paths?.[obj];
      if (path?.points?.length && path.points.length < 64) {
        const lastPt = path.points.at(-1), f = sizeFactor();
        const pt = dragPathPointInView(p, { startWorld: lastPt, startZ: lastPt[2] * f, startTarget: pointOnTargetPlane(p, camera, width, height) }, e);
        if (pt) {
          const newIdx = path.points.length;
          store.command({ op: 'path.addPoint', object: obj, point: pt });
          store.setView({ pathPoint: { object: obj, index: newIdx, point: pt } }, false);
        }
      }
    }
    if (drag?.type === 'room') {
      const a=draft[0],b=drawingPoint(screen(e),e).world;
      if(Math.abs(a[0]-b[0])>.01&&Math.abs(a[1]-b[1])>.01) {
        const corners=rectangle(a,b),tx=`room-${Date.now()}`;
        store.command({op:'transaction.begin',label:'Draw rectangular room'},tx);
        corners.forEach((a,i)=>store.command(wallCommand(a,corners[(i+1)%4]),tx));
        store.command({op:'transaction.commit'},tx);endDraft();
      }
    }
    if (drag?.final) {
      if (drag.type === 'vertex' || drag.type === 'surface') {
        const p = screen(e);
        updateGeometryDrag(drag, p, e, ...surfaceDragPlane(drag, p));
      }
      store.command(drag.final, drag.tx);
    }
    finish();
  });
  onCanvas('pointercancel', () => finish(true)); onCanvas('lostpointercapture', () => finish(true));
  onCanvas('contextmenu', e => e.preventDefault());
  onCanvas('wheel', e => {
    e.preventDefault();
    autoFrame = false;
    const dy = e.deltaMode === 1 ? e.deltaY * 33 : e.deltaMode === 2 ? e.deltaY * height : e.deltaY;
    const clampedDy = Math.max(-240, Math.min(240, dy));
    camera = zoomAt(camera, screen(e), Math.exp(clampedDy * 0.0015), width, height);
    store.view.camera = camera;
    schedule();
    persistCameraDebounced();
  }, { passive: false });
  onCanvas('dblclick', e => {
    if(store.view.pathDrawing!=null){finishPath();return;}
    if(isDrawing()){if(store.view.tool!=='room')closeShape();return;}
    const ref = pick(screen(e)); if (ref) { const points = pointsFor(ref); if (points.length) { autoFrame = false; const b = boundsOf(points); camera = { ...camera, target: b.min.map((v, i) => (v + b.max[i]) / 2) }; } } else fit();
    store.view.camera = camera; schedule(); persistCamera();
  });
  onCanvas('keydown', e => {
    if(store.view.pathDrawing!=null){
      if(['Enter','Escape'].includes(e.key)){e.preventDefault();finishPath(e.key==='Escape');return;}
      if(modKey(e)&&['z','y'].includes(e.key.toLowerCase())){
        e.preventDefault();
        const k=e.key.toLowerCase();
        const pts=store.view.pathDraft||[];
        const redo=store.view.pathRedo||[];
        if(k==='z'&&!e.shiftKey){
          if(pts.length>1){
            const popped=pts.at(-1);
            store.setView({pathDraft:pts.slice(0,-1),pathRedo:[...redo,popped]});
            schedule();
          }
        }else if((k==='y'&&!e.shiftKey)||(k==='z'&&e.shiftKey)){
          if(redo.length){
            const restored=redo.at(-1);
            store.setView({pathDraft:[...pts,restored],pathRedo:redo.slice(0,-1)});
            schedule();
          }
        }
        return;
      }
    }
    if(['Delete','Backspace'].includes(e.key)&&store.view.pathPoint){e.preventDefault();removePathPointWithFeedback(store.view.pathPoint.object,store.view.pathPoint.index);schedule();return;}

    if (e.key.toLowerCase() === 'f') { e.preventDefault(); fit(e.shiftKey); persistCamera(); }
    if (e.key === 'Home') { camera = createCamera(store.view.projection); fit(); persistCamera(); }
    if (e.key === 'Escape') { e.preventDefault();finish(true); endDraft(); store.setView({ selection: [], pathEditing: null, pathPoint: null }); }
    if (e.key === 'Enter' && draft.length)closeShape();
    if (modKey(e) && ['z','y'].includes(e.key.toLowerCase())) {
      e.preventDefault();
      const redo=e.key.toLowerCase()==='y'||e.shiftKey;
      if(!redo&&undoDraft())return;
      endDraft();
      store.command({ op: redo?'history.redo':'history.undo' });
    }

  });
  const nativeReset=()=>{resetPointer(false);endDraft();};window.addEventListener('verb-gestures-reset',nativeReset);
  const blur = () => resetPointer(true), hidden = () => { if (document.hidden) finish(true); };
  window.addEventListener('blur', blur); document.addEventListener('visibilitychange', hidden);
  const observer = new ResizeObserver(() => {
    const dpr = window.devicePixelRatio || 1, w = Math.max(1, root.clientWidth), h = Math.max(1, root.clientHeight);
    if (width === w && height === h && canvas.width === Math.round(w*dpr) && canvas.height === Math.round(h*dpr)) return;
    width = w; height = h;
    updateHint();
    canvas.width = Math.round(width * dpr); canvas.height = Math.round(height * dpr); ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    if (autoFrame || !store.view.camera) fit();
    else { camera.sceneDistance = frame(camera, boundsOf(scenePoints(store.state)), width, height).distance; store.view.camera = camera; }
    schedule();
  }); observer.observe(root);
  let geometry = null, poseKey = '', viewKey = '';
  const off = store.subscribe(({ reason }) => {
    if (reason === 'resync' || reason === 'transactionfailed') finish(true);
    if (!viewportEvent(reason) || !store.state) return;
    const state = store.state, v = state.parameters.values;
    updateHint();
    if(draftTool!==store.view.tool||store.view.page!=='edit'||camera.view!==store.view.projection){endDraft();draftTool=store.view.tool;}
    if(!preview){
      const pt = store.view.pathPoint;
      if(pt){
        const p = authorStore.state.document.paths?.[pt.object];
        if(!p?.points || p.points.length<2) store.view.pathPoint = null;
        else if(pt.index >= p.points.length) store.view.pathPoint = { object: pt.object, index: p.points.length - 1, point: p.points.at(-1) };
      }
      if(store.view.pathEditing!=null && !(authorStore.state.document.paths?.[store.view.pathEditing]?.points?.length>=2)) store.view.pathEditing = null;
    }
    const nextPose = JSON.stringify([state.document.sources.map((s,i)=>[s, ...['x','y','z','az'].map(k=>v[`s${i+1}${k}`])]),v.lx,v.ly,v.lz,v.laz,state.document.output,state.parameters.effective.zM,state.document.paths]);
    const nextView = JSON.stringify([store.view.page,store.view.projection,store.view.selection,store.view.energy,store.view.rays,store.view.rayOrder,store.view.raysOccluded,store.view.analysis,store.view.showMic,store.view.camera,store.view.pathDrawing,store.view.pathDraft,store.view.pathEditing,store.view.pathPoint,state.preferences,state.document.editorFlags]);
    const geometryChanged = geometry !== state.document.scene || poseKey !== nextPose;
    // T11 item 1: preset / room / file loads (including asynchronous JUCE load completion and same-preset reload)
    // always frame the whole scene in the current view; ordinary edits within the same room keep manual zoom.
    const key = state.document.sceneName, loadState = state.load?.state ?? 2, sessionCleared = reason === 'snapshot' && !authorStore.state.preferences?.sessionViewState;
    const newRoom = !sceneKey || (reason === 'snapshot' && (key !== sceneKey || sessionCleared));
    if (newRoom) pendingRoomFit = loadState === 1;
    const loadSettled = pendingRoomFit && loadState === 2;
    if (loadSettled) pendingRoomFit = false;
    const sizeNow = state.parameters.effective.sizePercent ?? 100, sizeChanged = lastSize !== null && sizeNow !== lastSize;
    const pageNow = store.view.page, pageChanged = pageNow !== lastPage;
    if (geometryChanged || camera.view !== store.view.projection || !store.view.camera || newRoom || loadSettled || sizeChanged || pageChanged) {
      if (camera.view !== store.view.projection || !store.view.camera || newRoom || loadSettled) {
        finish(true); camera = { ...camera, view: store.view.projection }; fit();
        if (sessionCleared || loadSettled) persistCamera();
      } else if ((sizeChanged || pageChanged) && !drag && (autoFrame || !screenExtent(allPoints(), camera, width, height).fits)) {
        fit();
      }
      if (sceneKey && key !== sceneKey) endDraft();
      sceneKey = key;
    }
    lastSize = sizeNow; lastPage = pageNow; lastLoadState = loadState;
    if(store.view.pathDrawing!=null)canvas.focus();
    if (geometryChanged || nextView !== viewKey || ['diagnostics.paths','rays.paths','diagnostics.clutter','analysis.result','analysis.cleared','analysis.stale'].includes(reason)) schedule();
    geometry = state.document.scene; poseKey = nextPose; viewKey = nextView;
    if (reason === 'analysis.cleared') energyLoader(null);
    else if (reason === 'analysis.result') loadEnergy();
  });
  updateHint();
  root.cancelGesture=()=>{resetPointer(false);endDraft();};root.undoDraft=undoDraft;
  root.redraw=()=>{if(preview){const yaw=camera.yaw,pitch=camera.pitch;fit();camera={...camera,yaw,pitch};}schedule();};
  root.frameStats = () => ({ ...screenExtent(allPoints(), camera, width, height), width, height, autoFrame, view: camera.view });
  root.frameAll = () => { fit(); persistCamera(); }; root.frameSelection = () => { fit(true); persistCamera(); };
  Object.defineProperty(root, 'liveCamera', { get: () => camera });
  Object.defineProperty(root, 'isInteracting', { get: () => !!drag });
  root.dispose = () => { previewViewport?.dispose();finish(true); disposed = true; off(); observer.disconnect(); if (persistTimer !== null) { clearTimeout(persistTimer); persistTimer = null; } if (raf !== null) cancelAnimationFrame(raf); window.removeEventListener('verb-gestures-reset',nativeReset);window.removeEventListener('blur', blur); document.removeEventListener('visibilitychange', hidden); };
  schedule(); loadEnergy(); return root;
}
