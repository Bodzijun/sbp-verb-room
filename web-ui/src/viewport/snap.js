// T6 B4: snap step follows the visible grid spacing (zoom), or a fixed manual step, or is off.
// Exact numeric entry never goes through here.
export const SNAP_STEPS = [1, 0.5, 0.1, 0.05, 0.01];   // m, coarse → fine
export const MIN_GRID_PX = 18;                          // finest step whose lines stay ≥ 18 px apart
export const pixelsPerMetre = (camera, height) => height / (2 * Math.tan(camera.fov / 2)) / camera.distance;
export function autoStep(camera, height, minPx = MIN_GRID_PX) {
  const ppm = pixelsPerMetre(camera, height); let pick = SNAP_STEPS[0];
  for (const step of SNAP_STEPS) if (step * ppm >= minPx) pick = step;
  return pick;
}
// view.snap: enabled flag (kept from older sessions); view.snapStep: 'auto' | metres.
export function snapSetting(view) { return !view.snap ? 'off' : typeof view.snapStep === 'number' && view.snapStep > 0 ? view.snapStep : 'auto'; }
export function effectiveStep(view, camera, height) { const s = snapSetting(view); return s === 'off' ? 0 : s === 'auto' ? autoStep(camera, height) : s; }
export function snapValue(value, step) { return step > 0 ? Number((Math.round(value / step) * step).toFixed(4)) : value; }
export const stepLabel = step => step >= 1 ? `${+step.toFixed(2)} m` : `${Math.round(step * 100)} cm`;
export const snapFace = (view, step) => { const s = snapSetting(view); return s === 'off' ? 'Snap off' : s === 'auto' ? `Snap auto · ${stepLabel(step)}` : `Snap ${stepLabel(s)}`; };
// Fine grid lines for the orthographic views, only over the visible rectangle, so the count is
// bounded by canvas size / MIN_GRID_PX regardless of zoom. 3D keeps the finite scene grid.
export function visibleGrid(view, [x0, y0], [x1, y1], step) {
  if (view === '3d' || !(step > 0)) return [];
  const axes = view === 'front' ? [0, 2] : view === 'side' ? [1, 2] : [0, 1];
  const lo = [Math.min(x0, x1), Math.min(y0, y1)], hi = [Math.max(x0, x1), Math.max(y0, y1)], lines = [];
  for (let axis = 0; axis < 2; axis++) {
    const first = Math.ceil(lo[axis] / step - 1e-9), last = Math.floor(hi[axis] / step + 1e-9);
    if (last - first > 400) return [];
    for (let i = first; i <= last; i++) {
      const a = [0, 0, 0], b = [0, 0, 0], other = 1 - axis;
      a[axes[axis]] = b[axes[axis]] = i * step; a[axes[other]] = lo[other]; b[axes[other]] = hi[other];
      lines.push({ a, b, major: i % 10 === 0 });
    }
  }
  return lines;
}
