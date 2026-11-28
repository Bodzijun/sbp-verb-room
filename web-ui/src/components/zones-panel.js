import { element, button, check, note } from './dom.js';
import { numericField } from './slider.js';
import { ZONE_MAX, ZONE_MAX_EDGE, ZONE_MAX_HYSTERESIS, zoneList, zoneColour, zoneLabel, zoneOutline, polygonArea, liveOccupancy, computeZoneOccupancy, ZONE_OBJECT_LISTENER } from '../state/trigger-zones.js';
import { withPathPoses } from '../viewport/movement-path.js';
// V04c Edit → Scene → Zones: list, create (box / polygon tools in the viewport), per-zone fields, live occupancy.
// Every field is one command (one undo step); geometry is edited in the viewport (one transaction per gesture).
const objectName = o => o === ZONE_OBJECT_LISTENER ? 'L' : `S${o + 1}`;
export function zoneOccupancy(store) {
  const zl = zoneList(store.state);
  const wire = liveOccupancy(store.state, store.telemetry);
  if (wire) return { map: wire, live: true };
  const ns = store.state.document.sources.length;
  return { map: new Map(computeZoneOccupancy(zl, withPathPoses(store.state).parameters.values, ns).map(o => [o.id, o.objects])), live: false };
}
function describeShape(z) {
  const o = zoneOutline(z), xs = o.map(p => p[0]), ys = o.map(p => p[1]);
  const size = `${(Math.max(...xs) - Math.min(...xs)).toFixed(2)} × ${(Math.max(...ys) - Math.min(...ys)).toFixed(2)} m`;
  return z.kind === 'box' ? `Box · ${size}` : `Polygon · ${z.points.length} vertices · ${Math.abs(polygonArea(o)).toFixed(2)} m²`;
}
export function zonesPanel(ctx) {
  const { store } = ctx, zl = zoneList(store.state), selected = store.view.zoneEdit ?? null, full = zl.length >= ZONE_MAX;
  const set = (id, patch) => store.command({ op: 'zone.set', id, ...patch });
  const tool = t => () => { store.setView({ page: 'edit', tool: t, zoneEdit: null, projection: 'plan' }); };
  const { map, live } = zoneOccupancy(store);
  const items = zl.map((z, i) => {
    const colour = zoneColour(z, i), inside = map.get(z.id) || [], lit = z.enabled !== false && inside.length > 0, open = selected === z.id;
    const swatch = element('input', { type: 'color', className: 'zone-swatch', value: colour, 'aria-label': `Zone ${i + 1} colour`, title: 'Zone colour (display only)' });
    swatch.addEventListener('change', () => set(z.id, { colour: swatch.value }));
    const name = element('input', { type: 'text', className: 'zone-name', value: z.name, maxLength: 64, 'aria-label': `Zone ${i + 1} name`, 'data-focus-key': `zone-name-${z.id}` });
    const commitName = () => { const v = name.value.trim().slice(0, 64); if (v && v !== z.name) set(z.id, { name: v }); else name.value = z.name; };
    name.addEventListener('change', commitName);
    name.addEventListener('keydown', e => { if (e.key === 'Enter') name.blur(); if (e.key === 'Escape') { name.value = z.name; name.blur(); } e.stopPropagation(); });
    const occ = element('span', { className: `zone-occ${lit ? ' lit' : ''}`, 'data-zone-occ': String(z.id), 'aria-label': `Zone ${i + 1} occupancy`,
      text: z.enabled === false ? 'off' : lit ? `● ${inside.map(objectName).join(' ')}` : '○', title: live ? 'Live (processor)' : 'Preview from current positions (live when audio runs)' });
    const head = element('div', { className: 'zone-head' }, [
      element('span', { className: 'zone-num', text: String(i + 1), title: 'Zone index (list order = priority)' }), swatch, name, occ,
      button(open ? 'Done' : 'Edit', () => store.setView({ zoneEdit: open ? null : z.id, ...(open ? {} : { projection: 'plan', tool: 'move', selection: [] }) }), { className: 'zone-edit', 'aria-pressed': String(open), 'aria-label': `${open ? 'Finish editing' : 'Edit'} zone ${i + 1}` })
    ]);
    const card = element('div', { className: `zone-item${open ? ' open' : ''}${lit ? ' lit' : ''}${z.enabled === false ? ' disabled' : ''}`, 'data-zone-id': String(z.id), style: `--zone-colour:${colour}` }, [head]);
    if (open) {
      const field = (label, key, min, max, step) => element('label', { className: 'zone-field' }, [element('span', { text: label }), numericField(`Zone ${i + 1} ${label}`, z[key], min, max, step, v => set(z.id, { [key]: v }))]);
      const ceil = store.state.document.scene.heightM;
      card.append(
        element('p', { className: 'zone-shape note', text: describeShape(z) }),
        element('div', { className: 'zone-fields' }, [
          field('Bottom, m', 'zMin', 0, ceil, 0.05), field('Top, m', 'zMax', 0, ceil, 0.05),
          field('Soft edge, m', 'edge', 0, ZONE_MAX_EDGE, 0.05), field('Hysteresis, m', 'hysteresis', 0, ZONE_MAX_HYSTERESIS, 0.01)]),
        element('div', { className: 'zone-row' }, [
          check('Enabled', z.enabled !== false, enabled => set(z.id, { enabled }), { 'aria-label': `Zone ${i + 1} enabled`, title: 'A disabled zone reads as outside for every route' }),
          check('Visible', z.visible !== false, visible => set(z.id, { visible }), { 'aria-label': `Zone ${i + 1} visible`, title: 'Display only; hidden zones still trigger' })]),
        element('div', { className: 'zone-row zone-actions' }, [
          button('↑', () => store.command({ op: 'zone.move', id: z.id, index: i - 1 }), { disabled: i === 0, 'aria-label': `Move zone ${i + 1} up`, title: 'Earlier = higher Zone index priority' }),
          button('↓', () => store.command({ op: 'zone.move', id: z.id, index: i + 1 }), { disabled: i === zl.length - 1, 'aria-label': `Move zone ${i + 1} down` }),
          button('Delete', () => { store.setView({ zoneEdit: null }); store.command({ op: 'zone.delete', id: z.id }); }, { className: 'danger', 'aria-label': `Delete zone ${i + 1}`, title: 'Routes that use it keep its id and read outside' })]),
        note('Drag a vertex, an edge or the inside in Plan. Esc cancels the drag; each drag is one undo step.'));
    }
    return card;
  });
  const nodes = [
    element('div', { className: 'zone-tools' }, [
      button('+ Box', tool('zoneBox'), { disabled: full, 'aria-label': 'Draw box zone', title: 'Drag opposite corners in Plan (T)' }),
      button('+ Polygon', tool('zonePoly'), { disabled: full, 'aria-label': 'Draw polygon zone', title: 'Click vertices in Plan; double-click or Enter finishes, Esc cancels (G)' }),
      element('span', { className: 'zone-count', text: `${zl.length} / ${ZONE_MAX}` })]),
    ...(zl.length ? [element('div', { className: 'zone-list', role: 'list', 'aria-label': 'Trigger zones' }, items)] :
      [note('No zones. A zone is an area of the room that drives modulation routes (In zone, Zone index, Distance to zone) and Mod Out parameters: Creative mode only.')]),
    ...(zl.length ? [note('Routes use them via Modulation → Driver: In zone / Zone index / Distance to zone (Creative only).')] : [])
  ];
  const root = element('div', { className: 'zones-panel stack', 'data-zone-count': String(zl.length) }, nodes);
  // Live occupancy without re-rendering the inspector: update the badges on status.load.
  const off = store.subscribe(({ reason }) => {
    if (!root.isConnected) { off(); return; }
    if (reason !== 'status.load' && reason !== 'parameters') return;
    const { map: m, live: isLive } = zoneOccupancy(store);
    root.querySelectorAll('[data-zone-occ]').forEach(el => {
      const id = Number(el.dataset.zoneOcc), z = zoneList(store.state).find(q => q.id === id); if (!z) return;
      const inside = m.get(id) || [], lit = z.enabled !== false && inside.length > 0;
      const text = z.enabled === false ? 'off' : lit ? `● ${inside.map(objectName).join(' ')}` : '○';
      if (el.textContent !== text) el.textContent = text;
      el.classList.toggle('lit', lit); el.closest('.zone-item')?.classList.toggle('lit', lit);
      el.title = isLive ? 'Live (processor)' : 'Preview from current positions (live when audio runs)';
    });
  });
  return { nodes: [root], hint: zl.length ? `${zl.length} zone${zl.length > 1 ? 's' : ''}` : 'triggers · Creative' };
}
