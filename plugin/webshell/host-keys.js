// host-keys.js — gives keys the page did not use back to the plugin host (DAW).
//
// Why: in a native JUCE editor a key nobody handles is posted to the host window. A focused WebView2 swallows keys in its
// own window, so the host never sees Space (play/stop) and friends. This module watches the page's keyboard events and,
// for every key the page did not use and that is not typed into a text field, calls a native function that posts the key to
// the host window (C++: VerbWebEditor::handleHostKey).
//
// Self-contained ES module, no imports, no DOM access at import time. Reuse it unchanged:
//
//   import { getNativeFunction } from "./juce/index.js";
//   import { installHostKeys } from "./host-keys.js";
//   const hostKey = getNativeFunction("hostKey");
//   const hostKeys = installHostKeys({ send: (payload) => hostKey(payload) });
//   // in the page's own shortcut handlers:  hostKeys.markHandled(event)   (or event.preventDefault())
//
// Rule: "the page did not handle it, and focus is not in a text-editing element -> forward".
//   * handled = event.defaultPrevented, hostKeys.markHandled(event), options.isHandled(event) is true, or a handler called
//     stopPropagation() (the event never reaches this listener);
//   * text-editing = input (except button-like types), textarea, select, contenteditable, role textbox/combobox/searchbox/
//     spinbutton/slider; nothing is forwarded from there. Opt a whole subtree out with data-host-keys="off";
//   * a focused button / link / checkbox / radio / summary keeps Space and Enter for itself (activation);
//   * Tab never leaves the page (focus navigation); IME composition is never forwarded.
// A forwarded key-down gets preventDefault() (the key does exactly one thing: the host's), except pure modifier keys.
// A key-up is forwarded exactly when its key-down was, whatever has the focus by then; a window blur releases held keys.
// Mouse-clicked buttons give up the keyboard focus (options.blurAfterClick) so that Space still reaches the host afterwards.
//
// Payload given to send(): { phase: "down"|"up", vk, code, key, ctrl, shift, alt, meta, repeat }  (vk = Windows virtual key).

const BUTTON_LIKE_INPUTS = new Set(["button", "submit", "reset", "image", "checkbox", "radio"]);
const EDITABLE_ROLES = new Set(["textbox", "searchbox", "combobox", "spinbutton", "slider", "listbox", "menu", "tree", "grid"]);
const ACTIVATION_ROLES = new Set(["button", "checkbox", "radio", "switch", "tab", "menuitem", "menuitemcheckbox", "menuitemradio", "option", "link"]);
const ACTIVATION_CODES = new Set(["Space", "Enter", "NumpadEnter"]);
const MODIFIER_VKS = new Set([16, 17, 18, 91, 92]);
const DEFAULT_NEVER_FORWARD = ["Tab", "MetaLeft", "MetaRight", "ContextMenu"];
const DEFAULT_BLUR_SELECTOR = 'button, [role="button"], input[type="checkbox"], input[type="radio"], summary, a[href]';

// KeyboardEvent.keyCode is the Windows virtual key in Chromium; this table covers events that carry only `code`.
const CODE_TO_VK = (() => {
  const t = {
    Backspace: 8, Tab: 9, Enter: 13, NumpadEnter: 13, ShiftLeft: 16, ShiftRight: 16, ControlLeft: 17, ControlRight: 17,
    AltLeft: 18, AltRight: 18, Pause: 19, CapsLock: 20, Escape: 27, Space: 32, PageUp: 33, PageDown: 34, End: 35, Home: 36,
    ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, PrintScreen: 44, Insert: 45, Delete: 46,
    MetaLeft: 91, MetaRight: 92, ContextMenu: 93, NumpadMultiply: 106, NumpadAdd: 107, NumpadSubtract: 109, NumpadDecimal: 110,
    NumpadDivide: 111, NumLock: 144, ScrollLock: 145, Semicolon: 186, Equal: 187, Comma: 188, Minus: 189, Period: 190,
    Slash: 191, Backquote: 192, BracketLeft: 219, Backslash: 220, BracketRight: 221, Quote: 222,
  };
  for (let i = 0; i < 26; i++) t["Key" + String.fromCharCode(65 + i)] = 65 + i;
  for (let i = 0; i < 10; i++) { t["Digit" + i] = 48 + i; t["Numpad" + i] = 96 + i; }
  for (let i = 1; i <= 24; i++) t["F" + i] = 111 + i;
  return t;
})();

function eventVk(e) {
  const k = e.keyCode | 0;
  if (k >= 1 && k <= 254 && k !== 229) return k;
  return CODE_TO_VK[e.code] || 0;
}

function eventTarget(e) {
  const path = typeof e.composedPath === "function" ? e.composedPath() : [];
  const first = path.length ? path[0] : e.target;
  return first && first.nodeType === 1 ? first : (e.target && e.target.nodeType === 1 ? e.target : null);
}

function roleOf(el) {
  const r = el.getAttribute && el.getAttribute("role");
  return r ? r.trim().toLowerCase() : "";
}

/** True when keys typed here belong to the page: text fields, selects, sliders, content-editable areas. */
export function isTextEditingTarget(el) {
  if (!el || el.nodeType !== 1) return false;
  const tag = el.tagName;
  if (tag === "TEXTAREA" || tag === "SELECT") return true;
  if (tag === "INPUT") return !BUTTON_LIKE_INPUTS.has(String(el.type || "text").toLowerCase());
  if (el.isContentEditable) return true;
  return EDITABLE_ROLES.has(roleOf(el));
}

/** True when Space/Enter on this element activate it (a focused button etc.). */
export function isActivationTarget(el) {
  if (!el || el.nodeType !== 1) return false;
  const tag = el.tagName;
  if (tag === "BUTTON" || tag === "SUMMARY") return true;
  if (tag === "A") return el.hasAttribute("href");
  if (tag === "INPUT") return BUTTON_LIKE_INPUTS.has(String(el.type || "text").toLowerCase());
  return ACTIVATION_ROLES.has(roleOf(el));
}

/**
 * @param {object} options
 * @param {(payload: object) => (Promise|void)} options.send   required: calls the native function `hostKey`.
 * @param {(event: KeyboardEvent) => boolean} [options.isHandled]  true = the page used this key-down (do not forward).
 * @param {boolean} [options.forwardRepeats=true]  forward auto-repeat key-downs (flagged as repeats), like a native editor.
 * @param {string[]} [options.neverForward]        KeyboardEvent.code values that stay in the page (default Tab, Meta, ContextMenu).
 * @param {string|null} [options.blurAfterClick]   selector of elements that give up focus after a mouse click (null = off).
 * @param {(info: object) => void} [options.onForward]  called with the payload of every forwarded key (diagnostics / demo).
 * @param {EventTarget} [options.target=window]
 * @returns {{ markHandled(e: Event): void, setEnabled(on: boolean): void, isEnabled(): boolean, releaseAll(): void, dispose(): void }}
 */
export function installHostKeys(options) {
  const opts = options || {};
  if (typeof opts.send !== "function") throw new TypeError("installHostKeys: options.send is required");
  const target = opts.target || window;
  const neverForward = new Set(opts.neverForward || DEFAULT_NEVER_FORWARD);
  const forwardRepeats = opts.forwardRepeats !== false;
  const blurSelector = opts.blurAfterClick === undefined ? DEFAULT_BLUR_SELECTOR : opts.blurAfterClick;
  const handled = new WeakSet();
  const down = new Map();  // vk -> last payload, for keys whose key-down was forwarded and not yet released
  let enabled = true;

  function send(payload) {
    try {
      const r = opts.send(payload);
      if (r && typeof r.catch === "function") r.catch(() => {});
    } catch (_) { /* the page must never break because the plugin is gone */ }
    if (opts.onForward) { try { opts.onForward(payload); } catch (_) {} }
  }

  function payloadOf(e, vk, phase) {
    return {
      phase, vk, code: e.code || "", key: e.key || "",
      ctrl: !!e.ctrlKey, shift: !!e.shiftKey, alt: !!e.altKey, meta: !!e.metaKey,
      repeat: phase === "down" && !!e.repeat,
    };
  }

  function onKeyDown(e) {
    if (!enabled || e.isComposing || e.keyCode === 229 || e.key === "Process" || e.defaultPrevented || handled.has(e)) return;  // 229 = IME
    const vk = eventVk(e);
    if (!vk || neverForward.has(e.code)) return;
    if (e.repeat && (!forwardRepeats || !down.has(vk))) return;  // a repeat only continues a key that was forwarded
    const el = eventTarget(e);
    if (el && el.closest && el.closest('[data-host-keys="off"]')) return;
    if (isTextEditingTarget(el)) return;
    if (isActivationTarget(el) && ACTIVATION_CODES.has(e.code)) return;
    if (opts.isHandled && opts.isHandled(e)) return;
    const payload = payloadOf(e, vk, "down");
    down.set(vk, payload);
    if (!MODIFIER_VKS.has(vk)) e.preventDefault();  // one key, one effect: the host's
    send(payload);
  }

  function onKeyUp(e) {  // capture phase: a key-up must reach the host even if the page stops its propagation
    if (e.keyCode === 229) return;
    const vk = eventVk(e);
    if (!vk || !down.has(vk)) return;
    down.delete(vk);
    if (!MODIFIER_VKS.has(vk)) e.preventDefault();
    send(payloadOf(e, vk, "up"));
  }

  function releaseAll() {
    for (const [vk, p] of Array.from(down)) send({ ...p, phase: "up", repeat: false, vk });
    down.clear();
  }

  function onBlur(e) { if (e.target === target || e.target === window) releaseAll(); }  // the window lost focus, not an element
  function onVisibility() { if (document.visibilityState === "hidden") releaseAll(); }

  function onClick(e) {
    // A mouse click (detail > 0) leaves the keyboard focus on the clicked button, which would then eat Space. Keyboard
    // activation (Tab to the button, Space/Enter) has detail 0 and keeps its focus.
    if (!blurSelector || !(e.detail > 0)) return;
    const el = eventTarget(e);
    const hit = el && el.closest ? el.closest(blurSelector) : null;
    if (hit && hit === document.activeElement && !isTextEditingTarget(hit) && typeof hit.blur === "function") hit.blur();
  }

  target.addEventListener("keydown", onKeyDown, false);   // bubble: after the page's own handlers had their say
  target.addEventListener("keyup", onKeyUp, true);
  target.addEventListener("blur", onBlur, true);
  target.addEventListener("click", onClick, true);
  document.addEventListener("visibilitychange", onVisibility);

  return {
    markHandled(e) { if (e && typeof e === "object") handled.add(e); },
    setEnabled(on) { enabled = !!on; if (!enabled) releaseAll(); },
    isEnabled() { return enabled; },
    releaseAll,
    dispose() {
      releaseAll();
      target.removeEventListener("keydown", onKeyDown, false);
      target.removeEventListener("keyup", onKeyUp, true);
      target.removeEventListener("blur", onBlur, true);
      target.removeEventListener("click", onClick, true);
      document.removeEventListener("visibilitychange", onVisibility);
    },
  };
}
