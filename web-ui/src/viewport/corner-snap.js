import { layerKey, surface, WALL_CORNER_TOLERANCE } from '../state/selectors.js';
import { snapValue } from './snap.js';

const wallEnds = ['a', 'b'];
const sameSurface = (a, b) => a.kind === b.kind && a.index === b.index;
export const sameCorner = (a, b) => sameSurface(a, b) && a.vertex === b.vertex;
export const cornerXY = (scene, ref) => {
  const obj = surface(scene, ref);
  return ref.kind === 'wall' ? obj?.[wallEnds[ref.vertex]] : obj?.vertices?.[ref.vertex];
};

export function wallCorners(wall, index, heightM) {
  const top = wall.zTopM == null || wall.zTopM >= 1e5 ? heightM : wall.zTopM;
  return wallEnds.map((end, vertex) => ({
    ref: { kind: 'wall', index, vertex }, xy: wall[end], heights: [wall.zBottomM, top]
  }));
}

export function visibleCorners(scene, modes) {
  const walls = modes.walls === 2 ? [] : scene.walls.flatMap((w, i) =>
    w.editor.hidden ? [] : wallCorners(w, i, scene.heightM));
  const zones = [['floorZone', scene.floorZones], ['ceilingZone', scene.ceilingZones]].flatMap(([kind, list]) =>
    list.flatMap((z, index) => z.editor.hidden || modes[layerKey(scene, { kind, index })] === 2 ? [] :
      z.vertices.map((xy, vertex) => ({ ref: { kind, index, vertex }, xy, heights: [z.heightM ?? (kind === 'floorZone' ? 0 : scene.heightM)] }))));
  return [...walls, ...zones];
}

export function movedVertexCorners(scene, ref, autoLink, wallMode) {
  if (ref.kind !== 'wall' || !autoLink || wallMode) return [ref];
  const xy = cornerXY(scene, ref);
  return [ref, ...scene.walls.flatMap((wall, index) => wall.editor.hidden || wall.editor.locked ? [] :
    wallCorners(wall, index, scene.heightM).filter(c => !sameCorner(c.ref, ref) &&
      Math.hypot(c.xy[0] - xy[0], c.xy[1] - xy[1]) < WALL_CORNER_TOLERANCE).map(c => c.ref))];
}

export function closestCorner(sources, candidates, project) {
  let best = null, distanceSquared = 9 ** 2;
  const targets = candidates.flatMap(c => c.heights.map(z => ({ corner: c, z, screen: project([...c.xy, z]) })));
  for (const source of sources) for (const target of targets) {
    const dx = source.screen[0] - target.screen[0], dy = source.screen[1] - target.screen[1];
    const gapSquared = dx * dx + dy * dy;
    if (gapSquared < distanceSquared) {
      distanceSquared = gapSquared;
      best = { source: source.corner, target: target.corner, z: target.z };
    }
  }
  return best;
}

export function snapVertex(raw, screen, candidates, project, step, free) {
  const hit = free ? null : closestCorner([{ screen }], candidates, project);
  if (hit) return { world: hit.target.xy.slice(), screen, snap: 'corner', target: hit.target.ref, z: hit.z };
  return { world: step > 0 && !free ? raw.map(v => snapValue(v, step)) : raw, screen, snap: step > 0 && !free ? 'grid' : 'free' };
}

export function snapTranslation(delta, movedCorners, candidates, project, free) {
  if (free) return null;
  const sources = movedCorners.flatMap(c => c.heights.map(z => ({
    corner: c, screen: project([c.xy[0] + delta[0], c.xy[1] + delta[1], z + (delta[2] || 0)])
  })));
  const hit = closestCorner(sources, candidates, project);
  if (!hit) return null;
  const { source, target, z } = hit;
  return {
    delta: [target.xy[0] - source.xy[0], target.xy[1] - source.xy[1], ...delta.slice(2)],
    from: source.ref, target: target.ref, world: target.xy, z
  };
}
