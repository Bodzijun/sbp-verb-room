// Stable on-screen size of source / listener markers (T6 A5).
// Radius depends only on the object role and the number of sources — never on the
// camera or on selection — so orbiting, zooming or clicking cannot make markers
// pulse. Perspective only moves the marker; it does not scale it.
import { basis, dot } from './camera.js';
export const MARKER_RADIUS = { listener: 8, source: 12, denseSource: 9 };
export const DENSE_SOURCES = 8;
export function markerRadius(kind, sourceCount) {
  return kind === 'listener' ? MARKER_RADIUS.listener : sourceCount > DENSE_SOURCES ? MARKER_RADIUS.denseSource : MARKER_RADIUS.source;
}
// Orientation tick: constant maximum screen length, foreshortened smoothly (and
// monotonically) by how much the heading points along the view axis.
export function orientationTick(camera, yawRad, maxPx = 18) {
  const b = basis(camera), u = [Math.cos(yawRad), Math.sin(yawRad), 0];
  return [dot(u, b.right) * maxPx, -dot(u, b.up) * maxPx];
}

export const directivityLength = value => 2+16*Math.max(0,Math.min(1,Number(value)||0));
export function directivityShape(value){
  const standards=[[0,'omni'],[.3,'subcardioid'],[.5,'cardioid'],[.75,'hypercardioid'],[1,'figure-8']];
  return standards.find(([p])=>Math.abs(value-p)<=.025)?.[1]||'';
}
export function directivityTicks(camera,yaw,value){
  const forward=orientationTick(camera,yaw,directivityLength(value));
  const back=Math.max(0,Math.min(1,(value-.8)/.2))*8;
  return {forward,backward:back?orientationTick(camera,yaw+Math.PI,back):null};
}
