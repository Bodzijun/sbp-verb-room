import { modKey } from './state/platform.js';
import {analysisFooterState} from './components/analysis-progress.js';
import { inspectorSplitter } from './components/inspector-splitter.js';
import { bakeFooterText } from './components/path-bake.js';
import { createMockBridge, FIXTURES } from './bridge/mock.js';
import { createJuceBridge } from './bridge/juce.js';
import { createStore } from './state/store.js';
import { element, button, select, check, row, note, unavailable, scrollArea, closeDropdowns } from './components/dom.js';
import { parameterControl } from './components/parameter-control.js';
import { editInspector } from './components/edit-inspector.js';
import { analysisPanel } from './components/analysis.js';
import { outliner } from './components/outliner.js';
import { macroRow, mainBottom, mainContext, mainSelectable } from './pages/main.js'; import { timelinePanel } from './components/timeline.js';
import { modulationEditorPanel, requestModEditorFocus, requestModReturnFocus, applyPendingModFocus } from './components/modulation.js';
import { editTools, layerBar } from './pages/edit-room.js';
import { createViewport } from './viewport/viewport.js';
import { surfaceRefs, surface, layerKey } from './state/selectors.js';
import { orientationMode } from './viewport/movement-path.js';
import { budget } from './state/selectors.js';
import { snapFace } from './viewport/snap.js';
import { viewportPalette, THEME_LABELS } from './viewport/palette.js';
import { historyButtons } from './components/history.js';
import { roomOptions, roomCommand } from './state/catalogs.js';
import { decorateIcon } from './components/icons.js';
import { createAboutDialog } from './components/about.js';
const app = document.getElementById('app'), notification = document.getElementById('notification');
app.addEventListener('scroll', e => {
  const el = e.target;
  if (el instanceof HTMLElement && !/^(INPUT|TEXTAREA)$/.test(el.tagName) && el.scrollLeft) el.scrollLeft = 0;
}, { capture: true, passive: true });
const params = new URLSearchParams(location.search), requestedBridge = params.get('bridge') || 'mock';
let bridge, store, viewport, aboutDialog = null, rendered = null, pendingRender = false, closed = false, lastSelection = '';
// Scroll positions survive the full re-render after every command. They are matched by a stable key (class + occurrence)
// instead of the element index, and re-applied after layout settles (the status line and banners change the heights after
// the first restore, which would otherwise clamp a long list such as 32 Sources). A user scroll cancels the pending restore.
let pendingScroll = null, renderSeq = 0;
const scrollKeys = nodes => { const seen = new Map(); return nodes.map(n => { const base = String(n.className); const i = seen.get(base) || 0; seen.set(base, i + 1); return `${base}#${i}`; }); };
for (const type of ['wheel', 'pointerdown', 'keydown', 'touchstart']) document.addEventListener(type, () => { pendingScroll = null; }, { capture: true, passive: true });
function errorMessage(error) {
  if(!error || typeof error!=='object')error={message:String(error)};
  bridge?.diagnostic?.(error);
  notification.replaceChildren(element('span', { text: `${error.code || 'Error'} · ${error.message}` }), button('×', () => { store?.clearError(); notification.replaceChildren(); }, { 'aria-label': 'Dismiss notification' }));
  notification.classList.add('visible');
}
// Surfaces are edited only in Edit room; going back to Main drops any surface selection.
function leaveEdit() { viewport?.cancelGesture?.();store.setView({ page: 'main', pathDrawing: null, pathDraft: null, pathRedo: null, selection: store.view.selection.filter(mainSelectable) }); }
function themePanel() {
  const state = store.state, preferences = state.preferences || {};
  const set = patch => store.command({ op: 'preferences.set', patch });
  const panel = element('details', { className: 'theme-menu', open: !!store.view.themeOpen }, [element('summary', { text: '◇ Theme' })]);
  const content = element('div', { className: 'theme-content panel' }, [element('div',{className:'theme-heading'},[element('h3', { text: 'Theme' }),button('×',()=>{panel.open=false;store.view.themeOpen=false;},{'aria-label':'Close theme'})]),
    // Window: the plugin chrome (panels, buttons, tabs). Viewport: the room preview / editor canvas.
    element('h3', { className: 'theme-section', text: 'Window' }),
    row('Accent', element('input', { type: 'color', value: preferences.accent || '#15d5a5', 'aria-label': 'Chrome accent colour', onchange: e => set({ accent: e.target.value }) })),
    row('Background tint', element('input', { type: 'color', value: preferences.chromeTint || '#101518', 'aria-label': 'Window background tint colour', onchange: e => set({ chromeTint: e.target.value, chromeTintAmount: preferences.chromeTintAmount || 12 }) })),
    row('Tint strength', select('Window tint strength', [[0, 'Off'], [6, '6 %'], [12, '12 %'], [18, '18 %'], [25, '25 %'], [35, '35 %']], preferences.chromeTintAmount || 0, n => set({ chromeTintAmount: Number(n) }))),
    element('h3', { className: 'theme-section', text: 'Viewport' }),
    element('div',{className:'theme-presets'},['Amber', 'Ember red', 'Ice blue', 'Violet', 'Monochrome', 'Classic emerald', 'Custom'].map(name=>button(THEME_LABELS[name]||name,()=>set({viewportTheme:name,roles:{}}),{className:(preferences.viewportTheme||'Amber')===name?'active':'','aria-label':THEME_LABELS[name]||name,'aria-pressed':String((preferences.viewportTheme||'Amber')===name)}))),
    row('Energy palette', select('Energy palette', ['Classic', 'Inferno', 'Viridis', 'Ice', 'Grayscale'].map(v => [v, v]), preferences.energyPalette || 'Inferno', energyPalette => set({ energyPalette })))]);
  for (const [role, colour] of Object.entries(viewportPalette(preferences))) content.append(row(({background:'Canvas background',grid:'Grid',wall:'Walls',floor:'Floors / floor zones',ceiling:'Ceilings / ceiling zones',plate:'Plates (raised zones)',opening:'Openings (open sky)',selection:'Selection',reflection:'Reflection rays (hue steps per order)',direct:'Direct path',source:'Sources',listener:'Listener',clutter:'Clutter objects',labels:'Text labels',pathLine:'Path line',pathNode:'Path nodes',pathArrow:'Path arrows',pathMarker:'Path current marker',pathDimmed:'Path (unselected)'})[role], element('input', { type: 'color', value: colour, 'aria-label': `${role} colour`, onchange: e => set({ viewportTheme:'Custom', roles: { ...preferences.roles, [role]: e.target.value } }) })));
  content.append(check('Mouse wheel changes values (Alt+wheel always does)', preferences.wheelChangesValues === true, wheelChangesValues => set({ wheelChangesValues })));
  content.append(check('Material-class colours instead of theme roles', !!preferences.classColours, classColours => set({ classColours })), button('Reset theme', () => set({ viewportTheme: 'Amber', accent: '#15d5a5', chromeTint: '', chromeTintAmount: 0, energyPalette: 'Inferno', roles: {}, materialColours: {}, classColours: false })), note(bridge.mode === 'mock' ? 'Preferences are owned by the bridge. Mock keeps them in memory only.' : 'Preferences are saved for this user.'), ...(bridge.mode === 'juce' ? [button('Diagnostics', async () => { try { const line=await bridge.readDiagnostics(); content.querySelector('[data-ui-diagnostics]')?.remove();const box=element('pre',{'data-ui-diagnostics':'true',className:'note',text:String(line)});box.style.whiteSpace='pre-wrap';content.append(box); } catch(e){errorMessage(e);} },{'data-action':'diagnostics'}), button('Classic UI', () => bridge.useNativeUi().catch(errorMessage), { 'data-action': 'classic-ui' })] : []));
  const heading = content.firstElementChild, rest = [...content.children].slice(1); content.replaceChildren(heading, scrollArea(rest, { className: 'theme-scroll' }));
  panel.append(content); panel.addEventListener('toggle', () => { store.view.themeOpen = panel.open; }); return panel;
}
function aboutButton() {
  return button('About', e => aboutDialog?.open(e.currentTarget), {
    className: 'about-header-btn',
    'data-about-btn': 'true',
    'data-focus-key': 'header-about',
    'aria-haspopup': 'dialog',
    'aria-label': 'About',
    title: 'About Verb Room',
  });
}
// T11 item 2: the viewport takes the remaining width; the control rack has a sensible width range.
// `mainSplit` (preferences) is the rack's share of the workspace width; absent = default proportion. Double-click restores it.
const SPLIT_MIN_RACK = 440, SPLIT_MIN_VIEW = 280;
function applyMainSplit(workspace, share) {
  if (Number.isFinite(share) && share > 0.1 && share < 0.9) workspace.style.setProperty('--rack-w', `clamp(${SPLIT_MIN_RACK}px, ${(share * 100).toFixed(2)}%, calc(100% - ${SPLIT_MIN_VIEW + 6}px))`);
  else workspace.style.removeProperty('--rack-w');
}
function mainSplitter(workspace) {
  const current = () => store.state.preferences?.mainSplit;
  const bar = element('div', { className: 'main-splitter', role: 'separator', tabIndex: 0, 'aria-orientation': 'vertical', 'aria-label': 'Viewport / controls split', title: 'Drag to resize · double-click restores the default proportion' });
  const rackWidth = () => workspace.querySelector(':scope > .rack').getBoundingClientRect().width, total = () => workspace.getBoundingClientRect().width;
  const commit = share => store.command({ op: 'preferences.set', patch: { mainSplit: share } });
  const clampShare = px => Math.max(SPLIT_MIN_RACK, Math.min(total() - SPLIT_MIN_VIEW - 6, px)) / total();
  let start = null;
  bar.addEventListener('pointerdown', e => {
    if (e.button !== 0) return;
    if (e.detail >= 2) { start = null; applyMainSplit(workspace, null); commit(null); e.preventDefault(); return; }
    start = { x: e.clientX, w: rackWidth(), moved: false }; bar.setPointerCapture(e.pointerId); bar.classList.add('dragging'); e.preventDefault();
  });
  bar.addEventListener('pointermove', e => { if (start && Math.abs(e.clientX - start.x) >= 1) { start.moved = true; applyMainSplit(workspace, clampShare(start.w - (e.clientX - start.x))); } });
  const end = e => { if (!start) return; const { x, w, moved } = start; start = null; bar.classList.remove('dragging'); if (e.type === 'pointerup' && moved) { const share = clampShare(w - (e.clientX - x)); applyMainSplit(workspace, share); commit(Number(share.toFixed(4))); } };
  bar.addEventListener('pointerup', end); bar.addEventListener('pointercancel', () => { start = null; bar.classList.remove('dragging'); applyMainSplit(workspace, current()); });
  bar.addEventListener('dblclick', () => { start = null; applyMainSplit(workspace, null); commit(null); });
  bar.addEventListener('keydown', e => {
    if (!['ArrowLeft', 'ArrowRight', 'Home'].includes(e.key)) return; e.preventDefault();
    if (e.key === 'Home') { commit(null); return; }
    commit(Number(clampShare(rackWidth() + (e.key === 'ArrowLeft' ? 24 : -24)).toFixed(4)));
  });
  return bar;
}
function controlRack(ctx) {
  const rack = element('div', { className: 'rack' });
  rack.append(mainContext(ctx), inspectorSplitter(rack, ctx.store), macroRow(ctx), mainBottom(ctx));
  return rack;
}
function header() {
  const state = store.state, mode = state.parameters.values.mode;
  const fixtureIndex = FIXTURES.indexOf(bridge.selectedFixture || 'default-physical'), roomIndex = fixtureIndex >= 0 && fixtureIndex < 4 ? fixtureIndex : 0;
  const modeButton = (value, label) => button(label, () => { bridge.parameters.begin('mode'); bridge.parameters.change('mode', value); bridge.parameters.end('mode'); }, { className: mode === value ? 'active' : '', 'aria-pressed': String(mode === value) });
  if(store.view.page === 'edit') return element('header',{className:'header edit-header'},[
    element('div',{className:'brand'},[element('span',{className:'brand-icon',text:'⬡'}),element('div',{},[element('strong',{text:'VERB ROOM'}),element('small',{text:'PHYSICALLY BASED REVERB'})])]),
    element('div',{className:'edit-room-heading'},[element('b',{text:`Edit room / ${state.document.sceneName}`}),element('small',{text:'Geometry → materials → sound'})]),
    note(bridge.mode === 'mock' ? 'Changes update the mock preview' : 'Changes update the engine'),themePanel(),aboutButton(),button('✓ Done',()=>leaveEdit(),{className:'primary'})]);
  const catalog = store.telemetry['catalogs.list'];
  const rooms = bridge.mode === 'mock' ? [[0, 'Built-in demo room'], [1, 'Legacy stage2 box'], [2, 'Vocal booth (dead)'], [3, 'VSL Grand hall · audience']] :
    roomOptions(catalog);
  const selectedRoom = bridge.mode === 'mock' ? roomIndex : rooms.find(([,name]) => name === state.document.sceneName || name === `User · ${state.document.sceneName}`)?.[0] ?? '';
  if (bridge.mode !== 'mock' && !rooms.some(([id]) => id === selectedRoom)) rooms.unshift(['', state.document.sceneName]);
  return element('header', { className: 'header' }, [element('div', { className: 'brand' }, [element('span', { className: 'brand-icon', text: '⬡' }), element('div', {}, [element('strong', { text: 'VERB ROOM' }), element('small', { text: 'PHYSICALLY BASED REVERB' })])]),
    select('Room preset', rooms, selectedRoom, key => {
      if (bridge.mode === 'mock') return store.command({ op:'preset.load', source:'builtin', id:key });
      const command = roomCommand(key); if (command) store.command(command);
    }, { title: state.document.sceneName, reselect:true }),
    element('div', {className:'preset-actions'}, [button('Save', () => store.command({ op:'preset.save', name:state.document.sceneName })), button('Load file', () => store.command({ op: 'preset.load', source: 'file' })), button('Export IR', () => store.command({ op: 'ir.export' }))]),
    element('div', { className: 'header-actions' }, [...historyButtons(store), unavailable(state, 'ab.compare', 'A / B'), element('div', { className: 'mode-switch' }, [modeButton(0, 'Physical'), modeButton(1, 'Creative'), element('small', { text: state.parameters.effective.mode === 'physical' ? 'ROOM-DERIVED' : 'CREATIVE OVERRIDES' })]), themePanel(), aboutButton(), button(store.view.page === 'main' ? 'Edit room ↗' : '✓ Done', () => {viewport?.cancelGesture?.();if(store.view.page==='main')store.setView({page:'edit',pathDrawing:null,pathDraft:null,pathRedo:null});else leaveEdit();}, { className: 'primary' })])]);
}
// N1: show / hide the engine's clutter objects. Remembered in preferences (default ON); only has an effect while clutter count > 0.
function pathsButton() {
  const on = store.state.preferences?.showPaths !== false, count = (store.state.document.paths || []).filter(p => p?.points?.length >= 2).length;
  return button('Paths', () => store.command({ op: 'preferences.set', patch: { showPaths: !on } }), { className: `${on ? 'active' : ''}${count ? '' : ' idle'}`.trim(), 'aria-pressed': String(on), 'data-paths-toggle': on ? 'on' : 'off',
    title: count ? `${on ? 'Hide' : 'Show'} movement paths (${count}). A path being drawn or edited always stays visible.` : 'No movement paths yet. Select a source or the listener and press Draw path.' });
}
function clutterButton() {
  const on = store.state.preferences?.showClutter !== false, count = store.state.document.scene.clutter?.count || 0;
  return button('Clutter', () => store.command({ op: 'preferences.set', patch: { showClutter: !on } }), { className: `${on ? 'active' : ''}${count ? '' : ' idle'}`.trim(), 'aria-pressed': String(on), 'data-clutter-toggle': on ? 'on' : 'off',
    title: count ? `${on ? 'Hide' : 'Show'} the ${count} clutter objects the engine placed (same seed as the sound)` : 'Clutter count is 0 — nothing to show. Set Clutter objects in Edit room → Scene.' });
}
function modButton() {
  const on = !!store.view.modEditorOpen, count = store.state?.document?.modulation?.routes?.length || 0;
  return button('Mod', () => {
    if (!store.view.modEditorOpen) { requestModEditorFocus('toolbar'); store.setView({ modEditorOpen: true }); }
    else { requestModReturnFocus('toolbar'); store.setView({ modEditorOpen: false }); }
  }, {
    className: on ? 'active' : '',
    'aria-label': on ? 'Close floating modulation editor' : 'Open floating modulation editor',
    'aria-expanded': String(on),
    'aria-pressed': String(on),
    'data-mod-toggle': on ? 'on' : 'off',
    'data-focus-key': 'viewport-mod-toggle',
    title: `${on ? 'Hide' : 'Show'} floating modulation editor (${count} routes)`
  });
}
function toolbar() {
  const nav=element('details', { className: 'nav-tools', open:!!store.view.navOpen }, [element('summary', { text: '⌖', title: 'Frame / navigation', 'aria-label': 'Viewport navigation' }), element('div', {}, [button('Frame all', () => viewport.frameAll()), button('Frame sel.', () => viewport.frameSelection()), select('Ray preview order', [[0,'Direct only'],[1,'Direct + first'],[2,'Direct + first + second']], store.view.rayOrder ?? 2, n=>store.setView({rayOrder:Number(n)})), check('Lock pan', store.state.document.editorFlags.navLocked, navLocked => store.flags({ navLocked }))])]);
  nav.addEventListener('toggle',()=>{store.view.navOpen=nav.open;});
  decorateIcon(nav.firstElementChild,'nav');nav.firstElementChild.title='Frame / navigation · F frame all, Shift+F selection, Home reset';
  // T6 B1/B2: perspectives on the left; overlays/options separated on the right.
  // The room-name caption stays on Main only: the Edit header already carries it.
  const perspectives = [['plan', 'Plan'], ['front', 'Front'], ['side', 'Side'], ['3d', '3D']].map(([projection, text]) => button(text, () => store.setView({ projection }), { className: store.view.projection === projection ? 'active' : '', 'aria-pressed': String(store.view.projection === projection) }));
  return element('div', { className: `viewport-toolbar panel-title toolbar-${store.view.page}` }, [
    store.view.page === 'main' ? element('span', { className: 'room-label', text: `${store.state.document.sceneName} / source placement` }) : null,
    element('div', { className: 'inline view-buttons persp-buttons', role: 'group', 'aria-label': 'View perspective' }, [...perspectives, ...(store.view.page==='main'?[button(store.state.preferences?.mainViewportLocked?'🔒':'🔓',()=>store.command({op:'preferences.set',patch:{mainViewportLocked:!store.state.preferences?.mainViewportLocked}}),{'aria-label':'Lock Main viewport','aria-pressed':String(!!store.state.preferences?.mainViewportLocked),className:store.state.preferences?.mainViewportLocked?'active':''})]:[])]),
    element('div', { className: 'inline view-buttons overlay-buttons', role: 'group', 'aria-label': 'Overlays and options' }, [
      button('Energy', () => store.setView({ energy: !store.view.energy }), { className: store.view.energy ? 'active' : '', 'aria-pressed': String(store.view.energy) }),
      button('Rays', () => store.setView({ rays: !store.view.rays }), { className: store.view.rays ? 'active' : '', 'aria-pressed': String(store.view.rays) }),
      clutterButton(),
      pathsButton(),
      modButton(),
      ...(store.view.page==='edit'?[button('▧',()=>store.setView({perspectivePreview:!(store.view.perspectivePreview??['room','wall','floorZone','ceilingZone','plate'].includes(store.view.tool))}),{'aria-label':'Perspective preview','aria-pressed':String(store.view.perspectivePreview??['room','wall','floorZone','ceilingZone','plate'].includes(store.view.tool)),title:'3D inset · default follows drawing tool'})]:[]),
      nav])]);
}
let cpuWarningLevel = 'none';
let lastBakeState='',bakeFinishedUntil=0,analysisJob=null,analysisStarted=0;
function footer() {
  const state = store.state, usage = budget(state.document);
  return element('footer', { className: 'footer' }, [element('span', { text: `● ${state.document.sources.length} sources · ${usage.points}/${state.limits.maxPoints} points · ${state.document.scene.walls.length}/${state.limits.maxWalls} walls · revision ${state.revision}` }),
    element('span',{className:'rebuild-indicator',role:'status','aria-live':'polite'},[element('span',{text:'Rebuilding…'}),element('i')]),
    element('span', { className: 'cpu-warning', role: 'status', 'aria-live': 'polite', hidden: true }),
    element('span',{className:'footer-analysis',hidden:true},[element('span',{className:'footer-analysis-text',role:'status','aria-live':'polite'}),button('Cancel',()=>store.command({op:'analysis.cancel'}),{className:'footer-analysis-cancel'})]),
    element('span',{className:'footer-bake',hidden:true},[element('span',{className:'footer-bake-text',role:'status','aria-live':'polite'}),button('Cancel',()=>store.command({op:'path.bakeCancel'}),{className:'footer-bake-cancel',hidden:true}),button('Bake again',()=>{const b=store.telemetry['status.load']?.pathBake;if(b)store.command({op:'path.bake',object:b.object>=0?b.object:'all'});},{className:'footer-bake-again',hidden:true,title:'The baked path no longer matches the room, quality or path: bake it again'})]),
    element('span', { className: 'footer-status', role:'status', text: store.telemetry['status.load']?.status || state.load.status }), element('span', { text: bridge.mode === 'mock' ? `MOCK · ${state.capabilities.pluginVersion} contract · no audio` : `ENGINE · ${state.capabilities.pluginVersion}` })]);
}
function disposeControls() { for (const node of app.querySelectorAll('*')) if (node !== viewport && !node.closest('.perspective-inset') && typeof node.dispose === 'function') node.dispose(); }
function render() {
  if (!store.state || closed) return;
  if (viewport?.isInteracting || app.querySelector('[data-gesture-active]')) { pendingRender = true; return; }
  if (store.view.page === 'main' && store.view.selection.some(r => !mainSelectable(r))) store.view.selection = store.view.selection.filter(mainSelectable);
  closeDropdowns();
  const selectionKey = JSON.stringify(store.view.selection);
  if (store.view.page === 'edit' && selectionKey !== lastSelection && store.view.selection.length && store.view.inspectorTab === 'scene') store.view.inspectorTab = 'selected';
  lastSelection = selectionKey;
  const active = document.activeElement, key = active?.getAttribute('data-focus-key'), restoreFocus = !!key && (active.tagName === 'INPUT' || active.matches?.(':focus-visible')), selection = restoreFocus && typeof active.setSelectionRange === 'function' && active.selectionStart !== null ? [active.selectionStart, active.selectionEnd] : null;
  const oldScrollNodes = [...app.querySelectorAll('.scroll')], oldScrollKeys = scrollKeys(oldScrollNodes);
  const scrolls = new Map(oldScrollNodes.map((n, i) => [oldScrollKeys[i], pendingScroll?.get(oldScrollKeys[i]) ?? n.scrollTop]));
  disposeControls();
  const ctx = { store, bridge, param: (id, opts) => parameterControl(store, bridge, id, opts), snapStep: () => viewport?.currentSnapStep?.() ?? 0.1 };
  const state = store.state;
  document.documentElement.style.setProperty('--accent', state.preferences?.accent || '#15d5a5');
  {const tint=state.preferences?.chromeTint,amount=Math.max(0,Math.min(35,Number(state.preferences?.chromeTintAmount)||0));
    document.documentElement.style.setProperty('--chrome-tint', /^#[0-9a-f]{6}$/i.test(tint||'')&&amount?tint:'transparent');document.documentElement.style.setProperty('--chrome-tint-a', String(amount/100));}
  document.documentElement.dataset.theme = state.preferences?.viewportTheme || 'Amber';
  document.documentElement.dataset.wheelValues = state.preferences?.wheelChangesValues === true ? 'on' : 'off';
  if (!viewport) { viewport = createViewport(store, bridge); viewport.addEventListener('snapstep', refreshSnapFace); viewport.addEventListener('interactionend', () => { if (pendingRender) { pendingRender = false; render(); } }); }
  const centre = element('div', { className: 'viewport-section' }, [toolbar(), store.view.page === 'edit' ? layerBar(ctx) : null, viewport]);
  viewport.querySelector(':scope > .analysis')?.remove();
  if(store.view.energy)viewport.append(analysisPanel(ctx)); viewport.querySelector(':scope > .timeline-panel')?.remove(); if (store.view.timelineOpen) viewport.append(timelinePanel(ctx));
  viewport.querySelector(':scope > .mod-editor-panel')?.remove(); if (store.view.modEditorOpen) viewport.append(modulationEditorPanel(ctx));
  const workspace = element('main', { className: store.view.page === 'edit' ? 'workspace edit-layout' : 'workspace main-layout' });
  if (store.view.page === 'edit') {
    workspace.append(editTools(ctx));
    if (state.document.editorFlags.outlinerVisible) {
      const dock = outliner(ctx), resize = element('div', { className: 'dock-resizer', role: 'separator', tabIndex: 0, 'aria-label': 'Outliner width', 'aria-valuemin': 150, 'aria-valuemax': 310, 'aria-valuenow': state.preferences?.dockWidth || 190, 'aria-orientation': 'vertical' });
      const width = state.preferences?.dockWidth || 190; dock.style.width = `${width}px`;
      resize.addEventListener('keydown', e => { if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') { e.preventDefault(); store.command({ op: 'preferences.set', patch: { dockWidth: Math.max(150, Math.min(310, width + (e.key === 'ArrowRight' ? 10 : -10))) } }); } });
      let start = null;
      resize.addEventListener('pointerdown', e => { start = e.clientX; resize.setPointerCapture(e.pointerId); });
      resize.addEventListener('pointermove', e => { if (start !== null) dock.style.width = `${Math.max(150, Math.min(310, width + e.clientX - start))}px`; });
      resize.addEventListener('pointerup', () => { start = null; store.command({ op: 'preferences.set', patch: { dockWidth: parseFloat(dock.style.width) } }); });
      workspace.append(element('div', { className: 'dock' }, [dock, resize]));
    }
    workspace.append(centre, editInspector(ctx));
  } else { workspace.append(centre, mainSplitter(workspace), controlRack(ctx)); applyMainSplit(workspace, state.preferences?.mainSplit); }
  const dev = bridge.mode === 'mock' && params.get('dev') !== '0' ? element('details', { className: 'mock-dev' }, [element('summary', { text: 'MOCK', title: 'Development fixtures' }), element('div', { className: 'mock-bar panel' }, [element('strong', { text: 'MOCK BRIDGE · offline fixtures · no audio' }), select('Mock fixture', FIXTURES.map(v => [v, v]), bridge.selectedFixture, name => store.fixture(name)), button('Error / resync', () => bridge.injectResync()), button('Host automation demo', () => bridge.automate('mix', state.parameters.values.mix === 100 ? 37.5 : 100)), element('span', { text: 'Energy / curves / meters are synthetic test data' })])]) : null;
  app.replaceChildren(...[header(), dev, workspace, footer()].filter(Boolean)); app.setAttribute('aria-busy', String(!store.ready));
  const restoreScrolls = () => { const nodes = [...app.querySelectorAll('.scroll')], keys = scrollKeys(nodes); nodes.forEach((n, i) => { const want = scrolls.get(keys[i]); if (want != null && Math.abs(n.scrollTop - want) > 1) n.scrollTop = want; }); };
  restoreScrolls(); pendingScroll = scrolls;
  const stamp = ++renderSeq, again = () => { if (renderSeq === stamp && pendingScroll === scrolls) restoreScrolls(); };
  requestAnimationFrame(() => { again(); requestAnimationFrame(again); }); setTimeout(() => { again(); if (pendingScroll === scrolls) pendingScroll = null; }, 250);
  if (restoreFocus) { const next = [...app.querySelectorAll('[data-focus-key]')].find(n => n.getAttribute('data-focus-key') === key); if (next) { next.focus({ preventScroll: true }); if (selection) next.setSelectionRange(...selection); } }
  applyPendingModFocus(app);
  refreshSnapFace();
  updateStatus();
  if (store.error) errorMessage(store.error);
}
function refreshSnapFace() { const text = document.querySelector('.snap-dd .dd-text'); if (text && viewport) text.textContent = snapFace(store.view, viewport.currentSnapStep()); }
function updateStatus() {
  const analysis=store.telemetry['analysis.progress'],result=store.telemetry['analysis.result'],analysisNode=app.querySelector('.footer-analysis');
  if(analysis && analysis.jobId!==analysisJob){analysisJob=analysis.jobId;analysisStarted=Date.now();setTimeout(()=>{if(!closed)updateStatus();},1050);}
  if(analysisNode){const footer=analysisFooterState(analysis,result,analysisStarted,Date.now());analysisNode.hidden=!footer.visible;analysisNode.querySelector('.footer-analysis-text').textContent=footer.text;}

  const bake=store.telemetry['status.load']?.pathBake,bakeNode=app.querySelector('.footer-bake');
  if(bakeNode){
    if(bake?.state==='baked'&&lastBakeState!=='baked'){bakeFinishedUntil=Date.now()+4000;setTimeout(()=>{if(!closed)updateStatus();},4100);}
    lastBakeState=bake?.state||'';const text=bakeFooterText(bake);bakeNode.hidden=!text||(bake?.state==='baked'&&!bake?.reason&&Date.now()>bakeFinishedUntil);
    bakeNode.querySelector('.footer-bake-text').textContent=text;bakeNode.querySelector('.footer-bake-cancel').hidden=bake?.state!=='baking';
    // An outdated bake is a standing warning with its own action, not a passing status line.
    const stale=bake?.state==='outdated';bakeNode.classList.toggle('warn',stale);bakeNode.querySelector('.footer-bake-again').hidden=!stale;
  }
  const rebuilding=!!store.telemetry['status.load']?.rebuilding;
  document.body.classList.toggle('rebuilding',rebuilding);
  document.querySelector('.decay-graph')?.setAttribute('aria-busy',String(rebuilding));
  syncRebuildingBanner(rebuilding);
  const cpu = store.telemetry['status.load']?.cpu;
  const cpuNode = app.querySelector('.cpu-warning');
  if (cpuNode) {
    if (!cpu || !cpu.active) cpuWarningLevel = 'none';
    else {
      const worst = Number(cpu.worstPercent) || 0, overrun = !!cpu.recentOverrun;
      if (worst >= 100 || overrun) cpuWarningLevel = 'red';
      else if (cpuWarningLevel === 'red') { if (worst < 85 && !overrun) cpuWarningLevel = worst >= 70 ? 'amber' : 'none'; }
      else if (worst >= 70) cpuWarningLevel = 'amber';
      else if (cpuWarningLevel === 'amber') { if (worst < 60) cpuWarningLevel = 'none'; }
    }
    if (cpuWarningLevel === 'none') {
      cpuNode.hidden = true; cpuNode.className = 'cpu-warning'; cpuNode.textContent = '';
      cpuNode.removeAttribute('title'); cpuNode.removeAttribute('data-level');
    } else {
      const worstPct = Math.round(Number(cpu.worstPercent) || 0), meanPct = Math.round(Number(cpu.meanPercent) || 0);
      const block = cpu.blockSize || 256, sr = cpu.sampleRate || store.state?.host?.sampleRate || 48000;
      const srKhz = `${Number((sr / 1000).toFixed(1))} kHz`;
      cpuNode.hidden = false; cpuNode.className = `cpu-warning ${cpuWarningLevel}`; cpuNode.dataset.level = cpuWarningLevel;
      cpuNode.textContent = cpuWarningLevel === 'red'
        ? `Audio overload — increase the audio buffer (now ${block} samples @ ${srKhz}) or reduce sources / tail quality`
        : `High CPU: ${worstPct} % of the audio buffer`;
      cpuNode.title = `Average ${meanPct} % · worst ${worstPct} % · ${block} samples @ ${srKhz} · overruns ${cpu.overruns ?? 0}`;
    }
  }
  const node = app.querySelector('.footer-status'); if (!node) return;
  const file = store.telemetry['status.file'], exp = store.telemetry['status.export'];
  const loadStatus = store.telemetry['status.load']?.status || store.state.load.status;
  const text = [statusNote.text, file?.message, exp?.state ? exp.message || 'Exporting IR…' : '', statusNote.text ? '' : loadStatus].filter(Boolean).join(' · ');
  // A finished action's message must not look like a running process: show it for a few seconds, then clear it.
  if (text !== footerStatus.text) {
    footerStatus.text = text; clearTimeout(footerStatus.timer); footerStatus.timer = null; footerStatus.expired = false;
    if (text && !statusInProgress(text)) footerStatus.timer = setTimeout(() => { footerStatus.expired = true; updateStatus(); }, FOOTER_STATUS_MS);
  }
  node.textContent = footerStatus.expired ? '' : text; node.title = text;
}
const REBUILD_APPEAR_MS = 150, REBUILD_MIN_VISIBLE_MS = 400;
const rebuildBanner = { want: false, visible: false, shownAt: 0, appearTimer: null, hideTimer: null };
let statusNote = { text: '', timer: null };
if (typeof window !== 'undefined') {
  window.addEventListener('verb-status-note', e => {
    clearTimeout(statusNote.timer);
    const text = String(e.detail?.text || '').trim();
    const duration = Number(e.detail?.durationMs) || 4500;
    statusNote = {
      text,
      timer: text ? setTimeout(() => { statusNote.text = ''; statusNote.timer = null; updateStatus(); }, duration) : null
    };
    updateStatus();
  });
}
function syncRebuildingBanner(want) {
  rebuildBanner.want = !!want;
  if (rebuildBanner.want) {
    if (rebuildBanner.hideTimer) { clearTimeout(rebuildBanner.hideTimer); rebuildBanner.hideTimer = null; }
    if (!rebuildBanner.visible && !rebuildBanner.appearTimer) {
      rebuildBanner.appearTimer = setTimeout(() => {
        rebuildBanner.appearTimer = null;
        if (!closed && rebuildBanner.want) {
          rebuildBanner.visible = true;
          rebuildBanner.shownAt = Date.now();
          viewport?.setRebuilding?.(true);
        }
      }, REBUILD_APPEAR_MS);
    }
  } else {
    if (rebuildBanner.appearTimer) { clearTimeout(rebuildBanner.appearTimer); rebuildBanner.appearTimer = null; }
    if (rebuildBanner.visible) {
      const remaining = REBUILD_MIN_VISIBLE_MS - (Date.now() - rebuildBanner.shownAt);
      if (remaining <= 0) {
        if (rebuildBanner.hideTimer) { clearTimeout(rebuildBanner.hideTimer); rebuildBanner.hideTimer = null; }
        rebuildBanner.visible = false;
        viewport?.setRebuilding?.(false);
      } else if (!rebuildBanner.hideTimer) {
        rebuildBanner.hideTimer = setTimeout(() => {
          rebuildBanner.hideTimer = null;
          if (!closed && !rebuildBanner.want) {
            rebuildBanner.visible = false;
            viewport?.setRebuilding?.(false);
          }
        }, remaining);
      }
    }
  }
  viewport?.setRebuilding?.(rebuildBanner.visible);
}
const FOOTER_STATUS_MS = 6000, footerStatus = { text: null, timer: null, expired: false };
export function statusInProgress(text) { return /(\.\.\.|…)\s*$/.test(text) || /\d+\s*\/\s*\d+/.test(text) || /^Error|failed/i.test(text); }
async function init() {
  bridge = requestedBridge === 'mock' ? await createMockBridge({ fixture: params.get('fixture') || 'default-physical' }) : createJuceBridge({ onError:errorMessage });
  store = createStore(bridge);
  aboutDialog = createAboutDialog({ store, getBridgeMode: () => bridge?.mode });
  const hash = location.hash.slice(1);
  if (hash.startsWith('edit')) { store.view.page = 'edit'; store.view.selection=[{kind:'wall',index:1}]; if (hash === 'edit-3d') store.view.projection = '3d'; if (hash === 'edit-source') store.view.selection = [{ kind: 'source', index: 0 }]; if (hash === 'edit-scene') { store.view.selection = []; store.view.inspectorTab = 'scene'; } }
  if (['sources', 'mix', 'mix-creative', 'modulation', 'output'].includes(hash)) store.view.tab = hash === 'mix-creative' ? 'mix' : hash;
  if (hash === 'sources') store.view.selection = [{ kind: 'source', index: 0 }];
  if (['front', 'side', '3d'].includes(hash)) store.view.projection = hash;
  if (hash === 'energy') store.view.energy = true;
  if (hash === 'listener') store.view.selection = [{ kind: 'listener', index: 0 }];
  if (hash === 'theme') store.view.themeOpen = true;
  store.subscribe(({ reason }) => {
    if (reason === 'error') errorMessage(store.error);
    if (reason === 'dismiss') { notification.replaceChildren(); notification.classList.remove('visible'); }
    if (!store.state) return;
    aboutDialog?.sync(store.state);
    // Only state/view changes may rebuild chrome. Meter/status events never scan the document.
    if (['snapshot','patch','parameters','view','resync','catalogs.list'].includes(reason)) {
      const state = store.state;
      const viewKey = ['view', 'snapshot', 'resync'].includes(reason)
        ? JSON.stringify({ ...store.view, camera: undefined, previewCamera: undefined, pathDraft: undefined, cornerJoinResult: store.cornerJoinResult }) : rendered?.viewKey;
      const docSig = rendered && rendered.document === state.document ? rendered.docSig : JSON.stringify(state.document);
      const prefsSig = rendered && rendered.preferences === state.preferences ? rendered.prefsSig : JSON.stringify({ ...(state.preferences || {}), sessionViewState: undefined });
      // Host channel counts / layout feed the inspector and the source budget text: a host change must rebuild them.
      const hostSig = JSON.stringify(state.host || null);
      if (!rendered || docSig !== rendered.docSig || prefsSig !== rendered.prefsSig || hostSig !== rendered.hostSig || rendered.mode !== state.parameters.values.mode || rendered.bridgeMode !== bridge.mode || rendered.ready !== store.ready || viewKey !== rendered.viewKey || reason === 'catalogs.list') {
        rendered = { document:state.document, preferences:state.preferences, docSig, prefsSig, hostSig, mode:state.parameters.values.mode, bridgeMode:bridge.mode, ready:store.ready, viewKey };
        render();
      }
    }
    if (['snapshot','patch','status.load','status.file','status.export','analysis.progress','analysis.result','analysis.cleared'].includes(reason)) updateStatus();
  });
  await store.start();
  if (hash === 'theme' && bridge.mode === 'mock') await store.command({op:'preferences.set',patch:{viewportTheme:'Ice blue',energyPalette:'Viridis'}});
  if (['creative', 'mix-creative'].includes(hash)) { bridge.parameters.begin('mode'); bridge.parameters.change('mode', 1); bridge.parameters.end('mode'); }
  if (hash === 'edit-outliner') await store.flags({ outlinerVisible: true });
  if (hash === 'edit-source') store.setView({ selection: [{ kind: 'source', index: 0 }] });
  // Debug access exists only in mock mode; components use the same bridge interface.
  if (bridge.mode === 'mock') window.verbRoom = { store, bridge, version: 'T2' };
}
document.addEventListener('keydown', e => {
  if (e.defaultPrevented || document.querySelector('dialog[open]')) return;
  if (!store || /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName) || e.target.isContentEditable) return;
  const ctrl=modKey(e),key=e.key.toLowerCase();
  if (['Delete','Backspace'].includes(e.key)) {
    e.preventDefault();
    const hasSurfSel = store.view.page === 'edit' && store.view.selection.some(r => !['source', 'listener', 'floor', 'ceiling'].includes(r.kind));
    if (!hasSurfSel && store.view.timelineOpen && store.view.azKey != null && !(store.view.page === 'main' && store.state.preferences?.mainViewportLocked)) {
      const ak = store.view.azKey, path = store.state?.document?.paths?.[ak.object];
      if (path && orientationMode(path) === 'manual' && path.azKeys?.[ak.index]) {
        store.setView({ azKey: null }, false);
        store.command({ op: 'path.removeAzimuthKey', object: ak.object, index: ak.index });
        return;
      }
    }
    if (!hasSurfSel && store.view.pathPoint != null && !(store.view.page === 'main' && store.state.preferences?.mainViewportLocked)) {
      const pt = store.view.pathPoint, path = store.state?.document?.paths?.[pt.object];
      if (path?.points?.length > 2) {
        const nextIdx = Math.min(pt.index, path.points.length - 2);
        store.command({ op: 'path.removePoint', object: pt.object, index: pt.index });
        store.setView({ pathPoint: { object: pt.object, index: nextIdx, point: path.points[nextIdx] } });
      } else if (path?.points?.length) {
        errorMessage({ code: 'Path', message: 'A path needs at least 2 points — use Clear path to remove it' });
      }
      return;
    }
    if(store.view.page==='edit'&&store.view.selection.length){
      const refs=store.view.selection.filter(r=>!['source','listener','floor','ceiling'].includes(r.kind)).map(({kind,index})=>({kind,index}));
      const sources=store.view.selection.filter(r=>r.kind==='source').sort((a,b)=>b.index-a.index);
      const tx=`delete-${Date.now()}`;
      if(refs.length||sources.length){store.command({op:'transaction.begin',label:'Delete selection'},tx);if(refs.length)store.command({op:'surface.delete',refs},tx);for(const r of sources)store.command({op:'source.remove',index:r.index},tx);store.command({op:'transaction.commit'},tx);store.setView({selection:[]});}
    }return;
  }
  if(ctrl&&['z','y','a','x','c','v','d'].includes(key)){
    e.preventDefault();
    if(key==='z'&&!e.shiftKey&&viewport?.undoDraft?.())return;
    if(key==='z'||key==='y')store.command({op:key==='y'||e.shiftKey?'history.redo':'history.undo'});
    if(key==='a'&&store.view.page==='edit'){const s=store.state.document.scene;store.setView({selection:surfaceRefs(s).filter(r=>!['floor','ceiling'].includes(r.kind)&&!surface(s,r).editor.hidden&&!surface(s,r).editor.locked&&!(store.state.document.editorFlags.layerModes?.[layerKey(s,r)]||0))});}
    return;
  }
  // Viewport shortcuts work wherever the focus is (not only after a click on the canvas): F / Shift+F frame, Home resets.
  if(!ctrl&&!e.altKey&&(key==='f'||e.key==='Home')){const canvas=viewport?.querySelector('canvas');if(canvas&&e.target!==canvas){e.preventDefault();canvas.focus();canvas.dispatchEvent(new KeyboardEvent('keydown',{key:e.key,code:e.code,shiftKey:e.shiftKey,bubbles:false,cancelable:true}));return;}}
  if(e.key.startsWith('Arrow')&&store.view.selection.length){e.preventDefault();return;}
  if (e.key === 'Escape') {
    e.preventDefault();
    if (document.querySelector('.dd-popup')) { closeDropdowns(); return; }
    const inModWin = !!e.target?.closest?.('.mod-editor-panel');
    if (inModWin && store.view.modEditorOpen) { requestModReturnFocus(); store.setView({ modEditorOpen: false }); }
    else if (store.view.timelineOpen) store.setView({ timelineOpen: false }, false);
    else if (store.view.modEditorOpen) { requestModReturnFocus(); store.setView({ modEditorOpen: false }); }
    else if (store.view.pathDrawing != null || store.view.pathDraft) store.setView({ pathDrawing: null, pathDraft: null });
    else store.setView({ selection: [], themeOpen: false, pathEditing: null, pathPoint: null, azKey: null });
  }
  if(store.view.page==='edit'&&!modKey(e)&&!e.ctrlKey&&!e.altKey){const tool={v:'move',b:'box',r:'room',w:'wall',z:'floorZone',c:'ceilingZone',p:'plate'}[e.key.toLowerCase()];if(tool){e.preventDefault();store.setView({tool});viewport?.querySelector('canvas').focus();}}
});
// The browser's own context menu (Back / Reload / Inspect) has no place in a plugin window; text fields keep theirs for copy/paste.
document.addEventListener('contextmenu',e=>{if(!(/^(INPUT|TEXTAREA)$/.test(e.target.tagName)||e.target.isContentEditable))e.preventDefault();});
document.addEventListener('verb-viewport-error',e=>errorMessage(e.detail));
const recoverUi = detail => {
  window.dispatchEvent(new CustomEvent('verb-ui-error',{detail}));viewport?.cancelGesture?.();closeDropdowns();
  document.querySelectorAll('[data-gesture-active]').forEach(el=>{el.removeAttribute('data-gesture-active');el.dispatchEvent(new Event('pointercancel'));});
  store?.recover(detail);
};
window.addEventListener('verb-bridge-error',e=>recoverUi(e.detail));
window.addEventListener('error',e=>{errorMessage(e.error||e.message);recoverUi(e.error||e.message);});
window.addEventListener('unhandledrejection',e=>{errorMessage(e.reason);recoverUi(e.reason);});
window.addEventListener('verb-control-end', () => { if (pendingRender) { pendingRender = false; render(); } });
window.addEventListener('pagehide', () => { closed = true; aboutDialog?.destroy(); aboutDialog = null; clearTimeout(rebuildBanner.appearTimer); clearTimeout(rebuildBanner.hideTimer); clearTimeout(footerStatus.timer); disposeControls(); viewport?.dispose(); store?.dispose(); bridge?.dispose(); });
init().catch(error => { app.replaceChildren(element('p', { className: 'loading', text: `Cannot open Verb Room: ${error.message}` })); errorMessage(error); });
