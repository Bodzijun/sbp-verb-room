import { element, check, note, row } from './dom.js';
import { BANDS } from '../state/selectors.js';
const colours = ['#dcaa7e', '#d9ba89', '#c3bd80', '#a5bd9c', '#80bbbd', '#a49bd0', '#c695bd'];
const MAX_HORIZON_S = 20;
const MIN_HORIZON_S = 0.5;
const TICK_STEPS_S = [0.1, 0.2, 0.25, 0.5, 1, 2, 2.5, 5];

function bandReach60Time(values, rt60, durationS) {
  let reach = Number.isFinite(rt60) && rt60 > 0 ? rt60 : 0;
  if (Array.isArray(values) && values.length > 1 && Number.isFinite(durationS) && durationS > 0) {
    const n = values.length;
    for (let i = 0; i < n; i++) {
      if (values[i] <= -60) {
        if (i === 0) { reach = Math.max(reach, 0); break; }
        const prev = values[i - 1], curr = values[i];
        const frac = prev > curr ? (prev - (-60)) / (prev - curr) : 1;
        const tCross = ((i - 1 + Math.max(0, Math.min(1, frac))) / (n - 1)) * durationS;
        reach = Math.max(reach, tCross);
        break;
      }
      if (i === n - 1 && values[i] > -60 && Number.isFinite(rt60) && rt60 > 0) {
        const extra = ((values[i] - (-60)) / 60) * rt60;
        reach = Math.max(reach, durationS + extra);
      }
    }
  }
  return reach;
}

export function chooseDecayTimeAxis(d, visibleBands = [true, true, true, true, true, true, true]) {
  if (!d) return { maxTimeS: 5, stepS: 1, ticks: [0, 1, 2, 3, 4, 5] };
  const durationS = Number.isFinite(d.durationS) && d.durationS > 0 ? d.durationS : 5;
  let required = 0;
  for (let i = 0; i < 7; i++) {
    if (!visibleBands?.[i] || !d.active?.[i]) continue;
    required = Math.max(
      required,
      bandReach60Time(d.decayDb?.[i], d.rt60?.[i], durationS),
      bandReach60Time(d.physicalDecayDb?.[i], d.physicalRT60?.[i], durationS)
    );
  }
  if (required <= 0) required = Math.min(MAX_HORIZON_S, Math.max(MIN_HORIZON_S, durationS));
  const target = Math.min(MAX_HORIZON_S, Math.max(MIN_HORIZON_S, required * 1.02));
  let stepS = TICK_STEPS_S[TICK_STEPS_S.length - 1];
  for (const s of TICK_STEPS_S) {
    const count = Math.ceil(target / s - 1e-9);
    if (count >= 2 && count <= 6) { stepS = s; break; }
  }
  const count = Math.max(2, Math.ceil(target / stepS - 1e-9));
  const maxTimeS = Number((count * stepS).toFixed(6));
  const ticks = Array.from({ length: count + 1 }, (_, i) => Number((i * stepS).toFixed(6)));
  return { maxTimeS, stepS, ticks };
}

function formatTickS(t, stepS) {
  if (t === 0) return '0s';
  const decimals = Number.isInteger(t) ? 0 : Number.isInteger(t * 10) ? 1 : 2;
  return `${Number(t.toFixed(decimals))}s`;
}

export function decayPanel(ctx) {
  const { store, param } = ctx;
  const root = element('div', { className: 'decay-panel' });
  const graph = element('div', { className: 'decay-graph' });
  const legend = element('div', { className: 'inline octave-legend' }, BANDS.map((b, i) => { const item = check(b >= 1000 ? `${b / 1000}k` : `${b}`, store.view.bands[i], visible => { const bands = store.view.bands.slice(); bands[i] = visible; store.setView({ bands }); }); item.style.setProperty('--band-colour', colours[i]); item.title = `${b} Hz`; return item; }));
  const canvas = element('canvas', { 'aria-label': `Seven octave tail decay curves${ctx.bridge.mode === 'mock' ? '; synthetic mock telemetry' : '; engine model'}` });
  graph.append(legend, canvas, element('span', { className: 'updating-label', text: 'Updating…' }), note(`${ctx.bridge.mode === 'mock' ? 'SYNTHETIC · ' : ''}solid reliable / dashed extrapolated / faint physical`));
  const side = element('div', { className: 'decay-side scroll' }, [element('h3', { text: 'Decay by band' }), ...['decayLow', 'decayMid', 'decayHigh'].map((id, i) => param(id, { label: ['Low 125–250 Hz', 'Mid 500–2k Hz', 'High 4k–8k Hz'][i] })), element('details', { className: 'decay-effective' }, [element('summary', { text: 'Effective / diagnostics' }), element('div', { className: 'decay-diagnostics' })])]);
  root.append(graph, side);
  function draw() {
    const d = store.telemetry['diagnostics.tail'], rect = canvas.getBoundingClientRect(), w = Math.max(1, rect.width), h = Math.max(1, rect.height), dpr = window.devicePixelRatio || 1;
    canvas.width = w * dpr; canvas.height = h * dpr; const c = canvas.getContext('2d'); c.scale(dpr, dpr); c.clearRect(0, 0, w, h);
    const left = 32, top = 10, right = Math.max(left + 20, w - 18), bottom = Math.max(top + 20, h - 20);
    const { maxTimeS, stepS, ticks } = chooseDecayTimeAxis(d, store.view.bands);
    c.font = '11px Segoe UI, -apple-system, BlinkMacSystemFont, sans-serif';
    for (let db = 0; db >= -60; db -= 20) {
      const y = top + (-db / 60) * (bottom - top);
      c.strokeStyle = '#344047'; c.beginPath(); c.moveTo(left, y); c.lineTo(right, y); c.stroke();
      c.fillStyle = '#91a0a6'; c.textAlign = 'left'; c.fillText(`${db}`, 4, y + 3);
    }
    ticks.forEach((t, idx) => {
      const x = left + (t / maxTimeS) * (right - left);
      c.strokeStyle = 'rgba(52, 64, 71, 0.35)';
      if (t > 0) { c.beginPath(); c.moveTo(x, top); c.lineTo(x, bottom); c.stroke(); }
      c.fillStyle = '#91a0a6';
      c.textAlign = idx === 0 ? 'left' : idx === ticks.length - 1 ? 'right' : 'center';
      c.fillText(formatTickS(t, stepS), x, h - 4);
    });
    c.textAlign = 'left';
    if (!d) { c.fillText('Waiting for diagnostics', left + 20, top + 30); return; }
    const curve = (values, band, physical) => {
      if (!Array.isArray(values) || !values.length) return;
      c.save();
      c.beginPath(); c.rect(left, top, right - left, bottom - top); c.clip();
      c.strokeStyle = colours[band]; c.globalAlpha = physical ? 0.22 : 0.9; c.lineWidth = physical ? 1 : 1.4;
      let previousReliable = null;
      const n = values.length;
      values.forEach((db, i) => {
        const t = (i / Math.max(1, n - 1)) * d.durationS;
        const reliable = t <= d.reliableSeconds[band];
        const x = left + (t / maxTimeS) * (right - left);
        const y = top + (Math.max(0, Math.min(60, -db)) / 60) * (bottom - top);
        if (i === 0 || reliable !== previousReliable) {
          if (i) c.stroke();
          c.beginPath(); c.moveTo(x, y); c.setLineDash(reliable || physical ? [] : [4, 3]);
        } else c.lineTo(x, y);
        previousReliable = reliable;
      });
      c.stroke();
      const lastDb = values[n - 1];
      const rt60 = physical ? d.physicalRT60?.[band] : d.rt60?.[band];
      if (lastDb > -60 && Number.isFinite(rt60) && rt60 > 0 && d.durationS < maxTimeS) {
        const tEnd = d.durationS + ((lastDb - (-60)) / 60) * rt60;
        const x0 = left + (d.durationS / maxTimeS) * (right - left);
        const y0 = top + (Math.max(0, Math.min(60, -lastDb)) / 60) * (bottom - top);
        const x1 = left + (tEnd / maxTimeS) * (right - left);
        const y1 = bottom;
        c.beginPath(); c.setLineDash(physical ? [] : [4, 3]); c.moveTo(x0, y0); c.lineTo(x1, y1); c.stroke();
      }
      c.restore();
    };
    for (let i = 0; i < 7; i++) if (store.view.bands[i] && d.active[i]) { curve(d.physicalDecayDb[i], i, true); curve(d.decayDb[i], i, false); }
    const statuses = ['applied', 'no measurable decay → unity', 'below 12-bin resolution → unity', '20 s cap', 'fit rejected → unity'];
    side.querySelector('.decay-diagnostics').replaceChildren(...BANDS.map((b, i) => row(`${b} Hz`, `${Number(d.effectiveMultipliers[i].toFixed(2))} × · ${statuses[d.reshapeStatus[i]]}`)), note(`${d.raysUsed.toLocaleString()} / ${d.rayLimit.toLocaleString()} rays · ${d.shared ? 'shared' : 'per-source'}${d.needsMoreRays ? ' · sparse' : ''}`));
  }
  const observer = new ResizeObserver(draw); observer.observe(graph);
  const off = store.subscribe(({ reason }) => { if (['diagnostics.tail', 'view', 'parameters'].includes(reason)) draw(); });
  root.dispose = () => { off(); observer.disconnect(); }; requestAnimationFrame(draw); return root;
}

