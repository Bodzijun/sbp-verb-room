import { button, element } from './dom.js';
import {
  LINKS,
  PLUGIN_INFO,
  SPONSORS,
  SUPPORT,
  TIPS,
  WHATS_NEW,
  WHATS_NEW_VERSION,
} from '../about-data.js';

const FOCUSABLE_SELECTOR = [
  'button:not([disabled])',
  '[href]',
  'input:not([disabled])',
  'select:not([disabled])',
  'textarea:not([disabled])',
  '[tabindex]:not([tabindex="-1"])',
].join(',');

export async function copyTextToClipboard(text, doc = document, nav = navigator) {
  if (nav?.clipboard?.writeText) {
    try {
      await nav.clipboard.writeText(text);
      return true;
    } catch {
      // Fall back to execCommand when Clipboard API is blocked (e.g. embedded WebView)
    }
  }
  if (!doc?.body || typeof doc.createElement !== 'function') return false;
  const ta = doc.createElement('textarea');
  ta.value = text;
  ta.setAttribute('readonly', '');
  ta.style.position = 'fixed';
  ta.style.top = '-9999px';
  ta.style.left = '-9999px';
  ta.style.opacity = '0';
  doc.body.appendChild(ta);
  const prevFocus = doc.activeElement;
  try {
    ta.focus({ preventScroll: true });
    ta.select();
    ta.setSelectionRange(0, text.length);
    return typeof doc.execCommand === 'function' ? Boolean(doc.execCommand('copy')) : false;
  } catch {
    return false;
  } finally {
    ta.remove();
    if (prevFocus && prevFocus.isConnected && typeof prevFocus.focus === 'function') {
      prevFocus.focus({ preventScroll: true });
    }
  }
}

export function createAboutDialog({ store, getBridgeMode, getSponsors = () => SPONSORS } = {}) {
  const dialog = element('dialog', {
    className: 'about-dialog',
    'aria-labelledby': 'about-dialog-title',
    'aria-describedby': 'about-dialog-tagline',
  });

  const versionBadge = element('span', { className: 'about-version-pill', 'data-about-version': '' });
  const modeBadge = element('span', { className: 'about-mode-pill', hidden: 'hidden' });
  const statusEl = element('span', { className: 'about-status', 'aria-live': 'polite', role: 'status' });
  let lastTrigger = null;
  const resetTimers = new Map();
  let statusTimer = null;

  const setStatus = (text, kind = 'info') => {
    statusEl.textContent = text || '';
    statusEl.className = kind === 'error' ? 'about-status is-error' : 'about-status';
    if (statusTimer) clearTimeout(statusTimer);
    if (text) statusTimer = setTimeout(() => { statusEl.textContent = ''; statusEl.className = 'about-status'; }, 2400);
  };

  const makeCopyButton = (label, value, testKey) => {
    let btn;
    btn = button(label, async () => {
      const ok = await copyTextToClipboard(value);
      if (!ok) {
        setStatus('Copy failed in this environment.', 'error');
        return;
      }
      const prev = resetTimers.get(btn);
      if (prev) clearTimeout(prev);
      btn.textContent = 'Copied';
      btn.classList.add('is-copied');
      btn.setAttribute('data-copied', 'true');
      setStatus(`Copied ${value}`);
      resetTimers.set(btn, setTimeout(() => {
        btn.textContent = label;
        btn.classList.remove('is-copied');
        btn.removeAttribute('data-copied');
      }, 1500));
    }, { className: 'about-copy-btn', 'data-copy-value': value, 'data-copy-key': testKey });
    return btn;
  };

  const openExternal = async (url) => {
    try {
      const res = await store?.command?.({ op: 'app.openExternal', url });
      const failed = res && (
        res.kind === 'error'
        || res.ok === false
        || (Array.isArray(res) && res.some(item => item?.kind === 'error'))
      );
      if (failed) setStatus('Could not open browser. Use Copy link instead.', 'error');
      else setStatus('Opening link in default browser...');
    } catch {
      setStatus('Could not open browser. Use Copy link instead.', 'error');
    }
  };

  const headerCloseBtn = button('\u00d7', () => close(), {
    className: 'about-close-icon',
    'aria-label': 'Close About dialog',
    title: 'Close (Esc)',
  });

  const footerCloseBtn = button('Close', () => close(), {
    className: 'about-footer-close',
  });

  const heroSection = element('div', { className: 'about-hero' }, [
    element('div', { className: 'about-hero-top' }, [
      element('div', { className: 'about-brand-wrap' }, [
        element('span', { className: 'brand-icon', 'aria-hidden': 'true', text: '\u2b21' }),
        element('div', { className: 'about-title-group' }, [
          element('div', { className: 'about-title-row' }, [
            element('h2', { id: 'about-dialog-title', text: PLUGIN_INFO.name }),
            versionBadge,
            modeBadge,
          ]),
          element('p', { id: 'about-dialog-tagline', className: 'about-tagline', text: PLUGIN_INFO.tagline }),
          element('p', { className: 'about-license', text: PLUGIN_INFO.license }),
        ]),
      ]),
      headerCloseBtn,
    ]),
  ]);

  const whatsNewSection = element('section', { className: 'about-section', 'aria-labelledby': 'about-whats-new' }, [
    element('h3', { id: 'about-whats-new', text: `What's new in ${WHATS_NEW_VERSION}` }),
    element('ul', { className: 'about-list' }, WHATS_NEW.map(item => element('li', { text: item }))),
  ]);

  const tipsSection = element('section', { className: 'about-section', 'aria-labelledby': 'about-tips' }, [
    element('h3', { id: 'about-tips', text: 'Tips' }),
    element('ul', { className: 'about-list' }, TIPS.map(item => element('li', { text: item }))),
  ]);

  const supportSection = element('section', { className: 'about-section', 'aria-labelledby': 'about-support' }, [
    element('h3', { id: 'about-support', text: 'Support' }),
    element('p', { className: 'about-copy-lead', text: SUPPORT.intro }),
    element('div', { className: 'about-support-grid' }, [
      element('div', { className: 'about-cta-row' }, [
        button(SUPPORT.kofi.label, () => openExternal(SUPPORT.kofi.url), {
          className: 'about-kofi-btn primary',
          'data-external-url': SUPPORT.kofi.url,
          'data-link-id': 'kofi',
        }),
        element('div', { className: 'about-meta-col' }, [
          element('span', { className: 'about-item-label', text: SUPPORT.kofi.platform }),
          element('code', { className: 'about-url-text', text: SUPPORT.kofi.url }),
        ]),
        makeCopyButton('Copy link', SUPPORT.kofi.url, 'kofi'),
      ]),
      element('div', { className: 'about-row' }, [
        button(SUPPORT.monobank.label, () => openExternal(SUPPORT.monobank.url), {
          className: 'about-link-btn',
          'data-external-url': SUPPORT.monobank.url,
          'data-link-id': 'monobank',
        }),
        element('div', { className: 'about-meta-col' }, [
          element('span', { className: 'about-item-note', text: SUPPORT.monobank.note }),
          element('code', { className: 'about-url-text', text: SUPPORT.monobank.url }),
        ]),
        makeCopyButton('Copy link', SUPPORT.monobank.url, 'monobank'),
      ]),
      element('div', { className: 'about-row' }, [
        element('span', { className: 'about-static-label', text: SUPPORT.paypal.label }),
        element('div', { className: 'about-meta-col' }, [
          element('code', { className: 'about-email-text', 'data-paypal-email': SUPPORT.paypal.email, text: SUPPORT.paypal.email }),
          element('span', { className: 'about-item-note', text: 'Email address shown as text \u2014 copy to send via PayPal.' }),
        ]),
        makeCopyButton('Copy', SUPPORT.paypal.email, 'paypal'),
      ]),
      element('div', { className: 'about-row' }, [
        element('span', { className: 'about-static-label', text: SUPPORT.contact.label }),
        element('div', { className: 'about-meta-col' }, [
          element('code', { className: 'about-email-text', 'data-contact-email': SUPPORT.contact.email, text: SUPPORT.contact.email }),
        ]),
        makeCopyButton('Copy', SUPPORT.contact.email, 'contact'),
      ]),
    ]),
  ]);

  const linksSection = element('section', { className: 'about-section', 'aria-labelledby': 'about-links' }, [
    element('h3', { id: 'about-links', text: 'Links' }),
    element('div', { className: 'about-links-list' }, LINKS.map(item => element('div', { className: 'about-row' }, [
      button(item.label, () => openExternal(item.url), {
        className: 'about-link-btn',
        'data-external-url': item.url,
        'data-link-id': item.id,
      }),
      element('code', { className: 'about-url-text about-meta-col', text: item.url }),
      makeCopyButton('Copy link', item.url, item.id),
    ]))),
  ]);

  const thanksSection = element('section', {
    className: 'about-section about-thanks',
    'data-about-thanks': 'true',
    'aria-labelledby': 'about-thanks-heading',
  });
  const renderSponsors = () => {
    const raw = typeof getSponsors === 'function' ? getSponsors() : SPONSORS;
    const names = Array.isArray(raw) ? raw.map(s => String(s || '').trim()).filter(Boolean) : [];
    if (!names.length) {
      thanksSection.hidden = true;
      thanksSection.replaceChildren();
      return;
    }
    thanksSection.hidden = false;
    thanksSection.replaceChildren(
      element('h3', { id: 'about-thanks-heading', text: 'Thanks' }),
      element('p', { className: 'about-thanks-line' }, [
        document.createTextNode('Thank you to our first supporters: '),
        element('strong', { 'data-about-sponsors': 'true', text: names.join(', ') }),
      ]),
    );
  };

  const scrollBody = element('div', { className: 'scroll about-scroll' }, [
    whatsNewSection,
    tipsSection,
    supportSection,
    linksSection,
    thanksSection,
  ]);

  const footer = element('div', { className: 'about-footer' }, [
    statusEl,
    footerCloseBtn,
  ]);

  const card = element('div', { className: 'about-card' }, [
    heroSection,
    scrollBody,
    footer,
  ]);
  dialog.append(card);

  const sync = (state) => {
    const rawVer = state?.capabilities?.pluginVersion;
    const ver = rawVer ? String(rawVer) : '\u2014';
    versionBadge.textContent = rawVer ? `v${ver}` : '\u2014';
    versionBadge.setAttribute('data-about-version', ver);
    const isMock = typeof getBridgeMode === 'function' && getBridgeMode() === 'mock';
    if (isMock) {
      modeBadge.hidden = false;
      modeBadge.textContent = 'mock fixture';
    } else {
      modeBadge.hidden = true;
      modeBadge.textContent = '';
    }
    renderSponsors();
  };

  const getFocusable = () => [...dialog.querySelectorAll(FOCUSABLE_SELECTOR)]
    .filter(el => !el.closest('[hidden]') && el.offsetParent !== null);

  function close({ restoreFocus = true } = {}) {
    if (dialog.open) dialog.close();
    if (!restoreFocus) return;
    const fallbackBtn = document.querySelector('[data-about-btn]');
    const target = (lastTrigger && lastTrigger.isConnected) ? lastTrigger : fallbackBtn;
    if (target && typeof target.focus === 'function') {
      requestAnimationFrame(() => {
        if (target.isConnected) target.focus({ preventScroll: true });
      });
    }
  }

  function open(triggerEl) {
    lastTrigger = triggerEl || document.querySelector('[data-about-btn]') || document.activeElement;
    sync(store?.state);
    if (!dialog.isConnected) document.body.appendChild(dialog);
    if (!dialog.open) dialog.showModal();
    scrollBody.scrollTop = 0;
    const focusables = getFocusable();
    const first = headerCloseBtn && !headerCloseBtn.disabled ? headerCloseBtn : focusables[0];
    if (first && typeof first.focus === 'function') first.focus({ preventScroll: true });
  }

  dialog.addEventListener('cancel', e => {
    e.preventDefault();
    close({ restoreFocus: true });
  });

  dialog.addEventListener('click', e => {
    if (e.target === dialog) close({ restoreFocus: true });
  });

  dialog.addEventListener('keydown', e => {
    if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      close({ restoreFocus: true });
      return;
    }
    if (e.key !== 'Tab') return;
    const focusables = getFocusable();
    if (!focusables.length) return;
    const first = focusables[0];
    const last = focusables[focusables.length - 1];
    const active = document.activeElement;
    if (e.shiftKey) {
      if (active === first || !dialog.contains(active)) {
        e.preventDefault();
        last.focus({ preventScroll: true });
      }
    } else if (active === last || !dialog.contains(active)) {
      e.preventDefault();
      first.focus({ preventScroll: true });
    }
  });

  if (typeof document !== 'undefined' && document.body && !dialog.isConnected) {
    document.body.appendChild(dialog);
  }
  sync(store?.state);

  return {
    dialog,
    open,
    close,
    sync,
    destroy() {
      for (const t of resetTimers.values()) clearTimeout(t);
      resetTimers.clear();
      if (statusTimer) clearTimeout(statusTimer);
      if (dialog.open) dialog.close();
      dialog.remove();
    },
  };
}
