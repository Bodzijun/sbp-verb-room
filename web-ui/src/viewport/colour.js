import { viewportPalette } from './palette.js';
// Material-class strokes: opt-in only (preferences.classColours). The default is the theme's
// role colour per object type, so surfaces of different types never share one tint.
export const CLASS_COLOURS = ['#b98c73', '#d8b98a', '#bf914e', '#c17f6d', '#ad7d62', '#b89271', '#b6a192', '#b9aa8c', '#a69c70', '#c79866', '#d1ad8d'];
// Object role of a surface: wall / floor (floor zone) / ceiling (ceiling zone) / plate (raised zone)
// / opening (ceiling without material = open sky).
export function surfaceRole(ref, obj) {
  if (ref.kind === 'wall') return 'wall';
  if (obj && obj.heightM !== null && obj.heightM !== undefined) return 'plate';
  if (ref.kind === 'floorZone' || ref.kind === 'floor') return 'floor';
  return obj && obj.material === null ? 'opening' : 'ceiling';
}
// Precedence: the colour the user chose for this material ALWAYS wins, in every theme and
// projection; then the opt-in class colours; then the theme's colour for the object role.
// (`wallOverride` from older sessions is a no-op now: walls already use the theme wall colour.)
export function materialColour(preferences = {}, material, roles = viewportPalette(preferences), role = 'wall') {
  const chosen = material ? preferences.materialColours?.[`${material.name}:${material.classId}`] : null;
  return chosen || (preferences.classColours ? CLASS_COLOURS[material?.classId ?? 7] : null) || roles[role] || roles.wall;
}
export function hexToHsl(hex) {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex || ''); if (!m) return [30, 40, 60];
  const n = parseInt(m[1], 16), r = (n >> 16 & 255) / 255, g = (n >> 8 & 255) / 255, b = (n & 255) / 255, max = Math.max(r, g, b), min = Math.min(r, g, b), l = (max + min) / 2, d = max - min;
  let h = 0, sat = 0;
  if (d) { sat = d / (1 - Math.abs(2 * l - 1)); h = max === r ? ((g - b) / d + 6) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4; h *= 60; }
  return [h, sat * 100, l * 100];
}
// Reflection order → colour: direct uses the theme's direct role; reflections walk
// the hue of the theme's reflection role so orders stay distinguishable in any theme.
export function rayColour(order, roles, pointIndex=0) {
  if(pointIndex>0){const [h,s,l]=hexToHsl(order<=0?roles.direct:roles.reflection);return `hsl(${Math.round((h+pointIndex*31)%360)} ${Math.round(s)}% ${Math.round(l)}%)`;}
  if (order <= 0) return roles.direct;
  const [h, s, l] = hexToHsl(roles.reflection), step = order - 1;
  return `hsl(${Math.round((h + step * 28) % 360)} ${Math.round(Math.min(90, s + 8))}% ${Math.round(Math.max(38, Math.min(70, l + 6 - step * 3)))}%)`;
}
