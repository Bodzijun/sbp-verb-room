import { WALL_CORNER_TOLERANCE } from './selectors.js';

const distance = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);

// Keep the traversal and anchor order in sync with scene.joinCorners in SceneCommands.cpp.
function clusters(walls, tolerance, wallMode, refs) {
  if (wallMode) return [];
  const included = refs && new Set(refs.map(ref => ref.index));
  const ends = walls.flatMap((wall, index) => included && !included.has(index) ? [] :
    ['a', 'b'].map(end => ({ index, end, xy: wall[end] })));
  const visited = new Set(), result = [];
  for (const seed of ends) {
    if (visited.has(seed)) continue;
    const cluster = [seed];
    visited.add(seed);
    let nearMiss = false;
    for (const from of cluster) for (const to of ends) {
      if (from.index === to.index) continue;
      const gap = distance(from.xy, to.xy);
      if (gap >= tolerance) continue;
      nearMiss ||= gap > WALL_CORNER_TOLERANCE;
      if (!visited.has(to)) { visited.add(to); cluster.push(to); }
    }
    if (nearMiss) result.push(cluster);
  }
  return result;
}

export function openCornerCount(walls, wallMode) {
  return clusters(walls, .10, wallMode).length;
}

export function joinCorners(walls, tolerance, wallMode, refs) {
  const result = { joined: 0, skipped: 0 };
  for (const cluster of clusters(walls, tolerance, wallMode, refs)) {
    const locked = cluster.filter(member => walls[member.index].editor.locked);
    const anchor = (locked[0] ?? cluster[0]).xy;
    if (locked.some(member => distance(anchor, member.xy) !== 0) ||
        cluster.some(member => distance(anchor, member.xy) > tolerance)) {
      result.skipped++;
      continue;
    }
    const proposed = new Map();
    for (const { index, end } of cluster) {
      if (walls[index].editor.locked) continue;
      if (!proposed.has(index)) proposed.set(index, { ...walls[index] });
      proposed.get(index)[end] = anchor.slice();
    }
    if (cluster.some(({ index }) => {
      const wall = proposed.get(index) ?? walls[index];
      return distance(wall.a, wall.b) < WALL_CORNER_TOLERANCE;
    })) { result.skipped++; continue; }
    for (const [index, wall] of proposed) walls[index] = wall;
    result.joined++;
  }
  return result;
}
