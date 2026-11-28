import { compilePath, pathId, pathGroupDriver, pathGroupMembers, MIN_WINDOW_GAP, isPointKey, clampPointTime, orientationMode, resolveFollowPath } from '../viewport/movement-path.js';
import { PATH_SHAPE_OPTIONS, PATH_SHAPE_META, defaultShapeParamsForObject, centreShapeOnListener, generatePathShapeForRoom } from '../viewport/path-shapes.js';
import { LFO_SHAPES, normalizeLfo, computePathLfoSwing, normalizeWander, isDefaultWander } from './modulation-model.js';
import { txSlider, txNumericField } from './tx-gesture.js';
import { bakeSourceText, pathBakeView } from './path-bake.js';
import { directivityShape } from '../viewport/marker.js';
import { element, button, select, check, row, note, unavailable, accordionSection } from './dom.js';
import { linkButton, linkPicker, followedNote, followerPathControls, isFollower } from './path-link.js';
import { slider, numericField } from './slider.js';
import { FORMATS, LAYOUTS, MICS, channels, budget, surface, motionEnabled } from '../state/selectors.js';
import { bandEditor } from './band-editor.js';
import { materialPicker, materialCatalogue, MATERIAL_CLASSES } from './material-picker.js';
import { materialColour, surfaceRole } from '../viewport/colour.js';
import { wallSelectionSummary, computeRoomMetrics } from '../viewport/room-metrics.js';
import { viewportState } from '../viewport/effective-scene.js';
import { sceneAutomationRange, isDefaultAutomationRange, computeFitRange } from '../bridge/mock-model.js';
export const heading = text => element('h3', { text });
export function parameterToggle(ctx, id, label) {
  const { store, bridge } = ctx, value = store.state.parameters.values[id];
  const root = check(label, value >= 0.5, enabled => { bridge.parameters.begin(id); bridge.parameters.change(id, enabled ? 1 : 0); bridge.parameters.end(id); });
  const update = () => { root.setChecked(store.state.parameters.values[id] >= 0.5); root.title = store.state.parameters.texts?.[id] ?? label; };
  const off = store.subscribe(({reason}) => { if (['parameters','snapshot','patch'].includes(reason)) update(); }); update(); root.dispose = off; return root;
}
// DAW-style square M / S button bound to a host parameter.
export function parameterSwitch(ctx, id, label, kind, name) {
  const { store, bridge } = ctx, on = () => store.state.parameters.values[id] >= 0.5;
  const root = element('button', { type: 'button', className: `ms ${kind}${on() ? ' on' : ''}`, 'aria-pressed': String(on()), 'aria-label': name, title: name, text: label, onclick: () => { const next = on() ? 0 : 1; bridge.parameters.begin(id); bridge.parameters.change(id, next); bridge.parameters.end(id); } });
  const update = () => { const v = on(); root.classList.toggle('on', v); root.setAttribute('aria-pressed', String(v)); root.title = `${name} · ${store.state.parameters.texts?.[id] ?? (v ? 'On' : 'Off')}`; };
  root.dispose = store.subscribe(({reason}) => { if (['parameters','snapshot','patch'].includes(reason)) update(); }); update();
  return root;
}
const group = (...children) => element('div', { className: 'stack' }, children);
// ── Output / listener -------------------------------------------------------
export function makeupControls(ctx) {
  return element('div', { className: 'inline makeup-controls' }, [
    ctx.param('makeup', { knob: true, label: 'Makeup' }),
    button('Auto', () => ctx.store.command({ op: 'output.autoMakeup' }), { title: 'Set Makeup from the last complete model; stop moving before pressing', 'aria-label': 'Auto makeup' })
  ]);
}
export function outputParts(ctx) {
  const { store } = ctx, state = store.state, o = state.document.output, command = patch => store.command({ op: 'output.set', ...patch });
  const effLayout = o.layout >= 0 ? o.layout : state.host.effectiveLayout;
  const ambiOrder = effLayout >= 9 && effLayout <= 11 ? effLayout - 8 : 0;  // AmbiX 1st..3rd order: fixed coincident capsules
  const radiusReason = effLayout === 0 ? 'Mono: single point' : ambiOrder ? 'AmbiX: coincident'  : o.micPattern !== 7 && effLayout === 1 ? 'ORTF: fixed 17 cm' : o.micPattern !== 7 && effLayout === 2 ? 'XY: coincident pair' : '';
  const micControl = ambiOrder
    ? select('Microphone pattern', [[o.micPattern, ambiOrder === 1 ? 'Fixed by layout (W omni, Y/Z/X figure-8)' : 'Fixed by layout (spherical harmonics)']], o.micPattern, () => {}, { disabled: true, title: 'Fixed by layout for AmbiX (' + ['1st', '2nd', '3rd'][ambiOrder - 1] + ' order)' })
    : select('Microphone pattern', MICS, o.micPattern, n => command({ micPattern: Number(n) }));
  return {
    main: [row('Layout', select('Output layout', LAYOUTS.map((v, i) => [i - 1, v]), o.layout, n => command({ layout: Number(n) }))),
      ...(ambiOrder ? [
        row('Ambisonics normalization', select('Ambisonics normalization', ['SN3D', 'N3D', 'FuMa'], o.ambisonicsNormalization ?? 0,
          value => command({ ambisonicsNormalization: Number(value) }), { title: 'SN3D (AmbiX) and N3D use ACN channel order. N3D raises the higher orders. FuMa uses Furse-Malham channel order and MaxN weighting.' })),
        row('Ambisonics level', select('Ambisonics level', [[0, '0 dB'], [-3, '−3 dB'], [-6, '−6 dB']], o.ambisonicsLevelDb ?? 0,
          value => command({ ambisonicsLevelDb: Number(value) }), { title: 'Attenuates all Ambisonic channels equally after normalization. AmbiX uses SN3D at 0 dB.' }))
      ] : []),
      row('Microphones', micControl),
      check('Show microphone array to scale', !!store.view.showMic, value => store.setView({ showMic: value })),
      slider('Radius', o.radiusM, 0, 1, 0.001, radiusM => command({ radiusM }), 'm', { locked: !!radiusReason, reason: radiusReason }),
      ...(ambiOrder && o.ambisonicsNormalization === 2 && state.host.outputChannels < (ambiOrder + 1) ** 2
        ? [note(`FuMa needs ${(ambiOrder + 1) ** 2} host channels for the complete signal. Missing components are silent.`)] : []),
      row('Rendered / host', `${state.host.renderedChannels} / ${state.host.outputChannels} ch`),
      row('Ring channels', `${Math.min(state.host.outputChannels, 16)} · follows host`)],
    extra: [note('Human ears is a microphone model; binaural rendering is unavailable.'), element('div', { className: 'inline' }, [unavailable(state, 'binaural', 'Binaural')]),
      check('Keep processing during silence', !!state.document.editorFlags?.keepProcessing, keepProcessing => store.flags({ keepProcessing }), { title: KEEP_PROCESSING_HINT })]
  };
}
const KEEP_PROCESSING_HINT = 'Position automation also works before the first sound. Uses more CPU. REAPER: or enable Compatibility settings > Ignore plug-in reported tail size.';
export function outputControls(ctx) { const p = outputParts(ctx); return element('div', { className: 'stack' }, [...p.main, ...p.extra]); }
const BUILTIN_DIRECTIVITIES = [
  { id: 'th_koeln_voice_sentence', name: 'Human Voice · Sentence (TH Köln)', category: 'Voice', license: 'CC BY 4.0', attribution: 'Pörschmann & Arend (2023), TH Köln, DOI 10.5281/zenodo.7834211', isModel: false },
  { id: 'th_koeln_voice_phonemes', name: 'Human Voice · Phonemes (TH Köln)', category: 'Voice', license: 'CC BY 4.0', attribution: 'Pörschmann & Arend (2021), TH Köln, DOI 10.5281/zenodo.4739183', isModel: false },
  { id: 'voice_mouth_sphere_model', name: 'Human Voice · Mouth on Sphere (model)', category: 'Voice (model)', license: 'First-principles model', attribution: 'Morse & Ingard (1968) §7.2 piston on rigid sphere (a=0.09 m, θ0=12°)', isModel: true },
  { id: 'brass_trumpet_bell_model', name: 'Trumpet · Bell (model)', category: 'Brass (model)', license: 'First-principles model', attribution: 'Morse & Ingard (1968) §7.2 piston on rigid sphere (a=0.062 m, θ0=45°)', isModel: true },
  { id: 'brass_trombone_bell_model', name: 'Trombone · Bell (model)', category: 'Brass (model)', license: 'First-principles model', attribution: 'Morse & Ingard (1968) §7.2 piston on rigid sphere (a=0.108 m, θ0=45°)', isModel: true },
  { id: 'brass_horn_bell_model', name: 'French Horn · Bell (model)', category: 'Brass (model)', license: 'First-principles model', attribution: 'Morse & Ingard (1968) §7.2 piston on rigid sphere (a=0.155 m, θ0=50°)', isModel: true },
  { id: 'brass_tuba_bell_model', name: 'Tuba · Bell (model)', category: 'Brass (model)', license: 'First-principles model', attribution: 'Morse & Ingard (1968) §7.2 piston on rigid sphere (a=0.220 m, θ0=50°)', isModel: true },
  { id: 'genelec_8030c', name: 'Genelec 8030C · Studio Monitor (Jade HS)', category: 'Loudspeaker', license: 'CC BY 4.0', attribution: 'Stärz, Budnik & Blau (2026), Jade HS Oldenburg, DOI 10.5281/zenodo.21486919', isModel: false }
];
function analyticPolarSlices(pattern) {
  const p = Math.max(0, Math.min(1, Number(pattern) || 0));
  const slice = Array.from({ length: 72 }, (_, k) => Math.abs((1 - p) + p * Math.cos(k * 2 * Math.PI / 72)));
  return [slice, slice, slice];
}
function polarPreview(slices, activeProfile) {
  const svgNS = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(svgNS, 'svg');
  svg.setAttribute('viewBox', '-40 -40 80 80');
  svg.setAttribute('class', 'polar-svg');
  svg.setAttribute('aria-label', 'Horizontal directivity polar plot (125 Hz, 1 kHz, 8 kHz)');
  for (const r of [12, 24, 34]) {
    const circle = document.createElementNS(svgNS, 'circle');
    circle.setAttribute('cx', '0'); circle.setAttribute('cy', '0'); circle.setAttribute('r', String(r));
    circle.setAttribute('class', 'polar-grid');
    svg.append(circle);
  }
  const axis = document.createElementNS(svgNS, 'line');
  axis.setAttribute('x1', '0'); axis.setAttribute('y1', '36'); axis.setAttribute('x2', '0'); axis.setAttribute('y2', '-36');
  axis.setAttribute('class', 'polar-axis');
  svg.append(axis);
  const colours = ['#d49b75', '#6bc4a6', '#a99bc8'];
  // Redrawn in place while the Directivity slider moves, so the shape follows the hand.
  const draw = current => {
  svg.querySelectorAll('polygon').forEach(p => p.remove());
  const maxVal = Math.max(1, ...current.flat().map(v => Number(v) || 0));
  const scale = 28 / maxVal;
  current.forEach((bandSlice, b) => {
    if (!Array.isArray(bandSlice) || !bandSlice.length) return;
    const pts = bandSlice.map((g, k) => {
      const phi = k * 2 * Math.PI / bandSlice.length;
      const r = Math.max(0, Math.min(36, (Number(g) || 0) * scale));
      const x = -Math.sin(phi) * r, y = -Math.cos(phi) * r;
      return `${x.toFixed(2)},${y.toFixed(2)}`;
    }).join(' ');
    const poly = document.createElementNS(svgNS, 'polygon');
    poly.setAttribute('points', pts);
    poly.setAttribute('stroke', colours[b] || '#6bc4a6');
    poly.setAttribute('fill', b === 1 ? 'rgba(107,196,166,0.10)' : 'none');
    poly.setAttribute('stroke-width', b === 1 ? '0.7' : '0.55');
    svg.append(poly);
  });
  };
  draw(slices);
  const legend = element('div', { className: 'polar-legend' }, [
    element('span', { className: 'polar-tag', text: activeProfile ? '3D SH · 0° ↑' : 'Analytic · 0° ↑' }),
    ...[['125 Hz', '#d49b75'], ['1 kHz', '#6bc4a6'], ['8 kHz', '#a99bc8']].map(([label, col]) =>
      element('span', { className: 'polar-band', style: `--polar-col:${col}`, text: label }))
  ]);
  const root = element('div', { className: 'polar-preview' }, [svg, legend]);
  root.redraw = draw;
  return root;
}
// ── Source ------------------------------------------------------------------
export function sourceParts(ctx, index, opts = {}) {
  const { store, param } = ctx, state = store.state, source = state.document.sources[index];
  if (!source) return null;
  const collapsible = !!opts.collapsible;
  const p = store.state.document.paths?.[index];
  const pathHint = isFollower(p) ? `Linked to ${state.document.sources?.[p.follow.target]?.name || `Source ${p.follow.target + 1}`}` : (p?.points?.length ?? 0) >= 2 ? `Length ${compilePath(p).length.toFixed(2)} m · ${p.points.length} pts` : '';
  const config = patch => store.command({ op: 'source.setConfig', index, ...patch }), prefix = `s${index + 1}`;
  const name = element('input', { type: 'text', value: source.name, maxLength: state.limits.maxNameLength, 'aria-label': 'Source name', 'data-focus-key': 'source-name', onchange: e => config({ name: e.target.value }) });
  const ms = element('div', { className: 'ms-row', title: 'Mute beats Solo' }, [parameterSwitch(ctx, `${prefix}mute`, 'M', 'mute', `Source ${index + 1} mute`), parameterSwitch(ctx, `${prefix}solo`, 'S', 'solo', `Source ${index + 1} solo`)]);
  const points = [];
  if (source.freePoints && !source.sum) {
    for (let channel = 0; channel < channels(source); channel++) {
      if ([5, 6].includes(source.format) && channel === 3) { points.push(note(`Channel ${channel + 1} · LFE, no spatial point`)); continue; }
      const offsets = source.offsetsM[channel] || [0, 0, 0]; points.push(heading(`Point ${channel + 1} · local`));
      ['X', 'Y', 'Z'].forEach((axis, i) => points.push(slider(`Point ${channel + 1} ${axis}`, offsets[i], -100, 100, 0.001, n => { const offsetM = offsets.slice(); offsetM[i] = n; store.command({ op: 'source.setPointOffset', index, channel, offsetM }); }, 'm')));
    }
  }
  const catalogProfiles = store.telemetry['catalogs.list']?.directivityProfiles || BUILTIN_DIRECTIVITIES;
  const instId = source.instrument || 'none';
  const instAmount = source.instrumentAmount ?? 1;
  const aimAz = source.aimAzDeg ?? 0, aimEl = source.aimElDeg ?? 0, aimRoll = source.aimRollDeg ?? 0;
  const instOptions = [
    ['none', 'None (analytic)'],
    ...catalogProfiles.map(p => [p.id, p.name]),
    ...(instId === 'custom' || source.customDirectivityName ? [['custom', `Custom · ${source.customDirectivityName || 'user .verbdir'}`]] : []),
    ['__load_file__', 'Load .verbdir file…']
  ];
  const instSelect = select('Instrument directivity', instOptions, instId, chosen => {
    if (chosen === '__load_file__') {
      instSelect.value = instId;
      store.command({ op: 'source.loadDirectivityFile', index });
    } else {
      store.command({ op: 'source.setDirectivity', index, instrument: chosen, amount: instAmount });
    }
  }, { reselect: true });
  const selectedProfile = catalogProfiles.find(p => p.id === instId);
  if (selectedProfile) instSelect.title = `${selectedProfile.name} · ${selectedProfile.license} · ${selectedProfile.attribution}`;
  const instRow = row('Instrument', instSelect);
  const directivityControls = [instRow];
  let analyticPreview = null, resetAimBtn = null;
  if (instId === 'none') {
    const directivity = slider('Directivity', source.pattern, 0, 1, .001, pattern => { analyticPreview?.redraw(analyticPolarSlices(pattern)); store.command({ op: 'source.setPattern', index, pattern }); }, '', { live: true });
    directivity.title = '0 omni · 0.5 cardioid · 1 figure-8; direction set with right-drag';
    const shape = directivityShape(source.pattern);
    if (shape) directivity.querySelector(':scope > span').append(element('small', { className: 'directivity-shape', text: ` · ${shape}` }));
    directivityControls.push(directivity);
  } else {
    const amountSlider = slider('Profile amount', Math.round(instAmount * 100), 0, 100, 1, pct => store.command({ op: 'source.setDirectivity', index, instrument: instId, amount: pct / 100 }), '%', { live: true });
    amountSlider.title = '0% omnidirectional · 100% full 3D spherical-harmonic directivity profile';
    directivityControls.push(amountSlider);
    const sendAim = patch => store.command({ op: 'source.setDirectivity', index, ...patch });
    directivityControls.push(
      slider('Aim azimuth', aimAz, -180, 180, 1, aimAzDeg => sendAim({ aimAzDeg }), '°', { live: true }),
      slider('Aim elevation', aimEl, -90, 90, 1, aimElDeg => sendAim({ aimElDeg }), '°', { live: true }),
      slider('Aim roll', aimRoll, -180, 180, 1, aimRollDeg => sendAim({ aimRollDeg }), '°', { live: true })
    );
    const flipBtn = button('Flip front/back', () => {
      const cur = store.state.document.sources[index]?.aimAzDeg ?? 0;
      let next = ((cur + 180) % 360 + 360) % 360;
      if (next > 180) next -= 360;
      sendAim({ aimAzDeg: next });
    }, { title: 'Rotate profile 180° around vertical axis (flip front/back)' });
    resetAimBtn = button('Reset', () => sendAim({ aimAzDeg: 0, aimElDeg: 0, aimRollDeg: 0 }), {
      disabled: aimAz === 0 && aimEl === 0 && aimRoll === 0,
      title: 'Reset profile aim offsets to 0°'
    });
    directivityControls.push(element('div', { className: 'inline directivity-aim-actions' }, [flipBtn, resetAimBtn]));
  }
  const activeSlices = instId !== 'none' && Array.isArray(source.polarSlices) && source.polarSlices.length === 3
    ? source.polarSlices
    : instId !== 'none' && Array.isArray(selectedProfile?.polarSlices) && selectedProfile.polarSlices.length === 3
      ? selectedProfile.polarSlices.map(band => band.map(g => (1 - instAmount) + instAmount * g))
      : analyticPolarSlices(source.pattern);
  const preview = polarPreview(activeSlices, instId !== 'none');
  if (instId === 'none') analyticPreview = preview;
  else {
    preview.dispose = store.subscribe(({ reason }) => {
      if (reason !== 'snapshot' && reason !== 'patch') return;
      const nextSrc = store.state.document.sources[index];
      if (!nextSrc || (nextSrc.instrument || 'none') === 'none') return;
      if (Array.isArray(nextSrc.polarSlices) && nextSrc.polarSlices.length === 3) preview.redraw(nextSrc.polarSlices);
      if (resetAimBtn) {
        const az = nextSrc.aimAzDeg ?? 0, el = nextSrc.aimElDeg ?? 0, roll = nextSrc.aimRollDeg ?? 0;
        resetAimBtn.disabled = az === 0 && el === 0 && roll === 0;
      }
    });
  }
  directivityControls.push(preview);
  if (selectedProfile) {
    directivityControls.push(element('p', { className: 'note directivity-attr', text: `${selectedProfile.license} · ${selectedProfile.attribution}`, title: selectedProfile.attribution }));
  }
  if (source.directivityWarning) {
    directivityControls.push(element('p', { className: 'note warning directivity-warning', text: source.directivityWarning }));
  }
  const pose = [param(`${prefix}x`, { label: 'X' }), param(`${prefix}y`, { label: 'Y' }), param(`${prefix}z`, { label: 'Z' }), param(`${prefix}az`, { label: 'Azimuth' })];
  const orientation = orientationControls(ctx, index);
  const path = pathControls(ctx, index, { collapsible });
  const routingOnly = [element('div', { className: 'check-pair' }, [check('Sum', source.sum, sum => config({ sum }), { title: 'Sum channels to one point' }), check('Free pts', source.freePoints, freePoints => config({ freePoints }), { disabled: source.sum, title: 'Free channel points' })]),
    slider('Spread', source.spreadM, 0, 20, 0.001, spreadM => store.command({ op: 'source.setSpread', index, spreadM }), 'm')];
  return {
    source,
    identity: [row('Name', name), row('Format', select('Source format', FORMATS, source.format, n => config({ format: Number(n) }))),
      slider('First input', source.firstIn + 1, 1, state.limits.maxInputs, 1, n => config({ firstIn: n - 1 })),
      source.format === 7 ? slider('Channels', source.channels, 2, state.limits.maxSourceChannels, 1, n => config({ channels: n })) : row('Channels', `${channels(source)} · In ${source.firstIn + 1}–${source.firstIn + channels(source)}`)],
    routingOnly,
    directivity: directivityControls,
    directivityHint: instId === 'none' ? 'Analytic' : (selectedProfile?.name || 'Custom'),
    routing: [...routingOnly, ...directivityControls],
    pose,
    orientation,
    path,
    pathHint,
    position: [...pose, ...orientation, ...path],
    level: [element('div', { className: 'gain-ms-row' }, [param(`${prefix}g`, { label: 'Gain' }), ms])],
    points, ms
  };
}
export function listenerParts(ctx, opts = {}) {
  const { param } = ctx, collapsible = !!opts.collapsible;
  const p = ctx.store.state.document.paths?.[32];
  const pathHint = (p?.points?.length ?? 0) >= 2 ? `Length ${compilePath(p).length.toFixed(2)} m · ${p.points.length} pts` : '';
  const pose = ['lx', 'ly', 'lz', 'laz'].map((id, i) => param(id, { label: ['X', 'Y', 'Z', 'Yaw'][i] }));
  const orientation = orientationControls(ctx, 32);
  const path = pathControls(ctx, 32, { collapsible });
  return { pose, orientation, path, pathHint, position: [...pose, ...orientation, ...path], output: outputParts(ctx) };
}
// ── Scene / room -----------------------------------------------------------
export function scenePartsRoom(ctx) {
  const { store } = ctx, s = store.state.document.scene;
  const materials = materialCatalogue(store).map(m => [m.name, m.name.replace(/_/g, ' ')]);
  const apply = (kind, preset) => store.command({ op: 'surface.applyPreset', refs: [{ kind, index: 0 }], preset });
  const factorRow = slider('Scale factor', 1, .05, 20, .01, () => {}, '×'), factor = factorRow.querySelector('.readout'), include = check('Also move sources, listener and height', false, () => {});
  factor.setAttribute('aria-label', 'Scale factor');
  factorRow.querySelector('.track').setAttribute('aria-label', 'Scale factor slider');
  const setSeed = seed => store.command({ op: 'room.setClutter', seed });
  const seedRow = slider('Seed', s.clutter.seed, 1, 9999, 1, setSeed);
  seedRow.classList.add('clutter-seed');
  seedRow.querySelector('.numeric').append(button('Randomize', () => {
    const current = store.state.document.scene.clutter.seed;
    const pick = 1 + Math.floor(Math.random() * 9998);
    setSeed(pick < current ? pick : pick + 1);
  }, { 'aria-label': 'Randomize clutter seed' }));
  const ar = sceneAutomationRange(s), isDefRange = isDefaultAutomationRange(ar);
  const autoRangeDraft = { xM: [...ar.xM], yM: [...ar.yM], zM: [...ar.zM] };
  const countOutsideRange = box => {
    const vals = store.state.parameters.values;
    let outside = 0;
    const checkPos = (px, py, pz) => {
      if (px < box.xM[0] - 1e-5 || px > box.xM[1] + 1e-5 || py < box.yM[0] - 1e-5 || py > box.yM[1] + 1e-5 || pz < box.zM[0] - 1e-5 || pz > box.zM[1] + 1e-5) outside++;
    };
    const count = (store.state.document.sources || []).length;
    for (let i = 0; i < count; i++) checkPos(vals[`s${i + 1}x`] ?? 0, vals[`s${i + 1}y`] ?? 0, vals[`s${i + 1}z`] ?? 1.5);
    checkPos(vals.lx ?? 0, vals.ly ?? 0, vals.lz ?? 1.5);
    return outside;
  };
  const clampNote = element('small', { className: 'note auto-range-clamp', hidden: true });
  const confirmMsg = element('small', { className: 'note' });
  const confirmBox = element('div', { className: 'auto-range-confirm stack', hidden: true });
  const confirmActions = element('div', { className: 'auto-range-actions' }, [
    button('Confirm fit', () => { confirmBox.hidden = true; store.command({ op: 'room.fitAutomationRange' }); }, { className: 'primary' }),
    button('Cancel', () => { confirmBox.hidden = true; })
  ]);
  confirmBox.append(confirmMsg, confirmActions);
  const fitBtn = button('Fit to scene', () => {
    if (isDefRange) {
      const fitted = computeFitRange(store.state);
      const nOut = countOutsideRange(fitted);
      confirmMsg.textContent = `Changing from ±200 m rescales normalized DAW automation envelopes for this instance.${nOut > 0 ? ` ${nOut} object(s) outside the new range will be clamped.` : ''}`;
      confirmBox.hidden = false;
    } else {
      store.command({ op: 'room.fitAutomationRange' });
    }
  });
  const resetRangeBtn = button('Reset (±200 m)', () => {
    confirmBox.hidden = true;
    store.command({ op: 'room.setAutomationRange', reset: true });
  }, { disabled: isDefRange });
  const rangeInputs = {};
  const previewDraftBox = () => {
    const parseInput = key => Number(rangeInputs[key].value);
    const x0 = parseInput('xMin'), x1 = parseInput('xMax'), y0 = parseInput('yMin'), y1 = parseInput('yMax'), z0 = parseInput('zMin'), z1 = parseInput('zMax');
    if (![x0, x1, y0, y1, z0, z1].every(Number.isFinite)) { clampNote.hidden = true; return; }
    if (x0 < -200 || x1 > 200 || x1 - x0 < 1 || y0 < -200 || y1 > 200 || y1 - y0 < 1 || z0 < 0 || z1 > 30 || z1 - z0 < 0.5) {
      clampNote.textContent = 'Minimum span is 1.0 m for X/Y and 0.5 m for Z within global bounds.';
      clampNote.hidden = false;
      return;
    }
    const nOut = countOutsideRange({ xM: [x0, x1], yM: [y0, y1], zM: [z0, z1] });
    if (nOut > 0) {
      clampNote.textContent = `${nOut} object(s) outside the new range will be clamped.`;
      clampNote.hidden = false;
    } else {
      clampNote.textContent = '';
      clampNote.hidden = true;
    }
  };
  const makeBoundField = (key, label, val, gLo, gHi, minSpan, axisKey, endIdx) => {
    const field = numericField(label, val, gLo, gHi, 0.5, v => {
      const next = { ...autoRangeDraft, [axisKey]: [...autoRangeDraft[axisKey]] };
      next[axisKey][endIdx] = v;
      if (next[axisKey][1] - next[axisKey][0] < minSpan - 1e-9) {
        field.setCustomValidity(`Min span ${minSpan} m`);
        field.reportValidity();
        field.value = String(Number(ar[axisKey][endIdx].toFixed(3)));
        return;
      }
      field.setCustomValidity('');
      autoRangeDraft[axisKey] = next[axisKey];
      confirmBox.hidden = true;
      store.command({ op: 'room.setAutomationRange', xM: next.xM, yM: next.yM, zM: next.zM });
    });
    field.addEventListener('input', previewDraftBox);
    rangeInputs[key] = field;
    return field;
  };
  const axisRow = (axisLabel, axisKey, minKey, maxKey, gLo, gHi, minSpan) => element('div', { className: 'auto-range-axis' }, [
    element('span', { className: 'auto-range-label', text: axisLabel }),
    makeBoundField(minKey, `Automation ${axisLabel[0]} min, m`, ar[axisKey][0], gLo, gHi, minSpan, axisKey, 0),
    makeBoundField(maxKey, `Automation ${axisLabel[0]} max, m`, ar[axisKey][1], gLo, gHi, minSpan, axisKey, 1)
  ]);

  return {
    room: [slider('Ceiling height', s.heightM, 1, 30, 0.001, heightM => store.command({ op: 'room.setHeight', heightM }), 'm'),
      row('Floor', select('Floor material', materials, s.materials[s.floorMaterial]?.name, n => apply('floor', n))),
      row('Ceiling extent', select('Ceiling extent', [['walls','Within walls'],['infinite','Everywhere']], s.ceilingExtent ?? 'walls', ceilingExtent => store.command({op:'room.setHeight',ceilingExtent}))),
      row('Ceiling', select('Ceiling material', materials, s.materials[s.ceilingMaterial]?.name, n => apply('ceiling', n))),
      check('Open sky (no ceiling)', s.ceilingMaterial === null, open => store.command({ op: 'room.setOpening', ref: { kind: 'ceiling', index: 0 }, open })),
      check('Edge diffraction', s.diffraction, enabled => store.command({ op: 'room.setDiffraction', enabled }))],
    air: [slider('Temperature', s.tempC, -20, 45, 0.1, tempC => store.command({ op: 'room.setAir', tempC }), '°C'), slider('Humidity', s.humidityPct, 5, 100, 0.1, humidityPct => store.command({ op: 'room.setAir', humidityPct }), '%')],
    clutter: [...[['Count', 'count', 0, 80, 1], ['Min length', 'minLenM', 0.1, 3, 0.1], ['Max length', 'maxLenM', 0.2, 5, 0.1], ['Min height', 'minHeightM', 0.1, 3, 0.1], ['Max height', 'maxHeightM', 0.2, 4, 0.1], ['Min TL', 'tlMinDb', 0, 30, 0.1], ['Max TL', 'tlMaxDb', 0, 40, 0.1]]
      .map(([label, key, min, max, step]) => slider(label, s.clutter[key], min, max, step, n => store.command({ op: 'room.setClutter', [key]: n }), key.endsWith('M') ? 'm' : key.endsWith('Db') ? 'dB' : '')), seedRow, slider('Source clearance', s.clutter.minDistM, 0, 10, .01, minDistM => store.command({op:'room.setClutter',minDistM}), 'm')],
    roughness: [slider('Roughness', s.roughness, 0, 1, 0.001, roughness => store.command({ op: 'room.setRoughness', roughness }))],
    autoRangeHint: isDefRange ? '±200 m default' : `X ${Number(ar.xM[0].toFixed(1))}..${Number(ar.xM[1].toFixed(1))} m`,
    automationRange: [
      element('small', { className: 'note auto-range-note', text: 'Position automation bounds for s1x..s32z and lx..lz. Changing range rescales normalized DAW envelopes.' }),
      element('div', { className: 'auto-range-actions' }, [fitBtn, resetRangeBtn]),
      confirmBox,
      element('div', { className: 'auto-range-head' }, [element('span'), element('small', { text: 'Min, m' }), element('small', { text: 'Max, m' })]),
      axisRow('X, m', 'xM', 'xMin', 'xMax', -200, 200, 1),
      axisRow('Y, m', 'yM', 'yMin', 'yMax', -200, 200, 1),
      axisRow('Z, m', 'zM', 'zMin', 'zMax', 0, 30, 0.5),
      clampNote
    ],
    scale: [note('Scale geometry once; this is not an automated Size parameter.'), factorRow, include, button('Apply scale', () => {
      const n = Number(factor.value); if (Number.isFinite(n) && n >= 0.05 && n <= 20) store.command({ op: 'room.scale', factor: n, includeObjects: include.checked });
      else { factor.setCustomValidity('Enter 0.05…20'); factor.reportValidity(); }
    })]
  };
}
// ── Surface ------------------------------------------------------------------
export function surfaceParts(ctx, refs) {
  const { store } = ctx, state = store.state, s = state.document.scene, ref = refs[0], obj = surface(s, ref);
  if (!obj) return null;
  const mat = s.materials[obj.material], cleanRefs = refs.map(({ kind, index }) => ({ kind, index })), command = (op, patch) => store.command({ op, refs: cleanRefs, ...patch });
  const out = { ref, obj, mat, dimensions: [], surface: [], material: [], scatter: [], heights: [] };
  const wallSummary = wallSelectionSummary(s, refs);
  if (wallSummary) {
    out.dimensions.push(
      row(wallSummary.lengthLabel, element('span', { className: 'wall-length-readout', 'data-wall-length': wallSummary.lengthM.toFixed(4), 'data-wall-count': String(wallSummary.count), text: wallSummary.lengthText })),
      row('Height', element('span', { className: 'wall-height-readout', 'data-wall-height': wallSummary.heightSpanM.toFixed(4), text: wallSummary.heightText }))
    );
  }

  out.surface.push(
    row('Name', element('input', { type: 'text', value: obj.editor.name, maxLength: 128, 'aria-label': 'Surface name', 'data-focus-key': 'surface-name', onchange: e => command('surface.setMetadata', { name: e.target.value }) })),
    element('div', { className: 'inline' }, [check('Hidden', obj.editor.hidden, hidden => command('surface.setMetadata', { hidden })), check('Locked', obj.editor.locked, locked => command('surface.setMetadata', { locked }))]),
    row('Material class', mat ? `${MATERIAL_CLASSES[mat.classId]} · read only` : 'Opening'));
  if (mat) {
    out.material.push(materialPicker(ctx, mat.name, preset => command('surface.applyPreset', { preset })), bandEditor(store, refs, mat));
    const key = `${mat.name}:${mat.classId}`;
    const chosen = state.preferences?.materialColours?.[key], setColours = materialColours => store.command({ op: 'preferences.set', patch: { materialColours } });
    out.material.push(row('Display colour', element('div', { className: 'inline' }, [
      element('input', { type: 'color', className: 'swatch', value: materialColour(state.preferences || {}, mat, undefined, surfaceRole(ref, obj)), 'aria-label': 'Material display colour', title: 'Your colour always wins over the viewport theme', onchange: e => setColours({ ...state.preferences?.materialColours, [key]: e.target.value }) }),
      button('Auto', () => { const { [key]: _drop, ...rest } = state.preferences?.materialColours || {}; setColours(rest); }, { disabled: !chosen, title: 'Back to the theme / class colour' })])));
    out.scatter.push(element('div', { className: 'surface-pair' }, [slider('Scatter LF', mat.scatterLF, 0, 1, 0.001, scatterLF => command('surface.setScatter', { scatterLF })), slider('HF', mat.scatterHF ?? mat.scatterLF, 0, 1, 0.001, scatterHF => command('surface.setScatter', { scatterHF }))]),
      check('Inherit LF scatter', mat.scatterHF === null, inherit => command('surface.setScatter', { scatterHF: inherit ? null : mat.scatterLF })));
    [[0, 1], [2, 3], [4, 5, 6]].forEach((bands, g) => {
      const tau = bands.reduce((n, i) => n + mat.tau[i], 0) / bands.length, tl = tau > 0 ? -10 * Math.log10(tau) : 60;
      out.scatter.push(slider(['TL 125–250', 'TL 500–1k', 'TL 2k–8k'][g], Math.min(60, tl), 0, 60, 0.1, n => command('surface.setTransmission', { bands: bands.map(band => ({ band, tau: n >= 60 ? 0 : Math.pow(10, -n / 10) })) }), 'dB'));
    });
    out.scatter.push(note('60 dB = opaque (wire tau=0). α + τ never exceeds 1.'));
  }
  if (refs.length===1 && ref.kind === 'wall') out.heights.push(slider('Bottom', obj.zBottomM, 0, 30, 0.001, zBottomM => command('surface.setHeights', { zBottomM }), 'm'),
    slider('Top', obj.zTopM ?? s.heightM, 0, 30, 0.001, zTopM => command('surface.setHeights', { zTopM }), 'm'), check('Full room height', obj.zTopM === null, full => command('surface.setHeights', { zTopM: full ? null : s.heightM })));
  if (refs.length===1 && obj.vertices) out.heights.push(slider('Plate height', obj.heightM ?? 0, 0, 30, 0.001, heightM => command('surface.setHeights', { heightM: heightM === 0 ? null : heightM }), 'm'));
  if (refs.length===1 && ref.kind.startsWith('ceiling')) out.heights.push(check('Opening', obj.material === null, open => store.command({ op: 'room.setOpening', ref: { kind: ref.kind, index: ref.index }, open })));
  if(refs.length>1){
    const objects=refs.map(r=>surface(s,r)),materials=objects.map(o=>s.materials[o.material]);
    const mixed=values=>values.some(v=>JSON.stringify(v)!==JSON.stringify(values[0]));
    const mark=(node)=>{if(!node)return;node.value='';node.placeholder='Mixed';node.setAttribute('aria-description','Mixed values in selection');};
    const nameRow = out.surface.find(r => r.querySelector?.('[data-focus-key="surface-name"]'));
    const switchRow = out.surface.find(r => r.querySelector?.('[role=switch]'));
    if(mixed(objects.map(o=>o.editor.name)))mark(nameRow?.querySelector('input'));
    for(const [i,key] of ['hidden','locked'].entries())if(mixed(objects.map(o=>o.editor[key])))switchRow?.querySelectorAll('[role=switch]')[i]?.setAttribute('aria-checked','mixed');
    const mixedMaterials=mixed(materials);
    if(mixedMaterials)out.material.unshift(note('Mixed materials · assigning a preset applies to every selected surface.'));
    const allMat=materials.every(Boolean);
    if(allMat){
      for(let b=0;b<7;b++)if(mixed(materials.map(m=>m.alpha[b]))){for(const node of out.material)mark(node.querySelector(`[data-focus-key="alpha-${b}"]`));}
      if(mixed(materials.map(m=>m.scatterLF)))mark(out.scatter[0]?.querySelector('.readout'));
      if(mixed(materials.map(m=>m.scatterHF)))mark(out.scatter[0]?.querySelectorAll('.readout')[1]);
    }
    out.heights.push(note('Heights: '+(mixed(objects.map(o=>[o.zBottomM,o.zTopM,o.heightM]))?'Mixed':'Common')+' · edit a single surface to change height.'));
    out.surface.push(element('div',{className:'inline'},[button('Link',()=>store.command({op:'surface.link',refs:cleanRefs.filter(r=>!['floor','ceiling'].includes(r.kind)),groupName:'Construction'})),button('Unlink',()=>command('surface.unlink',{}))]));
  }
  return out;
}
export const sceneFacts = ctx => {
  const { store } = ctx, state = store.state, usage = budget(state.document);
  const effScene = viewportState(state, false).document.scene, metrics = computeRoomMetrics(effScene);
  return [row('Sources', `${state.document.sources.length} / ${state.limits.maxSources}`), row('Points', `${usage.points} / ${state.limits.maxPoints}`), row('Walls', `${state.document.scene.walls.length} / ${state.limits.maxWalls}`),
    row('Output', LAYOUTS[state.document.output.layout + 1]), row('Microphones', MICS[state.document.output.micPattern]), row('Mode', state.parameters.effective.mode),
    row('L × W × H', metrics.dimensionsText), row('Floor area', metrics.areaText), row('Volume', metrics.volumeText)];
};
export { group };

// E19: one Orientation setting per object (replaces the boolean "Follow direction"). Manual = azimuth by hand / automation;
// Along path = path heading; Look at = face another object or a fixed point. The Azimuth control stays active and is the offset (90 = side-on).
export function orientationControls(ctx, object) {
  const { store } = ctx;
  const p = store.state.document.paths?.[object] || {};
  const mode = orientationMode(p);
  const sources = store.state.document.sources;
  const objLabel = o => o === 32 ? 'Listener' : (sources[o]?.name || `Source ${o + 1}`);
  const send = patch => store.command({ op: 'path.setFlags', object, ...patch });
  const rows = [row('Orientation', select('Orientation', [['manual', 'Manual'], ['along', 'Along path'], ['lookAt', 'Look at']], mode, v => send({ orientMode: v }),
    { 'data-orientation-mode': String(object), title: 'Manual: the Azimuth control sets the direction and can be automated. Baked tails use the angles captured when baked. · Along path: turn with the path direction · Look at: face another object or a point · Azimuth is added as an offset in both' }))];
  if (mode === 'lookAt') {
    const targets = [['', 'Choose target…'], ...sources.map((_, s) => s).filter(s => s !== object).map(s => [s, objLabel(s)]), ...(object !== 32 ? [[32, 'Listener']] : []), ['point', 'Point…']];
    const current = Array.isArray(p.lookPos) ? 'point' : Number.isInteger(p.lookTarget) ? p.lookTarget : '';
    const prefix = object === 32 ? 'l' : `s${object + 1}`, v = store.state.parameters.values;
    const targetSel = select('Look at target', targets, current, val => {
      if (val === 'point') send({ lookPos: p.lookPos || [Number(((v[prefix + 'x'] ?? 0) + 1).toFixed(2)), v[prefix + 'y'] ?? 0, v[prefix + 'z'] ?? 1.5] });
      else if (val !== '') send({ lookTarget: Number(val) });
    }, { 'data-orientation-target': String(object) });
    rows.push(element('div', { className: 'orient-row' }, [targetSel]));
    if (Array.isArray(p.lookPos)) {
      const ar = sceneAutomationRange(store.state.document.scene);
      const axis = (i, l) => {
        const b = [ar.xM, ar.yM, ar.zM][i];
        return numericField(`Look at ${l}`, p.lookPos[i], Math.min(b[0], p.lookPos[i]), Math.max(b[1], p.lookPos[i]), 0.1, n => { const next = p.lookPos.slice(); next[i] = n; send({ lookPos: next }); });
      };
      rows.push(element('div', { className: 'orient-xyz' }, ['X', 'Y', 'Z'].map((l, i) => element('label', {}, [element('span', { text: l }), axis(i, l)]))));
    }
  }
  if (mode === 'along') {
    const motionPath = isFollower(p) ? resolveFollowPath(store.state.document.paths, object) : p;
    if (!motionPath?.enabled || (motionPath.points?.length ?? 0) < 2)
      rows.push(element('small', { className: 'note', text: 'Needs an enabled path to take effect' }));
  }
  if ((p.points?.length ?? 0) >= 2) {
    const azCount = Array.isArray(p.azKeys) ? p.azKeys.length : 0;
    const focusStrip = button('Timeline', () => store.setView({
      selection: [object === 32 ? { kind: 'listener', index: 0 } : { kind: 'source', index: object }],
      timelineOpen: true,
      timelineMin: false,
      timelineFocusAz: object
    }, false), {
      className: 'az-keys-focus',
      'data-az-focus': String(object),
      'aria-label': 'Focus azimuth strip in timeline',
      title: 'Open Path timeline and focus this Azimuth strip'
    });
    rows.push(element('div', { className: `az-keys-row${mode !== 'manual' ? ' dim' : ''}`, 'data-az-keys-info': String(object) }, [
      element('span', { className: 'az-keys-label', text: `Azimuth keys: ${azCount}` }),
      focusStrip
    ]));
    if (mode !== 'manual') {
      rows.push(element('small', {
        className: 'note az-keys-dim-hint',
        text: mode === 'along' ? 'Angle is driven by Along path (keys kept)' : 'Angle is driven by Look at (keys kept)'
      }));
    }
  }
  return rows;
}
export function pathControls(ctx, object, { collapsible = false } = {}) {
  const { store, param } = ctx;
  const p = store.state.document.paths?.[object];
  if (isFollower(p)) return followerPathControls(ctx, object, { collapsible });  // V04d: linked to another source's path
  const hasPath = (p?.points?.length ?? 0) >= 2;
  const drawing = store.view.pathDrawing === object;
  const editing = store.view.pathEditing === object;
  const selectedPt = store.view.pathPoint?.object === object ? store.view.pathPoint.index : -1;
  const header = collapsible ? null : element('div', { className: 'path-head' }, [
    heading('Path'),
    ...(hasPath ? [element('small', { className: 'path-meta', text: `Length ${compilePath(p).length.toFixed(2)} m · ${p.points.length} pts` })] : [])
  ]);
  const drawBtn = button(drawing ? 'Drawing… (Finish)' : (hasPath ? 'Redraw' : 'Draw path'), () => {
    if (drawing) {
      const pts = store.view.pathDraft;
      if (pts?.length >= 2) store.command({ op: 'path.set', object, points: pts, enabled: true });
      store.setView({ pathDrawing: null, pathDraft: null, pathRedo: null });
    } else {
      store.setView({
        pathDrawing: object,
        pathDraft: null,
        pathRedo: null,
        pathEditing: null,
        pathShapePreview: store.view.pathShapePreview?.object === object ? null : store.view.pathShapePreview,
        pathOldGhost: store.view.pathOldGhost?.object === object ? null : store.view.pathOldGhost
      });
    }
  }, {
    className: `path-draw-btn${drawing ? ' active is-drawing' : ''}`,
    'aria-label': 'Draw path',
    'aria-pressed': String(drawing),
    title: 'Click viewport points to trace a path · Drag node to move · Ctrl+Z/Y point · Enter, double-click or button finishes · Esc cancels'
  });
  const actions = [drawBtn];
  if (object !== 32) actions.push(linkButton(ctx, object));  // V04d: Link to path… (rig)
  if (hasPath) {
    actions.push(button(editing ? 'Editing' : 'Edit', () => store.setView({
      pathEditing: editing ? null : object,
      pathDrawing: null,
      pathDraft: null,
      pathRedo: null
    }), {
      className: editing ? 'active' : '',
      'aria-label': 'Edit path',
      'aria-pressed': String(editing),
      title: 'Drag points · drag curve to insert · click empty space to append · right-click or Del removes a point'
    }));
    actions.push(button('', () => store.setView({ timelineOpen: !store.view.timelineOpen }, false), {
      className: `path-timeline-btn${store.view.timelineOpen ? ' active' : ''}`, 'aria-label': 'Path timeline', 'aria-pressed': String(!!store.view.timelineOpen),
      title: 'Open the timeline: Start / End, key times and easing of every moving object on one Position axis'
    }));
    actions.push(button('Clear', () => {
      store.setView({
        pathDrawing: store.view.pathDrawing === object ? null : store.view.pathDrawing,
        pathDraft: store.view.pathDrawing === object ? null : store.view.pathDraft,
        pathRedo: store.view.pathDrawing === object ? null : store.view.pathRedo,
        pathEditing: store.view.pathEditing === object ? null : store.view.pathEditing,
        pathPoint: store.view.pathPoint?.object === object ? null : store.view.pathPoint,
        pathShapePreview: store.view.pathShapePreview?.object === object ? null : store.view.pathShapePreview,
        pathOldGhost: store.view.pathOldGhost?.object === object ? null : store.view.pathOldGhost
      });
      store.command({ op: 'path.clear', object });
    }, { 'aria-label': 'Clear path', title: 'Remove the movement path and restore static XYZ' }));
  }
  const block = element('div', { className: 'path-section', 'data-path-object': String(object) }, [
    ...(header ? [header] : []),
    element('div', { className: 'path-actions' }, actions),
    ...[linkPicker(ctx, object), followedNote(ctx, object)].filter(Boolean)
  ]);

  // V04a / V04a-2: Closed path shape generator with live parameters and old-shape ghost overlay
  const rememberedShapes = store.view.pathShapeLast || {};
  const activePreview = store.view.pathShapePreview?.object === object ? store.view.pathShapePreview.params : null;
  let shapeParams = activePreview ? { ...activePreview } : null;
  let shapeDisposers = [];
  const disposeShapeFields = () => {
    shapeDisposers.forEach(fn => fn?.());
    shapeDisposers = [];
  };
  const shapePanelWrap = element('div', { className: 'path-shape-wrap', 'data-shape-object': String(object) });
  shapePanelWrap.dispose = disposeShapeFields;
  const setPreview = nextParams => {
    shapeParams = nextParams ? { ...nextParams } : null;
    if (shapeParams) {
      store.view.pathShapeLast = { ...(store.view.pathShapeLast || {}), [object]: { ...shapeParams } };
      store.setView({ pathShapePreview: { object, params: { ...shapeParams } } }, false);
    } else if (store.view.pathShapePreview?.object === object) {
      store.setView({ pathShapePreview: null }, false);
    }
  };
  const captureOldGhostIfPresent = () => {
    const curPath = store.state.document.paths?.[object];
    if (curPath?.points?.length >= 2) {
      store.view.pathOldGhost = {
        object,
        points: curPath.points.map(q => q.slice()),
        closed: !!curPath.closed,
        smooth: !!curPath.smooth
      };
    }
  };
  const buildShapePathSetCmd = paramsToApply => {
    const applied = generatePathShapeForRoom(paramsToApply, store.state.document.scene, true);
    const curPath = store.state.document.paths?.[object];
    const pts = applied.path.points.map(q => q.map(v => Number(v.toFixed(4))));
    return {
      applied,
      cmd: {
        op: 'path.set',
        object,
        points: pts,
        enabled: true,
        closed: true,
        smooth: applied.path.smooth,
        ...(curPath?.endless ? { endless: true, endlessPeriodS: curPath.endlessPeriodS ?? 8 } : {}),
        ...(curPath?.wander && !isDefaultWander(curPath.wander) ? { wander: curPath.wander } : {})
      }
    };
  };
  const renderShapeEditor = () => {
    disposeShapeFields();
    shapePanelWrap.replaceChildren();
    const curKind = shapeParams?.kind || '';
    const shapeSelect = select('Path shape', PATH_SHAPE_OPTIONS, curKind, chosen => {
      if (!chosen) {
        setPreview(null);
        renderShapeEditor();
        return;
      }
      captureOldGhostIfPresent();
      const prev = shapeParams || rememberedShapes[object] || null;
      const next = defaultShapeParamsForObject(store.state, object, chosen, prev);
      setPreview(next);
      renderShapeEditor();
    }, {
      className: 'dd path-shape-select',
      'data-path-shape': String(object),
      title: 'Generate a ready-made closed trajectory shape (Circle, Ellipse, Figure-8, Square, Rectangle, Triangle, Star, Spiral)'
    });
    shapeDisposers.push(() => shapeSelect.dispose?.());
    const headControls = [shapeSelect];
    if (store.view.pathOldGhost?.object === object && store.view.pathOldGhost?.points?.length >= 2) {
      headControls.push(button('Ghost ×', () => {
        store.setView({ pathOldGhost: null }, false);
        renderShapeEditor();
      }, {
        className: 'path-ghost-dismiss',
        'data-dismiss-ghost': String(object),
        'aria-label': 'Dismiss previous shape ghost',
        title: 'Dismiss previous shape ghost overlay'
      }));
    }
    if (curKind) {
      headControls.push(button('×', () => {
        setPreview(null);
        renderShapeEditor();
      }, {
        className: 'path-shape-close',
        'aria-label': 'Close shape generator',
        title: 'Close shape preview'
      }));
    }
    const shapeHeadRow = Object.assign(row('Shape', element('div', { className: 'path-shape-head-ctrl' }, headControls)), {
      className: 'row path-shape-row',
      title: 'Generate a ready-made closed trajectory shape'
    });
    shapePanelWrap.append(shapeHeadRow);
    if (!curKind || !shapeParams) return;

    const meta = PATH_SHAPE_META[curKind] || PATH_SHAPE_META.circle;
    const warningEl = element('small', { className: 'note warning path-shape-warning', role: 'status', hidden: true });
    let fitRoomBtn = null;
    const syncRoomFitUi = () => {
      const fit = generatePathShapeForRoom(shapeParams, store.state.document.scene, false);
      if (fit.outsideRoom) {
        warningEl.hidden = false;
        warningEl.textContent = `Outside room bounds — Fit to room scales to ${Number(fit.fittedParams.radiusX.toFixed(2))} m (Apply clamps automatically).`;
        if (fitRoomBtn) fitRoomBtn.hidden = false;
      } else {
        warningEl.hidden = true;
        warningEl.textContent = '';
        if (fitRoomBtn) fitRoomBtn.hidden = true;
      }
      return fit;
    };
    const updateParamAndApplyLive = patch => {
      shapeParams = { ...shapeParams, ...patch };
      setPreview(shapeParams);
      syncRoomFitUi();
      store.command(buildShapePathSetCmd(shapeParams).cmd);
    };
    const makeShapeField = (label, key, val, min, max, step, patchFn, extraOpts = {}) => {
      let snapshotBeforeTx = null;
      const field = txNumericField({
        store,
        label,
        txLabel: `Edit ${label.toLowerCase()}`,
        value: val,
        min,
        max,
        step,
        liveInput: true,
        throttleMs: 0,
        ...extraOpts,
        onPreview: raw => {
          if (!snapshotBeforeTx) snapshotBeforeTx = { ...shapeParams };
          const patch = patchFn(raw);
          shapeParams = { ...shapeParams, ...patch };
          setPreview(shapeParams);
          syncRoomFitUi();
        },
        getCommitCmd: raw => {
          const patch = patchFn(raw);
          const nextParams = { ...shapeParams, ...patch };
          shapeParams = nextParams;
          setPreview(shapeParams);
          syncRoomFitUi();
          return buildShapePathSetCmd(nextParams).cmd;
        },
        onCancel: () => {
          if (snapshotBeforeTx) {
            shapeParams = { ...snapshotBeforeTx };
            snapshotBeforeTx = null;
            setPreview(shapeParams);
            syncRoomFitUi();
          }
        }
      });
      field.dataset.shapeField = key;
      shapeDisposers.push(() => field.dispose?.());
      return field;
    };

    const cxField = makeShapeField('Shape centre X, m', 'cx', shapeParams.cx, -200, 200, 0.1, cx => ({ cx: Number(cx.toFixed(4)) }));
    const cyField = makeShapeField('Shape centre Y, m', 'cy', shapeParams.cy, -200, 200, 0.1, cy => ({ cy: Number(cy.toFixed(4)) }));
    const centreLisBtn = button('On listener', () => {
      const next = centreShapeOnListener(store.state, shapeParams);
      updateParamAndApplyLive({ cx: next.cx, cy: next.cy });
      renderShapeEditor();
    }, {
      className: 'path-shape-centre-lis',
      'aria-label': 'Centre shape on listener',
      title: 'Set shape centre (X, Y) to the Listener position'
    });
    const centreRow = element('div', { className: 'path-shape-centre', title: 'Shape centre X / Y (m) in plan view' }, [
      element('span', { className: 'path-shape-label', text: 'Centre X/Y', title: 'Shape centre X / Y (m)' }),
      cxField,
      cyField,
      centreLisBtn
    ]);

    const rxField = makeShapeField('Shape radius, m', 'radiusX', shapeParams.radiusX, 0.05, 200, 0.1, rx => {
      const nextRx = Number(Math.max(0.05, rx).toFixed(4));
      return !meta.hasRadiusY && curKind === 'square' ? { radiusX: nextRx, radiusY: nextRx } : { radiusX: nextRx };
    });
    let ryField = null;
    if (meta.hasRadiusY) {
      ryField = makeShapeField('Shape secondary radius, m', 'radiusY', shapeParams.radiusY, 0.02, 200, 0.1, ry => ({
        radiusY: Number(Math.max(0.02, ry).toFixed(4))
      }));
    }
    const sizeGrid = element('div', { className: 'path-shape-grid' }, [
      element('label', { className: 'path-window-field', title: `${meta.radiusXLabel} (m)` }, [element('span', { text: `${meta.radiusXLabel} m` }), rxField]),
      ...(ryField ? [element('label', { className: 'path-window-field', title: `${meta.radiusYLabel} (m)` }, [element('span', { text: `${meta.radiusYLabel} m` }), ryField])] : [])
    ]);

    const zField = meta.isSpiral
      ? makeShapeField('Spiral start height Z, m', 'z', shapeParams.z, 0, 30, 0.1, z => ({ z: Number(z.toFixed(4)) }))
      : makeShapeField('Shape height Z, m', 'z', shapeParams.z, 0, 30, 0.1, z => ({ z: Number(z.toFixed(4)), zEnd: Number(z.toFixed(4)) }));
    let zEndField = null;
    if (meta.isSpiral) {
      const initZEnd = Number.isFinite(shapeParams.zEnd) ? shapeParams.zEnd : shapeParams.z;
      zEndField = makeShapeField('Spiral end height Z, m', 'zEnd', initZEnd, 0, 30, 0.1, zEnd => ({ zEnd: Number(zEnd.toFixed(4)) }));
    }
    const rotField = makeShapeField('Shape rotation, deg', 'rotationDeg', shapeParams.rotationDeg, -180, 180, 5, rotationDeg => ({ rotationDeg: Number(rotationDeg.toFixed(2)) }));
    const phaseField = makeShapeField('Shape start phase, deg', 'startPhaseDeg', shapeParams.startPhaseDeg, 0, 360, 15, startPhaseDeg => ({ startPhaseDeg: Number(startPhaseDeg.toFixed(2)) }));
    const poseGrid = element('div', { className: 'path-shape-grid' }, [
      ...(meta.isSpiral
        ? [
            element('label', { className: 'path-window-field', title: 'Spiral start height Z of first point (m)' }, [element('span', { text: 'Start Z m' }), zField]),
            element('label', { className: 'path-window-field', title: 'Spiral end height Z of last point (m)' }, [element('span', { text: 'End Z m' }), zEndField])
          ]
        : [
            element('label', { className: 'path-window-field', title: 'Height Z (m)' }, [element('span', { text: 'Z m' }), zField])
          ]),
      element('label', { className: 'path-window-field', title: 'Rotation angle in plan view (deg)' }, [element('span', { text: 'Rot °' }), rotField]),
      element('label', { className: 'path-window-field', title: 'Starting phase along the loop (0–360°)' }, [element('span', { text: 'Phase °' }), phaseField])
    ]);

    const dirSelect = select('Shape direction', [['ccw', 'CCW ↺'], ['cw', 'CW ↻']], shapeParams.clockwise ? 'cw' : 'ccw', dir => {
      updateParamAndApplyLive({ clockwise: dir === 'cw' });
    }, { className: 'dd path-shape-dir', title: 'Traversal direction: Counter-clockwise (CCW) or Clockwise (CW)' });
    shapeDisposers.push(() => dirSelect.dispose?.());

    const extraFields = [
      element('label', { className: 'path-window-field', title: 'Traversal direction (CCW / CW)' }, [element('span', { text: 'Dir' }), dirSelect])
    ];
    if (meta.hasNodes) {
      const nodesField = makeShapeField('Shape node count', 'nodes', shapeParams.nodes, meta.minNodes, meta.maxNodes, meta.evenNodes ? 2 : 1, rawNodes => {
        let n = Math.round(Math.max(meta.minNodes, Math.min(meta.maxNodes, rawNodes)));
        if (meta.evenNodes && n % 2 !== 0) n = n < meta.maxNodes ? n + 1 : meta.maxNodes;
        return { nodes: n };
      }, { display: n => String(Math.round(n)), edit: n => String(Math.round(n)) });
      extraFields.push(element('label', { className: 'path-window-field', title: `Control nodes (${meta.minNodes}–${meta.maxNodes})` }, [element('span', { text: 'Nodes' }), nodesField]));
    }
    if (meta.isSpiral) {
      const turnsField = makeShapeField('Spiral turns', 'spiralTurns', shapeParams.spiralTurns ?? 2, 1, 6, 1, t => {
        const spiralTurns = Math.round(Math.max(1, Math.min(6, t)));
        return { spiralTurns };
      }, { display: n => String(Math.round(n)), edit: n => String(Math.round(n)) });
      const modeSel = select('Spiral direction', [['outIn', 'In→Out→In'], ['inOut', 'Out→In→Out']], shapeParams.spiralOutIn !== false ? 'outIn' : 'inOut', v => {
        updateParamAndApplyLive({ spiralOutIn: v === 'outIn' });
      }, { className: 'dd', title: 'Closed spiral loop radial direction' });
      shapeDisposers.push(() => modeSel.dispose?.());
      extraFields.push(
        element('label', { className: 'path-window-field', title: 'Number of spiral turns (1–6)' }, [element('span', { text: 'Turns' }), turnsField]),
        element('label', { className: 'path-window-field', title: 'Closed spiral loop sense' }, [element('span', { text: 'Loop' }), modeSel])
      );
    }
    const extraGrid = element('div', { className: 'path-shape-grid' }, extraFields);

    fitRoomBtn = button('Fit to room', () => {
      const fit = generatePathShapeForRoom(shapeParams, store.state.document.scene, false);
      const fp = fit.fittedParams;
      updateParamAndApplyLive({
        cx: fp.cx,
        cy: fp.cy,
        z: fp.z,
        ...(Number.isFinite(fp.zEnd) ? { zEnd: fp.zEnd } : {}),
        radiusX: fp.radiusX,
        radiusY: fp.radiusY
      });
      renderShapeEditor();
    }, {
      className: 'path-shape-fit-btn',
      'aria-label': 'Fit shape to room',
      title: 'Scale and clamp shape parameters so the trajectory fits inside the room walls and height',
      hidden: true
    });

    const applyBtn = button('Apply', async () => {
      const { applied, cmd } = buildShapePathSetCmd(shapeParams);
      if (applied.outsideRoom) {
        const fp = applied.fittedParams;
        cxField.value = String(fp.cx);
        cyField.value = String(fp.cy);
        zField.value = String(fp.z);
        if (zEndField && Number.isFinite(fp.zEnd)) zEndField.value = String(fp.zEnd);
        rxField.value = String(fp.radiusX);
        if (ryField) ryField.value = String(fp.radiusY);
        shapeParams = {
          ...shapeParams,
          cx: fp.cx,
          cy: fp.cy,
          z: fp.z,
          ...(Number.isFinite(fp.zEnd) ? { zEnd: fp.zEnd } : {}),
          radiusX: fp.radiusX,
          radiusY: fp.radiusY
        };
        setPreview(shapeParams);
      }
      await store.command(cmd);
    }, {
      className: 'primary path-shape-apply-btn',
      'aria-label': 'Apply path shape',
      'data-apply-shape': String(object),
      title: 'Create or replace the path with this closed shape (1 undo step)'
    });

    const footerActions = element('div', { className: 'path-shape-actions' }, [fitRoomBtn, applyBtn]);
    const panel = element('div', { className: 'path-shape-panel stack', 'data-shape-kind': curKind }, [
      centreRow,
      sizeGrid,
      poseGrid,
      extraGrid,
      warningEl,
      footerActions
    ]);
    shapePanelWrap.append(panel);
    syncRoomFitUi();
  };
  renderShapeEditor();
  block.append(shapePanelWrap);
  const bakeStatus = element('small', { className: 'path-bake-status', 'aria-live': 'polite' });
  const bakeSource = element('small', { className: 'path-bake-source', 'aria-live': 'polite' });
  const recordingNote = element('small', { className: 'path-recording-note' });
  const afterStop = 'Recorded - now press Bake path to compute the tail';
  const bakeHelp = 'Pre-compute the late tail so it follows the movement before objects stop. Large rooms can take 1–2 minutes. Bake again after changing the room, paths, or materials.';
  const drawnFlow = 'For drawn paths in one group, press Bake path only.';
  const automationFlow = 'For host automation, press Record automation, play the movement once, press Stop, then press Bake path.';
  const recordHelp = `${automationFlow} Record again if the automation changes. ${drawnFlow}`;
  const bakeTarget = hasPath && p.enabled ? object : 'all';
  const bakeStatusObject = bakeTarget === 'all' ? -1 : object;
  const bakeButton = button('Bake path', () => {
    const v = pathBakeView(store.telemetry['status.load']?.pathBake, bakeStatusObject);
    store.command({ op: v.running ? 'path.bakeCancel' : 'path.bake', object: bakeTarget });
  }, { 'data-bake-path': String(object) });
  const recordButton = button('Record automation', () => {
    const s = store.telemetry['status.load']?.pathBake;
    store.command({ op: [1, 2].includes(s?.passState) ? 'path.recordStop' : 'path.recordPass' });
  }, { 'data-record-pass': String(object) });
  const clearRecording = button('Discard', () => store.command({ op: 'path.recordClear' }), {
    'data-clear-recording': String(object), 'aria-label': 'Discard recording', title: 'Stop and discard the recording and any bake made from it. Drawn paths and their bakes stay.'
  });
  const clearButton = button('Clear bake', () => store.command({ op: 'path.bakeClear' }), {
    'data-clear-bake': String(object), title: 'Discard all baked tables. Drawn paths and the recording stay.'
  });
  const updateAuto = () => {
    const pass = store.telemetry['status.load']?.pathBake;
    const samples = pass?.passSamples ?? 0;
    const recorded = [3, 4].includes(pass?.passState) && samples >= 2;
    const usesRecording = pass?.bakeSource === 'recorded';
    const readyToBake = recorded && usesRecording && !['baking', 'baked', 'approximate'].includes(pass.state);
    const [recordLabel, recordTitle] = pass?.passState === 1
      ? ['Armed - Stop', 'Recording starts when the host plays, and you can press Stop to cancel.']
      : pass?.passState === 2
        ? [`Recording ${samples} - Stop`, 'The movement is being recorded as the project plays, so press Stop when the movement ends.']
        : recorded
          ? [`Recorded ${samples} - Record again`, readyToBake
            ? `${samples} samples ready: press Bake path to compute the tail or Record again to replace this recording.`
            : `${samples} samples kept. Bake source shows what Bake path will use.`]
          : ['Record automation', recordHelp];
    recordButton.textContent = recordLabel;
    recordButton.title = recordTitle;
    clearRecording.disabled = !recorded && ![1, 2].includes(pass?.passState);
    recordingNote.textContent = pass?.recordingNote || '';
    recordingNote.hidden = !pass?.recordingNote;
  };
  const updateBakeStatus = () => {
    const pass = store.telemetry['status.load']?.pathBake;
    const samples = pass?.passSamples ?? 0;
    const recorded = [3, 4].includes(pass?.passState) && samples >= 2;
    const usesRecording = pass?.bakeSource === 'recorded';
    const readyToBake = recorded && usesRecording && !['baking', 'baked', 'approximate'].includes(pass.state);
    const v = pathBakeView(pass, bakeStatusObject);
    bakeStatus.textContent = readyToBake ? `${v.text} · ${afterStop}` : v.text;
    bakeStatus.classList.toggle('warn', v.warn || readyToBake);
    bakeSource.textContent = `Bake source: ${bakeSourceText(pass)}`;
    const creativeNow = (store.state.parameters?.values?.mode ?? 0) >= 0.5;
    const endlessBypass = hasPath && p.enabled && p.closed && p.endless;
    const wanderBypass = creativeNow && hasPath && p.enabled && !!p.wander?.enabled;
    bakeButton.classList.toggle('warn', !!v.warn);
    bakeButton.textContent = v.label;
    bakeButton.disabled = (motionEnabled(store.state) || endlessBypass || wanderBypass) && !v.running;
    bakeButton.title = bakeButton.disabled
      ? (motionEnabled(store.state) ? 'Turn off Motion to bake the path.'
        : endlessBypass && wanderBypass ? 'Turn off Endless and Path wander to bake the path.'
        : endlessBypass ? 'Turn off Endless to bake the path.'
        : 'Turn off Path wander (or switch to Physical) to bake the path.')
      : v.running ? 'Stop the current bake.' : `${bakeHelp} ${drawnFlow} ${automationFlow}`;
  };
  updateAuto();
  updateBakeStatus();
  const bakeRow = (caption, controls) => element('div', { className: 'path-bake-row' }, [
    caption ? element('small', { className: 'path-bake-caption' }, [caption]) : null,
    element('div', { className: 'path-bake-buttons' }, controls)
  ]);
  let bakeNodes = [];
  const motionBakeNote = () => {
    const creativeNow = (store.state.parameters?.values?.mode ?? 0) >= 0.5;
    const endlessBypass = hasPath && p.enabled && p.closed && p.endless;
    const wanderBypass = creativeNow && hasPath && p.enabled && !!p.wander?.enabled;
    if (motionEnabled(store.state)) return element('small', { className: 'note', text: 'Motion bypasses baked tables.' });
    if (endlessBypass && wanderBypass) return element('small', { className: 'note path-endless-bake-note', text: 'Endless and Path wander bypass baked tables.' });
    if (endlessBypass) return element('small', { className: 'note path-endless-bake-note', text: 'Endless bypasses baked tables.' });
    if (wanderBypass) return element('small', { className: 'note path-wander-bake-note', text: 'Path wander bypasses baked tables.' });
    return null;
  };
  if (collapsible) {
    const autoWrap = element('div', { className: 'path-bake-actions path-bake-sub' }, [
      bakeRow(null, [recordButton, clearRecording]),
      recordingNote
    ]);
    autoWrap.dispose = store.subscribe(({ reason }) => { if (reason === 'status.load') updateAuto(); });
    const bakeWrap = element('div', { className: 'path-bake-actions path-bake-sub' }, [
      bakeRow(null, [bakeButton, clearButton]),
      motionBakeNote(), bakeSource, bakeStatus
    ]);
    bakeWrap.dispose = store.subscribe(({ reason }) => { if (reason === 'status.load') updateBakeStatus(); });
    bakeNodes = [
      accordionSection(store, 'main.path.automation', {
        title: element('span', { className: 'path-bake-caption', text: 'Host automation' }),
        defaultOpen: true,
        nested: true,
        nodes: [autoWrap]
      }),
      accordionSection(store, 'main.path.bake', {
        title: element('span', { className: 'path-bake-caption', text: 'Bake late tail' }),
        defaultOpen: true,
        nested: true,
        nodes: [bakeWrap]
      })
    ].filter(Boolean);
  } else {
    const bakeActions = element('div', { className: 'path-bake-actions' }, [
      bakeRow('Host automation', [recordButton, clearRecording]),
      bakeRow('Bake late tail', [bakeButton, clearButton]),
      motionBakeNote(), bakeSource, recordingNote, bakeStatus
    ]);
    bakeActions.dispose = store.subscribe(({ reason }) => {
      if (reason === 'status.load') { updateAuto(); updateBakeStatus(); }
    });
    bakeNodes = [bakeActions];
  }
  if (!hasPath) { block.append(...bakeNodes); return [block]; }
  const flags = element('div', { className: 'path-flags' }, [
    ['enabled', 'Enabled', 'Follow this path during playback'],
    ['closed', 'Closed', 'Loop back from the last point to the first'],
    ['smooth', 'Smooth', 'Smooth centripetal curve through the points']
  ].map(([key, label, title]) => check(label, !!p[key], value => store.command({ op: 'path.setFlags', object, [key]: value }), { title })));
  const curGroup = Number.isInteger(p.group) && p.group >= 0 ? p.group : -1;
  const groupOpts = [[-1, 'Independent'], ...Array.from({ length: 8 }, (_, g) => [g, `Group ${g + 1}`])];
  if (curGroup >= 8) groupOpts.push([curGroup, `Group ${curGroup + 1}`]);
  const allPaths = store.state.document.paths || [];
  const driver = pathGroupDriver(allPaths, object);
  const members = curGroup >= 0 ? pathGroupMembers(allPaths, curGroup) : [];
  const objLabel = o => o === 32 ? 'Listener' : (store.state.document.sources[o]?.name || `Source ${o + 1}`);
  const groupRow = row('Coupling', select('Coupling group', groupOpts, curGroup, val => store.command({ op: 'path.setFlags', object, group: Number(val) }), { 'data-path-group': String(object) }));
  let groupNote = null;
  if (curGroup >= 0) {
    const text = !p.enabled ? `Group ${curGroup + 1} · enable path to couple`
      : members.length <= 1 ? `Group ${curGroup + 1} · assign another path to couple`
      : driver === object ? `Drives Group ${curGroup + 1}: ${members.map(objLabel).join(', ')}`
      : `Driven by ${objLabel(driver)} (Group ${curGroup + 1})`;
    groupNote = element('small', { className: 'note path-group-note', text });
  }
  const posLabel = driver !== object ? `${objLabel(driver)} Position` : (members.length > 1 ? `Position (Group ${curGroup + 1})` : 'Position');

  const creativeMode = (store.state.parameters?.values?.mode ?? 0) >= 0.5;
  const lfo = normalizeLfo(p.lfo);
  const wrapTooltip = 'Modulates path Position around its base value. Closed paths wrap around [0, 100%); open paths clamp at 0% and 100%. Active in Creative mode only.';
  const lfoCmd = patch => ({ op: 'path.setLfo', object, lfo: normalizeLfo({ ...lfo, ...patch }) });
  const lfoEnableChk = check('Path LFO', lfo.enabled, enabled => store.command(lfoCmd({ enabled })), {
    disabled: !creativeMode,
    'aria-label': 'Enable path LFO',
    title: wrapTooltip
  });
  const lfoShapeSel = select('LFO shape', LFO_SHAPES.map(s => [s.id, s.label]), lfo.shape, shape => store.command(lfoCmd({ shape })), {
    disabled: !creativeMode,
    title: 'Oscillator waveform'
  });
  const lfoDisposers = [() => lfoShapeSel.dispose?.()];
  const rateSlider = txSlider({
    store,
    label: 'LFO rate',
    txLabel: 'Edit path LFO rate',
    value: Math.max(0.05, Math.min(8, lfo.rateHz || 0.5)),
    min: 0.05,
    max: 8,
    step: 0.05,
    unit: 'Hz',
    disabled: !creativeMode,
    title: 'LFO frequency (0.05–8 Hz)',
    getCommitCmd: rateHz => lfoCmd({ rateHz })
  });
  const readBasePos01 = () => Math.max(0, Math.min(1, Number(store.state.parameters?.values?.[pathId(driver)] ?? 0)));
  let previewDepthPct = Math.round(lfo.depth * 100);
  const swingValEl = element('span', { className: 'path-lfo-swing' });
  const warnEl = element('small', {
    className: 'note path-lfo-warn',
    text: 'Clipped at the path end: open path clamps, closed path wraps'
  });
  let keepBtn = null;
  const refreshSwingUi = (depthPct = previewDepthPct) => {
    previewDepthPct = Math.max(0, Math.min(100, Number(depthPct) || 0));
    const sw = computePathLfoSwing(readBasePos01(), previewDepthPct / 100, !!p.closed);
    const warns = previewDepthPct > 1e-6 && sw.leavesEnds;
    swingValEl.textContent = sw.text;
    swingValEl.classList.toggle('warn', warns);
    warnEl.hidden = !warns;
    if (keepBtn) keepBtn.hidden = !warns;
    return sw;
  };
  keepBtn = button('Keep inside path', () => {
    const sw = computePathLfoSwing(readBasePos01(), previewDepthPct / 100, !!p.closed);
    store.command(lfoCmd({ depth: sw.safeDepth }));
  }, {
    className: 'path-lfo-keep-btn',
    disabled: !creativeMode,
    'aria-label': 'Clamp LFO depth to keep swing inside path 0 to 100 percent',
    'data-focus-key': `path-lfo-keep-${object}`,
    title: 'Clamp depth so Position ± depth stays inside 0–100 %'
  });

  const depthSlider = txSlider({
    store,
    label: 'LFO depth',
    txLabel: 'Edit path LFO depth',
    value: previewDepthPct,
    min: 0,
    max: 100,
    step: 1,
    unit: '%',
    disabled: !creativeMode,
    title: wrapTooltip,
    onPreview: pct => refreshSwingUi(pct),
    getCommitCmd: depthPct => lfoCmd({ depth: Number((depthPct / 100).toFixed(4)) })
  });
  const phaseSlider = txSlider({
    store,
    label: 'LFO phase',
    txLabel: 'Edit path LFO phase',
    value: Math.round(lfo.phaseDeg),
    min: 0,
    max: 360,
    step: 1,
    unit: '°',
    disabled: !creativeMode,
    title: 'Initial LFO phase (0–360°)',
    getCommitCmd: phaseDeg => lfoCmd({ phaseDeg })
  });
  refreshSwingUi(previewDepthPct);
  lfoDisposers.push(
    () => rateSlider.dispose?.(),
    () => depthSlider.dispose?.(),
    () => phaseSlider.dispose?.(),
    store.subscribe(({ reason }) => {
      if (reason === 'parameters') refreshSwingUi(previewDepthPct);
      else if (reason === 'patch' || reason === 'snapshot') {
        const curLfo = normalizeLfo(store.state?.document?.paths?.[object]?.lfo);
        refreshSwingUi(Math.round(curLfo.depth * 100));
      }
    })
  );

  const lfoBlock = element('div', { className: `path-lfo${creativeMode ? '' : ' is-physical'}`, title: wrapTooltip }, [
    element('div', { className: 'path-lfo-head' }, [
      ...(collapsible ? [] : [element('span', { className: 'path-lfo-title', text: 'Path LFO' })]),
      element('span', { className: `path-lfo-hint${creativeMode ? '' : ' warning'}`, text: 'Creative mode only' })
    ]),
    element('div', { className: 'path-lfo-top' }, [lfoEnableChk, lfoShapeSel]),
    element('div', { className: 'path-lfo-sliders' }, [rateSlider, depthSlider, phaseSlider]),
    element('div', { className: 'path-lfo-swing-row' }, [swingValEl, keepBtn]),
    warnEl,
    ...(driver !== object ? [element('small', { className: 'note path-lfo-note', text: `Coupled group drives motion using ${objLabel(driver)}'s LFO.` })] : [])
  ]);
  lfoBlock.dispose = () => lfoDisposers.forEach(fn => fn?.());

  const wander = normalizeWander(p.wander);
  const wanderTooltip = 'Deterministic smooth random transverse deviation across the path (horizontal & vertical). Creative mode only.';
  const wanderCmd = patch => ({ op: 'path.setWander', object, wander: normalizeWander({ ...wander, ...patch }) });
  const wanderEnableChk = check('Path wander', wander.enabled, enabled => store.command(wanderCmd({ enabled })), {
    disabled: !creativeMode,
    'aria-label': 'Enable path wander',
    'data-path-wander': String(object),
    title: wanderTooltip
  });
  const reseedBtn = button(`Reseed #${wander.seed}`, () => {
    const nextSeed = 1 + ((Math.floor(Math.random() * 999998) + wander.seed) % 999999);
    store.command(wanderCmd({ seed: nextSeed === wander.seed ? (wander.seed % 999999) + 1 : nextSeed }));
  }, {
    className: 'path-wander-reseed',
    disabled: !creativeMode,
    'aria-label': 'Reseed path wander',
    'data-path-wander-reseed': String(object),
    title: `Generate new deterministic seed (current #${wander.seed})`
  });
  const wanderReadoutEl = element('div', { className: 'path-wander-readout' });
  let previewWander = { ...wander };
  const refreshWanderReadout = (patch = {}) => {
    previewWander = { ...previewWander, ...patch };
    const corrS = previewWander.rateHz > 0 ? 1 / previewWander.rateHz : 0;
    const hText = `±${previewWander.ampHorizM.toFixed(2)} m H`;
    const vText = previewWander.horizOnly ? 'Z off' : `±${previewWander.ampVertM.toFixed(2)} m V`;
    const lapText = p.closed && p.endless && (p.endlessPeriodS ?? 8) > 0
      ? ` (~${(corrS / (p.endlessPeriodS ?? 8)).toFixed(2)} lap)`
      : '';
    wanderReadoutEl.textContent = `${hText} · ${vText} · corr ${corrS.toFixed(2)} s${lapText}`;
  };
  const wanderHorizSlider = txSlider({
    store,
    label: 'Horiz amp',
    txLabel: 'Edit path wander horizontal amplitude',
    value: Math.min(10, wander.ampHorizM),
    min: 0,
    max: 10,
    step: 0.05,
    unit: 'm',
    disabled: !creativeMode,
    title: 'Horizontal transverse RMS amplitude (m)',
    onPreview: ampHorizM => refreshWanderReadout({ ampHorizM }),
    getCommitCmd: ampHorizM => wanderCmd({ ampHorizM: Number(ampHorizM.toFixed(3)) })
  });
  const wanderVertSlider = txSlider({
    store,
    label: 'Vert amp',
    txLabel: 'Edit path wander vertical amplitude',
    value: Math.min(5, wander.ampVertM),
    min: 0,
    max: 5,
    step: 0.05,
    unit: 'm',
    disabled: !creativeMode || wander.horizOnly,
    title: 'Vertical (Z) RMS amplitude (m)',
    onPreview: ampVertM => refreshWanderReadout({ ampVertM }),
    getCommitCmd: ampVertM => wanderCmd({ ampVertM: Number(ampVertM.toFixed(3)) })
  });
  const wanderRateSlider = txSlider({
    store,
    label: 'Wander rate',
    txLabel: 'Edit path wander rate',
    value: wander.rateHz,
    min: 0.05,
    max: 8,
    step: 0.05,
    unit: 'Hz',
    disabled: !creativeMode,
    title: 'Correlation node rate (0.05–8 Hz; lower = slower drift)',
    onPreview: rateHz => refreshWanderReadout({ rateHz }),
    getCommitCmd: rateHz => wanderCmd({ rateHz: Number(rateHz.toFixed(3)) })
  });
  const wanderSmoothSlider = txSlider({
    store,
    label: 'Smoothness',
    txLabel: 'Edit path wander smoothness',
    value: Math.round(wander.smoothness * 100),
    min: 0,
    max: 100,
    step: 1,
    unit: '%',
    disabled: !creativeMode,
    title: 'Spline smoothness (0% C1 Catmull-Rom → 100% C2 cubic B-spline)',
    getCommitCmd: pct => wanderCmd({ smoothness: Number((pct / 100).toFixed(3)) })
  });
  const pinEndsChk = check('Pin ends', wander.pinEnds, pinEnds => store.command(wanderCmd({ pinEnds })), {
    disabled: !creativeMode || !!p.closed,
    'aria-label': 'Pin path wander ends',
    title: p.closed ? 'Open paths only (closed paths interpolate continuously across closure)' : 'Taper wander offset to zero at path start and end'
  });
  const horizOnlyChk = check('Horiz only', wander.horizOnly, horizOnly => store.command(wanderCmd({ horizOnly })), {
    disabled: !creativeMode,
    'aria-label': 'Horizontal only path wander',
    title: 'Disable vertical Z displacement while preserving Vert amp setting'
  });
  refreshWanderReadout();
  const wanderBlock = element('div', { className: `path-lfo path-wander${creativeMode ? '' : ' is-physical'}`, title: wanderTooltip }, [
    element('div', { className: 'path-lfo-head' }, [
      ...(collapsible ? [] : [element('span', { className: 'path-lfo-title', text: 'Path wander' })]),
      element('span', { className: `path-lfo-hint${creativeMode ? '' : ' warning'}`, text: 'Creative mode only' })
    ]),
    element('div', { className: 'path-wander-top' }, [wanderEnableChk, reseedBtn]),
    element('div', { className: 'path-lfo-sliders' }, [wanderHorizSlider, wanderVertSlider, wanderRateSlider, wanderSmoothSlider]),
    element('div', { className: 'path-wander-flags' }, [pinEndsChk, horizOnlyChk]),
    wanderReadoutEl
  ]);
  wanderBlock.dispose = () => {
    wanderHorizSlider.dispose?.();
    wanderVertSlider.dispose?.();
    wanderRateSlider.dispose?.();
    wanderSmoothSlider.dispose?.();
  };

  if (!collapsible) block.append(flags, groupRow, ...(groupNote ? [groupNote] : []), param(pathId(driver), { label: posLabel }));
  else block.append(flags, param(pathId(driver), { label: posLabel }));

  const curStart = p.start ?? 0, curEnd = p.end ?? 100;
  const startField = numericField('Path start %', curStart, 0, 99.99, 0.5, raw => {
    const start = Number(Math.max(0, Math.min(curEnd - MIN_WINDOW_GAP, raw)).toFixed(4));
    store.command({ op: 'path.setFlags', object, start });
  });
  const endField = numericField('Path end %', curEnd, 0.01, 100, 0.5, raw => {
    const end = Number(Math.max(curStart + MIN_WINDOW_GAP, Math.min(100, raw)).toFixed(4));
    store.command({ op: 'path.setFlags', object, end });
  });
  const windowRow = element('div', { className: 'path-window' }, [
    element('label', { className: 'path-window-field' }, [element('span', { text: 'Start %' }), startField]),
    element('label', { className: 'path-window-field' }, [element('span', { text: 'End %' }), endField])
  ]);
  block.append(windowRow);

  if (p.closed) {
    const lapsField = numericField('Path laps', p.laps ?? 1, 1, 64, 1, laps => store.command({ op: 'path.setFlags', object, laps: Math.round(laps) }));
    if (p.endless) {
      lapsField.disabled = true;
      lapsField.setAttribute('aria-disabled', 'true');
      lapsField.title = 'Endless: position is driven by transport';
    }
    block.append(Object.assign(row('Laps', lapsField), {
      className: `row path-laps${p.endless ? ' is-disabled locked' : ''}`,
      title: p.endless ? 'Endless: position is driven by transport' : 'Number of circuits across the movement window'
    }));
    const isEndless = !!p.endless;
    const periodS = Number.isFinite(p.endlessPeriodS) && p.endlessPeriodS >= 0.25 ? p.endlessPeriodS : 8;
    const totalLen = compilePath(p).length;
    const speedMps = totalLen > 0 && periodS > 0 ? Number((totalLen / periodS).toFixed(2)) : 0;
    const endlessChk = check('Endless (∞)', isEndless, endless => {
      store.command({
        op: 'path.setFlags',
        object,
        endless,
        ...(endless && !Number.isFinite(p.endlessPeriodS) ? { endlessPeriodS: 8 } : {})
      });
    }, {
      'aria-label': 'Endless loop',
      'data-path-endless': String(object),
      title: 'Circulate continuously at constant speed while transport runs, starting at Position (no lap count)'
    });
    endlessChk.classList.add('path-endless-check');
    const endlessRow = element('div', { className: 'path-endless-row', title: 'Endless closed-loop circulation' }, [endlessChk]);
    if (isEndless) {
      const periodField = numericField('Endless period, s', Number(periodS.toFixed(2)), 0.25, 3600, 0.5, rawP => {
        const nextP = Number(Math.max(0.25, Math.min(3600, rawP)).toFixed(4));
        store.command({ op: 'path.setFlags', object, endlessPeriodS: nextP });
      });
      const speedField = numericField('Endless speed, m/s', speedMps, 0.01, 200, 0.1, rawV => {
        if (!(rawV > 0) || !(totalLen > 0)) return;
        const nextP = Number(Math.max(0.25, Math.min(3600, totalLen / rawV)).toFixed(4));
        store.command({ op: 'path.setFlags', object, endlessPeriodS: nextP });
      });
      endlessRow.append(
        element('label', { className: 'path-window-field path-endless-field', title: 'Seconds per lap (0.25–3600 s)' }, [element('span', { text: 'Period s' }), periodField]),
        element('label', { className: 'path-window-field path-endless-field', title: 'Constant circulation speed along path (m/s)' }, [element('span', { text: 'm/s' }), speedField])
      );
    }
    block.append(endlessRow);
    block.append(element('small', {
      className: 'note path-lap-note',
      text: isEndless
        ? `Endless: 1 lap per ${Number(periodS.toFixed(2))} s (${speedMps.toFixed(2)} m/s) from Position while transport runs`
        : 'Timing shapes overall progress before laps repeat'
    }));
  }

  if (collapsible) {
    const shapeLabel = LFO_SHAPES.find(s => s.id === lfo.shape)?.label || lfo.shape;
    const couplingNode = accordionSection(store, 'main.path.coupling', {
      title: 'Coupling / group',
      hint: curGroup >= 0 ? `Group ${curGroup + 1}` : '',
      defaultOpen: true,
      nested: true,
      nodes: [groupRow, ...(groupNote ? [groupNote] : [])]
    });
    const lfoNode = accordionSection(store, 'main.path.lfo', {
      title: 'Path LFO',
      hint: lfo.enabled ? shapeLabel : 'Off',
      defaultOpen: lfo.enabled,
      nested: true,
      nodes: [lfoBlock]
    });
    const wanderHint = !wander.enabled ? 'Off'
      : wander.horizOnly ? `±${wander.ampHorizM.toFixed(2)} m H`
      : `±${wander.ampHorizM.toFixed(2)}/${wander.ampVertM.toFixed(2)} m`;
    const wanderNode = accordionSection(store, 'main.path.wander', {
      title: 'Path wander',
      hint: wanderHint,
      defaultOpen: wander.enabled,
      nested: true,
      nodes: [wanderBlock]
    });
    block.append(...[couplingNode, lfoNode, wanderNode, ...bakeNodes].filter(Boolean));
  } else {
    block.append(lfoBlock, wanderBlock, ...bakeNodes);
  }

  let selectionPanel = null;  // timing box / hint: BELOW the point list so the rows never shift with the selection
  if (selectedPt >= 0 && selectedPt < p.points.length) {
    const compiled = compilePath(p);
    const pinnedStart = selectedPt === 0;
    const pinnedEnd = !p.closed && selectedPt === p.points.length - 1;
    const isPinned = pinnedStart || pinnedEnd;
    const rawTime = p.times?.[selectedPt];
    const isAuto = !isPinned && (rawTime === null || rawTime === undefined);
    const computedTime = compiled.computedTimes[selectedPt] ?? (pinnedEnd ? 100 : 0);
    const autoText = `auto (${Number(computedTime.toFixed(1))}%)`;
    const displayTime = isPinned ? (pinnedStart ? '0 (pinned)' : '100 (pinned)') : isAuto ? autoText : String(Number(rawTime.toFixed(3)));
    const timeInput = element('input', {
      type: 'text',
      className: 'readout path-pt-time-input',
      value: displayTime,
      placeholder: isAuto ? autoText : '0..100',
      disabled: isPinned,
      'aria-label': `Point ${selectedPt + 1} time %`,
      'data-focus-key': `path-pt-time-${object}-${selectedPt}`,
      title: isPinned ? 'First and open-last points are pinned to 0 / 100 % — use Path Start / End' : 'Set time % within window (clamped between neighbours), or clear for auto'
    });
    let timeCommitted = false;
    const submitTime = () => {
      if (isPinned || timeCommitted) return;
      timeCommitted = true;
      const raw = timeInput.value.trim();
      if (!raw || /^auto\b/i.test(raw)) {
        if (!isAuto) store.command({ op: 'path.setPointTiming', object, index: selectedPt, time: null });
        else timeInput.value = autoText;
        return;
      }
      const num = Number(raw.replace(/%/g, '').trim());
      if (!Number.isFinite(num)) { timeInput.value = displayTime; return; }
      const clamped = Number(clampPointTime(p, selectedPt, num).toFixed(4));
      timeInput.value = String(Number(clamped.toFixed(3)));
      if (isAuto || Math.abs(clamped - rawTime) > 1e-6)
        store.command({ op: 'path.setPointTiming', object, index: selectedPt, time: clamped });
    };
    timeInput.addEventListener('focus', () => {
      if (isPinned) return;
      timeCommitted = false;
      if (isAuto) timeInput.value = '';
      else timeInput.select();
    });
    timeInput.addEventListener('input', () => { timeCommitted = false; });
    timeInput.addEventListener('change', submitTime);
    timeInput.addEventListener('keydown', e => {
      if (e.key === 'Enter') { e.preventDefault(); submitTime(); timeInput.blur(); }
      if (e.key === 'Escape') { e.preventDefault(); timeCommitted = true; timeInput.value = displayTime; timeInput.blur(); }
    });
    timeInput.addEventListener('blur', () => {
      if (isPinned) return;
      if (isAuto && !timeInput.value.trim()) timeInput.value = autoText;
    });
    const autoBtn = button('Auto', () => store.command({ op: 'path.setPointTiming', object, index: selectedPt, time: null }), {
      className: 'path-pt-auto',
      disabled: isPinned || isAuto,
      'aria-label': 'Clear point time to auto',
      title: 'Reset point time to automatic arc-length interpolation'
    });
    const timingBox = element('div', { className: 'path-pt-timing', 'data-selected-point': String(selectedPt) }, [
      element('div', { className: 'path-pt-timing-head' }, [
        element('span', { text: `Point ${selectedPt + 1} timing` }),
        element('small', { text: isPinned ? 'Pinned' : isPointKey(p, selectedPt) ? 'Key' : 'Auto' })
      ]),
      element('div', { className: 'row' }, [
        element('span', { text: 'Time %' }),
        element('div', { className: 'path-pt-time-ctrl' }, [timeInput, autoBtn])
      ])
    ]);
    if (!pinnedEnd) {
      const curEase = p.easings?.[selectedPt] || 'linear';
      timingBox.append(row('Key easing', select('Point key easing', [
        ['linear', 'Linear'],
        ['easeIn', 'Ease in'],
        ['easeOut', 'Ease out'],
        ['easeInOut', 'Ease in-out']
      ], curEase, easing => {
        if (isAuto) {
          if (easing === 'linear') return;
          const time = Number(clampPointTime(p, selectedPt, computedTime).toFixed(4));
          store.command({ op: 'path.setPointTiming', object, index: selectedPt, time, easing });
        } else {
          store.command({ op: 'path.setPointTiming', object, index: selectedPt, easing });
        }
      }, { 'data-path-easing': String(selectedPt) })));
      if (isAuto) {
        timingBox.append(element('small', { className: 'note path-pt-ease-note', text: 'Setting an easing makes this point a key at its current time.' }));
      }
    } else {
      timingBox.append(element('small', { className: 'note path-pt-ease-note', text: 'End of path: no easing' }));
    }
    selectionPanel = timingBox;
  } else {
    selectionPanel = element('small', { className: 'note path-pt-hint', text: 'Click a point row above or in the viewport to edit Time % & easing' });
  }

  const ptsList = element('div', { className: 'path-points' });
  const arPts = sceneAutomationRange(store.state.document.scene);
  p.points.forEach((point, index) => {
    const latestPt = () => store.state.document.paths?.[object]?.points?.[index] || point;
    const xField = numericField(`Path point ${index + 1} X, m`, point[0], Math.min(arPts.xM[0], point[0]), Math.max(arPts.xM[1], point[0]), 0.01, x => {
      const cur = latestPt();
      store.command({ op: 'path.movePoint', object, index, point: [x, cur[1], cur[2]] });
    });
    const yField = numericField(`Path point ${index + 1} Y, m`, point[1], Math.min(arPts.yM[0], point[1]), Math.max(arPts.yM[1], point[1]), 0.01, y => {
      const cur = latestPt();
      store.command({ op: 'path.movePoint', object, index, point: [cur[0], y, cur[2]] });
    });
    const zField = numericField(`Path point ${index + 1} Z, m`, point[2], Math.min(arPts.zM[0], point[2]), Math.max(arPts.zM[1], point[2]), 0.01, z => {
      const cur = latestPt();
      store.command({ op: 'path.movePoint', object, index, point: [cur[0], cur[1], z] });
    });
    const coord = (axis, field) => element('label', { className: 'path-pt-coord', title: `Pt ${index + 1} ${axis}, m` }, [
      element('i', { className: 'path-pt-axis', text: axis }),
      field
    ]);
    const delBtn = button('×', () => store.command({ op: 'path.removePoint', object, index }), {
      className: 'path-pt-del',
      disabled: p.points.length <= 2,
      'aria-label': `Delete path point ${index + 1}`,
      title: p.points.length <= 2 ? 'A path needs at least 2 points — use Clear path' : `Remove point ${index + 1}`
    });
    const ptTime = p.times?.[index];
    const badge = (ptTime !== null && ptTime !== undefined)
      ? element('small', { className: 'path-pt-badge', text: ` @ ${Number(ptTime.toFixed(1))}%` })
      : null;
    ptsList.append(element('div', {
      className: `row path-pt-row${selectedPt === index ? ' is-selected-point' : ''}`,
      'data-point-index': String(index),
      onclick: e => { if (!e.target.closest('input,button')) store.setView({ pathPoint: { object, index, point } }); }
    }, [
      element('div', { className: 'path-pt-head' }, [
        element('span', { className: 'path-pt-label' }, [`Pt ${index + 1}`, ...(badge ? [badge] : [])]),
        delBtn
      ]),
      element('div', { className: 'path-pt-coords' }, [
        coord('X', xField),
        coord('Y', yField),
        coord('Z', zField)
      ])
    ]));
  });
  const defaultPointsOpen = p.points.length <= 8;
  const savedPointsOpen = store.view.pathPointsOpen?.[object];
  let isPointsOpen = typeof savedPointsOpen === 'boolean' ? savedPointsOpen : defaultPointsOpen;
  const ptsToggleBtn = element('button', {
    type: 'button',
    className: 'acc-head acc-nested-head path-points-toggle',
    'aria-expanded': String(isPointsOpen),
    'aria-label': `Path points (${p.points.length})`,
    'data-path-points-toggle': String(object),
    title: isPointsOpen ? 'Collapse XYZ point list' : 'Expand XYZ point list'
  }, [
    element('span', { className: 'acc-chevron', 'aria-hidden': 'true', text: isPointsOpen ? '▾' : '▸' }),
    element('span', { className: 'acc-title', text: `Points (${p.points.length})` }),
    element('small', { className: 'acc-hint', text: `${compilePath(p).length.toFixed(2)} m` })
  ]);
  const ptsBody = element('div', {
    className: 'acc-body path-points-body',
    hidden: !isPointsOpen
  }, [ptsList, ...(selectionPanel ? [selectionPanel] : [])]);
  const ptsSection = element('div', {
    className: `acc-section acc-nested path-points-section${isPointsOpen ? ' is-open' : ' is-collapsed'}`,
    'data-path-points-section': String(object),
    'data-points-count': String(p.points.length)
  }, [ptsToggleBtn, ptsBody]);
  ptsToggleBtn.addEventListener('click', () => {
    isPointsOpen = !isPointsOpen;
    store.view.pathPointsOpen = { ...(store.view.pathPointsOpen || {}), [object]: isPointsOpen };
    ptsSection.classList.toggle('is-open', isPointsOpen);
    ptsSection.classList.toggle('is-collapsed', !isPointsOpen);
    ptsToggleBtn.setAttribute('aria-expanded', String(isPointsOpen));
    ptsToggleBtn.title = isPointsOpen ? 'Collapse XYZ point list' : 'Expand XYZ point list';
    const chev = ptsToggleBtn.querySelector('.acc-chevron');
    if (chev) chev.textContent = isPointsOpen ? '▾' : '▸';
    ptsBody.hidden = !isPointsOpen;
  });
  block.append(ptsSection);
  return [block];
}
