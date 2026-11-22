export function roomOptions(catalog) {
  return [...(catalog?.builtinRooms || []).map(r => [`builtin:${r.id}`, r.name]),
    ...(catalog?.userPresets || []).map(r => [`user:${r.id}`, `User · ${r.name}`])];
}
export function roomCommand(key) {
  const split = key.indexOf(':'); if (split < 0) return null;
  const source = key.slice(0, split), id = key.slice(split + 1);
  return { op:'preset.load', source, id:source === 'builtin' ? Number(id) : id };
}
