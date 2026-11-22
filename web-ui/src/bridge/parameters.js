export const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
export function range(d) { return d.type === 'float' ? [d.min, d.max] : [0, d.type === 'bool' ? 1 : d.choices.length - 1]; }
export function canonical(d, value) {
  if (!Number.isFinite(value)) throw new Error('A finite parameter value is required');
  const [lo, hi] = range(d), step = d.step || 1;
  if(d.type==='float' && d.step===0)return clamp(value,lo,hi);
  return clamp(Number(clamp(lo + Math.round((clamp(value, lo, hi) - lo) / step) * step, lo, hi).toFixed(7)),lo,hi);
}
function skew(d) { const [lo, hi] = range(d); return d.skew?.type === 'centre' ? Math.log(0.5) / Math.log((d.skew.centre - lo) / (hi - lo)) : 1; }
export function toNormalised(d, value) {
  const [lo, hi] = range(d); return Math.pow(clamp((value - lo) / (hi - lo), 0, 1), skew(d));
}
export function fromNormalised(d, normalised) {
  const [lo, hi] = range(d); return canonical(d, lo + (hi - lo) * Math.pow(clamp(normalised, 0, 1), 1 / skew(d)));
}
export function toText(d, value) {
  if (d.type === 'bool') return value >= 0.5 ? 'On' : 'Off';
  if (d.type === 'choice') return d.choices[canonical(d, value)];
  return canonical(d, value).toFixed(d.decimals ?? 3);
}
// Display is deliberately independent from the lossless editing/wire precision.
export function displayNumber(value, unit = '', decimals = 3) {
  const precision = unit === 'dB' ? 1 : ['m', 's'].includes(unit) ? 2 : ['%', 'deg', '°C'].includes(unit) ? 1 : Math.min(decimals, 2);
  let text = Number(value).toFixed(precision);
  if (unit !== 'dB') text = String(Number(text));
  if (Number(text) === 0 && unit === 'dB') text = '0.0';
  return `${text}${unit ? ` ${unit}` : ''}`;
}
export function displayText(d, value) {
  if (d.type !== 'float') return toText(d, value);
  if (['absorption','diffusion'].includes(d.id)) return displayNumber(canonical(d, value) * 100, '%', 1);
  if (d.id === 'doppler') return displayNumber(canonical(d,value),'×');
  if (d.unit === 'deg') return `${Number(canonical(d,value).toFixed(1))}°`;
  if (/^decay(Low|Mid|High)$/.test(d.id)) return displayNumber(canonical(d, value), '×');
  return displayNumber(canonical(d, value), d.unit, d.decimals);
}
export function fromText(d, text) {
  const raw = String(text).trim().replace(/°$/,d.unit==='deg'?' deg':'°');
  if (['absorption','diffusion'].includes(d.id) && raw.endsWith('%')) return canonical(d, Number(raw.slice(0, -1).trim()) / 100);
  if (d.type === 'bool' && /^(on|off)$/i.test(raw)) return /^on$/i.test(raw) ? 1 : 0;
  if (d.type === 'choice') { const index = d.choices.findIndex(v => v.toLowerCase() === raw.toLowerCase()); if (index >= 0) return index; }
  const numeric = d.unit && raw.endsWith(d.unit) ? raw.slice(0, -d.unit.length).trim() : raw.replace(/\s*×$/, '');
  if (!numeric || !/^[+-]?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?$/i.test(numeric)) throw new Error('Invalid parameter text');
  return canonical(d, Number(numeric));
}
