#pragma once
#include "WebParameterBindings.h"
#include "EditorSession.h"
#include "energy.hpp"
#include <mutex>
#include <thread>
namespace verbweb {
// All public entrypoints run on the message thread. Workers own only immutable input copies.
class EditorBridge {
public:
    using Emit = std::function<void(const juce::String&, const juce::var&)>;
    explicit EditorBridge(VerbProcessor&, Emit = {});
    ~EditorBridge();
    juce::var handle(const juce::var&);
    juce::var parameterBatch(const juce::var& v) { return parameters.apply(v); }
    juce::var snapshot();
    void tick(bool visible, double nowMs);
    void suspend();
    bool resource(const juce::String&, std::vector<std::byte>&) const;
    juce::var reflectionPaths(const juce::var& request);
    juce::var clutterObjects();
    void setPreferenceLocation(const juce::File&);
    void setUiSwitch(std::function<void(bool)> fn) { uiSwitch = std::move(fn); }
    void windowSize(int w, int h);
    std::pair<int, int> preferredSize() const;
private:
    juce::var dispatch(const juce::var&);
    juce::var snapshotAttempt(int retries);
    uint64_t tailEventRevision = 0;
    std::deque<std::pair<juce::String, juce::var>> serviceReplies;
    uint64_t modelSeen = 0;
    double lastAnalysis = -1e20, lastMeterSent = -1e20;
    std::shared_ptr<const verb::TailDiagnostics> lastDiagnostics;
    std::map<juce::String, juce::String> lastEvents;
    juce::var envelope(const char* kind, juce::var payload, const juce::String& requestId = {}) const;
    juce::var ack(const juce::String&, const juce::String& job = {}) const;
    juce::var error(const juce::String&, const char*, const juce::String&, bool resync = false) const;
    juce::var document() const;
    juce::var host() const;
    juce::var history();
    juce::var catalogs(const juce::String& requestId);
    void fileCommand(const juce::var&, const juce::String&);
    bool loadFile(const juce::File&);
    void startAnalysis(const juce::var&);
    void pollAnalysis();
    void emitEvent(juce::var);
    void publishSnapshot();
    juce::String analysisSignature(int source) const;
    VerbProcessor& proc;
    WebParameterBindings parameters;
    Emit emit;
    juce::String sessionId = "editor-native";
    uint64_t sentRevision = 0, sentLoad = 0, sentScene = 0, meterSeq = 0;
    bool ready = false, visible = false, tailCurrent = false, documentAvailable = false;
    double lastSlow = -1e20, lastMeters = -1e20, lastPaths = -1e20, lastClutter = -1e20;
    double rebuildPendingSince = -1, lastAudioActivity = -1;
    uint64_t audioBlocksSeen = 0, cpuBlocksSeen = 0;
    double lastCpuBlockAt = -1e20, lastCpuSample = -1e20;
    bool lastCpuActive = false;
    juce::var cachedCpu;
    bool clutterSceneCurrentSeen=false;
    bool clutterDirty = true;
    juce::String lastClutterSignature;
    int lastState = -1;
    std::vector<juce::String> topics;
    juce::var preferences = obj();
    juce::File preferenceLocation;
    std::function<void(bool)> uiSwitch;
    std::unique_ptr<juce::FileChooser> chooser;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    std::vector<juce::File> userRooms;
    std::thread analysisWorker;
    std::atomic<bool> analysisCancel{false}, analysisRunning{false};
    std::atomic<double> analysisProgress{0};
    std::mutex analysisMutex;
    std::shared_ptr<const verb::EnergyMap> analysisPending;
    juce::String analysisError, analysisId, analysisRevision, analysisInputSignature, resultSignature;
    uint64_t analysisCounter = 0;
    int analysisSource = -1;
    bool staleSent = false;
    juce::var analysisDescriptor;
    std::vector<std::byte> analysisBinary;
    std::array<std::vector<std::byte>, 7> analysisBands;
    std::map<juce::String, std::vector<std::byte>> resourceCache;
    std::deque<juce::String> resourceIds;
    juce::var pathsRequest;
    bool pathsDirty = true;
};
juce::var sceneJson(const verb::Scene&, const double* home = nullptr);
juce::var materialJson(const verb::Material&);
}
