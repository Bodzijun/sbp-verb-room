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

// C1: optional per-source editor colour tag (#rrggbb lowercase, empty = Auto).
export const AUTO_STRIP_COLOURS = ['#d9a05b', '#d6756a', '#d8c06c', '#c98d70', '#8fb3a2', '#8ea6c6'];
export const SOURCE_COLOUR_PALETTE = [
  { hex: '#d65f5f', name: 'Strings · Red' },
  { hex: '#4fb074', name: 'Woodwinds · Green' },
  { hex: '#e0b84c', name: 'Brass · Yellow' },
  { hex: '#4f92d6', name: 'Percussion · Blue' },
  { hex: '#94a0a8', name: 'Choir · Grey' },
  { hex: '#3cbec4', name: 'Cyan' },
  { hex: '#e08448', name: 'Orange' },
  { hex: '#9e75d6', name: 'Purple' },
  { hex: '#d96ba8', name: 'Pink' },
  { hex: '#15d5a5', name: 'Emerald' },
  { hex: '#c8cf4a', name: 'Lime' },
  { hex: '#737dd6', name: 'Indigo' }
];
export function isValidSourceColorHex(value) {
  return typeof value === 'string' && /^#[0-9a-fA-F]{6}$/.test(value);
}
export function normalizeSourceColor(value) {
  if (value === null || value === undefined || value === '') return '';
  return isValidSourceColorHex(value) ? value.toLowerCase() : null;
}
function hexRgb(hex) {
  const m = /^#([0-9a-f]{6})$/i.exec(hex || '');
  if (!m) return null;
  const n = parseInt(m[1], 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}
function relativeLuminance(rgb) {
  const lin = c => { const s = c / 255; return s <= 0.03928 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4); };
  return 0.2126 * lin(rgb[0]) + 0.7152 * lin(rgb[1]) + 0.0722 * lin(rgb[2]);
}
export function contrastTextColour(hex) {
  const rgb = hexRgb(hex);
  if (!rgb) return '#f2f6f8';
  return relativeLuminance(rgb) > 0.36 ? '#111518' : '#f2f6f8';
}
export function sourceResolvedHex(source, index = 0) {
  if (isValidSourceColorHex(source?.color)) return source.color.toLowerCase();
  const i = ((index % AUTO_STRIP_COLOURS.length) + AUTO_STRIP_COLOURS.length) % AUTO_STRIP_COLOURS.length;
  return AUTO_STRIP_COLOURS[i];
}
export function sourceStripColour(source, index = 0) {
  if (isValidSourceColorHex(source?.color)) return source.color.toLowerCase();
  const i = ((index % 6) + 6) % 6;
  return `var(--strip-${i}, ${AUTO_STRIP_COLOURS[i]})`;
}
export function sourceMarkerColour(source, roles = viewportPalette({})) {
  return isValidSourceColorHex(source?.color) ? source.color.toLowerCase() : roles.source;
}
export function sourceLabelColour(source, roles = viewportPalette({})) {
  if (!isValidSourceColorHex(source?.color)) return roles.labels;
  const col = source.color.toLowerCase();
  const fg = hexRgb(col), bg = hexRgb(roles.background || '#100e0b');
  if (!fg || !bg) return col;
  const l1 = relativeLuminance(fg), l2 = relativeLuminance(bg);
  const ratio = (Math.max(l1, l2) + 0.05) / (Math.min(l1, l2) + 0.05);
  return ratio >= 2.2 ? col : roles.labels;
}
export function sourceTintRgba(hex, alpha = 0.28) {
  const rgb = hexRgb(hex);
  return rgb ? `rgba(${rgb[0]}, ${rgb[1]}, ${rgb[2]}, ${alpha})` : '';
}

