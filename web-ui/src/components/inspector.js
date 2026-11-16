import { compilePath, pathId, pathGroupDriver, pathGroupMembers, MIN_WINDOW_GAP, isPointKey, clampPointTime, orientationMode } from '../viewport/movement-path.js';
import { pathBakeView } from './path-bake.js';
import { directivityShape } from '../viewport/marker.js';
import { element, button, select, check, row, note, unavailable } from './dom.js';
import { slider, numericField } from './slider.js';
import { FORMATS, LAYOUTS, MICS, channels, budget, surface } from '../state/selectors.js';
import { bandEditor } from './band-editor.js';
import { materialPicker, materialCatalogue, MATERIAL_CLASSES } from './material-picker.js';
import { materialColour, surfaceRole } from '../viewport/colour.js';
import { wallSelectionSummary, computeRoomMetrics } from '../viewport/room-metrics.js';
import { viewportState } from '../viewport/effective-scene.js';
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
    ? select('Microphone pattern', [[o.micPattern, ambiOrder === 1 ? 'Fixed by layout (W omni, Y/Z/X figure-8)' : 'Fixed by layout (SN3D spherical harmonics)']], o.micPattern, () => {}, { disabled: true, title: 'Fixed by layout for AmbiX (' + ['1st', '2nd', '3rd'][ambiOrder - 1] + ' order)' })
    : select('Microphone pattern', MICS, o.micPattern, n => command({ micPattern: Number(n) }));
  return {
    main: [row('Layout', select('Output layout', LAYOUTS.map((v, i) => [i - 1, v]), o.layout, n => command({ layout: Number(n) }))),
      row('Microphones', micControl),
      check('Show microphone array to scale', !!store.view.showMic, value => store.setView({ showMic: value })),
      slider('Radius', o.radiusM, 0, 1, 0.001, radiusM => command({ radiusM }), 'm', { locked: !!radiusReason, reason: radiusReason }),
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
export function sourceParts(ctx, index) {
  const { store, param } = ctx, state = store.state, source = state.document.sources[index];
  if (!source) return null;
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
  return {
    source,
    identity: [row('Name', name), row('Format', select('Source format', FORMATS, source.format, n => config({ format: Number(n) }))),
      slider('First input', source.firstIn + 1, 1, state.limits.maxInputs, 1, n => config({ firstIn: n - 1 })),
      source.format === 7 ? slider('Channels', source.channels, 2, state.limits.maxSourceChannels, 1, n => config({ channels: n })) : row('Channels', `${channels(source)} · In ${source.firstIn + 1}–${source.firstIn + channels(source)}`)],
    routing: [element('div', { className: 'check-pair' }, [check('Sum', source.sum, sum => config({ sum }), { title: 'Sum channels to one point' }), check('Free pts', source.freePoints, freePoints => config({ freePoints }), { disabled: source.sum, title: 'Free channel points' })]),
      slider('Spread', source.spreadM, 0, 20, 0.001, spreadM => store.command({ op: 'source.setSpread', index, spreadM }), 'm'),
      ...directivityControls],
    position: [param(`${prefix}x`, { label: 'X' }), param(`${prefix}y`, { label: 'Y' }), param(`${prefix}z`, { label: 'Z' }), param(`${prefix}az`, { label: 'Azimuth' }), ...orientationControls(ctx, index), ...pathControls(ctx,index)],
    level: [element('div', { className: 'gain-ms-row' }, [param(`${prefix}g`, { label: 'Gain' }), ms])],
    points, ms
  };
}
export function listenerParts(ctx) {
  const { param } = ctx; return { position: [...['lx', 'ly', 'lz', 'laz'].map((id, i) => param(id, { label: ['X', 'Y', 'Z', 'Yaw'][i] })), ...orientationControls(ctx, 32), ...pathControls(ctx,32)], output: outputParts(ctx) };
}
// ── Scene / room -----------------------------------------------------------
export function scenePartsRoom(ctx) {
  const { store } = ctx, s = store.state.document.scene;
  const materials = materialCatalogue(store).map(m => [m.name, m.name.replace(/_/g, ' ')]);
  const apply = (kind, preset) => store.command({ op: 'surface.applyPreset', refs: [{ kind, index: 0 }], preset });
  const factorRow = slider('Scale factor', 1, .05, 20, .01, () => {}, '×'), factor = factorRow.querySelector('.readout'), include = check('Also move sources, listener and height', false, () => {});
  factor.setAttribute('aria-label', 'Scale factor');
  factorRow.querySelector('.track').setAttribute('aria-label', 'Scale factor slider');
  return {
    room: [slider('Ceiling height', s.heightM, 1, 30, 0.001, heightM => store.command({ op: 'room.setHeight', heightM }), 'm'),
      row('Floor', select('Floor material', materials, s.materials[s.floorMaterial]?.name, n => apply('floor', n))),
      row('Ceiling extent', select('Ceiling extent', [['walls','Within walls'],['infinite','Everywhere']], s.ceilingExtent ?? 'walls', ceilingExtent => store.command({op:'room.setHeight',ceilingExtent}))),
      row('Ceiling', select('Ceiling material', materials, s.materials[s.ceilingMaterial]?.name, n => apply('ceiling', n))),
      check('Open sky (no ceiling)', s.ceilingMaterial === null, open => store.command({ op: 'room.setOpening', ref: { kind: 'ceiling', index: 0 }, open })),
      check('Edge diffraction', s.diffraction, enabled => store.command({ op: 'room.setDiffraction', enabled }))],
    air: [slider('Temperature', s.tempC, -20, 45, 0.1, tempC => store.command({ op: 'room.setAir', tempC }), '°C'), slider('Humidity', s.humidityPct, 5, 100, 0.1, humidityPct => store.command({ op: 'room.setAir', humidityPct }), '%')],
    clutter: [...[['Count', 'count', 0, 80, 1], ['Min length', 'minLenM', 0.1, 3, 0.1], ['Max length', 'maxLenM', 0.2, 5, 0.1], ['Min height', 'minHeightM', 0.1, 3, 0.1], ['Max height', 'maxHeightM', 0.2, 4, 0.1], ['Min TL', 'tlMinDb', 0, 30, 0.1], ['Max TL', 'tlMaxDb', 0, 40, 0.1], ['Seed', 'seed', 1, 9999, 1]]
      .map(([label, key, min, max, step]) => slider(label, s.clutter[key], min, max, step, n => store.command({ op: 'room.setClutter', [key]: n }), key.endsWith('M') ? 'm' : key.endsWith('Db') ? 'dB' : '')), slider('Source clearance', s.clutter.minDistM, 0, 10, .01, minDistM => store.command({op:'room.setClutter',minDistM}), 'm')],
    roughness: [slider('Roughness', s.roughness, 0, 1, 0.001, roughness => store.command({ op: 'room.setRoughness', roughness }))],
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
    { 'data-orientation-mode': String(object), title: 'Manual: set by hand or automation · Along path: turn with the path direction · Look at: face another object or a point · Azimuth is added as an offset in both' }))];
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
      const axis = (i, l) => numericField(`Look at ${l}`, p.lookPos[i], i === 2 ? 0 : -200, i === 2 ? 30 : 200, 0.1, n => { const next = p.lookPos.slice(); next[i] = n; send({ lookPos: next }); });
      rows.push(element('div', { className: 'orient-xyz' }, ['X', 'Y', 'Z'].map((l, i) => element('label', {}, [element('span', { text: l }), axis(i, l)]))));
    }
  }
  if (mode === 'along') {
    if (!((p.points?.length ?? 0) >= 2 && p.enabled)) rows.push(element('small', { className: 'note', text: 'Needs an enabled path to take effect' }));
  }
  return rows;
}
export function pathControls(ctx, object) {
  const { store, param } = ctx;
  const p = store.state.document.paths?.[object];
  const hasPath = (p?.points?.length ?? 0) >= 2;
  const drawing = store.view.pathDrawing === object;
  const editing = store.view.pathEditing === object;
  const selectedPt = store.view.pathPoint?.object === object ? store.view.pathPoint.index : -1;
  const header = element('div', { className: 'path-head' }, [
    heading('Path'),
    ...(hasPath ? [element('small', { className: 'path-meta', text: `Length ${compilePath(p).length.toFixed(2)} m · ${p.points.length} pts` })] : [])
  ]);
  const drawBtn = button(drawing ? 'Drawing… (Finish)' : (hasPath ? 'Redraw' : 'Draw path'), () => {
    if (drawing) {
      const pts = store.view.pathDraft;
      if (pts?.length >= 2) store.command({ op: 'path.set', object, points: pts, enabled: true });
      store.setView({ pathDrawing: null, pathDraft: null, pathRedo: null });
    } else {
      store.setView({ pathDrawing: object, pathDraft: null, pathRedo: null, pathEditing: null });
    }
  }, {
    className: `path-draw-btn${drawing ? ' active is-drawing' : ''}`,
    'aria-label': 'Draw path',
    'aria-pressed': String(drawing),
    title: 'Click viewport points to trace a path · Drag node to move · Ctrl+Z/Y point · Enter, double-click or button finishes · Esc cancels'
  });
  const actions = [drawBtn];
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
        pathPoint: store.view.pathPoint?.object === object ? null : store.view.pathPoint
      });
      store.command({ op: 'path.clear', object });
    }, { 'aria-label': 'Clear path', title: 'Remove the movement path and restore static XYZ' }));
  }
  const block = element('div', { className: 'path-section', 'data-path-object': String(object) }, [
    header,
    element('div', { className: 'path-actions' }, actions)
  ]);
  const bakeStatus = element('small', { className: 'path-bake-status', 'aria-live': 'polite' });
  const bakeTarget = hasPath ? object : 'all';
  const bakeButton = button('Bake path', () => {
    const v = pathBakeView(store.telemetry['status.load']?.pathBake, hasPath ? object : -1);
    store.command({ op: v.running ? 'path.bakeCancel' : 'path.bake', object: bakeTarget });
  }, { 'data-bake-path': String(object) });
  const recordButton = button('Record pass', () => {
    const s = store.telemetry['status.load']?.pathBake;
    store.command({ op: [1, 2].includes(s?.passState) ? 'path.recordStop' : 'path.recordPass' });
  }, { 'data-record-pass': String(object) });
  const clearButton = button('Clear bake', () => store.command({ op: 'path.bakeClear' }), {
    'data-clear-bake': String(object), title: 'Discard all baked tables (paths and recorded pass stay); bake again when needed'
  });
  const updateBake = () => {
    const pass = store.telemetry['status.load']?.pathBake;
    recordButton.textContent = pass?.passState === 1 ? 'Armed - Stop'
      : pass?.passState === 2 ? `Recording ${pass.passSamples} - Stop`
      : [3, 4].includes(pass?.passState) ? `Recorded ${pass.passSamples} - Record again`
      : 'Record pass';
    const v = pathBakeView(pass, hasPath ? object : -1);
    bakeStatus.textContent = v.text;
    bakeStatus.title = v.text;
    bakeStatus.classList.toggle('warn', !!v.warn);
    bakeButton.classList.toggle('warn', !!v.warn);
    bakeButton.textContent = v.label;
  };
  updateBake();
  const bakeActions = element('div', { className: 'path-bake-actions' }, [recordButton, bakeButton, clearButton, bakeStatus]);
  block.dispose = store.subscribe(({ reason }) => { if (reason === 'status.load') updateBake(); });
  if (!hasPath) { block.append(bakeActions); return [block]; }
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
  block.append(flags, groupRow, ...(groupNote ? [groupNote] : []), param(pathId(driver), { label: posLabel }), bakeActions);

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
    block.append(Object.assign(row('Laps', numericField('Path laps', p.laps ?? 1, 1, 64, 1, laps => store.command({ op: 'path.setFlags', object, laps: Math.round(laps) }))), { className: 'row path-laps' }));
    block.append(element('small', { className: 'note path-lap-note', text: 'Timing shapes overall progress before laps repeat' }));
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
  p.points.forEach((point, index) => {
    const latestPt = () => store.state.document.paths?.[object]?.points?.[index] || point;
    const xField = numericField(`Path point ${index + 1} X, m`, point[0], -200, 200, 0.01, x => {
      const cur = latestPt();
      store.command({ op: 'path.movePoint', object, index, point: [x, cur[1], cur[2]] });
    });
    const yField = numericField(`Path point ${index + 1} Y, m`, point[1], -200, 200, 0.01, y => {
      const cur = latestPt();
      store.command({ op: 'path.movePoint', object, index, point: [cur[0], y, cur[2]] });
    });
    const zField = numericField(`Path point ${index + 1} Z, m`, point[2], 0, 30, 0.01, z => {
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
  block.append(ptsList);
  if (selectionPanel) block.append(selectionPanel);
  return [block];
}
