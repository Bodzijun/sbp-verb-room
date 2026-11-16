// Shared instrument readout: vertical drag, Shift fine, double-click precise entry.
export function valueField(input, { get, min, max, step=0.001, display=String, edit=String, start=()=>{}, preview=()=>{}, commit, end=()=>{} }) {
  let grab=null;
  input.readOnly=true;
  input.title=`${input.getAttribute('aria-label') || 'Value'} · drag vertically · Shift fine · double-click to type`;
  const paint=()=>{ input.value=display(get()); };
  const snap=n=>Number(Math.max(min,Math.min(max,min+Math.round((n-min)/step)*step)).toFixed(7));
  const finish=cancel=>{
    if(!grab) return;
    const g=grab; grab=null; if(input.hasPointerCapture(g.id))input.releasePointerCapture(g.id);
    if(g.moved && !cancel) commit(g.value);
    end(); input.removeAttribute('data-gesture-active'); paint();
    window.dispatchEvent(new Event('verb-control-end'));
  };
  input.addEventListener('pointerdown',e=>{
    if(e.button!==0||input.disabled||!input.readOnly) return;
    e.preventDefault(); grab={id:e.pointerId,y:e.clientY,initial:get(),value:get(),moved:false}; input.setPointerCapture(e.pointerId);
  });
  input.addEventListener('pointermove',e=>{
    if(!grab||grab.id!==e.pointerId) return;
    const dy=grab.y-e.clientY;
    if(!grab.moved&&Math.abs(dy)>3) { grab.moved=true; input.setAttribute('data-gesture-active','true'); start(); }
    if(grab.moved) { grab.value=snap(grab.initial+dy*(max-min)/(e.shiftKey?2400:240)); preview(grab.value); input.value=display(grab.value); }
  });
  input.addEventListener('pointerup',()=>finish(false));
  input.addEventListener('pointercancel',()=>finish(true));
  input.addEventListener('lostpointercapture',()=>finish(true));
  input.addEventListener('dblclick',()=>{ if(input.disabled) return; input.readOnly=false; input.value=edit(get()); input.focus(); input.select(); });
  input.addEventListener('blur',()=>{ input.readOnly=true; paint(); });
  input.addEventListener('keydown',e=>{ if(e.key==='Enter'||e.key==='Escape') { input.readOnly=true; requestAnimationFrame(paint); } });
  input.addEventListener('wheel',e=>{
    if(input.disabled||!input.readOnly) return;
    e.preventDefault(); start(); const v=snap(get()-Math.sign(e.deltaY)*step*(e.shiftKey?1:10)); preview(v); commit(v); end(); paint();
  },{passive:false});
  const cancel=()=>finish(true),visibility=()=>{if(document.hidden) cancel();};
  window.addEventListener('blur',cancel); window.addEventListener('verb-ui-error',cancel); window.addEventListener('verb-gestures-reset',cancel); document.addEventListener('visibilitychange',visibility);
  paint();
  return ()=>{cancel();window.removeEventListener('blur',cancel); window.removeEventListener('verb-ui-error',cancel); window.removeEventListener('verb-gestures-reset',cancel);document.removeEventListener('visibilitychange',visibility);};
}
