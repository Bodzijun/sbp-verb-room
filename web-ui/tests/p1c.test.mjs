import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import {createRequire} from 'node:module';
import {serve} from '../scripts/serve.mjs';
const require=createRequire(import.meta.url);
test('P1c inspector columns, microphone adjacency, splitter persistence/clamps/reset and screen-pixel arrows', async()=>{
 const pw=require(process.env.PLAYWRIGHT_MODULE||'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package');
 const {server,url}=await serve({port:0});const browser=await pw.chromium.launch({headless:true,executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
 try {
 const page=await browser.newPage({viewport:{width:1280,height:800}});
 await page.addInitScript(()=>{
  window.arrowTriangles=[];const proto=CanvasRenderingContext2D.prototype;
  const begin=proto.beginPath,move=proto.moveTo,line=proto.lineTo,fill=proto.fill;
  proto.beginPath=function(...a){this._points=[];return begin.apply(this,a);};
  proto.moveTo=function(x,y){this._points?.push([x,y]);return move.call(this,x,y);};
  proto.lineTo=function(x,y){this._points?.push([x,y]);return line.call(this,x,y);};
  proto.fill=function(...a){if(this._points?.length===3){const [t,b,c]=this._points;const n=Math.hypot(t[0]-b[0],t[1]-b[1]);if(Math.abs(n-18)<.001){window.arrowTriangles.push(n);window.lastArrow=t;}}return fill.apply(this,a);};
 });
 await page.goto(url);await page.waitForFunction(()=>window.verbRoom?.store?.ready);
 await fs.mkdir('shots/P1c',{recursive:true});
 await page.evaluate(async()=>{const s=window.verbRoom.store;for(const object of [0,32])await s.command({op:'path.set',object,points:[[2,2,1.5],[5,4,1.5],[7,2,1.5]],enabled:true});});
 for(const kind of ['source','listener']){
  await page.evaluate(kind=>window.verbRoom.store.setView({selection:[{kind,index:0}]}),kind);
  await page.waitForTimeout(100);
  assert.equal(await page.locator('.card-grid > :first-child').getAttribute('class'),'stack pose-column');
  assert.equal(await page.locator('.card-grid > :nth-child(2)').getAttribute('class'),'stack settings-column');
  const pose=await page.locator('.pose-column .slider-row').first().boundingBox(),path=await page.locator('.path-section').boundingBox();assert.ok(pose.y<path.y);
  if(kind==='listener')assert.equal(await page.locator('.settings-column').evaluate(e=>e.querySelector('[aria-label="Microphone pattern"]').closest('.row').nextElementSibling.textContent.trim()),'Show microphone array to scale');
  const track=await page.locator('.path-section .track').boundingBox(),value=await page.locator('.path-section .readout').first().boundingBox();
  assert.ok(Math.abs(track.x+track.width+5-value.x)<2,'Position track fills space to value box');
  await page.evaluate(async kind=>{await window.verbRoom.store.command({op:'path.setFlags',object:kind==='listener'?32:0,closed:true});},kind);
  await page.waitForTimeout(70);const laps=await page.getByRole('textbox',{name:'Path laps',exact:true}).boundingBox();
  assert.equal(Math.round(laps.width),48);assert.ok(Math.abs(laps.x+laps.width-value.x-value.width)<2,'Laps right aligned');
  await page.screenshot({path:`shots/P1c/${kind}-1280x800.png`});
  await page.evaluate(async kind=>{await window.verbRoom.store.command({op:'path.setFlags',object:kind==='listener'?32:0,closed:false});},kind);
 }
 await page.getByRole('tab',{name:'Settings',exact:true}).click();
 assert.equal(await page.locator('.output-panel [aria-label="Microphone pattern"]').evaluate(e=>e.closest('.row').nextElementSibling.textContent.trim()),'Show microphone array to scale');
 const bar=page.getByRole('separator',{name:'Inspector / tabs split'});
 const height=()=>page.locator('.main-context').evaluate(e=>e.getBoundingClientRect().height);
 const initial=await height();let box=await bar.boundingBox();
 await page.mouse.move(box.x+box.width/2,box.y+3);await page.mouse.down();await page.mouse.move(box.x+box.width/2,box.y-70,{steps:6});await page.mouse.up();await page.waitForTimeout(100);
 assert.ok(await height()<initial-50);await page.screenshot({path:'shots/P1c/splitter-up.png'});
 const share=await page.evaluate(()=>window.verbRoom.store.state.preferences.sessionViewState.inspectorSplit);assert.ok(share>0);
 await page.evaluate(()=>window.verbRoom.store.setView({tab:'mix'}));await page.waitForTimeout(100);assert.ok(Math.abs(await height()-share*await page.locator('.rack').evaluate(e=>e.clientHeight))<3, JSON.stringify(await page.evaluate(()=>({v:window.verbRoom.store.view.inspectorSplit,h:document.querySelector('.main-context').clientHeight,r:document.querySelector('.rack').clientHeight,style:document.querySelector('.rack').style.cssText})))+' share='+share);
 box=await bar.boundingBox();await page.mouse.move(box.x+100,box.y+3);await page.mouse.down();await page.mouse.move(box.x+100,760,{steps:8});await page.mouse.up();await page.waitForTimeout(100);
 assert.ok(await page.locator('.bottom').evaluate(e=>e.clientHeight)>=150);await page.screenshot({path:'shots/P1c/splitter-down.png'});
 box=await bar.boundingBox();await page.mouse.move(box.x+100,box.y+3);await page.mouse.down();await page.mouse.move(box.x+100,60,{steps:6});await page.mouse.up();await page.waitForTimeout(80);assert.ok(await height()>=176&&await height()<=178,'inspector lower clamp');
 await bar.focus();await page.keyboard.press('ArrowUp');await page.waitForTimeout(80);await bar.dblclick();await page.waitForTimeout(80);assert.equal(await page.evaluate(()=>window.verbRoom.store.view.inspectorSplit),null);
 for(const projection of ['plan','3d']){
 await page.evaluate(projection=>{window.arrowTriangles=[];window.verbRoom.store.setView({projection});},projection);await page.waitForTimeout(180);
 assert.ok(await page.evaluate(()=>window.arrowTriangles.length)>0,projection+' arrow is 18 screen pixels');
 const canvas=page.locator('.viewport canvas');const cb=await canvas.boundingBox();await page.mouse.move(cb.x+cb.width/2,cb.y+cb.height/2);await page.evaluate(()=>window.arrowTriangles=[]);await page.mouse.wheel(0,-180);await page.waitForTimeout(150);assert.ok(await page.evaluate(()=>window.arrowTriangles.length)>0,'zoom retains 18px arrow');
 await canvas.screenshot({path:`shots/P1c/arrow-${projection}.png`});
 const tip=await page.evaluate(()=>window.lastArrow);
 if(projection==='plan'&&tip)await page.screenshot({path:'shots/P1c/arrow-close-up.png',clip:{x:Math.max(cb.x,Math.min(cb.x+cb.width-100,cb.x+tip[0]-50)),y:Math.max(cb.y,Math.min(cb.y+cb.height-100,cb.y+tip[1]-50)),width:100,height:100}});
 }
 await page.setViewportSize({width:900,height:600});
 for(const kind of ['source','listener']){
 await page.evaluate(kind=>window.verbRoom.store.setView({selection:[{kind,index:0}]}),kind);await page.waitForTimeout(100);
 const pose=await page.locator('.pose-column').boundingBox(),settings=await page.locator('.settings-column').boundingBox();assert.ok(pose.x+pose.width<=settings.x+1);
 assert.ok(await page.locator('.macro-row').evaluate(e=>e.getBoundingClientRect().bottom)<=600);assert.ok(await page.locator('.bottom').evaluate(e=>e.clientHeight)>=150);
 await page.screenshot({path:`shots/P1c/${kind}-900x600.png`});
 }
 }finally{await browser.close();server.close();}
});


