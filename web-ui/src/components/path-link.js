// V04d: "Link to path…" (rig). A source follows another source's trajectory with a manual offset in the path frame
// (Forward = drawn tangent, Left = horizontal perpendicular, Up = world Z). All edits go through scoped commands:
// path.link (one or several sources, one undo step), path.setFollow (offset gesture = one undo step, Esc cancels),
// path.unlink (the follower keeps its motion as an own path), path.clear.
import { element, button, check, row, select } from './dom.js';
import { txNumericField } from './tx-gesture.js';
import { isFollower, followersOf, canBeFollowTarget, MAX_FOLLOW_OFFSET_M, resolveFollowPath, compilePath } from '../viewport/movement-path.js';
import { sourceStripColour } from '../viewport/colour.js';

const objName = (state, o) => o === 32 ? 'Listener' : (state.document.sources?.[o]?.name || `Source ${o + 1}`);
const swatch = (state, o) => element('i', { className: 'path-link-swatch', 'aria-hidden': 'true', style: `background:${sourceStripColour(state.document.sources?.[o], o)}` });

// Candidate link targets for `object`: existing sources (not itself) with a usable path that are not followers.
export function linkTargets(state, object) {
  const paths = state.document.paths || [], ns = state.document.sources?.length ?? 0, out = [];
  for (let o = 0; o < ns; o++) if (o !== object && canBeFollowTarget(paths[o])) out.push(o);
  return out;
}
// Sources that may be linked together with `object` in one step (multi-select rig): not the target, not followed by anyone.
function extraCandidates(state, object, target) {
  const paths = state.document.paths || [], ns = state.document.sources?.length ?? 0, out = [];
  for (let o = 0; o < ns; o++) if (o !== object && o !== target && followersOf(paths, o).length === 0) out.push(o);
  return out;
}

export function linkButton(ctx, object) {
  const { store } = ctx;
  const paths = store.state.document.paths || [];
  const followed = followersOf(paths, object);
  const targets = linkTargets(store.state, object);
  const open = store.view.pathLinkPicker?.object === object;
  const disabled = object === 32 || followed.length > 0 || targets.length === 0;
  const title = object === 32 ? 'Only sources can follow a path'
    : followed.length ? `Followed by ${followed.map(o => objName(store.state, o)).join(', ')}: a target cannot follow another path (no chains)`
    : !targets.length ? 'No other source has a path to follow yet: draw or generate one first'
    : 'Follow another source\'s path with an offset (head / hands / feet of one character on one path)';
  return button('Link to path…', () => {
    store.setView({ pathLinkPicker: open ? null : { object, target: targets[0] ?? null, extra: [] } }, false);
   
  }, {
    className: `path-link-btn${open ? ' active' : ''}`,
    'aria-label': 'Link to path',
    'aria-pressed': String(open),
    'data-path-link': String(object),
    disabled,
    title
  });
}

export function linkPicker(ctx, object) {
  const { store } = ctx;
  const pick = store.view.pathLinkPicker;
  if (pick?.object !== object) return null;
  const state = store.state;
  const targets = linkTargets(state, object);
  if (!targets.length) return null;
  const target = targets.includes(pick.target) ? pick.target : targets[0];
  const setPick = patch => { store.setView({ pathLinkPicker: { ...pick, target, ...patch } }, false); };
  const targetList = element('div', { className: 'path-link-targets', role: 'radiogroup', 'aria-label': 'Path to follow' },
    targets.map(o => {
      const p = state.document.paths[o];
      const len = compilePath(p).length;
      return element('button', {
        type: 'button', role: 'radio', 'aria-checked': String(o === target), className: `path-link-target${o === target ? ' is-on' : ''}`,
        'data-link-target': String(o), title: `Follow ${objName(state, o)}'s path`,
        onclick: () => setPick({ target: o, extra: (pick.extra || []).filter(e => e !== o) })
      }, [swatch(state, o), element('span', { text: objName(state, o) }), element('small', { text: `${p.closed ? 'closed' : 'open'} · ${len.toFixed(1)} m${p.endless ? ' · ∞' : ''}` })]);
    }));
  const extras = extraCandidates(state, object, target);
  const extraSel = new Set((pick.extra || []).filter(e => extras.includes(e)));
  const extraList = extras.length ? element('div', { className: 'path-link-extra', 'aria-label': 'Also link' },
    extras.map(o => check(objName(state, o), extraSel.has(o), on => {
      const next = new Set(extraSel); if (on) next.add(o); else next.delete(o);
      setPick({ extra: [...next] });
    }, { 'data-link-extra': String(o), title: `Also link ${objName(state, o)} to the same path (offset 0, 0, 0; set it afterwards)` }))) : null;
  const all = [object, ...[...extraSel].sort((a, b) => a - b)];
  const doLink = button(all.length > 1 ? `Link ${all.length} sources` : 'Link', () => {
    store.setView({ pathLinkPicker: null }, false);
    store.command(all.length > 1 ? { op: 'path.link', objects: all, target } : { op: 'path.link', object, target });
  }, { className: 'primary path-link-apply', 'data-link-apply': String(object), title: 'Replace the own path with a link: one undo step' });
  const cancel = button('Cancel', () => { store.setView({ pathLinkPicker: null }, false); }, { className: 'path-link-cancel' });
  return element('div', { className: 'path-link-picker', 'data-link-picker': String(object) }, [
    element('small', { className: 'path-link-caption', text: 'Follow the path of' }),
    targetList,
    ...(extraList ? [element('small', { className: 'path-link-caption', text: 'Link several sources' }), extraList] : []),
    element('small', { className: 'note', text: 'The own path, Position, Laps, Endless, LFO, wander and Coupling of the linked sources are replaced by the link (Unlink keeps the motion as an own path).' }),
    element('div', { className: 'path-link-actions' }, [doLink, cancel])
  ]);
}

// Note on a target: who follows it.
export function followedNote(ctx, object) {
  const { store } = ctx;
  const f = followersOf(store.state.document.paths || [], object);
  if (!f.length) return null;
  return element('small', { className: 'note path-link-followed', 'data-link-followed': String(object), text: `Rig: followed by ${f.map(o => objName(store.state, o)).join(', ')}` });
}

// The Path inspector of a follower.
export function followerPathControls(ctx, object, { collapsible = false } = {}) {
  const { store } = ctx;
  const state = store.state;
  const p = state.document.paths[object];
  const fo = p.follow;
  const t = fo.target;
  const resolved = resolveFollowPath(state.document.paths, object);
  const usable = (resolved?.points?.length ?? 0) >= 2 && resolved.enabled !== false;
  const disposers = [];
  const head = collapsible ? null : element('div', { className: 'path-head' }, [
    element('h3', { text: 'Path' }),
    element('small', { className: 'path-meta', text: `Linked to ${objName(state, t)}` })
  ]);
  const targetOpts = [...new Set([t, ...linkTargets(state, object)])].filter(o => o >= 0 && o < (state.document.sources?.length ?? 0)).map(o => [o, objName(state, o)]);
  const targetSel = select('Link target', targetOpts, t, v => store.command({ op: 'path.setFollow', object, target: Number(v) }), { 'data-link-target-select': String(object), title: 'Path to follow' });
  disposers.push(() => targetSel.dispose?.());
  const jump = button('Go to', () => store.setView({ selection: [{ kind: 'source', index: t }], pathLinkPicker: null }), {
    className: 'path-link-jump', 'data-link-jump': String(object), 'aria-label': `Select ${objName(state, t)}`, title: `Select the target ${objName(state, t)}`
  });
  const targetRow = element('div', { className: 'row path-link-target-row' }, [element('span', { text: 'Target' }), element('div', { className: 'path-link-target-ctrl' }, [swatch(state, t), targetSel, jump])]);
  const off = [...(fo.offset || [0, 0, 0])];
  const axes = [['Forward', 'Along the target\'s direction of travel (m)'], ['Left', 'Horizontal, to the left of the direction of travel (m)'], ['Up', 'World vertical: stays above / below even when the path climbs (m)']];
  const fields = axes.map(([label, title], k) => {
    const f = txNumericField({
      store,
      label: `Offset ${label}, m`,
      txLabel: 'Edit link offset',
      value: Number(off[k].toFixed(3)),
      min: -MAX_FOLLOW_OFFSET_M,
      max: MAX_FOLLOW_OFFSET_M,
      step: 0.01,
      liveInput: true,
      clampOnSubmit: true,
      title: `${title} · drag vertically · Shift fine · type a value · Esc cancels`,
      getCommitCmd: v => {
        const cur = [...(store.state.document.paths?.[object]?.follow?.offset || off)];
        cur[k] = Number(v.toFixed(4));
        return { op: 'path.setFollow', object, offset: cur };
      }
    });
    f.dataset.linkOffset = ['fwd', 'left', 'up'][k];
    disposers.push(() => f.dispose?.());
    return element('label', { className: 'path-link-offset', title }, [element('span', { text: label }), f]);
  });
  const offsetRow = element('div', { className: 'row path-link-offset-row', title: 'Offset in the path frame (m): Forward / Left / Up' }, [element('span', { text: 'Offset (m)' }), element('div', { className: 'path-link-offsets' }, fields)]);
  const creative = (state.parameters?.values?.mode ?? 0) >= 0.5;
  const targetWander = !!state.document.paths?.[t]?.wander?.enabled;
  const wanderChk = check('Follow wander', fo.followWander !== false, on => store.command({ op: 'path.setFollow', object, followWander: on }), {
    'data-link-wander': String(object),
    title: 'ON: displaced exactly like the target by its Path wander (one shared displacement for the rig). OFF: follows the drawn, undisplaced path.'
  });
  const lockedTip = `Linked: comes from ${objName(state, t)}'s path. Unlink to edit.`;
  const locked = ['Position', 'Laps', 'Endless', 'Path LFO', 'Path wander', 'Coupling'].map(name => element('span', {
    className: 'path-link-locked locked', title: lockedTip, 'data-link-locked': name
  }, [element('i', { className: 'lock-glyph', 'aria-hidden': 'true', text: '🔒' }), element('span', { text: name })]));
  const unlink = button('Unlink (keep as own path)', () => store.command({ op: 'path.unlink', object }), {
    className: 'path-link-unlink', 'data-link-unlink': String(object),
    title: 'Convert to an ordinary path: the current offset curve sampled into 64 points (one undo step)'
  });
  const clear = button('Clear', () => store.command({ op: 'path.clear', object }), { 'aria-label': 'Clear path', title: 'Remove the link and restore static XYZ' });
  const block = element('div', { className: 'path-section path-link-follower', 'data-path-object': String(object), 'data-link-follower': String(object) }, [
    ...(head ? [head] : []),
    targetRow,
    offsetRow,
    element('div', { className: 'path-flags path-link-flags' }, [wanderChk, element('small', { className: `path-link-hint${creative ? '' : ' warning'}`, text: targetWander ? (creative ? 'Target wander active' : 'Wander: Creative mode only') : 'Target has no wander' })]),
    element('div', { className: 'path-link-locks', title: lockedTip }, locked),
    element('div', { className: 'path-actions' }, [unlink, clear]),
    ...(usable ? [] : [element('small', { className: 'note warning', text: `${objName(state, t)} has no usable path: this source stays at its XYZ until the target path is enabled.` })]),
    element('small', { className: 'note path-link-bake-note', text: 'Followers are not baked: they render live (like Endless / wander). Motion needs no Bake.' })
  ]);
  block.dispose = () => disposers.forEach(fn => fn?.());
  return [block];
}

export { isFollower };
