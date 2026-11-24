import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { serve } from '../scripts/serve.mjs';

const require = createRequire(import.meta.url);
const candidates = [
  process.env.PLAYWRIGHT_MODULE,
  'playwright',
  'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'
].filter(Boolean);
let playwright;
for (const c of candidates) { try { playwright = require(c); break; } catch {} }
assert.ok(playwright, 'Set PLAYWRIGHT_MODULE to a Playwright installation');

const executablePath = process.env.CHROME_PATH || [
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
].find(existsSync);

const SHOTS_DIR = fileURLToPath(new URL('../../out/G9/shots', import.meta.url));

async function withPage(fn) {
  const { server, url } = await serve({ port: 0 });
  const browser = await playwright.chromium.launch({
    headless: true,
    ...(executablePath ? { executablePath } : {})
  });
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 }, reducedMotion: 'reduce' });
    page.setDefaultTimeout(5000);
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
    await page.goto(url);
    await page.waitForFunction(() => window.verbRoom?.store?.ready && document.querySelector('.bottom .tabs'));
    await fn(page);
    assert.deepEqual(errors, []);
  } finally {
    await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
}

test('G9 bottom tabs stretch 100% evenly across 900/1100/1280/1920 without altering Edit-room tabs', { timeout: 60000 }, async () => {
  await fs.mkdir(SHOTS_DIR, { recursive: true });
  await withPage(async page => {
    for (const [width, height] of [[900, 600], [1100, 650], [1280, 800], [1920, 1080]]) {
      await page.setViewportSize({ width, height });
      await page.waitForTimeout(80);
      const metrics = await page.evaluate(() => {
        const bar = document.querySelector('.bottom .tabs');
        const buttons = [...bar.querySelectorAll('button')];
        const barRect = bar.getBoundingClientRect();
        const items = buttons.map(b => {
          const r = b.getBoundingClientRect();
          const clipped = b.scrollWidth > b.clientWidth + 2;
          return { text: b.textContent.trim(), left: r.left, width: r.width, right: r.right, clipped };
        });
        return { barWidth: barRect.width, barRight: barRect.right, items };
      });
      assert.equal(metrics.items.length, 5, `expected 5 tabs at ${width}`);
      const totalWidth = metrics.items.reduce((s, x) => s + x.width, 0);
      assert.ok(Math.abs(totalWidth - metrics.barWidth) <= 3, `tabs width sum ${totalWidth} != bar ${metrics.barWidth} at ${width}`);
      const expectedEach = metrics.barWidth / 5;
      for (const item of metrics.items) {
        assert.ok(Math.abs(item.width - expectedEach) <= 2.5, `tab "${item.text}" width ${item.width} != ${expectedEach} at ${width}`);
        assert.equal(item.clipped, false, `tab "${item.text}" clipped at ${width}`);
      }
      assert.ok(Math.abs(metrics.items[4].right - metrics.barRight) <= 3, `right edge flush at ${width}`);
      if (width === 1280) {
        await page.screenshot({ path: `${SHOTS_DIR}/g9-tabs-1280x800.png` });
      }
    }

    // Verify Edit-room tabs keep their 96px styling at desktop widths.
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.evaluate(() => window.verbRoom.store.setView({ page: 'edit', selection: [{ kind: 'source', index: 0 }] }));
    await page.locator('.edit-inspector .tabs button').first().waitFor();
    const editTabWidths = await page.locator('.edit-inspector .tabs button').evaluateAll(es => es.map(e => e.getBoundingClientRect().width));
    assert.ok(editTabWidths.length >= 2);
    for (const w of editTabWidths) {
      assert.ok(Math.abs(w - 96) <= 1.5, `edit tab width ${w} expected 96`);
    }
  });
});

test('G9 main inspector collapsible sections: defaults, keyboard, persistence, scrolling, no jump, >=11px font', { timeout: 60000 }, async () => {
  await fs.mkdir(SHOTS_DIR, { recursive: true });
  await withPage(async page => {
    await page.evaluate(async () => {
      const { store, bridge } = window.verbRoom;
      bridge.parameters.begin('mode'); bridge.parameters.change('mode', 1); bridge.parameters.end('mode');
      if (store.state.document.sources.length < 2) await store.command({ op: 'source.add' });
      await store.command({ op: 'source.setConfig', index: 0, format: 7, channels: 2, sum: false, freePoints: true });
      await store.command({ op: 'path.set', object: 0, points: [[1, 1, 1.5], [4, 3, 1.5], [7, 1, 1.5]], enabled: true, closed: false });
      await store.command({ op: 'path.set', object: 1, points: [[2, 2, 1.5], [5, 2, 1.5]], enabled: true, closed: false });
      await store.command({ op: 'path.set', object: 32, points: [[0, 0, 1.5], [1, 1, 1.5]], enabled: false, closed: false });
      await store.setView({ selection: [{ kind: 'source', index: 0 }], accordion: {} });
    });

    const head = id => page.locator(`.selected-card [data-acc-id="${id}"]`);
    const body = id => page.locator(`.selected-card #acc-${id.replace(/\./g, '\\.')}`);

    // Default open/collapsed states: Path LFO collapsed when LFO is off; others open.
    for (const id of ['main.pose', 'main.orientation', 'main.path', 'main.path.coupling', 'main.path.automation', 'main.path.bake', 'main.source.routing', 'main.source.directivity', 'main.source.level', 'main.source.points']) {
      assert.equal(await head(id).getAttribute('aria-expanded'), 'true', `${id} default open`);
    }
    assert.equal(await head('main.path.lfo').getAttribute('aria-expanded'), 'false', 'Path LFO collapsed by default when off');

    // Expand Path LFO -> nested body appears, then enable LFO and reset accordion to check auto-open when LFO is enabled.
    await head('main.path.lfo').click();
    assert.equal(await head('main.path.lfo').getAttribute('aria-expanded'), 'true');
    await body('main.path.lfo').locator('.path-lfo').waitFor();

    // Capture expanded screenshot on 1280x800 (scroll Path into view for visibility).
    await head('main.path').scrollIntoViewIfNeeded();
    await page.screenshot({ path: `${SHOTS_DIR}/g9-inspector-path-expanded.png` });

    // Collapse Path by mouse click: flags, Coupling, Start/End, Redraw, Edit, LFO, Host automation, Bake all collapse with it.
    await head('main.path').click();
    assert.equal(await head('main.path').getAttribute('aria-expanded'), 'false');
    assert.equal(await page.locator('.selected-card .path-actions').count(), 0);
    assert.equal(await page.locator('.selected-card .path-window').count(), 0);
    assert.equal(await head('main.path.coupling').count(), 0);
    assert.equal(await head('main.path.lfo').count(), 0);
    assert.equal(await head('main.path.automation').count(), 0);
    assert.equal(await head('main.path.bake').count(), 0);
    await page.screenshot({ path: `${SHOTS_DIR}/g9-inspector-path-collapsed.png` });

    // Keyboard Enter & Space toggle + focus restore.
    await head('main.path').focus();
    await page.keyboard.press('Enter');
    assert.equal(await head('main.path').getAttribute('aria-expanded'), 'true');
    assert.equal(await page.evaluate(() => document.activeElement?.getAttribute('data-acc-id')), 'main.path');
    await page.keyboard.press(' ');
    assert.equal(await head('main.path').getAttribute('aria-expanded'), 'false');
    assert.equal(await page.evaluate(() => document.activeElement?.getAttribute('data-acc-id')), 'main.path');

    // Also collapse Orientation and Directivity and verify persistence across selection & sessionViewState & Edit round-trip.
    await head('main.orientation').click();
    await head('main.source.directivity').click();
    const geomSource1 = await page.evaluate(() => {
      const card = document.querySelector('.selected-card');
      const pose = card.querySelector('.pose-column').getBoundingClientRect();
      const sett = card.querySelector('.settings-column').getBoundingClientRect();
      return { poseX: pose.x, poseW: pose.width, settX: sett.x, settW: sett.width };
    });

    // Switch to Source 2: same section IDs remain collapsed without column geometry jump.
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'source', index: 1 }] }));
    assert.equal(await head('main.path').getAttribute('aria-expanded'), 'false');
    assert.equal(await head('main.orientation').getAttribute('aria-expanded'), 'false');
    assert.equal(await head('main.source.directivity').getAttribute('aria-expanded'), 'false');
    const geomSource2 = await page.evaluate(() => {
      const card = document.querySelector('.selected-card');
      const pose = card.querySelector('.pose-column').getBoundingClientRect();
      const sett = card.querySelector('.settings-column').getBoundingClientRect();
      return { poseX: pose.x, poseW: pose.width, settX: sett.x, settW: sett.width };
    });
    assert.ok(Math.abs(geomSource2.poseX - geomSource1.poseX) <= 1);
    assert.ok(Math.abs(geomSource2.poseW - geomSource1.poseW) <= 1);
    assert.ok(Math.abs(geomSource2.settW - geomSource1.settW) <= 1);

    // Switch to Listener: shared Pose / Orientation / Path keep state; Listener Output / microphones is collapsible.
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'listener', index: 0 }] }));
    assert.equal(await head('main.path').getAttribute('aria-expanded'), 'false');
    assert.equal(await head('main.orientation').getAttribute('aria-expanded'), 'false');
    assert.equal(await head('main.listener.output').getAttribute('aria-expanded'), 'true');
    await head('main.listener.output').click();
    assert.equal(await head('main.listener.output').getAttribute('aria-expanded'), 'false');

    // Edit room round-trip preserves store.view.accordion & sessionViewState.accordion.
    await page.evaluate(() => window.verbRoom.store.setView({ page: 'edit' }));
    await page.evaluate(() => window.verbRoom.store.setView({ page: 'main', selection: [{ kind: 'source', index: 0 }] }));
    assert.equal(await head('main.path').getAttribute('aria-expanded'), 'false');
    assert.equal(await head('main.orientation').getAttribute('aria-expanded'), 'false');
    const persisted = await page.evaluate(() => window.verbRoom.store.state.preferences.sessionViewState.accordion);
    assert.equal(persisted['main.path'], false);
    assert.equal(persisted['main.orientation'], false);
    assert.equal(persisted['main.listener.output'], false);

    // Clear accordion overrides, enable LFO on Source 0, verify main.path.lfo defaults open when enabled.
    await page.evaluate(async () => {
      const { store } = window.verbRoom;
      await store.command({ op: 'path.setLfo', object: 0, lfo: { enabled: true, shape: 'sine', rateHz: 1, depth: 0.25, phaseDeg: 0, seed: 1 } });
      await store.setView({ accordion: {} });
    });
    assert.equal(await head('main.path.lfo').getAttribute('aria-expanded'), 'true');

    // Verify 900x560 and 1100x560: column scrollArea scrolls, zero horizontal overflow, min font >= 11px in inspector.
    for (const [width, height] of [[900, 560], [1100, 560]]) {
      await page.setViewportSize({ width, height });
      await page.waitForTimeout(80);
      const layout = await page.evaluate(() => {
        const poseScroll = document.querySelector('.selected-card .pose-scroll');
        const settScroll = document.querySelector('.selected-card .settings-scroll');
        const canScrollPose = poseScroll.scrollHeight > poseScroll.clientHeight;
        poseScroll.scrollTop = poseScroll.scrollHeight;
        const scrolledPoseTop = poseScroll.scrollTop;
        poseScroll.scrollTop = 0;

        const minFonts = [];
        for (const el of document.querySelectorAll('.selected-card *')) {
          if (!el.getClientRects().length) continue;
          const hasDirectText = [...el.childNodes].some(n => n.nodeType === 3 && n.textContent.trim().length > 0);
          if (!hasDirectText) continue;
          const fs = parseFloat(getComputedStyle(el).fontSize);
          if (Number.isFinite(fs) && fs < 10.99) minFonts.push({ cls: el.className, text: el.textContent.trim().slice(0, 30), fs });
        }
        const findOverflows = container => {
          const cr = container.getBoundingClientRect();
          const out = [];
          for (const el of container.querySelectorAll('*')) {
            if (!el.getClientRects().length) continue;
            const r = el.getBoundingClientRect();
            if (r.right > cr.right + 1 || r.left < cr.left - 1) {
              out.push({ tag: el.tagName, cls: el.className, w: Math.round(r.width), over: Math.round(r.right - cr.right), text: el.textContent.trim().slice(0, 25) });
            }
          }
          return out.slice(0, 8);
        };
        const horizOverflow = poseScroll.scrollWidth > poseScroll.clientWidth + 2 || settScroll.scrollWidth > settScroll.clientWidth + 2;
        return {
          canScrollPose,
          scrolledPoseTop,
          minFonts,
          horizOverflow,
          poseScroll: [poseScroll.scrollWidth, poseScroll.clientWidth],
          settScroll: [settScroll.scrollWidth, settScroll.clientWidth],
          poseOver: findOverflows(poseScroll),
          settOver: findOverflows(settScroll)
        };
      });
      assert.equal(layout.canScrollPose, true, `pose column should scroll at ${width}x${height}`);
      assert.ok(layout.scrolledPoseTop > 10, `pose column did not scroll at ${width}x${height}`);
      assert.equal(layout.horizOverflow, false, `horizontal overflow inside inspector at ${width}x${height}: ${JSON.stringify(layout)}`);
      assert.deepEqual(layout.minFonts, [], `found <11px font in inspector at ${width}x${height}: ${JSON.stringify(layout.minFonts)}`);
    }
  });
});
