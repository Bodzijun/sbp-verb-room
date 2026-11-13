// T1 test of the WebView editor shell. Run as CTest `web_shell` (seconds, not minutes).
// Part A needs no browser: resource table / provider / navigation policy / editor preference.
// Part B: createEditor() choices and the native fallbacks that need no browser.
// Part C needs the WebView2 Runtime and a desktop: handshake, ping round trip, resize, blocked navigation, lifecycle,
// forced failures. A test window is shown for a few seconds. If the runtime is missing, part C is reported as SKIPPED
// (never as passed) and the test fails unless VERB_WEB_TEST_ALLOW_NO_RUNTIME=1.
#include <juce_audio_utils/juce_audio_utils.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "../PluginEditor.h"
#include "../PluginProcessor.h"
#include "../VerbWebEditor.h"
#include "../WebResources.h"

static int fails = 0, passes = 0, skipped = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (ok) ++passes; else ++fails;
}
static void skip(const std::string& what) {
    std::printf("  [SKIP] %s\n", what.c_str());
    ++skipped;
}

static double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
static bool waitFor(const std::function<bool()>& cond, double ms) {
    const double t0 = nowMs();
    while (nowMs() - t0 < ms) {
        if (cond()) return true;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }
    return cond();
}
static void pump(double ms) { waitFor([] { return false; }, ms); }

using verbweb::VerbWebEditor;
using EditorPtr = std::unique_ptr<juce::AudioProcessorEditor>;

static void setEnv(const char* name, const char* value) { SetEnvironmentVariableA(name, value); }

static std::unique_ptr<VerbProcessor> makeProcessor() {
    auto p = std::make_unique<VerbProcessor>();
    p->setPlayConfigDetails(2, 2, 48000, 256);
    p->prepareToPlay(48000, 256);
    return p;
}

static void show(juce::AudioProcessorEditor& ed, int x = 40, int y = 40) {
    ed.setTopLeftPosition(x, y);
    ed.addToDesktop(juce::ComponentPeer::windowIsTemporary);  // no taskbar entry
    ed.setVisible(true);
}

static VerbWebEditor* asWeb(EditorPtr& e) { return dynamic_cast<VerbWebEditor*>(e.get()); }

// ------------------------------------------------------------------------------------------------ part A
static void partA(const juce::File& scratch) {
    std::printf("A. resource provider, MIME, navigation policy, preference\n");
    using namespace verbweb;

    static const unsigned char kHtml[] = {'<', 'h', '1', '>'};
    static const unsigned char kJs[] = {'1', ';'};
    static const unsigned char kEmpty[] = {0};
    static const EmbeddedFile files[] = {{"index.html", kHtml, 4}, {"sub/app.js", kJs, 2}, {"a.bin", kEmpty, 0}, {"data.json", kJs, 2}};
    ResourceTable t(files, 4);

    const auto root = t.find("/");
    check(root && root->size == 4 && root->data[0] == '<' && root->mime.startsWith("text/html"), "'/' serves index.html with text/html");
    const auto js = t.find("/sub/app.js?v=3#x");
    check(js && js->size == 2 && js->mime.startsWith("text/javascript"), "query and fragment are ignored, .js is text/javascript");
    check(t.find("/data.json") && t.find("/data.json")->mime.startsWith("application/json"), ".json is application/json");
    check(t.find("/a.bin") && t.find("/a.bin")->size == 0 && t.find("/a.bin")->mime == "application/octet-stream", "empty file, unknown extension -> octet-stream");
    check(!t.find("/missing.js"), "missing file -> 404 (nullopt)");
    check(!t.find("/../index.html") && !t.find("/sub/../index.html") && !t.find("/%2e%2e/index.html") && !t.find("/sub/%2E%2E/index.html"), "'..' (plain or escaped) is refused");
    check(!t.find("/sub\\app.js") && !t.find("/C:/x") && !t.find("/sub//app.js") && !t.find("/sub/") && !t.find("/%zz") && !t.find("/a%00b"), "backslash, drive letter, empty segment, bad escapes, NUL are refused");
    check(t.find("/sub%2Fapp.js").has_value(), "an escaped slash is just a slash (still inside the table)");
    check(mimeTypeFor("x.css").startsWith("text/css") && mimeTypeFor("x.svg") == "image/svg+xml" && mimeTypeFor("x.png") == "image/png"
              && mimeTypeFor("x.woff2") == "font/woff2" && mimeTypeFor("x.PNG") == "image/png" && mimeTypeFor("noext") == "application/octet-stream",
          "MIME table (css, svg, png, woff2, case, no extension)");

    const auto provider = makeResourceProvider(t);
    const auto hit = provider("/sub/app.js");
    check(hit && hit->data.size() == 2 && (char)hit->data[0] == '1' && hit->mimeType.startsWith("text/javascript"), "provider returns the bytes and MIME of a hit");
    const auto miss = provider("/nope");
    check(miss && miss->mimeType.startsWith("text/plain") && miss->data.size() == 13 && (char)miss->data[0] == '4', "provider answers a miss with a text/plain '404 Not Found' body (never lets it reach the network)");

    std::size_t n = 0;
    const auto* emb = embeddedFileTable(n);
    check(n >= 5 && emb != nullptr, "the generated table embeds the shell directory (" + std::to_string(n) + " files)");
    ResourceTable real;
    const juce::File src("plugin/webshell");
    bool allSame = src.isDirectory();
    for (const char* rel : {"index.html", "app.js", "style.css", "host-keys.js", "juce/index.js", "juce/check_native_interop.js"}) {
        const auto f = real.find(juce::String("/") + rel);
        const auto disk = src.getChildFile(rel);
        juce::MemoryBlock mb;
        const bool read = disk.loadFileAsData(mb);
        const bool same = f && read && f->size == mb.getSize() && f->size > 0 && std::memcmp(f->data, mb.getData(), mb.getSize()) == 0;
        if (!same) std::printf("     embedded file differs from disk: %s\n", rel);
        allSame = allSame && same;
    }
    check(allSame, "embedded bytes equal the files in plugin/webshell/ (index.html, app.js, style.css, host-keys.js, juce/*.js)");
    check(real.find("/") && real.find("/")->mime.startsWith("text/html"), "the real table serves '/' as text/html");

    const auto& base = juce::WebBrowserComponent::getResourceProviderRoot();
    check(isNavigationAllowed(base) && isNavigationAllowed(base + "app.js") && isNavigationAllowed("about:blank"), "navigation to the resource origin and about:blank is allowed");
    check(!isNavigationAllowed("https://example.com/") && !isNavigationAllowed("http://localhost:8080/") && !isNavigationAllowed("file:///C:/Windows/win.ini")
              && !isNavigationAllowed("https://juce.backend.evil.example/") && !isNavigationAllowed("javascript:alert(1)") && !isNavigationAllowed(""),
          "external, localhost, file:, look-alike host and javascript: navigation is refused");

    // Host keys: payload parsing and the Win32 message that is built (no window involved).
    {
        const auto mk = [](const char* json) { juce::var v; juce::JSON::parse(json, v); return v; };
        HostKeyEvent e;
        check(parseHostKey(mk(R"({"phase":"down","vk":32,"code":"Space","key":" ","ctrl":false,"repeat":false})"), e) && e.down && e.vk == 32 && e.code == "Space" && !e.repeat,
              "host key: a valid down payload is parsed");
        check(parseHostKey(mk(R"({"phase":"up","vk":90,"ctrl":true,"repeat":true})"), e) && !e.down && e.ctrl && !e.repeat, "host key: a key-up never carries the repeat flag");
        check(!parseHostKey(mk(R"({"phase":"down","vk":0})"), e) && !parseHostKey(mk(R"({"phase":"down","vk":255})"), e) && !parseHostKey(mk(R"({"phase":"down","vk":"32"})"), e)
                  && !parseHostKey(mk(R"({"phase":"sideways","vk":32})"), e) && !parseHostKey(mk(R"({"vk":32})"), e) && !parseHostKey(juce::var(), e) && !parseHostKey(mk("[1,2]"), e)
                  && !parseHostKey(mk(R"({"phase":"down","vk":32.5})"), e),
              "host key: vk outside 1..254, text or fractional vk, unknown phase, missing phase, non-objects are refused");
        HostKeyEvent sp; sp.vk = VK_SPACE; sp.code = "Space";
        auto m = buildHostKeyMessage(sp, hostKeyIsSys(sp));
        check(m.message == WM_KEYDOWN && m.wParam == VK_SPACE && m.lParam == (1u | (0x39u << 16)), "host key: Space down = WM_KEYDOWN, VK 0x20, repeat 1, scan 0x39 (lParam " + juce::String::toHexString((juce::int64)m.lParam).toStdString() + ")");
        sp.repeat = true;
        check((buildHostKeyMessage(sp, false).lParam & (1u << 30)) != 0 && (buildHostKeyMessage(sp, false).lParam & (1u << 31)) == 0, "host key: an auto-repeat sets the previous-state bit only");
        sp.down = false; sp.repeat = false;
        m = buildHostKeyMessage(sp, false);
        check(m.message == WM_KEYUP && (m.lParam & (3u << 30)) == (3u << 30), "host key: key-up = WM_KEYUP with previous-state and transition bits");
        HostKeyEvent rt; rt.vk = VK_RIGHT; rt.code = "ArrowRight";
        check((buildHostKeyMessage(rt, false).lParam & (1u << 24)) != 0, "host key: an arrow key is flagged extended");
        HostKeyEvent af; af.vk = 'F'; af.alt = true; af.code = "KeyF";
        check(hostKeyIsSys(af) && (buildHostKeyMessage(af, true).lParam & (1u << 29)) != 0 && buildHostKeyMessage(af, true).message == WM_SYSKEYDOWN, "host key: Alt+F is WM_SYSKEYDOWN with the context bit");
        HostKeyEvent ag = af; ag.ctrl = true;
        HostKeyEvent f10; f10.vk = VK_F10;
        HostKeyEvent plainSpace; plainSpace.vk = VK_SPACE;
        check(!hostKeyIsSys(ag) && hostKeyIsSys(f10) && !hostKeyIsSys(plainSpace), "host key: AltGr (Ctrl+Alt) is a plain key, F10 is a SYS key, Space is plain");
    }

    // Preference: env VERB_UI > file > native.
    auto& s = verbweb::settings();
    s.prefFileOverride = scratch.getChildFile("pref/editor.ini");
    s.prefFileOverride.deleteFile();
    setEnv("VERB_UI", nullptr);
    check(preferredUi() == UiChoice::native, "no env, no file -> native (the default)");
    setPreferredUi(UiChoice::web);
    check(preferredUi() == UiChoice::web && s.prefFileOverride.loadFileAsString().contains("ui=web"), "stored preference ui=web -> web");
    setEnv("VERB_UI", "native");
    check(preferredUi() == UiChoice::native, "env VERB_UI=native wins over the stored web preference");
    setEnv("VERB_UI", "nonsense");
    check(preferredUi() == UiChoice::native, "an unknown VERB_UI value stays native");
    setPreferredUi(UiChoice::native);
    setEnv("VERB_UI", "web");
    check(preferredUi() == UiChoice::web, "env VERB_UI=web wins over the stored native preference");
    setEnv("VERB_UI", nullptr);
    check(preferredUi() == UiChoice::native, "back to native once the env is cleared");
}

// ------------------------------------------------------------------------------------------------ part B
static void partB(const juce::File& scratch) {
    std::printf("B. createEditor() choices that need no browser\n");
    auto& s = verbweb::settings();
    s.prefFileOverride = scratch.getChildFile("pref/editor.ini");
    s.prefFileOverride.deleteFile();
    s.udfRootOverride = scratch.getChildFile("udf-b");
    setEnv("VERB_UI", nullptr);
    auto p = makeProcessor();
    {
        EditorPtr e(p->createEditor());
        check(dynamic_cast<VerbEditor*>(e.get()) != nullptr && asWeb(e) == nullptr, "VERB_UI unset: createEditor() returns the native VerbEditor itself");
        check(e->getWidth() == 1160 && e->getHeight() == 760, "the native editor keeps its size 1160x760");
    }
    setEnv("VERB_UI", "native");
    {
        EditorPtr e(p->createEditor());
        check(dynamic_cast<VerbEditor*>(e.get()) != nullptr, "VERB_UI=native: native editor");
    }
    setEnv("VERB_UI", "web");
    s.forceRuntimeMissing = true;
    verbweb::recordDiagnostics({});
    {
        EditorPtr e(p->createEditor());
        check(dynamic_cast<VerbEditor*>(e.get()) != nullptr && asWeb(e) == nullptr, "VERB_UI=web, runtime missing: native editor, decided in createEditor()");
        check(verbweb::diagnosticsLine().contains("not available"), "the reason is kept for the diagnostics line: " + verbweb::diagnosticsLine().toStdString());
    }
    s.forceRuntimeMissing = false;
    // A user-data root that cannot be created (a file is in the way).
    const auto blocker = scratch.getChildFile("blocker.txt");
    blocker.replaceWithText("x");
    s.udfRootOverride = blocker.getChildFile("udf");
    verbweb::recordDiagnostics({});
    {
        EditorPtr e(p->createEditor());
        check(dynamic_cast<VerbEditor*>(e.get()) != nullptr && asWeb(e) == nullptr, "VERB_UI=web, user-data folder cannot be created: native editor");
        check(verbweb::diagnosticsLine().contains("user-data"), "the reason names the user-data folder: " + verbweb::diagnosticsLine().toStdString());
    }
    s.udfRootOverride = scratch.getChildFile("udf-b");
    setEnv("VERB_UI", nullptr);
}

// ------------------------------------------------------------------------------------------------ part C
static bool waitReady(VerbWebEditor& w, double ms) { return waitFor([&] { return w.isReady(); }, ms); }

static juce::String evalSync(VerbWebEditor& w, const juce::String& js) {
    juce::String out;
    bool done = false;
    w.testEvaluate(js, [&](juce::String r) { out = r; done = true; });
    waitFor([&] { return done; }, 5000);
    return done ? out : juce::String("<timeout>");
}

static void partC(const juce::File& scratch) {
    std::printf("C. WebView2 runtime: handshake, ping, resize, navigation, lifecycle, failures\n");
    auto& s = verbweb::settings();
    s.prefFileOverride = scratch.getChildFile("pref/editor.ini");
    s.prefFileOverride.deleteFile();
    s.udfRootOverride = scratch.getChildFile("udf-c");
    s.udfRootOverride.deleteRecursively();
    setEnv("VERB_UI", "web");

    const auto rt = verbweb::probeRuntime();
    if (!rt.available) {
        std::printf("     WebView2 runtime not found: %s\n", rt.error.toRawUTF8());
        if (std::getenv("VERB_WEB_TEST_ALLOW_NO_RUNTIME") != nullptr) { skip("part C skipped: no WebView2 runtime (allowed by VERB_WEB_TEST_ALLOW_NO_RUNTIME)"); return; }
        check(false, "WebView2 runtime is required for part C (set VERB_WEB_TEST_ALLOW_NO_RUNTIME=1 to skip)");
        return;
    }
    std::printf("     WebView2 runtime %s\n", rt.version.toRawUTF8());

    auto p = makeProcessor();
    double coldMs = -1, warmSum = 0;
    int warmN = 0;

    // --- first open: handshake, resources, ping, resize, navigation, 404
    {
        const double t0 = nowMs();
        EditorPtr e(p->createEditor());
        auto* w = asWeb(e);
        check(w != nullptr, "VERB_UI=web with the runtime present: createEditor() returns the web editor");
        if (w == nullptr) return;
        check(w->getWidth() == 1280 && w->getHeight() == 800, "default size 1280x800");
        show(*e);
        const bool ready = waitReady(*w, 30000);
        coldMs = nowMs() - t0;
        check(ready, "the page reports ready (cold start " + std::to_string((int)coldMs) + " ms from createEditor)");
        if (!ready) return;
        check(w->getMode() == VerbWebEditor::Mode::web, "mode is 'web' after ready");
        check(evalSync(*w, "document.documentElement.dataset.state") == "ready", "the page shows state=ready (the hello event arrived)");
        check(evalSync(*w, "document.getElementById('pluginVersion').textContent") == juce::String(VERB_WEB_PLUGIN_VERSION),
              "the page shows the plugin version from the hello event (" + std::string(VERB_WEB_PLUGIN_VERSION) + ")");
        const auto revText = evalSync(*w, "document.getElementById('revision').textContent");
        check(revText == juce::String((juce::uint64)p->getRevision()), "the page shows the processor revision (" + revText.toStdString() + ")");
        check(evalSync(*w, "document.getElementById('capabilities').textContent").contains("t1-shell"), "the page shows the capabilities stub");
        check(evalSync(*w, "location.href").startsWith(juce::WebBrowserComponent::getResourceProviderRoot()), "the page lives on the custom origin, not file:// or the network");
        const auto st = w->getStats();
        check(st.readyCount == 1 && st.resourceRequests >= 5, "the resource provider served the page, 2 scripts, juce helper and css (" + std::to_string(st.resourceRequests) + " requests)");
        check(evalSync(*w, "(function(){var x=new XMLHttpRequest();x.open('GET','/nope.txt',false);x.send();return x.responseText;})()") == "404 Not Found",
              "an unknown path is answered by the provider with the 404 body");
        check(evalSync(*w, "(function(){var x=new XMLHttpRequest();x.open('GET','/style.css',false);x.send();return x.getResponseHeader('Content-Type');})()").startsWith("text/css"),
              "style.css is served as text/css");

        const int before = w->getStats().pingDoneOk;
        w->requestPing();
        check(waitFor([&] { return w->getStats().pingDoneOk > before; }, 5000), "ping round trip C++ -> page -> native ping -> page -> C++ completed");
        check(w->getStats().pingCalls >= 1 && w->getStats().pingDoneBad == 0, "native ping calls were counted, none failed");

        const auto sz0 = w->getStats();
        w->setSize(1000, 650);
        const bool resized = waitFor([&] { const auto cur = w->getStats(); return cur.pageWidth > 0 && cur.pageWidth != sz0.pageWidth; }, 5000);
        const auto sz1 = w->getStats();
        const double ratio = sz0.pageWidth > 0 ? (double)sz1.pageWidth / sz0.pageWidth : 0.0;
        check(resized && std::abs(ratio - 1000.0 / 1280.0) < 0.06, "resize 1280->1000: the page sees the new width (" + std::to_string(sz0.pageWidth) + " -> " + std::to_string(sz1.pageWidth) + " CSS px)");

        w->testNavigate("https://example.com/");
        check(waitFor([&] { return w->getStats().blockedNavigations >= 1; }, 5000), "navigation to an external URL is refused");
        pump(300);
        check(evalSync(*w, "location.href").startsWith(juce::WebBrowserComponent::getResourceProviderRoot()), "the page is still the embedded one after the refused navigation");
        check(w->getMode() == VerbWebEditor::Mode::web, "a refused navigation does not trigger the fallback");
        check(w->getUserDataFolder().isDirectory() && w->getUserDataFolder().isAChildOf(s.udfRootOverride) && w->getUserDataFolder().getFullPathName().contains(juce::String((int)GetCurrentProcessId())),
              "the user-data folder is <root>/<host pid>/<token>: " + w->getUserDataFolder().getFullPathName().toStdString());
    }
    pump(200);

    // --- 20 open/close cycles, same processor; some destroyed while the page is loading
    {
        std::printf("     20 open/close cycles (every 4th waits for ready, the others close at once or after a short wait)\n");
        const double t0 = nowMs();
        int readyOk = 0, readyTried = 0, webMade = 0;
        for (int i = 0; i < 20; ++i) {
            EditorPtr e(p->createEditor());
            auto* w = asWeb(e);
            if (w != nullptr) ++webMade;
            if (w == nullptr) continue;
            show(*e);
            if (i % 4 == 0) {
                ++readyTried;
                const double a = nowMs();
                if (waitReady(*w, 15000)) { ++readyOk; warmSum += nowMs() - a; ++warmN; }
            } else if (i % 4 == 1) {
                // destroyed immediately, the message loop never ran
            } else if (i % 4 == 2) {
                pump(60);
            } else {
                pump(250);
            }
        }
        pump(300);
        check(webMade == 20, "all 20 cycles created the web editor");
        check(readyOk == readyTried && readyTried == 5, "every cycle that waited got ready (" + std::to_string(readyOk) + "/" + std::to_string(readyTried) + ")");
        std::printf("     cycles took %d ms\n", (int)(nowMs() - t0));
        EditorPtr e(p->createEditor());
        auto* w = asWeb(e);
        show(*e);
        check(w != nullptr && waitReady(*w, 15000), "after the cycles the same processor still opens a working web editor (user-data folder reused)");
    }

    // --- destroyed while loading, never shown, shown then hidden
    {
        EditorPtr a(p->createEditor());
        a.reset();
        EditorPtr b(p->createEditor());
        show(*b);
        b.reset();
        pump(300);
        EditorPtr c(p->createEditor());
        show(*c);
        pump(100);
        c->setVisible(false);
        pump(200);
        c->setVisible(true);
        auto* w = asWeb(c);
        check(w != nullptr && waitReady(*w, 15000), "hide and show again: the page comes back and reports ready");
        c.reset();
        check(true, "never shown / destroyed before the browser exists / hidden-shown: no crash");
    }

    // --- 4 simultaneous instances (4 processors)
    {
        std::vector<std::unique_ptr<VerbProcessor>> procs;
        std::vector<EditorPtr> eds;
        const double t0 = nowMs();
        for (int i = 0; i < 4; ++i) procs.push_back(makeProcessor());
        for (int i = 0; i < 4; ++i) { eds.emplace_back(procs[i]->createEditor()); show(*eds.back(), 40 + 30 * i, 40 + 30 * i); }
        bool all = true;
        for (auto& e : eds) all = all && asWeb(e) != nullptr && waitReady(*asWeb(e), 30000);
        check(all, "4 simultaneous instances all report ready (" + std::to_string((int)(nowMs() - t0)) + " ms for all)");
        bool pingsOk = true;
        for (auto& e : eds) {
            auto* w = asWeb(e);
            if (w == nullptr) { pingsOk = false; continue; }
            const int before = w->getStats().pingDoneOk;
            w->requestPing();
            pingsOk = pingsOk && waitFor([&] { return w->getStats().pingDoneOk > before; }, 5000);
        }
        check(pingsOk, "each of the 4 instances completes its own ping round trip");
        bool distinct = true;
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j) distinct = distinct && asWeb(eds[i]) && asWeb(eds[j]) && asWeb(eds[i])->getUserDataFolder() != asWeb(eds[j])->getUserDataFolder();
        check(distinct, "each processor has its own user-data folder");
        eds.erase(eds.begin(), eds.begin() + 2);  // close two, keep two alive while their processors go away later
        pump(300);
        eds.clear();
        procs.clear();
        pump(300);
        check(true, "closing editors and destroying processors in a different order: no crash");
    }

    // --- forced failure: the page never gets ready -> native editor
    {
        s.forceNoReady = true;
        s.readyTimeoutMs = 1500;
        verbweb::recordDiagnostics({});
        EditorPtr e(p->createEditor());
        auto* w = asWeb(e);
        check(w != nullptr, "web editor created even though the page will not get ready");
        if (w != nullptr) {
            show(*e);
            const bool toNative = waitFor([&] { return w->getMode() == VerbWebEditor::Mode::native; }, 15000);
            check(toNative, "no ready within the timeout: the editor falls back to the native editor");
            check(w->getNativeEditor() != nullptr && dynamic_cast<VerbEditor*>(w->getNativeEditor()) != nullptr, "the fallback content is the real VerbEditor");
            check(verbweb::diagnosticsLine().contains("did not report ready"), "diagnostics line: " + verbweb::diagnosticsLine().toStdString());
            check(w->getNativeEditor() != nullptr && w->getNativeEditor()->getWidth() == w->getWidth() && w->getNativeEditor()->getHeight() == w->getHeight(), "the native editor fills the outer editor");
            w->setSize(1000, 700);
            check(w->getNativeEditor() != nullptr && w->getNativeEditor()->getWidth() == 1000, "and follows a resize");
        }
        s.forceNoReady = false;
        s.readyTimeoutMs = 10000;
    }

    // --- failure while open: plain message, then "use classic UI"
    {
        EditorPtr e(p->createEditor());
        auto* w = asWeb(e);
        show(*e);
        if (w != nullptr && waitReady(*w, 15000)) {
            w->testSimulateWebFailure("simulated crash");
            check(w->getMode() == VerbWebEditor::Mode::failed && w->getNativeEditor() == nullptr, "a failure while open shows the native message, not the browser");
            check(w->switchToWeb("test: retry from failure") && waitReady(*w,15000), "Retry recreates a live web view after failure");
            w->testSimulateWebFailure("simulated second crash");
            pump(200);
            w->testUseNativeNow("test: use classic UI");
            check(w->getMode() == VerbWebEditor::Mode::native && w->getNativeEditor() != nullptr, "'use classic UI' then switches to the native editor");
        } else {
            check(false, "web editor for the failure test did not get ready");
        }
    }

    // --- heartbeat watchdog: a page that stops talking is treated as dead
    {
        s.heartbeatMs = 60000;      // the page will not ping again within the test
        s.heartbeatLimitMs = 1200;
        EditorPtr e(p->createEditor());
        auto* w = asWeb(e);
        show(*e);
        if (w != nullptr && waitReady(*w, 15000)) {
            juce::Thread::sleep(1600); // native dispatch blocked beyond the 1200 ms page deadline
            pump(200);
            check(w->getMode()==VerbWebEditor::Mode::web, "native dispatch stall does not falsely kill a live web view");
            check(waitFor([&] { return w->getMode() == VerbWebEditor::Mode::failed; }, 8000), "no sign of life within the limit: the editor shows the failure message");
        } else {
            check(false, "web editor for the watchdog test did not get ready");
        }
        s.heartbeatMs = 2000;
        s.heartbeatLimitMs = 15000;
    }

    // --- stale user-data folder cleanup removes only dead folders
    {
        const auto session = s.udfRootOverride.getChildFile("99999999");
        const auto old = juce::Time::getCurrentTime() - juce::RelativeTime::minutes(30);
        const auto make = [&](const char* name, bool aged) {
            const auto dir = session.getChildFile(name);
            dir.createDirectory();
            dir.getChildFile("data.bin").replaceWithText("x");
            dir.getChildFile("verb.lock").replaceWithText("x");
            if (aged) dir.getChildFile("verb.lock").setLastModificationTime(old);
            return dir;
        };
        const auto dead = make("dead", true), fresh = make("fresh", false), held = make("held", true);
        HANDLE heldHandle = CreateFileW(held.getChildFile("verb.lock").getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        held.getChildFile("verb.lock").setLastModificationTime(old);
        EditorPtr live(p->createEditor());
        show(*live);
        const auto liveDir = asWeb(live) != nullptr ? asWeb(live)->getUserDataFolder() : juce::File();
        liveDir.getChildFile("verb.lock").setLastModificationTime(old);  // even an old-looking lock file does not make a live folder stale
        const int removed = verbweb::cleanStaleUserDataFolders();
        check(!dead.exists(), "the cleaner removes a dead folder (no live lock, older than 5 minutes)");
        check(fresh.isDirectory(), "the cleaner keeps a recently used folder");
        check(held.isDirectory(), "the cleaner keeps a folder whose lock is held by a live editor (another process)");
        check(liveDir.isDirectory(), "the cleaner keeps the folder of an editor alive in this process");
        check(removed == 1, "exactly one folder was removed (" + std::to_string(removed) + ")");
        if (heldHandle != INVALID_HANDLE_VALUE) CloseHandle(heldHandle);
    }

    std::printf("     measured: cold open to ready %d ms; warm opens to ready average %d ms over %d\n", (int)coldMs, warmN ? (int)(warmSum / warmN) : -1, warmN);
}

// ------------------------------------------------------------------------------------------------ part D
// A top-level stub window plays the host window the plugin is attached to: the editor is a child of it, so a key the page
// forwards must arrive here, exactly where HWNDComponentPeer::forwardMessageToParent sends the keys of a native editor.
struct StubHost {
    struct Msg { UINT message; WPARAM w; LPARAM l; };
    HWND hwnd = nullptr;
    std::vector<Msg> msgs;

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) {
            if (auto* self = reinterpret_cast<StubHost*>(GetWindowLongPtrW(h, GWLP_USERDATA))) self->msgs.push_back({m, w, l});
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
    StubHost() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &StubHost::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VerbWebShellStubHost";
        RegisterClassW(&wc);  // fails harmlessly when already registered
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"stub host", WS_OVERLAPPEDWINDOW, 20, 20, 1340, 900, nullptr, nullptr, wc.hInstance, nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    ~StubHost() { if (hwnd != nullptr) DestroyWindow(hwnd); }
    void dump(const char* what) const {
        std::printf("     [%s] host window got %d message(s):", what, (int)msgs.size());
        for (const auto& x : msgs) std::printf(" %04x/%llx/%llx", (unsigned)x.message, (unsigned long long)x.w, (unsigned long long)x.l);
        std::printf("\n");
    }
    int count(UINT m, bool onlyNonRepeat = false) const {
        int n = 0;
        for (const auto& x : msgs) if (x.message == m && !(onlyNonRepeat && (x.l & (1u << 30)))) ++n;
        return n;
    }
};

static const char* kKeyHelper = R"JS((window.__k=function(t,o,id){var el=id?document.getElementById(id):document.body;o.bubbles=true;o.cancelable=true;var ev=new KeyboardEvent(t,o);el.dispatchEvent(ev);return ev.defaultPrevented?'prevented':'passed';},
document.addEventListener('keydown',function(e){if(e.code==='KeyU')e.preventDefault();}),'ok'))JS";

// Dispatches a synthetic KeyboardEvent in the page and returns 'prevented' / 'passed'.
static juce::String keyEvent(VerbWebEditor& w, const char* type, const char* init, const char* targetId = nullptr) {
    return evalSync(w, juce::String("window.__k('") + type + "'," + init + "," + (targetId != nullptr ? juce::String("'") + targetId + "'" : juce::String("null")) + ")");
}

static void partD(const juce::File& scratch) {
    std::printf("D. keys the page does not use reach the host window (stub parent HWND)\n");
    auto& s = verbweb::settings();
    s.prefFileOverride = scratch.getChildFile("pref/editor.ini");
    s.udfRootOverride = scratch.getChildFile("udf-d");
    s.udfRootOverride.deleteRecursively();
    setEnv("VERB_UI", "web");
    setEnv("VERB_HOST_KEYS", nullptr);
    if (!verbweb::probeRuntime().available) {
        if (std::getenv("VERB_WEB_TEST_ALLOW_NO_RUNTIME") != nullptr) skip("part D skipped: no WebView2 runtime");
        else check(false, "WebView2 runtime is required for part D");
        return;
    }

    StubHost host;
    auto p = makeProcessor();
    EditorPtr e(p->createEditor());
    auto* w = asWeb(e);
    check(w != nullptr, "web editor created for the key tests");
    if (w == nullptr) return;
    e->setTopLeftPosition(0, 0);
    e->addToDesktop(0, host.hwnd);  // a child of the stub host window, like an editor inside a plugin host
    e->setVisible(true);
    const bool ready = waitReady(*w, 30000);
    check(ready && w->getMode() == VerbWebEditor::Mode::web, "the page is ready inside the stub host window");
    if (!ready) return;
    check(GetParent(static_cast<HWND>(e->getPeer()->getNativeHandle())) == host.hwnd, "the editor's window is a child of the stub host window");
    check(evalSync(*w, "document.getElementById('capabilities').textContent").contains("\"hostKeys\":true"), "hello capabilities announce hostKeys:true");
    check(evalSync(*w, kKeyHelper) == "ok", "test helper installed in the page");

    const auto waitMsgs = [&](size_t n) { return waitFor([&] { return host.msgs.size() >= n; }, 4000); };
    const auto quiet = [&](double ms = 200) { pump(ms); };
    const auto received = [&] { return w->getStats().hostKeysReceived; };

    // 1. Space: down, up; exactly one message each.
    check(keyEvent(*w, "keydown", "{code:'Space',key:' ',keyCode:32}") == "prevented", "Space on the page background: the forwarder takes it (default action prevented, no page scroll)");
    check(waitMsgs(1), "Space down arrives at the host window");
    quiet();
    check(host.msgs.size() == 1 && host.msgs[0].message == WM_KEYDOWN && host.msgs[0].w == VK_SPACE && (host.msgs[0].l & 0xFFFF) == 1 && ((host.msgs[0].l >> 16) & 0xFF) == 0x39
              && (host.msgs[0].l & (3u << 30)) == 0,
          "exactly one WM_KEYDOWN VK_SPACE (repeat 1, scan 0x39, no previous-state bit): no double delivery");
    keyEvent(*w, "keyup", "{code:'Space',key:' ',keyCode:32}");
    check(waitMsgs(2), "Space up arrives at the host window");
    quiet();
    check(host.msgs.size() == 2 && host.msgs[1].message == WM_KEYUP && host.msgs[1].w == VK_SPACE && (host.msgs[1].l & (3u << 30)) == (3u << 30), "exactly one WM_KEYUP with the previous-state and transition bits");

    // 2. Ctrl+Z: the modifier and the letter are separate key events, the forwarder log carries the modifier state.
    host.msgs.clear();
    keyEvent(*w, "keydown", "{code:'ControlLeft',key:'Control',keyCode:17,ctrlKey:true}");
    keyEvent(*w, "keydown", "{code:'KeyZ',key:'z',keyCode:90,ctrlKey:true}");
    check(waitMsgs(2), "Ctrl then Z arrive");
    const auto& log = w->getHostKeyLog();
    check(!log.empty() && log.back().vk == 'Z' && log.back().ctrl && !log.back().shift && !log.back().alt && log.back().code == "KeyZ" && log.back().posted,
          "the forwarder saw Z with Ctrl (code KeyZ, vk 0x5A, no Shift/Alt)");
    check(host.msgs.size() == 2 && host.msgs[0].w == VK_CONTROL && host.msgs[1].w == 'Z' && host.msgs[1].message == WM_KEYDOWN, "the host gets WM_KEYDOWN VK_CONTROL, then WM_KEYDOWN 'Z'");
    keyEvent(*w, "keyup", "{code:'KeyZ',key:'z',keyCode:90,ctrlKey:true}");
    keyEvent(*w, "keyup", "{code:'ControlLeft',key:'Control',keyCode:17}");
    check(waitMsgs(4), "Z and Ctrl are released at the host");

    // 3. Alt+F is a SYS key.
    host.msgs.clear();
    keyEvent(*w, "keydown", "{code:'AltLeft',key:'Alt',keyCode:18,altKey:true}");
    keyEvent(*w, "keydown", "{code:'KeyF',key:'f',keyCode:70,altKey:true}");
    keyEvent(*w, "keyup", "{code:'KeyF',key:'f',keyCode:70,altKey:true}");
    keyEvent(*w, "keyup", "{code:'AltLeft',key:'Alt',keyCode:18}");  // Alt itself is released: altKey is already false
    check(waitMsgs(4), "Alt+F sequence arrives");
    quiet();
    check(host.msgs.size() == 4 && host.msgs[0].message == WM_SYSKEYDOWN && host.msgs[1].message == WM_SYSKEYDOWN && (host.msgs[1].l & (1u << 29)) != 0
              && host.msgs[2].message == WM_SYSKEYUP && host.msgs[3].message == WM_SYSKEYUP && host.msgs[3].w == VK_MENU,
          "Alt+F: WM_SYSKEYDOWN with the context bit, and the release of Alt is a WM_SYSKEYUP as well");

    // 4. Key repeat: the first press is plain, repeats carry the previous-state bit, the release is one message.
    host.msgs.clear();
    keyEvent(*w, "keydown", "{code:'ArrowRight',key:'ArrowRight',keyCode:39}");
    keyEvent(*w, "keydown", "{code:'ArrowRight',key:'ArrowRight',keyCode:39,repeat:true}");
    keyEvent(*w, "keydown", "{code:'ArrowRight',key:'ArrowRight',keyCode:39,repeat:true}");
    keyEvent(*w, "keyup", "{code:'ArrowRight',key:'ArrowRight',keyCode:39}");
    check(waitMsgs(4), "an arrow key held down: press, two repeats, release");
    quiet();
    if (host.msgs.size() != 4) host.dump("arrow");
    check(host.msgs.size() == 4 && (host.msgs[0].l & (1u << 30)) == 0 && (host.msgs[1].l & (1u << 30)) != 0 && (host.msgs[2].l & (1u << 30)) != 0 && host.msgs[3].message == WM_KEYUP
              && (host.msgs[0].l & (1u << 24)) != 0,
          "repeats are marked as repeats, the arrow is flagged extended, one release");
    // A repeat whose first press was never forwarded is dropped (the press happened in a text field).
    const int recBefore = received();
    keyEvent(*w, "keydown", "{code:'KeyQ',key:'q',keyCode:81,repeat:true}");
    quiet();
    check(received() == recBefore && host.msgs.size() == 4, "a repeat without a forwarded press stays in the page");

    // 5. Text field: typing stays in the page, nothing is forwarded or prevented.
    host.msgs.clear();
    evalSync(*w, "(document.getElementById('demoText').focus(),document.activeElement.id)");
    const int rec0 = received();
    check(keyEvent(*w, "keydown", "{code:'Space',key:' ',keyCode:32}", "demoText") == "passed", "Space typed in the text field is not prevented (it types a space)");
    keyEvent(*w, "keyup", "{code:'Space',key:' ',keyCode:32}", "demoText");
    keyEvent(*w, "keydown", "{code:'KeyA',key:'a',keyCode:65}", "demoText");
    keyEvent(*w, "keydown", "{code:'ArrowLeft',key:'ArrowLeft',keyCode:37}", "demoText");
    keyEvent(*w, "keydown", "{code:'KeyZ',key:'z',keyCode:90,ctrlKey:true}", "demoText");
    quiet(300);
    check(received() == rec0 && host.msgs.empty(), "keys typed into the text field are not forwarded to the host (Space, a letter, an arrow, Ctrl+Z)");
    // The press began in the text field: its release on the page background must not be forwarded either.
    keyEvent(*w, "keyup", "{code:'KeyA',key:'a',keyCode:65}");
    quiet();
    check(received() == rec0 && host.msgs.empty(), "a key-up whose key-down was not forwarded is not forwarded");
    // Focus back on the background (blur): Space is forwarded again.
    evalSync(*w, "(document.getElementById('demoText').blur(),document.activeElement.tagName)");
    keyEvent(*w, "keydown", "{code:'Space',key:' ',keyCode:32}");
    keyEvent(*w, "keyup", "{code:'Space',key:' ',keyCode:32}");
    check(waitMsgs(2), "after leaving the text field Space reaches the host again");

    // 6. Keys the page used, Tab, Meta, IME, buttons.
    host.msgs.clear();
    const int rec1 = received();
    check(keyEvent(*w, "keydown", "{code:'KeyU',key:'u',keyCode:85}") == "prevented", "(the page's own handler prevents U)");
    keyEvent(*w, "keydown", "{code:'ControlLeft',key:'Control',keyCode:17,ctrlKey:true}");  // a modifier alone is forwarded
    check(keyEvent(*w, "keydown", "{code:'KeyY',key:'y',keyCode:89,ctrlKey:true}") == "passed", "Ctrl+Y: the page's handler marks it handled (no preventDefault needed)");
    keyEvent(*w, "keyup", "{code:'KeyY',key:'y',keyCode:89,ctrlKey:true}");
    keyEvent(*w, "keyup", "{code:'ControlLeft',key:'Control',keyCode:17}");
    check(keyEvent(*w, "keydown", "{code:'Tab',key:'Tab',keyCode:9}") == "passed", "Tab is left to the page (focus navigation keeps working)");
    keyEvent(*w, "keyup", "{code:'Tab',key:'Tab',keyCode:9}");
    keyEvent(*w, "keydown", "{code:'MetaLeft',key:'Meta',keyCode:91,metaKey:true}");
    keyEvent(*w, "keydown", "{code:'KeyP',key:'p',keyCode:80,isComposing:true}");
    keyEvent(*w, "keydown", "{code:'KeyP',key:'Process',keyCode:229}");
    check(waitMsgs(2), "only the Ctrl press and release were forwarded");
    quiet(300);
    if (!(received() == rec1 + 2 && host.msgs.size() == 2)) { std::printf("     received %d (expected %d)\n", received(), rec1 + 2); host.dump("step 6"); }
    check(received() == rec1 + 2 && host.msgs.size() == 2 && host.msgs[0].w == VK_CONTROL && host.msgs[1].w == VK_CONTROL && host.msgs[1].message == WM_KEYUP,
          "a key the page handled (preventDefault or markHandled), Tab, the Meta key and IME input produced no host message");
    host.msgs.clear();
    keyEvent(*w, "keydown", "{code:'Space',key:' ',keyCode:32}", "pingBtn");
    quiet();
    check(host.msgs.empty(), "Space on a focused button activates the button and is not forwarded");
    // A mouse click (detail>0) lets the button give up the focus; keyboard activation (detail 0) keeps it.
    check(evalSync(*w, "(function(){var b=document.getElementById('pingBtn');b.focus();b.click();return document.activeElement.tagName;})()") == "BUTTON", "a script/keyboard click (detail 0) keeps the focus on the button");
    check(evalSync(*w, "(function(){var b=document.getElementById('pingBtn');b.focus();b.dispatchEvent(new MouseEvent('click',{bubbles:true,cancelable:true,detail:1}));return document.activeElement.tagName;})()") == "BODY",
          "after a mouse click the button gives up the focus, so that Space reaches the host next");
    keyEvent(*w, "keydown", "{code:'Space',key:' ',keyCode:32}");
    check(waitMsgs(1), "Space after the click goes to the host");
    keyEvent(*w, "keyup", "{code:'Space',key:' ',keyCode:32}");
    check(waitMsgs(2), "and is released");

    // 7. Window blur releases held keys.
    host.msgs.clear();
    keyEvent(*w, "keydown", "{code:'KeyS',key:'s',keyCode:83,ctrlKey:true}");
    check(waitMsgs(1), "S pressed");
    evalSync(*w, "(window.dispatchEvent(new Event('blur')),'ok')");
    const bool blurUp = waitMsgs(2) && host.msgs[1].message == WM_KEYUP && host.msgs[1].w == 'S';
    if (!blurUp) host.dump("blur");
    check(blurUp, "a window blur releases the held key at the host");
    // an element's blur is not a window blur
    host.msgs.clear();
    keyEvent(*w, "keydown", "{code:'KeyD',key:'d',keyCode:68}");
    check(waitMsgs(1), "D pressed");
    evalSync(*w, "(document.getElementById('demoText').focus(),document.getElementById('demoText').blur(),'ok')");
    quiet(300);
    check(host.msgs.size() == 1, "blurring an element does not release the held key");
    keyEvent(*w, "keyup", "{code:'KeyD',key:'d',keyCode:68}");
    check(waitMsgs(2), "D released normally");

    // 8. C++ side validation through the same path as the native function.
    host.msgs.clear();
    const auto mk = [](const char* json) { juce::var v; juce::JSON::parse(json, v); return v; };
    const auto dropped0 = w->getStats().hostKeysDropped;
    w->testInjectHostKey(mk(R"({"phase":"up","vk":88})"));
    w->testInjectHostKey(mk(R"({"phase":"down","vk":88,"repeat":true})"));
    w->testInjectHostKey(mk(R"({"phase":"down","vk":0})"));
    w->testInjectHostKey(juce::var("nonsense"));
    quiet();
    check(host.msgs.empty() && w->getStats().hostKeysDropped == dropped0 + 4, "a stray key-up, a repeat without a press and invalid payloads are dropped by the plugin");
    s.forwardHostKeys = false;
    check(!verbweb::hostKeysEnabled(), "settings().forwardHostKeys = false switches forwarding off");
    w->testInjectHostKey(mk(R"({"phase":"down","vk":88})"));
    s.forwardHostKeys = true;
    setEnv("VERB_HOST_KEYS", "off");
    check(!verbweb::hostKeysEnabled(), "VERB_HOST_KEYS=off switches forwarding off");
    w->testInjectHostKey(mk(R"({"phase":"down","vk":88})"));
    setEnv("VERB_HOST_KEYS", nullptr);
    quiet();
    check(host.msgs.empty() && verbweb::hostKeysEnabled(), "nothing was posted while switched off; forwarding is on again afterwards");

    // 9. Leaving the web view releases held keys and stops forwarding; a destroyed editor posts nothing.
    host.msgs.clear();
    keyEvent(*w, "keydown", "{code:'Space',key:' ',keyCode:32}");
    check(waitMsgs(1), "Space pressed before the editor switches away");
    w->testUseNativeNow("test: leave the web view");
    quiet();
    check(host.msgs.size() == 2 && host.msgs[1].message == WM_KEYUP && host.msgs[1].w == VK_SPACE, "leaving the web view releases the held Space at the host");
    w->testInjectHostKey(mk(R"({"phase":"down","vk":32})"));
    quiet();
    check(host.msgs.size() == 2, "no key is forwarded once the editor left the web view");
    e.reset();
    pump(300);

    EditorPtr e2(p->createEditor());
    auto* w2 = asWeb(e2);
    check(w2 != nullptr, "a second editor for the destruction test");
    if (w2 != nullptr) {
        e2->setTopLeftPosition(0, 0);
        e2->addToDesktop(0, host.hwnd);
        e2->setVisible(true);
        if (waitReady(*w2, 30000) && evalSync(*w2, kKeyHelper) == "ok") {
            host.msgs.clear();
            w2->testEvaluate("window.__k('keydown',{code:'Space',key:' ',keyCode:32},null)", [](juce::String) {});
            e2.reset();  // destroyed while the key may still be on its way from the page
            pump(300);
            const auto n1 = host.msgs.size();
            pump(600);
            check(host.msgs.size() == n1, "nothing is forwarded after the editor was destroyed (" + std::to_string(n1) + " message(s) before, same after)");
            check(host.count(WM_KEYDOWN, true) + host.count(WM_SYSKEYDOWN, true) == host.count(WM_KEYUP) + host.count(WM_SYSKEYUP), "every key press that reached the host was also released (no stuck key)");
        } else {
            check(false, "second editor did not become ready");
        }
    }
    setEnv("VERB_HOST_KEYS", nullptr);
}

// ------------------------------------------------------------------------------------------------ part E (N2)
struct ProcessCounters { DWORD handles = 0, user = 0, gdi = 0; };
static ProcessCounters counters() {
    ProcessCounters c;
    GetProcessHandleCount(GetCurrentProcess(), &c.handles);
    c.user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    c.gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    return c;
}

static void partE(const juce::File& scratch) {
    std::printf("E. N2: the way back from the classic editor to the new one\n");
    auto& s = verbweb::settings();
    s.prefFileOverride = scratch.getChildFile("pref-e/editor.ini");
    s.prefFileOverride.deleteFile();
    s.udfRootOverride = scratch.getChildFile("udf-e");
    s.udfRootOverride.deleteRecursively();
    const auto readPref = [&] { return s.prefFileOverride.existsAsFile() ? s.prefFileOverride.loadFileAsString().trim() : juce::String(); };

    // --- the button is absent when the web editor cannot be used (these need no browser)
    {
        setEnv("VERB_UI", "native");
        auto p = makeProcessor();
        s.forceRuntimeMissing = true;
        EditorPtr e(p->createEditor());
        auto* v = dynamic_cast<VerbEditor*>(e.get());
        check(v != nullptr && !v->newUiButtonShown(), "runtime probe fails -> the classic editor has no \"New UI\" button");
        e.reset();
        s.forceRuntimeMissing = false;

        const auto blocker = scratch.getChildFile("blocker.txt");
        blocker.replaceWithText("a file where a folder is needed");
        const auto savedPref = s.prefFileOverride;
        s.prefFileOverride = blocker.getChildFile("sub/editor.ini");  // the folder cannot be created
        EditorPtr e2(p->createEditor());
        auto* v2 = dynamic_cast<VerbEditor*>(e2.get());
        check(v2 != nullptr && !v2->newUiButtonShown(), "preference folder not writable -> no button");
        e2.reset();
        s.prefFileOverride = savedPref;

        const auto savedRoot = s.udfRootOverride;
        s.udfRootOverride = blocker.getChildFile("udf");
        EditorPtr e3(p->createEditor());
        auto* v3 = dynamic_cast<VerbEditor*>(e3.get());
        check(v3 != nullptr && !v3->newUiButtonShown(), "user-data folder not writable -> no button");
        e3.reset();
        s.udfRootOverride = savedRoot;
        blocker.deleteFile();
    }

    if (!verbweb::probeRuntime().available) {
        if (std::getenv("VERB_WEB_TEST_ALLOW_NO_RUNTIME") != nullptr) skip("part E (browser part) skipped: no WebView2 runtime");
        else check(false, "WebView2 runtime is required for part E");
        return;
    }

    // --- a plain classic editor is the host's top-level editor: remember the choice and say so, never touch the window
    {
        setEnv("VERB_UI", nullptr);
        auto p = makeProcessor();
        EditorPtr e(p->createEditor());
        auto* v = dynamic_cast<VerbEditor*>(e.get());
        check(v != nullptr, "no preference -> createEditor() is the classic editor (the default is unchanged)");
        if (v != nullptr) {
            show(*e);
            pump(150);
            check(v->newUiButtonShown(), "classic editor with the runtime present shows \"New UI\"");
            check(v->newUiNoteText().isEmpty(), "no note before the click");
            const auto snapshot = [&](const juce::String& name) {
                const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile("web-ui/shots/T6");
                if (!dir.isDirectory()) return;
                juce::PNGImageFormat png;
                juce::FileOutputStream out(dir.getChildFile(name));
                if (out.openedOk()) { out.setPosition(0); out.truncate(); png.writeImageToStream(v->createComponentSnapshot(v->getLocalBounds(), true, 1.0f), out); }
            };
            snapshot("N2-classic-new-ui-button.png");
            v->testClickNewUi();
            pump(150);
            check(readPref() == "ui=web", "the click writes ui=web to editor.ini");
            snapshot("N2-classic-new-ui-note.png");
            check(v->newUiNoteText().containsIgnoreCase("close and reopen the plugin window"), "and shows the one-line note: \"" + v->newUiNoteText().toStdString() + "\"");
            check(dynamic_cast<VerbEditor*>(e.get()) != nullptr, "the window itself is not replaced");
            e.reset();
            EditorPtr again(p->createEditor());
            check(asWeb(again) != nullptr, "the next createEditor() builds the web editor");
            again.reset();
            pump(400);
        }
        verbweb::setPreferredUi(verbweb::UiChoice::native);
        EditorPtr stay(p->createEditor());
        check(dynamic_cast<VerbEditor*>(stay.get()) != nullptr, "native stays the default when the preference says native");
        stay.reset();
        setEnv("VERB_UI", "web");  // the loops below start in the web editor
    }

    // --- web -> classic -> New UI -> web, five times, in the same editor / host window
    {
        auto p = makeProcessor();
        EditorPtr e(p->createEditor());
        auto* w = asWeb(e);
        check(w != nullptr, "VERB_UI=web: the web editor is created");
        if (w == nullptr) return;
        show(*e);
        check(waitReady(*w, 30000), "the page reports ready");
        ProcessCounters base, last;
        for (int i = 1; i <= 5; ++i) {
            w->testUseNativeNow("test: Classic UI (cycle " + juce::String(i) + ")");
            auto* classic = dynamic_cast<VerbEditor*>(w->getNativeEditor());
            const bool nativeOk = w->getMode() == VerbWebEditor::Mode::native && classic != nullptr;
            pump(200);
            check(nativeOk && classic->newUiButtonShown(), "cycle " + std::to_string(i) + ": classic editor inside the same window shows \"New UI\"");
            if (!nativeOk) break;
            classic->testClickNewUi();
            const bool back = waitFor([&] { return w->getMode() == VerbWebEditor::Mode::web && w->isReady(); }, 30000);
            check(back && w->getNativeEditor() == nullptr && w->getStats().readyCount == 1, "cycle " + std::to_string(i) + ": the web editor is ready again in place (mode web, classic editor gone)");
            check(evalSync(*w, "document.documentElement.dataset.state") == "ready", "cycle " + std::to_string(i) + ": the new page is alive (state=ready)");
            check(readPref() == "ui=web", "cycle " + std::to_string(i) + ": preference is ui=web");
            if (!back) break;
            pump(600);
            last = counters();
            if (i == 1) base = last;
        }
        std::printf("     counters after cycle 1: handles %lu, user %lu, gdi %lu; after cycle 5: handles %lu, user %lu, gdi %lu\n",
                    (unsigned long)base.handles, (unsigned long)base.user, (unsigned long)base.gdi, (unsigned long)last.handles, (unsigned long)last.user, (unsigned long)last.gdi);
        check((long)last.handles - (long)base.handles <= 40, "no handle leak over cycles 2-5 (growth " + std::to_string((long)last.handles - (long)base.handles) + ")");
        check((long)last.user - (long)base.user <= 8 && (long)last.gdi - (long)base.gdi <= 8, "no USER/GDI object leak over cycles 2-5");
        check(!w->switchToWeb("test: already web"), "switchToWeb() while the web page is showing is refused");
        // the runtime vanishes between the click and the swap: stay in the classic editor
        w->testUseNativeNow("test: classic again");
        s.forceRuntimeMissing = true;
        check(!w->switchToWeb("test: runtime gone") && w->getMode() == VerbWebEditor::Mode::native && w->getNativeEditor() != nullptr, "runtime lost before the swap -> stays classic, nothing breaks");
        s.forceRuntimeMissing = false;
        e.reset();
        pump(500);
    }
    setEnv("VERB_UI", nullptr);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    const double t0 = nowMs();
    const auto scratch = juce::File::getCurrentWorkingDirectory().getChildFile("out/T1/test-scratch");
    scratch.deleteRecursively();
    scratch.createDirectory();

    partA(scratch);
    partB(scratch);
    partC(scratch);
    partD(scratch);
    partE(scratch);

    setEnv("VERB_UI", nullptr);
    pump(800);  // let the browser processes of the last editors exit so that their folders can be removed
    scratch.deleteRecursively();
    std::printf("web_shell: %d passed, %d failed, %d skipped, %d ms\n", passes, fails, skipped, (int)(nowMs() - t0));
    return fails == 0 ? 0 : 1;
}
