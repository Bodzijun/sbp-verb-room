import { element, button, note, scrollArea, accordion } from './dom.js';
import { sourceParts, listenerParts, surfaceParts, scenePartsRoom } from './inspector.js';
import { sourceStripColour } from '../viewport/colour.js';
import { cornerRepair } from '../pages/edit-room.js';
import { zonesPanel } from './zones-panel.js';
// Edit-page inspector: two tabs (Selected object / Scene) with accordion sections.
// Open/closed state and the active tab live in the session view state; only the
// first section of a tab is open until the user changes it.
export function editInspector(ctx) {
  const { store } = ctx, view = store.view, tab = view.inspectorTab === 'scene' ? 'scene' : 'selected';
  const selection = view.selection, isObject = r => ['source', 'listener'].includes(r.kind);
  // A box/Shift selection can mix surfaces with sources or the listener: surface editing then applies to the surfaces only.
  const surfaces = selection.filter(r => !isObject(r)), others = selection.length - surfaces.length, ref = surfaces[0] || selection[0];
  const root = element('aside', { className: 'context panel edit-inspector' });
  const tabButton = (key, label) => button(label, () => store.setView({ inspectorTab: key }), { role: 'tab', id: `itab-${key}`, 'aria-selected': String(tab === key), className: tab === key ? 'active' : '' });
  root.append(element('div', { className: 'tabs inspector-tabs', role: 'tablist', 'aria-label': 'Inspector' }, [tabButton('selected', 'Selected'), tabButton('scene', 'Scene')]));
  const body = [];
  if (tab === 'scene') {
    const p = scenePartsRoom(ctx);
    body.push(...accordion(store, 'scene', [
      { key: 'room', title: 'Room', nodes: [...p.room, ...cornerRepair(ctx)] },
      { key: 'air', title: 'Air', hint: 'temperature · humidity', nodes: p.air },
      (() => { const z = zonesPanel(ctx); return { key: 'zones', title: 'Zones', hint: z.hint, nodes: z.nodes }; })(),
      { key: 'clutter', title: 'Clutter', nodes: p.clutter },
      { key: 'rough', title: 'Roughness', nodes: p.roughness },
      { key: 'autoRange', title: 'Automation range', hint: p.autoRangeHint, nodes: p.automationRange },
      { key: 'scale', title: 'Scale room', hint: 'one-time command', nodes: p.scale }
    ]));
  } else if (ref?.kind === 'source') {
    const p = sourceParts(ctx, ref.index);
    if (p) body.push(element('div', { className: 'obj-title' }, [element('i', { className: 'source-color-dot', style: `background:${sourceStripColour(p.source, ref.index)}`, 'aria-hidden': 'true' }), `Source ${String(ref.index + 1).padStart(2, '0')} · ${p.source.name}`]), ...accordion(store, 'source', [{ key: 'pos', title: 'Position & level', nodes: p.position }, { key: 'route', title: 'Routing & channels', nodes: [...p.identity, ...p.routing, ...p.level] }, { key: 'points', title: 'Channel points', nodes: p.points }]));
  } else if (ref?.kind === 'listener') {
    const p = listenerParts(ctx);
    body.push(element('div', { className: 'obj-title', text: 'Listener' }), ...accordion(store, 'listener', [{ key: 'pose', title: 'Pose', nodes: p.position }, { key: 'out', title: 'Output / microphones', nodes: [...p.output.main, ...p.output.extra] }]));
  } else if (ref) {
    const p = surfaceParts(ctx, surfaces);
    if (p) body.push(element('div', { className: 'obj-title', text: (surfaces.length > 1 ? `${surfaces.length} surfaces` : [`${p.ref.kind} ${p.ref.index + 1}`, p.obj.editor.name].filter(Boolean).join(' · ')) + (others ? ` + ${others} other object${others > 1 ? 's' : ''}` : '') }),
      ...(p.dimensions?.length ? [element('div', { className: 'surface-dimensions stack' }, p.dimensions)] : []),
      ...accordion(store, 'surface', [
      { key: 'material', title: 'Material', hint: p.mat ? p.mat.name.replace(/_/g, ' ') : 'opening', nodes: p.material }, { key: 'surface', title: 'Surface', nodes: p.surface },
      { key: 'scatter', title: 'Scatter & transmission', nodes: p.scatter }, { key: 'heights', title: 'Heights & openings', nodes: p.heights }]));
  } else body.push(element('div', { className: 'empty-state' }, [element('b', { text: 'Nothing selected' }), note('Click an object in the viewport or outliner. Air, clutter and room settings are on the Scene tab.'), button('Open Scene settings', () => store.setView({ inspectorTab: 'scene' }))]));
  const head = ref && tab === 'selected' ? button('× Clear', () => store.setView({ selection: [] }), { className: 'inspector-clear', 'aria-label': 'Clear selection' }) : null;
  root.append(scrollArea(body, { className: 'context-body' }));
  if (head) root.querySelector('.inspector-tabs').append(head);
  return root;
}
