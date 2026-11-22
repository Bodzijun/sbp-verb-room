import { channels } from '../state/selectors.js';
export function facingAngle(origin, target) {
  if (!target || Math.hypot(target[0]-origin[0],target[1]-origin[1]) < 1e-8) return null;
  return ((Math.atan2(target[1]-origin[1],target[0]-origin[0])*180/Math.PI+180)%360+360)%360-180;
}
export function autoNumberSources(sources, available, reserved = []) {
  let cursor=0, missingChannels=0, skippedSources=0;
  const assignments=[];
  sources.forEach((source,index)=>{
    const count=channels(source);
    for (const [start, end] of reserved) {
      if (cursor < end && cursor + count > start) cursor = end;
    }
    if(cursor+count<=available) assignments.push({index,firstIn:cursor});
    else {missingChannels+=Math.max(0,cursor+count-Math.max(cursor,available));skippedSources++;}
    cursor+=count;
  });
  return {assignments,missingChannels,skippedSources};
}
export function cursorEnergyLevel(world, descriptor, db) {
  if(!world)return null;
  const x=Math.floor((world[0]-descriptor.x0)/descriptor.dx),y=Math.floor((world[1]-descriptor.y0)/descriptor.dy);
  return x<0||y<0||x>=descriptor.nx||y>=descriptor.ny?null:db[y*descriptor.nx+x];
}
