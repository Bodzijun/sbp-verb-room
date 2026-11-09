#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "SourceModel.h"
#include "layouts.hpp"
#include "rt_engine.hpp"
#include "scene.hpp"

namespace verbp {
constexpr int kMaxReceivers = 16;
constexpr int kListener = kMaxSources;  // object index of the listener (sources are 0..kMaxSources-1)

// Parameter id of object `obj` (source 0..31, listener = kListener), component k: 0 x, 1 y, 2 z, 3 azimuth/yaw,
// 4 gain (sources only).
inline juce::String paramId(int obj, int k) {
    static const char* comp[5] = {"x", "y", "z", "az", "g"};
    return (obj == kListener ? juce::String("l") : "s" + juce::String(obj + 1)) + comp[k];
}

struct OutputCfg {
    // The output layout is a setting of the listener, independent of how many channels the track happens to have
    // (a track gets extra channels just to carry side inputs). Default: stereo (ORTF). -1 = match the track's channels.
    int layout = (int)verb::Layout::StereoORTF;
    double radius = 0.1;  // distance of the virtual microphones from the listener centre, metres
    int micPattern = 0;   // verb::MicPattern: 0 = the layout's own (cardioid per channel), omni ... figure-8, human ears
};

struct TailSettings {
    int rays = 48000;
    bool adaptive = true;
};
}  // namespace verbp

class VerbProcessor : public juce::AudioProcessor, private juce::Thread {
public:
    VerbProcessor();
    ~VerbProcessor() override;

    const juce::String getName() const override { return "Verb Room"; }
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    // Hosts may query and cache this before the asynchronous room has loaded.
    // Report the maximum supported tail from the outset, including engine latency.
    double getTailLengthSeconds() const override { return 20.25; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // ---- room (scene) ----
    void loadSceneText(const juce::String& text, const juce::String& name, bool applyDefaults, bool keepPendingDefaults = false);
    // Loads a room together with the sources of the plugin (one per `source` line of the text, in that order: the text gives
    // their places) and, if >= 0, the listener's microphone pattern.
    void loadRoomWithSetup(const juce::String& text, const juce::String& name, const std::vector<verbp::SourceCfg>& setup, int micPattern);
    int getLoadState() const { return state.load(); }  // 0 idle, 1 loading, 2 ready, 3 error
    juce::String getStatus() const;
    juce::String getSceneName() const;
    verb::Scene getDisplayScene(double* home /*[4]: x y z yaw of the listener anchor*/ = nullptr) const;
    // Edit the live room (message thread): `fn` changes a copy of the scene (walls, surface materials, floor,
    // ceiling, height); the result is validated, kept in the saved state and pushed to the running engine without
    // interrupting the audio. Rejected edits (too many walls, bad indices) change nothing.
    void editScene(const std::function<void(verb::Scene&)>& fn);
    uint64_t getLoadSerial() const { return loadSerial.load(); }  // increments when a new engine is built (a room is loaded, the output is changed)
    uint64_t getSceneSerial() const { return sceneSerial.load(); }  // increments on every change of the room or the setup

    // ---- sources and outputs (message thread; structural changes rebuild the engine in the background) ----
    std::vector<verbp::SourceCfg> getSources() const;
    void setSources(const std::vector<verbp::SourceCfg>& cfg);  // replaces the list (positions of new sources: defaults)
    int addSource();                                             // returns the new index, or -1 if full
    void removeSource(int index);                                // keeps at least one source
    void setSourcePattern(int index, double pattern);  // live: directivity 0 omni, 0.5 cardioid, 1 figure-8
    void setSourceSpread(int index, double radius);  // live: moves the channels of a multichannel source, no rebuild
    // Turns a source (or the listener, obj == kListener) so that its azimuth/yaw points at the world position (x, y).
    void orientObject(int obj, double x, double y);
    // Scales one wall segment about its midpoint.
    void scaleWall(int wall, double factor);
    // Scales the whole room about the centre of its walls: wall endpoints, ceiling height and (optionally) the
    // positions of all sources and the listener.
    void scaleRoom(double factor, bool includeObjects);
    // Live: moves one channel point of a free-placement source (offset in the source's frame), no rebuild.
    void setPointOffset(int source, int channel, float ox, float oy, float oz);
    // ---- room library / IR export ----
    // The room as scene text with the CURRENT source and listener positions (for the room library).
    juce::String getRoomText() const;
    // Renders the impulse response of every engine point (one multichannel WAV per point, channels = the output
    // layout) from the current scene, positions and layout, into `dir`, on a background thread. Also writes the room.
    void startExportIR(const juce::File& dir);
    int getExportState() const { return exportState.load(); }  // 0 idle, 1 running, 2 done, 3 error
    juce::String getExportMessage() const;
    std::vector<verbp::PointMap> getLivePoints() const;  // engine points of the running setup (grouped by source)
    verbp::OutputCfg getOutputCfg() const;
    void setOutputCfg(const verbp::OutputCfg&);
    verbp::TailSettings getTailSettings() const;
    void setTailSettings(verbp::TailSettings);
    std::shared_ptr<const verb::TailDiagnostics> getTailDiagnostics() const;
    int hostOutputChannels() const { return hostOutCh.load(); }
    int hostInputChannels() const { return hostInCh.load(); }
    // Peak of every input channel over the last host block (0-based channel, 0..1+): the editor shows them so the
    // numbering of the host's sends can be checked against the sources that read them.
    bool inputMetersEnabled() const { return inputMetersOn.load(); }
    void setInputMetersEnabled(bool on) { inputMetersOn = on; }  // an editor option saved with the project
    bool keepRoutingEnabled() const { return keepRouting.load(); }
    void setKeepRoutingEnabled(bool on) { keepRouting = on; }
    float inputPeak(int ch) const { return ch >= 0 && ch < verbp::kMaxInputChannels ? inPeaks[(size_t)ch].load(std::memory_order_relaxed) : 0.f; }
    int numPoints() const { return numPts.load(); }
    int numOutputChannels() const { return builtOutCh.load(); }  // channels the engine renders (the layout's)
    int effectiveLayout() const { return builtLayout.load(); }   // verb::Layout used by the running engine

    // Suggested x/y slider range {minX, maxX, minY, maxY}: the walls' bounding box plus a margin. A hint only,
    // positions are not limited by the walls. False when the scene has no walls.
    bool getBounds(double* b) const {
        if (!hasBounds.load()) return false;
        for (int i = 0; i < 4; ++i) b[i] = bounds[i].load();
        return true;
    }

    juce::AudioProcessorValueTreeState apvts;

private:
    struct Request {
        std::string text, name;
        double sr = 48000;
        std::vector<verbp::SourceCfg> setup;  // optional sources that come with the room (applied with the defaults)
        int micPattern = -1;                   // optional listener microphone pattern (-1 = keep)
        bool keepRouting = false;              // preserve source configuration and listener output settings
    };
    std::vector<verbp::SourceCfg> nextSetup;   // guarded by reqLock: picked up by the next loadSceneText
    int nextMic = -1;
    // Everything the audio thread needs, swapped as one unit under engineLock.
    struct Bundle {
        std::unique_ptr<verb::Engine> engine;
        std::vector<verbp::SourceCfg> cfg;
        std::vector<verbp::PointMap> points;
        std::vector<int> pointStart, pointCount;  // points of each source (they are stored grouped by source)
        std::vector<std::vector<float>> in, out;  // scratch: one per point / per output channel
        std::vector<float> lfe;                   // summed LFE inputs for the block
        int R = 0, lfeOut = -1, maxBlock = 512;
    };
    void run() override;
    void runExport(juce::File dir);
    std::thread exportThread;
    std::atomic<int> exportState{0};
    std::atomic<bool> exportCancel{false};
    juce::String exportMsg;  // guarded by dispLock
    void applyDefaultsFrom(const verb::Scene&, const double* home);
    void updateBounds(const verb::Scene&);
    void setParamValue(const juce::String& id, double v);
    void rebuild();

    std::unique_ptr<Bundle> bundle;
    mutable juce::SpinLock engineLock;
    juce::CriticalSection reqLock;
    Request req;
    bool hasReq = false;
    juce::WaitableEvent wake;

    mutable juce::CriticalSection dispLock;
    verb::Scene display;
    double displayHome[4] = {0, 0, 0, 0};
    juce::String status = "No scene";
    juce::String sceneText, sceneName;
    std::vector<verbp::SourceCfg> srcCfg;
    verbp::OutputCfg outCfg;
    verbp::TailSettings tailSettings;
    std::atomic<int> state{0};
    std::atomic<bool> inputMetersOn{true};
    std::atomic<bool> keepRouting{false};
    std::array<std::atomic<float>, verbp::kMaxInputChannels> inPeaks{};
    std::array<int, verbp::kMaxInputChannels> inMap{};  // host input channel -> buffer channel (see InputOrder.h)
    std::atomic<int> numPts{0}, builtOutCh{0}, builtLayout{0}, hostOutCh{2}, hostInCh{2}, builtForHostCh{0};
    std::atomic<double> sceneHeight{3.0}, builtSr{0.0};
    std::atomic<double> bounds[4] = {};
    std::atomic<bool> hasBounds{false}, ceilOpen{false};
    std::atomic<bool> geomDirty{false};
    std::atomic<bool> qualityDirty{false};
    std::atomic<uint64_t> sceneSerial{0}, loadSerial{0}, reqSerial{0};
    std::atomic<bool> defaultsOwed{false};
    uint64_t serialSeen = ~0ull;

    std::atomic<float>* pv[verbp::kMaxSources + 1][5] = {};
    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* earlyParam = nullptr;
    std::atomic<float>* diffusionParam = nullptr;
    std::atomic<float>* dopplerParam = nullptr;
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* tailParam = nullptr;
    std::atomic<float>* soloParam[3] = {};
    juce::SmoothedValue<float> lfeSoloGain{1.f};
    float prevWet = 1.f, prevDry = 0.f;  // audio thread: the dry/wet gains at the end of the last block
    std::atomic<float> srcPattern[verbp::kMaxSources] = {};  // live directivity of every source (audio thread reads)
    void syncPatterns();
    std::atomic<float> ptOff[verbp::kMaxPoints][3] = {};  // live offset of every engine point from its source centre
    std::vector<verbp::PointMap> livePoints;                // guarded by dispLock
    double prev[verbp::kMaxSources + 1][4] = {};
    // Trajectory smoothing of the automated positions (x, y, z): velocity at the end of the last block and the block's
    // average velocity (units per sample), so that consecutive host blocks join with a continuous velocity.
    double hVel[verbp::kMaxSources + 1][3] = {}, hPEnd[verbp::kMaxSources + 1][3] = {};
    double hLast[verbp::kMaxSources + 1][3] = {}, hLast2[verbp::kMaxSources + 1][3] = {};  // automation values at the last two block starts
    int hN = 0, hN2 = 0;
    uint64_t hSeen = ~0ull;  // engine serial the smoothing state belongs to
    float prevGain[verbp::kMaxSources] = {};
    double hostSr = 48000;
    int maxBlock = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VerbProcessor)
};
