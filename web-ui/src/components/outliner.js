import { modKey } from '../state/platform.js';
import { element, button, note, scrollArea } from './dom.js';
import { surfaceRefs, surface } from '../state/selectors.js';
import { toggleSelection, rangeSelection, sameRef } from '../state/selection.js';
import { decorateIcon } from './icons.js';
export function outliner(ctx) {
  const { store } = ctx, s = store.state.document.scene;
  const body = [];
  const groups = new Map();
  const order = surfaceRefs(s);
  for (const ref of surfaceRefs(s)) {
    const obj = surface(s, ref), group = obj.editor.group || ref.kind;
    if (!groups.has(group)) groups.set(group, []); groups.get(group).push(ref);
  }
  groups.forEach((refs, title) => {
    const details = element('details', { open: true }, element('summary', { text: `${title} · ${refs.length}` }));
    if(refs.some(r=>surface(s,r).editor.group)) {
      const flags=element('div',{className:'outliner-group-controls','aria-label':`${title} group`});
      for(const [key,label] of [['hidden','Visibility'],['locked','Lock']]) {
        const count=refs.filter(r=>surface(s,r).editor[key]).length,all=count===refs.length,mixed=count>0&&!all;
        flags.append(decorateIcon(button('',()=>store.command({op:'surface.setMetadata',refs,[key]:!all}),{'aria-label':`${label} group ${title}`,'aria-pressed':mixed?'mixed':String(all),className:`row-icon${mixed?' mixed':''}`,title:`${label}: ${mixed?'mixed':all?'all on':'all off'} · applies to ${refs.length} members`}),key==='hidden'?(all?'eyeOff':'eye'):(all?'lock':'unlock')));
      }
      details.firstElementChild.append(flags);
      flags.addEventListener('click',e=>e.stopPropagation());
    }
    refs.forEach(ref => {
      const obj = surface(s, ref), selected = store.view.selection.some(r => r.kind === ref.kind && r.index === ref.index);
      details.append(element('div', { className: `outliner-row${selected ? ' selected' : ''}` }, [
        button(obj.editor.name || `${ref.kind} ${ref.index + 1}`, e => {
          const selection=e.shiftKey ? rangeSelection(order,store.view.selectionAnchor,ref) : modKey(e) ? toggleSelection(store.view.selection,ref) : [ref];
          store.setView({selection,...(!e.shiftKey?{selectionAnchor:ref}:{})});
        }, { className: 'object-name', 'aria-pressed': String(selected) }),
        decorateIcon(button('', () => store.command({ op: 'surface.setMetadata', refs: [ref], hidden: !obj.editor.hidden }), { className: `row-icon${obj.editor.hidden ? ' off' : ''}`, title: 'Toggle visibility (does not change acoustics)', 'aria-label': `Visibility ${ref.kind} ${ref.index + 1}` }),obj.editor.hidden?'eyeOff':'eye'),
        decorateIcon(button('', () => store.command({ op: 'surface.setMetadata', refs: [ref], locked: !obj.editor.locked }), { className: `row-icon${obj.editor.locked ? ' lit' : ''}`, title: 'Toggle object lock', 'aria-label': `Lock ${ref.kind} ${ref.index + 1}` }),obj.editor.locked?'lock':'unlock')]));
    }); body.push(details);
  });
  const sources = element('details', { open: true }, [element('summary', { text: `Sources / listener · ${store.state.document.sources.length + 1}` }),
    ...store.state.document.sources.map((source, index) => element('div', { className: `outliner-row${store.view.selection.some(r => r.kind === 'source' && r.index === index) ? ' selected' : ''}` }, button(source.name, () => store.setView({ selection: [{ kind: 'source', index }] }), { className: 'object-name' }))),
    element('div', { className: `outliner-row${store.view.selection.some(r => r.kind === 'listener') ? ' selected' : ''}` }, button('Listener', () => store.setView({ selection: [{ kind: 'listener', index: 0 }] }), { className: 'object-name' }))]);
  body.push(sources);
  const name = element('input', { type: 'text', value: 'Construction', maxLength: 128, 'aria-label': 'Group name' });
  const refs = () => store.view.selection.filter(r => !['source', 'listener'].includes(r.kind)).map(({ kind, index }) => ({ kind, index }));
  body.push(element('div', { className: 'outliner-groups' }, [element('h3', { text: 'Groups' }), name, element('div', { className: 'inline' }, [button('Link', () => store.command({ op: 'surface.link', refs: refs(), groupName: name.value })), button('Unlink', () => store.command({ op: 'surface.unlink', refs: refs() }))]), note('Ctrl/Cmd toggles · Shift selects a range. Hide affects display only.')]));
  const area = scrollArea(body, { className: 'outliner-body' });
  const root = element('aside', { className: 'outliner panel', 'aria-label': 'Room outliner' }, [element('div', { className: 'card-title' }, 'Room outliner'), area]);
  return root;
}
