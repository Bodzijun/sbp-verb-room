// Geometric image-source preview only. No acoustic weights / engine claims.
import { add, sub, mul, dot } from './camera.js';
const EPS = 1e-7;
export function mirror(point, plane) { return sub(point, mul(plane.normal, 2 * dot(sub(point, plane.origin), plane.normal))); }
export function planeHit(a, b, plane) {
  const delta = sub(b, a), den = dot(delta, plane.normal);
  if (Math.abs(den) < EPS) return null;
  const t = dot(sub(plane.origin, a), plane.normal) / den;
  return t > EPS && t < 1 - EPS ? { point:add(a, mul(delta, t)), t } : null;
}
function inside(p, polygon) {
  let yes=false;
  for (let i=0,j=polygon.length-1;i<polygon.length;j=i++) {
    const a=polygon[i],b=polygon[j];
    if ((a[1]>p[1]) !== (b[1]>p[1]) && p[0]<(b[0]-a[0])*(p[1]-a[1])/(b[1]-a[1])+a[0]) yes=!yes;
  }
  return yes;
}
// Recover a closed shell; disconnected fixtures do not invent a floor rectangle.
function boundary(walls) {
  if (!walls.length) return [];
  const rest=walls.slice(1), poly=[walls[0].a,walls[0].b];
  while (rest.length) {
    const last=poly[poly.length-1];
    if (Math.hypot(last[0]-poly[0][0],last[1]-poly[0][1])<EPS) return poly.slice(0,-1);
    const i=rest.findIndex(w=>[w.a,w.b].some(p=>Math.hypot(p[0]-last[0],p[1]-last[1])<EPS));
    if(i<0) return [];
    const w=rest.splice(i,1)[0]; poly.push(Math.hypot(w.a[0]-last[0],w.a[1]-last[1])<EPS?w.b:w.a);
  }
  return Math.hypot(...sub(poly[0],poly[poly.length-1]))<EPS ? poly.slice(0,-1):[];
}
export function reflectionPlanes(scene) {
  const planes=scene.walls.map((w,index)=>{
    const dx=w.b[0]-w.a[0],dy=w.b[1]-w.a[1],length=Math.hypot(dx,dy);
    if(length<EPS) return null;
    return { id:`wall:${index}`, normal:[dy/length,-dx/length,0], origin:[...w.a,0], contains:p=>{
      const u=((p[0]-w.a[0])*dx+(p[1]-w.a[1])*dy)/(length*length);
      return u>EPS && u<1-EPS && p[2]>=w.zBottomM-EPS && p[2]<=(w.zTopM??scene.heightM)+EPS;
    }};
  }).filter(Boolean);
  const footprint=scene.ceilingFootprint ?? [boundary(scene.walls)].filter(p=>p.length>=3);
  const ceilingPresent=p=>{
    const zone=[...scene.ceilingZones].reverse().find(z=>z.heightM===null&&inside(p,z.vertices));
    if(zone)return zone.material!==null;
    return scene.ceilingMaterial!==null&&(scene.ceilingExtent==='infinite'||!footprint.length||footprint.some(poly=>inside(p,poly)));
  };
  if(scene.floorMaterial!==null)planes.push({id:'floor:0',normal:[0,0,1],origin:[0,0,0],contains:()=>true});
  if(scene.ceilingMaterial!==null||scene.ceilingZones.some(z=>z.heightM===null&&z.material!==null))
    planes.push({id:'ceiling:0',normal:[0,0,1],origin:[0,0,scene.heightM],contains:ceilingPresent});
  for(const kind of ['floorZone','ceilingZone']) (kind==='floorZone'?scene.floorZones:scene.ceilingZones).forEach((z,i)=>{
    if(z.heightM===null || z.material===null) return;
    planes.push({id:`${kind}:${i}`,normal:[0,0,1],origin:[0,0,z.heightM],contains:p=>inside(p,z.vertices)});
  });
  return planes;
}
export function imageSourcePaths(source, listener, planes, maxOrder=2) {
  const visible=(a,b)=>!planes.some(p=>{const hit=planeHit(a,b,p); return hit&&p.contains(hit.point);});
  const result=[];
  const accept=(points,surfaces)=>{
    if(points.slice(1).some((p,i)=>!visible(points[i],p))) return;
    result.push({order:surfaces.length,surfaces:surfaces.map(p=>p.id),points,lengthM:points.slice(1).reduce((n,p,i)=>n+Math.hypot(...sub(p,points[i])),0)});
  };
  accept([source,listener],[]);
  const trace=(sequence)=>{
    const images=[source]; sequence.forEach(p=>images.push(mirror(images[images.length-1],p)));
    let end=listener; const bounces=[];
    for(let i=sequence.length-1;i>=0;i--) {
      const plane=sequence[i];
      if(dot(sub(images[i],plane.origin),plane.normal)*dot(sub(end,plane.origin),plane.normal)<=EPS) return;
      const hit=planeHit(end,images[i+1],plane);
      if(!hit||!plane.contains(hit.point)) return;
      bounces.unshift(hit.point); end=hit.point;
    }
    accept([source,...bounces,listener],sequence);
  };
  for(const a of planes) trace([a]);
  // Explicit work budget: 128 walls are legal; second order is previewed up to 24 planes.
  if(maxOrder>=2 && planes.length<=24) for(const a of planes) for(const b of planes) if(a!==b) trace([a,b]);
  return result.sort((a,b)=>a.order-b.order||a.lengthM-b.lengthM);
}

// Geometric escape guides, not sampled acoustic energy or extra image sources.
export function skyEscapeGuides(source,scene) {
 if(source[2]>=scene.heightM)return [];
 const planes=reflectionPlanes(scene),ceiling=planes.find(p=>p.id==='ceiling:0'),height=scene.heightM-source[2];
 return [[0,0],[1,0],[-1,0],[0,1],[0,-1]].map(([x,y])=>[source[0]+x*height,source[1]+y*height,scene.heightM+.02]).filter(end=>!ceiling?.contains(end)&&!planes.some(p=>p.id!=='ceiling:0'&&planeHit(source,end,p)&&p.contains(planeHit(source,end,p).point))).map(end=>[source,end,[end[0],end[1],end[2]+1.]]);
}
