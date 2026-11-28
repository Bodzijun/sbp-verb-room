import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { serve } from '../scripts/serve.mjs';
import { pickOption } from './browser-helpers.mjs';
import {
  MOD_DRIVERS, MOD_TARGETS, MOD_PRESETS,
  evaluateModCurve, applyPathLfo, computeModulationTelemetry,
  normalizeRoute, computePathLfoSwing
} from '../src/components/modulation-model.js';

const require = createRequire(import.meta.url);
const shotsDir = process.env.VERB_BROWSER_SHOTS_DIR || resolve('..', 'out', 'G8', 'screenshots');
mkdirSync(shotsDir, { recursive: true });

async function withPage(fn, viewport = { width: 1280, height: 760 }) {
  const pw = require(process.env.PLAYWRIGHT_MODULE || 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package');
  const { server, url } = await serve({ port: 0 });
  const browser = await pw.chromium.launch({
    headless: true,
    executablePath: process.env.CHROME_PATH || 'C:/Program Files/Google/Chrome/Application/chrome.exe'
  });
  const errors = [];
  try {
    const page = await browser.newPage({ viewport, reducedMotion: 'reduce' });
    page.setDefaultTimeout(5000);
    page.on('pageerror', e => errors.push(e.message));
    await page.goto(url);
    await page.waitForFunction(() => window.verbRoom?.store?.ready);
    await fn(page);
    assert.deepEqual(errors, []);
  } finally {
    await browser.close();
    server.close?.();
  }
}

async function pickScopedOption(container, name, optionText) {
  await container.getByRole('combobox', { name, exact: true }).click();
  await container.page().locator('.dd-popup [role=option]', { hasText: optionText }).first().click();
}

test('modulation-model exports 14 drivers, 9 targets (incl. none), safe range clamping, Path LFO swing & math parity', () => {
  assert.equal(MOD_DRIVERS.length, 14);
  assert.equal(MOD_TARGETS.length, 9);
  assert.equal(MOD_TARGETS.at(-1)?.id, 'none');
  assert.equal(MOD_PRESETS.length, 5);
  assert.equal(MOD_DRIVERS.find(d => d.id === 'pathPosition')?.inLabel, 'In (path 0–1)');

  // G7 safe output clamping per target bounds
  const clampedGain = normalizeRoute({
    target: 'sourceGain',
    curve: { inputRange: [0, 10], outputRange: [20000, -120] }
  }, 4);
  assert.deepEqual(clampedGain.curve.outputRange, [12, -60]);

  const clampedMix = normalizeRoute({
    target: 'mix',
    curve: { inputRange: [0, 1], outputRange: [-250, 300] }
  }, 4);
  assert.deepEqual(clampedMix.curve.outputRange, [-100, 100]);

  const clampedLpf = normalizeRoute({
    target: 'sourceLowPass',
    curve: { inputRange: [0, 10], outputRange: [5, 30000] }
  }, 4);
  assert.deepEqual(clampedLpf.curve.outputRange, [20, 20000]);

  // Path LFO swing computation & safe depth guarantee
  const safeSw = computePathLfoSwing(0.5, 0.15, false);
  assert.equal(safeSw.leavesEnds, false);
  assert.equal(safeSw.text, 'Swing 35–65 %');

  const openClip = computePathLfoSwing(0.85, 0.30, false);
  assert.equal(openClip.leavesEnds, true);
  assert.equal(openClip.text, 'Swing 55–100 %');
  assert.equal(openClip.safeDepth, 0.15);
  assert.equal(computePathLfoSwing(0.85, openClip.safeDepth, false).leavesEnds, false);

  const closedWrap = computePathLfoSwing(0.85, 0.30, true);
  assert.equal(closedWrap.leavesEnds, true);
  assert.equal(closedWrap.text, 'Swing 55–15 % (wraps)');

  const smoothCurve = {
    inputRange: [0, 10],
    outputRange: [100, 200],
    invert: false,
    clamp: true,
    nodes: [{ x: 0, y: 0, smooth: true }, { x: 1, y: 1, smooth: false }]
  };
  assert.equal(evaluateModCurve(smoothCurve, -2), 100);
  assert.equal(evaluateModCurve(smoothCurve, 5), 150);
  assert.equal(evaluateModCurve(smoothCurve, 15), 200);

  const unclampedInv = {
    inputRange: [0, 10],
    outputRange: [0, 100],
    invert: true,
    clamp: false,
    nodes: [{ x: 0, y: 0, smooth: false }, { x: 1, y: 1, smooth: false }]
  };
  assert.ok(Math.abs(evaluateModCurve(unclampedInv, 0) - 100) < 1e-6);
  assert.ok(Math.abs(evaluateModCurve(unclampedInv, -2) - 120) < 1e-6);

  assert.equal(applyPathLfo({ enabled: true, shape: 'square', rateHz: 1, depth: 0.4, phaseDeg: 0, seed: 1 }, 0.1, 0.75, false), 0);
  const wrapped = applyPathLfo({ enabled: true, shape: 'square', rateHz: 1, depth: 0.4, phaseDeg: 0, seed: 1 }, 0.1, 0.75, true);
  assert.ok(Math.abs(wrapped - 0.7) < 1e-9);
  const sr1 = applyPathLfo({ enabled: true, shape: 'smoothRandom', rateHz: 2, depth: 0.25, phaseDeg: 10, seed: 42 }, 0.5, 0.37, false);
  const sr2 = applyPathLfo({ enabled: true, shape: 'smoothRandom', rateHz: 2, depth: 0.25, phaseDeg: 10, seed: 42 }, 0.5, 0.37, false);
  assert.equal(sr1, sr2);

  const fakeState = {
    document: {
      sources: [{ name: 'S1' }],
      paths: [],
      scene: { heightM: 4, ceilingMaterial: 0, walls: [] },
      modulation: { routes: [MOD_PRESETS[0].build(0)] }
    },
    parameters: { values: { mode: 1, s1x: 4, s1y: 2, s1z: 1.5, lx: 1, ly: 2, lz: 1.5, laz: 0 } }
  };
  const telCr = computeModulationTelemetry(fakeState, 0);
  assert.equal(telCr.routes[0].active, true);
  assert.ok(Math.abs(telCr.routes[0].raw - 3) < 1e-6);
  assert.ok(telCr.routes[0].value < 20000 && telCr.routes[0].value > 1200);

  fakeState.parameters.values.mode = 0;
  const telPh = computeModulationTelemetry(fakeState, 0);
  assert.equal(telPh.routes[0].active, false);
});

test('G7 Modulation UI: docked editor in tab, no auto-open, Mod toolbar toggle, target/driver safety & 1-step undo', { timeout: 60000 }, async () => {
  await withPage(async page => {
    const tab = page.locator('.bottom-modulation');
    const win = page.locator('.mod-editor-panel');
    const modToggle = page.locator('.viewport-toolbar [data-focus-key="viewport-mod-toggle"]');

    await modToggle.waitFor();
    assert.equal(await modToggle.getAttribute('aria-pressed'), 'false');

    // Modulation tab houses the full docked editor even in Physical mode; floating window closed by default
    await page.getByRole('tab', { name: 'Modulation' }).click();
    await tab.locator('.mod-panel.is-docked').waitFor();
    await tab.locator('.mod-mode-banner').getByText('Modulation works in Creative mode').waitFor();
    assert.equal(await tab.getByRole('button', { name: 'Add modulation route' }).isDisabled(), true);
    assert.equal(await tab.locator('.mod-dock-editor-col').count(), 1);
    assert.equal(await win.count(), 0, 'floating panel closed by default');
    await page.screenshot({ path: resolve(shotsDir, '01_docked_empty_physical.png') });

    // Toolbar Mod button opens floating panel in Physical mode; Esc closes and returns focus to Mod button
    await modToggle.focus();
    await page.keyboard.press('Enter');
    await win.waitFor();
    assert.equal(await modToggle.getAttribute('aria-pressed'), 'true');
    assert.equal(await win.evaluate(el => el.contains(document.activeElement)), true, 'focus moves inside panel on open');
    assert.equal(await tab.locator('.mod-panel.is-detached').count(), 1);
    await win.locator('.mod-mode-banner').getByText('Modulation works in Creative mode').waitFor();
    await page.keyboard.press('Escape');
    await win.waitFor({ state: 'detached' });
    assert.equal(await modToggle.evaluate(el => document.activeElement === el), true, 'Esc returns focus to toolbar Mod toggle');
    await tab.locator('.mod-panel.is-docked').waitFor();

    // Switch to Creative mode
    await page.evaluate(() => {
      const { bridge } = window.verbRoom;
      bridge.parameters.begin('mode');
      bridge.parameters.change('mode', 1);
      bridge.parameters.end('mode');
    });
    await tab.locator('.mod-mode-banner').waitFor({ state: 'detached' });
    assert.equal(await tab.getByRole('button', { name: 'Add modulation route' }).isDisabled(), false);

    // Add from preset MUST NOT auto-open floating window; adds a new route, highlights it & shows status message
    assert.equal(await tab.locator('.mod-preset-dd .dd-text').textContent(), 'Add from preset…');
    await pickScopedOption(tab, 'Add from preset', 'Behind the listener → low-pass');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation?.routes?.length === 1);
    await tab.locator('.mod-dock-editor-col .mod-curve-svg').waitFor();
    assert.equal(await win.count(), 0, 'Preset add must not auto-open floating window');
    assert.equal(await tab.locator('.mod-budget').textContent(), '1 / 64 routes');
    assert.equal(await tab.locator('.mod-dock-editor-col .mod-curve-node').count(), 5);
    assert.equal(await tab.locator('.mod-route-item[data-route-index="0"]').getAttribute('data-just-added'), 'true');
    assert.equal(await page.locator('.footer-status').textContent(), 'Added route: Azimuth to listener → Low-pass');

    // Raise bottom panel with splitter and verify complete docked editor fits cleanly without overflow/scroll
    const spBox = await page.locator('.inspector-splitter').boundingBox();
    await page.mouse.move(spBox.x + spBox.width / 2, spBox.y + spBox.height / 2);
    await page.mouse.down();
    await page.mouse.move(spBox.x + spBox.width / 2, 120, { steps: 10 });
    await page.mouse.up();
    await page.waitForTimeout(120);
    const raisedFits = await page.evaluate(() => {
      const v = document.querySelector('.bottom-modulation .mod-scroll-view');
      const svg = document.querySelector('.bottom-modulation .mod-curve-svg')?.getBoundingClientRect();
      return {
        scrollH: Math.round(v.scrollHeight),
        clientH: Math.round(v.clientHeight),
        svgW: Math.round(svg?.width || 0),
        svgH: Math.round(svg?.height || 0)
      };
    });
    assert.ok(raisedFits.svgW >= 240 && raisedFits.svgH >= 112, `Docked SVG size unexpected: ${JSON.stringify(raisedFits)}`);
    assert.ok(raisedFits.scrollH <= raisedFits.clientH + 4, `Raised bottom panel should fit without scroll: ${JSON.stringify(raisedFits)}`);
    await page.screenshot({ path: resolve(shotsDir, '02_docked_raised_splitter_creative.png') });

    // Edit output min immediately before target switch -> verify atomic 1-step reset + 1-step undo
    const outMinBox = tab.getByRole('textbox', { name: 'Output min', exact: true });
    await outMinBox.dblclick();
    await outMinBox.fill('18000');
    await outMinBox.press('Enter');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].curve.outputRange[0] === 18000);

    const undoBeforeTarget = await page.evaluate(() => window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0);
    await pickScopedOption(tab, 'Modulation target', 'Source gain Δ');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].target === 'sourceGain');

    const afterGainSwitch = await page.evaluate(() => window.verbRoom.store.state.document.modulation.routes[0]);
    assert.deepEqual(afterGainSwitch.curve.outputRange, [0, -24]);
    assert.deepEqual(afterGainSwitch.curve.nodes, [{ x: 0, y: 0, smooth: false }, { x: 1, y: 1, smooth: false }]);
    assert.equal(afterGainSwitch.curve.invert, false);
    assert.equal(afterGainSwitch.curve.clamp, true);
    assert.equal(await tab.locator('.mod-ranges-card .mod-lbl', { hasText: 'Out (dB)' }).count(), 1);
    const undoAfterTarget = await page.evaluate(() => window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0);
    assert.equal(undoAfterTarget, undoBeforeTarget + 1, 'Target switch after range edit must keep its own 1 undo step');

    // 1-step undo restores previous target (sourceLowPass), [18000, 1400], and all 5 nodes
    await page.evaluate(() => window.verbRoom.store.command({ op: 'history.undo' }));
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].target === 'sourceLowPass');
    const restored = await page.evaluate(() => window.verbRoom.store.state.document.modulation.routes[0]);
    assert.deepEqual(restored.curve.outputRange, [18000, 1400]);
    assert.equal(restored.curve.nodes.length, 5);

    // Switch back to Source gain and verify UI clamps typed out-of-range value 20000 -> 12 dB
    await pickScopedOption(tab, 'Modulation target', 'Source gain Δ');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].target === 'sourceGain');
    const outMaxBox = tab.getByRole('textbox', { name: 'Output max', exact: true });
    await outMaxBox.dblclick();
    await outMaxBox.fill('20000');
    await outMaxBox.press('Enter');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].curve.outputRange[1] === 12);

    // Driver switch resets inputRange to driver defaults while keeping outputRange, and shows clear pathPosition label
    await pickScopedOption(tab, 'Modulation target', 'Wet / dry mix Δ');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].target === 'mix');
    assert.equal(await tab.getByRole('combobox', { name: 'Target object' }).count(), 0);
    await pickScopedOption(tab, 'Modulation driver', 'Path position');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].driver === 'pathPosition');
    const pathRoute = await page.evaluate(() => window.verbRoom.store.state.document.modulation.routes[0]);
    assert.deepEqual(pathRoute.curve.inputRange, [0, 1]);
    assert.deepEqual(pathRoute.curve.outputRange, [0, -25]);
    const pathLbl = tab.locator('.mod-ranges-card .mod-lbl', { hasText: 'In (path 0–1)' });
    await pathLbl.waitFor();
    assert.match(await pathLbl.getAttribute('title'), /Path progress: 0 = start, 1 = end/i);

    await pickScopedOption(tab, 'Modulation driver', 'Speed');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].driver === 'speed');
    const spdRoute = await page.evaluate(() => window.verbRoom.store.state.document.modulation.routes[0]);
    assert.deepEqual(spdRoute.curve.inputRange, [0, 10]);
    assert.deepEqual(spdRoute.curve.outputRange, [0, -25]);

    // G8: select Host out slot + Host output only ('none') target in docked editor, check tooltip & live readout
    await pickScopedOption(tab, 'Host output slot', '3');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].hostOut === 3);
    const hostDdTitle = await tab.locator('.mod-dock-editor-col .mod-field-hostout').getAttribute('title');
    assert.match(hostDdTitle || '', /Mod Out 3/i);
    assert.match(hostDdTitle || '', /Link from FX parameter/i);
    const hostValText = await tab.locator('.mod-dock-editor-col .mod-host-val').textContent();
    assert.match(hostValText || '', /^0\.\d{2}$|^1\.00$/);
    await tab.locator('.mod-route-item[data-route-index="0"] .mod-row-host-tag', { hasText: '→ MO 3' }).waitFor();

    await pickScopedOption(tab, 'Modulation target', 'Host output only');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].target === 'none');
    await tab.locator('.mod-dock-editor-col .mod-hostout-note', { hasText: 'Host out' }).waitFor();
    await page.screenshot({ path: resolve(shotsDir, 'g8_host_out_docked.png') });
    await pickScopedOption(tab, 'Host output slot', '—');
    await page.waitForFunction(() => !window.verbRoom.store.state.document.modulation.routes[0].hostOut);
    await pickScopedOption(tab, 'Modulation target', 'Wet / dry mix Δ');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].target === 'mix');

    // 1-step curve drag undo works directly inside docked editor
    const dragUndoBefore = await page.evaluate(() => window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0);
    const dockNodeLoc = tab.locator('.mod-dock-editor-col .mod-node-g[data-node-index="1"] .mod-curve-node');
    await dockNodeLoc.scrollIntoViewIfNeeded();
    const dnb = await dockNodeLoc.boundingBox();
    const beforeDockY = await page.evaluate(() => window.verbRoom.store.state.document.modulation.routes[0].curve.nodes[1].y);
    await page.mouse.move(dnb.x + dnb.width / 2, dnb.y + dnb.height / 2);
    await page.mouse.down();
    await page.mouse.move(dnb.x + dnb.width / 2, dnb.y + dnb.height / 2 + 28, { steps: 5 });
    await page.mouse.up();
    await page.waitForFunction(y0 => window.verbRoom.store.state.document.modulation.routes[0].curve.nodes[1].y !== y0, beforeDockY);
    await page.waitForFunction(b => (window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0) > b, dragUndoBefore);
    assert.equal(await page.evaluate(() => window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0), dragUndoBefore + 1);

    // Duplicate route -> double-click route 0 ONLY selects it, does NOT auto-open floating window
    await tab.getByRole('button', { name: 'Duplicate route 1' }).click();
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes.length === 2);
    assert.equal(await tab.locator('.mod-route-item[data-route-index="1"].selected').count(), 1);
    await tab.locator('.mod-route-item[data-route-index="0"]').dblclick();
    await page.waitForFunction(() => window.verbRoom.store.view.modRoute === 0);
    assert.equal(await win.count(), 0, 'Double-click must only select route and never auto-open floating window');

    // Add 3 routes with different drivers to similar filters -> verify distinct, complete Driver → Target labels, state badge & separate live target readout
    await page.evaluate(async () => {
      const { store } = window.verbRoom;
      await store.command({
        op: 'modulation.setRoutes',
        routes: [
          {
            enabled: true, object: 0, driver: 'pathPosition',
            curve: { inputRange: [0, 1], outputRange: [20000, 1400], invert: false, clamp: true, nodes: [{ x: 0, y: 0, smooth: false }, { x: 1, y: 1, smooth: false }] },
            targetObject: -1, target: 'sourceLowPass', attackMs: 15, releaseMs: 80
          },
          {
            enabled: true, object: 0, driver: 'speed',
            curve: { inputRange: [0, 10], outputRange: [0, -6], invert: false, clamp: true, nodes: [{ x: 0, y: 0, smooth: false }, { x: 1, y: 1, smooth: false }] },
            targetObject: -1, target: 'sourceHighShelf', attackMs: 20, releaseMs: 100
          },
          {
            enabled: false, object: 0, driver: 'distanceToListener',
            curve: { inputRange: [1, 15], outputRange: [20000, 1400], invert: false, clamp: true, nodes: [{ x: 0, y: 0, smooth: false }, { x: 1, y: 1, smooth: false }] },
            targetObject: -1, target: 'sourceLowPass', attackMs: 15, releaseMs: 80
          }
        ]
      });
      await store.setView({ modRoute: 0 }, false);
    });
    await tab.locator('.mod-route-item[data-route-index="2"]').waitFor();
    const rowChecks = await page.evaluate(() => {
      return [...document.querySelectorAll('.bottom-modulation .mod-route-item')].map(el => {
        const titleEl = el.querySelector('.mod-row-title');
        const stateEl = el.querySelector('.mod-row-state');
        const valEl = el.querySelector('.mod-row-readout');
        return {
          title: titleEl?.textContent,
          fullTooltip: el.getAttribute('title'),
          textOverflow: getComputedStyle(titleEl).textOverflow,
          overflowsHoriz: titleEl.scrollWidth > titleEl.clientWidth + 2,
          state: stateEl?.textContent,
          val: valEl?.textContent
        };
      });
    });
    assert.deepEqual(rowChecks.map(r => r.title), [
      'Path position → Low-pass',
      'Speed → High shelf',
      'Distance to listener → Low-pass'
    ]);
    for (const rc of rowChecks) {
      assert.notEqual(rc.textOverflow, 'ellipsis', `Route title must not use ellipsis: ${rc.title}`);
      assert.equal(rc.overflowsHoriz, false, `Route title overflows horizontally: ${rc.title}`);
      assert.ok(rc.fullTooltip && rc.fullTooltip.includes('→'), `Missing full tooltip on ${rc.title}`);
      assert.match(rc.val, /(?:Hz|kHz|dB|%)/, `Live target value must include units: ${rc.val}`);
    }
    assert.deepEqual(rowChecks.map(r => r.state), ['Active', 'Active', 'Off']);

    // Selecting from "Add from preset…" appends a NEW route (keeps route 0 unchanged), selects & highlights it + shows footer status
    const beforePresetAdd0 = await page.evaluate(() => JSON.stringify(window.verbRoom.store.state.document.modulation.routes[0]));
    await pickScopedOption(tab, 'Add from preset', 'Speed → high shelf');
    await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes.length === 4);
    assert.equal(await page.evaluate(() => JSON.stringify(window.verbRoom.store.state.document.modulation.routes[0])), beforePresetAdd0, 'Add from preset must append without overwriting selected route');
    assert.equal(await page.evaluate(() => window.verbRoom.store.view.modRoute), 3);
    assert.equal(await tab.locator('.mod-route-item[data-route-index="3"].selected').count(), 1);
    assert.equal(await tab.locator('.mod-route-item[data-route-index="3"]').getAttribute('data-just-added'), 'true');
    assert.equal(await page.locator('.footer-status').textContent(), 'Added route: Speed → High shelf');

    // Manual Detach ↗ opens floating window & keeps route selection + Edit room Mod toggle working
    const detachBtn = tab.getByRole('button', { name: 'Detach modulation editor into floating window' });
    await detachBtn.click();
    await win.waitFor();
    assert.equal(await win.locator('.mod-preset-dd .dd-text').textContent(), 'Add from preset…');
    assert.equal(await tab.locator('.mod-panel.is-detached').count(), 1);
    assert.equal(await win.locator('.mod-route-item[data-route-index="3"].selected').count(), 1);
    await win.locator('.mod-route-item[data-route-index="1"]').click();
    await page.waitForFunction(() => window.verbRoom.store.view.modRoute === 1);
    assert.equal(await tab.locator('.mod-route-item[data-route-index="1"].selected').count(), 1);

    // Edit room keeps Mod toolbar toggle and optional floating window functional
    await page.getByRole('button', { name: 'Edit room ↗' }).click();
    await modToggle.waitFor();
    assert.equal(await win.count(), 1);
    assert.equal(await modToggle.getAttribute('aria-pressed'), 'true');
    await page.screenshot({ path: resolve(shotsDir, '03_edit_room_mod_toggle.png') });
    await modToggle.click();
    await win.waitFor({ state: 'detached' });
    assert.equal(await modToggle.getAttribute('aria-pressed'), 'false');
    await modToggle.click();
    await win.waitFor();

    // Back to Main -> Dock ↙ restores docked editor
    await page.getByRole('button', { name: '✓ Done' }).click();
    await tab.locator('.mod-panel.is-detached').waitFor();
    await tab.getByRole('button', { name: 'Dock modulation editor back into Modulation tab' }).last().click();
    await win.waitFor({ state: 'detached' });
    await tab.locator('.mod-dock-editor-col .mod-curve-svg').waitFor();
  });
});

test('G7 Floating editor drag, resize, curve growth, viewport clamp, collapse, and view-state-only persistence', { timeout: 60000 }, async () => {
  await withPage(async page => {
    await page.evaluate(async () => {
      const { store, bridge } = window.verbRoom;
      bridge.parameters.begin('mode'); bridge.parameters.change('mode', 1); bridge.parameters.end('mode');
      await store.command({ op: 'modulation.addRoute', route: {
        enabled: true, object: 0, driver: 'distanceToListener',
        curve: { inputRange: [1, 15], outputRange: [20000, 1200], invert: false, clamp: true, nodes: [{ x: 0, y: 0, smooth: true }, { x: 1, y: 1, smooth: false }] },
        targetObject: -1, target: 'sourceLowPass', attackMs: 15, releaseMs: 80
      } });
      await store.setView({ page: 'main', tab: 'modulation', modRoute: 0, modEditorOpen: true });
    });

    const win = page.locator('.mod-editor-panel');
    await win.waitFor();
    const insideViewport = async () => page.evaluate(() => {
      const p = document.querySelector('.mod-editor-panel')?.getBoundingClientRect();
      const v = document.querySelector('.viewport')?.getBoundingClientRect();
      return !!p && !!v && p.left >= v.left - 1 && p.right <= v.right + 1 && p.top >= v.top - 1 && p.bottom <= v.bottom + 1;
    });
    assert.equal(await insideViewport(), true);

    const beforeBox = await win.boundingBox();
    const curveBefore = await win.locator('.mod-curve-svg').boundingBox();

    const hdr = await win.locator('.mod-win-header b').boundingBox();
    await page.mouse.move(hdr.x + 8, hdr.y + 5);
    await page.mouse.down();
    await page.mouse.move(hdr.x + 36, hdr.y - 70, { steps: 6 });
    await page.mouse.up();
    await page.waitForFunction(() => !document.querySelector('[data-gesture-active]')
      && !!window.verbRoom.store.view.modEditorBox
      && !!window.verbRoom.store.state.preferences?.sessionViewState?.modEditorBox);

    const movedBox = await win.boundingBox();
    assert.ok(movedBox.x > beforeBox.x + 15 && movedBox.y < beforeBox.y - 40, `Expected panel move, got ${JSON.stringify([beforeBox, movedBox])}`);
    assert.equal(await insideViewport(), true);

    const persisted = await page.evaluate(() => {
      const { state } = window.verbRoom.store;
      return {
        inDoc: 'modEditorOpen' in state.document || 'modEditorBox' in state.document || 'modEditorOpen' in (state.document.modulation || {}),
        vsOpen: state.preferences?.sessionViewState?.modEditorOpen,
        vsBox: state.preferences?.sessionViewState?.modEditorBox
      };
    });
    assert.equal(persisted.inDoc, false);
    assert.equal(persisted.vsOpen, true);
    assert.ok(persisted.vsBox && (persisted.vsBox.dx !== 0 || persisted.vsBox.dy !== 0));

    for (const [tx, ty] of [[-500, 120], [3000, 120], [180, -500], [180, 3000]]) {
      const h = await win.locator('.mod-win-header b').boundingBox();
      await page.mouse.move(h.x + 8, h.y + 5);
      await page.mouse.down();
      await page.mouse.move(tx, ty, { steps: 6 });
      await page.mouse.up();
      await page.waitForFunction(() => !document.querySelector('[data-gesture-active]'));
      assert.equal(await insideViewport(), true, `Must stay inside viewport after drag to ${tx},${ty}`);
    }

    await win.locator('.mod-win-header b').dblclick();
    await page.waitForFunction(() => !window.verbRoom.store.view.modEditorBox);
    const grip = await win.locator('.mod-win-grip').boundingBox();
    await page.mouse.move(grip.x + 6, grip.y + 6);
    await page.mouse.down();
    await page.mouse.move(grip.x + 110, grip.y - 110, { steps: 8 });
    await page.mouse.up();
    await page.waitForFunction(() => !document.querySelector('[data-gesture-active]')
      && !!window.verbRoom.store.view.modEditorBox
      && !!window.verbRoom.store.state.preferences?.sessionViewState?.modEditorBox);
    assert.equal(await insideViewport(), true);
    const curveAfter = await win.locator('.mod-curve-svg').boundingBox();
    assert.ok(curveAfter.height > curveBefore.height + 25, `Expected curve height to grow with panel (${curveBefore.height} -> ${curveAfter.height})`);

    await page.setViewportSize({ width: 900, height: 560 });
    await page.waitForTimeout(200);
    assert.equal(await insideViewport(), true, 'Shrinking window to 900x560 keeps floating panel inside viewport');

    await win.getByRole('button', { name: 'Collapse modulation editor' }).click();
    await page.waitForSelector('.mod-editor-panel.collapsed');
    assert.equal(await win.locator('.mod-win-body').isVisible(), false);
    assert.ok((await win.boundingBox()).height < 48);
    await win.getByRole('button', { name: 'Collapse modulation editor' }).click();
    await page.waitForFunction(() => !document.querySelector('.mod-editor-panel.collapsed'));
    assert.equal(await win.locator('.mod-win-body').isVisible(), true);
  }, { width: 1400, height: 860 });
});

test('G7 Path LFO swing readout, end warning, Keep inside path button, 1-step undo & Edit room parity', { timeout: 60000 }, async () => {
  await withPage(async page => {
    await page.evaluate(async () => {
      const { store, bridge } = window.verbRoom;
      bridge.parameters.begin('mode'); bridge.parameters.change('mode', 1); bridge.parameters.end('mode');
      await store.command({ op: 'path.set', object: 0, points: [[1, 1, 1.5], [4, 3, 1.5], [7, 1, 1.5]], enabled: true, closed: false });
      bridge.parameters.begin('s1path'); bridge.parameters.change('s1path', 0.85); bridge.parameters.end('s1path');
      await store.setView({ selection: [{ kind: 'source', index: 0 }] });
    });

    const lfoAcc = page.locator('[data-acc-id="main.path.lfo"]');
    assert.equal(await lfoAcc.getAttribute('aria-expanded'), 'false');
    await lfoAcc.click();
    const lfoBox = page.locator('.selected-card .path-lfo');
    await lfoBox.waitFor();
    assert.equal(await lfoBox.locator('.path-lfo-swing').textContent(), 'Swing 85–85 %');
    assert.equal(await lfoBox.locator('.path-lfo-warn').isVisible(), false);

    await lfoBox.getByRole('switch', { name: 'Enable path LFO' }).click();
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].lfo?.enabled === true);
    await pickOption(page, 'LFO shape', 'Smooth random');
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].lfo?.shape === 'smoothRandom');

    // Drag depth to ~30% at Pos=85% -> leaves upper end -> warning + Keep inside path button
    const undoBefore = await page.evaluate(() => window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0);
    const depthTrack = lfoBox.getByRole('slider', { name: 'LFO depth' });
    await depthTrack.scrollIntoViewIfNeeded();
    const box = await depthTrack.boundingBox();
    await page.mouse.move(box.x + 2, box.y + box.height / 2);
    await page.mouse.down();
    await page.mouse.move(box.x + box.width * 0.30, box.y + box.height / 2, { steps: 5 });
    await page.mouse.up();
    await page.waitForFunction(() => (window.verbRoom.store.state.document.paths[0].lfo?.depth ?? 0) >= 0.25);
    await page.waitForFunction(b => (window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0) > b, undoBefore);
    assert.equal(await page.evaluate(() => window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0), undoBefore + 1);

    await lfoBox.locator('.path-lfo-warn').waitFor({ state: 'visible' });
    assert.match(await lfoBox.locator('.path-lfo-warn').textContent(), /Clipped at the path end: open path clamps, closed path wraps/);
    assert.match(await lfoBox.locator('.path-lfo-swing').textContent(), /100 %/);

    // Closed path switches live swing indicator to "(wraps)"
    await page.getByRole('switch', { name: 'Closed' }).click();
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].closed === true);
    assert.match(await lfoBox.locator('.path-lfo-swing').textContent(), /\(wraps\)/);
    await page.getByRole('switch', { name: 'Closed' }).click();
    await page.waitForFunction(() => window.verbRoom.store.state.document.paths[0].closed === false);

    // Clicking "Keep inside path" clamps depth cleanly to 15% and clears warning
    const keepBtn = lfoBox.getByRole('button', { name: /Clamp LFO depth/i });
    await keepBtn.click();
    await page.waitForFunction(() => Math.abs((window.verbRoom.store.state.document.paths[0].lfo?.depth ?? 0) - 0.15) < 1e-6);
    assert.equal(await lfoBox.locator('.path-lfo-swing').textContent(), 'Swing 70–100 %');
    assert.equal(await lfoBox.locator('.path-lfo-warn').isVisible(), false);
    assert.equal(await keepBtn.isVisible(), false);

    // Moving Position live updates swing again without rebuild errors
    await page.evaluate(() => {
      const { bridge } = window.verbRoom;
      bridge.parameters.begin('s1path'); bridge.parameters.change('s1path', 0.92); bridge.parameters.end('s1path');
    });
    await lfoBox.locator('.path-lfo-warn').waitFor({ state: 'visible' });

    await page.evaluate(() => window.verbRoom.store.setView({ timelineOpen: true }, false));
    await page.locator('.tl-lfo-band').waitFor();

    await page.evaluate(() => window.verbRoom.store.setView({ timelineOpen: false, page: 'edit', inspectorTab: 'selected', selection: [{ kind: 'source', index: 0 }] }, false));
    await page.locator('.edit-inspector .path-lfo .path-lfo-swing').waitFor();
  });
});

test('G7 Small windows 1100x560 and 900x560 docked + detached usability (8x3 routes, min font >= 11px)', { timeout: 60000 }, async () => {
  const setupRoutes = async (page, openFloating) => {
    await page.evaluate(async open => {
      const { store, bridge } = window.verbRoom;
      bridge.parameters.begin('mode'); bridge.parameters.change('mode', 1); bridge.parameters.end('mode');
      for (let i = 1; i < 8; i++) await store.command({ op: 'source.add' });
      const routes = [];
      for (let s = 0; s < 8; s++) {
        routes.push(
          { enabled: true, object: s, driver: 'distanceToListener', curve: { inputRange: [1, 15], outputRange: [20000, 1200], invert: false, clamp: true, nodes: [{ x: 0, y: 0, smooth: true }, { x: 1, y: 1, smooth: false }] }, targetObject: -1, target: 'sourceLowPass', attackMs: 15, releaseMs: 80 },
          { enabled: true, object: s, driver: 'azimuthToListener', curve: { inputRange: [-180, 180], outputRange: [0, -9], invert: false, clamp: true, nodes: [{ x: 0, y: 1, smooth: true }, { x: 0.5, y: 0, smooth: true }, { x: 1, y: 1, smooth: false }] }, targetObject: -1, target: 'sourceGain', attackMs: 20, releaseMs: 100 },
          { enabled: true, object: s, driver: 'speed', curve: { inputRange: [0, 8], outputRange: [0, 4], invert: false, clamp: true, nodes: [{ x: 0, y: 0, smooth: false }, { x: 1, y: 1, smooth: false }] }, targetObject: -1, target: 'sourceHighShelf', attackMs: 25, releaseMs: 120 }
        );
      }
      await store.command({ op: 'modulation.setRoutes', routes });
      await store.setView({ tab: 'modulation', modFilter: 'all', modRoute: 0, modEditorOpen: open, selection: [{ kind: 'source', index: 0 }] }, false);
    }, openFloating);
  };

  const runAudit = async (page, expectDetached, shotName) => {
    await page.locator('.bottom-modulation .mod-route-item').nth(23).waitFor();
    if (expectDetached) await page.locator('.mod-editor-panel .mod-curve-svg').waitFor();
    else await page.locator('.bottom-modulation .mod-dock-editor-col .mod-curve-svg').waitFor();
    await page.waitForTimeout(150);

    const audit = await page.evaluate(detached => {
      const checkRoot = root => {
        let minFont = 999;
        for (const el of root.querySelectorAll('*')) {
          if (!el.getClientRects().length) continue;
          const fs = parseFloat(getComputedStyle(el).fontSize);
          if (Number.isFinite(fs) && fs > 0 && fs < minFont) minFont = fs;
        }
        const style = e => getComputedStyle(e);
        const scrollable = v => v === 'auto' || v === 'scroll';
        const clipBox = p => {
          const r = p.getBoundingClientRect(), x = r.left + p.clientLeft, y = r.top + p.clientTop;
          return { left: x, top: y, right: x + p.clientWidth, bottom: y + p.clientHeight };
        };
        const clipped = [];
        for (const c of [...root.querySelectorAll('button,input,select,[role=slider]')].filter(el => el.getClientRects().length)) {
          const ancestors = [];
          for (let a = c.parentElement; a && a !== root.parentElement; a = a.parentElement) ancestors.push(a);
          const saved = [];
          for (const a of ancestors) {
            const s = style(a), b = clipBox(a), r = c.getBoundingClientRect();
            const dx = r.right > b.right ? r.right - b.right : Math.min(0, r.left - b.left);
            const dy = r.bottom > b.bottom ? r.bottom - b.bottom : Math.min(0, r.top - b.top);
            if (scrollable(s.overflowX) || scrollable(s.overflowY)) {
              saved.push([a, a.scrollLeft, a.scrollTop]);
              if (scrollable(s.overflowX)) a.scrollLeft += dx;
              if (scrollable(s.overflowY)) a.scrollTop += dy;
            }
          }
          const r = c.getBoundingClientRect();
          const badAnc = ancestors.find(a => {
            const s = style(a), b = clipBox(a);
            return (s.overflowX !== 'visible' && (r.left < b.left - 2 || r.right > b.right + 2))
              || (s.overflowY !== 'visible' && (r.top < b.top - 2 || r.bottom > b.bottom + 2));
          });
          if (badAnc || r.width <= 0 || r.height <= 0 || r.right > window.innerWidth + 2 || r.left < -2 || r.bottom > window.innerHeight + 2 || r.top < -2) {
            clipped.push(c.getAttribute('aria-label') || c.textContent?.trim() || c.className);
          }
          for (const [a, x, y] of saved) { a.scrollLeft = x; a.scrollTop = y; }
        }
        return { minFont, clipped };
      };

      const tabEl = document.querySelector('.bottom-modulation .mod-scroll-sa');
      const winEl = document.querySelector('.mod-editor-panel');
      const vp = document.querySelector('.viewport').getBoundingClientRect();
      const wr = winEl?.getBoundingClientRect();
      return {
        tabRoutes: tabEl.querySelectorAll('.mod-route-item').length,
        winRoutes: winEl ? winEl.querySelectorAll('.mod-route-item').length : 0,
        budgetText: tabEl.querySelector('.mod-budget')?.textContent,
        winInsideViewport: !detached || (!!wr && wr.left >= vp.left - 1 && wr.right <= vp.right + 1 && wr.top >= vp.top - 1 && wr.bottom <= vp.bottom + 1),
        dockedCurveCount: tabEl.querySelectorAll('.mod-dock-editor-col .mod-curve-svg').length,
        tabAudit: checkRoot(tabEl),
        winAudit: winEl ? checkRoot(winEl) : { minFont: 999, clipped: [] }
      };
    }, expectDetached);

    assert.equal(audit.tabRoutes, 24);
    assert.equal(audit.budgetText, '24 / 64 routes');
    assert.ok(audit.tabAudit.minFont >= 11, `Tab min font >= 11px, got ${audit.tabAudit.minFont}`);
    assert.deepEqual(audit.tabAudit.clipped, [], `Tab clipped controls: ${audit.tabAudit.clipped}`);
    if (expectDetached) {
      assert.equal(audit.winRoutes, 24);
      assert.equal(audit.winInsideViewport, true);
      assert.ok(audit.winAudit.minFont >= 11, `Win min font >= 11px, got ${audit.winAudit.minFont}`);
      assert.deepEqual(audit.winAudit.clipped, [], `Win clipped controls: ${audit.winAudit.clipped}`);
    } else {
      assert.equal(audit.dockedCurveCount, 1);
    }
    await page.screenshot({ path: resolve(shotsDir, shotName) });
  };

  await withPage(async page => {
    await setupRoutes(page, false);
    await runAudit(page, false, '04_small_1100x560_docked.png');
    await page.evaluate(() => window.verbRoom.store.setView({ modEditorOpen: true }, false));
    await runAudit(page, true, '05_small_1100x560_detached.png');
  }, { width: 1100, height: 560 });

  await withPage(async page => {
    await setupRoutes(page, false);
    await runAudit(page, false, '06_small_900x560_docked.png');
    await page.evaluate(() => window.verbRoom.store.setView({ modEditorOpen: true }, false));
    await runAudit(page, true, '07_small_900x560_detached.png');
  }, { width: 900, height: 560 });
});
