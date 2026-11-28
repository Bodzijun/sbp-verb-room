// V04e: route MIDI out (model, mock, editor UI + live indicator) and the viewport Zones visibility toggle.
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import test from 'node:test';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { serve } from '../scripts/serve.mjs';
import { validator, readFixture } from './helpers.mjs';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';
import { normalizeRoute, normalizeMidiOut, midiNoteName, describeMidiMessage, simulateMidiOut, MIDI_MODES } from '../src/components/modulation-model.js';

const require = createRequire(import.meta.url);
let playwright;
for (const name of [process.env.PLAYWRIGHT_MODULE, 'playwright', 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'].filter(Boolean)) {
  try { playwright = require(name); break; } catch {}
}
const CHROME = process.env.CHROME_PATH || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const __dirname = path.dirname(fileURLToPath(import.meta.url));
// Screenshots never go to web-ui/shots: default out/V04e/shots, override with VERB_V04E_SHOTS_DIR.
const SHOTS_DIR = process.env.VERB_V04E_SHOTS_DIR || path.resolve(__dirname, '../../out/V04e/shots');

test('V04e MIDI model: normalization, note names, message text, emulated generator rules', () => {
  assert.deepEqual(MIDI_MODES.map(m => m.id), ['off', 'gate', 'trigger', 'cc', 'index']);
  assert.equal(normalizeMidiOut(undefined), undefined);
  assert.equal(normalizeMidiOut({ mode: 'off', channel: 3 }), undefined);
  assert.deepEqual(normalizeMidiOut({ mode: 'gate', channel: 22, number: 200, velocity: 0, threshold: 2 }),
    { mode: 'gate', channel: 16, number: 127, velocity: 1, velocityFromValue: false, threshold: 1, hysteresis: 0.05, lengthMs: 100, ccLo: 0, ccHi: 127, program: false });
  assert.equal(midiNoteName(60), 'C3');
  assert.equal(midiNoteName(36), 'C1');
  assert.equal(midiNoteName(61), 'C#3');
  assert.equal(describeMidiMessage([0x90, 60, 100]), 'Note on C3 (60) vel 100 · ch 1');
  assert.equal(describeMidiMessage([0xb2, 11, 64]), 'CC 11 = 64 · ch 3');
  // route model keeps the block; Off drops it (old documents unchanged)
  const base = { object: 0, driver: 'inZone', zoneId: 1, target: 'none', hostOut: 1 };
  assert.equal('midi' in normalizeRoute(base, 1), false);
  assert.equal('midi' in normalizeRoute({ ...base, midi: { mode: 'off' } }, 1), false);
  assert.equal(normalizeRoute({ ...base, midi: { mode: 'cc', number: 7 } }, 1).midi.number, 7);
  // emulated generator: gate with hysteresis, keyswitch with release-first
  const mem = [];
  const gate = [{ midi: { mode: 'gate', channel: 1, number: 60 } }];
  const seq = [0, 0.6, 0.48, 0.52, 0.3, 0.7].map(norm => simulateMidiOut(gate, [{ active: true, norm }], mem)[0]);
  assert.deepEqual(seq.map(s => s?.count ?? 0), [0, 1, 1, 1, 2, 3]);
  assert.deepEqual(seq[4].last, [0x80, 60, 0]);
  const mem2 = [];
  const ks = [{ midi: { mode: 'index', channel: 2, number: 36, program: true } }];
  const k1 = simulateMidiOut(ks, [{ active: true, raw: 2 }], mem2)[0];
  assert.deepEqual(k1, { count: 2, last: [0xc1, 2, 0] });
  const k2 = simulateMidiOut(ks, [{ active: true, raw: 1 }], mem2)[0];
  assert.equal(k2.count, 5);  // off 37, on 36, program 1
});

test('V04e mock: MIDI route commands, schema validation, one undo step, fixture, indicator telemetry', async () => {
  const bridge = await createMockBridge({ read: readFixture, validator, fixture: 'zones-midi-creative', delayMs: 0 });
  const store = createStore(bridge);
  try {
    await store.start();
    const routes = () => store.state.document.modulation.routes;
    assert.equal(routes()[0].midi.mode, 'gate');
    assert.equal(routes()[1].midi.mode, 'index');
    const depth = () => store.telemetry['history.changed']?.undoDepth ?? 0;
    const d0 = depth();
    const r0 = routes()[0];
    const ack = await store.command({ op: 'modulation.setRoute', index: 0, route: { ...r0, midi: { mode: 'cc', channel: 3, number: 11, ccLo: 10, ccHi: 90 } } });
    assert.equal(ack.kind, 'ack');
    assert.equal(routes()[0].midi.mode, 'cc');
    assert.equal(routes()[0].midi.ccHi, 90);
    assert.equal(depth(), d0 + 1, 'one undo step');
    await assert.rejects(store.command({ op: 'modulation.setRoute', index: 0, route: { ...r0, midi: { mode: 'gate', channel: 17 } } }), /Contract validation|channel/, 'channel 17 rejected by the command schema');
    assert.equal(routes()[0].midi.mode, 'cc');
    await store.command({ op: 'modulation.setRoute', index: 0, route: { ...r0, midi: { mode: 'off' } } });
    assert.equal('midi' in routes()[0], false, 'mode off removes the block');
    await store.command({ op: 'history.undo' });
    assert.equal(routes()[0].midi.mode, 'cc');
  } finally { store.dispose(); bridge.dispose(); }
});

async function withBrowser(t, fn) {
  if (!playwright) { t.skip('Playwright not available'); return; }
  await fs.mkdir(SHOTS_DIR, { recursive: true });
  const { server, url } = await serve();
  let browser;
  try {
    browser = await playwright.chromium.launch({ headless: true, executablePath: CHROME });
    await fn(browser, url);
  } finally {
    await browser?.close();
    server.close?.();
  }
}
const openPage = async (browser, url, fixture, viewport) => {
  const page = await browser.newPage({ viewport, reducedMotion: 'reduce' });
  page.setDefaultTimeout(5000);
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.goto(`${url}/?fixture=${fixture}&dev=0`);
  await page.waitForFunction(() => window.verbRoom?.store?.ready);
  return { page, errors };
};

test('V04e Browser: Zones toggle hides zones + labels in every view, keeps the edited zone, persists as a preference', { timeout: 90000 }, async t => {
  await withBrowser(t, async (browser, url) => {
    for (const viewport of [{ width: 1280, height: 720 }, { width: 1100, height: 560 }]) {
      const tag = `${viewport.width}x${viewport.height}`;
      const { page, errors } = await openPage(browser, url, 'zones-creative', viewport);
      const vp = 'section.viewport';
      const drawn = () => page.evaluate(sel => document.querySelector(sel)?.dataset.zonesDrawn, vp);
      const toggle = page.locator('[data-zones-toggle]');
      await page.evaluate(() => window.verbRoom.store.setView({ projection: 'plan' }));
      await page.waitForFunction(sel => document.querySelector(sel)?.dataset.zonesDrawn === '2', vp);
      assert.equal(await toggle.getAttribute('data-zones-toggle'), 'on');
      assert.equal(await toggle.getAttribute('aria-pressed'), 'true');
      const box = await toggle.boundingBox();
      assert.ok(box && box.x + box.width <= viewport.width && box.y + box.height <= viewport.height, 'toggle visible in the compact toolbar');
      await page.screenshot({ path: path.join(SHOTS_DIR, `zones-toggle-on-plan-${tag}.png`) });
      await toggle.click();
      await page.waitForFunction(sel => document.querySelector(sel)?.dataset.zonesDrawn === '0', vp);
      assert.equal(await page.evaluate(() => window.verbRoom.store.state.preferences.showZones), false);
      assert.equal(await page.evaluate(() => 'showZones' in window.verbRoom.store.state.document), false, 'not part of the scene document');
      await page.screenshot({ path: path.join(SHOTS_DIR, `zones-toggle-off-plan-${tag}.png`) });
      for (const projection of ['front', 'side', '3d']) {
        await page.evaluate(p => window.verbRoom.store.setView({ projection: p }), projection);
        await page.waitForFunction(p => document.querySelector('.viewport')?.liveCamera?.view === p || p === '3d', projection);
        await page.waitForTimeout(60);
        assert.equal(await drawn(), '0', `hidden in ${projection}`);
      }
      if (viewport.width === 1280) {
        // Edit page: the zone selected in the Zones list stays visible (and editable); a zone tool shows all zones.
        await page.evaluate(() => window.verbRoom.store.setView({ page: 'edit', projection: 'plan', inspectorTab: 'scene', selection: [], zoneEdit: 1 }));
        await page.waitForFunction(sel => document.querySelector(sel)?.dataset.zonesDrawn === '1', vp);
        await page.waitForFunction(sel => document.querySelector(sel).zoneHandleScreens().length === 8, vp);
        await page.screenshot({ path: path.join(SHOTS_DIR, `zones-toggle-off-edited-zone-${tag}.png`) });
        await page.evaluate(() => window.verbRoom.store.setView({ zoneEdit: null, tool: 'zoneBox' }));
        await page.waitForFunction(sel => document.querySelector(sel)?.dataset.zonesDrawn === '2', vp);
        await page.evaluate(() => window.verbRoom.store.setView({ tool: 'move' }));
        await page.waitForFunction(sel => document.querySelector(sel)?.dataset.zonesDrawn === '0', vp);
        // Hidden zones are not hit-tested: a click on the Stage zone body does not select it.
        await page.evaluate(() => window.verbRoom.store.setView({ zoneEdit: null }));
        const hit = await page.evaluate(sel => document.querySelector(sel).zoneHandleScreens?.().length ?? 0, vp);
        assert.equal(hit, 0);
      }
      await page.evaluate(() => window.verbRoom.store.setView({ page: 'main', projection: 'plan' }));
      await page.locator('[data-zones-toggle]').click();
      await page.waitForFunction(sel => document.querySelector(sel)?.dataset.zonesDrawn === '2', vp);
      assert.equal(await page.evaluate(() => window.verbRoom.store.state.preferences.showZones), true);
      assert.deepEqual(errors, []);
      await page.close();
    }
  });
});

test('V04e Browser: route editor MIDI out section, live indicator, mode switch = 1 undo step, compact viewports', { timeout: 120000 }, async t => {
  await withBrowser(t, async (browser, url) => {
    for (const viewport of [{ width: 1280, height: 720 }, { width: 1100, height: 560 }]) {
      const tag = `${viewport.width}x${viewport.height}`;
      const { page, errors } = await openPage(browser, url, 'zones-midi-creative', viewport);
      await page.evaluate(() => window.verbRoom.store.setView({ page: 'main', tab: 'modulation', modRoute: 0, modEditorOpen: true, projection: 'plan' }));
      const win = page.locator('.mod-editor-panel');
      await win.waitFor();
      const row = win.locator('.mod-midi-row[data-midi-mode="gate"]');
      await row.waitFor();
      // Source 1 sits inside Stage: the gate fires, the indicator shows the note-on and flashes.
      await page.waitForFunction(() => /Note on C3 \(60\) vel 100 · ch 1/.test(document.querySelector('.mod-editor-panel .mod-midi-live')?.textContent || ''));
      assert.equal(await row.locator('.mod-midi-note').first().textContent(), 'C3');
      const rb = await row.boundingBox(), wb = await win.boundingBox();
      assert.ok(rb.x >= wb.x - 1 && rb.x + rb.width <= wb.x + wb.width + 1, 'MIDI row fits the floating editor width');
      await page.screenshot({ path: path.join(SHOTS_DIR, `midi-route-gate-${tag}.png`) });

      // Mode switch (custom dropdown) = one command = one undo step.
      const depth = () => page.evaluate(() => window.verbRoom.store.telemetry['history.changed']?.undoDepth ?? 0);
      const d0 = await depth();
      await win.getByRole('combobox', { name: 'MIDI out mode', exact: true }).click();
      await page.locator('.dd-popup [role=option]', { hasText: 'CC' }).first().click();
      await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].midi?.mode === 'cc');
      assert.equal(await depth(), d0 + 1);
      assert.equal(await page.evaluate(() => window.verbRoom.store.state.document.modulation.routes[0].midi.number), 1);
      await win.locator('.mod-midi-row[data-midi-mode="cc"]').waitFor();
      // CC number typed = one undo step.
      const cc = win.getByRole('textbox', { name: 'MIDI CC number', exact: true });
      await cc.dblclick(); await cc.fill('74'); await cc.press('Enter');
      await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[0].midi?.number === 74);
      assert.equal(await depth(), d0 + 2);
      await page.waitForFunction(() => /CC 74 = \d+ · ch 1/.test(document.querySelector('.mod-editor-panel .mod-midi-live')?.textContent || ''));
      await page.screenshot({ path: path.join(SHOTS_DIR, `midi-route-cc-${tag}.png`) });
      // Keyswitch route (Zone index on the listener).
      await page.evaluate(() => window.verbRoom.store.setView({ modRoute: 1 }));
      await win.locator('.mod-midi-row[data-midi-mode="index"]').waitFor();
      assert.equal(await win.getByRole('switch', { name: 'Also send program change' }).getAttribute('aria-checked'), 'true');
      await page.screenshot({ path: path.join(SHOTS_DIR, `midi-route-keyswitch-${tag}.png`) });
      // Off removes the block; undo restores it.
      await win.getByRole('combobox', { name: 'MIDI out mode', exact: true }).click();
      await page.locator('.dd-popup [role=option]', { hasText: 'Off' }).first().click();
      await page.waitForFunction(() => !('midi' in window.verbRoom.store.state.document.modulation.routes[1]));
      await page.evaluate(() => window.verbRoom.store.command({ op: 'history.undo' }));
      await page.waitForFunction(() => window.verbRoom.store.state.document.modulation.routes[1].midi?.mode === 'index');
      // Physical: the indicator says so.
      await page.evaluate(() => { const { bridge } = window.verbRoom; bridge.parameters.begin('mode'); bridge.parameters.change('mode', 0); bridge.parameters.end('mode'); });
      await page.waitForFunction(() => /Physical: no MIDI/.test(document.querySelector('.mod-editor-panel .mod-midi-live')?.textContent || ''));
      // Docked editor (bottom tab): a route without MIDI shows only the compact MIDI button.
      await page.evaluate(async () => {
        const { store, bridge } = window.verbRoom; bridge.parameters.begin('mode'); bridge.parameters.change('mode', 1); bridge.parameters.end('mode');
        const r = store.state.document.modulation.routes[1]; const { midi, ...rest } = r;
        await store.command({ op: 'modulation.setRoute', index: 1, route: rest });
        await store.setView({ modEditorOpen: false, modRoute: 1, tab: 'modulation' });
      });
      const dockSel = page.locator('.bottom-modulation .mod-field-midi').getByRole('combobox', { name: 'MIDI out mode', exact: true });
      await dockSel.waitFor();
      await page.screenshot({ path: path.join(SHOTS_DIR, `midi-dock-button-${tag}.png`) });
      // Picking a mode in the dock must NOT detach the editor: the MIDI row appears in the dock.
      await dockSel.click();
      await page.locator('.dd-popup [role=option]', { hasText: 'Note gate' }).first().click();
      await page.locator('.bottom-modulation .mod-midi-row.on').waitFor();
      assert.equal(await page.evaluate(() => !!window.verbRoom.store.view.modEditorOpen), false, 'MIDI in the dock must not open the floating editor');
      assert.equal(await page.locator('.mod-editor-panel').count(), 0, 'floating editor must stay closed');
      await page.screenshot({ path: path.join(SHOTS_DIR, `midi-dock-row-${tag}.png`) });
      assert.deepEqual(errors, []);
      await page.close();
    }
  });
});
