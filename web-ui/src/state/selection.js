export const sameRef = (a,b) => a.kind===b.kind && a.index===b.index;
export const cleanSelection = refs => refs.reduce((out,{kind,index}) => out.some(r=>sameRef(r,{kind,index}))?out:[...out,{kind,index}],[]);
export function toggleSelection(selection,ref) { return selection.some(r=>sameRef(r,ref))?selection.filter(r=>!sameRef(r,ref)):[...cleanSelection(selection),{kind:ref.kind,index:ref.index}]; }
export function rangeSelection(order,anchor,ref) {
 const a=order.findIndex(r=>sameRef(r,anchor||ref)),b=order.findIndex(r=>sameRef(r,ref));
 return cleanSelection(order.slice(Math.min(a,b),Math.max(a,b)+1));
}
// A marquee touches an object (native semantics), rather than requiring every
// projected vertex to be inside it. Liang–Barsky also covers crossing edges.
export function touchesBox(points,start,end,polygon=false) {
 const lo=start.map((v,i)=>Math.min(v,end[i])),hi=start.map((v,i)=>Math.max(v,end[i]));
 const contains=p=>p[0]>=lo[0]&&p[0]<=hi[0]&&p[1]>=lo[1]&&p[1]<=hi[1];
 if(points.some(contains))return true;
 for(let i=0;i<points.length-(polygon?0:1);i++) {
  const a=points[i],b=points[(i+1)%points.length],d=b.map((v,j)=>v-a[j]);let t0=0,t1=1,ok=true;
  for(const [p,q] of [[-d[0],a[0]-lo[0]],[d[0],hi[0]-a[0]],[-d[1],a[1]-lo[1]],[d[1],hi[1]-a[1]]]){
   if(p===0){if(q<0)ok=false;}else if(p<0)t0=Math.max(t0,q/p);else t1=Math.min(t1,q/p);
  }if(ok&&t0<=t1)return true;
 }
 if(polygon){let inside=false;for(let i=0,j=points.length-1;i<points.length;j=i++){const a=points[i],b=points[j];if((a[1]>lo[1])!==(b[1]>lo[1])&&lo[0]<(b[0]-a[0])*(lo[1]-a[1])/(b[1]-a[1])+a[0])inside=!inside;}return inside;}
 return false;
}
export function boxSelection(base,hits,{shiftKey=false,ctrlKey=false,altKey=false,metaKey=false}={}) {
 const refs=cleanSelection(hits);
 return ctrlKey||altKey||metaKey?base.filter(r=>!refs.some(h=>sameRef(r,h))):shiftKey?cleanSelection([...base,...refs]):refs;
}
