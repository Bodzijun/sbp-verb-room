// Custom dark dropdowns are button + listbox, not <select>; this opens one and picks a label.
export async function pickOption(page, name, optionText) {
  await page.getByRole('combobox', { name, exact: true }).click();
  await page.locator('.dd-popup [role=option]', { hasText: optionText }).first().click();
}
// Native scrollbars / native form controls that must never appear in the instrument chrome.
export async function nativeChromeAudit(page) {
  return page.evaluate(() => {
    const visible = e => { const r = e.getBoundingClientRect(), cs = getComputedStyle(e); return r.width > 0 && r.height > 0 && cs.visibility !== 'hidden' && cs.display !== 'none'; };
    const scrollbars = [], natives = [];
    for (const e of document.querySelectorAll('body, body *')) {
      if (!visible(e)) continue; const cs = getComputedStyle(e);
      if (['auto', 'scroll'].includes(cs.overflowY) || ['auto', 'scroll'].includes(cs.overflowX)) {
        const v = e.offsetWidth - e.clientWidth - parseFloat(cs.borderLeftWidth) - parseFloat(cs.borderRightWidth), h = e.offsetHeight - e.clientHeight - parseFloat(cs.borderTopWidth) - parseFloat(cs.borderBottomWidth);
        if (v > 0.5 || h > 0.5) scrollbars.push(`${e.tagName}.${e.className}`.slice(0, 60));
      }
      if (e.tagName === 'SELECT') natives.push('select');
      if (e.tagName === 'INPUT' && ['checkbox', 'number', 'radio', 'range'].includes(e.type)) natives.push(`input[${e.type}]`);
    }
    const root = document.documentElement; if (root.scrollWidth > innerWidth + 1 || root.scrollHeight > innerHeight + 1) scrollbars.push('document');
    return { scrollbars, natives };
  });
}
