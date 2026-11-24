import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { createRequire } from 'node:module';
import { serve } from '../scripts/serve.mjs';
import { pickOption } from './browser-helpers.mjs';

const require = createRequire(import.meta.url);
let playwright;
for (const name of [process.env.PLAYWRIGHT_MODULE, 'playwright', 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'].filter(Boolean)) {
  try { playwright = require(name); break; } catch {}
}
assert.ok(playwright, 'Set PLAYWRIGHT_MODULE to a Playwright installation');
const executablePath = process.env.CHROME_PATH || [
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
].find(existsSync);
const { server, url } = await serve();
let browser;
const defects = [];
let checkCount = 0, screenCount = 0;
try {
  browser = await playwright.chromium.launch({ headless: true, executablePath });
  for (const [width, height] of [[1000, 600], [900, 560], [900, 600], [1280, 720], [1920, 1080]]) {
    const page = await browser.newPage({ viewport: { width, height }, reducedMotion: 'reduce' });
    page.setDefaultTimeout(5000);
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.goto(`${url}/?fixture=large-hall`);
    await page.waitForFunction(() => window.verbRoom?.store.ready && document.querySelector('.macro-row'));
    const view = patch => page.evaluate(patch => window.verbRoom.store.setView(patch, false), patch);
    const command = cmd => page.evaluate(cmd => window.verbRoom.store.command(cmd), cmd);
    const audit = async (label, expected) => {
      await page.locator(expected).first().waitFor({ state: 'attached' });
      const { count, failures } = await page.evaluate(() => {
        const selectors = 'button,input,select,summary,[role=slider],.knob,[role=option],.path-bake-source,.path-bake-status,.path-recording-note,.path-bake-caption';
        const failures = [], tolerance = 2;
        const style = e => getComputedStyle(e);
        const visible = e => {
          if (!e.getClientRects().length || style(e).visibility !== 'visible' || e.closest('[hidden],[inert]')) return false;
          for (let details = e.closest('details:not([open])'); details; details = details.parentElement.closest('details:not([open])'))
            if (!details.querySelector(':scope > summary')?.contains(e)) return false;
          return true;
        };
        const elements = [...document.querySelectorAll(selectors)].filter(visible);
        const describe = e => e.getAttribute('aria-label') || e.title || e.textContent.trim().slice(0, 70) || e.className;
        const scrollable = value => value === 'auto' || value === 'scroll';
        const clipBox = parent => {
          const r = parent.getBoundingClientRect(), x = r.left + parent.clientLeft, y = r.top + parent.clientTop;
          return { left: x, top: y, right: x + parent.clientWidth, bottom: y + parent.clientHeight };
        };
        for (const e of elements) {
          const ancestors = [];
          for (let a = e.parentElement; a; a = a.parentElement) {
            ancestors.push(a);
            if (style(a).position === 'fixed') break;
          }
          const positions = [];
          // Use only containers the user can scroll. scrollIntoView() also scrolls
          // overflow:hidden boxes and would hide regressions in clipped controls.
          for (const a of ancestors) {
            const s = style(a), b = clipBox(a), r = e.getBoundingClientRect();
            const dx = r.right > b.right ? r.right - b.right : Math.min(0, r.left - b.left);
            const dy = r.bottom > b.bottom ? r.bottom - b.bottom : Math.min(0, r.top - b.top);
            if (scrollable(s.overflowX) || scrollable(s.overflowY)) {
              positions.push([a, a.scrollLeft, a.scrollTop]);
              if (scrollable(s.overflowX)) a.scrollLeft += dx;
              if (scrollable(s.overflowY)) a.scrollTop += dy;
            }
          }
          const r = e.getBoundingClientRect();
          const outsideX = b => r.left < b.left - tolerance || r.right > b.right + tolerance;
          const outsideY = b => r.top < b.top - tolerance || r.bottom > b.bottom + tolerance;
          const blocking = ancestors.find(a => {
            const s = style(a), b = clipBox(a);
            return (s.overflowX !== 'visible' && outsideX(b)) || (s.overflowY !== 'visible' && outsideY(b));
          });
          const viewport = { left: 0, top: 0, right: innerWidth, bottom: innerHeight };
          if (blocking || outsideX(viewport) || outsideY(viewport) || !r.width || !r.height)
            failures.push(`${describe(e)} → ${blocking?.className || 'viewport'}`);
          for (const [a, x, y] of positions) { a.scrollLeft = x; a.scrollTop = y; }
        }
        for (const e of document.querySelectorAll('button .dd-text, button.check span, .macro-row .parameter-name, .macro-row .knob-label, .row > span:first-child, .dd-option')) {
          if (!visible(e)) continue;
          const clipped = e.scrollWidth > e.clientWidth + tolerance || e.scrollHeight > e.clientHeight + tolerance;
          if (clipped && !e.closest('[title]:not([title=""])')) failures.push(`label: ${describe(e)}`);
        }
        return { count: elements.length, failures };
      });
      assert.ok(count > 20, label);
      console.log(`${width}×${height} ${label}: ${count} controls${failures.length ? ` · ${[...new Set(failures)].join(' | ')}` : ''}`);
      checkCount += count; ++screenCount;
      if (failures.length) defects.push({ width, height, label, failures: [...new Set(failures)] });
    };

    for (const [tab, selector] of [['decay', '.bottom-decay'], ['sources', '.sources-panel'], ['mix', '.mix-columns'], ['modulation', '.mod-panel'], ['output', '.output-panel']]) {
      await view({ page: 'main', tab, selection: [] });
      await audit(`Main / ${tab}, Scene, Reverb time, Listen, Macros`, selector);
    }
    await page.evaluate(() => {
      const { bridge } = window.verbRoom;
      bridge.parameters.begin('mode'); bridge.parameters.change('mode', 1); bridge.parameters.end('mode');
    });
    await command({
      op: 'modulation.addRoute',
      route: {
        enabled: true, object: 0, driver: 'distanceToPoint', point: [2, 3, 1.5],
        curve: { inputRange: [0, 12], outputRange: [20000, 1000], invert: false, clamp: true, nodes: [{ x: 0, y: 0, smooth: true }, { x: 0.5, y: 0.4, smooth: true }, { x: 1, y: 1, smooth: false }] },
        targetObject: -1, target: 'sourceLowPass', attackMs: 15, releaseMs: 80
      }
    });
    await view({ page: 'main', tab: 'modulation', selection: [{ kind: 'source', index: 0 }], modRoute: 0, modEditorOpen: false });
    await audit('Main / Modulation active route + point + curve (docked editor)', '.bottom-modulation .mod-dock-editor-col .mod-curve-svg');
    await view({ modEditorOpen: true });
    await audit('Main / Modulation active route + point + curve (detached editor)', '.mod-editor-panel .mod-curve-svg');
    await page.evaluate(() => {
      const { bridge } = window.verbRoom;
      bridge.parameters.begin('mode'); bridge.parameters.change('mode', 0); bridge.parameters.end('mode');
    });
    await audit('Main / Modulation Physical detached editor', '.mod-editor-panel .mod-mode-banner');
    await view({ modEditorOpen: false });
    await command({ op: 'modulation.setRoutes', routes: [] });
    await view({ tab: 'decay' });
    await page.locator('.decay-effective summary').click();
    await audit('Main / Decay, effective diagnostics', '.decay-effective[open]');
    await view({ tab: 'output' });
    await page.locator('.theme-menu summary').click();
    await audit('Theme panel', '.theme-menu[open] button[aria-label="Close theme"]');
    await page.getByRole('button', { name: 'Close theme' }).click();
    await page.getByRole('button', { name: 'About' }).click();
    await audit('About dialog', 'dialog.about-dialog[open] .about-kofi-btn');
    await page.keyboard.press('Escape');
    await page.locator('dialog.about-dialog').waitFor({ state: 'hidden', timeout: 5000 });
    await pickOption(page, 'Output layout', 'AmbiX (3rd order)');
    await page.getByRole('combobox', { name: 'Ambisonics normalization' }).waitFor();
    await audit('Main / Settings, Ambisonics', '.output-panel [aria-label="Ambisonics level"]');
    await pickOption(page, 'Ambisonics normalization', 'FuMa');
    await page.waitForFunction(() => window.verbRoom.store.state.document.output.ambisonicsNormalization === 2);
    await pickOption(page, 'Ambisonics level', '−6 dB');
    await page.waitForFunction(() => window.verbRoom.store.state.document.output.ambisonicsLevelDb === -6 && window.verbRoom.store.state.document.output.ambisonicsNormalization === 2);
    await command({ op: 'history.undo' });
    await page.waitForFunction(() => window.verbRoom.store.state.document.output.ambisonicsLevelDb === 0);
    const layout = page.getByRole('combobox', { name: 'Output layout', exact: true });
    await layout.focus();
    await layout.press('Home').catch(() => {});
    await pickOption(page, 'Output layout', 'Stereo (ORTF)');
    await page.getByRole('combobox', { name: 'Ambisonics level' }).waitFor({ state: 'detached' });
    assert.equal(await page.evaluate(() => window.verbRoom.store.state.document.output.ambisonicsNormalization), 2);
    await command({ op: 'output.set', layout: 11 });
    await view({ tab: 'sources', selection: [{ kind: 'listener', index: 0 }] });
    await audit('Main / Listener', '.selected-card [aria-label="Ambisonics normalization"]');
    await view({ selection: [{ kind: 'source', index: 0 }] });
    await audit('Main / Source and path actions', '.selected-card .path-actions');
    await page.getByRole('button', { name: 'Source 1 colour', exact: true }).click();
    await audit('Source colour dialog', '.source-color-popover');
    await page.keyboard.press('Escape');
    await page.getByRole('combobox', { name: 'Room preset' }).click();
    await audit('Room preset popover', '.dd-popup');
    await page.keyboard.press('Escape');
    await command({ op: 'path.set', object: 0, points: [[1, 1, 1], [2, 2, 1], [3, 2, 2], [4, 1, 2]],
      enabled: true, smooth: true, start: 14, end: 90, laps: 3 });
    await view({ pathPoint: { object: 0, index: 1 } });
    await audit('Main / Populated path, point timing and bake', '.path-pt-timing');
    await page.evaluate(() => window.verbRoom.bridge.setBakeStatus({ passState: 3, passSamples: 4096 }));
    await audit('Main / Populated path with a kept recording', '.path-recording-note:not([hidden])');
    await page.evaluate(() => {
      const { bridge } = window.verbRoom;
      bridge.parameters.begin('mode'); bridge.parameters.change('mode', 1); bridge.parameters.end('mode');
    });
    await command({ op: 'path.setLfo', object: 0, lfo: { enabled: true, shape: 'sine', rateHz: 2, depth: 0.3, phaseDeg: 45, seed: 1 } });
    await audit('Main / Populated path LFO', '.path-lfo [aria-label="LFO depth"]');
    await view({ timelineOpen: true });
    await audit('Main / Populated timeline', '.timeline-panel .tl-bar');
    await command({ op: 'path.setLfo', object: 0, lfo: { enabled: false, shape: 'sine', rateHz: 0.5, depth: 0, phaseDeg: 0, seed: 1 } });
    await page.evaluate(() => {
      const { bridge } = window.verbRoom;
      bridge.parameters.begin('mode'); bridge.parameters.change('mode', 0); bridge.parameters.end('mode');
    });
    await view({ timelineOpen: false, energy: true });
    await audit('Main / Energy', '.viewport .analysis');
    await page.locator('.energy-advanced summary').click();
    await audit('Main / Energy advanced', '.energy-advanced[open]');
    await view({ energy: false, page: 'edit', selection: [] });
    await audit('Edit room / Empty selection', '.edit-inspector .empty-state');

    await command({ op: 'surface.create', kind: 'floorZone', vertices: [[1,1], [3,1], [3,3]] });
    await command({ op: 'surface.create', kind: 'ceilingZone', heightM: 2, vertices: [[1,1], [3,1], [3,3]] });
    const indices = await page.evaluate(() => {
      const { floorZones, ceilingZones } = window.verbRoom.store.state.document.scene;
      return [floorZones.length - 1, ceilingZones.length - 1];
    });
    await command({ op: 'surface.create', kind: 'wall', a: [100, 100], b: [103, 100] });
    await command({ op: 'surface.create', kind: 'wall', a: [103.04, 100], b: [103, 104] });
    await page.getByRole('button', { name: 'Outliner', exact: true }).click();
    await view({ accordion: {
      'surface.material': true, 'surface.surface': true, 'surface.scatter': true, 'surface.heights': true,
      'scene.air': true, 'scene.clutter': true, 'scene.rough': true, 'scene.scale': true,
      'source.route': true, 'source.points': true, 'listener.out': true,
    } });
    for (const [kind, index, selector] of [
      ['wall', 0, '.material-list'], ['floorZone', indices[0], '.edit-inspector .obj-title'],
      ['ceilingZone', indices[1], '.edit-inspector .obj-title'],
      ['source', 0, '.edit-inspector .path-actions'], ['listener', 0, '.edit-inspector [aria-label="Ambisonics normalization"]'],
    ]) {
      assert.ok(index >= 0, kind);
      await view({ selection: [{ kind, index }], inspectorTab: 'selected' });
      await audit(`Edit room / ${kind}, Outliner`, selector);
    }
    await view({ inspectorTab: 'scene' });
    await page.getByRole('button', { name: 'Join corners', exact: true }).waitFor();
    await audit('Edit room / Scene, Open corners and Join corners', '.edit-inspector .acc-body');
    await view({ inspectorTab: 'selected', selection: [{ kind: 'source', index: 0 }],
      timelineOpen: true, pathPoint: { object: 0, index: 2 } });
    await audit('Edit room / Populated timeline and bake', '.timeline-panel .tl-bar');
    assert.deepEqual(errors, []);
    await page.close();
  }
  console.log(`Audited ${checkCount} controls in ${screenCount} views`);
  assert.deepEqual(defects, []);
} finally {
  await browser?.close();
  server.close();
}
