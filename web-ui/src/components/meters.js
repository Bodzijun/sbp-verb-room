import { element } from './dom.js';
export function meters(store) {
  const root = element('div', { className: 'meters', 'aria-label': `Input meters.${store.mode === 'mock' ? ' Synthetic mock telemetry.' : ''}` });
  let levels = [], holds = [], last = performance.now();
  const update = ({ reason }) => {
    if (reason !== 'meters.input') return;
    const event = store.telemetry['meters.input']; if (!event) return;
    const dt = performance.now() - last; last = performance.now();
    if (root.children.length !== event.peaks.length) { root.replaceChildren(...event.peaks.map((_, i) => element('div', { className: 'meter', title: `Input ${i + 1}` }, [element('i'), element('b')]))); levels = []; holds = []; }
    event.peaks.forEach((peak, i) => {
      levels[i] = Math.max(peak, (levels[i] || 0) * Math.pow(0.88, dt / 100)); holds[i] = Math.max(peak, (holds[i] || 0) * Math.pow(0.99, dt / 100));
      root.children[i].firstChild.style.height = `${Math.max(0, (20 * Math.log10(Math.max(1e-9, levels[i])) + 60) / 60) * 100}%`;
      root.children[i].lastChild.style.bottom = `${Math.max(0, (20 * Math.log10(Math.max(1e-9, holds[i])) + 60) / 60) * 100}%`;
    });
  };
  const off = store.subscribe(update); root.dispose = off; return root;
}
// Horizontal DAW-style level meter per source row: bar follows the loudest host
// input the source reads (post-meter telemetry is input-peak only; no DSP claim).
const dbFill = v => `${Math.max(0, Math.min(1, (20 * Math.log10(Math.max(1e-9, v)) + 60) / 60)) * 100}%`;
export function sourceMeters(store, ranges) {
  const nodes = ranges.map(([first, count]) => ({ first, count, level: 0, hold: 0, root: element('div', { className: 'hmeter', title: `Input ${first + 1}${count > 1 ? `–${first + count}` : ''} · peak, −60…0 dB${store.mode === 'mock' ? ' (synthetic)' : ''}` }, [element('i'), element('b')]) }));
  let last = performance.now();
  const off = store.subscribe(({ reason }) => {
    if (reason !== 'meters.input') return;
    const event = store.telemetry['meters.input']; if (!event) return;
    const now = performance.now(), dt = now - last; last = now;
    for (const n of nodes) {
      let peak = 0; for (let c = 0; c < n.count; c++) peak = Math.max(peak, event.peaks[n.first + c] || 0);
      n.level = Math.max(peak, n.level * Math.pow(0.88, dt / 100)); n.hold = Math.max(peak, n.hold * Math.pow(0.985, dt / 100));
      n.root.firstChild.style.left = dbFill(n.level); n.root.lastChild.style.left = dbFill(n.hold);
      n.root.lastChild.hidden = n.hold < .001;
    }
  });
  return { nodes: nodes.map(n => n.root), dispose: off };
}

export function outputMeterFrame(previous,event,now) {
 return event.peaks.map((peak,i)=>{const old=previous[i]||{hold:0,until:0};const hold=peak>=old.hold||now>=old.until?peak:old.hold;return {peak,rms:event.rms[i]||0,clip:!!event.clips[i],hold,until:peak>=old.hold||now>=old.until?now+1500:old.until};});
}
export function outputMeters(store) {
 const root=element('div',{className:'output-meters','aria-label':'Output meters'});let frames=[];
 root.append(element('small',{text:'Output peak / RMS - dBFS -60 ... 0'}));const rows=element('div');root.append(rows);
 const off=store.subscribe(({reason})=>{if(reason!=='meters.output')return;const e=store.telemetry['meters.output'];frames=outputMeterFrame(frames,e,performance.now());
  if(rows.children.length!==frames.length)rows.replaceChildren(...frames.map((_,i)=>element('div',{className:'output-meter-row'},[element('small',{text:String(i+1)}),element('div',{className:'output-meter-bar'},[element('i'),element('b'),element('em')]),element('small',{className:'output-meter-readout'}),element('button',{type:'button',text:'CLIP','aria-label':`Clear output ${i+1} clip`,onclick:()=>store.command({op:'output.resetClip',channel:i})})])));
  frames.forEach((f,i)=>{const row=rows.children[i],bar=row.children[1];bar.children[0].style.width=dbFill(f.rms);bar.children[1].style.left=dbFill(f.peak);bar.children[2].style.left=dbFill(f.hold);row.children[2].textContent=`${(20*Math.log10(Math.max(1e-3,f.peak))).toFixed(1)} / ${(20*Math.log10(Math.max(1e-3,f.rms))).toFixed(1)}`;row.lastChild.classList.toggle('lit',f.clip);row.lastChild.setAttribute('aria-pressed',String(f.clip));});
 });root.dispose=off;return root;
}
