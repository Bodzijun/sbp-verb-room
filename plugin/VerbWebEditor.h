#pragma once
// WebView (WebView2) editor shell of Verb Room, task T1 of docs/WebView_UI_plan_uk.md.
// The native editor (VerbEditor) stays the default; this editor is opt-in (see preferredUi()).
// The shell shows an embedded offline page, answers the `ready` handshake and a `ping` round trip, and falls back to
// the native editor when the runtime is missing, the page does not report `ready`, or the page cannot be loaded.
// Everything here runs on the message thread; nothing touches the audio thread.
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "PluginProcessor.h"
#include "WebResources.h"
#include "EditorBridge.h"

namespace verbweb {

enum class UiChoice { native, web };

// Process-wide settings (tests change them; the defaults are the production values).
struct Settings {
    int readyTimeoutMs = 10000;       // the page must report `ready` within this time after it became visible
    int heartbeatMs = 2000;           // the page pings the plugin this often (sent in the hello event)
    int heartbeatLimitMs = 15000;     // no ping for this long while visible -> the web view is considered dead
    bool forceRuntimeMissing = false;  // test: the preflight reports that WebView2 is not installed
    bool forceNoReady = false;         // test: the plugin ignores the page's `ready` (a page that never gets ready)
    bool forwardHostKeys = true;       // keys the page did not use are posted to the host window (env VERB_HOST_KEYS=off disables)
    juce::File udfRootOverride;        // test: user-data root other than %LOCALAPPDATA%/SBP/VerbRoom/WebView
    juce::File prefFileOverride;       // test: preference file other than %LOCALAPPDATA%/SBP/VerbRoom/editor.ini
};
Settings& settings();

// One keyboard event the page did not use and hands to the host (page -> native function `hostKey`).
// Why this exists: with a native JUCE editor an unhandled key travels HWNDComponentPeer -> PostMessage(GetParent(hwnd)) to the
// host window. A focused WebView2 swallows the keys in its own (browser-process) window, so nothing reaches the host unless
// the page forwards them. See docs/WebView_build_uk.md, "Клавіші для хоста".
struct HostKeyEvent {
    bool down = true;
    int vk = 0;                 // Windows virtual-key code (1..254)
    bool ctrl = false, shift = false, alt = false, meta = false;
    bool repeat = false;        // auto-repeat of a key that is already down
    juce::String code, key;     // KeyboardEvent.code / .key (diagnostics, extended-key detection)
    bool posted = false;        // set by the editor: a message was handed to the host window
    bool sys = false;           // posted as WM_SYSKEYDOWN/UP
};

// The Win32 message that carries `e` (what the OS would deliver for the physical key). No window is touched.
struct HostKeyMessage {
    unsigned message = 0;       // WM_KEYDOWN / WM_KEYUP / WM_SYSKEYDOWN / WM_SYSKEYUP
    std::uintptr_t wParam = 0;  // virtual-key code
    std::uintptr_t lParam = 0;  // repeat count, scan code, extended, context, previous state, transition (32 bit, zero-extended)
};
// `sys` selects the SYSKEY variant; use hostKeyIsSys() for a key-down and reuse its result for the matching key-up.
bool hostKeyIsSys(const HostKeyEvent& e);
HostKeyMessage buildHostKeyMessage(const HostKeyEvent& e, bool sys);
// Parses the JS payload {phase, vk, code, key, ctrl, shift, alt, meta, repeat}; false if it is not a valid key.
bool parseHostKey(const juce::var& payload, HostKeyEvent& out);
bool hostKeysEnabled();  // settings().forwardHostKeys and the VERB_HOST_KEYS environment variable

// Which editor createEditor() builds: env VERB_UI=web|native wins, then the stored preference (editor.ini, `ui=web`),
// default native. Anything else is native.
UiChoice preferredUi();
void setPreferredUi(UiChoice choice);  // persists to the preference file
juce::File preferenceFile();

// One line for diagnostics: the last reason the web editor was not used (or was left); empty if none.
juce::String diagnosticsLine();
void recordDiagnostics(const juce::String& reason);

struct RuntimeInfo {
    bool available = false;
    juce::String version;  // e.g. "157.0.4341.42"
    juce::String error;
};
RuntimeInfo probeRuntime();  // cheap: reads the installed runtime version, creates nothing

// N2: can the web editor be used right now? WebView2 Runtime present, the user-data root and the preference folder writable.
// The classic editor shows its "New UI" button only when this is true. Cheap (registry read + two tiny file probes).
bool webEditorAvailable(juce::String& why);

juce::File userDataRoot();  // %LOCALAPPDATA%/SBP/VerbRoom/WebView
// Per processor: <root>/<host pid>/<token>. Not next to the plugin binary; one folder per processor instance, reused when
// the editor is reopened.
juce::File userDataFolderFor(const void* processorKey);

// Removes user-data folders that nobody uses: no live lock (no editor of any host process holds it), not leased by a
// processor of this process, and untouched for `minAgeMinutes`. Returns the number of folders removed. Checks
// `shouldStop` between folders. Run on a background thread by the editor; synchronous here so that it can be tested.
int cleanStaleUserDataFolders(const std::function<bool()>& shouldStop = {}, int minAgeMinutes = 5);

struct UdfLease;  // keeps the user-data folder of a processor locked while an editor uses it

class VerbWebEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    enum class Mode { loading, web, native, failed };

    // Preflight (runtime, user-data folder). Returns null and sets `reason` when the web editor cannot be used;
    // the caller then builds the native editor. Never blocks for the browser (the page loads asynchronously).
    static std::unique_ptr<VerbWebEditor> create(VerbProcessor& proc, juce::String& reason);
    ~VerbWebEditor() override;

    Mode getMode() const { return mode; }
    bool isReady() const { return readyCount.load() > 0; }
    juce::AudioProcessorEditor* getNativeEditor() const { return nativeEditor.get(); }
    // N2: classic -> new UI in place, inside the same host window (the shell that hosts the classic editor swaps it for a
    // fresh web page). Message thread only; never call it from inside the classic editor's own callback (use callAsync).
    // False (and nothing changes) when this editor is not showing the classic UI or the web editor is not available.
    bool switchToWeb(const juce::String& reason);
    juce::String getFailureReason() const { return failureReason; }

    struct Stats {
        int readyCount = 0;        // `ready` calls answered
        double maxTimerGapMs = 0, maxBridgeTickMs = 0, maxHeartbeatGapMs = 0;
        int pingCalls = 0;         // native `ping` calls received
        int pingDoneOk = 0;        // round trips confirmed by the page (verb.pingDone ok)
        int pingDoneBad = 0;
        int blockedNavigations = 0;
        int resourceRequests = 0;  // requests answered by the resource provider (page, scripts, styles, 404s)
        int pageWidth = 0, pageHeight = 0;
        int hostKeysReceived = 0;  // `hostKey` calls from the page
        int hostKeysPosted = 0;    // of them posted to the host window
        int hostKeysDropped = 0;   // not posted (disabled, wrong mode, no host window, stray key-up/repeat, invalid)
    };
    Stats getStats() const;
    // Message thread only. Recent events (cap 64), the newest last; each says whether it was posted.
    const std::vector<HostKeyEvent>& getHostKeyLog() const { return hostKeyLog; }
    void testInjectHostKey(const juce::var& payload) { handleHostKey(payload); }  // the same path as the native function

    // Test / diagnostics hooks.
    void requestPing();                                  // C++ -> page: run a ping round trip
    void testNavigate(const juce::String& url);          // goToURL; used to check that external navigation is refused
    void testEvaluate(const juce::String& script, std::function<void(juce::String)> done);  // page state read-back
    void testSimulateWebFailure(const juce::String& reason) { showFailure(reason); }
    void testUseNativeNow(const juce::String& reason) { switchToNative(reason); }
    juce::File getUserDataFolder() const { return udf; }

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    class ShellBrowser;

    VerbWebEditor(VerbProcessor& proc, std::shared_ptr<UdfLease> lease, juce::File udfFolder);
    void createBrowser();
    void switchToNative(const juce::String& reason);
    void showFailure(const juce::String& reason);
    void onNativeReady(const juce::var& args, juce::WebBrowserComponent::NativeFunctionCompletion done);
    void onNativePing(const juce::var& args, juce::WebBrowserComponent::NativeFunctionCompletion done);
    bool handleHostKey(const juce::var& payload);  // message thread; true if a message was posted to the host window
    void* hostWindow() const;                      // HWND the host gave the plugin (parent of our window), or null
    void releaseHostKeys();                        // posts key-ups for keys still held, then forgets them
    void timerCallback() override;

    VerbProcessor& proc;
    std::shared_ptr<UdfLease> lease;
    juce::File udf;
    ResourceTable table;
    Mode mode = Mode::loading;
    juce::String failureReason;

    std::atomic<int> readyCount{0}, pingCalls{0}, pingDoneOk{0}, pingDoneBad{0}, blockedNav{0}, resourceHits{0};
    std::atomic<int> pageW{0}, pageH{0};
    std::atomic<int> hostKeysReceived{0}, hostKeysPosted{0}, hostKeysDropped{0};
    std::map<int, bool> hostKeysDown;    // vk -> posted as SYSKEY; pairs every key-up with the key-down that was posted
    std::vector<HostKeyEvent> hostKeyLog;
    juce::uint32 showingSince = 0;  // when the page first became visible (0 = not yet)
    double lastTimerAt = 0, lastPingAt = 0, maxTimerGapMs = 0, maxBridgeTickMs = 0, maxHeartbeatGapMs = 0;
    juce::uint32 lastBeat = 0;      // last sign of life of the page while visible (0 = hidden)

    std::unique_ptr<EditorBridge> bridge;
    std::unique_ptr<ShellBrowser> browser;
    std::unique_ptr<juce::AudioProcessorEditor> nativeEditor;  // VerbEditor after a fallback
    juce::Label failureLabel;
    juce::TextButton classicButton{"Use classic UI"}, retryButton{"Retry web UI"};
    std::unique_ptr<juce::Thread> cleaner;
};

// What VerbProcessor::createEditor() returns when the web editor is compiled in: the web editor when the preference
// asks for it and the preflight succeeds, otherwise exactly the native editor.
juce::AudioProcessorEditor* createEditorFor(VerbProcessor& proc);

}  // namespace verbweb
