import assert from 'node:assert/strict';
import test from 'node:test';
import { createRequire } from 'node:module';
import { mkdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { serve } from '../scripts/serve.mjs';
import { createMockBridge } from '../src/bridge/mock.js';
import { readFixture, validator } from './helpers.mjs';
import { LINKS, PLUGIN_INFO, SPONSORS, SUPPORT, TIPS, WHATS_NEW, WHATS_NEW_VERSION } from '../src/about-data.js';

const require = createRequire(import.meta.url);
const { chromium } = require('C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package');
const CHROME = 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const REPO_ROOT = path.resolve(fileURLToPath(new URL('../..', import.meta.url)));
const SHOTS_DIR = path.join(REPO_ROOT, 'out', 'G10', 'shots');

test('G10 unit: about-data sponsors/links and mock app.openExternal allow-list', { timeout: 15000 }, async () => {
  assert.deepEqual(SPONSORS, ['smandrap', 'Teniente Powell']);
  assert.equal(PLUGIN_INFO.name, 'Verb Room');
  assert.equal(PLUGIN_INFO.tagline, 'Physically based room reverb for moving sources and listener');
  assert.match(PLUGIN_INFO.license, /AGPLv3.*Built with JUCE/);
  assert.equal(WHATS_NEW_VERSION, '0.3.0');
  assert.ok(WHATS_NEW.length >= 6);
  assert.ok(TIPS.length >= 5);
  assert.equal(SUPPORT.kofi.url, 'https://ko-fi.com/s/405ebacfcc');
  assert.equal(SUPPORT.monobank.url, 'https://send.monobank.ua/jar/7uwNLVbptd');
  assert.match(SUPPORT.monobank.note, /Ukrainian page.*UAH.*45 UAH per 1 USD/);
  assert.equal(SUPPORT.paypal.email, 'bodzik@gmail.com');
  assert.equal(SUPPORT.contact.email, 'bodzik@gmail.com');
  assert.deepEqual(LINKS.map(l => l.url), [
    'https://github.com/Bodzijun/sbp-verb-room/blob/main/docs/Manual_en.md',
    'https://bodzijun.github.io/sbp-verb-room/',
    'https://github.com/Bodzijun/sbp-verb-room',
  ]);

  const bridge = await createMockBridge({ read: readFixture, validator, fixture: 'default-physical', delayMs: 0 });
  try {
    const acked = [];
    const waitReply = (pred, trigger) => new Promise(resolve => {
      const off = bridge.subscribe(msg => {
        if (pred(msg)) { off(); resolve(msg); }
      });
      trigger();
    });
    const snap = await waitReply(
      msg => msg.kind === 'snapshot',
      () => bridge.send({ schemaVersion: 1, sessionId: 'unit', kind: 'hello', payload: { supportedVersions: [1], ready: true } }),
    );
    const rev = String(snap.payload?.revision ?? '0');

    const sendCmd = (id, payload) => waitReply(
      msg => msg.requestId === id,
      () => bridge.send({ schemaVersion: 1, sessionId: 'unit', kind: 'command', requestId: id, baseRevision: rev, payload }),
    );

    const valid = [
      SUPPORT.kofi.url,
      SUPPORT.monobank.url,
      ...LINKS.map(l => l.url),
    ];
    for (let i = 0; i < valid.length; ++i) {
      const rep = await sendCmd(`ok-${i}`, { op: 'app.openExternal', url: valid[i] });
      assert.equal(rep.kind, 'ack', `expected ack for ${valid[i]}`);
      acked.push(valid[i]);
    }
    assert.equal(acked.length, 5);

    const bad = [
      'http://ko-fi.com/s/405ebacfcc',
      'javascript:alert(1)',
      'file:///C:/Windows/win.ini',
      'mailto:bodzik@gmail.com',
      'https://evil.com/x',
      'https://notko-fi.com/s/405ebacfcc',
      'https://ko-fi.com.evil.com/x',
      'https://sub.github.com/x',
      'https://ko-fi.com@evil.com/x',
      'https://ko-fi.com:443/x',
      'https://ko-fi.com/x y',
      'https://',
      123,
    ];
    for (let i = 0; i < bad.length; ++i) {
      await assert.rejects(
        () => bridge.send({ schemaVersion: 1, sessionId: 'unit', kind: 'command', requestId: `schema-bad-${i}`, baseRevision: rev, payload: { op: 'app.openExternal', url: bad[i] } }),
        /Contract validation/,
        `schema validator must reject ${String(bad[i])}`,
      );
    }

    const unvalidatedBridge = await createMockBridge({
      read: readFixture,
      validator: { assert: () => {} },
      fixture: 'default-physical',
      delayMs: 0,
    });
    try {
      const waitUnval = (pred, trigger) => new Promise(resolve => {
        const off = unvalidatedBridge.subscribe(msg => {
          if (pred(msg)) { off(); resolve(msg); }
        });
        trigger();
      });
      const usnap = await waitUnval(
        msg => msg.kind === 'snapshot',
        () => unvalidatedBridge.send({ schemaVersion: 1, sessionId: 'unit2', kind: 'hello', payload: { supportedVersions: [1], ready: true } }),
      );
      const urev = String(usnap.payload?.revision ?? '0');
      for (let i = 0; i < bad.length; ++i) {
        const rep = await waitUnval(
          msg => msg.requestId === `mock-bad-${i}`,
          () => unvalidatedBridge.send({ schemaVersion: 1, sessionId: 'unit2', kind: 'command', requestId: `mock-bad-${i}`, baseRevision: urev, payload: { op: 'app.openExternal', url: bad[i] } }),
        );
        assert.equal(rep.kind, 'error', `mock runtime must reject ${String(bad[i])}`);
        assert.equal(rep.payload.code, 'invalid');
      }
    } finally {
      unvalidatedBridge.dispose();
    }
  } finally {
    bridge.dispose();
  }
});

test('G10 browser E2E: About modal, Main/Edit headers, focus trap, dynamic version, links, copy, sponsors, 900x560 & 1100x560 layout', { timeout: 60000 }, async () => {
  await mkdir(SHOTS_DIR, { recursive: true });
  let server = null, browser = null;
  try {
    server = await serve({ port: 0 });
    browser = await chromium.launch({ executablePath: CHROME });

    const context = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const errors = [];
    const externalHttpRequests = [];
    await context.route('**/*', route => {
      const rawUrl = route.request().url();
      const parsed = new URL(rawUrl);
      if (parsed.protocol === 'data:' || parsed.protocol === 'blob:' || parsed.hostname === '127.0.0.1' || parsed.hostname === 'localhost') {
        return route.continue();
      }
      externalHttpRequests.push(rawUrl);
      return route.abort();
    });
    const page = await context.newPage();
    page.setDefaultTimeout(5000);
    page.on('pageerror', e => errors.push(`pageerror: ${e.message}`));
    page.on('console', m => { if (m.type() === 'error') errors.push(`console.error: ${m.text()}`); });

    await page.goto(server.url);
    await page.waitForFunction(() => window.verbRoom?.store?.ready);

    // Header button placement next to Theme on Main
    const mainPlacement = await page.evaluate(() => {
      const themeNext = document.querySelector('.header-actions .theme-menu')?.nextElementSibling;
      const header = document.querySelector('#app > .header');
      return {
        isAboutAfterTheme: themeNext?.getAttribute('data-about-btn') === 'true',
        headerOverflow: header ? header.scrollWidth - header.clientWidth : 999,
      };
    });
    assert.equal(mainPlacement.isAboutAfterTheme, true, 'About button must sit right after Theme on Main header');
    assert.ok(mainPlacement.headerOverflow <= 1, `Main header horizontal overflow at 1280x800: ${mainPlacement.headerOverflow}`);

    // Open About dialog on Main and audit structure, focus, content, fonts
    await page.getByRole('button', { name: 'About' }).click();
    await page.waitForSelector('dialog.about-dialog[open]');

    const aboutAudit = await page.evaluate(() => {
      const dialog = document.querySelector('dialog.about-dialog[open]');
      const scroll = dialog.querySelector('.about-scroll');
      const expectedVer = window.verbRoom?.store?.state?.capabilities?.pluginVersion;
      const verAttr = dialog.querySelector('[data-about-version]')?.getAttribute('data-about-version');
      const verText = dialog.querySelector('[data-about-version]')?.textContent?.trim();
      const paypalEl = dialog.querySelector('[data-paypal-email]');
      const contactEl = dialog.querySelector('[data-contact-email]');
      const sponsorsEl = dialog.querySelector('[data-about-sponsors]');
      const thanksSection = dialog.querySelector('[data-about-thanks]');
      const anchors = dialog.querySelectorAll('a[href]');
      const textEls = [dialog, ...dialog.querySelectorAll('h2, h3, p, li, span, code, button')];
      const smallFonts = textEls
        .filter(el => el.offsetParent !== null && getComputedStyle(el).display !== 'none')
        .map(el => ({ tag: el.tagName, cls: el.className, size: parseFloat(getComputedStyle(el).fontSize) }))
        .filter(x => x.size < 11);
      return {
        isModal: dialog.matches(':modal'),
        focusInside: dialog.contains(document.activeElement),
        expectedVer,
        verAttr,
        verText,
        fullText: dialog.textContent,
        paypalText: paypalEl?.textContent,
        paypalInteractiveParent: Boolean(paypalEl?.closest('a, button')),
        contactText: contactEl?.textContent,
        anchorCount: anchors.length,
        thanksVisible: !thanksSection?.hidden && getComputedStyle(thanksSection).display !== 'none',
        sponsorsText: sponsorsEl?.textContent,
        canScroll: scroll.scrollHeight > scroll.clientHeight,
        horizOverflow: scroll.scrollWidth - scroll.clientWidth,
        smallFonts,
      };
    });

    assert.equal(aboutAudit.isModal, true, 'About dialog must open via showModal()');
    assert.equal(aboutAudit.focusInside, true, 'Focus must move inside About dialog on open');
    assert.ok(aboutAudit.expectedVer, 'Snapshot must provide capabilities.pluginVersion');
    assert.equal(aboutAudit.verAttr, aboutAudit.expectedVer);
    assert.equal(aboutAudit.verText, `v${aboutAudit.expectedVer}`);
    assert.match(aboutAudit.fullText, /Verb Room/);
    assert.match(aboutAudit.fullText, /Physically based room reverb for moving sources and listener/);
    assert.match(aboutAudit.fullText, /AGPLv3 · Built with JUCE/);
    assert.match(aboutAudit.fullText, /What's new in 0\.3\.0/);
    assert.match(aboutAudit.fullText, /Azimuth keys/);
    assert.match(aboutAudit.fullText, /Fit to scene/);
    assert.match(aboutAudit.fullText, /Mod Out 1/);
    assert.match(aboutAudit.fullText, /Creative mode/);
    assert.match(aboutAudit.fullText, /Bake path/);
    assert.match(aboutAudit.fullText, /Record automation/);
    assert.match(aboutAudit.fullText, /Support the project/);
    assert.match(aboutAudit.fullText, /Monobank jar/);
    assert.match(aboutAudit.fullText, /45 UAH per 1 USD/);
    assert.equal(aboutAudit.paypalText, 'bodzik@gmail.com');
    assert.equal(aboutAudit.paypalInteractiveParent, false, 'PayPal email must be plain text, not a link or button');
    assert.equal(aboutAudit.contactText, 'bodzik@gmail.com');
    assert.equal(aboutAudit.anchorCount, 0, 'External URLs must not be raw <a> links that could navigate WebView');
    assert.equal(aboutAudit.thanksVisible, true);
    assert.match(aboutAudit.fullText, /Thank you to our first supporters:\s*smandrap, Teniente Powell/);
    assert.equal(aboutAudit.sponsorsText, 'smandrap, Teniente Powell');
    assert.ok(aboutAudit.horizOverflow <= 1, `Dialog scroll horizontal overflow: ${aboutAudit.horizOverflow}`);
    assert.deepEqual(aboutAudit.smallFonts, [], `Fonts < 11px inside dialog: ${JSON.stringify(aboutAudit.smallFonts)}`);

    await page.screenshot({ path: path.join(SHOTS_DIR, '01-main-about-1280x800.png') });
    await page.evaluate(() => {
      const s = document.querySelector('dialog.about-dialog .about-scroll');
      s.scrollTop = s.scrollHeight;
    });
    await page.screenshot({ path: path.join(SHOTS_DIR, '02-about-bottom-1280x800.png') });
    await page.evaluate(() => {
      document.querySelector('dialog.about-dialog .about-scroll').scrollTop = 0;
    });

    // Keyboard focus trap: Tab forward & Shift+Tab backward stay inside dialog and wrap cleanly
    const visitedIds = [];
    for (let i = 0; i < 22; ++i) {
      await page.keyboard.press('Tab');
      const state = await page.evaluate(() => {
        const dialog = document.querySelector('dialog.about-dialog');
        const ae = document.activeElement;
        return {
          inside: Boolean(dialog && ae && dialog.contains(ae)),
          id: ae?.getAttribute('data-copy-key') || ae?.getAttribute('data-link-id') || ae?.getAttribute('aria-label') || ae?.textContent?.trim(),
        };
      });
      assert.equal(state.inside, true, `Tab step ${i} escaped modal dialog (active=${state.id})`);
      visitedIds.push(state.id);
    }
    const uniqueVisited = new Set(visitedIds);
    assert.ok(uniqueVisited.size >= 8, `Expected keyboard traversal through dialog controls, got ${[...uniqueVisited].join(', ')}`);

    // Shift+Tab from first focusable wraps to last focusable inside dialog
    await page.evaluate(() => document.querySelector('dialog.about-dialog .about-close-icon').focus());
    await page.keyboard.press('Shift+Tab');
    const wrappedBack = await page.evaluate(() => ({
      inside: document.querySelector('dialog.about-dialog').contains(document.activeElement),
      isFooterClose: document.activeElement?.classList.contains('about-footer-close'),
    }));
    assert.equal(wrappedBack.inside, true);
    assert.equal(wrappedBack.isFooterClose, true, 'Shift+Tab on first focusable must wrap to footer Close');

    // External links trigger app.openExternal with exact URLs and zero network traffic
    await page.evaluate(() => {
      window.__extCalls = [];
      const store = window.verbRoom.store;
      const orig = store.command.bind(store);
      store.command = async (payload) => {
        if (payload?.op === 'app.openExternal') window.__extCalls.push(payload.url);
        return orig(payload);
      };
    });
    for (const id of ['kofi', 'monobank', 'manual', 'project', 'source']) {
      await page.click(`dialog.about-dialog [data-link-id="${id}"]`);
    }
    const extCalls = await page.evaluate(() => window.__extCalls);
    assert.deepEqual(extCalls, [
      'https://ko-fi.com/s/405ebacfcc',
      'https://send.monobank.ua/jar/7uwNLVbptd',
      'https://github.com/Bodzijun/sbp-verb-room/blob/main/docs/Manual_en.md',
      'https://bodzijun.github.io/sbp-verb-room/',
      'https://github.com/Bodzijun/sbp-verb-room',
    ]);

    // Clipboard copy (Clipboard API path + execCommand fallback + status feedback)
    await page.evaluate(() => {
      window.__copiedWrites = [];
      Object.defineProperty(navigator, 'clipboard', {
        configurable: true,
        value: { writeText: async (txt) => { window.__copiedWrites.push(txt); } },
      });
    });
    await page.click('dialog.about-dialog [data-copy-key="paypal"]');
    await page.click('dialog.about-dialog [data-copy-key="source"]');
    const clipboardCheck1 = await page.evaluate(() => ({
      writes: window.__copiedWrites,
      paypalLabel: document.querySelector('dialog.about-dialog [data-copy-key="paypal"]').textContent,
      sourceLabel: document.querySelector('dialog.about-dialog [data-copy-key="source"]').textContent,
    }));
    assert.deepEqual(clipboardCheck1.writes, ['bodzik@gmail.com', 'https://github.com/Bodzijun/sbp-verb-room']);
    assert.equal(clipboardCheck1.paypalLabel, 'Copied');
    assert.equal(clipboardCheck1.sourceLabel, 'Copied');

    // Force Clipboard API failure -> verify execCommand('copy') fallback keeps focus inside modal
    await page.evaluate(() => {
      window.__execCopyCalls = 0;
      Object.defineProperty(navigator, 'clipboard', {
        configurable: true,
        value: { writeText: async () => { throw new Error('blocked'); } },
      });
      document.execCommand = (cmd) => {
        if (cmd === 'copy') { window.__execCopyCalls += 1; return true; }
        return false;
      };
      document.querySelector('dialog.about-dialog [data-copy-key="manual"]').focus();
    });
    await page.keyboard.press('Enter');
    await page.waitForSelector('dialog.about-dialog [data-copy-key="manual"][data-copied="true"]');
    const fallbackRes = await page.evaluate(() => ({
      execCopyCalls: window.__execCopyCalls,
      manualLabel: document.querySelector('dialog.about-dialog [data-copy-key="manual"]').textContent,
      focusInside: document.querySelector('dialog.about-dialog').contains(document.activeElement),
    }));
    assert.equal(fallbackRes.execCopyCalls, 1);
    assert.equal(fallbackRes.manualLabel, 'Copied');
    assert.equal(fallbackRes.focusInside, true);

    // Esc closes modal and returns focus to Main header About button
    await page.keyboard.press('Escape');
    await page.locator('dialog.about-dialog').waitFor({ state: 'hidden' });
    await page.waitForFunction(() => document.activeElement?.getAttribute('data-about-btn') === 'true');

    // Edit room header About button placement, open, screenshot, close button -> returns focus
    await page.getByRole('button', { name: /Edit room/i }).click();
    await page.waitForSelector('.edit-header');
    const editPlacement = await page.evaluate(() => ({
      isAboutAfterTheme: document.querySelector('.edit-header .theme-menu')?.nextElementSibling?.getAttribute('data-about-btn') === 'true',
      overflow: document.querySelector('.edit-header').scrollWidth - document.querySelector('.edit-header').clientWidth,
    }));
    assert.equal(editPlacement.isAboutAfterTheme, true, 'About button must sit right after Theme on Edit room header');
    assert.ok(editPlacement.overflow <= 1, `Edit header overflow: ${editPlacement.overflow}`);

    await page.getByRole('button', { name: 'About' }).click();
    await page.waitForSelector('dialog.about-dialog[open]');
    await page.screenshot({ path: path.join(SHOTS_DIR, '03-edit-about-1280x800.png') });
    await page.locator('dialog.about-dialog .about-footer-close').click();
    await page.locator('dialog.about-dialog').waitFor({ state: 'hidden' });
    await page.waitForFunction(() => document.activeElement?.getAttribute('data-about-btn') === 'true');

    await page.getByRole('button', { name: /Done/i }).click();
    await page.waitForSelector('.header:not(.edit-header)');

    // Tight viewports: 900x560 and 1100x560 (check header non-overlap and modal fit)
    for (const [w, h, shotName] of [[900, 560, '04-about-900x560.png'], [1100, 560, '05-about-1100x560.png']]) {
      await page.setViewportSize({ width: w, height: h });
      await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
      const hdr = await page.evaluate((vw) => {
        const hEl = document.querySelector('#app > .header');
        const btn = hEl.querySelector('[data-about-btn]');
        const r = btn.getBoundingClientRect();
        const kids = [...hEl.children].filter(el => el.offsetParent !== null);
        const overlaps = [];
        for (let i = 1; i < kids.length; ++i) {
          const prev = kids[i - 1].getBoundingClientRect();
          const cur = kids[i].getBoundingClientRect();
          if (prev.right > cur.left + 0.5) overlaps.push([kids[i - 1].className, prev.right, kids[i].className, cur.left]);
        }
        const brandText = hEl.querySelector('.brand strong')?.getBoundingClientRect();
        const nextBox = hEl.querySelector('.brand')?.nextElementSibling?.getBoundingClientRect();
        if (brandText && nextBox && brandText.right > nextBox.left + 0.5) {
          overlaps.push(['brand-strong', brandText.right, 'next', nextBox.left]);
        }
        return {
          overflow: hEl.scrollWidth - hEl.clientWidth,
          overlaps,
          btnRight: r.right,
          btnWidth: r.width,
          btnFont: parseFloat(getComputedStyle(btn).fontSize),
          inView: r.left >= 0 && r.right <= vw + 0.5 && r.width > 20,
        };
      }, w);
      assert.ok(hdr.overflow <= 1, `Header horizontal overflow at ${w}x${h}: ${hdr.overflow}`);
      assert.deepEqual(hdr.overlaps, [], `Header elements overlap at ${w}x${h}: ${JSON.stringify(hdr.overlaps)}`);
      assert.equal(hdr.inView, true, `About button clipped/outside viewport at ${w}x${h}: right=${hdr.btnRight}`);
      assert.ok(hdr.btnFont >= 11, `Header About font < 11px at ${w}x${h}: ${hdr.btnFont}`);

      await page.getByRole('button', { name: 'About' }).click();
      await page.waitForSelector('dialog.about-dialog[open]');
      await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
      const box = await page.evaluate(async ([vw, vh]) => {
        const d = document.querySelector('dialog.about-dialog[open]');
        const s = d.querySelector('.about-scroll');
        const r = d.getBoundingClientRect();
        const beforeTop = s.scrollTop;
        s.scrollTop = s.scrollHeight;
        await new Promise(res => requestAnimationFrame(res));
        const afterTop = s.scrollTop;
        s.scrollTop = 0;
        return {
          left: r.left,
          top: r.top,
          right: r.right,
          bottom: r.bottom,
          fits: r.left >= 0 && r.top >= 0 && r.right <= vw + 0.5 && r.bottom <= vh + 0.5,
          scrollCanScroll: s.scrollHeight > s.clientHeight,
          didScroll: afterTop > beforeTop,
          scrollHorizOverflow: s.scrollWidth - s.clientWidth,
        };
      }, [w, h]);
      assert.equal(box.fits, true, `About dialog clipped at ${w}x${h}: ${JSON.stringify(box)}`);
      assert.equal(box.scrollCanScroll && box.didScroll, true, `About dialog must scroll at ${w}x${h}`);
      assert.ok(box.scrollHorizOverflow <= 1, `About scroll horizontal overflow at ${w}x${h}: ${box.scrollHorizOverflow}`);
      await page.screenshot({ path: path.join(SHOTS_DIR, shotName) });
      await page.locator('dialog.about-dialog .about-close-icon').click();
      await page.locator('dialog.about-dialog').waitFor({ state: 'hidden' });
    }

    // Dynamic pluginVersion verification (never hardcoded)
    const verPage = await context.newPage();
    verPage.setDefaultTimeout(5000);
    await verPage.route('**/fixtures/snapshot.default-physical.json', async route => {
      const resp = await route.fetch();
      const data = await resp.json();
      for (const m of data.messages || []) {
        if (m?.payload?.capabilities) m.payload.capabilities.pluginVersion = '0.9.42-dyn';
      }
      if (data.payload?.capabilities) data.payload.capabilities.pluginVersion = '0.9.42-dyn';
      if (data.capabilities) data.capabilities.pluginVersion = '0.9.42-dyn';
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(data) });
    });
    await verPage.goto(server.url);
    await verPage.waitForFunction(() => window.verbRoom?.store?.ready);
    await verPage.getByRole('button', { name: 'About' }).click();
    await verPage.waitForSelector('dialog.about-dialog[open]');
    const dynVer = await verPage.locator('dialog.about-dialog [data-about-version]').textContent();
    assert.equal(dynVer?.trim(), 'v0.9.42-dyn');
    await verPage.close();

    // Empty SPONSORS = [] hides entire "Thank you to our first supporters:" line
    const emptyPage = await context.newPage();
    emptyPage.setDefaultTimeout(5000);
    await emptyPage.route('**/src/about-data.js', async route => {
      const resp = await route.fetch();
      const js = await resp.text();
      const patched = js.replace(/export const SPONSORS = \[[\s\S]*?\];/, 'export const SPONSORS = [];');
      await route.fulfill({ status: 200, contentType: 'application/javascript', body: patched });
    });
    await emptyPage.goto(server.url);
    await emptyPage.waitForFunction(() => window.verbRoom?.store?.ready);
    await emptyPage.getByRole('button', { name: 'About' }).click();
    await emptyPage.waitForSelector('dialog.about-dialog[open]');
    const emptyState = await emptyPage.evaluate(() => {
      const d = document.querySelector('dialog.about-dialog[open]');
      const t = d.querySelector('[data-about-thanks]');
      return {
        hidden: Boolean(t?.hidden),
        hasSentence: d.textContent.includes('Thank you to our first supporters'),
      };
    });
    assert.equal(emptyState.hidden, true);
    assert.equal(emptyState.hasSentence, false, 'Empty SPONSORS must hide the supporters sentence completely');
    await emptyPage.close();

    assert.deepEqual(externalHttpRequests, [], `Unexpected external HTTP requests: ${externalHttpRequests.join(', ')}`);
    assert.deepEqual(errors, [], `Unexpected page/console errors: ${errors.join('\n')}`);
  } finally {
    await browser?.close().catch(() => {});
    server?.server?.closeAllConnections?.();
    if (server?.server) await new Promise(r => server.server.close(r));
  }
});
