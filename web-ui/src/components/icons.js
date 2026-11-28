import { element } from './dom.js';
const paths = {
 move:'M5 3 19 12 12 13 9 20Z', box:'M4 9V4H9 M15 4H20V9 M20 15V20H15 M9 20H4V15',
 wall:'M5 18 19 6 M5 18h.01 M19 6h.01', room:'M5 5H19V19H5Z M5 5h.01 M19 5h.01 M19 19h.01 M5 19h.01',
 floorZone:'M3 17 8 7 21 9 17 20Z M6 16 10 8 M10 18 14 9 M14 19 18 9',
 ceilingZone:'M3 9 8 3 21 5 17 12Z M4 15 8 19 18 19 21 15 M8 13V19 M17 14V19',
 plate:'M3 14 12 10 21 14 12 18Z M3 14V18L12 22 21 18V14 M12 2V9 M9 5 12 2 15 5',
 zoneBox:'M4 6H8 M11 6H13 M16 6H20V9 M20 12V14 M20 17V18H16 M13 18H11 M8 18H4V17 M4 14V12 M4 9V6 M12 12h.01 M10 12H14 M12 10V14',
 zonePoly:'M4 18 7 6 16 4 20 14 12 20Z M10 12H14 M12 10V14',
 delete:'M3 14 13 4 21 12 11 22H8Z M8 9 16 17 M11 22H22',
 undo:'M8 5 3 10 8 15 M3 10H14C23 10 23 21 14 21', redo:'M16 5 21 10 16 15 M21 10H10C1 10 1 21 10 21',
 clear:'M4 6H20 M9 6V3H15V6 M6 6 7 21H17L18 6 M10 10V17 M14 10V17',
 eye:'M2 12Q12 1 22 12Q12 23 2 12Z M15 12A3 3 0 1 1 9 12A3 3 0 1 1 15 12',
 eyeOff:'M3 3 21 21 M2 12Q6 5 12 5Q18 5 22 12Q19 17 16 18 M11 19Q5 18 2 12',
 lock:'M7 10V7A5 5 0 0 1 17 7V10 M5 10H19V22H5Z M12 14V18',
 unlock:'M7 10V7A5 5 0 0 1 17 7 M5 10H19V22H5Z M12 14V18',
 nav:'M12 2V8 M12 16V22 M2 12H8 M16 12H22 M16 12A4 4 0 1 1 8 12A4 4 0 1 1 16 12'
};
export function icon(name) {
 const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');
 for(const [k,v] of Object.entries({viewBox:'0 0 24 24',fill:'none',stroke:'currentColor','stroke-width':1.6,'stroke-linecap':'round','stroke-linejoin':'round','aria-hidden':'true'}))svg.setAttribute(k,v);
 const p=document.createElementNS(svg.namespaceURI,'path');p.setAttribute('d',paths[name]||paths.move);svg.append(p);
 if(['wall','room'].includes(name))for(const [cx,cy] of name==='wall'?[[5,18],[19,6]]:[[5,5],[19,5],[19,19],[5,19]]){const c=document.createElementNS(svg.namespaceURI,'circle');c.setAttribute('cx',cx);c.setAttribute('cy',cy);c.setAttribute('r','2');c.setAttribute('fill','var(--panel)');svg.append(c);}
 return svg;
}
export function decorateIcon(button,name,tooltip) {
 button.replaceChildren(icon(name));
 if(tooltip){button.removeAttribute('title');button.dataset.tooltip=tooltip;button.append(element('span',{className:'tool-tip',role:'tooltip',text:tooltip}));}
 return button;
}
