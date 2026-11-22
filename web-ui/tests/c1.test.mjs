import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { existsSync } from 'node:fs';
import { validator, readFixture } from './helpers.mjs';
import { createMockBridge } from '../src/bridge/mock.js';
import { createStore } from '../src/state/store.js';
import { serve } from '../scripts/serve.mjs';
import {
  SOURCE_COLOUR_PALETTE,
  AUTO_STRIP_COLOURS,
  isValidSourceColorHex,
  normalizeSourceColor,
  contrastTextColour,
  sourceStripColour,
  sourceMarkerColour,
  sourceLabelColour
} from '../src/viewport/colour.js';
import { viewportPalette } from '../src/viewport/palette.js';
import { objectColour, pathRow } from '../src/components/timeline-model.js';

const require = createRequire(import.meta.url);
const executablePath = process.env.CHROME_PATH || [
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
].find(existsSync);

test('C1 colour helpers: palette, normalization, contrast, markers, labels, timeline', () => {
  assert.equal(SOURCE_COLOUR_PALETTE.length, 12);
  assert.equal(new Set(SOURCE_COLOUR_PALETTE.map(c => c.hex)).size, 12);
  for (const item of SOURCE_COLOUR_PALETTE) {
    assert.match(item.hex, /^#[0-9a-f]{6}$/);
  }

  assert.equal(normalizeSourceColor(''), '');
  assert.equal(normalizeSourceColor(null), '');
  assert.equal(normalizeSourceColor(undefined), '');
  assert.equal(normalizeSourceColor('#AABBCC'), '#aabbcc');
  assert.equal(normalizeSourceColor('#12af09'), '#12af09');
  assert.equal(normalizeSourceColor('red'), null);
  assert.equal(normalizeSourceColor('#abc'), null);
  assert.equal(normalizeSourceColor('#12345g'), null);
  assert.equal(normalizeSourceColor('#1234567'), null);
  assert.equal(normalizeSourceColor(123456), null);

  assert.equal(isValidSourceColorHex('#4fb074'), true);
  assert.equal(isValidSourceColorHex('#4FB074'), true);
  assert.equal(isValidSourceColorHex(''), false);

  // Bright yellow/lime -> dark text; deeper hues -> light text
  assert.equal(contrastTextColour('#e0b84c'), '#111518');
  assert.equal(contrastTextColour('#c8cf4a'), '#111518');
  assert.equal(contrastTextColour('#111518'), '#f2f6f8');
  assert.equal(contrastTextColour('#4f92d6'), '#f2f6f8');

  const roles = viewportPalette({ viewportTheme: 'Amber' });
  assert.equal(sourceStripColour({}, 0), `var(--strip-0, ${AUTO_STRIP_COLOURS[0]})`);
  assert.equal(sourceStripColour({ color: '#4fb074' }, 0), '#4fb074');
  assert.equal(sourceMarkerColour({}, roles), roles.source);
  assert.equal(sourceMarkerColour({ color: '#4FB074' }, roles), '#4fb074');
  assert.equal(sourceLabelColour({}, roles), roles.labels);
  assert.equal(sourceLabelColour({ color: '#4fb074' }, roles), '#4fb074');
  // Unreadable dark colour falls back to theme label colour
  assert.equal(sourceLabelColour({ color: '#050505' }, roles), roles.labels);

  assert.equal(objectColour(0), 'var(--strip-0, #d9a05b)');
  const state = {
    document: {
      sources: [{ name: 'Flute', color: '#4fb074' }],
      paths: [{ points: [[0, 0, 1], [2, 0, 1]], enabled: true, closed: false, laps: 1 }]
    },
    parameters: { values: { s1path: 0.25 } }
  };
  assert.equal(pathRow(state, 0).colour, '#4fb074');
});

test('C1 mock bridge & contract: capability, setConfig, omission, validation, Keep I/O, remove, undo', async t => {
  const bridge = await createMockBridge({ read: readFixture, validator, delayMs: 0 });
  const store = createStore(bridge);
  t.after(() => { store.dispose(); bridge.dispose(); });
  await store.start();

  assert.equal(store.state.capabilities.features['colours.source']?.available, true);
  assert.equal('color' in store.state.document.sources[0], false);

  // Set uppercase -> normalized lowercase, passes contract validator
  let reply = await store.command({ op: 'source.setConfig', index: 0, color: '#4FB074' });
  assert.equal(reply.kind, 'ack');
  assert.equal(store.state.document.sources[0].color, '#4fb074');

  // Set second colour -> undo steps back one by one (no coalescing)
  await store.command({ op: 'source.setConfig', index: 0, color: '#D65F5F' });
  assert.equal(store.state.document.sources[0].color, '#d65f5f');
  assert.equal(store.telemetry['history.changed']?.undoLabel, 'Source colour');

  await store.command({ op: 'history.undo' });
  assert.equal(store.state.document.sources[0].color, '#4fb074');

  await store.command({ op: 'history.undo' });
  assert.equal('color' in store.state.document.sources[0], false);

  // Invalid color rejected by contract validator and by mock-model applyMutation
  await assert.rejects(
    () => store.command({ op: 'source.setConfig', index: 0, color: 'not-a-color' }),
    /Contract validation/i
  );
  const { applyMutation } = await import('../src/bridge/mock-model.js');
  assert.throws(
    () => applyMutation(structuredClone(store.state), { op: 'source.setConfig', index: 0, color: '#12345z' }),
    err => err.code === 'invalid'
  );

  // Reset via "" and null both omit the key
  await store.command({ op: 'source.setConfig', index: 0, color: '#e0b84c' });
  assert.equal(store.state.document.sources[0].color, '#e0b84c');
  await store.command({ op: 'source.setConfig', index: 0, color: '' });
  assert.equal('color' in store.state.document.sources[0], false);

  await store.command({ op: 'source.setConfig', index: 0, color: '#4f92d6' });
  assert.equal(store.state.document.sources[0].color, '#4f92d6');
  await store.command({ op: 'source.setConfig', index: 0, color: null });
  assert.equal('color' in store.state.document.sources[0], false);

  // Add second source, colour second, delete first -> colour stays on the surviving source
  await store.command({ op: 'source.add' });
  await store.command({ op: 'source.setConfig', index: 1, color: '#9e75d6' });
  await store.command({ op: 'source.remove', index: 0 });
  assert.equal(store.state.document.sources.length, 1);
  assert.equal(store.state.document.sources[0].color, '#9e75d6');

  // Keep I/O ON preserves colours across preset.load; Keep I/O OFF drops local colour
  await store.flags({ keepRouting: true });
  await store.command({ op: 'preset.load', source: 'builtin', id: '2' });
  assert.equal(store.state.document.sources[0].color, '#9e75d6');

  await store.flags({ keepRouting: false });
  await store.command({ op: 'preset.load', source: 'builtin', id: '0' });
  assert.equal('color' in store.state.document.sources[0], false);

  // All messages emitted validated cleanly
  bridge.audit.filter(a => ['in', 'out'].includes(a.direction)).forEach(a => validator.assert(a.message));
});

test('C1 browser UI: popover pick, Auto, invalid hex, Esc, Inspector dot, and Viewport views', { skip: !executablePath }, async () => {
  const pw = require(process.env.PLAYWRIGHT_MODULE || 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package');
  const { server, url } = await serve({ port: 0 });
  const browser = await pw.chromium.launch({ headless: true, executablePath });
  const errors = [];
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 }, reducedMotion: 'reduce' });
    page.on('pageerror', e => errors.push(e.message));
    page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });

    await page.goto(`${url}/?fixture=default-physical#sources`);
    await page.waitForFunction(() => window.verbRoom?.store?.ready && document.querySelector('canvas'));
    await page.waitForTimeout(200);

    // Selecting source still works via exact button name
    await page.getByRole('button', { name: 'Select source 1', exact: true }).click();
    await page.waitForSelector('.selected-card.has-selection .source-color-dot');

    // Open colour popover from swatch button
    const swatchTrigger = page.getByRole('button', { name: 'Source 1 colour', exact: true });
    await swatchTrigger.click();
    await page.waitForSelector('.source-color-popover');
    assert.equal(await page.locator('.source-color-popover .source-color-swatch').count(), 12);
    const popoverText = await page.locator('.source-color-popover').innerText();
    assert.ok(!popoverText.includes('[object'), `popover text must not contain object stringification: ${popoverText}`);
    assert.equal(await page.locator('.source-color-popover .source-color-auto').innerText(), 'Auto');

    // Invalid hex rejected inline without closing popover
    await page.locator('.source-color-hex').fill('badhex');
    await page.locator('.source-color-apply').click();
    assert.equal(await page.locator('.source-color-error').isVisible(), true);
    assert.equal(await page.locator('.source-color-popover').count(), 1);
    assert.equal(await page.evaluate(() => 'color' in window.verbRoom.store.state.document.sources[0]), false);

    // Escape closes popover and restores focus to trigger
    await page.keyboard.press('Escape');
    assert.equal(await page.locator('.source-color-popover').count(), 0);
    assert.equal(await page.evaluate(() => document.activeElement?.getAttribute('aria-label')), 'Source 1 colour');

    // Pick Woodwinds Green (#4fb074)
    await swatchTrigger.click();
    await page.locator('.source-color-swatch[data-color="#4fb074"]').click();
    await page.waitForFunction(() => window.verbRoom.store.state.document.sources[0].color === '#4fb074');
    assert.equal(await page.locator('.mix-row.has-custom-color').count(), 1);

    const rowBg = await page.locator('.mix-row[role="row"]:not(.mix-head) .strip-swatch').first().evaluate(e => getComputedStyle(e).backgroundColor);
    assert.equal(rowBg, 'rgb(79, 176, 116)');

    const cardDotBg = await page.locator('.selected-card .source-color-dot').evaluate(e => getComputedStyle(e).backgroundColor);
    assert.equal(cardDotBg, 'rgb(79, 176, 116)');

    // Check viewport renders #4fb074 (79, 176, 116) pixels across Plan, Front, Side, 3D
    const countGreenPixels = () => page.evaluate(() => {
      const c = document.querySelector('.viewport canvas');
      const px = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
      let count = 0;
      for (let i = 0; i < px.length; i += 4) {
        if (Math.hypot(px[i] - 79, px[i + 1] - 176, px[i + 2] - 116) < 28) count++;
      }
      return count;
    });
    for (const view of ['Plan', 'Front', 'Side', '3D']) {
      await page.getByRole('button', { name: view, exact: true }).first().click();
      await page.waitForTimeout(180);
      const pxCount = await countGreenPixels();
      assert.ok(pxCount >= 20, `${view} view should draw custom green source (${pxCount} px)`);
    }

    // Check Edit room Inspector header dot as well
    await page.getByRole('button', { name: 'Edit room ↗', exact: true }).click();
    await page.waitForSelector('.edit-inspector .obj-title .source-color-dot');
    const editDotBg = await page.locator('.edit-inspector .obj-title .source-color-dot').evaluate(e => getComputedStyle(e).backgroundColor);
    assert.equal(editDotBg, 'rgb(79, 176, 116)');
    assert.ok(await countGreenPixels() >= 20, 'Edit viewport draws custom green source');

    await page.getByRole('button', { name: '✓ Done', exact: true }).click();
    await page.waitForSelector('.mixer');

    // Reset via Auto button
    await page.getByRole('button', { name: 'Source 1 colour', exact: true }).click();
    await page.waitForSelector('.source-color-popover');
    await page.getByRole('button', { name: 'Auto (index)', exact: true }).click();
    await page.waitForFunction(() => !('color' in window.verbRoom.store.state.document.sources[0]));
    assert.equal(await page.locator('.mix-row.has-custom-color').count(), 0);

    assert.deepEqual(errors, [], 'no page or console errors');
  } finally {
    await browser.close();
    server.close();
  }
});
