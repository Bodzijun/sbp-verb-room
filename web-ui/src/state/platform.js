// Platform helpers. Windows/Linux behaviour is unchanged: there Ctrl (and, as before, Meta) is the command modifier.
// On macOS the command modifier is Cmd (metaKey); Ctrl+click is a context click there, so it is not treated as a command.
export const isMac = (() => {
  try {
    const n = globalThis.navigator || {};
    return /Mac|iPhone|iPad|iPod/i.test(String(n.userAgentData?.platform || n.platform || n.userAgent || ''));
  } catch { return false; }
})();
export function modKey(e) { return isMac ? !!e.metaKey : !!(e.ctrlKey || e.metaKey); }
