// T6 B5: every theme is a small multi-hue palette — one hue per object role, harmonised with
// the theme's tinted background. Roles: walls, floors (floor zones), ceilings (ceiling zones),
// plates (raised zones), openings (open ceiling), clutter (engine's seeded half-obstacles), sources, listener, selection, rays
// (direct + reflection hue that walks with the order), grid, labels. User-chosen material
// colours still win over these (see colour.js).
export const PALETTES = {
  Amber: { background: '#100e0b', grid: '#5b4a33', wall: '#dccfb4', floor: '#4fb39c', ceiling: '#8e9ae6', plate: '#e06fa8', opening: '#c6dd62', source: '#ff8a2e', listener: '#fff3d6', selection: '#ffe08a', reflection: '#4da3ff', direct: '#f4efe2', clutter: '#b375d1', labels: '#d2c9b6', pathLine: '#ffbe6b', pathNode: '#ffe4a8', pathArrow: '#ffd080', pathMarker: '#5ee8c5', pathDimmed: '#7d6b52' },
  'Ember red': { background: '#170d0c', grid: '#5e332d', wall: '#e3cdc4', floor: '#dcb84e', ceiling: '#5fb4a8', plate: '#b07be0', opening: '#8ccf6a', source: '#ff6648', listener: '#ffe9de', selection: '#ffd08a', reflection: '#3fa7d6', direct: '#f6e6df', clutter: '#d175b3', labels: '#e2bfb3', pathLine: '#ff9678', pathNode: '#ffd9cc', pathArrow: '#ffb499', pathMarker: '#5ce0c8', pathDimmed: '#7f554d' },
  'Ice blue': { background: '#0a121c', grid: '#31526b', wall: '#d2e0ea', floor: '#62c79a', ceiling: '#b793e8', plate: '#e8b84a', opening: '#ec7fb8', source: '#4cc2ff', listener: '#ffffff', selection: '#fff1a0', reflection: '#ff8566', direct: '#e3f0f8', clutter: '#91d175', labels: '#b7d5e8', pathLine: '#7cd4ff', pathNode: '#d8f3ff', pathArrow: '#a8e3ff', pathMarker: '#ffd166', pathDimmed: '#456a82' },
  Violet: { background: '#120e19', grid: '#4d3660', wall: '#ddd0ea', floor: '#67c4a0', ceiling: '#f0b25c', plate: '#f06f8f', opening: '#b5d95f', source: '#d28cff', listener: '#fbf0ff', selection: '#fff3a6', reflection: '#5ab0ff', direct: '#efe6fa', clutter: '#78d175', labels: '#d6c8e6', pathLine: '#e0a6ff', pathNode: '#f4dcff', pathArrow: '#ecc4ff', pathMarker: '#6ee7b7', pathDimmed: '#6b537d' },
  // Key kept for saved preferences; shown as "Graphite": neutral chrome and background, still multi-hue objects.
  Monochrome: { background: '#101214', grid: '#42474a', wall: '#c9d0d4', floor: '#5fb7a8', ceiling: '#e0a860', plate: '#d878a8', opening: '#8fcf6d', source: '#6fa0ff', listener: '#ffffff', selection: '#fff0a0', reflection: '#e85a6a', direct: '#e8eef0', clutter: '#a975d1', labels: '#b7c2c8', pathLine: '#9ec0ff', pathNode: '#e6effa', pathArrow: '#bfd7ff', pathMarker: '#62deb8', pathDimmed: '#566270' },
  'Classic emerald': { background: '#091511', grid: '#245a49', wall: '#d0e4da', floor: '#e0b85a', ceiling: '#7fa6f0', plate: '#e8607a', opening: '#d890d8', source: '#4fe0a0', listener: '#ffffff', selection: '#fff3a0', reflection: '#a98cff', direct: '#d6efe5', clutter: '#91d175', labels: '#aed3c5', pathLine: '#6ff0ba', pathNode: '#d6ffed', pathArrow: '#9ef7d2', pathMarker: '#ffd166', pathDimmed: '#3a6e59' }
};
export const THEME_LABELS = { Monochrome: 'Graphite' };
export const OBJECT_ROLES = ['wall', 'floor', 'ceiling', 'plate', 'opening', 'source', 'listener', 'clutter'];
export function viewportPalette(preferences = {}) { return { ...(PALETTES[preferences.viewportTheme] || PALETTES.Amber), ...preferences.roles }; }
