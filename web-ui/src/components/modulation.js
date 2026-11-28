import { element, button, select, check, scrollArea } from './dom.js';
import { createTxGesture, txNumericField, tryCapture } from './tx-gesture.js';
import {
  MOD_MAX_ROUTES, MOD_MAX_HOST_OUTS, MOD_MIN_NODES, MOD_MAX_NODES,
  MOD_DRIVERS, MOD_TARGETS, MOD_PRESETS,
  driverMeta, targetMeta, isGlobalTarget,
  createDefaultRoute, normalizeRoute,
  evaluateModCurve, evaluateModCurveNormalized, computeModulationTelemetry, formatModValue, routeMatchesObject
} from './modulation-model.js';
import { zoneList, zoneLabel, zoneColour, liveOccupancy, computeZoneOccupancy } from '../state/trigger-zones.js';
import { MIDI_MODES, midiNoteName, normalizeMidiOut, describeMidiMessage } from './modulation-model.js';
import { withPathPoses } from '../viewport/movement-path.js';

const SVG_NS = 'http://www.w3.org/2000/svg';
const svgEl = (tag, attrs = {}, children = []) => {
  const n = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k === 'className' ? 'class' : k, String(v));
  for (const c of children) n.append(c);
  return n;
};
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

let pendingModFocus = null;
let lastModOpenSource = 'detach';
export function requestModEditorFocus(source = 'detach') {
  pendingModFocus = 'editor';
  lastModOpenSource = source;
}
export function requestModReturnFocus(source = null) {
  pendingModFocus = (source || lastModOpenSource) === 'toolbar' ? 'toolbar-toggle' : 'detach';
}
export function requestModExpandFocus() { requestModReturnFocus('detach'); }
export function applyPendingModFocus(rootEl) {
  const want = pendingModFocus;
  if (!want || !rootEl) return;
  pendingModFocus = null;
  if (want === 'editor') {
    const panel = rootEl.querySelector('.mod-editor-panel');
    if (!panel) return;
    if (panel.classList.contains('collapsed')) {
      panel.querySelector('.mod-win-min')?.focus({ preventScroll: true });
      return;
    }
    const target = panel.querySelector(
      '.mod-editor-wrap button:not(:disabled), .mod-editor-wrap input:not(:disabled), .mod-win-toolbar button:not(:disabled), .mod-win-min, .mod-win-close'
    ) || panel;
    target.focus({ preventScroll: true });
  } else if (want === 'toolbar-toggle') {
    const tbBtn = rootEl.querySelector('[data-focus-key="viewport-mod-toggle"]');
    tbBtn?.focus({ preventScroll: true });
  } else if (want === 'detach') {
    const detachBtn = rootEl.querySelector('[data-focus-key="mod-detach-btn"]')
      || rootEl.querySelector('[data-focus-key="viewport-mod-toggle"]');
    detachBtn?.focus({ preventScroll: true });
  }
}

function objectOptions(sources, includeSelfFor = null) {
  const items = [];
  if (includeSelfFor !== null && includeSelfFor < 32) {
    items.push([-1, 'Same object (self)']);
  }
  sources.forEach((s, i) => items.push([i, `${String(i + 1).padStart(2, '0')} ${s.name || `Source ${i + 1}`}`]));
  return items;
}

function sourceObjectOptions(sources) {
  return [
    ...sources.map((s, i) => [i, `${String(i + 1).padStart(2, '0')} ${s.name || `Source ${i + 1}`}`]),
    [32, 'Listener']
  ];
}

function shortObjectLabel(sources, obj) {
  if (obj === 32) return 'Listener';
  if (obj < 0) return 'Self';
  return `S${obj + 1}`;
}

function liveCurveX(curve, raw) {
  const inMin = curve?.inputRange?.[0] ?? 0;
  const inMax = curve?.inputRange?.[1] ?? 10;
  const span = inMax - inMin;
  let t = Math.abs(span) > 1e-12 ? (raw - inMin) / span : 0;
  if (!Number.isFinite(t)) t = 0;
  if (curve?.clamp !== false) t = Math.max(0, Math.min(1, t));
  if (curve?.invert) t = 1 - t;
  return Math.max(0, Math.min(1, t));
}

function resolveModContext(store) {
  const state = store.state;
  const sources = state.document.sources || [];
  const ns = sources.length;
  const creative = (state.parameters.values.mode ?? 0) >= 0.5;
  const routes = state.document.modulation?.routes || [];

  const sel = store.view.selection?.find(s => ['source', 'listener'].includes(s.kind));
  const selObj = sel ? (sel.kind === 'listener' ? 32 : sel.index) : null;
  const filterMode = store.view.modFilter || 'selected';

  const visibleEntries = routes
    .map((route, index) => ({ route, index }))
    .filter(({ route }) => filterMode === 'all' || selObj === null || routeMatchesObject(route, selObj));

  let activeIdx = store.view.modRoute;
  if (!visibleEntries.some(e => e.index === activeIdx)) {
    activeIdx = visibleEntries.length ? visibleEntries[0].index : (routes.length ? 0 : -1);
  }
  const activeRouteRaw = activeIdx >= 0 && routes[activeIdx] ? routes[activeIdx] : null;
  const activeRoute = activeRouteRaw ? normalizeRoute(activeRouteRaw, ns) : null;
  const nodeCount = activeRoute?.curve?.nodes?.length ?? 0;
  const activeNodeIdx = Number.isInteger(store.view.modNode) ? Math.max(0, Math.min(Math.max(0, nodeCount - 1), store.view.modNode)) : 0;

  return {
    state, sources, ns, creative, routes, selObj, filterMode,
    visibleEntries, activeIdx, activeRoute, activeNodeIdx,
    disabledEdit: !creative,
    fullBudget: routes.length >= MOD_MAX_ROUTES,
    targetDefaultObj: selObj !== null ? selObj : 0
  };
}

let flashModRoute = null;
function announceAddedRoute(routeObj, index) {
  const dm = driverMeta(routeObj.driver), tm = targetMeta(routeObj.target);
  const summary = `${dm.label} → ${tm.shortLabel || tm.label}`;
  flashModRoute = { index, until: performance.now() + 2000 };
  if (typeof window !== 'undefined') {
    window.dispatchEvent(new CustomEvent('verb-status-note', {
      detail: { text: `Added route: ${summary}`, durationMs: 4500 }
    }));
  }
}

function createModActions(store, m) {
  const { ns, creative, routes, selObj, activeIdx, fullBudget } = m;

  const addRoute = async (routeObj, { openEditor = false } = {}) => {
    if (!creative || fullBudget) return;
    const nextIdx = routes.length;
    const clean = normalizeRoute(routeObj, ns);
    const reply = await store.command({ op: 'modulation.addRoute', route: clean });
    if (reply?.kind === 'ack') {
      const opening = openEditor && !store.view.modEditorOpen;
      if (opening) requestModEditorFocus();
      announceAddedRoute(clean, nextIdx);
      store.setView({
        modRoute: nextIdx,
        modNode: 0,
        ...(openEditor ? { modEditorOpen: true } : {}),
        ...(selObj !== null && !routeMatchesObject(clean, selObj) ? { modFilter: 'all' } : {})
      });
    }
  };

  const applyRoutePatch = (idx, nextRoute, txId) => {
    const clean = normalizeRoute(nextRoute, ns);
    return txId
      ? store.command({ op: 'modulation.setRoute', index: idx, route: clean }, txId)
      : store.command({ op: 'modulation.setRoute', index: idx, route: clean });
  };

  const removeRoute = async index => {
    if (!creative) return;
    const reply = await store.command({ op: 'modulation.removeRoute', index });
    if (reply?.kind === 'ack') {
      const nextIdx = index === activeIdx
        ? Math.max(0, Math.min(routes.length - 2, index))
        : (activeIdx > index ? activeIdx - 1 : activeIdx);
      store.setView({ modRoute: nextIdx, modNode: 0 });
    }
  };

  const resolveTelemetryFor = rIdx => {
    const wire = store.telemetry['status.load']?.modulation?.routes?.[rIdx];
    const sim = computeModulationTelemetry(store.state, 0);
    const preview = sim.previews[rIdx] || { raw: 0, value: 0, active: false };
    if (wire && wire.active) return { raw: wire.raw, value: wire.value, active: true, preview: false };
    return { raw: preview.raw, value: preview.value, active: false, preview: creative };
  };

  return { addRoute, applyRoutePatch, removeRoute, resolveTelemetryFor };
}

function buildCreativeHint(creative) {
  if (creative) return null;
  return element('div', { className: 'mod-mode-banner', role: 'status' }, [
    element('strong', { text: 'Modulation works in Creative mode' }),
    element('span', { text: 'Switch Mode to Creative to edit or activate routes. Stored routes are preserved.' })
  ]);
}

function buildModToolbar({ store, m, actions, scope, register, showExpand = false, showDetach = false, budgetClass = 'mod-budget', className = 'mod-toolbar' }) {
  const { routes, selObj, filterMode, creative, disabledEdit, fullBudget, targetDefaultObj, ns } = m;
  const { addRoute } = actions;

  const addBtn = button('+ Add route', () => addRoute(createDefaultRoute(targetDefaultObj, null, ns)), {
    className: 'mod-add-btn primary',
    disabled: disabledEdit || fullBudget,
    'aria-label': 'Add modulation route',
    'data-focus-key': `${scope}-mod-add`,
    title: !creative ? 'Switch to Creative mode to add modulation routes' : fullBudget ? 'Maximum 64 modulation routes reached' : 'Add a new modulation route'
  });

  const presetSelect = register(select('Add from preset', MOD_PRESETS.map(p => [p.id, p.label]), '', presetId => {
    const p = MOD_PRESETS.find(item => item.id === presetId);
    if (!p) return;
    const built = p.build(targetDefaultObj === 32 && p.id !== 'speed-hshelf' ? 0 : targetDefaultObj);
    if (built.object >= ns && built.object !== 32) built.object = 0;
    addRoute(built);
  }, {
    reselect: true,
    placeholder: 'Add from preset…',
    disabled: disabledEdit || fullBudget,
    className: 'dd mod-preset-dd',
    'data-focus-key': `${scope}-mod-preset`,
    title: !creative ? 'Switch to Creative mode to add a route from a preset' : 'Insert a new modulation route from a starter preset'
  }));

  const filterLabelSel = selObj === null ? 'All objects' : selObj === 32 ? 'Listener + Global' : `S${selObj + 1} + Global`;
  const filterGroup = element('div', { className: 'mod-filter-seg', role: 'group', 'aria-label': 'Route list filter' }, [
    button(filterLabelSel, () => store.setView({ modFilter: 'selected' }), {
      className: filterMode === 'selected' ? 'active' : '',
      'aria-pressed': String(filterMode === 'selected'),
      'data-focus-key': `${scope}-mod-filter-sel`,
      title: 'Show routes for the selected object plus global routes'
    }),
    button(`All (${routes.length})`, () => store.setView({ modFilter: 'all' }), {
      className: filterMode === 'all' ? 'active' : '',
      'aria-pressed': String(filterMode === 'all'),
      'data-focus-key': `${scope}-mod-filter-all`,
      title: 'Show all routes in the project'
    })
  ]);

  const budgetNode = element('span', {
    className: `${budgetClass}${fullBudget ? ' warning' : ''}`,
    text: `${routes.length} / ${MOD_MAX_ROUTES} routes`,
    'aria-label': `Modulation routes budget ${routes.length} of ${MOD_MAX_ROUTES}`
  });

  const isOpen = !!store.view.modEditorOpen;
  const showDetachControl = showDetach || showExpand;
  const detachBtn = showDetachControl ? button(isOpen ? 'Dock ↙' : 'Detach ↗', () => {
    if (!store.view.modEditorOpen) {
      requestModEditorFocus('detach');
      store.setView({ modEditorOpen: true });
    } else {
      requestModReturnFocus('detach');
      store.setView({ modEditorOpen: false });
    }
  }, {
    className: `mod-detach-btn${isOpen ? ' active' : ''}`,
    'aria-label': isOpen ? 'Dock modulation editor back into Modulation tab' : 'Detach modulation editor into floating window',
    'aria-expanded': String(isOpen),
    'aria-pressed': String(isOpen),
    'data-focus-key': 'mod-detach-btn',
    title: isOpen ? 'Dock modulation editor back into Modulation tab' : 'Detach modulation editor into floating viewport window'
  }) : null;

  return element('div', { className }, [
    addBtn,
    presetSelect,
    filterGroup,
    element('span', { className: 'mod-toolbar-spacer' }),
    budgetNode,
    ...(detachBtn ? [detachBtn] : [])
  ]);
}

function buildRouteList({ store, m, actions, scope, register, viewClass = 'mod-list-view', rootClass = 'mod-list-col' }) {
  const { sources, creative, routes, selObj, visibleEntries, activeIdx, disabledEdit, fullBudget } = m;
  const { addRoute, applyRoutePatch, removeRoute, resolveTelemetryFor } = actions;

  const listStack = element('div', { className: 'mod-list-items' });
  const listRowRefs = new Map();

  if (!visibleEntries.length) {
    const emptyText = routes.length
      ? `No routes match ${selObj === 32 ? 'Listener' : `Source ${selObj + 1}`}. Use All (${routes.length}) or + Add route.`
      : 'No modulation routes yet. Click + Add route or pick a starter preset.';
    listStack.append(element('p', { className: 'note mod-empty-note', text: emptyText }));
  } else {
    const lastHostOwner = new Map();
    routes.forEach((rt, i) => {
      if (rt?.enabled && (rt.hostOut || 0) > 0) lastHostOwner.set(rt.hostOut, i);
    });
    visibleEntries.forEach(({ route, index }) => {
      const dm = driverMeta(route.driver), tm = targetMeta(route.target);
      const hostSlot = route.hostOut || 0;
      const srcName = shortObjectLabel(sources, route.object);
      const tgtPrefix = isGlobalTarget(route.target)
        ? (tm.hostOnly ? srcName : 'Global')
        : route.targetObject >= 0 && route.targetObject !== route.object
          ? `${shortObjectLabel(sources, route.targetObject)}`
          : srcName;
      const targetTitleLabel = tm.hostOnly && hostSlot > 0 ? `Mod Out ${hostSlot}` : (tm.shortLabel || tm.label);
      const driverTargetTitle = `${dm.label} → ${targetTitleLabel}`;
      const scopeLabel = tgtPrefix === srcName ? srcName : `${srcName} → ${tgtPrefix}`;
      const stateLabel = !route.enabled ? 'Off' : creative ? 'Active' : 'Bypassed';
      const isSelected = index === activeIdx;
      const isHostOverridden = hostSlot > 0 && route.enabled && lastHostOwner.has(hostSlot) && lastHostOwner.get(hostSlot) !== index;
      const hostTagEl = (!tm.hostOnly && hostSlot > 0)
        ? element('span', {
            className: `mod-row-host-tag${isHostOverridden ? ' dup' : ''}`,
            text: `→ MO ${hostSlot}`,
            title: isHostOverridden
              ? `Mod Out ${hostSlot} (overridden by a later active route in the list)`
              : `Also drives host parameter Mod Out ${hostSlot}`
          })
        : null;

      const enSwitch = check('', !!route.enabled, enabled => {
        applyRoutePatch(index, { ...route, enabled });
      }, {
        className: 'check mod-row-enable',
        disabled: disabledEdit,
        'aria-label': `Enable route ${index + 1}: ${driverTargetTitle} (${scopeLabel})`,
        'data-focus-key': `${scope}-mod-en-${index}`,
        title: disabledEdit ? 'Creative mode only' : `Toggle route ${index + 1}`
      });
      enSwitch.addEventListener('click', e => e.stopPropagation());
      enSwitch.addEventListener('dblclick', e => e.stopPropagation());

      const readoutEl = element('span', { className: 'mod-row-readout' });
      const stateEl = element('span', {
        className: `mod-row-state ${route.enabled && creative ? 'on' : 'off'}`,
        text: stateLabel,
        'data-route-state': stateLabel.toLowerCase(),
        title: !route.enabled ? 'Route disabled' : creative ? 'Route active in Creative mode' : 'Bypassed in Physical mode'
      });
      const initT = resolveTelemetryFor(index);
      const formatRowTooltip = t => `${dm.label} → ${tm.label} (${scopeLabel}) · ${stateLabel} · ${formatModValue(t.raw, dm.unit)} → ${formatModValue(t.value, tm.unit, tm.isFreq)}`;
      readoutEl.textContent = formatModValue(initT.value, tm.unit, tm.isFreq);
      readoutEl.title = `${tm.label}: ${formatModValue(initT.value, tm.unit, tm.isFreq)} (driver ${formatModValue(initT.raw, dm.unit)})`;
      listRowRefs.set(index, { route, dm, tm, readoutEl, stateEl, formatRowTooltip });

      const dupBtn = button('⧉', async e => {
        e.stopPropagation();
        await addRoute({ ...route });
      }, {
        className: 'mod-row-icon',
        disabled: disabledEdit || fullBudget,
        'aria-label': `Duplicate route ${index + 1}`,
        'data-focus-key': `${scope}-mod-dup-${index}`,
        title: fullBudget ? 'Route limit reached' : `Duplicate route ${index + 1}`
      });
      dupBtn.addEventListener('dblclick', e => e.stopPropagation());

      const delBtn = button('×', async e => {
        e.stopPropagation();
        await removeRoute(index);
      }, {
        className: 'mod-row-icon mod-row-del',
        disabled: disabledEdit,
        'aria-label': `Delete route ${index + 1}`,
        'data-focus-key': `${scope}-mod-del-${index}`,
        title: `Delete route ${index + 1}`
      });
      delBtn.addEventListener('dblclick', e => e.stopPropagation());

      const rowTooltip = formatRowTooltip(initT);
      const isFlashed = !!flashModRoute && flashModRoute.index === index && performance.now() < flashModRoute.until;
      const rowCard = element('div', {
        className: `mod-route-item${isSelected ? ' selected' : ''}${isFlashed ? ' just-added' : ''}${route.enabled && creative ? '' : ' muted'}`,
        'data-route-index': String(index),
        'data-focus-key': `${scope}-mod-row-${index}`,
        ...(isFlashed ? { 'data-just-added': 'true' } : {}),
        tabIndex: 0,
        role: 'button',
        'aria-pressed': String(isSelected),
        'aria-label': `Select route ${index + 1}: ${driverTargetTitle} (${scopeLabel}, ${stateLabel})`,
        title: rowTooltip
      }, [
        element('div', { className: 'mod-row-top' }, [
          enSwitch,
          element('div', { className: 'mod-row-title', text: driverTargetTitle, title: rowTooltip }),
          element('div', { className: 'mod-row-actions' }, [dupBtn, delBtn])
        ]),
        element('div', { className: 'mod-row-meta' }, [
          element('span', { className: 'mod-row-scope', text: scopeLabel, title: `Source → target: ${scopeLabel}` }),
          stateEl,
          ...(hostTagEl ? [hostTagEl] : []),
          readoutEl
        ])
      ]);
      if (isFlashed) {
        const remainMs = Math.max(50, Math.round(flashModRoute.until - performance.now()));
        const clearTimer = setTimeout(() => {
          rowCard.classList.remove('just-added');
          rowCard.removeAttribute('data-just-added');
          if (flashModRoute && performance.now() >= flashModRoute.until) flashModRoute = null;
        }, remainMs);
        register?.({ dispose: () => clearTimeout(clearTimer) });
        requestAnimationFrame(() => { if (rowCard.isConnected) rowCard.scrollIntoView({ block: 'nearest' }); });
      }

      const selectThis = () => store.setView({ modRoute: index, modNode: 0 });
      rowCard.addEventListener('click', e => {
        if (e.target.closest('button')) return;
        selectThis();
      });
      rowCard.addEventListener('dblclick', e => {
        if (e.target.closest('button')) return;
        selectThis();
      });
      rowCard.addEventListener('keydown', e => {
        if ((e.key === 'Enter' || e.key === ' ') && e.target === rowCard) {
          e.preventDefault();
          selectThis();
        }
      });

      listStack.append(rowCard);
    });
  }

  const listCol = register(scrollArea(listStack, { className: viewClass, rootClass }));
  const updateListReadouts = () => {
    if (!store.state) return;
    for (const [idx, ref] of listRowRefs) {
      const t = resolveTelemetryFor(idx);
      const valText = formatModValue(t.value, ref.tm.unit, ref.tm.isFreq);
      ref.readoutEl.textContent = valText;
      ref.readoutEl.title = `${ref.tm.label}: ${valText} (driver ${formatModValue(t.raw, ref.dm.unit)})`;
    }
  };
  return { listCol, updateListReadouts };
}

function buildRouteEditor({ store, m, actions, register, scope = 'win' }) {
  const { sources, ns, routes, activeIdx, activeRoute, activeNodeIdx: initNodeIdx, disabledEdit } = m;
  const { applyRoutePatch } = actions;

  const editorWrap = element('div', { className: 'mod-editor-wrap' });
  let liveUpdater = null;
  let svgResizeObs = null;

  if (!activeRoute || activeIdx < 0) {
    editorWrap.append(element('div', {
      className: 'mod-editor-empty note',
      text: 'Select or add a modulation route to edit its driver, target and shaping curve.'
    }));
    return { editorWrap, liveUpdater, dispose: () => {} };
  }

  let activeNodeIdx = initNodeIdx;
  const r = activeRoute;
  const dm = driverMeta(r.driver);
  const tm = targetMeta(r.target);
  let draftCurve = structuredClone(r.curve);

  const objSelect = register(select('Driver object', sourceObjectOptions(sources), r.object, val => {
    const nextObj = Number(val);
    const next = { ...r, object: nextObj };
    if (!isGlobalTarget(next.target) && nextObj === 32 && next.targetObject < 0) next.targetObject = 0;
    applyRoutePatch(activeIdx, next);
  }, { disabled: disabledEdit, 'data-focus-key': `${scope}-mod-src` }));

  const drvSelect = register(select('Modulation driver', MOD_DRIVERS.map(d => [d.id, d.label]), r.driver, val => {
    const nextDm = driverMeta(val);
    const nextRoute = {
      ...r,
      driver: val,
      ...(val === 'distanceToPoint' ? { point: r.point || [0, 0, 0] } : {}),
      ...(nextDm.hasZone ? { zoneId: r.zoneId || zoneList(store.state)[0]?.id || 1 } : {}),
      curve: {
        ...r.curve,
        inputRange: [...nextDm.defaultIn]
      }
    };
    applyRoutePatch(activeIdx, nextRoute);
  }, { disabled: disabledEdit, title: dm.desc, 'data-focus-key': `${scope}-mod-drv` }));

  const tgtSelect = register(select('Modulation target', MOD_TARGETS.map(t => [t.id, t.label]), r.target, val => {
    const nextTm = targetMeta(val);
    let tgtObj = r.targetObject;
    if (nextTm.global) tgtObj = -1;
    else if (r.object === 32 && tgtObj < 0) tgtObj = 0;
    store.setView({ modNode: 0 }, false);
    applyRoutePatch(activeIdx, {
      ...r,
      target: val,
      targetObject: tgtObj,
      curve: {
        ...r.curve,
        outputRange: [...nextTm.defaultOut],
        invert: false,
        clamp: true,
        nodes: [{ x: 0, y: 0, smooth: false }, { x: 1, y: 1, smooth: false }]
      }
    });
  }, { disabled: disabledEdit, 'data-focus-key': `${scope}-mod-tgt` }));

  const tgtObjOpts = objectOptions(sources, r.object);
  const tgtObjSelect = !tm.global ? register(select('Target object', tgtObjOpts, r.targetObject, val => {
    applyRoutePatch(activeIdx, { ...r, targetObject: Number(val) });
  }, { disabled: disabledEdit, 'data-focus-key': `${scope}-mod-tgtobj` })) : null;

  const enableToggle = check('Active', !!r.enabled, enabled => applyRoutePatch(activeIdx, { ...r, enabled }), {
    disabled: disabledEdit,
    'aria-label': 'Route enabled',
    'data-focus-key': `${scope}-mod-active`,
    title: 'Enable or bypass this modulation route'
  });

  const hostOutVal = r.hostOut || 0;
  const hostOutOpts = [['0', '—'], ...Array.from({ length: MOD_MAX_HOST_OUTS }, (_, i) => [String(i + 1), String(i + 1)])];
  const hostOutTitle = hostOutVal > 0
    ? `Send smoothed curve (0–1) to host output parameter Mod Out ${hostOutVal} (REAPER: Parameter modulation → Link from FX parameter)`
    : 'Send smoothed curve (0–1) to a read-only host output parameter Mod Out 1–16 (REAPER: Parameter modulation → Link from FX parameter)';
  const hostOutSelect = register(select('Host output slot', hostOutOpts, String(hostOutVal), val => {
    applyRoutePatch(activeIdx, { ...r, hostOut: Number(val) });
  }, {
    disabled: disabledEdit,
    title: hostOutTitle,
    'data-focus-key': `${scope}-mod-hostout`
  }));
  const hostOutLiveEl = element('span', {
    className: 'mod-host-val',
    'aria-label': 'Host output live value',
    text: hostOutVal > 0 ? '0.50' : '—',
    ...(hostOutVal > 0 ? {} : { hidden: true })
  });

  const attackField = register(txNumericField({
    store,
    label: 'Attack, ms',
    txLabel: 'Edit route attack',
    value: r.attackMs,
    min: 0,
    max: 2000,
    step: 1,
    disabled: disabledEdit,
    display: v => `${Math.round(v)}`,
    getCommitCmd: attackMs => ({ op: 'modulation.setRoute', index: activeIdx, route: normalizeRoute({ ...r, attackMs }, ns) })
  }));

  const releaseField = register(txNumericField({
    store,
    label: 'Release, ms',
    txLabel: 'Edit route release',
    value: r.releaseMs,
    min: 0,
    max: 5000,
    step: 1,
    disabled: disabledEdit,
    display: v => `${Math.round(v)}`,
    getCommitCmd: releaseMs => ({ op: 'modulation.setRoute', index: activeIdx, route: normalizeRoute({ ...r, releaseMs }, ns) })
  }));

  // V04e: in the compact dock a route without MIDI shows only a small "MIDI" mode select next to Host out.
  const midiTailSlot = element('div', { className: 'mod-field mod-field-midi', hidden: true });
  const metaGrid = element('div', { className: 'mod-meta-grid' }, [
    element('div', { className: 'mod-field' }, [element('span', { className: 'mod-lbl', text: 'Source' }), objSelect]),
    element('div', { className: 'mod-field mod-field-grow' }, [element('span', { className: 'mod-lbl', text: 'Driver' }), drvSelect]),
    element('div', { className: 'mod-field mod-field-grow' }, [element('span', { className: 'mod-lbl', text: 'Target' }), tgtSelect]),
    ...(tgtObjSelect ? [element('div', { className: 'mod-field' }, [element('span', { className: 'mod-lbl', text: 'Apply to' }), tgtObjSelect])] : []),
    element('div', { className: 'mod-meta-tail' }, [
      element('div', { className: 'mod-field mod-field-hostout', title: hostOutTitle }, [element('span', { className: 'mod-lbl', text: 'Host out' }), hostOutSelect, hostOutLiveEl]),
      midiTailSlot,
      element('div', { className: 'mod-field mod-field-ms' }, [element('span', { className: 'mod-lbl', text: scope === 'dock' ? 'Atk' : 'Atk ms', title: 'Attack smoothing (0..2000 ms)' }), attackField]),
      element('div', { className: 'mod-field mod-field-ms' }, [element('span', { className: 'mod-lbl', text: scope === 'dock' ? 'Rel' : 'Rel ms', title: 'Release smoothing (0..5000 ms)' }), releaseField]),
      enableToggle
    ])
  ]);

  let pointRow = null;
  if (r.driver === 'distanceToPoint') {
    const pt = r.point || [0, 0, 0];
    const ptField = (axis, idx, lo, hi) => register(txNumericField({
      store,
      label: `Reference point ${axis}, m`,
      txLabel: 'Edit reference point',
      value: pt[idx],
      min: lo,
      max: hi,
      step: 0.1,
      disabled: disabledEdit,
      getCommitCmd: val => {
        const nextPt = [...(r.point || [0, 0, 0])];
        nextPt[idx] = val;
        return { op: 'modulation.setRoute', index: activeIdx, route: normalizeRoute({ ...r, point: nextPt }, ns) };
      }
    }));
    pointRow = element('div', { className: 'mod-point-row' }, [
      element('span', { className: 'mod-lbl', text: 'Point (m)' }),
      element('label', { className: 'mod-xyz-item' }, [element('span', { text: 'X' }), ptField('X', 0, -200, 200)]),
      element('label', { className: 'mod-xyz-item' }, [element('span', { text: 'Y' }), ptField('Y', 1, -200, 200)]),
      element('label', { className: 'mod-xyz-item' }, [element('span', { text: 'Z' }), ptField('Z', 2, 0, 30)])
    ]);
  }

  // V04c: zone picker for In zone / Distance to zone (Zone index needs none) + live inside/outside readout.
  let zonePickRow = null, zoneLiveEl = null;
  if (dm.hasZone || r.driver === 'zoneIndex') {
    const zl = zoneList(store.state);
    zoneLiveEl = element('span', { className: 'mod-zone-live', 'aria-label': 'Zone occupancy', text: '' });
    const kids = [element('span', { className: 'mod-lbl', text: dm.hasZone ? 'Zone' : 'Zones' })];
    if (dm.hasZone) {
      const opts = zl.map((z, i) => [String(z.id), `${i + 1}. ${zoneLabel(z)}`]);
      if (!zl.some(z => z.id === r.zoneId)) opts.push([String(r.zoneId || 0), `Missing zone #${r.zoneId || 0} (reads outside)`]);
      kids.push(register(select('Zone for driver', opts, String(r.zoneId || 0), val => applyRoutePatch(activeIdx, { ...r, zoneId: Number(val) }), {
        disabled: disabledEdit || !zl.length, 'data-focus-key': `${scope}-mod-zone`, title: 'Trigger zone this route reads (Edit → Scene → Zones)'
      })));
    } else kids.push(element('span', { className: 'mod-zone-order', text: zl.length ? zl.map((z, i) => `${i + 1} ${zoneLabel(z)}`).join(' · ') : '—' }));
    if (!zl.length) kids.push(element('span', { className: 'mod-zone-none', text: 'No zones yet: draw one in Edit → Scene → Zones.' }));
    kids.push(zoneLiveEl);
    zonePickRow = element('div', { className: 'mod-zone-row' }, kids);
  }

  // V04e: MIDI out section (optional; mode Off removes the block). Selects/checkboxes = one command each; numeric
  // fields are transactional gestures (one undo step per drag / edit).
  const midi = normalizeMidiOut(r.midi);
  const midiMode = midi?.mode || 'off';
  const midiCommit = patch => ({ op: 'modulation.setRoute', index: activeIdx, route: normalizeRoute({ ...r, midi: { ...(midi || {}), ...patch } }, ns) });
  const midiPatch = patch => applyRoutePatch(activeIdx, { ...r, midi: { ...(midi || {}), ...patch } });
  const midiModeSelect = register(select('MIDI out mode', MIDI_MODES.map(m => [m.id, m.label]), midiMode, val => {
    if (val === 'off') { const { midi: _drop, ...rest } = r; applyRoutePatch(activeIdx, rest); return; }
    const base = midi || { channel: 1, velocity: 100, threshold: 0.5, hysteresis: 0.05, lengthMs: 100, ccLo: 0, ccHi: 127 };
    const number = midi && midi.mode !== 'cc' && val !== 'cc' ? midi.number : (val === 'cc' ? 1 : val === 'index' ? 36 : 60);
    applyRoutePatch(activeIdx, { ...r, midi: { ...base, mode: val, number } });
  }, { disabled: disabledEdit, 'data-focus-key': `${scope}-mod-midi-mode`, title: (MIDI_MODES.find(m => m.id === midiMode)?.desc) || 'Send MIDI notes or CC to the host (Creative only, while the transport plays)' }));
  const midiLiveEl = element('span', { className: 'mod-midi-live', 'aria-label': 'Last MIDI message sent', 'aria-live': 'off', text: midi ? 'no message yet' : '' });
  const midiNum = (label, key, lo, hi, step, display, extra = {}) => register(txNumericField({
    store, label, txLabel: 'Edit route MIDI out', value: midi?.[key] ?? lo, min: lo, max: hi, step, disabled: disabledEdit,
    display: display || (v => `${Math.round(v)}`), getCommitCmd: v => midiCommit({ [key]: step < 1 ? v : Math.round(v) }), ...extra
  }));
  let midiRow = null;
  {
    const kids = [element('span', { className: 'mod-lbl', text: 'MIDI out' }), midiModeSelect];
    if (midi) {
      kids.push(element('span', { className: 'mod-midi-item' }, [element('span', { className: 'mod-lbl', text: 'Ch' }),
        register(select('MIDI channel', Array.from({ length: 16 }, (_, i) => [String(i + 1), String(i + 1)]), String(midi.channel), val => midiPatch({ channel: Number(val) }), { disabled: disabledEdit, 'data-focus-key': `${scope}-mod-midi-ch` }))]));
      const isCc = midi.mode === 'cc';
      kids.push(element('span', { className: 'mod-midi-item' }, [
        element('span', { className: 'mod-lbl', text: isCc ? 'CC' : midi.mode === 'index' ? 'Base' : 'Note' }),
        midiNum(isCc ? 'MIDI CC number' : midi.mode === 'index' ? 'Keyswitch base note' : 'MIDI note', 'number', 0, 127, 1),
        ...(isCc ? [] : [element('span', { className: 'mod-midi-note', text: midiNoteName(midi.number), title: 'Note name (middle C = C3 = 60)' })])
      ]));
      if (midi.mode === 'gate' || midi.mode === 'trigger') {
        kids.push(element('span', { className: 'mod-midi-item' }, [element('span', { className: 'mod-lbl', text: 'Thr', title: 'Threshold of the route output (0–1)' }), midiNum('MIDI threshold', 'threshold', 0, 1, 0.01, v => v.toFixed(2))]));
        if (midi.mode === 'gate') kids.push(element('span', { className: 'mod-midi-item' }, [element('span', { className: 'mod-lbl', text: 'Hyst', title: 'Note-off only below threshold − hysteresis (no chatter)' }), midiNum('MIDI hysteresis', 'hysteresis', 0, 0.5, 0.01, v => v.toFixed(2))]));
        else kids.push(element('span', { className: 'mod-midi-item' }, [element('span', { className: 'mod-lbl', text: 'Len ms' }), midiNum('MIDI note length, ms', 'lengthMs', 1, 5000, 1)]));
      }
      if (midi.mode !== 'cc') {
        kids.push(element('span', { className: 'mod-midi-item' }, [element('span', { className: 'mod-lbl', text: 'Vel' }),
          midiNum('MIDI velocity', 'velocity', 1, 127, 1, null, { disabled: disabledEdit || midi.velocityFromValue }),
          check('= value', !!midi.velocityFromValue, on => midiPatch({ velocityFromValue: on }), { disabled: disabledEdit, 'aria-label': 'Velocity from route value', title: 'Velocity from the route output (0–1 → 1–127)' })]));
      } else {
        kids.push(element('span', { className: 'mod-midi-item' }, [element('span', { className: 'mod-lbl', text: 'Range' }), midiNum('CC value at output 0', 'ccLo', 0, 127, 1), element('span', { text: '…' }), midiNum('CC value at output 1', 'ccHi', 0, 127, 1)]));
      }
      if (midi.mode === 'index') kids.push(check('Program change', !!midi.program, on => midiPatch({ program: on }), { disabled: disabledEdit, 'aria-label': 'Also send program change', title: 'Also send a program change (program = index) on every index change' }));
      kids.push(midiLiveEl);
    } else kids.push(element('span', { className: 'mod-midi-hint', text: 'Notes / CC to the next FX or instrument (Creative, while playing)' }));
    if (scope === 'dock' && !midi) {
      // Compact dock: a small MIDI mode select next to Host out. Picking a mode adds the MIDI block and the full
      // MIDI out row then appears in the dock itself; the editor never leaves the dock (Detach has its own button).
      midiTailSlot.hidden = false;
      midiTailSlot.dataset.midiMode = 'off';
      // The closed select reads "MIDI" (no extra label), so the tail row keeps its height at 1100x560.
      midiTailSlot.append(register(select('MIDI out mode', MIDI_MODES.map(m => [m.id, m.id === 'off' ? 'MIDI' : m.label]), 'off', val => {
        if (val === 'off') return;
        const number = val === 'cc' ? 1 : val === 'index' ? 36 : 60;
        applyRoutePatch(activeIdx, { ...r, midi: { channel: 1, velocity: 100, threshold: 0.5, hysteresis: 0.05, lengthMs: 100, ccLo: 0, ccHi: 127, mode: val, number } });
      }, { disabled: disabledEdit, 'data-focus-key': `${scope}-mod-midi-mode`, title: 'MIDI out (notes / CC) for this route: pick a mode; the settings appear below (Creative only, while the transport plays)' })));
    } else midiRow = element('div', { className: `mod-midi-row${midi ? ' on' : ''}`, 'data-midi-mode': midiMode }, kids);
  }

  // ── Large Responsive Curve SVG Editor (1 SVG unit = 1 CSS pixel via ResizeObserver) ──
  let PL = scope === 'dock' ? 46 : 52;
  const PR = 12, PT = 12, PB = 20;
  let W = scope === 'dock' ? 360 : 420, H = scope === 'dock' ? 128 : 190;
  let PW = W - PL - PR, PH = H - PT - PB;
  const toSvgX = u => PL + Math.max(0, Math.min(1, u)) * PW;
  const toSvgY = v => PT + (1 - Math.max(0, Math.min(1, v))) * PH;
  const fromSvgX = x => Math.max(0, Math.min(1, (x - PL) / Math.max(1, PW)));
  const fromSvgY = y => Math.max(0, Math.min(1, 1 - (y - PT) / Math.max(1, PH)));

  const plotBg = svgEl('rect', { className: 'mod-plot-bg', x: PL, y: PT, width: PW, height: PH, rx: 2 });
  const curvePathEl = svgEl('path', { className: 'mod-curve-line', fill: 'none' });
  const curveFillEl = svgEl('path', { className: 'mod-curve-area', stroke: 'none' });
  const liveCrossV = svgEl('line', { className: 'mod-live-cross', x1: PL, y1: PT, x2: PL, y2: PT + PH });
  const liveCrossH = svgEl('line', { className: 'mod-live-cross', x1: PL, y1: PT + PH, x2: PL + PW, y2: PT + PH });
  const liveDotEl = svgEl('circle', { className: 'mod-live-dot', r: 4.8, cx: PL, cy: PT + PH });
  const nodesGroup = svgEl('g', { className: 'mod-curve-nodes' });

  const leftAxisMax = svgEl('text', { className: 'mod-axis-lbl', x: PL - 6, y: PT + 9, 'text-anchor': 'end' });
  const leftAxisMin = svgEl('text', { className: 'mod-axis-lbl', x: PL - 6, y: PT + PH, 'text-anchor': 'end' });
  const botAxisLeft = svgEl('text', { className: 'mod-axis-lbl', x: PL, y: H - 5, 'text-anchor': 'start' });
  const botAxisRight = svgEl('text', { className: 'mod-axis-lbl', x: PL + PW, y: H - 5, 'text-anchor': 'end' });

  const gridFracs = [0, 0.25, 0.5, 0.75, 1];
  const gridV = gridFracs.map(f => ({
    f,
    el: svgEl('line', { className: f === 0 || f === 1 ? 'mod-grid-edge' : 'mod-grid-line', x1: toSvgX(f), y1: PT, x2: toSvgX(f), y2: PT + PH })
  }));
  const gridH = gridFracs.map(f => ({
    f,
    el: svgEl('line', { className: f === 0 || f === 1 ? 'mod-grid-edge' : 'mod-grid-line', x1: PL, y1: toSvgY(f), x2: PL + PW, y2: toSvgY(f) })
  }));

  const svg = svgEl('svg', {
    className: 'mod-curve-svg',
    viewBox: `0 0 ${W} ${H}`,
    'aria-label': `Modulation transfer curve with ${draftCurve.nodes.length} nodes`
  }, [
    plotBg,
    ...gridV.map(g => g.el),
    ...gridH.map(g => g.el),
    curveFillEl,
    liveCrossV,
    liveCrossH,
    curvePathEl,
    liveDotEl,
    nodesGroup,
    leftAxisMax,
    leftAxisMin,
    botAxisLeft,
    botAxisRight
  ]);

  const updateAxisLabels = () => {
    leftAxisMax.textContent = tm.hostOnly ? '1' : formatModValue(draftCurve.outputRange[1], '', tm.isFreq);
    leftAxisMin.textContent = tm.hostOnly ? '0' : formatModValue(draftCurve.outputRange[0], '', tm.isFreq);
    const x0 = draftCurve.invert ? draftCurve.inputRange[1] : draftCurve.inputRange[0];
    const x1 = draftCurve.invert ? draftCurve.inputRange[0] : draftCurve.inputRange[1];
    botAxisLeft.textContent = formatModValue(x0, dm.unit);
    botAxisRight.textContent = formatModValue(x1, dm.unit);
  };

  const sampleCurveEval = xCurve => {
    const nodes = draftCurve.nodes;
    const last = nodes.length - 1;
    if (xCurve <= nodes[0].x) return nodes[0].y;
    if (xCurve >= nodes[last].x) return nodes[last].y;
    for (let i = 1; i < nodes.length; i++) {
      if (xCurve <= nodes[i].x) {
        const a = nodes[i - 1], b = nodes[i], dx = b.x - a.x;
        if (dx <= 1e-12) return b.y;
        const s = Math.max(0, Math.min(1, (xCurve - a.x) / dx));
        const shaped = a.smooth ? (s * s * (3 - 2 * s)) : s;
        return a.y + (b.y - a.y) * shaped;
      }
    }
    return nodes[last].y;
  };

  const paintCurveGeometry = () => {
    updateAxisLabels();
    const samples = 80;
    const pts = [];
    for (let i = 0; i <= samples; i++) {
      const u = i / samples;
      pts.push([toSvgX(u).toFixed(1), toSvgY(sampleCurveEval(u)).toFixed(1)]);
    }
    curvePathEl.setAttribute('d', pts.map(([x, y], i) => `${i === 0 ? 'M' : 'L'}${x} ${y}`).join(' '));
    curveFillEl.setAttribute('d', `M${toSvgX(0).toFixed(1)} ${(PT + PH).toFixed(1)} ` + pts.map(([x, y]) => `L${x} ${y}`).join(' ') + ` L${toSvgX(1).toFixed(1)} ${(PT + PH).toFixed(1)} Z`);
  };

  const positionNodes = () => {
    nodesGroup.querySelectorAll('.mod-node-g').forEach((g, ni) => {
      const nd = draftCurve.nodes[ni];
      if (!nd) return;
      const cx = toSvgX(nd.x), cy = toSvgY(nd.y);
      g.querySelector('.mod-node-hit')?.setAttribute('cx', String(cx));
      g.querySelector('.mod-node-hit')?.setAttribute('cy', String(cy));
      g.querySelector('.mod-curve-node')?.setAttribute('cx', String(cx));
      g.querySelector('.mod-curve-node')?.setAttribute('cy', String(cy));
    });
  };

  let nodeXInput = null, nodeYInput = null, nodeSmoothBtn = null, addNodeBtn = null, delNodeBtn = null, nodeHeadEl = null;

  const commitCurveNow = (nextCurve, selectNode = activeNodeIdx) => {
    if (disabledEdit) return Promise.resolve();
    store.setView({ modNode: selectNode }, false);
    return applyRoutePatch(activeIdx, { ...r, curve: nextCurve });
  };

  const deleteNodeAt = idx => {
    if (disabledEdit || draftCurve.nodes.length <= MOD_MIN_NODES) return;
    const nodes = draftCurve.nodes.filter((_, i) => i !== idx);
    const nextSel = Math.max(0, Math.min(nodes.length - 1, idx - 1));
    commitCurveNow({ ...draftCurve, nodes }, nextSel);
  };

  const addNodeAt = (nx, ny) => {
    if (disabledEdit || draftCurve.nodes.length >= MOD_MAX_NODES) return;
    const nodes = [...draftCurve.nodes, { x: Number(nx.toFixed(4)), y: Number(ny.toFixed(4)), smooth: false }]
      .sort((a, b) => a.x - b.x);
    const addedIdx = nodes.findIndex(n => Math.abs(n.x - nx) < 5e-4 && Math.abs(n.y - ny) < 5e-4);
    commitCurveNow({ ...draftCurve, nodes }, addedIdx >= 0 ? addedIdx : 0);
  };

  const insertMidNode = () => {
    if (disabledEdit || draftCurve.nodes.length >= MOD_MAX_NODES) return;
    const nsList = draftCurve.nodes;
    let bestI = 0, bestSpan = -1;
    for (let i = 0; i < nsList.length - 1; i++) {
      const sp = nsList[i + 1].x - nsList[i].x;
      if (sp > bestSpan) { bestSpan = sp; bestI = i; }
    }
    const nx = (nsList[bestI].x + nsList[bestI + 1].x) * 0.5;
    const ny = sampleCurveEval(nx);
    addNodeAt(nx, ny);
  };

  const renderNodes = () => {
    nodesGroup.replaceChildren();
    draftCurve.nodes.forEach((nd, i) => {
      const cx = toSvgX(nd.x), cy = toSvgY(nd.y);
      const isSel = i === activeNodeIdx;
      const g = svgEl('g', { class: `mod-node-g${isSel ? ' sel' : ''}${nd.smooth ? ' smooth' : ''}`, 'data-node-index': String(i) });
      const hit = svgEl('circle', { class: 'mod-node-hit', cx, cy, r: 12 });
      const vis = svgEl('circle', {
        class: `mod-curve-node${isSel ? ' sel' : ''}`,
        cx, cy, r: isSel ? 5.8 : 4.5,
        tabindex: disabledEdit ? -1 : 0,
        role: 'slider',
        'aria-label': `Curve node ${i + 1} of ${draftCurve.nodes.length}`,
        'aria-valuemin': 0,
        'aria-valuemax': 1,
        'aria-valuenow': Number(nd.y.toFixed(3)),
        'data-focus-key': `mod-node-${activeIdx}-${i}`
      });

      g.append(hit, vis);
      g.addEventListener('pointerdown', e => {
        if (disabledEdit || e.button !== 0 || !store.ready) return;
        e.preventDefault();
        e.stopPropagation();
        svg.setAttribute('data-gesture-active', 'true');
        activeNodeIdx = i;
        store.setView({ modNode: i }, false);
        nodesGroup.querySelectorAll('.mod-node-g').forEach((el, ni) => {
          const on = ni === i;
          el.classList.toggle('sel', on);
          const c = el.querySelector('.mod-curve-node');
          if (c) {
            c.classList.toggle('sel', on);
            c.setAttribute('r', on ? '5.8' : '4.5');
          }
        });
        const curNd = draftCurve.nodes[i] || nd;
        if (nodeHeadEl) nodeHeadEl.textContent = `Node ${i + 1} / ${draftCurve.nodes.length}`;
        if (nodeXInput) nodeXInput.value = String(Number(curNd.x.toFixed(3)));
        if (nodeYInput) nodeYInput.value = String(Number(curNd.y.toFixed(3)));
        if (nodeSmoothBtn) nodeSmoothBtn.setChecked?.(!!curNd.smooth);
        vis.focus({ preventScroll: true });

        if (!tryCapture(vis, e)) { svg.removeAttribute('data-gesture-active'); window.dispatchEvent(new Event('verb-control-end')); return; }
        const gesture = createTxGesture(store, 'Edit modulation curve');
        let active = true;
        const loX = i > 0 ? draftCurve.nodes[i - 1].x : 0;
        const hiX = i < draftCurve.nodes.length - 1 ? draftCurve.nodes[i + 1].x : 1;

        const updateFromPointer = ev => {
          const rect = svg.getBoundingClientRect();
          const sx = ((ev.clientX - rect.left) / Math.max(1, rect.width)) * W;
          const sy = ((ev.clientY - rect.top) / Math.max(1, rect.height)) * H;
          const snap = v => ev.shiftKey ? Number(v.toFixed(4)) : Number((Math.round(v * 100) / 100).toFixed(3));
          const nx = Math.max(loX, Math.min(hiX, snap(fromSvgX(sx))));
          const ny = snap(fromSvgY(sy));
          draftCurve = {
            ...draftCurve,
            nodes: draftCurve.nodes.map((node, ni) => ni === i ? { ...node, x: nx, y: ny } : node)
          };
          const ncx = toSvgX(nx), ncy = toSvgY(ny);
          hit.setAttribute('cx', String(ncx));
          hit.setAttribute('cy', String(ncy));
          vis.setAttribute('cx', String(ncx));
          vis.setAttribute('cy', String(ncy));
          vis.setAttribute('aria-valuenow', String(ny));
          if (nodeXInput) nodeXInput.value = String(Number(nx.toFixed(3)));
          if (nodeYInput) nodeYInput.value = String(Number(ny.toFixed(3)));
          paintCurveGeometry();
          liveUpdater?.();
          gesture.send({ op: 'modulation.setRoute', index: activeIdx, route: normalizeRoute({ ...r, curve: draftCurve }, ns) });
        };

        const move = ev => { if (active && ev.pointerId === e.pointerId) updateFromPointer(ev); };
        const finish = async cancelled => {
          if (!active) return;
          active = false;
          vis.removeEventListener('pointermove', move);
          vis.removeEventListener('pointerup', up);
          vis.removeEventListener('pointercancel', cancel);
          vis.removeEventListener('lostpointercapture', cancel);
          window.removeEventListener('verb-gestures-reset', cancel);
          window.removeEventListener('verb-ui-error', cancel);
          if (cancelled) gesture.cancel();
          else await gesture.end();
          svg.removeAttribute('data-gesture-active');
          window.dispatchEvent(new Event('verb-control-end'));
        };
        const up = () => finish(false), cancel = () => finish(true);
        vis.addEventListener('pointermove', move);
        vis.addEventListener('pointerup', up);
        vis.addEventListener('pointercancel', cancel);
        vis.addEventListener('lostpointercapture', cancel);
        window.addEventListener('verb-gestures-reset', cancel);
        window.addEventListener('verb-ui-error', cancel);
      });

      vis.addEventListener('keydown', e => {
        if (disabledEdit) return;
        if (e.key === 'Delete' || e.key === 'Backspace') {
          e.preventDefault(); e.stopPropagation();
          deleteNodeAt(i);
          return;
        }
        const dx = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0;
        const dy = e.key === 'ArrowUp' ? 1 : e.key === 'ArrowDown' ? -1 : 0;
        if (!dx && !dy) return;
        e.preventDefault(); e.stopPropagation();
        const step = e.shiftKey ? 0.005 : 0.025;
        const loX = i > 0 ? draftCurve.nodes[i - 1].x : 0;
        const hiX = i < draftCurve.nodes.length - 1 ? draftCurve.nodes[i + 1].x : 1;
        const nx = Math.max(loX, Math.min(hiX, Number((nd.x + dx * step).toFixed(4))));
        const ny = Math.max(0, Math.min(1, Number((nd.y + dy * step).toFixed(4))));
        commitCurveNow({ ...draftCurve, nodes: draftCurve.nodes.map((node, ni) => ni === i ? { ...node, x: nx, y: ny } : node) }, i);
      });
      nodesGroup.append(g);
    });
  };

  svg.addEventListener('dblclick', e => {
    if (disabledEdit || e.target.closest('.mod-node-g') || draftCurve.nodes.length >= MOD_MAX_NODES) return;
    const rect = svg.getBoundingClientRect();
    const sx = ((e.clientX - rect.left) / Math.max(1, rect.width)) * W;
    const sy = ((e.clientY - rect.top) / Math.max(1, rect.height)) * H;
    addNodeAt(fromSvgX(sx), fromSvgY(sy));
  });

  // ── Live driver & target indicator ──────────────────────────────────────────
  const liveStatusBadge = element('span', { className: 'mod-live-badge' });
  const liveRawText = element('span', { className: 'mod-live-val' });
  const liveRawFill = element('i', { className: 'mod-live-fill' });
  const liveTgtText = element('span', { className: 'mod-live-val' });
  const liveTgtFill = element('i', { className: 'mod-live-fill tgt' });

  const liveCard = element('div', { className: 'mod-live-card', 'aria-label': 'Live driver and target readout' }, [
    element('div', { className: 'mod-live-head' }, [
      element('span', { text: 'Live telemetry' }),
      liveStatusBadge
    ]),
    element('div', { className: 'mod-live-metric' }, [
      element('div', { className: 'mod-live-row' }, [element('span', { text: dm.label }), liveRawText]),
      element('div', { className: 'mod-live-bar' }, liveRawFill)
    ]),
    element('div', { className: 'mod-live-metric' }, [
      element('div', { className: 'mod-live-row' }, [element('span', { text: tm.label }), liveTgtText]),
      element('div', { className: 'mod-live-bar' }, liveTgtFill)
    ])
  ]);

  liveUpdater = () => {
    if (!store.state) return;
    const curMode = (store.state.parameters?.values?.mode ?? 0) >= 0.5;
    const wire = store.telemetry['status.load']?.modulation?.routes?.[activeIdx];
    const sim = computeModulationTelemetry({
      ...store.state,
      document: {
        ...store.state.document,
        modulation: {
          routes: routes.map((rt, idx) => idx === activeIdx ? normalizeRoute({ ...r, curve: draftCurve }, ns) : rt)
        }
      }
    }, 0);
    const simPrev = sim.previews[activeIdx] || { raw: 0, value: 0 };
    const isWireLive = curMode && !!r.enabled && !!(wire && wire.active);
    const raw = isWireLive ? wire.raw : simPrev.raw;
    const simVal = tm.hostOnly
      ? Math.max(0, Math.min(1, evaluateModCurveNormalized(draftCurve, raw)))
      : evaluateModCurve(draftCurve, raw);
    const val = isWireLive ? wire.value : simVal;

    if (zoneLiveEl) {
      const zl = zoneList(store.state);
      const occ = liveOccupancy(store.state, store.telemetry) || new Map(computeZoneOccupancy(zl, withPathPoses(store.state).parameters.values, ns).map(o => [o.id, o.objects]));
      const insideIds = zl.filter(z => (occ.get(z.id) || []).includes(r.object)).map(z => z.id);
      const zr = dm.hasZone ? zl.find(z => z.id === r.zoneId) : null;
      const lit = dm.hasZone ? insideIds.includes(r.zoneId) : insideIds.length > 0;
      zoneLiveEl.textContent = dm.hasZone ? (zr ? (lit ? '● inside' : '○ outside') : '○ missing') : (lit ? `● in zone ${zl.findIndex(z => z.id === insideIds[0]) + 1}` : '○ outside all');
      zoneLiveEl.className = `mod-zone-live${lit ? ' lit' : ''}`;
      zoneLiveEl.style.setProperty('--zone-colour', zr ? zoneColour(zr, zl.indexOf(zr)) : 'var(--accent, #22c55e)');
    }
    liveStatusBadge.className = `mod-live-badge ${!curMode ? 'phys' : !r.enabled ? 'off' : isWireLive ? 'live' : 'idle'}`;
    liveStatusBadge.textContent = !curMode ? 'Bypassed (Physical)' : !r.enabled ? 'Disabled' : isWireLive ? 'Live' : 'Idle preview';
    liveRawText.textContent = formatModValue(raw, dm.unit);
    liveTgtText.textContent = formatModValue(val, tm.unit, tm.isFreq);

    const in0 = draftCurve.inputRange[0], in1 = draftCurve.inputRange[1];
    const inFrac = Math.abs(in1 - in0) > 1e-12 ? Math.max(0, Math.min(1, (raw - in0) / (in1 - in0))) : 0;
    const out0 = draftCurve.outputRange[0], out1 = draftCurve.outputRange[1];
    const outMin = Math.min(out0, out1), outMax = Math.max(out0, out1);
    const outFrac = tm.hostOnly
      ? Math.max(0, Math.min(1, val))
      : (Math.abs(outMax - outMin) > 1e-12 ? Math.max(0, Math.min(1, (val - outMin) / (outMax - outMin))) : 0);
    liveRawFill.style.width = `${(inFrac * 100).toFixed(1)}%`;
    liveTgtFill.style.width = `${(outFrac * 100).toFixed(1)}%`;

    if (midi) {  // V04e: last message sent + flash when the send counter moves
      const wm = wire?.midi;
      const count = Number(wm?.count) || 0;
      midiLiveEl.textContent = !curMode ? 'Physical: no MIDI' : !r.enabled ? 'Route disabled' : (wm?.last ? describeMidiMessage(wm.last) : 'no message yet');
      if (count && count !== Number(midiLiveEl.dataset.count || 0)) {
        midiLiveEl.dataset.count = String(count);
        midiLiveEl.classList.remove('flash'); void midiLiveEl.offsetWidth; midiLiveEl.classList.add('flash');
      }
    }
    if (hostOutVal > 0) {
      const liveHost = !curMode ? 0.5
        : !r.enabled ? 0.5
        : tm.hostOnly ? clamp(val, 0, 1)
        : clamp(evaluateModCurveNormalized(draftCurve, raw), 0, 1);
      hostOutLiveEl.textContent = liveHost.toFixed(2);
      hostOutLiveEl.className = `mod-host-val${!curMode || !r.enabled ? ' phys' : ''}`;
      hostOutLiveEl.title = !curMode
        ? `Mod Out ${hostOutVal} held neutral at 0.50 in Physical mode`
        : !r.enabled
          ? `Mod Out ${hostOutVal} held at 0.50 (route disabled)`
          : `Mod Out ${hostOutVal} output: ${liveHost.toFixed(3)} (REAPER: Parameter modulation → Link from FX parameter)`;
    }

    const xN = liveCurveX(draftCurve, raw);
    const yN = sampleCurveEval(xN);
    const cx = toSvgX(xN).toFixed(1), cy = toSvgY(yN).toFixed(1);
    liveCrossV.setAttribute('x1', cx); liveCrossV.setAttribute('x2', cx);
    liveCrossV.setAttribute('y1', String(PT)); liveCrossV.setAttribute('y2', String(PT + PH));
    liveCrossH.setAttribute('x1', String(PL)); liveCrossH.setAttribute('x2', String(PL + PW));
    liveCrossH.setAttribute('y1', cy); liveCrossH.setAttribute('y2', cy);
    liveDotEl.setAttribute('cx', cx); liveDotEl.setAttribute('cy', cy);
    liveDotEl.classList.toggle('inactive', !curMode || !r.enabled);
  };

  const relayoutSvg = () => {
    svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
    plotBg.setAttribute('x', String(PL));
    plotBg.setAttribute('width', String(PW));
    plotBg.setAttribute('height', String(PH));
    for (const g of gridV) {
      const x = toSvgX(g.f);
      g.el.setAttribute('x1', String(x)); g.el.setAttribute('x2', String(x));
      g.el.setAttribute('y1', String(PT)); g.el.setAttribute('y2', String(PT + PH));
    }
    for (const g of gridH) {
      const y = toSvgY(g.f);
      g.el.setAttribute('x1', String(PL)); g.el.setAttribute('x2', String(PL + PW));
      g.el.setAttribute('y1', String(y)); g.el.setAttribute('y2', String(y));
    }
    leftAxisMax.setAttribute('x', String(PL - 6)); leftAxisMax.setAttribute('y', String(PT + 9));
    leftAxisMin.setAttribute('x', String(PL - 6)); leftAxisMin.setAttribute('y', String(PT + PH));
    botAxisLeft.setAttribute('x', String(PL)); botAxisLeft.setAttribute('y', String(H - 5));
    botAxisRight.setAttribute('x', String(PL + PW)); botAxisRight.setAttribute('y', String(H - 5));
    paintCurveGeometry();
    positionNodes();
    liveUpdater();
  };

  paintCurveGeometry();
  renderNodes();
  liveUpdater();

  const updateSvgSize = () => {
    if (!svg.isConnected || svg.hasAttribute('data-gesture-active')) return;
    const rect = svg.getBoundingClientRect();
    const nw = Math.round(rect.width), nh = Math.round(rect.height);
    if (nw < 80 || nh < 60) return;
    const nextPL = nw < 280 ? 42 : nw < 360 ? 46 : 52;
    if (Math.abs(nw - W) < 2 && Math.abs(nh - H) < 2 && nextPL === PL) return;
    PL = nextPL;
    W = nw; H = nh;
    PW = Math.max(40, W - PL - PR);
    PH = Math.max(40, H - PT - PB);
    relayoutSvg();
  };
  if (typeof ResizeObserver === 'function') {
    svgResizeObs = new ResizeObserver(updateSvgSize);
    svgResizeObs.observe(svg);
  }

  // ── Curve Numeric Settings & Selected Node Controls ─────────────────────────
  const curNode = draftCurve.nodes[activeNodeIdx] || draftCurve.nodes[0];
  const loNodeX = () => activeNodeIdx > 0 ? draftCurve.nodes[activeNodeIdx - 1].x : 0;
  const hiNodeX = () => activeNodeIdx < draftCurve.nodes.length - 1 ? draftCurve.nodes[activeNodeIdx + 1].x : 1;

  nodeHeadEl = element('span', { className: 'mod-node-title', text: `Node ${activeNodeIdx + 1} / ${draftCurve.nodes.length}` });
  addNodeBtn = button('+ Node', insertMidNode, {
    disabled: disabledEdit || draftCurve.nodes.length >= MOD_MAX_NODES,
    'aria-label': 'Add curve node',
    'data-focus-key': `${scope}-mod-node-add`,
    title: 'Insert a curve node (or double-click curve)'
  });
  delNodeBtn = button('− Node', () => deleteNodeAt(activeNodeIdx), {
    disabled: disabledEdit || draftCurve.nodes.length <= MOD_MIN_NODES,
    'aria-label': 'Delete curve node',
    'data-focus-key': `${scope}-mod-node-del`,
    title: 'Remove selected node (min 2 nodes)'
  });

  nodeXInput = register(txNumericField({
    store,
    label: 'Node X',
    txLabel: 'Edit curve node X',
    value: curNode.x,
    min: loNodeX,
    max: hiNodeX,
    step: 0.01,
    disabled: disabledEdit,
    onPreview: x => {
      draftCurve = { ...draftCurve, nodes: draftCurve.nodes.map((n, i) => i === activeNodeIdx ? { ...n, x } : n) };
      paintCurveGeometry(); positionNodes(); liveUpdater?.();
    },
    getCommitCmd: x => ({
      op: 'modulation.setRoute',
      index: activeIdx,
      route: normalizeRoute({ ...r, curve: { ...draftCurve, nodes: draftCurve.nodes.map((n, i) => i === activeNodeIdx ? { ...n, x } : n) } }, ns)
    })
  }));

  nodeYInput = register(txNumericField({
    store,
    label: 'Node Y',
    txLabel: 'Edit curve node Y',
    value: curNode.y,
    min: 0,
    max: 1,
    step: 0.01,
    disabled: disabledEdit,
    onPreview: y => {
      draftCurve = { ...draftCurve, nodes: draftCurve.nodes.map((n, i) => i === activeNodeIdx ? { ...n, y } : n) };
      paintCurveGeometry(); positionNodes(); liveUpdater?.();
    },
    getCommitCmd: y => ({
      op: 'modulation.setRoute',
      index: activeIdx,
      route: normalizeRoute({ ...r, curve: { ...draftCurve, nodes: draftCurve.nodes.map((n, i) => i === activeNodeIdx ? { ...n, y } : n) } }, ns)
    })
  }));

  nodeSmoothBtn = check('Smooth', !!curNode.smooth, smooth => {
    const nextNodes = draftCurve.nodes.map((n, i) => i === activeNodeIdx ? { ...n, smooth } : n);
    commitCurveNow({ ...draftCurve, nodes: nextNodes }, activeNodeIdx);
  }, {
    disabled: disabledEdit || activeNodeIdx >= draftCurve.nodes.length - 1,
    'aria-label': 'Smooth segment after selected node',
    'data-focus-key': `${scope}-mod-node-smooth`,
    title: 'Smooth S-curve transition from this node to the next'
  });

  const rangeField = (lbl, txLbl, curVal, minVal, maxVal, stepVal, applyRange) => register(txNumericField({
    store,
    label: lbl,
    txLabel: txLbl,
    value: curVal,
    min: minVal,
    max: maxVal,
    step: stepVal,
    clampOnSubmit: true,
    disabled: disabledEdit,
    getCommitCmd: v => ({ op: 'modulation.setRoute', index: activeIdx, route: normalizeRoute({ ...r, curve: applyRange(v) }, ns) })
  }));

  const inMinF = rangeField('Input min', 'Edit curve input min', draftCurve.inputRange[0], -1000000, 1000000, dm.step, v => ({ ...draftCurve, inputRange: [v, draftCurve.inputRange[1]] }));
  const inMaxF = rangeField('Input max', 'Edit curve input max', draftCurve.inputRange[1], -1000000, 1000000, dm.step, v => ({ ...draftCurve, inputRange: [draftCurve.inputRange[0], v] }));
  const outMinF = rangeField('Output min', 'Edit curve output min', draftCurve.outputRange[0], tm.min, tm.max, tm.step, v => ({ ...draftCurve, outputRange: [v, draftCurve.outputRange[1]] }));
  const outMaxF = rangeField('Output max', 'Edit curve output max', draftCurve.outputRange[1], tm.min, tm.max, tm.step, v => ({ ...draftCurve, outputRange: [draftCurve.outputRange[0], v] }));

  const invertChk = check('Invert', !!draftCurve.invert, invert => commitCurveNow({ ...draftCurve, invert }, activeNodeIdx), {
    disabled: disabledEdit,
    'aria-label': 'Invert curve input',
    'data-focus-key': `${scope}-mod-invert`,
    title: 'Flip curve input direction'
  });
  const clampChk = check('Clamp', draftCurve.clamp !== false, clampVal => commitCurveNow({ ...draftCurve, clamp: clampVal }, activeNodeIdx), {
    disabled: disabledEdit,
    'aria-label': 'Clamp curve output',
    'data-focus-key': `${scope}-mod-clamp`,
    title: 'Clamp driver & output inside configured Input/Output ranges'
  });

  const curveBox = element('div', { className: 'mod-curve-box' }, [svg]);

  const nodeBar = element('div', { className: 'mod-node-bar' }, [
    nodeHeadEl,
    element('label', { className: 'mod-node-kv' }, [element('span', { text: 'X' }), nodeXInput]),
    element('label', { className: 'mod-node-kv' }, [element('span', { text: 'Y' }), nodeYInput]),
    nodeSmoothBtn,
    element('span', { className: 'mod-node-spacer' }),
    addNodeBtn,
    delNodeBtn
  ]);

  const rangesCard = element('div', { className: 'mod-ranges-card' }, [
    element('div', { className: 'mod-range-row' }, [
      element('span', { className: 'mod-lbl', text: dm.inLabel || `In (${dm.unit || 'u'})`, title: dm.desc || `Input range (${dm.unit || 'normalized'})` }),
      inMinF,
      element('span', { className: 'mod-sep', text: '…' }),
      inMaxF
    ]),
    ...(tm.hostOnly
      ? [element('div', { className: 'mod-hostout-note', text: 'Out: 0–1 (normalized curve to Host out)' })]
      : [element('div', { className: 'mod-range-row' }, [
          element('span', { className: 'mod-lbl', text: `Out (${tm.unit})`, title: `Output range (${tm.unit})` }),
          outMinF,
          element('span', { className: 'mod-sep', text: '…' }),
          outMaxF
        ])]),
    element('div', { className: 'mod-opts-row' }, [invertChk, clampChk])
  ]);

  const bottomGrid = element('div', { className: 'mod-win-bottom-grid' }, [rangesCard, liveCard]);
  editorWrap.append(metaGrid, ...(pointRow ? [pointRow] : []), ...(zonePickRow ? [zonePickRow] : []), ...(midiRow ? [midiRow] : []), curveBox, nodeBar, bottomGrid);

  return {
    editorWrap,
    liveUpdater,
    dispose: () => { svgResizeObs?.disconnect(); }
  };
}

// ── Bottom Tab Version (Docked route list + full editor, or compact list + hint when detached) ──
export function modulationPanel(ctx) {
  const { store } = ctx;
  const m = resolveModContext(store);
  const actions = createModActions(store, m);
  const disposers = [];
  const register = el => { if (typeof el?.dispose === 'function') disposers.push(() => el.dispose()); return el; };

  const toolbar = buildModToolbar({
    store, m, actions, scope: 'tab', register, showDetach: true,
    budgetClass: 'mod-budget', className: 'mod-toolbar'
  });
  const creativeHint = buildCreativeHint(m.creative);
  const isDetached = !!store.view.modEditorOpen;
  const { listCol, updateListReadouts } = buildRouteList({
    store, m, actions, scope: 'tab', register,
    viewClass: 'mod-list-view',
    rootClass: isDetached ? 'mod-list-col' : 'mod-list-col mod-dock-list-col'
  });

  let editor = null;
  let mainContent;
  if (isDetached) {
    const hintRow = element('div', { className: 'mod-tab-hint' }, [
      element('span', {
        className: 'mod-tab-hint-text',
        text: 'Floating window detached · route selection and edits stay in sync'
      }),
      button('Dock ↙', () => {
        requestModReturnFocus('detach');
        store.setView({ modEditorOpen: false });
      }, {
        className: 'mod-open-hint-btn',
        'aria-label': 'Dock modulation editor back into Modulation tab',
        'data-focus-key': 'tab-mod-dock-btn',
        title: 'Dock modulation editor back into Modulation tab'
      })
    ]);
    mainContent = [listCol, hintRow];
  } else {
    editor = buildRouteEditor({ store, m, actions, register, scope: 'dock' });
    disposers.push(() => editor.dispose?.());
    const editorCol = element('div', { className: 'mod-dock-editor-col' }, [editor.editorWrap]);
    mainContent = [element('div', { className: 'mod-dock-split' }, [listCol, editorCol])];
  }

  disposers.push(store.subscribe(({ reason }) => {
    if (['status.load', 'parameters', 'patch', 'snapshot'].includes(reason)) {
      editor?.liveUpdater?.();
      updateListReadouts();
    }
  }));

  const inner = element('div', { className: `mod-panel${isDetached ? ' is-detached' : ' is-docked'}${!m.creative ? ' is-physical' : ''}` }, [
    toolbar,
    ...(creativeHint ? [creativeHint] : []),
    ...mainContent
  ]);

  const sa = scrollArea(inner, { className: 'mod-scroll-view', rootClass: 'mod-scroll-sa' });
  const baseDispose = sa.dispose;
  sa.dispose = () => {
    disposers.forEach(fn => { try { fn(); } catch {} });
    baseDispose?.();
  };
  return sa;
}

// ── Floating Movable Resizable Modulation Editor Panel (in .viewport) ────────────────────────
export function modulationEditorPanel(ctx) {
  const { store } = ctx;
  const m = resolveModContext(store);
  const actions = createModActions(store, m);
  const disposers = [];
  const register = el => { if (typeof el?.dispose === 'function') disposers.push(() => el.dispose()); return el; };
  const isMin = !!store.view.modEditorMin;

  const root = element('section', {
    className: `mod-editor-panel panel${!m.creative ? ' is-physical' : ''}${isMin ? ' collapsed' : ''}`,
    role: 'region',
    'aria-label': 'Modulation editor',
    'data-mod-editor': 'open',
    tabIndex: -1
  });

  const activeRouteSummary = m.activeRoute
    ? (() => {
        const tm = targetMeta(m.activeRoute.target);
        const srcName = shortObjectLabel(m.sources, m.activeRoute.object);
        const tgtPrefix = isGlobalTarget(m.activeRoute.target)
          ? 'Global'
          : m.activeRoute.targetObject >= 0 && m.activeRoute.targetObject !== m.activeRoute.object
            ? shortObjectLabel(m.sources, m.activeRoute.targetObject)
            : srcName;
        if (tm.hostOnly) {
          const ho = m.activeRoute.hostOut || 0;
          return `${srcName} → ${ho > 0 ? `Mod Out ${ho}` : tm.label}`;
        }
        return `${srcName} → ${tgtPrefix} ${tm.label}`;
      })()
    : `${m.routes.length} / ${MOD_MAX_ROUTES} routes`;

  const closePanel = () => {
    requestModReturnFocus();
    store.setView({ modEditorOpen: false });
  };

  const header = element('div', { className: 'mod-win-header' }, [
    element('b', { text: 'Modulation editor' }),
    element('span', { className: 'mod-win-sub', text: activeRouteSummary, title: activeRouteSummary }),
    element('span', { className: 'mod-win-spacer' }),
    button(isMin ? '+' : '−', () => store.setView({ modEditorMin: !isMin }), {
      className: 'mod-win-min',
      'aria-label': 'Collapse modulation editor',
      'aria-expanded': String(!isMin),
      'data-focus-key': 'mod-win-min',
      title: 'Collapse / expand'
    }),
    button('×', closePanel, {
      className: 'mod-win-close',
      'aria-label': 'Close modulation editor',
      'data-focus-key': 'mod-win-close',
      title: 'Dock / close floating editor (Esc)'
    })
  ]);

  const toolbar = buildModToolbar({
    store, m, actions, scope: 'win', register, showExpand: false,
    budgetClass: 'mod-win-budget', className: 'mod-toolbar mod-win-toolbar'
  });
  const creativeHint = buildCreativeHint(m.creative);
  const { listCol, updateListReadouts } = buildRouteList({
    store, m, actions, scope: 'win', register,
    viewClass: 'mod-win-list-view', rootClass: 'mod-list-col mod-win-list-col'
  });
  const editor = buildRouteEditor({ store, m, actions, register, scope: 'win' });
  disposers.push(() => editor.dispose?.());

  const editorScroll = register(scrollArea(editor.editorWrap, {
    className: 'mod-win-editor-view',
    rootClass: 'mod-editor-col mod-win-editor-sa'
  }));

  const split = element('div', { className: 'mod-win-split' }, [listCol, editorScroll]);
  const body = element('div', { className: 'mod-win-body' }, [
    toolbar,
    ...(creativeHint ? [creativeHint] : []),
    split
  ]);
  const grip = element('div', { className: 'mod-win-grip', 'aria-label': 'Resize modulation editor', title: 'Drag to resize' });

  root.append(header, body, grip);

  disposers.push(store.subscribe(({ reason }) => {
    if (['status.load', 'parameters', 'patch', 'snapshot'].includes(reason)) {
      editor.liveUpdater?.();
      updateListReadouts();
    }
  }));

  // ── Movable / Resizable box state (timelinePanel pattern) ───────────────────
  const bounds = () => root.parentElement?.getBoundingClientRect() || { left: 0, top: 0, right: window.innerWidth, bottom: window.innerHeight };
  const box = { dx: 0, dy: 0, w: 0, h: 0, ...(store.view.modEditorBox || {}) };
  const MIN_W = 340, MIN_H = 260;

  const applyBox = b => {
    root.style.transform = b.dx || b.dy ? `translate(${b.dx}px, ${b.dy}px)` : '';
    if (b.w) { root.style.right = 'auto'; root.style.width = `${b.w}px`; } else { root.style.right = ''; root.style.width = ''; }
    if (b.h && !isMin) { root.style.maxHeight = 'none'; root.style.height = `${b.h}px`; } else { root.style.maxHeight = ''; root.style.height = ''; }
  };
  applyBox(box);

  const panelDrag = (e, el, onMove) => {
    if (e.button !== 0) return;
    e.preventDefault();
    if (!tryCapture(el, e)) return;
    root.setAttribute('data-gesture-active', 'true');
    const x0 = e.clientX, y0 = e.clientY, r0 = root.getBoundingClientRect(), b0 = { ...box };
    const move = ev => { if (ev.pointerId === e.pointerId) onMove(ev.clientX - x0, ev.clientY - y0, r0, b0); };
    const finish = () => {
      el.removeEventListener('pointermove', move);
      el.removeEventListener('pointerup', finish);
      el.removeEventListener('pointercancel', finish);
      el.removeEventListener('lostpointercapture', finish);
      window.removeEventListener('verb-gestures-reset', finish);
      window.removeEventListener('verb-ui-error', finish);
      store.setView({ modEditorBox: box.dx || box.dy || box.w || box.h ? { ...box } : null });
      root.removeAttribute('data-gesture-active');
      window.dispatchEvent(new Event('verb-control-end'));
    };
    el.addEventListener('pointermove', move);
    el.addEventListener('pointerup', finish);
    el.addEventListener('pointercancel', finish);
    el.addEventListener('lostpointercapture', finish);
    window.addEventListener('verb-gestures-reset', finish);
    window.addEventListener('verb-ui-error', finish);
  };

  header.addEventListener('pointerdown', e => {
    if (e.target.closest('button')) return;
    panelDrag(e, header, (ddx, ddy, r0, b0) => {
      const P = bounds();
      const cx = clamp(ddx, P.left - r0.left, P.right - r0.right);
      const cy = clamp(ddy, P.top - r0.top, P.bottom - r0.bottom);
      box.dx = Math.round(b0.dx + cx);
      box.dy = Math.round(b0.dy + cy);
      applyBox(box);
    });
  });
  header.addEventListener('dblclick', e => {
    if (!e.target.closest('button')) store.setView({ modEditorBox: null });
  });
  grip.addEventListener('pointerdown', e => panelDrag(e, grip, (ddx, ddy, r0, b0) => {
    const P = bounds();
    const maxW = Math.max(200, P.right - r0.left);
    const maxH = Math.max(160, r0.bottom - P.top);
    box.w = Math.round(clamp(r0.width + ddx, Math.min(MIN_W, maxW), maxW));
    box.h = Math.round(clamp(r0.height - ddy, Math.min(MIN_H, maxH), maxH));
    box.dx = b0.dx;
    box.dy = b0.dy;
    applyBox(box);
  }));

  const fit = () => {
    if (!root.isConnected) return;
    const P = bounds();
    const availW = Math.max(160, Math.floor(P.right - P.left - 8));
    const availH = Math.max(120, Math.floor(P.bottom - P.top - 8));
    let r = root.getBoundingClientRect();
    let changed = false;
    if (box.w && r.width > P.right - P.left - 2) {
      box.w = Math.max(Math.min(MIN_W, availW), Math.floor(P.right - P.left - 16));
      changed = true;
    }
    if (!isMin && box.h && r.height > P.bottom - P.top - 2) {
      box.h = Math.max(Math.min(MIN_H, availH), Math.floor(P.bottom - P.top - 16));
      changed = true;
    }
    if (changed) { applyBox(box); r = root.getBoundingClientRect(); }
    const dx = r.left < P.left ? P.left - r.left : r.right > P.right ? P.right - r.right : 0;
    const dy = r.top < P.top ? P.top - r.top : r.bottom > P.bottom ? P.bottom - r.bottom : 0;
    if (Math.abs(dx) >= 1 || Math.abs(dy) >= 1) {
      box.dx += Math.round(dx);
      box.dy += Math.round(dy);
      applyBox(box);
    }
    store.view.modEditorBox = box.dx || box.dy || box.w || box.h ? { ...box } : null;
  };

  let resizeObs = null;
  const rafId = requestAnimationFrame(() => {
    if (!root.isConnected) return;
    fit();
    listCol.update?.();
    editorScroll.update?.();
    if (typeof ResizeObserver !== 'undefined' && root.parentElement) {
      resizeObs = new ResizeObserver(() => {
        fit();
        listCol.update?.();
        editorScroll.update?.();
      });
      resizeObs.observe(root.parentElement);
    }
  });

  root.dispose = () => {
    cancelAnimationFrame(rafId);
    resizeObs?.disconnect();
    disposers.forEach(fn => { try { fn(); } catch {} });
  };
  return root;
}
