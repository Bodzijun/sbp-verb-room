import { createRequire } from 'node:module';
import { existsSync } from 'node:fs';
import { writeFile, mkdir } from 'node:fs/promises';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { serve } from '../scripts/serve.mjs';
import { pickOption } from './browser-helpers.mjs';
const require = createRequire(import.meta.url);
const candidates = [process.env.PLAYWRIGHT_MODULE, 'playwright', 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'].filter(Boolean);
let playwright;
for (const candidate of candidates) { try { playwright = require(candidate); break; } catch {} }
if (!playwright) { console.error('No headless browser runner available. See README.uk.md for manual checks or set PLAYWRIGHT_MODULE.'); process.exit(2); }
const executablePath = process.env.CHROME_PATH || ['C:/Program Files/Google/Chrome/Application/chrome.exe', 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'].find(existsSync);
const root = fileURLToPath(new URL('../', import.meta.url));
await mkdir(`${root}/shots`, { recursive: true });
const { server, url } = await serve();
const report = { engine: 'Chromium', executablePath, errors: [], externalRequests: [], checks: [], screenshots: [], fixtures: [], schemaMessages: 0, webkit: 'not run' }, audit = [];
let browser;
function pass(name) { report.checks.push({ name, result: 'PASS' }); console.log(`PASS ${name}`); }
async function captureAudit(page) { const entries = await page.evaluate(() => window.verbRoom?.bridge.audit || []); audit.push(...entries); }
try {
  browser = await playwright.chromium.launch({ headless: true, ...(executablePath ? { executablePath } : {}) }); report.browserVersion = browser.version();
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 }, reducedMotion: 'reduce' });
  page.on('pageerror', e => report.errors.push(e.message)); page.on('console', msg => { if (msg.type() === 'error') report.errors.push(msg.text()); });
  page.on('request', req => { if (/^https?:/.test(req.url()) && !req.url().startsWith(url)) report.externalRequests.push(req.url()); });
  const ready = async () => { await page.waitForFunction(() => window.verbRoom?.store.ready && document.querySelector('canvas')); };
  const open = async (fixture = 'default-physical', hash = '') => { await page.goto(`${url}/?fixture=${fixture}#${hash}`); await page.reload(); await ready(); };
  for (const [width, height] of [[1280, 800], [900, 600]]) {
    await page.setViewportSize({ width, height });
    for (const fixture of ['default-physical', 'legacy-creative', 'small-room', 'large-hall', 'energy-result', 'error-resync']) {
      for (const pageName of ['main', 'edit']) {
        await open(fixture, pageName === 'edit' ? 'edit' : ''); await page.waitForTimeout(450);
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true, `${fixture}/${pageName} width`);
        assert.equal(await page.evaluate(() => { const r = document.querySelector('.viewport').getBoundingClientRect(); return r.width > 150 && r.height > 80 && r.bottom <= innerHeight; }), true);
        await captureAudit(page); report.fixtures.push({ fixture, page: pageName, width, height, result: 'PASS' });
      }
    }
    for (const [name, hash] of [['main-physical', 'physical'], ['main-creative', 'creative'], ['main-energy', 'energy'], ['main-front', 'front'], ['main-side', 'side'], ['main-3d', '3d'], ['main-listener', 'listener'], ['main-sources', 'sources'], ['main-mix', 'mix'], ['main-output', 'output'], ['main-theme', 'theme'], ['edit', 'edit'], ['edit-outliner', 'edit-outliner'], ['edit-source', 'edit-source'], ['edit-3d', 'edit-3d'], ['main-scene', 'scene']]) {
      await open(hash === 'energy' ? 'energy-result' : 'default-physical', hash); await page.waitForTimeout(460);
      const file = `${name}-${width}x${height}.png`; await page.screenshot({ path: `${root}/shots/${file}` }); report.screenshots.push(file);
    }
    pass(`Every page / fixture and 16 reference states ${width}×${height}`);
  }
  await page.setViewportSize({ width: 1280, height: 800 }); await open('legacy-creative', 'mix');
  await page.locator('[data-parameter="early"] .readout').dblclick(); await page.locator('[data-parameter="early"] .readout').fill('-12.000'); await page.locator('[data-parameter="early"] .readout').press('Enter');
  assert.equal(await page.evaluate(() => window.verbRoom.store.state.parameters.values.early), -12);
  await page.getByRole('button', { name: 'Physical', exact: true }).click();
  assert.equal(await page.locator('[data-parameter="early"] .readout').isDisabled(), true);
  assert.equal(await page.locator('[data-parameter="diffusion"] .readout').isDisabled(), false);
  assert.equal(await page.evaluate(() => window.verbRoom.store.state.parameters.values.early), -12);
  await page.getByRole('button', { name: 'Creative', exact: true }).click(); assert.equal(await page.locator('[data-parameter="early"] .readout').inputValue(), '-12.0 dB'); pass('Physical effective values and Creative restore; diffusion active');
  const knob = page.locator('[data-parameter="gain"] [role=slider]'), rect = await knob.boundingBox();
  await page.mouse.move(rect.x + rect.width / 2, rect.y + rect.height / 2); await page.mouse.down(); await page.mouse.move(rect.x + rect.width / 2, rect.y - 30, { steps: 8 }); await page.evaluate(() => window.dispatchEvent(new Event('blur'))); await page.mouse.up();
  assert.deepEqual(await page.evaluate(() => window.verbRoom.bridge.openGestures), []); pass('Knob pointer gesture ends on blur');
  await page.locator('.mock-dev>summary').click(); await page.getByRole('button', { name: 'Host automation demo', exact: true }).click(); assert.equal(await page.locator('[data-parameter="mix"] .readout').inputValue(), '37.5 %'); pass('Automation updates reflected without echo'); await page.locator('.mock-dev>summary').click();
  await page.getByRole('tab', { name: 'Settings', exact: true }).click(); assert.equal(await page.locator('.bottom').getByText('Keep I/O', { exact: true }).count(), 0); assert.equal(await page.locator('.bottom').getByRole('button', { name: '+ Source', exact: true }).count(), 0); pass('Settings has no Source / Keep I/O row');
  await page.getByRole('tab', { name: 'Sources', exact: true }).click(); await page.getByRole('button', { name: '+ Source', exact: true }).click(); await page.waitForFunction(() => window.verbRoom.store.state.document.sources.length === 3); pass('Add source acknowledged through canonical resync');
  const sourceName = page.getByRole('textbox', { name: 'Source 1 name', exact: true }); await sourceName.fill('Lead <safe>'); await sourceName.press('Tab'); await page.waitForFunction(() => window.verbRoom.store.state.document.sources[0].name === 'Lead <safe>'); assert.equal(await page.locator('script:not([src])').count(), 0); pass('Source rename treated as text and canonical state');
  await page.locator('.mock-dev>summary').click(); await page.getByRole('button', { name: 'Error / resync', exact: true }).click(); await page.waitForFunction(() => window.verbRoom.store.ready); assert.ok(await page.locator('#notification').textContent()); pass('Error and resync visible; ready restored');
  await captureAudit(page);
  await open('default-physical', 'energy'); await page.getByRole('button', { name: 'Analyze', exact: true }).click(); await page.waitForFunction(() => window.verbRoom.store.telemetry['analysis.result']?.outcome === 'done');
  assert.ok(await page.locator('.analysis-status').textContent());
  const traces = await page.evaluate(() => window.verbRoom.bridge.audit.filter(a => a.direction === 'out' && a.message.payload.op === 'analysis.start').length);
  await pickOption(page, 'Energy band', '1k Hz');
  assert.equal(await page.evaluate(() => window.verbRoom.bridge.audit.filter(a => a.direction === 'out' && a.message.payload.op === 'analysis.start').length), traces); pass('Energy component / band rendering changes do not retrace');
  await page.getByRole('button', { name: 'Analyze', exact: true }).click(); await page.waitForTimeout(90); await page.getByRole('button', { name: 'Cancel', exact: true }).click(); await page.waitForFunction(() => window.verbRoom.store.telemetry['analysis.result']?.outcome === 'cancelled'); pass('Energy cancellation acknowledged');
  await captureAudit(page);
  await open('default-physical', 'edit-outliner');
  await page.getByRole('button', { name: 'Lock wall 1', exact: true }).click(); await page.waitForFunction(() => window.verbRoom.store.state.document.scene.walls[0].editor.locked);
  await page.getByRole('button', { name: '✓ Done', exact: true }).click(); await page.getByRole('button', { name: 'Edit room ↗', exact: true }).click(); assert.equal(await page.evaluate(() => window.verbRoom.store.state.document.scene.walls[0].editor.locked), true); pass('Object lock is owned by bridge and survives page changes');
  await page.getByRole('button', { name: 'wall 2', exact: true }).click();
  const alphaBefore = await page.evaluate(() => { const s = window.verbRoom.store.state.document.scene; return s.materials[s.walls[1].material].alpha[0]; });
  await page.getByRole('slider', { name: '125 Hz absorption', exact: true }).waitFor(); await page.waitForTimeout(250);
  const alphaBar = await page.getByRole('slider', { name: '125 Hz absorption', exact: true }).boundingBox();
  await page.mouse.move(alphaBar.x + alphaBar.width / 2, alphaBar.y + alphaBar.height / 2); await page.mouse.down(); await page.mouse.move(alphaBar.x + alphaBar.width / 2, alphaBar.y + alphaBar.height / 2 - 15, { steps: 5 }); await page.mouse.up();
  await page.waitForFunction(value => { const s = window.verbRoom.store.state.document.scene; return s.materials[s.walls[1].material].alpha[0] > value; }, alphaBefore);
  await page.screenshot({ path: `${root}/shots/edit-surface-1280x800.png` }); report.screenshots.push('edit-surface-1280x800.png'); pass('Absorption bar transaction updates canonical cloned material');
  await page.getByRole('button', { name: 'Plan', exact: true }).click();
  const [sourcePosition, ...aimPoints] = await page.evaluate(async () => {
    const { project } = await import('/src/viewport/camera.js');
    const { store } = window.verbRoom;
    const canvas = document.querySelector('.viewport canvas');
    const rect = canvas.getBoundingClientRect();
    const { s1x, s1y, s1z } = store.state.parameters.values;
    return [[0, 0], [1, 0], [0, 1]].map(([dx, dy]) => {
      const [x, y] = project([s1x + dx, s1y + dy, s1z], store.view.camera, canvas.clientWidth, canvas.clientHeight);
      return [rect.x + x, rect.y + y];
    });
  });
  // RMB aims the selected object toward the cursor. A left click selects the source.
  await page.mouse.click(...sourcePosition);
  await page.waitForFunction(() => window.verbRoom.store.view.selection.some(ref => ref.kind === 'source' && ref.index === 0));
  for (const [index, point] of aimPoints.entries()) {
    const expectedYaw = index * 90;
    await page.mouse.move(...sourcePosition);
    await page.mouse.down({ button: 'right' });
    await page.mouse.move(...point, { steps: 5 });
    await page.mouse.up({ button: 'right' });
    await page.waitForFunction(yaw => Math.abs(window.verbRoom.store.state.parameters.values.s1az - yaw) < 2, expectedYaw);
    assert.deepEqual(await page.evaluate(() => window.verbRoom.bridge.openGestures), []);
  }
  pass('Repeated RMB drags aim the selected source at distinct targets and close gestures');
  await page.getByRole('button', { name: '3D', exact: true }).click(); const canvas = page.locator('.viewport canvas'), canvasRect = await canvas.boundingBox();
  const before = await page.evaluate(() => window.verbRoom.store.view.camera);
  await page.mouse.move(canvasRect.x + canvasRect.width * 0.6, canvasRect.y + canvasRect.height * 0.6); await page.mouse.down({ button: 'middle' }); await page.mouse.move(canvasRect.x + canvasRect.width * 0.6 + 40, canvasRect.y + canvasRect.height * 0.6 + 20, { steps: 5 }); await page.mouse.up({ button: 'middle' });
  const after = await page.evaluate(() => window.verbRoom.store.view.camera); assert.notDeepEqual(after.target, before.target); pass('3D middle-button pan changes camera target');
  await page.locator('.nav-tools>summary').click(); await page.getByRole('button', { name: 'Frame all', exact: true }).click(); await captureAudit(page);
  // Verify device pixels and intermediate layouts, not only the two requested snapshots.
  for (const [width, height] of [[900, 760], [1160, 760], [1600, 900]]) { await page.setViewportSize({ width, height }); assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true); }
  pass('Intermediate layout widths hold');
  const retina = await browser.newPage({ viewport: { width: 900, height: 600 }, deviceScaleFactor: 2 }); await retina.goto(url); await retina.waitForFunction(() => { const c = document.querySelector('.viewport canvas'); return window.verbRoom?.store.ready && c && Math.abs(c.width - c.clientWidth * 2) < 3; }); await retina.close(); pass('DPR 2 canvas backing store');
  assert.deepEqual(report.errors, []); assert.deepEqual(report.externalRequests, []);
  report.schemaMessages = audit.filter(a => ['in', 'out'].includes(a.direction)).length;
  await writeFile(`${root}/tests/browser-audit.json`, JSON.stringify(audit));
} catch (error) { report.failure = error.stack; console.error(error); process.exitCode = 1; }
finally { if (browser) await browser.close(); await new Promise(resolve => server.close(resolve)); await writeFile(`${root}/tests/browser-report.json`, JSON.stringify(report, null, 2)); }
