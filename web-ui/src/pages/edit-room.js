import { element, button, check, select, note, unavailable } from '../components/dom.js';
import { SNAP_STEPS, snapSetting, snapFace, stepLabel } from '../viewport/snap.js';
import { historyButtons } from '../components/history.js';
import { icon, decorateIcon } from '../components/icons.js';
export function editTools(ctx) {
  const { store } = ctx;
  const refs = () => store.view.selection.filter(r => !['source', 'listener'].includes(r.kind)).map(({ kind, index }) => ({ kind, index }));
  return element('nav', { className: 'tool-rail panel', 'aria-label': 'Room editing tools' }, [
    ...[['move', 'Move','V'], ['box', 'Box select','B'], ['room', 'Draw rectangular room · drag opposite corners','R'], ['wall', 'Add wall · click two endpoints','W'], ['floorZone', 'Add floor zone · click vertices, Enter to finish','Z'], ['ceilingZone', 'Add ceiling zone · click vertices, Enter to finish','C'],['plate','Add raised plate · click vertices, Enter to finish','P']].map(([tool, title, key]) => decorateIcon(button('', () => store.setView({ tool }), { 'data-tool':tool, 'aria-label': title, 'aria-keyshortcuts':key, 'aria-pressed': String(store.view.tool === tool), className: store.view.tool === tool ? 'active' : '' }),tool,`${title} · ${key}`)),
    decorateIcon(button('', () => { if (refs().length) store.command({ op: 'surface.delete', refs: refs() }); }, { 'aria-label': 'Delete selected surfaces',className:'tool-group-start' }),'delete','Delete selected surfaces · Del'),
    ...historyButtons(store).map((b,i)=>decorateIcon(b,i?'redo':'undo',i?'Redo · Ctrl+Shift+Z':'Undo · Ctrl+Z')),
    decorateIcon(button('', () => { if (window.confirm('Clear walls, zones, clutter and roughness? Sources, listener and air remain.')) store.command({ op: 'room.clear' }); }, { 'aria-label': 'Clear room' }),'clear','Clear room · confirmation')]);
}
// T6 B4: auto (follows the visible grid) / fixed step / off. The face shows the live step.
export function snapControl(ctx) {
  const { store } = ctx, setting = snapSetting(store.view);
  const items = [['auto', 'Auto · follows grid'], ...SNAP_STEPS.map(step => [String(step), stepLabel(step)]), ['off', 'Off']];
  const control = select('Snap step', items, setting === 'off' || setting === 'auto' ? setting : String(setting), key => {
    if (key === 'off') store.setView({ snap: false }); else if (key === 'auto') store.setView({ snap: true, snapStep: 'auto' }); else store.setView({ snap: true, snapStep: Number(key) });
  }, { className: 'dd snap-dd', title: 'Snap step for drawing walls, zones and rooms. Alt = no snap. Moving existing corners stays free; typed values are exact.' });
  control.querySelector('.dd-text').textContent = snapFace(store.view, ctx.snapStep());
  return control;
}
// T6 B3: Outliner first, then the four layer filters; Snap and Auto-link on the right.
export function layerBar(ctx) {
  const { store } = ctx, modes = store.state.document.editorFlags.layerModes || {};
  const link=check('Auto-link corners', store.view.autoLink, autoLink => store.setView({ autoLink }),{'aria-label':'Auto-link corners',title:'Continue walls from the last corner; double-click closes the chain. Escape / right click ends it.'});
  link.lastElementChild.className='link-label';link.append(element('span',{className:'link-short',text:'Link','aria-hidden':'true'}));
  return element('div', { className: 'layer-bar' }, [
    button('Outliner', () => store.flags({ outlinerVisible: !store.state.document.editorFlags.outlinerVisible }), { className: 'outliner-toggle', 'aria-pressed': String(store.state.document.editorFlags.outlinerVisible) }),
    element('div', { className: 'layer-filters', role: 'group', 'aria-label': 'Layer filters' }, ['walls', 'floors', 'ceilings', 'plates'].map(key => {
      const mode=modes[key]||0,b=button('', () => store.flags({ layerModes: { walls: 0, floors: 0, ceilings: 0, plates: 0, ...modes, [key]: (mode + 1) % 3 } }), { 'aria-label':`${key}: ${['shown','locked','hidden'][mode]}`, title:`${key}: ${['shown','locked','hidden'][mode]}. Click: shown → locked → hidden. Acoustics unchanged.` });
      b.append(icon(['eye','lock','eyeOff'][mode]),element('span',{text:key[0].toUpperCase()+key.slice(1)}));return b;
    })),
    element('div', { className: 'layer-options', role: 'group', 'aria-label': 'Editing options' }, [snapControl(ctx),link])]);
}
