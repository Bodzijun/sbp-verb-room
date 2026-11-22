import { add, sub, mul, dot, basis, project } from './camera.js';
export const NEAR = 0.08;
export function clipNear(a,b,camera,near=NEAR) {
  if(camera.view!=='3d') return [a,b];
  const back=basis(camera).back;
  const depth=p=>camera.distance-dot(sub(p,camera.target),back);
  const da=depth(a),db=depth(b);
  if(da<near&&db<near) return null;
  if(da>=near&&db>=near) return [a,b];
  const cut=add(a,mul(sub(b,a),(near-da)/(db-da)));
  return da<near?[cut,b]:[a,cut];
}
// Sutherland–Hodgman clip of a world polygon against the camera's near plane (3D only). Without it a vertex
// behind the camera projects with a flipped sign and the face is drawn as a huge false shape.
// Returns the same array when nothing is clipped, null when the polygon is wholly behind the camera.
export function clipPolygonNear(points,camera,near=NEAR) {
  if(camera.view!=='3d') return points;
  const back=basis(camera).back;
  const depth=p=>camera.distance-dot(sub(p,camera.target),back);
  const d=points.map(depth);
  if(d.every(v=>v>=near)) return points;
  if(d.every(v=>v<near)) return null;
  const out=[];
  for(let i=0;i<points.length;i++) {
    const j=(i+1)%points.length,a=points[i],b=points[j];
    if(d[i]>=near) out.push(a);
    if((d[i]>=near)!==(d[j]>=near)) out.push(add(a,mul(sub(b,a),(near-d[i])/(d[j]-d[i]))));
  }
  return out.length>=3?out:null;
}
// Liang–Barsky screen clipping prevents near-plane intersections from huge coordinates.
export function clipScreen(a,b,width,height) {
  const dx=b[0]-a[0],dy=b[1]-a[1]; let lo=0,hi=1;
  for(const [p,q] of [[-dx,a[0]],[dx,width-a[0]],[-dy,a[1]],[dy,height-a[1]]]) {
    if(Math.abs(p)<1e-12) { if(q<0) return null; continue; }
    const t=q/p; if(p<0) lo=Math.max(lo,t); else hi=Math.min(hi,t);
    if(lo>hi) return null;
  }
  return [[a[0]+lo*dx,a[1]+lo*dy],[a[0]+hi*dx,a[1]+hi*dy]];
}
export function projectedSegment(a,b,camera,width,height) {
  const clipped=clipNear(a,b,camera);
  return clipped?clipScreen(project(clipped[0],camera,width,height),project(clipped[1],camera,width,height),width,height):null;
}
export function gridStep(bounds,view) {
  const axes=view==='front'?[0,2]:view==='side'?[1,2]:[0,1];
  const span=Math.max(...axes.map(i=>bounds.max[i]-bounds.min[i]),1);
  return Math.pow(10,Math.ceil(Math.log10(span/18)));
}
export function finiteGrid(bounds,view) {
  const axes=view==='front'?[0,2]:view==='side'?[1,2]:[0,1];
  const span=Math.max(...axes.map(i=>bounds.max[i]-bounds.min[i]),1);
  const step=gridStep(bounds,view), padding=span*.35;
  const min=axes.map(i=>Math.floor((bounds.min[i]-padding)/step)*step),max=axes.map(i=>Math.ceil((bounds.max[i]+padding)/step)*step);
  const center=min.map((v,i)=>(v+max[i])/2),half=min.map((v,i)=>(max[i]-v)/2);
  const lines=[];
  for(let axis=0;axis<2;axis++) for(let v=min[axis];v<=max[axis]+step*.01;v+=step) {
    // Split into short segments so opacity fades along each line as well as across it.
    const other=1-axis;
    for(let u=min[other];u<max[other];u+=step) {
      const a=[0,0,0],b=[0,0,0]; a[axes[axis]]=b[axes[axis]]=v; a[axes[other]]=u; b[axes[other]]=Math.min(max[other],u+step);
      const edge=Math.max(Math.abs(v-center[axis])/half[axis],Math.abs(u+step*.5-center[other])/half[other]);
      lines.push({a,b,alpha:Math.max(0,Math.min(1,(1-edge)/.28))*.34});
    }
  }
  return lines;
}
