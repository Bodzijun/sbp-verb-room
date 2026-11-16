// Binary layout v1: LSB-first validity mask padded to 4 bytes, then LE 21-float cells.
export function decodeEnergy(buffer, descriptor) {
  const count = descriptor.nx * descriptor.ny, offset = descriptor.blob.maskByteLength;
  if (descriptor.blob.layoutVersion !== 1 || offset !== Math.ceil(count / 32) * 4 || buffer.byteLength !== offset + count * 84) throw new Error('Invalid Energy binary layout / byte length');
  const bytes = new Uint8Array(buffer), view = new DataView(buffer);
  return { descriptor, valid: index => !!(bytes[index >> 3] & (1 << (index & 7))),
    energy(index, band = -1, components = 7) {
      if (!this.valid(index)) return null;
      let result = 0;
      for (let c = 0; c < 3; c++) if (components & (1 << c)) {
        if (band < 0) for (let b = 0; b < 7; b++) result += view.getFloat32(offset + (index * 21 + c * 7 + b) * 4, true) / 7;
        else result += view.getFloat32(offset + (index * 21 + c * 7 + band) * 4, true);
      }
      return result;
    }
  };
}
export function projectEnergy(decoded, band, components, auto = true, requestedRange = 60) {
  const { nx, ny } = decoded.descriptor;
  const values = Array.from({ length: nx * ny }, (_, i) => decoded.energy(i, band, components));
  const peak = Math.max(0, ...values.filter(v => v !== null));
  const db = values.map(v => v === null ? null : v <= 0 || peak <= 0 ? -Infinity : 10 * Math.log10(v / peak));
  const lit = db.filter(Number.isFinite).sort((a, b) => a - b);
  const range = auto && lit.length ? Math.max(0.5, Math.min(120, -lit[Math.floor(lit.length * 0.05)])) : requestedRange;
  return { values, db, peak, range };
}
export function makeSyntheticEnergy(d, valid = true) {
  const buffer = new ArrayBuffer(d.blob.fullByteLength), bytes = new Uint8Array(buffer), view = new DataView(buffer);
  for (let i = 0; i < d.nx * d.ny; i++) {
    if (valid) bytes[i >> 3] |= 1 << (i & 7);
    const x = (i % d.nx) / d.nx, y = Math.floor(i / d.nx) / d.ny;
    for (let c = 0; c < 3; c++) for (let b = 0; b < 7; b++) {
      const dist = (x - 0.35) ** 2 + (y - 0.7) ** 2;
      const value = c === 0 ? Math.exp(-dist * (12 + b)) : c === 1 ? Math.exp(-dist * 6) * (0.2 + Math.abs(Math.sin(x * 14 + y * 8)) * 0.3) : 0.12 * Math.exp(-dist * 2);
      view.setFloat32(d.blob.maskByteLength + (i * 21 + c * 7 + b) * 4, value, true);
    }
  }
  return buffer;
}
export const ENERGY_PALETTES = ['Classic', 'Inferno', 'Viridis', 'Ice', 'Grayscale'];
export function energyColour(t, palette = 'Inferno') {
  t = Math.max(0, Math.min(1, t));
  if (palette === 'Classic') {
    const stops=[[23,44,73],[40,126,170],[42,172,137],[239,190,57],[244,108,84]], f=t*4,k=Math.min(3,Math.floor(f));
    return `rgb(${stops[k].map((v,i)=>Math.round(v+(stops[k+1][i]-v)*(f-k))).join(',')})`;
  }
  const stops = palette === 'Viridis' ? [[68, 1, 84], [33, 145, 140], [253, 231, 37]] : palette === 'Ice' ? [[10, 20, 36], [33, 120, 190], [210, 250, 255]] : palette === 'Grayscale' ? [[10, 10, 10], [120, 120, 120], [255, 255, 255]] : [[15, 8, 30], [170, 44, 70], [255, 225, 110]];
  const index = t < 0.5 ? 0 : 1, f = t < 0.5 ? t * 2 : (t - 0.5) * 2;
  return `rgb(${stops[index].map((n, i) => Math.round(n + (stops[index + 1][i] - n) * f)).join(',')})`;
}

// Invalidate outstanding binary reads on LOAD, even if they resolve afterwards.
export function createEnergyLoader(read, accept, reject) {
  let generation = 0, current = null;
  return async result => {
    if (!result) { ++generation; current = null; accept(null); return; }
    if (current === result.resultId) return;
    current = result.resultId; const token = ++generation;
    try { const buffer = await read(result.blob.fullUrl); if (token === generation) accept(decodeEnergy(buffer, result)); }
    catch (error) { if (token === generation) { current = null; reject(error); } }
  };
}
