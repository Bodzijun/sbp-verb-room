#include "VerbWebEditor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>
#include <map>
#include <mutex>
#include <string>

#include "PluginEditor.h"

#ifndef VERB_WEB_PLUGIN_VERSION
#define VERB_WEB_PLUGIN_VERSION "dev"
#endif

namespace verbweb {
bool probeWebView2RuntimeVersion(std::wstring& version, long& hresult);  // WebRuntimeProbe.cpp

// ---------------------------------------------------------------- settings, preference, diagnostics
Settings& settings() {
    static Settings s;
    return s;
}

namespace {
juce::CriticalSection& diagLock() { static juce::CriticalSection c; return c; }
juce::String& diagText() { static juce::String s; return s; }

juce::File sbpRoot() {
    // %LOCALAPPDATA% (not roaming): a browser profile must never follow the user to another machine.
    const auto local = juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", {});
    const juce::File base = local.isNotEmpty() ? juce::File(local) : juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
    return base.getChildFile("SBP").getChildFile("VerbRoom");
}
}  // namespace

juce::File preferenceFile() {
    const auto& o = settings().prefFileOverride;
    return o != juce::File() ? o : sbpRoot().getChildFile("editor.ini");
}

UiChoice preferredUi() {
    const auto env = juce::SystemStats::getEnvironmentVariable("VERB_UI", {}).trim().toLowerCase();
    if (env == "web") return UiChoice::web;
    if (env == "native") return UiChoice::native;
    if (env.isNotEmpty()) return UiChoice::native;  // unknown value: stay on the default
    const auto f = preferenceFile();
    if (f.existsAsFile()) {
        juce::StringArray lines;
        lines.addLines(f.loadFileAsString());
        for (auto line : lines) {
            line = line.trim();
            if (line.startsWithIgnoreCase("ui=")) return line.fromFirstOccurrenceOf("=", false, false).trim().equalsIgnoreCase("web") ? UiChoice::web : UiChoice::native;
        }
    }
    return UiChoice::native;
}

void setPreferredUi(UiChoice choice) {
    const auto f = preferenceFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText(juce::String("ui=") + (choice == UiChoice::web ? "web" : "native") + "\n");
}

juce::String diagnosticsLine() {
    const juce::ScopedLock l(diagLock());
    return diagText();
}

void recordDiagnostics(const juce::String& reason) {
    const juce::ScopedLock l(diagLock());
    diagText() = reason;
}

bool webEditorAvailable(juce::String& why) {
    const auto rt = probeRuntime();
    if (!rt.available) { why = "WebView2 Runtime is not available (" + rt.error + ")"; return false; }
    const auto writable = [](const juce::File& dir) {
        if (!dir.createDirectory().wasOk() || !dir.isDirectory()) return false;
        const auto probe = dir.getChildFile(".verb-write-probe-" + juce::String(juce::Time::getMillisecondCounter()));
        const bool ok = probe.replaceWithText("x");
        probe.deleteFile();
        return ok;
    };
    if (!writable(userDataRoot())) { why = "the user-data folder is not writable: " + userDataRoot().getFullPathName(); return false; }
    if (!writable(preferenceFile().getParentDirectory())) { why = "the preference folder is not writable: " + preferenceFile().getParentDirectory().getFullPathName(); return false; }
    return true;
}

RuntimeInfo probeRuntime() {
    RuntimeInfo r;
    if (settings().forceRuntimeMissing) {
        r.error = "forced by test (forceRuntimeMissing)";
        return r;
    }
    std::wstring version;
    long hr = 0;
    if (probeWebView2RuntimeVersion(version, hr)) {
        r.available = true;
        r.version = juce::String(version.c_str());
    } else {
        r.error = "GetAvailableCoreWebView2BrowserVersionString failed, HRESULT 0x" + juce::String::toHexString((int)hr);
    }
    return r;
}

// ---------------------------------------------------------------- keys for the host
bool hostKeysEnabled() {
    const auto env = juce::SystemStats::getEnvironmentVariable("VERB_HOST_KEYS", {}).trim().toLowerCase();
    if (env == "off" || env == "0" || env == "false" || env == "no") return false;
    return settings().forwardHostKeys;
}

bool parseHostKey(const juce::var& p, HostKeyEvent& e) {
    if (!p.isObject()) return false;
    const auto phase = p.getProperty("phase", {}).toString();
    if (phase != "down" && phase != "up") return false;
    const auto vkVar = p.getProperty("vk", {});
    if (!(vkVar.isInt() || vkVar.isInt64() || vkVar.isDouble())) return false;
    const double vk = (double)vkVar;
    if (!(vk >= 1.0 && vk <= 254.0) || vk != std::floor(vk)) return false;
    e = {};
    e.down = phase == "down";
    e.vk = (int)vk;
    e.ctrl = (bool)p.getProperty("ctrl", false);
    e.shift = (bool)p.getProperty("shift", false);
    e.alt = (bool)p.getProperty("alt", false);
    e.meta = (bool)p.getProperty("meta", false);
    e.repeat = e.down && (bool)p.getProperty("repeat", false);
    e.code = p.getProperty("code", {}).toString().substring(0, 32);
    e.key = p.getProperty("key", {}).toString().substring(0, 32);
    return true;
}

bool hostKeyIsSys(const HostKeyEvent& e) {
    // Windows sends the SYS variants while Alt is held (not with AltGr = Ctrl+Alt) and for F10.
    return (e.alt && !e.ctrl) || e.vk == VK_F10 || e.vk == VK_MENU;
}

HostKeyMessage buildHostKeyMessage(const HostKeyEvent& e, bool sys) {
    HostKeyMessage m;
    m.message = e.down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    m.wParam = (std::uintptr_t)e.vk;
    const UINT scanEx = MapVirtualKeyW((UINT)e.vk, MAPVK_VK_TO_VSC_EX);
    // Extended keys: the grey cursor/editing block, the right-hand Ctrl/Alt, numpad Enter and Divide (MapVirtualKey does not tell).
    const bool greyBlock = (e.vk >= VK_PRIOR && e.vk <= VK_DOWN) || e.vk == VK_INSERT || e.vk == VK_DELETE || e.vk == VK_SNAPSHOT || e.vk == VK_LWIN
                           || e.vk == VK_RWIN || e.vk == VK_APPS || e.vk == VK_NUMLOCK;
    const bool extended = (greyBlock && !e.code.startsWith("Numpad")) || (scanEx & 0xFF00) == 0xE000 || (scanEx & 0xFF00) == 0xE100
                          || e.code == "NumpadEnter" || e.code == "NumpadDivide" || e.code == "ControlRight" || e.code == "AltRight";
    std::uint32_t l = 1u;                                   // repeat count
    l |= (scanEx & 0xFFu) << 16;                            // scan code
    if (extended) l |= 1u << 24;
    if (sys && e.alt) l |= 1u << 29;                        // context code: Alt is down
    if (!e.down) l |= (1u << 30) | (1u << 31);              // previous state down, transition to up
    else if (e.repeat) l |= 1u << 30;                       // auto-repeat: the key was already down
    m.lParam = l;
    return m;
}

// ---------------------------------------------------------------- user-data folder (UDF)
juce::File userDataRoot() {
    const auto& o = settings().udfRootOverride;
    return o != juce::File() ? o : sbpRoot().getChildFile("WebView");
}

namespace {
std::mutex gUdfMutex;
std::map<const void*, juce::File>& udfPaths() { static std::map<const void*, juce::File> m; return m; }
}  // namespace

juce::File userDataFolderFor(const void* processorKey) {
    std::lock_guard<std::mutex> g(gUdfMutex);
    auto& paths = udfPaths();
    const auto it = paths.find(processorKey);
    // A changed root (tests) invalidates the remembered path.
    if (it != paths.end() && it->second.isAChildOf(userDataRoot())) return it->second;
    const auto session = juce::String((int)GetCurrentProcessId());
    const auto token = juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()).paddedLeft('0', 16).substring(0, 12);
    const auto dir = userDataRoot().getChildFile(session).getChildFile(token);
    paths[processorKey] = dir;
    return dir;
}

// A lease keeps `<udf>/verb.lock` open without sharing for as long as an editor uses the folder, so the stale-folder
// cleaner of any host process can tell live folders from dead ones.
struct UdfLease {
    juce::File dir;
    juce::File lockFile;
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~UdfLease() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        lockFile.setLastModificationTime(juce::Time::getCurrentTime());  // the age of a dead folder counts from here
    }
};

namespace {
std::map<const void*, std::weak_ptr<void>>& leaseMap() { static std::map<const void*, std::weak_ptr<void>> m; return m; }

bool isLeasedHere(const juce::File& dir) {
    std::lock_guard<std::mutex> g(gUdfMutex);
    for (auto& [key, weak] : leaseMap()) {
        const auto it = udfPaths().find(key);
        if (it != udfPaths().end() && it->second == dir && !weak.expired()) return true;
    }
    return false;
}
}  // namespace

int cleanStaleUserDataFolders(const std::function<bool()>& shouldStop, int minAgeMinutes) {
    int removed = 0;
    const auto root = userDataRoot();
    if (!root.isDirectory()) return 0;
    const auto now = juce::Time::getCurrentTime();
    const auto stale = [&](const juce::File& dir) {
        const auto lock = dir.getChildFile("verb.lock");
        juce::Time touched = dir.getLastModificationTime();
        if (lock.existsAsFile()) {
            HANDLE h = CreateFileW(lock.getFullPathName().toWideCharPointer(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return false;  // held by a live editor (here or in another host)
            CloseHandle(h);
            touched = lock.getLastModificationTime();
        }
        return (now - touched).inMinutes() >= minAgeMinutes;
    };
    for (const auto& session : root.findChildFiles(juce::File::findDirectories, false)) {
        for (const auto& dir : session.findChildFiles(juce::File::findDirectories, false)) {
            if (shouldStop && shouldStop()) return removed;
            if (!isLeasedHere(dir) && stale(dir) && dir.deleteRecursively()) ++removed;
        }
        if (session.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0) session.deleteFile();
    }
    return removed;
}

// Background removal of dead user-data folders; the stop flag is checked between folders so that closing an editor never waits long.
class StaleUdfCleaner : public juce::Thread {
public:
    StaleUdfCleaner() : juce::Thread("VerbWebUdfCleaner") {}
    void run() override { cleanStaleUserDataFolders([this] { return threadShouldExit(); }); }
};

// ---------------------------------------------------------------- the browser component
class VerbWebEditor::ShellBrowser : public juce::WebBrowserComponent {
public:
    ShellBrowser(const Options& o, VerbWebEditor& e) : juce::WebBrowserComponent(o), owner(&e) {}

    bool pageAboutToLoad(const juce::String& url) override {
        if (isNavigationAllowed(url)) return true;
        if (owner != nullptr) ++owner->blockedNav;
        return false;
    }
    bool pageLoadHadNetworkError(const juce::String& error) override {
        // Our own page could not be loaded: leave the browser out of it. Not done inside the callback.
        juce::Component::SafePointer<VerbWebEditor> self(owner);
        juce::MessageManager::callAsync([self, error] {
            if (self != nullptr && self->mode == Mode::loading) self->switchToNative("the page could not be loaded: " + error);
            else if (self != nullptr && self->mode == Mode::web) self->showFailure("the page failed to load: " + error);
        });
        return false;
    }
    void newWindowAttemptingToLoad(const juce::String&) override { if (owner != nullptr) ++owner->blockedNav; }
    void windowCloseRequest() override {}

private:
    juce::Component::SafePointer<VerbWebEditor> owner;
};

namespace {
std::shared_ptr<UdfLease> acquireLease(const void* key, juce::String& reason) {
    const auto dir = userDataFolderFor(key);
    std::lock_guard<std::mutex> g(gUdfMutex);
    auto& weak = leaseMap()[key];
    if (auto existing = std::static_pointer_cast<UdfLease>(weak.lock())) return existing;
    if (!dir.createDirectory().wasOk() || !dir.isDirectory()) {
        reason = "the user-data folder cannot be created: " + dir.getFullPathName();
        return nullptr;
    }
    auto lease = std::make_shared<UdfLease>();
    lease->dir = dir;
    lease->lockFile = dir.getChildFile("verb.lock");
    lease->handle = CreateFileW(lease->lockFile.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lease->handle == INVALID_HANDLE_VALUE) {
        reason = "the user-data folder is not writable: " + dir.getFullPathName();
        return nullptr;
    }
    weak = lease;
    return lease;
}
}  // namespace

// ---------------------------------------------------------------- the editor
std::unique_ptr<VerbWebEditor> VerbWebEditor::create(VerbProcessor& proc, juce::String& reason) {
    const auto rt = probeRuntime();
    if (!rt.available) {
        reason = "WebView2 Runtime is not available (" + rt.error + ")";
        return nullptr;
    }
    auto lease = acquireLease(&proc, reason);
    if (lease == nullptr) return nullptr;
    const auto dir = lease->dir;
    return std::unique_ptr<VerbWebEditor>(new VerbWebEditor(proc, std::move(lease), dir));
}

VerbWebEditor::VerbWebEditor(VerbProcessor& p, std::shared_ptr<UdfLease> l, juce::File f)
    : juce::AudioProcessorEditor(&p), proc(p), lease(std::move(l)), udf(std::move(f)) {
    setOpaque(true);
    setResizeLimits(900, 600, 2600, 1800);
    setResizable(true, true);
    setSize(1280, 800);

    failureLabel.setJustificationType(juce::Justification::centred);
    failureLabel.setColour(juce::Label::textColourId, juce::Colour(0xffdfeee7));
    addChildComponent(failureLabel);
    addChildComponent(classicButton);
    addChildComponent(retryButton);
    retryButton.onClick=[this]{switchToWeb("user retried the web UI after a failure");};
    classicButton.onClick = [this] { switchToNative("user chose the classic UI after a failure"); };

    createBrowser();
    startTimer(34);

    static std::atomic<juce::uint32> lastCleanup{0};
    const auto now = juce::Time::getMillisecondCounter();
    const auto last = lastCleanup.load();
    if (last == 0 || now - last > 60000) {
        lastCleanup = now;
        cleaner = std::make_unique<StaleUdfCleaner>();
        cleaner->startThread(juce::Thread::Priority::low);
    }
}

VerbWebEditor::~VerbWebEditor() {
    stopTimer();
    releaseHostKeys();
    mode = Mode::failed;  // late callbacks see a closed editor
    if (bridge) { bridge->windowSize(getWidth(), getHeight()); bridge->suspend(); }
    bridge.reset();
    browser.reset();
    nativeEditor.reset();
    if (cleaner != nullptr) cleaner->stopThread(3000);
}

void VerbWebEditor::createBrowser() {
    using Opt = juce::WebBrowserComponent::Options;
    using Done = juce::WebBrowserComponent::NativeFunctionCompletion;
    juce::Component::SafePointer<VerbWebEditor> self(this);

    bridge = std::make_unique<EditorBridge>(proc, [self](const juce::String& channel, const juce::var& payload) {
        if (self != nullptr && self->browser != nullptr && self->mode == Mode::web) self->browser->emitEventIfBrowserIsVisible(channel, finiteJson(payload.clone()));
    });
    bridge->setPreferenceLocation(preferenceFile().getSiblingFile("web-preferences.json"));
    bridge->setUiSwitch([self](bool native) {
        setPreferredUi(native ? UiChoice::native : UiChoice::web);
        if (native) juce::MessageManager::callAsync([self] { if (self) self->switchToNative("the page selected native UI"); });
    });
    const auto size = bridge->preferredSize(); setSize(size.first, size.second);
    const auto baseProvider = makeResourceProvider(table);
    auto provider = [self, baseProvider](const juce::String& url) {
        if (self != nullptr) {
            ++self->resourceHits;
            juce::WebBrowserComponent::Resource dynamic;
            if (self->bridge && self->bridge->resource(url, dynamic.data)) { dynamic.mimeType = "application/octet-stream"; return std::optional<juce::WebBrowserComponent::Resource>(std::move(dynamic)); }
        }
        return baseProvider(url);
    };

    auto options = Opt{}
        .withBackend(Opt::Backend::webview2)
        .withWinWebView2Options(Opt::WinWebView2{}
                                    .withUserDataFolder(udf)
                                    .withStatusBarDisabled()
                                    .withBuiltInErrorPageDisabled()
                                    .withBackgroundColour(juce::Colour(0xff101513)))
        .withNativeIntegrationEnabled()
        .withResourceProvider(provider)
        .withNativeFunction("verbMessage", [self](const juce::Array<juce::var>& a, Done done) {
            done(self && self->bridge && !a.isEmpty() ? self->bridge->handle(a[0]) : juce::var());
        })
        .withNativeFunction("verbParameters", [self](const juce::Array<juce::var>& a, Done done) {
            done(self && self->bridge && !a.isEmpty() ? self->bridge->parameterBatch(a[0]) : juce::var());
        })
        .withNativeFunction("ready", [self](const juce::Array<juce::var>& a, Done done) {
            if (self == nullptr) { done(juce::var()); return; }
            self->onNativeReady(a.isEmpty() ? juce::var() : a[0], std::move(done));
        })
        .withNativeFunction("ping", [self](const juce::Array<juce::var>& a, Done done) {
            if (self == nullptr) { done(juce::var()); return; }
            self->onNativePing(a.isEmpty() ? juce::var() : a[0], std::move(done));
        })
        .withNativeFunction("useClassicUi", [self](const juce::Array<juce::var>&, Done done) {
            done(juce::var(true));
            // Explicit user action in the page: remember it, and swap editors outside of the browser callback.
            juce::MessageManager::callAsync([self] {
                if (self == nullptr) return;
                setPreferredUi(UiChoice::native);
                self->switchToNative("the page asked for the classic UI");
            });
        })
        .withNativeFunction("hostKey", [self](const juce::Array<juce::var>& a, Done done) {
            // Message thread. `self` is a SafePointer: a call that outlives the editor finds it null and does nothing.
            const bool posted = self != nullptr && self->handleHostKey(a.isEmpty() ? juce::var() : a[0]);
            done(juce::var(posted));
        })
        .withEventListener("verb.pingDone", [self](juce::var v) {
            if (self == nullptr) return;
            if (static_cast<bool>(v.getProperty("ok", false))) ++self->pingDoneOk; else ++self->pingDoneBad;
        })
        .withEventListener("verb.pageSize", [self](juce::var v) {
            if (self == nullptr) return;
            self->pageW = (int)v.getProperty("w", 0);
            self->pageH = (int)v.getProperty("h", 0);
        });

    browser = std::make_unique<ShellBrowser>(options, *this);
    addAndMakeVisible(*browser);
    browser->setBounds(getLocalBounds());
    browser->goToURL(juce::WebBrowserComponent::getResourceProviderRoot() + "?bridge=juce&dev=0");
}

void VerbWebEditor::onNativeReady(const juce::var& args, juce::WebBrowserComponent::NativeFunctionCompletion done) {
    juce::ignoreUnused(args);
    if (mode == Mode::failed || mode == Mode::native || browser == nullptr) { done(juce::var()); return; }
    if (settings().forceNoReady) { done(juce::var()); return; }  // test: a page that never gets ready
    ++readyCount;
    if (mode == Mode::loading) mode = Mode::web;
    lastBeat = juce::Time::getMillisecondCounter();

    auto ack = std::make_unique<juce::DynamicObject>();
    ack->setProperty("ok", true);
    done(juce::var(ack.release()));

    auto caps = std::make_unique<juce::DynamicObject>();
    caps->setProperty("bridge", "t1-shell");
    caps->setProperty("commands", false);
    caps->setProperty("parameters", false);
    caps->setProperty("hostKeys", hostKeysEnabled());
    auto hello = std::make_unique<juce::DynamicObject>();
    hello->setProperty("schemaVersion", 1);
    hello->setProperty("pluginVersion", juce::String(VERB_WEB_PLUGIN_VERSION));
    hello->setProperty("revision", juce::String((juce::uint64)proc.getRevision()));  // decimal string, as in the contract
    hello->setProperty("capabilities", juce::var(caps.release()));
    hello->setProperty("heartbeatMs", settings().heartbeatMs);
    browser->emitEventIfBrowserIsVisible("verb.hello", juce::var(hello.release()));
}

void VerbWebEditor::onNativePing(const juce::var& args, juce::WebBrowserComponent::NativeFunctionCompletion done) {
    if (mode == Mode::failed || mode == Mode::native) { done(juce::var()); return; }
    const double pingAt=juce::Time::getMillisecondCounterHiRes();
    if(lastPingAt>0)maxHeartbeatGapMs=std::max(maxHeartbeatGapMs,pingAt-lastPingAt);
    lastPingAt=pingAt;
    ++pingCalls;
    lastBeat = juce::Time::getMillisecondCounter();
    auto reply = std::make_unique<juce::DynamicObject>();
    reply->setProperty("seq", args.getProperty("seq", 0));
    reply->setProperty("revision", juce::String((juce::uint64)proc.getRevision()));
    done(juce::var(reply.release()));
}

void* VerbWebEditor::hostWindow() const {
    if (auto* peer = getPeer())
        if (auto* own = static_cast<HWND>(peer->getNativeHandle()))
            if (HWND parent = GetParent(own); parent != nullptr && IsWindow(parent)) return parent;
    return nullptr;
}

void VerbWebEditor::releaseHostKeys() {
    // Keys that were posted as pressed and not yet released: let go of them so that the host never keeps a stuck key.
    HWND parent = static_cast<HWND>(hostWindow());
    for (const auto& [vk, sys] : hostKeysDown) {
        if (parent == nullptr) break;
        HostKeyEvent e;
        e.down = false;
        e.vk = vk;
        const auto msg = buildHostKeyMessage(e, sys);
        PostMessageW(parent, msg.message, (WPARAM)msg.wParam, (LPARAM)msg.lParam);
    }
    hostKeysDown.clear();
}

bool VerbWebEditor::handleHostKey(const juce::var& payload) {
    JUCE_ASSERT_MESSAGE_THREAD
    ++hostKeysReceived;
    HostKeyEvent e;
    const auto drop = [this, &e](bool log) {
        ++hostKeysDropped;
        if (log && e.vk != 0) {
            hostKeyLog.push_back(e);
            if (hostKeyLog.size() > 64) hostKeyLog.erase(hostKeyLog.begin());
        }
        return false;
    };
    if (!parseHostKey(payload, e)) return drop(false);
    // Only the live web page forwards keys: not a closed, failed or replaced editor, and not when switched off.
    if (mode != Mode::web || browser == nullptr || !hostKeysEnabled()) return drop(true);

    bool sys = false;
    if (e.down) {
        const auto it = hostKeysDown.find(e.vk);
        if (e.repeat && it == hostKeysDown.end()) return drop(true);  // repeat of a key whose first press was never posted
        sys = (e.repeat && it != hostKeysDown.end()) ? it->second : hostKeyIsSys(e);
    } else {
        const auto it = hostKeysDown.find(e.vk);
        if (it == hostKeysDown.end()) return drop(true);              // a release whose press was never posted
        sys = it->second;
    }

    // The same route JUCE uses for a key nobody handled: PostMessage to the parent of the editor's window, i.e. the
    // window the host gave to IPlugView::attached (HWNDComponentPeer::forwardMessageToParent).
    HWND parent = static_cast<HWND>(hostWindow());
    if (parent == nullptr) return drop(true);

    const auto msg = buildHostKeyMessage(e, sys);
    if (!PostMessageW(parent, msg.message, (WPARAM)msg.wParam, (LPARAM)msg.lParam)) return drop(true);
    if (e.down) hostKeysDown[e.vk] = sys; else hostKeysDown.erase(e.vk);
    e.posted = true;
    e.sys = sys;
    ++hostKeysPosted;
    hostKeyLog.push_back(e);
    if (hostKeyLog.size() > 64) hostKeyLog.erase(hostKeyLog.begin());
    return true;
}

void VerbWebEditor::timerCallback() {
    const double tickAt=juce::Time::getMillisecondCounterHiRes();
    const double nativeGap=lastTimerAt>0?tickAt-lastTimerAt:0.;
    if(lastTimerAt>0)maxTimerGapMs=std::max(maxTimerGapMs,nativeGap);
    lastTimerAt=tickAt;
    if (bridge) bridge->tick(mode == Mode::web && isShowing(), juce::Time::getMillisecondCounterHiRes());
    const double bridgeMs=juce::Time::getMillisecondCounterHiRes()-tickAt;
    maxBridgeTickMs=std::max(maxBridgeTickMs,bridgeMs);
    const auto now=juce::Time::getMillisecondCounter();
    // A heartbeat cannot be delivered while native dispatch is unavailable. Give
    // the page a complete response window after dispatch resumes or a room loads.
    if(nativeGap>250. || bridgeMs>100. || proc.isRoomLoadPending()){
        lastBeat=now;
        if(mode==Mode::loading)showingSince=now;
    }
    if (mode == Mode::loading) {
        if (!isShowing()) { showingSince = 0; return; }
        if (showingSince == 0) showingSince = now;
        if ((int)(now - showingSince) > settings().readyTimeoutMs)
            switchToNative("the page did not report ready within " + juce::String(settings().readyTimeoutMs) + " ms");
    } else if (mode == Mode::web) {
        if (!isShowing()) { lastBeat = 0; return; }  // a hidden web view unloads its page; nothing to watch
        if (lastBeat == 0) lastBeat = now;
        if ((int)(now - lastBeat) > settings().heartbeatLimitMs)
            showFailure("the web view stopped responding (no heartbeat for " + juce::String(settings().heartbeatLimitMs) + " ms)");
    } else {
        stopTimer();
    }
}

void VerbWebEditor::switchToNative(const juce::String& reason) {
    if (nativeEditor != nullptr) return;
    recordDiagnostics("web editor left: " + reason);
    stopTimer();
    releaseHostKeys();
    if (bridge) bridge->suspend();
    browser.reset();
    failureLabel.setVisible(false);
    classicButton.setVisible(false);
    retryButton.setVisible(false);
    nativeEditor = std::make_unique<VerbEditor>(proc);
    nativeEditor->setResizable(false, false);  // the outer editor owns resizing (and its corner)
    addAndMakeVisible(*nativeEditor);
    nativeEditor->setBounds(getLocalBounds());
    mode = Mode::native;
    failureReason = reason;
    repaint();
}

bool VerbWebEditor::switchToWeb(const juce::String& reason) {
    JUCE_ASSERT_MESSAGE_THREAD
    if (mode != Mode::native && mode != Mode::failed) return false;
    juce::String why;
    if (!webEditorAvailable(why)) { recordDiagnostics("web editor not re-entered: " + why); return false; }
    recordDiagnostics("web editor re-entered: " + reason);
    setPreferredUi(UiChoice::web);
    browser.reset(); bridge.reset();
    nativeEditor.reset();  // the classic editor (and its timers) goes first; the outer shell and the user-data lease stay
    failureLabel.setVisible(false);
    classicButton.setVisible(false);
    retryButton.setVisible(false);
    failureReason = {};
    readyCount = 0;
    showingSince = 0;
    lastBeat = 0;lastTimerAt=0;lastPingAt=0;
    hostKeysDown.clear();
    mode = Mode::loading;  // `ready` from the new page makes it Mode::web
    createBrowser();       // new bridge + browser, the saved window size
    startTimer(34);
    resized();
    repaint();
    return true;
}

void VerbWebEditor::showFailure(const juce::String& reason) {
    if (mode == Mode::native) return;
    recordDiagnostics("web editor failed: " + reason);
    stopTimer();
    releaseHostKeys();
    if (bridge) bridge->suspend();
    browser.reset();
    mode = Mode::failed;
    failureReason = reason;
    failureLabel.setText("The web editor stopped: " + reason + ".\nThe plugin keeps running and the sound is not affected.",
                         juce::dontSendNotification);
    failureLabel.setVisible(true);
    classicButton.setVisible(true);
    retryButton.setVisible(true);
    resized();
    repaint();
}

VerbWebEditor::Stats VerbWebEditor::getStats() const {
    Stats s;
    s.maxTimerGapMs=maxTimerGapMs;s.maxBridgeTickMs=maxBridgeTickMs;s.maxHeartbeatGapMs=maxHeartbeatGapMs;
    s.readyCount = readyCount.load();
    s.pingCalls = pingCalls.load();
    s.pingDoneOk = pingDoneOk.load();
    s.pingDoneBad = pingDoneBad.load();
    s.blockedNavigations = blockedNav.load();
    s.resourceRequests = resourceHits.load();
    s.pageWidth = pageW.load();
    s.pageHeight = pageH.load();
    s.hostKeysReceived = hostKeysReceived.load();
    s.hostKeysPosted = hostKeysPosted.load();
    s.hostKeysDropped = hostKeysDropped.load();
    return s;
}

void VerbWebEditor::requestPing() {
    if (browser != nullptr) browser->emitEventIfBrowserIsVisible("verb.pingRequest", juce::var());
}

void VerbWebEditor::testNavigate(const juce::String& url) {
    if (browser != nullptr) browser->goToURL(url);
}

void VerbWebEditor::testEvaluate(const juce::String& script, std::function<void(juce::String)> done) {
    if (browser == nullptr) { done("<no browser>"); return; }
    browser->evaluateJavascript(script, [done](juce::WebBrowserComponent::EvaluationResult r) {
        if (const auto* v = r.getResult()) done(v->toString());
        else done("<error>");
    });
}

void VerbWebEditor::resized() {
    const auto b = getLocalBounds();
    if (bridge) bridge->windowSize(b.getWidth(), b.getHeight());
    if (browser != nullptr) browser->setBounds(b);
    if (nativeEditor != nullptr) nativeEditor->setBounds(b);
    failureLabel.setBounds(b.withSizeKeepingCentre(juce::jmin(560, b.getWidth() - 40), 120).translated(0, -30));
    classicButton.setBounds(b.withSizeKeepingCentre(180, 32).translated(-100, 60));
    retryButton.setBounds(b.withSizeKeepingCentre(180, 32).translated(100, 60));
}

void VerbWebEditor::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff101513));
    if (mode == Mode::loading) {
        g.setColour(juce::Colour(0xff8aa597));
        g.setFont(15.f);
        g.drawText("Loading Verb Room…", getLocalBounds(), juce::Justification::centred);
    }
}

juce::AudioProcessorEditor* createEditorFor(VerbProcessor& proc) {
    if (preferredUi() == UiChoice::web) {
        juce::String reason;
        if (auto web = VerbWebEditor::create(proc, reason)) return web.release();
        recordDiagnostics("web editor not used: " + reason);
    }
    return new VerbEditor(proc);
}

}  // namespace verbweb
