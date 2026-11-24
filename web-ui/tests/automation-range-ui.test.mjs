import assert from 'node:assert/strict';
import test from 'node:test';
import { createRequire } from 'node:module';
import { serve } from '../scripts/serve.mjs';

const require = createRequire(import.meta.url);
let playwright;
for (const name of [process.env.PLAYWRIGHT_MODULE, 'playwright', 'C:/Users/User/AppData/Local/ms-playwright-go/1.57.0/package'].filter(Boolean)) {
  try { playwright = require(name); break; } catch {}
}
const CHROME = process.env.CHROME_PATH || 'C:/Program Files/Google/Chrome/Application/chrome.exe';

test('G3 UI: Automation range accordion, Fit confirm, dotted frame, clamp warning, Undo & 900x560 scroll', { timeout: 60000 }, async t => {
  if (!playwright) { t.skip('Playwright not available'); return; }
  const { server, url } = await serve();
  let browser;
  try {
    browser = await playwright.chromium.launch({ headless: true, executablePath: CHROME });
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 }, reducedMotion: 'reduce' });
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.goto(`${url}/?fixture=default-physical#edit`);
    await page.waitForFunction(() => window.verbRoom?.store.ready && document.querySelector('.edit-inspector'));

    await page.getByRole('tab', { name: 'Scene', exact: true }).click();
    const accBtn = page.getByRole('button', { name: /Automation range/ });
    await accBtn.waitFor();
    assert.equal(await accBtn.getAttribute('aria-expanded'), 'false');
    assert.equal(await page.evaluate(() => document.querySelector('.viewport')?.dataset.autoRangeBox || ''), '');

    await accBtn.click();
    assert.equal(await accBtn.getAttribute('aria-expanded'), 'true');
    await page.waitForFunction(() => document.querySelector('.viewport')?.dataset.autoRangeBox === 'visible');

    // Confirm prompt before changing from ±200 m default
    await page.getByRole('button', { name: 'Reset (±200 m)', exact: true }).waitFor();
    assert.equal(await page.getByRole('button', { name: 'Reset (±200 m)', exact: true }).isDisabled(), true);
    await page.getByRole('button', { name: 'Fit to scene', exact: true }).click();
    await page.getByRole('button', { name: 'Confirm fit', exact: true }).waitFor();
    await page.getByRole('button', { name: 'Confirm fit', exact: true }).click();

    await page.waitForFunction(() => {
      const ar = window.verbRoom.store.state.document.scene.automationRange;
      return ar && ar.xM[0] > -200 && ar.xM[1] < 200;
    });
    assert.equal(await page.getByRole('button', { name: 'Reset (±200 m)', exact: true }).isDisabled(), false);

    // Check parameter slider updates aria-valuemin/valuemax
    await page.evaluate(() => window.verbRoom.store.setView({ selection: [{ kind: 'source', index: 0 }] }, false));
    await page.getByRole('tab', { name: 'Selected', exact: true }).click();
    await page.waitForFunction(() => {
      const sl = document.querySelector('[data-parameter="s1x"] [role="slider"]');
      const ar = window.verbRoom.store.state.document.scene.automationRange;
      return sl && Number(sl.getAttribute('aria-valuemax')) === ar.xM[1];
    });

    // Manual narrowing + clamping note + 1-step Undo
    await page.getByRole('tab', { name: 'Scene', exact: true }).click();
    const xMaxInput = page.getByRole('textbox', { name: 'Automation X max, m', exact: true });
    await xMaxInput.dblclick();
    await xMaxInput.fill('2.5');
    await page.waitForFunction(() => (document.querySelector('.auto-range-clamp')?.textContent || '').includes('object(s) outside the new range will be clamped.'));
    await xMaxInput.press('Enter');
    await page.waitForFunction(() => window.verbRoom.store.state.document.scene.automationRange.xM[1] === 2.5);

    await page.evaluate(() => window.verbRoom.store.command({ op: 'history.undo' }));
    await page.waitForFunction(() => window.verbRoom.store.state.document.scene.automationRange.xM[1] > 2.5);

    // Collapsing section hides dotted box in viewport (Owner Decision 3-A)
    await accBtn.click();
    await page.waitForFunction(() => !document.querySelector('.viewport')?.dataset.autoRangeBox);

    // Small window 900x560 layout & scroll check with Automation range expanded
    await page.setViewportSize({ width: 900, height: 560 });
    await accBtn.click();
    await page.waitForFunction(() => document.documentElement.scrollWidth <= innerWidth);
    const fitsClean = await page.evaluate(() => {
      const body = document.querySelector('.edit-inspector .context-body');
      const box = document.getElementById('acc-scene.autoRange');
      return !!body && !!box && body.scrollWidth <= body.clientWidth + 2 && box.getBoundingClientRect().height > 50;
    });
    assert.equal(fitsClean, true, 'Automation range fits cleanly inside 900x560 scroll container without horizontal overflow');

    assert.deepEqual(errors, []);
  } finally {
    await browser?.close();
    server.close();
  }
});
