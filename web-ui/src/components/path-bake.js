export function pathBakeView(status, object) {
  const perPath=Array.isArray(status?.paths)?status.paths.find(x=>x?.object===object):null;
  const s=perPath||(status && (object===-1 || status.object===object || status.object===-1)?status:{state:'notBaked'});
  const running=s.state==='baking';
  const label=running?'Cancel':['outdated','failed','approximate'].includes(s.state)?'Bake again':'Bake path';
  const text=({notBaked:'Not baked',baking:`Baking ${s.percent??0} %`,baked:'Baked',approximate:'Baked (approx.)',outdated:'Outdated',cancelled:'Not baked · Cancelled',blocked:'Cannot bake',failed:'Bake failed'})[s.state] || 'Not baked';
  return {running,label,warn:s.state==='outdated',text:s.reason?`${text} · ${s.reason}`:text};
}
export function bakeSourceText(status) {
  if (status?.bakeSource === 'recorded') return `recorded automation (${status.passSamples} samples)`;
  if (status?.bakeSource === 'drawn') return `drawn paths (${status.bakeSourceGroup ? `Group ${status.bakeSourceGroup}` : 'independent'})`;
  return 'nothing to bake';
}
export function bakeFooterText(s) {
  if(!s)return '';
  if(s.state==='baking')return `Baking path · ${s.name} · ${s.percent??0} %${s.reason?` · ${s.reason}`:''}`;
  if(s.state==='baked')return s.reason?`${s.name||'Path'} · ${s.reason}`:(s.name&&s.object===-1?`${s.name} baked`:'Path baked');
  if(s.state==='approximate')return `Path baked (approx.) · ${s.reason||'Some intervals exceed tolerance'}`;
  if(['blocked','failed','outdated'].includes(s.state))return s.reason||'Path needs a new bake';
  if(s.state==='cancelled')return 'Path bake cancelled';
  return '';
}
