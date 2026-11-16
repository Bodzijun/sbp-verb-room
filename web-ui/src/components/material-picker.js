import { element, select, scrollArea } from './dom.js';
import { BANDS } from '../state/selectors.js';
// Material classes are the plugin's closed class list (read-only taxonomy).
export const MATERIAL_CLASSES = ['Masonry', 'Glass', 'Wood', 'Partitions', 'Soft fabric / carpet', 'Absorber / treatment', 'Metal', 'Opening / screen', 'Outdoor / ground', 'Furniture / objects', 'Custom'];
// Catalogue source. Until the plugin sends the full library (see POLISH_REQUESTS.md,
// request 1) only materials present in the scene are known; the picker is built
// for hundreds of entries and merges a catalogue as soon as it appears.
export function materialCatalogue(store) {
  const scene = store.state.document.scene, map = new Map();
  const add = (m, source) => { const key = m.name; if (!map.has(key)) map.set(key, { name: m.name, classId: m.classId, alpha: m.alpha, tags: m.tags || [], source }); };
  const catalogue = store.telemetry['materials.catalogue']?.materials || store.state.materialCatalogue?.materials || [];
  catalogue.forEach(m => add(m, 'library')); scene.materials.forEach(m => add(m, 'scene'));
  return [...map.values()];
}
const mini = alpha => element('span', { className: 'abs-mini', 'aria-hidden': 'true' }, BANDS.map((_, i) => element('i', { style: `height:${Math.max(8, Math.round((alpha?.[i] ?? 0) * 100))}%` })));
// `onPick(name)` applies the preset to the current selection. Search + class filter
// + grouped slim-scroll list; nothing here depends on the number of materials.
export function materialPicker(ctx, current, onPick, state = ctx.store.view.materialPicker || {}) {
  const { store } = ctx, all = materialCatalogue(store);
  const query = element('input', { type: 'text', className: 'material-query', placeholder: `Search ${all.length} material${all.length === 1 ? '' : 's'}…`, 'aria-label': 'Search materials', value: state.q || '', 'data-focus-key': 'material-query' });
  const classes = [['-1', 'All classes'], ...MATERIAL_CLASSES.map((n, i) => [String(i), n]).filter(([i]) => all.some(m => String(m.classId) === i))];
  const filter = select('Material class filter', classes, String(state.cls ?? -1), v => { state.cls = Number(v); store.view.materialPicker = state; fill(); });
  const list = scrollArea([], { rootClass: 'material-list' }), count = element('small', { className: 'material-count' });
  const fill = () => {
    const q = query.value.trim().toLowerCase(), cls = Number(state.cls ?? -1);
    const hits = all.filter(m => (cls < 0 || m.classId === cls) && (!q || `${m.name} ${MATERIAL_CLASSES[m.classId] || ''} ${m.tags.join(' ')}`.toLowerCase().includes(q)));
    const groups = new Map(); hits.forEach(m => { const g = MATERIAL_CLASSES[m.classId] ?? 'Other'; if (!groups.has(g)) groups.set(g, []); groups.get(g).push(m); });
    const nodes = []; groups.forEach((items, g) => {
      nodes.push(element('div', { className: 'material-group', text: `${g} · ${items.length}` }));
      items.sort((a, b) => a.name.localeCompare(b.name)).forEach(m => nodes.push(element('button', { type: 'button', className: `material-item${m.name === current ? ' sel' : ''}`, role: 'option', 'aria-selected': String(m.name === current), title: `${m.name.replace(/_/g, ' ')} · α ${m.alpha.map(v => v.toFixed(2)).join(' / ')}`, onclick: () => onPick(m.name) }, [element('span', { className: 'material-name', text: m.name.replace(/_/g, ' ') }), mini(m.alpha)])));
    });
    list.view.replaceChildren(...(nodes.length ? nodes : [element('p', { className: 'note', text: 'No material matches' })]));
    list.view.setAttribute('role', 'listbox'); count.textContent = `${hits.length} / ${all.length}`; list.update();
    for (const child of list.view.children) list.view.dataset.n = String(hits.length);
  };
  query.addEventListener('input', () => { state.q = query.value; store.view.materialPicker = state; fill(); });
  fill();
  const root = element('div', { className: 'material-picker' }, [element('div', { className: 'material-tools' }, [query, filter, count]), list]);
  root.dispose = () => list.dispose();
  return root;
}
