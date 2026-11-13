// Verb Room WebView shell page (T1). Proves the mechanics only: offline resources, the ready handshake,
// a ping round trip, resize reporting. No project data. The real UI replaces this directory later.
import { getNativeFunction } from "./juce/index.js";
import { installHostKeys } from "./host-keys.js";

const PAGE_VERSION = "t1-shell-1";
const root = document.documentElement;
const $ = (id) => document.getElementById(id);
const backend = window.__JUCE__.backend;

const nativeReady = getNativeFunction("ready");
const nativePing = getNativeFunction("ping");
const nativeUseClassic = getNativeFunction("useClassicUi");
const nativeHostKey = getNativeFunction("hostKey");

// Keys the page does not use go to the DAW (Space = transport). Typing into the demo field stays in the page.
const hostKeys = installHostKeys({
  send: (payload) => nativeHostKey(payload),
  onForward: (p) => { $("lastKey").textContent = p.phase + " " + (p.code || p.vk) + (p.repeat ? " (repeat)" : ""); },
});
// Demo of "the page used this key": Ctrl+Y marks the event handled, so it is not forwarded.
document.addEventListener("keydown", (e) => {  // document runs before the window-level forwarder
  if (e.ctrlKey && e.code === "KeyY") { hostKeys.markHandled(e); $("lastKey").textContent = "Ctrl+Y used by the page"; }
});

let pings = 0;
let seq = 0;
let heartbeatTimer = null;

function setState(state, text) {
  root.dataset.state = state;
  $("status").textContent = text;
}

async function doPing() {
  const mySeq = ++seq;
  const t0 = performance.now();
  try {
    const reply = await nativePing({ seq: mySeq });
    const ok = !!reply && reply.seq === mySeq;
    if (ok) {
      pings += 1;
      $("pings").textContent = String(pings);
      if (reply.revision !== undefined) $("revision").textContent = String(reply.revision);
    }
    backend.emitEvent("verb.pingDone", { seq: mySeq, ok, rttMs: Math.round(performance.now() - t0) });
  } catch (e) {
    backend.emitEvent("verb.pingDone", { seq: mySeq, ok: false, rttMs: -1 });
  }
}

function reportSize() {
  const w = window.innerWidth, h = window.innerHeight;
  $("size").textContent = w + " × " + h;
  backend.emitEvent("verb.pageSize", { w, h, dpr: window.devicePixelRatio });
}

function startHeartbeat(ms) {
  if (heartbeatTimer !== null) clearInterval(heartbeatTimer);
  heartbeatTimer = setInterval(doPing, ms > 0 ? ms : 2000);
}

backend.addEventListener("verb.hello", (hello) => {
  $("pluginVersion").textContent = hello.pluginVersion;
  $("revision").textContent = String(hello.revision);
  $("capabilities").textContent = JSON.stringify(hello.capabilities);
  $("pageVersion").textContent = PAGE_VERSION;
  hostKeys.setEnabled(!hello.capabilities || hello.capabilities.hostKeys !== false);
  setState("ready", "Connected to the plugin.");
  startHeartbeat(hello.heartbeatMs);
  reportSize();
});
backend.addEventListener("verb.pingRequest", () => { doPing(); });

window.addEventListener("resize", reportSize);
$("pingBtn").addEventListener("click", doPing);
$("classicBtn").addEventListener("click", () => { nativeUseClassic(); });
$("demoText").addEventListener("input", () => { $("typed").textContent = $("demoText").value; });

// Listeners are registered above; only now announce readiness (the answer arrives as the "verb.hello" event).
nativeReady({ pageVersion: PAGE_VERSION, supportedSchemaVersions: [1] }).catch(() => {
  setState("error", "The plugin did not accept the handshake.");
});
