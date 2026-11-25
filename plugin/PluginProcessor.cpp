#include "PluginProcessor.h"
#include "AmbisonicsOutput.h"
#include <functional>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <limits>
#include <locale>

#include "EditorSession.h"
#include "InputOrder.h"
#include "tail.hpp"
#include "PluginEditor.h"
#if JUCE_WINDOWS
#include "bake_diagnostics.hpp"
#endif
#if VERB_WEB_UI_ENABLED
#include "VerbWebEditor.h"
#endif

namespace {

bool bakeLogEnabled() {
#if JUCE_WINDOWS
    return verb::bake_diag::enabled();
#else
    static const bool enabled = juce::SystemStats::getEnvironmentVariable("VERB_BAKE_LOG", {}) == "1";
    return enabled;
#endif
}

void writeBakeLog(const void* instance, double now, const juce::String& message) {
    static juce::CriticalSection lock;
    const juce::ScopedLock guard(lock);
    auto prefix = juce::Time::getCurrentTime().toISO8601(true) + " VERB_BAKE_LOG instance="
        + juce::String::toHexString((juce::uint64)reinterpret_cast<uintptr_t>(instance))
        + " clockMs=" + juce::String(now, 1);
#if JUCE_WINDOWS
    prefix += " pid=" + juce::String((juce::uint64)GetCurrentProcessId());
#endif
    juce::String block;
    for (const auto& line : juce::StringArray::fromLines(message)) block += prefix + " " + line + "\n";
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("VerbRoom-bake.log");
    if (!file.appendText(block)) juce::Logger::outputDebugString(block);
}

const char* kDefaultScene = R"(# Built-in demo room: furnished 8 x 6 x 3 m room, one source.
# material: name, absorption 125 Hz .. 8 kHz, scatter at 125 Hz, scatter at 8 kHz (rough surfaces scatter HF more)
sr 48000
length 2.0
height 3.0
order 2

material wall   0.12 0.15 0.20 0.25 0.30 0.35 0.40  0.10 0.70
material carpet 0.05 0.10 0.25 0.45 0.65 0.70 0.70  0.20 0.60
material tiles  0.10 0.30 0.55 0.70 0.75 0.75 0.70  0.10 0.50
class wall 0
class carpet 4
class tiles 5

floor carpet
ceiling tiles
wall 0 8 0 0 wall
wall 0 0 6 0 wall
wall 6 0 6 8 wall
wall 6 8 0 8 wall

listener 3 2 1.5 90
source Voice 3 6 1.5 -90

receiver L 6 3 1.5  55 0.5
receiver R 6 3 1.5 -55 0.5
)";

std::array<std::shared_ptr<verbp::AxisRangeStore>, 3> makeRangeStores() {
    return {
        std::make_shared<verbp::AxisRangeStore>((float)verbp::AutomationRange::kGlobalMinXY, (float)verbp::AutomationRange::kGlobalMaxXY),
        std::make_shared<verbp::AxisRangeStore>((float)verbp::AutomationRange::kGlobalMinXY, (float)verbp::AutomationRange::kGlobalMaxXY),
        std::make_shared<verbp::AxisRangeStore>((float)verbp::AutomationRange::kGlobalMinZ,  (float)verbp::AutomationRange::kGlobalMaxZ)
    };
}

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout(const std::array<std::shared_ptr<verbp::AxisRangeStore>, 3>& rangeStores) {
    using namespace verbp;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    auto add = [&](const juce::String& id, const juce::String& name, float lo, float hi, float def, const char* unit) {
        p.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{id, 1}, name, juce::NormalisableRange<float>(lo, hi, 0.001f), def,
                                                                juce::AudioParameterFloatAttributes().withLabel(unit)));
    };
    auto addPos = [&](const juce::String& id, const juce::String& name, int axis, float def, const char* unit) {
        p.push_back(std::make_unique<verbp::DynamicPositionParameter>(juce::ParameterID{id, 1}, name, rangeStores[(size_t)axis], def, unit));
    };
    for (int o = 0; o <= kMaxSources; ++o) {
        const juce::String nm = o == kListener ? "Listener" : "Source " + juce::String(o + 1);
        addPos(paramId(o, 0), nm + " X", 0, 0.f, "m");
        addPos(paramId(o, 1), nm + " Y", 1, 0.f, "m");
        addPos(paramId(o, 2), nm + " Z", 2, 1.5f, "m");
        add(paramId(o, 3), nm + (o == kListener ? " Yaw" : " Azimuth"), -180.f, 180.f, 0.f, "deg");
        if (o != kListener) add(paramId(o, 4), nm + " Gain", -60.f, 12.f, 0.f, "dB");
    }
    add("gain", "Output Gain", -36.f, 12.f, 0.f, "dB");
    add("early", "Early Reflections", -60.f, 6.f, 0.f, "dB");  // 0 dB = strict physical level
    add("diffusion", "Early Diffusion", 0.f, 1.f, 0.5f, "");
    add("doppler", "Doppler Strength", 0.f, 2.f, 1.f, "");  // 1 = physical, 0 = no pitch shift
    add("tail", "Tail Level", -48.f, 12.f, 0.f, "dB");      // the late reverberant field alone (direct and early reflections unchanged)
    add("mix", "Dry/Wet Mix", 0.f, 100.f, 100.f, "%");      // 100 = only the room (the usual setup: dry stays on the source tracks)
    p.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"spatialDry", 1}, "Positioned Dry", true));
    p.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"soloDirect", 1}, "Solo Direct", false));
    p.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"soloEarly", 1}, "Solo Early", false));
    p.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"soloTail", 1}, "Solo Tail", false));
    for (int s = 0; s < kMaxSources; ++s) {
        p.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{sourceSwitchId(s, false), 1}, "Source " + juce::String(s + 1) + " Mute", false));
        p.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{sourceSwitchId(s, true), 1}, "Source " + juce::String(s + 1) + " Solo", false));
    }
    // Append new parameters so hosts that persist indices retain all existing indices.
    p.push_back(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{"mode", 1}, "Mode", juce::StringArray{"Physical", "Creative"}, 0));
    for (const char* id : {"absorption", "decayLow", "decayMid", "decayHigh"}) {
        juce::NormalisableRange<float> range(.3f, 3.f, .001f); range.setSkewForCentre(1.f);
        p.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{id, 1}, id, range, 1.f));
    }
    add("size","Size",50,160,100,"%");
    add("predelay","Predelay",0,250,0,"ms");
    add("stereoWidth","Stereo width",0,200,100,"%");
    p.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"freeze",1},"Freeze",false));
    add("duck","Duck amount",0,24,0,"dB");
    add("duckRelease","Duck release",50,2000,300,"ms");
    add("makeup", "Makeup", 0.f, 36.f, 0.f, "dB");
    for(int o=0;o<=kMaxSources;++o)
        p.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{VerbProcessor::pathParamId(o),1},
            (o==kListener?juce::String("Listener"):"Source "+juce::String(o+1))+" Path position",juce::NormalisableRange<float>(0.f,1.f),0.f));
    return {p.begin(), p.end()};
}

juce::String fmtModDouble(double val) {
    std::ostringstream number;
    number.imbue(std::locale::classic());
    number << std::setprecision(std::numeric_limits<double>::max_digits10) << val;
    return juce::String(number.str());
}

double parseModDouble(const juce::var& value) {
    if (!value.isString()) return (double)value;
    std::istringstream number(value.toString().toStdString());
    number.imbue(std::locale::classic());
    double result = std::numeric_limits<double>::quiet_NaN();
    number >> result;
    return result;
}

juce::ValueTree modRoutesToValueTree(const std::vector<verbp::ModulationRoute>& routes) {
    juce::ValueTree modTree("Modulation");
    for (int i = 0; i < (int)routes.size() && i < verbp::kMaxModRoutes; ++i) {
        const auto& r = routes[(size_t)i];
        juce::ValueTree rt("Route");
        rt.setProperty("enabled", r.enabled, nullptr);
        rt.setProperty("object", r.object, nullptr);
        rt.setProperty("driver", juce::String(verbp::modDriverName(r.driver)), nullptr);
        if (r.driver == verbp::ModDriver::DistanceToPoint) {
            rt.setProperty("pointX", fmtModDouble(r.pointX), nullptr);
            rt.setProperty("pointY", fmtModDouble(r.pointY), nullptr);
            rt.setProperty("pointZ", fmtModDouble(r.pointZ), nullptr);
        }
        rt.setProperty("targetObject", r.targetObject, nullptr);
        rt.setProperty("target", juce::String(verbp::modTargetName(r.target)), nullptr);
        if (r.hostOut > 0)
            rt.setProperty("hostOut", r.hostOut, nullptr);
        rt.setProperty("attackMs", fmtModDouble(r.attackMs), nullptr);
        rt.setProperty("releaseMs", fmtModDouble(r.releaseMs), nullptr);
        juce::ValueTree cv("Curve");
        cv.setProperty("inputMin", fmtModDouble(r.curve.inputMin), nullptr);
        cv.setProperty("inputMax", fmtModDouble(r.curve.inputMax), nullptr);
        cv.setProperty("outputMin", fmtModDouble(r.curve.outputMin), nullptr);
        cv.setProperty("outputMax", fmtModDouble(r.curve.outputMax), nullptr);
        cv.setProperty("invert", r.curve.invert, nullptr);
        cv.setProperty("clamp", r.curve.clamp, nullptr);
        for (int ni = 0; ni < r.curve.nodeCount && ni < verbp::kMaxCurveNodes; ++ni) {
            const auto& nd = r.curve.nodes[(size_t)ni];
            juce::ValueTree nt("Node");
            nt.setProperty("x", fmtModDouble(nd.x), nullptr);
            nt.setProperty("y", fmtModDouble(nd.y), nullptr);
            if (nd.smooth) nt.setProperty("smooth", true, nullptr);
            cv.appendChild(nt, nullptr);
        }
        rt.appendChild(cv, nullptr);
        modTree.appendChild(rt, nullptr);
    }
    return modTree;
}

std::vector<verbp::ModulationRoute> modRoutesFromValueTree(const juce::ValueTree& modTree) {
    std::vector<verbp::ModulationRoute> out;
    if (!modTree.isValid()) return out;
    for (int i = 0; i < modTree.getNumChildren() && (int)out.size() < verbp::kMaxModRoutes; ++i) {
        const auto rt = modTree.getChild(i);
        if (!rt.hasType("Route")) continue;
        verbp::ModulationRoute r;
        r.enabled = (bool)rt.getProperty("enabled", true);
        r.object = (int)rt.getProperty("object", -1);
        verbp::ModDriver drv{};
        if (!verbp::parseModDriver(rt.getProperty("driver", "").toString().toStdString(), drv)) continue;
        r.driver = drv;
        if (rt.hasProperty("pointX")) r.pointX = parseModDouble(rt.getProperty("pointX"));
        if (rt.hasProperty("pointY")) r.pointY = parseModDouble(rt.getProperty("pointY"));
        if (rt.hasProperty("pointZ")) r.pointZ = parseModDouble(rt.getProperty("pointZ"));
        r.targetObject = (int)rt.getProperty("targetObject", -1);
        verbp::ModTarget tgt{};
        if (!verbp::parseModTarget(rt.getProperty("target", "").toString().toStdString(), tgt)) continue;
        r.target = tgt;
        if (rt.hasProperty("hostOut")) r.hostOut = (int)rt.getProperty("hostOut", 0);
        if (rt.hasProperty("attackMs")) r.attackMs = parseModDouble(rt.getProperty("attackMs"));
        if (rt.hasProperty("releaseMs")) r.releaseMs = parseModDouble(rt.getProperty("releaseMs"));
        const auto cv = rt.getChildWithName("Curve");
        if (cv.isValid()) {
            if (cv.hasProperty("inputMin")) r.curve.inputMin = parseModDouble(cv.getProperty("inputMin"));
            if (cv.hasProperty("inputMax")) r.curve.inputMax = parseModDouble(cv.getProperty("inputMax"));
            if (cv.hasProperty("outputMin")) r.curve.outputMin = parseModDouble(cv.getProperty("outputMin"));
            if (cv.hasProperty("outputMax")) r.curve.outputMax = parseModDouble(cv.getProperty("outputMax"));
            r.curve.invert = (bool)cv.getProperty("invert", false);
            r.curve.clamp = (bool)cv.getProperty("clamp", true);
            int nc = 0;
            for (int ni = 0; ni < cv.getNumChildren() && nc < verbp::kMaxCurveNodes; ++ni) {
                const auto nt = cv.getChild(ni);
                if (!nt.hasType("Node")) continue;
                r.curve.nodes[(size_t)nc].x = parseModDouble(nt.getProperty("x"));
                r.curve.nodes[(size_t)nc].y = parseModDouble(nt.getProperty("y"));
                r.curve.nodes[(size_t)nc].smooth = (bool)nt.getProperty("smooth", false);
                ++nc;
            }
            if (nc > 0) r.curve.nodeCount = nc;
        }
        if (verbp::validModRoute(r)) out.push_back(r);
    }
    return out;
}

}  // namespace

VerbProcessor::VerbProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Sources", juce::AudioChannelSet::discreteChannels(2), true)
                         .withOutput("Room", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("Verb scene loader"),
      axisStores(makeRangeStores()),
      apvts(*this, nullptr, "VerbState", makeLayout(axisStores)),
      modMailbox(std::make_unique<verbp::ModRoutesMailbox>()),
      wallMailbox(std::make_unique<verbp::WallSnapshotMailbox>()),
      modProc(std::make_unique<verbp::ModulationProcessor>()) {
    for (int o = 0; o <= verbp::kMaxSources; ++o)
        for (int k = 0; k < (o == verbp::kListener ? 4 : 5); ++k) pv[o][k] = apvts.getRawParameterValue(verbp::paramId(o, k));
    for(int o=0;o<=verbp::kMaxSources;++o) pathParams[o]=apvts.getRawParameterValue(pathParamId(o));
    sizeParam=apvts.getRawParameterValue("size");predelayParam=apvts.getRawParameterValue("predelay");
    widthParam=apvts.getRawParameterValue("stereoWidth");freezeParam=apvts.getRawParameterValue("freeze");
    duckParam=apvts.getRawParameterValue("duck");duckReleaseParam=apvts.getRawParameterValue("duckRelease");
    gainParam = apvts.getRawParameterValue("gain");
    makeupParam = apvts.getRawParameterValue("makeup");
    modeParam = apvts.getRawParameterValue("mode");
    absorptionParam = apvts.getRawParameterValue("absorption");
    const char* decayIds[] = {"decayLow","decayMid","decayHigh"};
    for (int g=0;g<3;++g) decayParam[g] = apvts.getRawParameterValue(decayIds[g]);
    earlyParam = apvts.getRawParameterValue("early");
    diffusionParam = apvts.getRawParameterValue("diffusion");
    dopplerParam = apvts.getRawParameterValue("doppler");
    mixParam = apvts.getRawParameterValue("mix");
    spatialDryParam = apvts.getRawParameterValue("spatialDry");
    tailParam = apvts.getRawParameterValue("tail");
    soloParam[0] = apvts.getRawParameterValue("soloDirect");
    soloParam[1] = apvts.getRawParameterValue("soloEarly");
    soloParam[2] = apvts.getRawParameterValue("soloTail");
    for (int s = 0; s < verbp::kMaxSources; ++s) {
        sourceMute[s] = apvts.getRawParameterValue(verbp::sourceSwitchId(s, false));
        sourceSolo[s] = apvts.getRawParameterValue(verbp::sourceSwitchId(s, true));
    }
    // Append 16 read-only host modulation outputs AFTER apvts so indices 0..282 stay untouched
    // and Mo parameters never enter APVTS copyState/presets or Web UI parameter lists.
    for (int s = 0; s < verbp::kMaxModHostOuts; ++s) {
        auto p = std::make_unique<verbp::ModOutParameter>(s + 1);
        modOutParams[(size_t)s] = p.get();
        addParameter(p.release());
    }
    session = std::make_unique<verbp::EditorSession>(*this);
    sceneText = kDefaultScene;
    sceneName = "Built-in demo room";
    startThread();
    loadSceneText(sceneText, sceneName, true); // geometry retains the Physical parameter default
}

VerbProcessor::~VerbProcessor() {
    targetCancel = true; if (targetThread.joinable()) targetThread.join();
    exportCancel = true;
    if (exportThread.joinable()) exportThread.join();
    signalThreadShouldExit();
    wake.signal();
    stopThread(30000);
}

bool VerbProcessor::isBusesLayoutSupported(const BusesLayout& l) const {
    const int nOut = l.getMainOutputChannels(), nIn = l.getMainInputChannels();
    return nOut >= 1 && nOut <= verbp::kMaxInputChannels && nIn <= verbp::kMaxInputChannels;
}

void VerbProcessor::prepareToPlay(double sr, int block) {
    hostSr = sr;
    maxBlock = std::max(block, 128);
    webLastBlockSize.store(std::max(1, block), std::memory_order_relaxed);
    webCpuMean.store(0.f, std::memory_order_relaxed);
    webCpuWorst.store(0.f, std::memory_order_relaxed);
    webCpuOverrunHold.store(0.f, std::memory_order_relaxed);
    hostOutCh = std::max(1, getTotalNumOutputChannels());
    hostInCh = getTotalNumInputChannels();
    {   // channel n of the host's track is read from the buffer channel JUCE put it in
        for (int i = 0; i < verbp::kMaxInputChannels; ++i) inMap[(size_t)i] = i;
        if (const auto* bus = getBus(true, 0)) {
            const auto m = verbp::hostToBufferIndices(bus->getCurrentLayout());
            for (size_t i = 0; i < m.size() && i < inMap.size(); ++i) inMap[i] = m[i];
        }
    }
    serialSeen = ~0ull;
    haveLastProcessWall = false;
    havePlayHead = false;
    modProc->reset();
    modFallbackClock = 0;
    lfeSoloGain.reset(sr, 0.01);
    bool needRebuild;
    {
        juce::SpinLock::ScopedLockType l(engineLock);
        needRebuild = !bundle || std::fabs(builtSr.load() - sr) > 0.5 || bundle->maxBlock < maxBlock;
    }
    if (builtForHostCh.load() != hostOutCh.load()) needRebuild = true;
    if (needRebuild) rebuild();
}

void VerbProcessor::captureOutputMeters(const juce::AudioBuffer<float>& buffer) {
    if(!outputMeteringEnabled.load(std::memory_order_relaxed)||buffer.getNumSamples()<=0)return;
    const int channels=std::min({getTotalNumOutputChannels(),buffer.getNumChannels(),verbp::kMaxReceivers});
    const double sr=std::max(8000.,hostSr);const float alpha=(float)(1.-std::exp(-buffer.getNumSamples()/(sr*.15)));
    for(int ch=0;ch<verbp::kMaxReceivers;++ch){float peak=0;double square=0;
        if(ch<channels){const auto* input=buffer.getReadPointer(ch);for(int i=0;i<buffer.getNumSamples();++i){const float value=input[i];peak=std::max(peak,std::abs(value));square+=(double)value*value;}square/=buffer.getNumSamples();}
        outMeanSquare[(size_t)ch]+=alpha*((float)square-outMeanSquare[(size_t)ch]);outRms[(size_t)ch].store(std::sqrt(std::max(0.f,outMeanSquare[(size_t)ch])),std::memory_order_relaxed);
        auto& target=outPeaks[(size_t)ch];float previous=target.load(std::memory_order_relaxed);while(previous<peak&&!target.compare_exchange_weak(previous,peak,std::memory_order_relaxed)){}
        if(peak>=1.f)outClips[(size_t)ch].store(true,std::memory_order_relaxed);
    }
}
void VerbProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) {
    const auto blockStart = std::chrono::steady_clock::now();
    juce::ScopedNoDenormals noDenormals;
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "Web processing observation must be lock-free");
    static_assert(std::atomic<int>::is_always_lock_free, "Web block size observation must be lock-free");
    webProcessedBlocks.fetch_add(1, std::memory_order_relaxed);
    const int n = buf.getNumSamples(), nIn = getTotalNumInputChannels(), nOut = getTotalNumOutputChannels();
    static_assert(std::atomic<float>::is_always_lock_free, "Web peaks must remain lock-free");
    struct BlockLoadGuard {
        VerbProcessor& self;
        std::chrono::steady_clock::time_point start;
        int samples;
        ~BlockLoadGuard() noexcept {
            if (samples <= 0) return;
            const double sr = self.builtSr.load(std::memory_order_relaxed) > 0.0 ? self.builtSr.load(std::memory_order_relaxed) : self.hostSr;
            if (!(sr > 0.0)) return;
            const double blockSec = (double)samples / sr;
            const double elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const float overrideRatio = self.webCpuTestOverride.load(std::memory_order_relaxed);
            const float ratio = overrideRatio >= 0.f ? overrideRatio : (float)std::max(0.0, elapsedSec / blockSec);
            const float dt = (float)blockSec;
            const float alpha = 1.f - std::exp(-dt / 0.6f);
            const float prevMean = self.webCpuMean.load(std::memory_order_relaxed);
            self.webCpuMean.store(prevMean + alpha * (ratio - prevMean), std::memory_order_relaxed);
            const float decay = std::exp(-dt / 2.5f);
            const float prevWorst = self.webCpuWorst.load(std::memory_order_relaxed);
            self.webCpuWorst.store(std::max(ratio, prevWorst * decay), std::memory_order_relaxed);
            float hold = self.webCpuOverrunHold.load(std::memory_order_relaxed);
            if (ratio >= 1.0f) {
                self.webCpuOverruns.fetch_add(1, std::memory_order_relaxed);
                hold = 3.5f;
            } else {
                hold = std::max(0.f, hold - dt);
            }
            self.webCpuOverrunHold.store(hold, std::memory_order_relaxed);
            self.webLastBlockSize.store(samples, std::memory_order_relaxed);
        }
    } blockLoadGuard{*this, blockStart, n};
    struct OutputMeterGuard {VerbProcessor& owner;const juce::AudioBuffer<float>& buffer;~OutputMeterGuard(){owner.captureOutputMeters(buffer);}} outputMeterGuard{*this,buf};

    const bool accumulateWeb = webMetersActive.load(std::memory_order_relaxed);
    for (int ch = 0; ch < verbp::kMaxInputChannels; ++ch) {
        const float peak = ch < nIn && inMap[(size_t)ch] < buf.getNumChannels() ? buf.getMagnitude(inMap[(size_t)ch], 0, n) : 0.f;
        inPeaks[(size_t)ch].store(peak, std::memory_order_relaxed);
        if (accumulateWeb) {
            auto& target = webInPeaks[(size_t)ch]; auto previous = target.load(std::memory_order_relaxed);
            while (previous < peak && !target.compare_exchange_weak(previous, peak, std::memory_order_relaxed)) {}
        }
    }
    juce::SpinLock::ScopedTryLockType lk(engineLock);
    if (!lk.isLocked() || !bundle) {
        buf.clear();
        return;
    }
    Bundle& b = *bundle;
    verb::Engine& e = *b.engine;
    const int NS = (int)b.cfg.size(), P = (int)b.points.size(), R = b.R;
    const int format = b.ambisonic ? ambisonicsFormat.load(std::memory_order_relaxed) : 0;
    const verbp::AmbisonicsOutput ambisonics(format);
    const bool reorder = ambisonics.reorders();
    const int ambisonicChannels = b.ambisonic ? std::min(R, nOut) : 0;
    const double H = sceneHeight.load();
    const double zTop = ceilOpen.load() ? 30.0 : H;  // x/y are free; z stays between floor and ceiling

    double cur[verbp::kMaxSources + 1][4];
    float curGain[verbp::kMaxSources];
    float sourceGate[verbp::kMaxSources];
    bool anySourceSolo = false;
    for (int i = 0; i < NS; ++i) anySourceSolo |= sourceSolo[i]->load(std::memory_order_relaxed) >= 0.5f;
    for (int i = 0; i < NS; ++i) {
        for (int k = 0; k < 4; ++k) cur[i][k] = pv[i][k]->load(std::memory_order_relaxed);
        cur[i][2] = std::min(std::max(cur[i][2], 0.0), zTop);
        curGain[i] = juce::Decibels::decibelsToGain(pv[i][4]->load(std::memory_order_relaxed), -60.f);
        sourceGate[i] = sourceMute[i]->load(std::memory_order_relaxed) < 0.5f
                        && (!anySourceSolo || sourceSolo[i]->load(std::memory_order_relaxed) >= 0.5f) ? 1.f : 0.f;
        curGain[i] *= sourceGate[i];
    }
    for (int k = 0; k < 4; ++k) cur[verbp::kListener][k] = pv[verbp::kListener][k]->load(std::memory_order_relaxed);
    cur[verbp::kListener][2] = std::min(std::max(cur[verbp::kListener][2], 0.0), zTop);
    double baseManualAz[verbp::kMaxSources + 1] = {};
    for (int o = 0; o <= verbp::kMaxSources; ++o) baseManualAz[o] = cur[o][3];
    float baseGainDb[verbp::kMaxSources] = {};
    for (int i = 0; i < NS; ++i) baseGainDb[i] = pv[i][4]->load(std::memory_order_relaxed);
    const verbp::CompiledPath* cp[verbp::kMaxSources + 1] = {};
    for(int o=0;o<=verbp::kMaxSources;++o) cp[o] = &pathMailboxes[(size_t)o].consume();
    const auto& modSet = modMailbox->consume();
    const auto& wallSnap = wallMailbox->consume();
    double pathTangent[verbp::kMaxSources + 1] = {};
    bool pathActive[verbp::kMaxSources + 1] = {};
    for(int o=0;o<=verbp::kMaxSources;++o) {
        const auto& path=*cp[o];
        if(!path.enabled || (o>=NS && o!=verbp::kListener))continue;
        int driver=o;
        if(path.group>=0)for(int d=0;d<NS;++d)if(cp[d]->enabled && cp[d]->group==path.group){driver=d;break;}
        const double u=pathParams[driver]->load(std::memory_order_relaxed);
        double yaw=0;const auto q=path.evaluate(u,&yaw);
        for(int k=0;k<3;++k)cur[o][k]=q[(size_t)k];
        cur[o][2]=std::clamp(cur[o][2],0.0,zTop);
        cur[o][3]=path.evaluateAzimuth(u,cur[o][3]);
        pathTangent[o]=yaw;pathActive[o]=true;
    }
    // E19: orientation is resolved after every position is known (Look at needs the target's position this block).
    // The azimuth control is the offset of Along path / Look at, so the raw manual value is kept: cur[o][3] holds the resolved azimuth afterwards.
    double manualAz[verbp::kMaxSources + 1] = {};
    for(int o=0;o<=verbp::kMaxSources;++o)manualAz[o]=cur[o][3];
    verbp::applyOrientation(NS,cur,pathTangent,pathActive,[&](int o)->const verbp::CompiledPath&{return *cp[o];});
    for(int o=0;o<=verbp::kMaxSources;++o)if(o<NS||o==verbp::kListener)liveAz[(size_t)o].store((float)cur[o][3],std::memory_order_relaxed);
    const uint64_t ser = loadSerial.load();
    const bool newEngine = (ser != serialSeen);
    const double appliedSize=engineSize.load();
    const double sizeScale=appliedSize*.01;
    const double sampleRate = std::max(8000.0, builtSr.load(std::memory_order_relaxed) > 0.0 ? builtSr.load(std::memory_order_relaxed) : hostSr);
    const double blockSec = (double)std::max(1, n) / sampleRate;
    double curPtOff[verbp::kMaxPoints][3] = {};
    for (int p = 0; p < P; ++p)
        for (int k = 0; k < 3; ++k) curPtOff[p][k] = ptOff[p][k].load(std::memory_order_relaxed);
    if (newEngine) {  // new engine: start from the current values, no glide
        prevAnySourceSolo = anySourceSolo;
        for (int i = 0; i < NS; ++i) {
            std::memcpy(prev[i], cur[i], sizeof prev[i]);
            std::memcpy(prevRaw[i], cur[i], sizeof prevRaw[i]);
            samplesSinceMove[i] = 0;
            prevGain[i] = curGain[i]; prevSourceGate[i] = sourceGate[i];
        }
        std::memcpy(prev[verbp::kListener], cur[verbp::kListener], sizeof prev[verbp::kListener]);
        std::memcpy(prevRaw[verbp::kListener], cur[verbp::kListener], sizeof prevRaw[verbp::kListener]);
        samplesSinceMove[verbp::kListener] = 0;
        for (int p = 0; p < P; ++p) std::memcpy(prevPtOff[p], curPtOff[p], sizeof prevPtOff[p]);
        havePlayHead = false;
        haveLastProcessWall = false;
        modProc->reset();
        modFallbackClock = 0;
        serialSeen = ser;
    }
    const double wallGap = haveLastProcessWall ? std::chrono::duration<double>(blockStart - lastProcessWall).count() : 0.0;
    const bool gapJump = !newEngine && haveLastProcessWall && wallGap > std::max(0.25, 8.0 * blockSec);
    lastProcessWall = blockStart;
    haveLastProcessWall = true;
    bool transportJump = false;
    bool passPlaying=false;
    bool phPresent = false, phPlaying = false;
    int64_t phBaseSample = 0;
    double phBaseSec = -1.0;
    if (auto* ph = getPlayHead()) {
        if (const auto posInfo = ph->getPosition()) {
            phPresent = true;
            const bool isPlaying = posInfo->getIsPlaying();
            phPlaying = isPlaying;
            passPlaying=isPlaying;
            if (const auto sec = posInfo->getTimeInSeconds()) phBaseSec = *sec;
            if (const auto ts = posInfo->getTimeInSamples()) {
                const int64_t curTs = *ts;
                phBaseSample = curTs;
                phBaseSec = -1.0;
                if (!newEngine && havePlayHead) {
                    const int64_t expected = prevPlayHeadSample + (prevPlaying ? (int64_t)prevBlockSamples : 0);
                    const int64_t tol = std::max<int64_t>(n / 2 + 64, (int64_t)std::llround(0.02 * sampleRate));
                    if (isPlaying && !prevPlaying) {
                        if (std::llabs(curTs - prevPlayHeadSample) > tol) transportJump = true;
                    } else if (isPlaying && prevPlaying) {
                        if (std::llabs(curTs - expected) > tol) transportJump = true;
                    }
                }
                prevPlayHeadSample = curTs;
                prevPlaying = isPlaying;
                prevBlockSamples = n;
                havePlayHead = true;
            }
        }
    }
    bool objJumped[verbp::kMaxSources + 1] = {};
    bool yawJumped[verbp::kMaxSources + 1] = {};
    bool anyJumped = false;
    bool anyMoved = false;
    if (!newEngine && n > 0) {
        auto checkObj = [&](int o) {
            const double dPos = std::hypot(std::hypot(cur[o][0] - prevRaw[o][0], cur[o][1] - prevRaw[o][1]), cur[o][2] - prevRaw[o][2]) * sizeScale;
            const double dAng = std::fabs(std::remainder(cur[o][3] - prevRaw[o][3], 360.0));
            const double dtSinceMove = (double)(samplesSinceMove[o] + (uint64_t)n) / sampleRate;
            const bool largeStep = dPos > std::max(0.75, 90.0 * blockSec);
            const bool fastStep = (dtSinceMove <= 1.5 * blockSec) && dPos > std::max(0.45, 90.0 * blockSec);
            const bool angJump = dAng > std::max(45.0, 1440.0 * blockSec);
            // A Manual turn still snaps azimuth and triggers the acoustic jump handling.
            // It must not reset the XYZ tracker while the object continues along its trajectory.
            yawJumped[o] = angJump;
            if (largeStep || fastStep || (angJump && (cp[o]->orient || cp[o]->lookAt))) objJumped[o] = true;
            anyJumped |= objJumped[o] || yawJumped[o];
            if (dPos > 0.02 || dAng > 2.0) anyMoved = true;
            if (dPos > 1e-6 || dAng > 1e-4) samplesSinceMove[o] = 0;
            else samplesSinceMove[o] += (uint64_t)n;
            std::memcpy(prevRaw[o], cur[o], sizeof prevRaw[o]);
        };
        for (int i = 0; i < NS; ++i) checkObj(i);
        checkObj(verbp::kListener);
        for (int p = 0; p < P; ++p) {
            const int o = b.points[(size_t)p].source;
            const double dPt = std::hypot(std::hypot(curPtOff[p][0] - prevPtOff[p][0], curPtOff[p][1] - prevPtOff[p][1]), curPtOff[p][2] - prevPtOff[p][2]) * sizeScale;
            const double dtSinceMove = (double)(samplesSinceMove[o] + (uint64_t)n) / sampleRate;
            const bool largePt = dPt > std::max(0.75, 90.0 * blockSec);
            const bool fastPt = (dtSinceMove <= 1.5 * blockSec) && dPt > std::max(0.45, 90.0 * blockSec);
            if (largePt || fastPt) { objJumped[o] = true; anyJumped = true; }
            if (dPt > 0.02) anyMoved = true;
            std::memcpy(prevPtOff[p], curPtOff[p], sizeof prevPtOff[p]);
        }
        if ((gapJump || transportJump) && anyMoved) {
            anyJumped = true;
            for (int i = 0; i < NS; ++i) objJumped[i] = true;
            objJumped[verbp::kListener] = true;
        }
        if (anyJumped) e.notifyJump();
    }
    const float makeupGain = juce::Decibels::decibelsToGain(makeupParam->load(std::memory_order_relaxed));
    const float outGain = juce::Decibels::decibelsToGain(gainParam->load(std::memory_order_relaxed));
    const bool creative = modeParam->load(std::memory_order_relaxed) >= .5f;
    e.setDecayMultipliers(creative ? decayParam[0]->load() : 1., creative ? decayParam[1]->load() : 1., creative ? decayParam[2]->load() : 1.);
    e.setEarlyLevel(creative ? earlyParam->load(std::memory_order_relaxed) : 0.);
    e.setEarlyDiffusion(diffusionParam->load(std::memory_order_relaxed));
    e.setDopplerStrength(creative ? dopplerParam->load(std::memory_order_relaxed) : 1.);
    e.setTailLevel(creative ? tailParam->load(std::memory_order_relaxed) : 0.);
    e.setFreezeAvailable(creative);
    e.setCreativeWet(creative?predelayParam->load():0,creative&&b.R>1?widthParam->load():100,
        creative&&freezeParam->load()>=.5f,creative?duckParam->load():0,creative?duckReleaseParam->load():300);
    int solo = 0;
    for (int k = 0; k < 3; ++k) if (soloParam[k]->load(std::memory_order_relaxed) >= 0.5f) solo |= 1 << k;
    e.setSoloMask(solo);
    lfeSoloGain.setTargetValue(solo == 0 || (solo & 1) ? 1.f : 0.f);
    // dry/wet: equal-power crossfade between the input channels passed straight through (channel k -> output k) and the room
    const float mixP = solo ? 1.f : std::min(std::max(mixParam->load(std::memory_order_relaxed) * 0.01f, 0.f), 1.f);
    float wetG1 = std::sin(mixP * 1.5707963f), dryG1 = std::cos(mixP * 1.5707963f);
    if (mixP >= 0.9999f) { wetG1 = 1.f; dryG1 = 0.f; }
    if (mixP <= 0.0001f) { wetG1 = 0.f; dryG1 = 1.f; }
    const bool positioned = spatialDryParam->load(std::memory_order_relaxed) >= 0.5f;
    e.setReverbMix(positioned && !solo ? mixP : 1.f);
    if (positioned) { wetG1 = 1.f; dryG1 = 0.f; }
    const float wetG0 = prevWet, dryG0 = prevDry;
    bool anyLfoActive = false;
    for (int o = 0; o <= verbp::kMaxSources; ++o) {
        if (o >= NS && o != verbp::kListener) continue;
        if (!pathActive[o]) continue;
        int driver = o;
        if (cp[o]->group >= 0) {
            for (int d = 0; d < NS; ++d)
                if (cp[d]->enabled && cp[d]->group == cp[o]->group) { driver = d; break; }
        }
        if (cp[driver]->lfo.enabled && cp[driver]->lfo.depth > 0.0) { anyLfoActive = true; break; }
    }
    bool hasAudioModRoute = false;
    for (int i = 0; i < modSet.count && i < verbp::kMaxModRoutes; ++i) {
        if (modSet.routes[(size_t)i].target != verbp::ModTarget::None) {
            hasAudioModRoute = true;
            break;
        }
    }
    const bool hasAudioModFeature = anyLfoActive || hasAudioModRoute;
    const bool hasAnyModFeature = anyLfoActive || (modSet.count > 0);
    const bool useModPath = (hasAudioModFeature && (creative || modProc->currentCreativeBlend() > 1e-6))
                         || modProc->hasAnyWetFilter()
                         || (modProc->currentCreativeBlend() > 0.0 && !creative && hasAudioModFeature);
    const bool needModEval = useModPath
                          || (hasAnyModFeature && (creative || modProc->currentCreativeBlend() > 1e-6))
                          || modProc->hasPendingHostFade();
    if (!needModEval) {
        modProc->creativeBlend = creative ? 1.0 : 0.0;
        modProc->snapped = false;
        modProc->prevSegGainValid = false;
        modProc->lastGen = modSet.generation;
        if (modLiveCount.load(std::memory_order_relaxed) != 0)
            modLiveCount.store(0, std::memory_order_release);
    }

    // The automation value read at the start of a host block is the position AT THE START of that block. The block is
    // rendered along a cubic Hermite curve that leaves where the previous block ended (continuous position and velocity:
    // a straight line per block makes the velocity jump at every block boundary, heard as steps in the Doppler pitch) and
    // ends at the position predicted for the end of this block (velocity and acceleration extrapolated from the last two
    // blocks: no extra latency, the old scheme lagged the automation by a whole block).
    struct HermSeg { double p0, p1, m0, m1; };
    HermSeg hs[verbp::kMaxSources + 1][3];
    if (n > 0) {
        // An alpha-beta tracker follows position and velocity of each coordinate. The measurement is the automation value
        // at the block start; it may arrive irregularly (a mouse drag writes ~60 times a second whatever the block size, a
        // sparse envelope less often): blocks without a new value must not read as "stopped", so the gains are set by a
        // time constant, not by the block, and the velocity is a smoothed estimate.
        const double alpha = 1.0 - std::exp(-(double)n / (0.020 * sampleRate)), beta = alpha * alpha / (2.0 - alpha);
        auto build = [&](int o) {
            const bool snapObj = (ser != hSeen || gapJump || transportJump || objJumped[o]);
            if (snapObj) prev[o][3] = cur[o][3];
            for (int k = 0; k < 3; ++k) {
                const double c0 = cur[o][k];
                if (snapObj || std::fabs(c0 - hPEnd[o][k]) > 20.0) {  // a new engine or a jump of the automation: start afresh here
                    hPEnd[o][k] = c0;
                    hVel[o][k] = 0.0;
                }
                const double nPrev = (double)std::max(hN, 1);
                const double p0 = hPEnd[o][k];                    // where the last block ended (what was already played)
                const double e = c0 - p0;                         // innovation
                const double xEst = p0 + alpha * e;               // corrected position at the block start
                const double vEst = hVel[o][k] + beta * e / nPrev;
                const double pEnd = xEst + vEst * n;              // predicted position at the block end
                const double d = pEnd - p0;
                const double sgn = d >= 0 ? 1.0 : -1.0, tl = 3.0 * std::fabs(d) / (double)n;  // monotone Hermite: no overshoot of the segment
                auto limit = [&](double v) { return sgn * std::min(std::max(v * sgn, 0.0), tl); };
                const double vS = limit(hVel[o][k]), vE = limit(vEst);
                hs[o][k] = {p0, pEnd, vS * n, vE * n};
                hPEnd[o][k] = pEnd;
                hVel[o][k] = vE;
            }
        };
        for (int i = 0; i < NS; ++i) build(i);
        build(verbp::kListener);
        hN2 = hN;
        hN = n;
        hSeen = ser;
    }

    const float* ip[verbp::kMaxPoints];
    float* op[verbp::kMaxReceivers];
    bool anySegMixMod = false;
    float segWetCur = prevWet, segDryCur = prevDry;
    double lastSegAz[verbp::kMaxSources + 1] = {};
    for (int o = 0; o <= verbp::kMaxSources; ++o) lastSegAz[o] = cur[o][3];
    float segModRaw[verbp::kMaxModRoutes] = {};
    float segModValue[verbp::kMaxModRoutes] = {};
    uint8_t segModActive[verbp::kMaxModRoutes] = {};
    double segHostOutNorm[verbp::kMaxModHostOuts];
    std::fill_n(segHostOutNorm, verbp::kMaxModHostOuts, verbp::kModHostOutNeutral);
    int done = 0;
    while (done < n) {
        const int m = std::min(n - done, b.maxBlock);
        // Detector: power sum of every host dry input, before routes, gains or reverb.
        std::fill_n(b.detector.data(),m,0.f);
        for(int ch=0;ch<nIn;++ch) {
            const float* dry=buf.getReadPointer(inMap[(size_t)ch])+done;
            for(int i=0;i<m;++i)if(std::abs(dry[i])<1e6f)b.detector[i]+=dry[i]*dry[i];
        }
        for(int i=0;i<m;++i)b.detector[i]=std::sqrt(b.detector[i]);
        // ---- inputs -> one signal per engine point (read before any output is written: in/out share the buffer) ----
        if (b.lfeOut >= 0) std::fill(b.lfe.begin(), b.lfe.begin() + m, 0.f);
        for (int p = 0; p < P; ++p) {
            const verbp::PointMap& pm = b.points[(size_t)p];
            const verbp::SourceCfg& c = b.cfg[(size_t)pm.source];
            float* d = b.in[(size_t)p].data();
            std::fill(d, d + m, 0.f);
            int cnt = 0;
            if (pm.channel >= 0) {
                const int ch = c.firstIn + pm.channel;
                if (ch < nIn) { std::memcpy(d, buf.getReadPointer(inMap[(size_t)ch]) + done, sizeof(float) * (size_t)m); cnt = 1; }
            } else {  // sum of all non-LFE channels
                const int nc = c.effectiveChannels();
                const int divisor = nc - (c.lfeChannel() >= 0 ? 1 : 0);
                for (int q = 0; q < nc; ++q) {
                    if (q == c.lfeChannel()) continue;
                    const int ch = c.firstIn + q;
                    if (ch >= nIn) continue;
                    const float* s = buf.getReadPointer(inMap[(size_t)ch]) + done;
                    for (int i = 0; i < m; ++i) d[i] += s[i];
                    ++cnt;
                }
                const float inv = 1.f / (float)std::max(1, divisor);
                for (int i = 0; i < m; ++i) d[i] *= inv;
            }
            // never let a NaN/Inf/absurd sample from upstream into the filters and delay lines (it would stay there)
            for (int i = 0; i < m; ++i) if (!(std::fabs(d[i]) < 1e6f)) d[i] = 0.f;
            if (!useModPath && cnt > 0) {  // source gain, ramped over the host block (bit-exact fast path)
                const float g0 = prevGain[pm.source], g1 = curGain[pm.source];
                for (int i = 0; i < m; ++i) d[i] *= g0 + (g1 - g0) * (float)(done + i + 1) / (float)n;
            }
        }
        if (!useModPath && b.lfeOut >= 0)
            for (int s = 0; s < NS; ++s) {
                const verbp::SourceCfg& c = b.cfg[(size_t)s];
                const int ch = c.lfeChannel() >= 0 ? c.firstIn + c.lfeChannel() : -1;
                if (ch < 0 || ch >= nIn) continue;
                const float* src = buf.getReadPointer(inMap[(size_t)ch]) + done;
                const float g0 = prevGain[s], g1 = curGain[s];
                for (int i = 0; i < m; ++i) {
                    const float v = std::fabs(src[i]) < 1e6f ? src[i] : 0.f;  // same guard as for the points
                    b.lfe[(size_t)i] += v * (g0 + (g1 - g0) * (float)(done + i + 1) / (float)n);
                }
            }
        // ---- engine, in segments of 128 samples with gliding positions ----
        for (int off = 0; off < m; off += 128) {
            const int len = std::min(128, m - off);
            const double t = (double)(done + off + len) / n;
            const double t2 = t * t, t3 = t2 * t;
            const double h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
            // positions follow a cubic Hermite curve between the automation values (continuous velocity: a straight line
            // per block makes the velocity jump at every block boundary, which is heard as steps in the Doppler pitch)
            auto mix = [&](int o, int k) {
                if (k >= 3) {
                    if (objJumped[o] || yawJumped[o]) return cur[o][k];
                    return prev[o][k] + std::remainder(cur[o][k] - prev[o][k], 360.0) * t;
                }
                const HermSeg& sg = hs[o][k];
                return h00 * sg.p0 + h10 * sg.m0 + h01 * sg.p1 + h11 * sg.m1;
            };
            double mpx[verbp::kMaxSources + 1] = {}, mpy[verbp::kMaxSources + 1] = {}, mpz[verbp::kMaxSources + 1] = {}, mpt[verbp::kMaxSources + 1] = {};
            if (!useModPath) {
                for (int o = 0; o <= verbp::kMaxSources; ++o) if (o < NS || o == verbp::kListener) {
                    mpx[o] = mix(o, 0); mpy[o] = mix(o, 1); mpz[o] = mix(o, 2);
                    if (pathActive[o] && !cp[o]->points.empty()) cp[o]->projectOnPath(mpx[o], mpy[o], mpz[o], &mpt[o]);
                }
                auto azOf = [&](int o) -> double {
                    const verbp::CompiledPath& cpa = *cp[o];
                    if (!cpa.lookAt) {
                        if (cpa.orient && pathActive[o])
                            return verbp::orientedAzimuth(verbp::OrientMode::AlongPath, manualAz[o], mpt[o], false, 0.0, cpa.orientOffset);
                        return mix(o, 3);
                    }
                    double tx = 0, ty = 0, look = 0; bool ok = false;
                    if (cpa.lookPoint) { tx = cpa.lookPos[0]; ty = cpa.lookPos[1]; ok = true; }
                    else { const int tt = cpa.lookTarget; if (tt >= 0 && tt != o && tt <= verbp::kListener && (tt < NS || tt == verbp::kListener)) { tx = mpx[tt]; ty = mpy[tt]; ok = true; } }
                    ok = ok && verbp::lookAtYaw(mpx[o], mpy[o], tx, ty, look);
                    return verbp::orientedAzimuth(verbp::OrientMode::LookAt, manualAz[o], 0.0, ok, look, cpa.orientOffset);
                };
                for (int s = 0; s < NS; ++s) {
                    const double az = azOf(s), x = mpx[s], y = mpy[s], z = mpz[s];
                    const double ca = std::cos(az * verb::kPi / 180.0), sa = std::sin(az * verb::kPi / 180.0);
                    for (int p = b.pointStart[(size_t)s]; p < b.pointStart[(size_t)s] + b.pointCount[(size_t)s]; ++p) {
                        // live offset in the source's frame (x forward along the azimuth, y left, z up)
                        const double ox = curPtOff[p][0], oy = curPtOff[p][1], oz = curPtOff[p][2];
                        e.setSourcePosition(p, (x + ca * ox - sa * oy)*sizeScale, (y + sa * ox + ca * oy)*sizeScale, std::min(std::max(z + oz, 0.0), zTop)*sizeScale);
                        e.setSourceAzimuth(p, az);
                    }
                }
                e.setListener(mpx[verbp::kListener]*sizeScale, mpy[verbp::kListener]*sizeScale, mpz[verbp::kListener]*sizeScale, azOf(verbp::kListener));
                if (needModEval) {
                    const double blend = modProc->advanceCreativeBlend(creative, len, sampleRate);
                    double segPoses[verbp::kMaxSources + 1][4] = {};
                    double segU[verbp::kMaxSources + 1] = {};
                    for (int o = 0; o <= verbp::kMaxSources; ++o) {
                        if (o >= NS && o != verbp::kListener) continue;
                        segPoses[o][0] = mpx[o];
                        segPoses[o][1] = mpy[o];
                        segPoses[o][2] = mpz[o];
                        segPoses[o][3] = azOf(o);
                        if (pathActive[o]) {
                            int driver = o;
                            if (cp[o]->group >= 0) {
                                for (int d = 0; d < NS; ++d)
                                    if (cp[d]->enabled && cp[d]->group == cp[o]->group) { driver = d; break; }
                            }
                            segU[o] = std::clamp((double)pathParams[driver]->load(std::memory_order_relaxed), 0.0, 1.0);
                        }
                        lastSegAz[o] = segPoses[o][3];
                    }
                    std::array<verbp::SourceModState, verbp::kModMaxSources> srcMod{};
                    verbp::GlobalModState globMod{};
                    modProc->evaluateSegment(modSet, NS, segPoses, segU, objJumped, wallSnap, creative, blend, len, sampleRate, srcMod, globMod, segModRaw, segModValue, segModActive, segHostOutNorm);
                    modProc->snapped = false;
                }
                modProc->prevSegGainValid = false;
            } else {
                const bool segSnap = modProc->snapped;
                const double blend = modProc->advanceCreativeBlend(creative, len, sampleRate);
                const double tLfoSec = phPresent
                    ? (phPlaying
                        ? (phBaseSec >= 0.0 ? std::max(0.0, phBaseSec + (double)(done + off) / sampleRate)
                                            : std::max(0.0, (double)(phBaseSample + done + off) / sampleRate))
                        : 0.0)
                    : (double)(modFallbackClock + done + off) / sampleRate;
                double segPoses[verbp::kMaxSources + 1][4] = {};
                double segU[verbp::kMaxSources + 1] = {};
                bool segPathActive[verbp::kMaxSources + 1] = {};
                for (int o = 0; o <= verbp::kMaxSources; ++o) {
                    if (o >= NS && o != verbp::kListener) continue;
                    segPathActive[o] = pathActive[o];
                    if (pathActive[o]) {
                        int driver = o;
                        if (cp[o]->group >= 0) {
                            for (int d = 0; d < NS; ++d)
                                if (cp[d]->enabled && cp[d]->group == cp[o]->group) { driver = d; break; }
                        }
                        const double uBase = std::clamp((double)pathParams[driver]->load(std::memory_order_relaxed), 0.0, 1.0);
                        double uEff = uBase;
                        if (cp[driver]->lfo.enabled && cp[driver]->lfo.depth > 0.0 && blend > 1e-9) {
                            uEff = cp[driver]->lfo.applyToU(uBase, tLfoSec, cp[o]->closed, blend);
                            double yaw = 0.0;
                            const auto q = cp[o]->evaluate(uEff, &yaw);
                            mpx[o] = q[0]; mpy[o] = q[1]; mpz[o] = std::clamp(q[2], 0.0, zTop);
                            mpt[o] = yaw;
                            segPoses[o][3] = cp[o]->evaluateAzimuth(uEff, baseManualAz[o]);
                        } else {
                            mpx[o] = mix(o, 0); mpy[o] = mix(o, 1); mpz[o] = mix(o, 2);
                            if (!cp[o]->points.empty()) cp[o]->projectOnPath(mpx[o], mpy[o], mpz[o], &mpt[o]);
                            segPoses[o][3] = (cp[o]->orient || cp[o]->lookAt) ? manualAz[o] : mix(o, 3);
                        }
                        segU[o] = uEff;
                    } else {
                        mpx[o] = mix(o, 0); mpy[o] = mix(o, 1); mpz[o] = mix(o, 2);
                        segPoses[o][3] = cp[o]->lookAt ? manualAz[o] : mix(o, 3);
                    }
                    segPoses[o][0] = mpx[o];
                    segPoses[o][1] = mpy[o];
                    segPoses[o][2] = mpz[o];
                }
                verbp::applyOrientation(NS, segPoses, mpt, segPathActive, [&](int o) -> const verbp::CompiledPath& { return *cp[o]; });
                for (int o = 0; o <= verbp::kMaxSources; ++o) if (o < NS || o == verbp::kListener) lastSegAz[o] = segPoses[o][3];

                std::array<verbp::SourceModState, verbp::kModMaxSources> srcMod{};
                verbp::GlobalModState globMod{};
                modProc->evaluateSegment(modSet, NS, segPoses, segU, objJumped, wallSnap, creative, blend, len, sampleRate, srcMod, globMod, segModRaw, segModValue, segModActive, segHostOutNorm);

                float targetSegGain[verbp::kMaxSources] = {};
                for (int s = 0; s < NS; ++s) {
                    const auto& st = srcMod[(size_t)s];
                    if (st.hasGain) {
                        const double effDb = std::clamp((double)baseGainDb[s] + st.gainDb, -60.0, 12.0);
                        targetSegGain[s] = juce::Decibels::decibelsToGain((float)effDb, -60.f) * sourceGate[s];
                    } else {
                        targetSegGain[s] = prevGain[s] + (curGain[s] - prevGain[s]) * (float)t;
                    }
                    if (!modProc->prevSegGainValid) modProc->prevSegGain[(size_t)s] = segSnap ? targetSegGain[s] : prevGain[s];
                }
                for (int p = 0; p < P; ++p) {
                    const int s = b.points[(size_t)p].source;
                    float* segBuf = b.in[(size_t)p].data() + off;
                    const float g0 = modProc->prevSegGain[(size_t)s], g1 = targetSegGain[s];
                    const float dg = len > 0 ? (g1 - g0) / (float)len : 0.f;
                    for (int i = 0; i < len; ++i) segBuf[i] *= g0 + dg * (float)(i + 1);
                    modProc->processPointFilters(p, segBuf, len, sampleRate, srcMod[(size_t)s], creative, segSnap);
                }
                if (b.lfeOut >= 0) {
                    for (int s = 0; s < NS; ++s) {
                        const verbp::SourceCfg& c = b.cfg[(size_t)s];
                        const int ch = c.lfeChannel() >= 0 ? c.firstIn + c.lfeChannel() : -1;
                        if (ch < 0 || ch >= nIn) continue;
                        const float* src = buf.getReadPointer(inMap[(size_t)ch]) + done + off;
                        const float g0 = modProc->prevSegGain[(size_t)s], g1 = targetSegGain[s];
                        const float dg = len > 0 ? (g1 - g0) / (float)len : 0.f;
                        for (int i = 0; i < len; ++i) {
                            const float v = std::fabs(src[i]) < 1e6f ? src[i] : 0.f;
                            b.lfe[(size_t)(off + i)] += v * (g0 + dg * (float)(i + 1));
                        }
                    }
                }
                for (int s = 0; s < NS; ++s) modProc->prevSegGain[(size_t)s] = targetSegGain[s];
                modProc->prevSegGainValid = true;
                modProc->snapped = false;

                if (globMod.hasEarly || creative) {
                    const double baseE = earlyParam->load(std::memory_order_relaxed);
                    e.setEarlyLevel(creative ? std::clamp(baseE + globMod.earlyDb, -60.0, 6.0) : 0.0);
                }
                if (globMod.hasTail || creative) {
                    const double baseT = tailParam->load(std::memory_order_relaxed);
                    e.setTailLevel(creative ? std::clamp(baseT + globMod.tailDb, -48.0, 12.0) : 0.0);
                }
                if (globMod.hasMix && !solo) {
                    const double baseM = mixParam->load(std::memory_order_relaxed);
                    const float effM = std::clamp((float)(baseM + (creative ? globMod.mixPct : 0.0)) * 0.01f, 0.f, 1.f);
                    e.setReverbMix(positioned ? effM : 1.f);
                    if (!positioned) {
                        if (!anySegMixMod && off > 0) {
                            for (int i = 0; i < off; ++i) {
                                const float ti = (float)(done + i + 1) / (float)n;
                                b.wetMix[(size_t)i] = wetG0 + (wetG1 - wetG0) * ti;
                                b.dryMix[(size_t)i] = dryG0 + (dryG1 - dryG0) * ti;
                            }
                            segWetCur = b.wetMix[(size_t)(off - 1)];
                            segDryCur = b.dryMix[(size_t)(off - 1)];
                        }
                        anySegMixMod = true;
                        float wTarget = std::sin(effM * 1.5707963f), dTarget = std::cos(effM * 1.5707963f);
                        if (effM >= 0.9999f) { wTarget = 1.f; dTarget = 0.f; }
                        if (effM <= 0.0001f) { wTarget = 0.f; dTarget = 1.f; }
                        const float dw = len > 0 ? (wTarget - segWetCur) / (float)len : 0.f;
                        const float dd = len > 0 ? (dTarget - segDryCur) / (float)len : 0.f;
                        for (int i = 0; i < len; ++i) {
                            b.wetMix[(size_t)(off + i)] = segWetCur + dw * (float)(i + 1);
                            b.dryMix[(size_t)(off + i)] = segDryCur + dd * (float)(i + 1);
                        }
                        segWetCur = wTarget; segDryCur = dTarget;
                    }
                } else if (anySegMixMod && !positioned && !solo) {
                    const float wTarget = wetG0 + (wetG1 - wetG0) * (float)t;
                    const float dTarget = dryG0 + (dryG1 - dryG0) * (float)t;
                    const float dw = len > 0 ? (wTarget - segWetCur) / (float)len : 0.f;
                    const float dd = len > 0 ? (dTarget - segDryCur) / (float)len : 0.f;
                    for (int i = 0; i < len; ++i) {
                        b.wetMix[(size_t)(off + i)] = segWetCur + dw * (float)(i + 1);
                        b.dryMix[(size_t)(off + i)] = segDryCur + dd * (float)(i + 1);
                    }
                    segWetCur = wTarget; segDryCur = dTarget;
                }

                for (int s = 0; s < NS; ++s) {
                    const double az = segPoses[s][3], x = segPoses[s][0], y = segPoses[s][1], z = segPoses[s][2];
                    const double ca = std::cos(az * verb::kPi / 180.0), sa = std::sin(az * verb::kPi / 180.0);
                    for (int p = b.pointStart[(size_t)s]; p < b.pointStart[(size_t)s] + b.pointCount[(size_t)s]; ++p) {
                        // live offset in the source's frame (x forward along the azimuth, y left, z up)
                        const double ox = curPtOff[p][0], oy = curPtOff[p][1], oz = curPtOff[p][2];
                        e.setSourcePosition(p, (x + ca * ox - sa * oy)*sizeScale, (y + sa * ox + ca * oy)*sizeScale, std::min(std::max(z + oz, 0.0), zTop)*sizeScale);
                        e.setSourceAzimuth(p, az);
                    }
                }
                e.setListener(segPoses[verbp::kListener][0]*sizeScale, segPoses[verbp::kListener][1]*sizeScale, segPoses[verbp::kListener][2]*sizeScale, segPoses[verbp::kListener][3]);
            }
            for (int p = 0; p < P; ++p) ip[p] = b.in[(size_t)p].data() + off;
            for (int r = 0; r < R; ++r) op[r] = b.out[(size_t)r].data() + off;
            e.acknowledgePoseScale(appliedSize/b.constructionSize);
            e.recordPassAudio(passPlaying);
            e.process(ip, op, len, b.detector.data()+off);
            duckReduction.store(e.duckReductionDb(),std::memory_order_relaxed);
        }
        for (int ch = 0; ch < nOut; ++ch) {
            float* raw = buf.getWritePointer(ch) + done;
            // Mix in ACN first, reusing the engine's channel buffers for FuMa.
            float* d = reorder && ch < R ? b.out[(size_t)ch].data() : raw;
            bool mapped = false;
            float gate0 = 0.f, gate1 = 0.f;
            for (int s = 0; s < NS; ++s) if (ch >= b.cfg[s].firstIn && ch < b.cfg[s].firstIn + b.cfg[s].effectiveChannels()) {
                mapped = true; gate0 = std::max(gate0, prevSourceGate[s]); gate1 = std::max(gate1, sourceGate[s]);
            }
            if (!mapped) { gate0 = prevAnySourceSolo ? 0.f : 1.f; gate1 = anySourceSolo ? 0.f : 1.f; }
            const bool hasDry = ch < nIn && (anySegMixMod || dryG1 > 0.f || dryG0 > 0.f);
            auto wetAt = [&](int i) { return anySegMixMod ? b.wetMix[(size_t)i] : (wetG0 + (wetG1 - wetG0) * (float)(done + i + 1) / (float)n); };
            auto dryAt = [&](int i) {
                const float g = anySegMixMod ? b.dryMix[(size_t)i] : (dryG0 + (dryG1 - dryG0) * ((float)(done + i + 1) / (float)n));
                const float t = (float)(done + i + 1) / (float)n;
                return g * (gate0 + (gate1 - gate0) * t);
            };
            if (ch < R) {
                const float* s = b.out[(size_t)ch].data();
                if (ch == b.lfeOut) {
                    for (int i = 0; i < m; ++i) {
                        const float dry = hasDry ? (std::fabs(raw[i]) < 1e6f ? raw[i] : 0.f) * dryAt(i) : 0.f;
                        d[i] = (s[i] + lfeSoloGain.getNextValue() * b.lfe[(size_t)i]) * outGain * wetAt(i) + dry;
                    }
                } else {
                    for (int i = 0; i < m; ++i) {
                        const float dry = hasDry ? (std::fabs(raw[i]) < 1e6f ? raw[i] : 0.f) * dryAt(i) : 0.f;
                        d[i] = s[i] * outGain * wetAt(i) + dry;
                    }
                }
                for (int i = 0; i < m; ++i) if (!(std::fabs(d[i]) < 1e6f)) d[i] = 0.f;  // last line of defence: never hand NaN/Inf to the host
            } else {
                for (int i = 0; i < m; ++i) {  // beyond the layout: silent, except for the dry signal
                    const float dry = hasDry ? (std::fabs(raw[i]) < 1e6f ? raw[i] : 0.f) * dryAt(i) : 0.f;
                    d[i] = dry;
                }
            }
        }
        if (reorder) for (int ch = 0; ch < ambisonicChannels; ++ch) {
            auto* destination = buf.getWritePointer(ch) + done;
            const int source = ambisonics.acn(ch);
            if (source < ambisonicChannels) juce::FloatVectorOperations::copy(destination, b.out[(size_t)source].data(), m);
            else juce::FloatVectorOperations::clear(destination, m); // omitted ACN component on a short host bus
        }
        for(int ch=0;ch<nOut;++ch) juce::FloatVectorOperations::multiply(buf.getWritePointer(ch)+done,makeupGain,m);
        if (format != 0) for (int ch = 0; ch < ambisonicChannels; ++ch)
            juce::FloatVectorOperations::multiply(buf.getWritePointer(ch) + done, ambisonics.gain(ch), m);
        done += m;
    }
    if (needModEval && n > 0) {
        for (int o = 0; o <= verbp::kMaxSources; ++o) if (o < NS || o == verbp::kListener) liveAz[(size_t)o].store((float)lastSegAz[o], std::memory_order_relaxed);
        const int nRoutes = std::min(modSet.count, verbp::kMaxModRoutes);
        for (int i = 0; i < nRoutes; ++i) {
            modLiveRaw[(size_t)i].store(segModRaw[(size_t)i], std::memory_order_relaxed);
            modLiveValue[(size_t)i].store(segModValue[(size_t)i], std::memory_order_relaxed);
            modLiveActive[(size_t)i].store(segModActive[(size_t)i], std::memory_order_relaxed);
        }
        modLiveCount.store(nRoutes, std::memory_order_relaxed);
        modLiveGen.store(modSet.generation, std::memory_order_release);
    }
    if (n > 0) {
        if (needModEval) {
            for (int s = 0; s < verbp::kMaxModHostOuts; ++s)
                if (modOutParams[(size_t)s])
                    modOutParams[(size_t)s]->pushBlockValue((float)segHostOutNorm[s]);
        } else {
            for (int s = 0; s < verbp::kMaxModHostOuts; ++s)
                if (modOutParams[(size_t)s])
                    modOutParams[(size_t)s]->resetToNeutralIfChanged();
        }
    }
    if (!phPresent || phPlaying) modFallbackClock += n;
    prevWet = anySegMixMod ? segWetCur : wetG1;
    prevDry = anySegMixMod ? segDryCur : dryG1;
    for (int i = 0; i < NS; ++i) {
        std::memcpy(prev[i], cur[i], sizeof prev[i]);
        prevGain[i] = (useModPath && modProc->prevSegGainValid) ? modProc->prevSegGain[(size_t)i] : curGain[i];
        prevSourceGate[i] = sourceGate[i];
    }
    prevAnySourceSolo = anySourceSolo;
    std::memcpy(prev[verbp::kListener], cur[verbp::kListener], sizeof prev[verbp::kListener]);
}

#if VERB_WEB_UI_ENABLED
// Opt-in WebView editor (VERB_UI=web or the stored preference); the native editor otherwise, and on any failure.
juce::AudioProcessorEditor* VerbProcessor::createEditor() { return verbweb::createEditorFor(*this); }
#else
juce::AudioProcessorEditor* VerbProcessor::createEditor() { return new VerbEditor(*this); }
#endif

// ---------- state ----------
void VerbProcessor::getStateInformation(juce::MemoryBlock& dest) {
    auto st = apvts.copyState();
    // A held sound is ephemeral and its audio history is intentionally not serialized.
    auto freezeState=st.getChildWithProperty("id","freeze");if(freezeState.isValid())freezeState.setProperty("value",0.f,nullptr);
    {
        juce::ScopedLock l(dispLock);
        st.setProperty("sceneText", sceneText, nullptr);
        if (!automationRange.isDefault()) {
            st.setProperty("autoXMin", automationRange.xMin, nullptr);
            st.setProperty("autoXMax", automationRange.xMax, nullptr);
            st.setProperty("autoYMin", automationRange.yMin, nullptr);
            st.setProperty("autoYMax", automationRange.yMax, nullptr);
            st.setProperty("autoZMin", automationRange.zMin, nullptr);
            st.setProperty("autoZMax", automationRange.zMax, nullptr);
        }
        st.setProperty("sceneName", sceneName, nullptr);
        st.setProperty("inputMeters", inputMetersOn.load(), nullptr);
        st.setProperty("keepRouting", keepRouting.load(), nullptr);
        if (keepProcessing.load()) st.setProperty("keepProcessing", true, nullptr);   // optional: absent in old projects and when OFF
        else st.removeProperty("keepProcessing", nullptr);                           // the APVTS tree may still carry the field of a restored state
        st.setProperty("mixerCompact", mixerCompact.load(), nullptr);
        st.setProperty("navLocked", navLocked.load(), nullptr);
        st.setProperty("showOutliner", showOutliner.load(), nullptr);
        st.setProperty("tailRays", tailSettings.rays, nullptr);
        st.setProperty("adaptiveTail", tailSettings.adaptive, nullptr);
        st.removeChild(st.getChildWithName("Sources"), nullptr);
        st.removeChild(st.getChildWithName("Output"), nullptr);
        juce::ValueTree srcs("Sources");
        bool anyInstrument = false;
        for (const auto& c : srcCfg) {
            juce::ValueTree v("Src");
            v.setProperty("name", juce::String(c.name), nullptr);
            if (!c.color.empty()) {
                const auto normColor = verbp::normalizeSourceColor(c.color);
                if (!normColor.empty()) v.setProperty("color", juce::String(normColor), nullptr);
            }
            v.setProperty("format", c.format, nullptr);
            v.setProperty("channels", c.channels, nullptr);
            v.setProperty("firstIn", c.firstIn, nullptr);
            v.setProperty("sum", c.sum, nullptr);
            v.setProperty("spread", c.spread, nullptr);
            const bool hasInst = !c.instrument.empty() && c.instrument != "none";
            if (hasInst && c.directivity != nullptr) {
                v.setProperty("pattern", verb::fitAnalyticPattern(*c.directivity, c.instrumentAmount, c.aimAzDeg, c.aimElDeg, c.aimRollDeg), nullptr);
                v.setProperty("savedPattern", c.pattern, nullptr);
            } else {
                v.setProperty("pattern", c.pattern, nullptr);
            }
            if (hasInst) {
                anyInstrument = true;
                v.setProperty("instrument", juce::String(c.instrument), nullptr);
                v.setProperty("instrumentAmount", c.instrumentAmount, nullptr);
                if (c.aimAzDeg != 0.0) v.setProperty("aimAzDeg", c.aimAzDeg, nullptr);
                if (c.aimElDeg != 0.0) v.setProperty("aimElDeg", c.aimElDeg, nullptr);
                if (c.aimRollDeg != 0.0) v.setProperty("aimRollDeg", c.aimRollDeg, nullptr);
            }
            if (!c.customDirectivityData.empty() && c.customDirectivityData.size() <= verb::kMaxVerbdirBytes)
                v.setProperty("customDirectivityData", juce::String(c.customDirectivityData), nullptr);
            if (!c.customDirectivityName.empty())
                v.setProperty("customDirectivityName", juce::String(c.customDirectivityName), nullptr);
            if (!c.customDirectivityPath.empty())
                v.setProperty("customDirectivityPath", juce::String(c.customDirectivityPath), nullptr);
            v.setProperty("free", c.freePoints, nullptr);
            auto join = [](const std::vector<float>& a) {
                juce::StringArray s;
                for (float f : a) s.add(juce::String(f, 4));
                return s.joinIntoString(",");
            };
            v.setProperty("ox", join(c.ox), nullptr);
            v.setProperty("oy", join(c.oy), nullptr);
            v.setProperty("oz", join(c.oz), nullptr);
            srcs.appendChild(v, nullptr);
        }
        if (anyInstrument) st.setProperty("instrumentDataRev", 1, nullptr);
        st.appendChild(srcs, nullptr);
        juce::ValueTree out("Output");
        out.setProperty("layout", outCfg.layout, nullptr);
        out.setProperty("radius", outCfg.radius, nullptr);
        out.setProperty("micPattern", outCfg.micPattern, nullptr);
        if (outCfg.ambisonicsNormalization != 0) out.setProperty("ambisonicsNormalization", outCfg.ambisonicsNormalization, nullptr);
        if (outCfg.ambisonicsLevelDb != 0) out.setProperty("ambisonicsLevelDb", outCfg.ambisonicsLevelDb, nullptr);
        st.appendChild(out, nullptr);
    }
    st.removeChild(st.getChildWithName("MovementPaths"),nullptr);
    juce::ValueTree paths("MovementPaths");
    for(int o=0;o<=verbp::kMaxSources;++o) {
        const auto path=getMovementPath(o);if(path.points.empty()&&!path.lookAt&&path.orientOffset==0.0&&path.azKeys.empty()&&path.lfo.isDefault())continue;
        juce::ValueTree v("Path");v.setProperty("object",o,nullptr);
        v.setProperty("enabled",path.enabled,nullptr);v.setProperty("closed",path.closed,nullptr);
        v.setProperty("smooth",path.smooth,nullptr);v.setProperty("orient",path.orient,nullptr);v.setProperty("laps",path.laps,nullptr);
        if(path.group>=0)v.setProperty("group",path.group,nullptr);
        auto fmtDouble=[](double val){std::ostringstream number;number.imbue(std::locale::classic());number<<std::setprecision(std::numeric_limits<double>::max_digits10)<<val;return juce::String(number.str());};
        // E19 orientation: optional, written only when not at the defaults (old projects stay byte-identical).
        if(path.lookAt){
            v.setProperty("lookAt",true,nullptr);
            if(path.lookPoint){v.setProperty("lookPoint",true,nullptr);v.setProperty("lookX",fmtDouble(path.lookPos[0]),nullptr);v.setProperty("lookY",fmtDouble(path.lookPos[1]),nullptr);v.setProperty("lookZ",fmtDouble(path.lookPos[2]),nullptr);}
            else v.setProperty("lookTarget",path.lookTarget,nullptr);
        }
        if(path.orientOffset!=0.0)v.setProperty("orientOffset",fmtDouble(path.orientOffset),nullptr);
        if(path.start!=0.0)v.setProperty("start",fmtDouble(path.start),nullptr);
        if(path.end!=100.0)v.setProperty("end",fmtDouble(path.end),nullptr);
        for(size_t pi=0;pi<path.points.size();++pi){
            const auto& q=path.points[pi];
            juce::ValueTree pt("Point");
            for(int k=0;k<3;++k)pt.setProperty(k==0?"x":k==1?"y":"z",fmtDouble(q[(size_t)k]),nullptr);
            if(pi<path.times.size()&&path.times[pi].has_value())pt.setProperty("time",fmtDouble(*path.times[pi]),nullptr);
            if(pi<path.easings.size()&&path.easings[pi]!=verbp::PathEasing::Linear){
                const char* eName="linear";
                if(path.easings[pi]==verbp::PathEasing::EaseIn)eName="easeIn";
                else if(path.easings[pi]==verbp::PathEasing::EaseOut)eName="easeOut";
                else if(path.easings[pi]==verbp::PathEasing::EaseInOut)eName="easeInOut";
                pt.setProperty("easing",juce::String(eName),nullptr);
            }
            v.appendChild(pt,nullptr);
        }
        for(const auto& k:path.azKeys){
            juce::ValueTree ak("AzKey");
            ak.setProperty("t",fmtDouble(k.t),nullptr);
            ak.setProperty("az",fmtDouble(k.azDeg),nullptr);
            if(k.easing!=verbp::AzimuthEasing::Linear){
                const char* eName="linear";
                if(k.easing==verbp::AzimuthEasing::EaseIn)eName="easeIn";
                else if(k.easing==verbp::AzimuthEasing::EaseOut)eName="easeOut";
                else if(k.easing==verbp::AzimuthEasing::EaseInOut)eName="easeInOut";
                else if(k.easing==verbp::AzimuthEasing::Hold)eName="hold";
                ak.setProperty("easing",juce::String(eName),nullptr);
            }
            v.appendChild(ak,nullptr);
        }
        if(!path.lfo.isDefault()){
            juce::ValueTree lfoTree("Lfo");
            lfoTree.setProperty("enabled",path.lfo.enabled,nullptr);
            lfoTree.setProperty("shape",juce::String(verbp::lfoShapeName(path.lfo.shape)),nullptr);
            lfoTree.setProperty("rateHz",fmtDouble(path.lfo.rateHz),nullptr);
            lfoTree.setProperty("depth",fmtDouble(path.lfo.depth),nullptr);
            lfoTree.setProperty("phaseDeg",fmtDouble(path.lfo.phaseDeg),nullptr);
            lfoTree.setProperty("seed",(int)path.lfo.seed,nullptr);
            v.appendChild(lfoTree,nullptr);
        }
        paths.appendChild(v,nullptr);
    }
    if(paths.getNumChildren())st.appendChild(paths,nullptr);
    st.removeChild(st.getChildWithName("Modulation"), nullptr);
    {
        const auto routes = getModulationRoutes();
        if (!routes.empty()) st.appendChild(modRoutesToValueTree(routes), nullptr);
    }
    st.removeChild(st.getChildWithName("RecordedPass"),nullptr);
    {
        std::shared_ptr<verb::Engine> eng;
        { juce::SpinLock::ScopedLockType el(engineLock); if (bundle) eng = bundle->engine; }
        verb::Engine::RecordedPassData rp;
        bool useEngine = false;
        { juce::ScopedLock l(dispLock); useEngine = !roomLoadPendingLocked() && eng != nullptr; if (!useEngine) rp = savedRecordedPass; }
        if (useEngine) rp = eng->exportRecordedPass();
        if (rp.samples >= 2 && rp.objects >= 2 && rp.poses.size() == (size_t)rp.samples * (size_t)rp.objects * 4u) {
            juce::MemoryBlock raw(rp.poses.data(), rp.poses.size() * sizeof(double));
            juce::ValueTree rpTree("RecordedPass");
            rpTree.setProperty("objects", rp.objects, nullptr);
            rpTree.setProperty("samples", rp.samples, nullptr);
            rpTree.setProperty("data", raw.toBase64Encoding(), nullptr);
            st.appendChild(rpTree, nullptr);
        }
    }
    // P4: persist the baked tables with the project/preset (optional child; old states simply lack it).
    // The tables carry a content signature; on load they are only installed when it still matches the scene.
    st.removeChild(st.getChildWithName("BakeCache"), nullptr);
    {
        std::shared_ptr<verb::Engine> eng;
        { juce::SpinLock::ScopedLockType el(engineLock); if (bundle) eng = bundle->engine; }
        std::vector<verb::BakedTable> tables;
        bool pending = false;
        { juce::ScopedLock l(dispLock); pending = roomLoadPendingLocked(); if (pending) tables = savedBakeTables; }
        bool baked = false;
        if (!pending && eng) {
            const auto bs = eng->bakeStats();
            baked = bs.state == verb::Engine::BakeState::Baked || bs.state == verb::Engine::BakeState::Approximate;
            if (baked) for (const auto& t : eng->bakedTablesSnapshot()) if (t) tables.push_back(*t);
        }
        if (!tables.empty()) {
            const auto bin = verb::bakeSerializeTables(tables);
            if (bin.size() <= verb::kBakePersistMaxBytes) {
                bakeTooLargeToStore = false;
                juce::MemoryBlock raw(bin.data(), bin.size());
                juce::ValueTree bc("BakeCache");
                bc.setProperty("v", (int)verb::kBakePersistVersion, nullptr);
                bc.setProperty("data", raw.toBase64Encoding(), nullptr);
                st.appendChild(bc, nullptr);
            } else {
                bakeTooLargeToStore = true;  // keep today's behaviour; the status text says so
            }
        } else if (baked || pending) {
            bakeTooLargeToStore = false;
        }
    }
    if (auto xml = st.createXml()) copyXmlToBinary(*xml, dest);
}

void VerbProcessor::setStateInformation(const void* data, int size) {
    auto xml = getXmlFromBinary(data, size);
    if (!xml) return;
    auto st = juce::ValueTree::fromXml(*xml);
    if (!st.isValid()) return;
    const verbp::AutomationRange prevRange = getAutomationRange();
    verbp::AutomationRange loadedRange;
    if (st.hasProperty("autoXMin") && st.hasProperty("autoXMax")
        && st.hasProperty("autoYMin") && st.hasProperty("autoYMax")
        && st.hasProperty("autoZMin") && st.hasProperty("autoZMax")) {
        loadedRange.xMin = (double)st.getProperty("autoXMin", loadedRange.xMin);
        loadedRange.xMax = (double)st.getProperty("autoXMax", loadedRange.xMax);
        loadedRange.yMin = (double)st.getProperty("autoYMin", loadedRange.yMin);
        loadedRange.yMax = (double)st.getProperty("autoYMax", loadedRange.yMax);
        loadedRange.zMin = (double)st.getProperty("autoZMin", loadedRange.zMin);
        loadedRange.zMax = (double)st.getProperty("autoZMax", loadedRange.zMax);
        loadedRange = verbp::AutomationRange::sanitised(loadedRange);
    }
    setAutomationRange(loadedRange, false, false);
    verb::Engine::RecordedPassData loadedPass;
    if (auto rpTree = st.getChildWithName("RecordedPass"); rpTree.isValid()) {
        const int objs = (int)rpTree.getProperty("objects", 0);
        const int samps = (int)rpTree.getProperty("samples", 0);
        const juce::String b64 = rpTree.getProperty("data", "").toString();
        if (objs >= 2 && objs <= verbp::kMaxPoints + 1 && samps >= 2 && samps <= 4096 && b64.isNotEmpty()) {
            juce::MemoryBlock raw;
            const size_t expectedBytes = (size_t)samps * (size_t)objs * 4u * sizeof(double);
            if (raw.fromBase64Encoding(b64) && raw.getSize() == expectedBytes) {
                std::vector<double> vals((size_t)samps * (size_t)objs * 4u);
                std::memcpy(vals.data(), raw.getData(), expectedBytes);
                bool finiteOk = true;
                for (double x : vals) if (!std::isfinite(x)) { finiteOk = false; break; }
                if (finiteOk) {
                    loadedPass.objects = objs;
                    loadedPass.samples = samps;
                    loadedPass.poses = std::move(vals);
                }
            }
        }
    }
    // P4: optional baked-table child. Absent (old projects), truncated, corrupt or for another format version
    // => ignored, the scene loads exactly as before and reports notBaked. Never throws, never blocks the load.
    std::vector<verb::BakedTable> loadedTables;
    if (auto bc = st.getChildWithName("BakeCache"); bc.isValid()) {
        const juce::String b64 = bc.getProperty("data", "").toString();
        if ((int)bc.getProperty("v", 0) == (int)verb::kBakePersistVersion && b64.isNotEmpty()
            && b64.length() <= (int)(verb::kBakePersistMaxBytes * 2)) {
            juce::MemoryBlock raw;
            if (raw.fromBase64Encoding(b64) && raw.getSize() <= verb::kBakePersistMaxBytes)
                verb::bakeDeserializeTables((const uint8_t*)raw.getData(), raw.getSize(), loadedTables);
        }
    }
    // Cancel pending constructor/default loads before restoring source data.
    { juce::ScopedLock l(reqLock); hasReq=false; defaultsOwed=false; reqSerial.fetch_add(1); }
    {
        juce::ScopedLock l(dispLock);
        savedRecordedPass = std::move(loadedPass);
        savedBakeTables = std::move(loadedTables);
        srcCfg.clear();
        auto srcs = st.getChildWithName("Sources");
        for (int i = 0; i < srcs.getNumChildren() && i < verbp::kMaxSources; ++i) {
            auto v = srcs.getChild(i);
            verbp::SourceCfg c;
            c.name = v.getProperty("name", "Source").toString().toStdString();
            c.color = verbp::normalizeSourceColor(v.getProperty("color", "").toString().toStdString());
            c.format = std::min(std::max((int)v.getProperty("format", 0), 0), (int)verbp::sourceFormats().size() - 1);
            c.channels = std::min(std::max((int)v.getProperty("channels", 1), 1), 16);
            c.firstIn = std::min(std::max((int)v.getProperty("firstIn", 0), 0), verbp::kMaxInputChannels - 1);
            c.sum = (bool)v.getProperty("sum", false);
            c.spread = verb::clampTo((double)v.getProperty("spread", 0.5), 0.0, 20.0, 0.5);
            c.pattern = verb::clampTo((double)v.getProperty("pattern", 0.0), 0.0, 1.0, 0.0);
            if (v.hasProperty("savedPattern"))
                c.pattern = verb::clampTo((double)v.getProperty("savedPattern", c.pattern), 0.0, 1.0, c.pattern);
            c.instrument = v.getProperty("instrument", "none").toString().toStdString();
            if (c.instrument.empty()) c.instrument = "none";
            c.instrumentAmount = verb::clampTo((double)v.getProperty("instrumentAmount", 1.0), 0.0, 1.0, 1.0);
            c.aimAzDeg = verb::clampTo((double)v.getProperty("aimAzDeg", 0.0), -180.0, 180.0, 0.0);
            c.aimElDeg = verb::clampTo((double)v.getProperty("aimElDeg", 0.0), -90.0, 90.0, 0.0);
            c.aimRollDeg = verb::clampTo((double)v.getProperty("aimRollDeg", 0.0), -180.0, 180.0, 0.0);
            c.customDirectivityData = v.getProperty("customDirectivityData", "").toString().toStdString();
            c.customDirectivityName = v.getProperty("customDirectivityName", "").toString().toStdString();
            c.customDirectivityPath = v.getProperty("customDirectivityPath", "").toString().toStdString();
            verbp::resolveSourceDirectivity(c);
            c.freePoints = (bool)v.getProperty("free", false);
            auto split = [](const juce::String& s) {
                std::vector<float> r;
                juce::StringArray a = juce::StringArray::fromTokens(s, ",", "");
                for (auto& t : a)
                    if (t.isNotEmpty() && r.size() < 16) r.push_back((float)verb::clampTo(t.getDoubleValue(), -100.0, 100.0, 0.0));
                return r;
            };
            c.ox = split(v.getProperty("ox").toString());
            c.oy = split(v.getProperty("oy").toString());
            c.oz = split(v.getProperty("oz").toString());
            srcCfg.push_back(c);
        }
        auto out = st.getChildWithName("Output");
        outCfg.layout = out.isValid() ? (int)out.getProperty("layout", (int)verb::Layout::StereoORTF) : (int)verb::Layout::StereoORTF;
        outCfg.layout = std::min(std::max(outCfg.layout, -1), (int)verb::Layout::AmbiX3);  // an unknown layout id must not reach the engine
        outCfg.radius = verb::clampTo(out.isValid() ? (double)out.getProperty("radius", 0.1) : 0.1, 0.0, 1.0, 0.1);
        outCfg.micPattern = std::min(std::max(out.isValid() ? (int)out.getProperty("micPattern", 0) : 0, 0), (int)verb::MicPattern::HumanEars);
        outCfg.ambisonicsNormalization = out.isValid() ? (int)out.getProperty("ambisonicsNormalization", 0) : 0;
        outCfg.ambisonicsLevelDb = out.isValid() ? (int)out.getProperty("ambisonicsLevelDb", 0) : 0;
        publishAmbisonicsFormat();
        tailSettings.rays = std::min(std::max((int)st.getProperty("tailRays", 48000), 8000), 384000);
        tailSettings.adaptive = (bool)st.getProperty("adaptiveTail", false);
        syncPatterns();
    }
    inputMetersOn = (bool)st.getProperty("inputMeters", true);
    keepRouting = (bool)st.getProperty("keepRouting", false);
    keepProcessing = (bool)st.getProperty("keepProcessing", false);
    mixerCompact = (bool)st.getProperty("mixerCompact", true);
    navLocked = (bool)st.getProperty("navLocked", false);
    showOutliner = (bool)st.getProperty("showOutliner", false);
    if (!st.getChildWithProperty("id", "spatialDry").isValid()) {
        juce::ValueTree param("PARAM"); param.setProperty("id", "spatialDry", nullptr);
        param.setProperty("value", 0.f, nullptr); st.appendChild(param, nullptr);  // old projects retain raw bypass
    }
    for (int s = 0; s < verbp::kMaxSources; ++s) for (bool solo : {false, true}) {
        const auto id = verbp::sourceSwitchId(s, solo);
        if (!st.getChildWithProperty("id", id).isValid()) {
            juce::ValueTree param("PARAM"); param.setProperty("id", id, nullptr); param.setProperty("value", 0.f, nullptr); st.appendChild(param, nullptr);
        }
    }
    for (const char* id : {"soloDirect", "soloEarly", "soloTail"}) {
        if (!st.getChildWithProperty("id", id).isValid()) {
            juce::ValueTree p("PARAM"); p.setProperty("id", id, nullptr); p.setProperty("value", 0.f, nullptr);
            st.appendChild(p, nullptr);
        }
    }
    if (!st.getChildWithProperty("id", "mode").isValid() && !st.getChildWithProperty("id", "early").isValid()) {
        juce::ValueTree p("PARAM");p.setProperty("id","early",nullptr);p.setProperty("value",-9.f,nullptr);st.appendChild(p,nullptr);
    }
    // Missing stage-2 controls MUST NOT inherit the previous room's overrides.
    for (const char* id : {"mode", "absorption", "decayLow", "decayMid", "decayHigh"}) {
        if (!st.getChildWithProperty("id", id).isValid()) {
            juce::ValueTree p("PARAM"); p.setProperty("id", id, nullptr);
            p.setProperty("value", 1.f, nullptr); st.appendChild(p, nullptr); // legacy => Creative; neutral scale/decays
        }
    }
    for(const auto& entry:std::initializer_list<std::pair<const char*,float>>{{"size",100},{"predelay",0},{"stereoWidth",100},{"freeze",0},{"duck",0},{"duckRelease",300},{"makeup",0}}) {
        auto p=st.getChildWithProperty("id",entry.first);
        if(!p.isValid()){p=juce::ValueTree("PARAM");p.setProperty("id",entry.first,nullptr);st.appendChild(p,nullptr);p.setProperty("value",entry.second,nullptr);}
        if(juce::String(entry.first)=="freeze")p.setProperty("value",0.f,nullptr);
    }
    // Invalidate a finishing geometry request BEFORE replacing parameter state.
    // Geometry imports never write mode; only this state migration selects legacy Creative.
    for(int o=0;o<=verbp::kMaxSources;++o) {
        verbp::MovementPath path;
        const auto v=st.getChildWithName("MovementPaths").getChildWithProperty("object",o);
        if(v.isValid()) {
            path.enabled=(bool)v.getProperty("enabled",true);path.closed=(bool)v.getProperty("closed",false);
            path.smooth=(bool)v.getProperty("smooth",false);path.orient=(bool)v.getProperty("orient",false);path.laps=std::clamp((int)v.getProperty("laps",1),1,64);
            path.group=std::clamp((int)v.getProperty("group",-1),-1,31);
            auto coordinate=[](const juce::var& value){if(!value.isString())return (double)value;std::istringstream number(value.toString().toStdString());number.imbue(std::locale::classic());double result=std::numeric_limits<double>::quiet_NaN();number>>result;return result;};
            if((bool)v.getProperty("lookAt",false)){
                path.lookAt=true;path.orient=false;
                if((bool)v.getProperty("lookPoint",false)){path.lookPoint=true;path.lookPos={coordinate(v.getProperty("lookX",0.0)),coordinate(v.getProperty("lookY",0.0)),coordinate(v.getProperty("lookZ",0.0))};}
                else path.lookTarget=(int)v.getProperty("lookTarget",-1);
            }
            if(v.hasProperty("orientOffset"))path.orientOffset=coordinate(v.getProperty("orientOffset"));
            if(v.hasProperty("start"))path.start=coordinate(v.getProperty("start"));
            if(v.hasProperty("end"))path.end=coordinate(v.getProperty("end"));
            std::vector<std::optional<double>> loadedTimes;
            std::vector<verbp::PathEasing> loadedEasings;
            bool anyTime=false,anyEase=false;
            for(auto pt:v) {
                if(pt.hasType("Point")){
                    if(path.points.size()>64)continue;
                    path.points.push_back({coordinate(pt.getProperty("x")),coordinate(pt.getProperty("y")),coordinate(pt.getProperty("z"))});
                    if(pt.hasProperty("time")){anyTime=true;loadedTimes.push_back(coordinate(pt.getProperty("time")));}
                    else loadedTimes.emplace_back();
                    auto ease=verbp::PathEasing::Linear;
                    if(pt.hasProperty("easing")){
                        anyEase=true;
                        const auto es=pt.getProperty("easing").toString();
                        if(es=="linear")ease=verbp::PathEasing::Linear;
                        else if(es=="easeIn")ease=verbp::PathEasing::EaseIn;
                        else if(es=="easeOut")ease=verbp::PathEasing::EaseOut;
                        else if(es=="easeInOut")ease=verbp::PathEasing::EaseInOut;
                        else ease=(verbp::PathEasing)255;
                    }
                    loadedEasings.push_back(ease);
                }else if(pt.hasType("AzKey")){
                    if(path.azKeys.size()>verbp::kMaxAzKeys)continue;
                    verbp::AzimuthKey ak;
                    ak.t=coordinate(pt.getProperty("t"));
                    ak.azDeg=coordinate(pt.getProperty("az"));
                    if(pt.hasProperty("easing")){
                        const auto es=pt.getProperty("easing").toString();
                        if(es=="linear")ak.easing=verbp::AzimuthEasing::Linear;
                        else if(es=="easeIn")ak.easing=verbp::AzimuthEasing::EaseIn;
                        else if(es=="easeOut")ak.easing=verbp::AzimuthEasing::EaseOut;
                        else if(es=="easeInOut")ak.easing=verbp::AzimuthEasing::EaseInOut;
                        else if(es=="hold")ak.easing=verbp::AzimuthEasing::Hold;
                        else ak.easing=(verbp::AzimuthEasing)255;
                    }
                    path.azKeys.push_back(ak);
                }else if(pt.hasType("Lfo")){
                    path.lfo.enabled=(bool)pt.getProperty("enabled",false);
                    if(pt.hasProperty("shape")){
                        verbp::LfoShape sh{};
                        if(verbp::parseLfoShape(pt.getProperty("shape").toString().toRawUTF8(),sh))path.lfo.shape=sh;
                        else path.lfo.shape=(verbp::LfoShape)255;
                    }
                    if(pt.hasProperty("rateHz"))path.lfo.rateHz=coordinate(pt.getProperty("rateHz"));
                    if(pt.hasProperty("depth"))path.lfo.depth=coordinate(pt.getProperty("depth"));
                    if(pt.hasProperty("phaseDeg"))path.lfo.phaseDeg=coordinate(pt.getProperty("phaseDeg"));
                    if(pt.hasProperty("seed"))path.lfo.seed=(uint32_t)std::max(0,(int)pt.getProperty("seed",1));
                }
            }
            if(anyTime)path.times=std::move(loadedTimes);
            if(anyEase)path.easings=std::move(loadedEasings);
        }
        if(!verbp::validPath(path))path={};
        else {verbp::normalizePathTiming(path);verbp::normalizePathAzKeys(path);}
        setMovementPath(o,path);
        if(!st.getChildWithProperty("id",pathParamId(o)).isValid()) {juce::ValueTree p("PARAM");p.setProperty("id",pathParamId(o),nullptr);p.setProperty("value",0.f,nullptr);st.appendChild(p,nullptr);}
    }
    if (const auto modTree = st.getChildWithName("Modulation"); modTree.isValid())
        setModulationRoutes(modRoutesFromValueTree(modTree));
    else
        setModulationRoutes({});
    apvts.replaceState(st);
    if (!isCreativeMode() || modConfiguredCount.load(std::memory_order_acquire) == 0) {
        modProc->reset();
        for (int s = 0; s < verbp::kMaxModHostOuts; ++s)
            if (modOutParams[(size_t)s])
                modOutParams[(size_t)s]->resetToNeutralIfChanged();
    }
    if (prevRange != loadedRange) {
        for (int o = 0; o <= verbp::kMaxSources; ++o)
            for (int k = 0; k < 3; ++k)
                if (auto* p = apvts.getParameter(verbp::paramId(o, k)))
                    p->sendValueChangedMessageToListeners(p->getValue());
        updateHostDisplay(ChangeDetails().withParameterInfoChanged(true).withNonParameterStateChanged(true));
    }
    const juce::String text = st.getProperty("sceneText").toString(), name = st.getProperty("sceneName").toString();
    if (text.isNotEmpty()) loadSceneText(text, name, false);
}

// ---------- scene loading ----------
void VerbProcessor::loadRoomWithSetup(const juce::String& text, const juce::String& name, const std::vector<verbp::SourceCfg>& setup, int micPattern) {
    {
        juce::ScopedLock l(reqLock);
        nextSetup = setup;
        if (nextSetup.size() > (size_t)verbp::kMaxSources) nextSetup.resize((size_t)verbp::kMaxSources);
        nextMic = micPattern;
    }
    loadSceneText(text, name, true);
}

void VerbProcessor::loadSceneText(const juce::String& text, const juce::String& name, bool applyDefaults, bool keepPendingDefaults) {
    if(applyDefaults) {
        // Only explicit room/geometry loads own per-room overrides. State/preset
        // restores call this with false and retain their complete saved values.
        verbp::AutomationRange nextRange;
        bool parsedOk = false;
        try { std::istringstream check(text.toStdString()); const auto parsed = verb::parseScene(check,name.toStdString());
            parsedOk = true;
            if (parsed.hasAutomationRange) {
                nextRange.xMin = parsed.automationRange[0]; nextRange.xMax = parsed.automationRange[1];
                nextRange.yMin = parsed.automationRange[2]; nextRange.yMax = parsed.automationRange[3];
                nextRange.zMin = parsed.automationRange[4]; nextRange.zMax = parsed.automationRange[5];
                nextRange = verbp::AutomationRange::sanitised(nextRange);
            }
            cancelTargetRT60();
            for(const char* id:{"absorption","decayLow","decayMid","decayHigh"})setParamValue(id,1.);
        } catch(const std::exception&) { /* loader reports invalid scene without changing knobs */ }
        if (parsedOk) setAutomationRange(nextRange, false, true);
        for(int o=0;o<=verbp::kMaxSources;++o)setMovementPath(o,{});
        setModulationRoutes({});
        { juce::ScopedLock l(dispLock); savedRecordedPass = {}; }
    }
    queueScene(text, name, applyDefaults, keepPendingDefaults, false, 0);
}

// `isRebuild`: the same room again (new engine for new sources / output / sample rate). Edits of the display scene made
// after `editsAtRequest` (the count of edits when the text was read) must then survive the load.
void VerbProcessor::queueScene(const juce::String& text, const juce::String& name, bool applyDefaults, bool keepPendingDefaults, bool isRebuild, uint64_t editsAtRequest) {
    uint64_t gen;
    {
        juce::ScopedLock l(dispLock);
        if (!isRebuild) { ++roomGen; ++structRevision; }  // a new room: the undo history belongs to the old one
        gen = roomGen;
    }
    if (!isRebuild && session) session->notifyDocumentReplaced();
    {
        juce::ScopedLock l(reqLock);
        req.rebuild = isRebuild;
        req.editsAtRequest = editsAtRequest;
        req.gen = gen;
        req.text = text.toStdString();
        req.name = name.toStdString();
        req.setup = nextSetup;
        req.micPattern = nextMic;
        req.keepRouting = keepRouting.load();
        nextSetup.clear();
        nextMic = -1;
        req.sr = hostSr;
        if (applyDefaults) defaultsOwed = true;
        else if (!keepPendingDefaults) defaultsOwed = false;  // an explicit restore cancels pending defaults
        reqSerial.fetch_add(1);
        hasReq = true;
    }
    state = 1;
    {
        juce::ScopedLock l(dispLock);
        sceneText = text;
        sceneName = name;
        status = "Loading \"" + name + "\" ...";
    }
    wake.signal();
}

void VerbProcessor::rebuild() {
    std::shared_ptr<verb::Engine> eng;
    { juce::SpinLock::ScopedLockType el(engineLock); if (bundle) eng = bundle->engine; }
    juce::String t, n;
    uint64_t edits;
    {
        juce::ScopedLock l(dispLock);
        if (!roomLoadPendingLocked() && eng) {
            auto pass = eng->exportRecordedPass();
            if (pass.samples >= 2) savedRecordedPass = std::move(pass);
        }
        t = sceneText;
        n = sceneName;
        edits = displayEdits;
    }
    queueScene(t, n, false, true, true, edits);
}

void VerbProcessor::setParamValue(const juce::String& id, double v) {
    if (auto* p = apvts.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1((float)v));
}

void VerbProcessor::applyDefaultsFrom(const verb::Scene& sc, const double* home) {
    for (int s = 0; s < verbp::kMaxSources && s < (int)sc.sources.size(); ++s) {
        setParamValue(verbp::paramId(s, 0), sc.sources[(size_t)s].pos.x);
        setParamValue(verbp::paramId(s, 1), sc.sources[(size_t)s].pos.y);
        setParamValue(verbp::paramId(s, 2), sc.sources[(size_t)s].pos.z);
        setParamValue(verbp::paramId(s, 3), sc.sources[(size_t)s].azimuthDeg);
        setParamValue(verbp::paramId(s, 4), 0.0);
    }
    for (int k = 0; k < 4; ++k) setParamValue(verbp::paramId(verbp::kListener, k), home[k]);
}

void VerbProcessor::updateBounds(const verb::Scene& sc) {
    sceneHeight = sc.height;
    ceilOpen = sc.ceilMat < 0;
    {
        auto ws = std::make_unique<verbp::WallSnapshot>();
        ws->count = std::min((int)sc.walls.size(), verbp::kMaxWallSnapshotSegs);
        for (int i = 0; i < ws->count; ++i) {
            const auto& w = sc.walls[(size_t)i];
            ws->segs[(size_t)i] = {w.ax, w.ay, w.bx, w.by, w.z0, w.z1};
        }
        juce::ScopedLock wl(wallMailLock);
        wallMailbox->publish(*ws);
    }
    if (!sc.walls.empty()) {  // suggested slider range: bounding box plus a generous margin (not a limit)
        double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
        for (auto& w : sc.walls) {
            x0 = std::min({x0, w.ax, w.bx}); x1 = std::max({x1, w.ax, w.bx});
            y0 = std::min({y0, w.ay, w.by}); y1 = std::max({y1, w.ay, w.by});
        }
        const double margin = 20.0;
        bounds[0] = x0 - margin; bounds[1] = x1 + margin; bounds[2] = y0 - margin; bounds[3] = y1 + margin;
        hasBounds = true;
    } else {
        hasBounds = false;
    }
}

bool VerbProcessor::isRoomLoadPending() const {
    juce::ScopedLock l(dispLock);
    return roomLoadPendingLocked();
}

bool VerbProcessor::editScene(const std::function<void(verb::Scene&)>& fn) {
    {
        juce::ScopedLock l(dispLock);
        if (roomLoadPendingLocked()) return false;  // the pending room wins; `display` is about to be replaced
        verb::Scene tmp = display;
        fn(tmp);
        verb::sanitizeScene(tmp);  // e.g. an endpoint dragged onto the other one would leave a zero-length wall
        verb::compactMaterials(tmp);
        if ((int)tmp.walls.size() > 128 || tmp.floorMat < 0 || tmp.floorMat >= (int)tmp.mats.size() || tmp.ceilMat >= (int)tmp.mats.size()) return false;
        for (auto& w : tmp.walls)
            if (w.mat < 0 || w.mat >= (int)tmp.mats.size()) return false;
        tmp.height = std::min(std::max(tmp.height, 1.0), 30.0);
        display = tmp;
        ++displayEdits;
        sceneText = verb::serializeScene(display, 6, std::numeric_limits<double>::max_digits10);
        updateBounds(display);
        ++sceneSerial; ++geometryEpoch; ++structRevision;
    }
    geomDirty = true;
    wake.signal();
    return true;
}

// ---------- sources / outputs ----------
bool VerbProcessor::editSceneMetadata(const std::function<void(verb::Scene&)>& fn) {
    {
        juce::ScopedLock lock(dispLock);
        if (roomLoadPendingLocked()) return false;  // see editScene
        fn(display);
        ++displayEdits;
        sceneText = verb::serializeScene(display, 6, std::numeric_limits<double>::max_digits10);
        ++sceneSerial; ++structRevision;
    }
    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
    return true;
}

std::array<int, 4> VerbProcessor::getLayerModes() const {
    juce::ScopedLock l(dispLock);
    return display.layerModes;
}

void VerbProcessor::setLayerMode(int layer, int mode) {
    if (layer < 0 || layer > 3) return;
    mode = std::min(std::max(mode, 0), 2);
    editSceneMetadata([layer, mode](verb::Scene& s) { s.layerModes[(size_t)layer] = mode; });
}

std::vector<verbp::SourceCfg> VerbProcessor::getSources() const {
    juce::ScopedLock l(dispLock);
    return srcCfg;
}

void VerbProcessor::setSources(const std::vector<verbp::SourceCfg>& cfg) {
    {
        juce::ScopedLock l(dispLock);
        srcCfg = cfg;
        for (auto& c : srcCfg) {
            c.color = verbp::normalizeSourceColor(c.color);
            c.format = std::min(std::max(c.format, 0), (int)verbp::sourceFormats().size() - 1);
            c.channels = std::min(std::max(c.channels, 1), 16);
            c.firstIn = std::min(std::max(c.firstIn, 0), verbp::kMaxInputChannels - 1);
            c.spread = std::min(std::max(c.spread, 0.0), 20.0);
            c.pattern = std::min(std::max(c.pattern, 0.0), 1.0);
        }
        syncPatterns();
        ++sceneSerial; ++structRevision;
    }
    rebuild();
}

int VerbProcessor::addSource() {
    std::vector<verbp::SourceCfg> v = getSources();
    if ((int)v.size() >= verbp::kMaxSources) return -1;
    verbp::SourceCfg c;
    c.name = "Source " + std::to_string(v.size() + 1);
    int next = 0;  // first input channel after the ones already used
    for (auto& s : v) next = std::max(next, s.firstIn + s.effectiveChannels());
    c.firstIn = std::min(next, verbp::kMaxInputChannels - 1);
    v.push_back(c);
    const int idx = (int)v.size() - 1;
    const verb::Scene sc = getDisplayScene();
    double cx = 4, cy = 3;
    if (!sc.walls.empty()) {
        double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
        for (auto& w : sc.walls) { x0 = std::min({x0, w.ax, w.bx}); x1 = std::max({x1, w.ax, w.bx}); y0 = std::min({y0, w.ay, w.by}); y1 = std::max({y1, w.ay, w.by}); }
        cx = (x0 + x1) / 2; cy = (y0 + y1) / 2;
    }
    setParamValue(verbp::paramId(idx, 0), cx - 1.0 + 0.4 * (idx % 5));
    setParamValue(verbp::paramId(idx, 1), cy + 0.4 * (idx / 5));
    setParamValue(verbp::paramId(idx, 2), 1.5);
    setParamValue(verbp::paramId(idx, 3), 0.0);
    setParamValue(verbp::paramId(idx, 4), 0.0);
    setParamValue(verbp::sourceSwitchId(idx, false), 0.0);
    setParamValue(verbp::sourceSwitchId(idx, true), 0.0);
    setSources(v);
    return idx;
}

void VerbProcessor::removeSource(int index) {
    std::vector<verbp::SourceCfg> v = getSources();
    if (v.size() <= 1 || index < 0 || index >= (int)v.size()) return;
    for (int i = index; i + 1 < (int)v.size(); ++i) {  // later sources move up with their positions and gains
        for (int k = 0; k < 5; ++k) setParamValue(verbp::paramId(i, k), pv[i + 1][k]->load());
        setParamValue(verbp::sourceSwitchId(i, false), sourceMute[i + 1]->load());
        setParamValue(verbp::sourceSwitchId(i, true), sourceSolo[i + 1]->load());
    }
    v.erase(v.begin() + index);
    {
        auto routes = getModulationRoutes();
        verbp::remapModRoutesOnSourceRemove(routes, index);
        juce::ScopedLock ml(modLock);
        modRoutes = std::move(routes);
        modMailbox->publish(modRoutes.data(), (int)modRoutes.size());
        modConfiguredCount.store((int)modRoutes.size(), std::memory_order_release);
    }
    setSources(v);
}

void VerbProcessor::syncPatterns() {  // dispLock must be held
    for (int s = 0; s < verbp::kMaxSources; ++s) {
        if (s < (int)srcCfg.size()) {
            verbp::resolveSourceDirectivity(srcCfg[(size_t)s]);
            srcPattern[s].store((float)verbp::effectiveAnalyticPattern(srcCfg[(size_t)s]));
            std::atomic_store(&srcDirectivity[s], srcCfg[(size_t)s].directivity);
            srcInstrumentAmount[s].store((float)srcCfg[(size_t)s].instrumentAmount);
            srcAimAz[s].store((float)srcCfg[(size_t)s].aimAzDeg);
            srcAimEl[s].store((float)srcCfg[(size_t)s].aimElDeg);
            srcAimRoll[s].store((float)srcCfg[(size_t)s].aimRollDeg);
        } else {
            srcPattern[s].store(0.f);
            std::atomic_store(&srcDirectivity[s], std::shared_ptr<const verb::DirectivityProfile>{});
            srcInstrumentAmount[s].store(1.f);
            srcAimAz[s].store(0.f);
            srcAimEl[s].store(0.f);
            srcAimRoll[s].store(0.f);
        }
    }
    srcDirectivityEpoch.fetch_add(1, std::memory_order_release);
}

void VerbProcessor::setSourcePattern(int index, double pattern) {
    pattern = verb::clampTo(pattern, 0.0, 1.0, 0.0);
    juce::ScopedLock l(dispLock);
    if (index < 0 || index >= (int)srcCfg.size()) return;
    srcCfg[(size_t)index].pattern = pattern;
    srcPattern[index].store((float)verbp::effectiveAnalyticPattern(srcCfg[(size_t)index]));
    srcDirectivityEpoch.fetch_add(1, std::memory_order_release); wake.signal();
    ++structRevision;  // not part of sceneSerial; the command revision must cover it
}

void VerbProcessor::setSourceDirectivity(int index, const std::string& instrument, double amount,
                                         const std::string& customData, const std::string& customName,
                                         const std::string& customPath,
                                         std::optional<double> aimAzDeg,
                                         std::optional<double> aimElDeg,
                                         std::optional<double> aimRollDeg) {
    juce::ScopedLock l(dispLock);
    if (index < 0 || index >= (int)srcCfg.size()) return;
    auto& c = srcCfg[(size_t)index];
    c.instrument = instrument.empty() ? "none" : instrument;
    c.instrumentAmount = verb::clampTo(amount, 0.0, 1.0, 1.0);
    if (c.instrument == "custom") {
        c.customDirectivityData = customData;
        c.customDirectivityName = customName;
        c.customDirectivityPath = customPath;
    } else if (c.instrument == "none") {
        c.customDirectivityData.clear();
        c.customDirectivityName.clear();
        c.customDirectivityPath.clear();
        c.aimAzDeg = 0.0;
        c.aimElDeg = 0.0;
        c.aimRollDeg = 0.0;
    }
    if (c.instrument != "none") {
        if (aimAzDeg) c.aimAzDeg = verb::clampTo(*aimAzDeg, -180.0, 180.0, 0.0);
        if (aimElDeg) c.aimElDeg = verb::clampTo(*aimElDeg, -90.0, 90.0, 0.0);
        if (aimRollDeg) c.aimRollDeg = verb::clampTo(*aimRollDeg, -180.0, 180.0, 0.0);
    }
    verbp::resolveSourceDirectivity(c);
    srcPattern[index].store((float)verbp::effectiveAnalyticPattern(c));
    std::atomic_store(&srcDirectivity[index], c.directivity);
    srcInstrumentAmount[index].store((float)c.instrumentAmount);
    srcAimAz[index].store((float)c.aimAzDeg);
    srcAimEl[index].store((float)c.aimElDeg);
    srcAimRoll[index].store((float)c.aimRollDeg);
    srcDirectivityEpoch.fetch_add(1, std::memory_order_release); wake.signal();
    ++structRevision;
}

void VerbProcessor::setSourceSpread(int index, double radius) {
    radius = std::min(std::max(radius, 0.0), 20.0);
    juce::ScopedLock l(dispLock);
    if (index < 0 || index >= (int)srcCfg.size()) return;
    srcCfg[(size_t)index].spread = radius;
    ++structRevision;
    if (srcCfg[(size_t)index].freePoints) return;  // free points keep their own offsets
    for (size_t p = 0; p < livePoints.size(); ++p)
        if (livePoints[p].source == index && livePoints[p].radius > 0) {
            const double a = livePoints[p].angleDeg * verb::kPi / 180.0;
            ptOff[p][0] = (float)(radius * std::cos(a));
            ptOff[p][1] = (float)(radius * std::sin(a));
        }
}

void VerbProcessor::setSourceColor(int index, const std::string& color) {
    const std::string norm = verbp::normalizeSourceColor(color);
    bool changed = false;
    {
        juce::ScopedLock l(dispLock);
        if (index < 0 || index >= (int)srcCfg.size()) return;
        if (srcCfg[(size_t)index].color != norm) {
            srcCfg[(size_t)index].color = norm;
            ++structRevision;
            changed = true;
        }
    }
    if (changed) updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
}

void VerbProcessor::orientObject(int obj, double x, double y) {
    if (obj < 0 || obj > verbp::kListener) return;
    const double cx = pv[obj][0]->load(), cy = pv[obj][1]->load();
    if (std::hypot(x - cx, y - cy) < 1e-6) return;
    setParamValue(verbp::paramId(obj, 3), std::atan2(y - cy, x - cx) * 180.0 / verb::kPi);
}

void VerbProcessor::scaleWall(int wall, double f) {
    f = std::min(std::max(f, 0.05), 20.0);
    editScene([wall, f](verb::Scene& s) {
        if (wall < 0 || wall >= (int)s.walls.size()) return;
        verb::Wall& w = s.walls[(size_t)wall];
        const double mx = 0.5 * (w.ax + w.bx), my = 0.5 * (w.ay + w.by);
        w.ax = mx + (w.ax - mx) * f; w.ay = my + (w.ay - my) * f;
        w.bx = mx + (w.bx - mx) * f; w.by = my + (w.by - my) * f;
    });
}

void VerbProcessor::scaleRoom(double f, bool includeObjects) {
    f = std::min(std::max(f, 0.05), 20.0);
    double cx = 0, cy = 0;
    {
        juce::ScopedLock l(dispLock);
        if (display.walls.empty()) return;
        double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
        for (auto& w : display.walls) { x0 = std::min({x0, w.ax, w.bx}); x1 = std::max({x1, w.ax, w.bx}); y0 = std::min({y0, w.ay, w.by}); y1 = std::max({y1, w.ay, w.by}); }
        cx = 0.5 * (x0 + x1); cy = 0.5 * (y0 + y1);
    }
    editScene([f, cx, cy, includeObjects](verb::Scene& s) {
        verb::scaleRoomGeometry(s,f,cx,cy,includeObjects);
        if(includeObjects){
            s.height=std::clamp(s.height,1.,30.);
            for(auto* zones:{&s.floorZones,&s.ceilZones})for(auto& z:*zones)if(z.z>=0)z.z=std::min(z.z,s.height);
        }
        // Not scaled on purpose: clutter objects (their lengths, heights and clearance are physical sizes of furniture; their
        // positions are drawn from the seeded generator inside the scaled walls), the scene's own source / receiver / listener
        // poses (the live ones are the plugin parameters, scaled below).
    });
    const auto curRange = getAutomationRange();
    if (!curRange.isDefault()) {
        auto nextRange = curRange.scaled(cx, cy, f, includeObjects);
        if (!includeObjects) {
            const int ns = (int)getSources().size();
            for (int o = 0; o <= ns; ++o) {
                const int obj = o == ns ? verbp::kListener : o;
                nextRange.expandPoint(pv[obj][0]->load(), pv[obj][1]->load(), pv[obj][2]->load());
            }
        }
        setAutomationRange(nextRange, false, true);
    }
    if (includeObjects) {
        const int ns = (int)getSources().size();
        for (int o = 0; o <= ns; ++o) {
            const int obj = o == ns ? verbp::kListener : o;
            setParamValue(verbp::paramId(obj, 0), cx + (pv[obj][0]->load() - cx) * f);
            setParamValue(verbp::paramId(obj, 1), cy + (pv[obj][1]->load() - cy) * f);
            setParamValue(verbp::paramId(obj, 2), pv[obj][2]->load() * f);
        }
    }
}

verbp::AutomationRange VerbProcessor::getAutomationRange() const {
    juce::ScopedLock l(dispLock);
    return automationRange;
}

void VerbProcessor::setAutomationRange(const verbp::AutomationRange& rangeRaw, bool clampObjects, bool notifyHost) {
    const auto next = verbp::AutomationRange::sanitised(rangeRaw);
    verbp::AutomationRange prev;
    bool changed = false;
    {
        juce::ScopedLock l(dispLock);
        prev = automationRange;
        changed = (prev != next);
        automationRange = next;
    }
    axisStores[0]->setRange((float)next.xMin, (float)next.xMax);
    axisStores[1]->setRange((float)next.yMin, (float)next.yMax);
    axisStores[2]->setRange((float)next.zMin, (float)next.zMax);

    for (int o = 0; o <= verbp::kMaxSources; ++o) {
        for (int k = 0; k < 3; ++k) {
            const float cur = pv[o][k]->load();
            const float target = clampObjects ? (float)next.clampAxis(k, (double)cur) : cur;
            const bool clamped = clampObjects && std::abs(target - cur) > 1e-6f;
            if (clamped) {
                if (auto* p = apvts.getParameter(verbp::paramId(o, k)))
                    p->setValueNotifyingHost(p->convertTo0to1(target));
            } else if (notifyHost && changed) {
                if (auto* p = apvts.getParameter(verbp::paramId(o, k))) {
                    p->sendValueChangedMessageToListeners(p->convertTo0to1(cur));
                    // Restore exact unnormalised metre value: ParameterAdapter's
                    // denormalise(convertTo0to1(cur)) can drift by 1-2 ULPs on non-power-of-two ranges.
                    if (auto* raw = apvts.getRawParameterValue(verbp::paramId(o, k)))
                        raw->store(cur, std::memory_order_relaxed);
                }
            }
        }
    }
    if (changed) {
        bumpRevision();
        if (notifyHost)
            updateHostDisplay(ChangeDetails().withParameterInfoChanged(true).withNonParameterStateChanged(true));
    }
}

verbp::AutomationRange VerbProcessor::computeFitAutomationRange(double marginM) const {
    verb::Scene sc;
    std::vector<verbp::SourceCfg> sources;
    verbp::OutputCfg output;
    {
        juce::ScopedLock l(dispLock);
        sc = display;
        sources = srcCfg;
        output = outCfg;
    }
    std::array<verbp::MovementPath, verbp::kMaxSources + 1> allPaths;
    {
        juce::ScopedLock l(pathLock);
        allPaths = movementPaths;
    }
    auto readParam = [this](const char* id) -> double {
        if (auto* p = apvts.getRawParameterValue(id)) return (double)p->load();
        return 0.0;
    };
    return verbp::computeFitAutomationRange(sc, sources, output.layout, output.radius, allPaths, readParam, marginM);
}

verbp::AutomationRange VerbProcessor::fitAutomationRangeToScene(bool clampObjects, bool notifyHost, double marginM) {
    const auto fitted = computeFitAutomationRange(marginM);
    setAutomationRange(fitted, clampObjects, notifyHost);
    return fitted;
}

int VerbProcessor::countObjectsOutsideRange(const verbp::AutomationRange& range) const {
    const int nSrc = (int)getSources().size();
    auto readParam = [this](const char* id) -> double {
        if (auto* p = apvts.getRawParameterValue(id)) return (double)p->load();
        return 0.0;
    };
    return verbp::countObjectsOutsideRange(range, nSrc, readParam);
}

void VerbProcessor::setPointOffset(int source, int channel, float ox, float oy, float oz) {
    juce::ScopedLock l(dispLock);
    if (source < 0 || source >= (int)srcCfg.size()) return;
    verbp::SourceCfg& c = srcCfg[(size_t)source];
    const size_t n = (size_t)c.effectiveChannels();
    if (c.ox.size() != n || c.oy.size() != n || c.oz.size() != n) verbp::freezeLayout(c);
    if (channel < 0 || channel >= (int)n) return;
    c.ox[(size_t)channel] = ox; c.oy[(size_t)channel] = oy; c.oz[(size_t)channel] = oz;
    ++structRevision;
    for (size_t p = 0; p < livePoints.size(); ++p)
        if (livePoints[p].source == source && livePoints[p].channel == channel) { ptOff[p][0] = ox; ptOff[p][1] = oy; ptOff[p][2] = oz; }
}

std::vector<verbp::PointMap> VerbProcessor::getLivePoints() const {
    juce::ScopedLock l(dispLock);
    return livePoints;
}

verbp::OutputCfg VerbProcessor::getOutputCfg() const {
    juce::ScopedLock l(dispLock);
    return outCfg;
}

void VerbProcessor::publishAmbisonicsFormat() {
    auto& normalization = outCfg.ambisonicsNormalization;
    if (normalization < 0 || normalization > 2) normalization = 0;
    auto& levelDb = outCfg.ambisonicsLevelDb;
    if (levelDb != -3 && levelDb != -6) levelDb = 0;
    ambisonicsFormat.store(verbp::AmbisonicsOutput::encode(normalization, levelDb), std::memory_order_relaxed);
}

void VerbProcessor::setOutputCfg(const verbp::OutputCfg& o) {
    bool rebuildReceivers;
    {
        juce::ScopedLock l(dispLock);
        const auto previous = outCfg;
        outCfg = o;
        outCfg.layout = std::min(std::max(outCfg.layout, -1), (int)verb::Layout::AmbiX3);
        outCfg.radius = verb::clampTo(outCfg.radius, 0.0, 1.0, 0.1);
        outCfg.micPattern = std::min(std::max(outCfg.micPattern, 0), (int)verb::MicPattern::HumanEars);
        publishAmbisonicsFormat();
        if (outCfg == previous) return;
        rebuildReceivers = !outCfg.sameReceivers(previous);
        if (rebuildReceivers) ++sceneSerial;
        ++structRevision;
    }
    if (rebuildReceivers) rebuild();
}

verbp::TailSettings VerbProcessor::getTailSettings() const {
    juce::ScopedLock l(dispLock);
    return tailSettings;
}

void VerbProcessor::setTailSettings(verbp::TailSettings settings) {
    settings.rays = std::min(std::max(settings.rays, 8000), 384000);
    {
        juce::ScopedLock l(dispLock);
        if (settings.rays == tailSettings.rays && settings.adaptive == tailSettings.adaptive) return;
        tailSettings = settings;
        ++sceneSerial; ++structRevision;
    }
    qualityDirty = true;
    wake.signal();
    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
}

std::shared_ptr<const verb::TailDiagnostics> VerbProcessor::getTailDiagnostics() const {
    juce::SpinLock::ScopedLockType lock(engineLock);
    return bundle ? bundle->engine->tailDiagnostics() : nullptr;
}

bool VerbProcessor::isTailCurrent() const {
    juce::SpinLock::ScopedLockType lock(engineLock);
    return state.load()==2 && !geomDirty.load() && !qualityDirty.load() && geometryEpoch.load()==appliedGeometryEpoch.load()
        && engineAbsorption.load()==absorptionParam->load() && engineSize.load()==sizeParam->load() && bundle && bundle->engine->tailIsCurrent();
}

verb::Scene VerbProcessor::targetScene() const {
    auto s = getDisplayScene(); // original, unscaled material data
    std::shared_ptr<const verb::Scene> modeled;
    {juce::SpinLock::ScopedLockType lock(engineLock);if(bundle)modeled=bundle->engine->sceneSnapshot();}
    // Reproduce the engine's ORIGINAL seeded placement anchors before substituting live
    // excitation. Moving a source must not regenerate a different set of furniture for
    // the assistant. Reconstruct unscaled roughened materials to avoid undoing a clamp.
    const double sf=engineSize.load()*.01;
    verb::scaleRoomGeometry(s,sf);
    if (modeled) {s.sources=modeled->sources;s.receivers=modeled->receivers;}
    s=verb::withClutter(verb::withRoughness(s),128);s.clutter.count=0;s.roughness=0;
    if (!s.sources.empty()) {
        s.sources.resize(1);
        const double az=pv[0][3]->load()*verb::kPi/180.;
        const double ox=ptOff[0][0].load(),oy=ptOff[0][1].load(),oz=ptOff[0][2].load();
        s.sources[0].pos={pv[0][0]->load()+std::cos(az)*ox-std::sin(az)*oy,
            pv[0][1]->load()+std::sin(az)*ox+std::cos(az)*oy,
            std::clamp((double)pv[0][2]->load()+oz,0.,s.ceilMat<0?30.:s.height/sf)};
        s.sources[0].pos=s.sources[0].pos*sf;
        s.sources[0].pattern=srcPattern[0].load();s.sources[0].azimuthDeg=pv[0][3]->load();
        s.sources[0].directivity=std::atomic_load(&srcDirectivity[0]);
        s.sources[0].instrumentAmount=srcInstrumentAmount[0].load(std::memory_order_relaxed);
        s.sources[0].aimAzDeg=srcAimAz[0].load(std::memory_order_relaxed);
        s.sources[0].aimElDeg=srcAimEl[0].load(std::memory_order_relaxed);
        s.sources[0].aimRollDeg=srcAimRoll[0].load(std::memory_order_relaxed);
    }
    verb::Transducer receiver;
    receiver.pos={pv[verbp::kListener][0]->load(),pv[verbp::kListener][1]->load(),pv[verbp::kListener][2]->load()};
    receiver.pos=receiver.pos*sf;
    s.receivers={receiver};return s;
}

std::array<double, 3> VerbProcessor::getRT60Estimate() const {
    const auto d = getTailDiagnostics();
    const auto bands = d ? d->physicalRT60 : verb::estimateRoomRT60(verb::withAbsorptionScale(targetScene(),absorptionParam->load()));
    return {{(bands[0]+bands[1])/2.,(bands[2]+bands[3]+bands[4])/3.,(bands[5]+bands[6])/2.}};
}

double VerbProcessor::solveTargetRT60(double seconds, verb::Scene scene, uint64_t serial) {
    if(!std::isfinite(seconds) || seconds<=0){targetState=5;return -1.;}
    targetState = 1;
    const auto requestAtStart = reqSerial.load();
    auto cancelled = [&] { return targetCancel.load() || sceneSerial.load() != serial || reqSerial.load() != requestAtStart; };
    auto mid = [](const auto& bands) { return (bands[2]+bands[3]+bands[4])/3.; };
    try {
        if (getLoadState() != 2 || !std::isfinite(seconds) || seconds <= 0 || scene.sources.empty()) throw std::runtime_error("Invalid target");
        // Eyring + air loss gives a deterministic starting bracket without tracing.
        double lo = .3, hi = 3.;
        for (int i=0;i<16;++i) {
            if (cancelled()) throw std::runtime_error("Cancelled");
            const double x = std::sqrt(lo*hi);
            const double rt = mid(verb::estimateRoomRT60(verb::withAbsorptionScale(scene,x)));
            if (rt > seconds) lo = x; else hi = x;
        }
        const double estimate = std::sqrt(lo*hi);
        auto trace = [&](double scale) {
            if (cancelled()) throw std::runtime_error("Cancelled");
            auto s = verb::withAbsorptionScale(scene,scale); s.length = 20.;
            // Match the live engine's seeded roughness/clutter and 48k reference budget.
            s = verb::withClutter(verb::withRoughness(s),128);
            const auto h = verb::traceTail(s,s.sources[0],s.receivers,1,48000,s.seed,true,cancelled);
            const double rt = mid(verb::analyzeTail(h).rt60);
            if (!std::isfinite(rt) || rt <= 0) throw std::runtime_error("No measurable decay");
            return rt;
        };
        lo = .3; hi = 3.; const double longest = trace(lo), shortest = trace(hi);
        double bestScale = std::abs(longest-seconds) < std::abs(shortest-seconds) ? lo : hi;
        double best = bestScale == lo ? longest : shortest;
        const bool reachable = seconds <= longest && seconds >= shortest;
        double x = estimate;
        for (int i=0;reachable && i<5;++i) {
            const double achieved = trace(x);
            if (std::abs(achieved-seconds) < std::abs(best-seconds)) { best=achieved; bestScale=x; }
            if (std::abs(best/seconds-1.) <= .025) break;
            if (achieved > seconds) lo=x; else hi=x;
            // Multiplicative RT/absorption correction, bounded inside the traced bracket.
            x = std::clamp(x*achieved/seconds,lo+.08*(hi-lo),hi-.08*(hi-lo));
        }
        if (cancelled()) throw std::runtime_error("Cancelled");
        setParamValue("absorption",bestScale);
        targetAchieved = best; targetState = reachable && std::abs(best/seconds-1.) <= .07 ? 2 : 3;
        return best;
    } catch (...) { targetState = 4; return -1.; }
}

double VerbProcessor::setTargetRT60(double seconds) {
    targetCancel = true; if (targetThread.joinable()) targetThread.join();
    targetCancel = false;
    return solveTargetRT60(seconds,targetScene(),sceneSerial.load());
}

void VerbProcessor::startTargetRT60(double seconds) {
    targetCancel = true; if (targetThread.joinable()) targetThread.join();
    auto scene = targetScene(); const auto serial = sceneSerial.load();
    targetCancel = false; targetState = 1;
    targetThread = std::thread([this, seconds, scene=std::move(scene), serial]() mutable {
        solveTargetRT60(seconds,std::move(scene),serial);
    });
}

// Web UI publications are immutable and produced off the audio thread. The page never holds engineLock.
bool VerbProcessor::isWebTailCurrent() const {
    if (!webTailCurrent.load() || !isWebSceneCurrent() || qualityDirty.load()) return false;
    const auto d = getWebTailDiagnostics();
    if (!d) return false;
    // The worker publication can predate an APVTS change. Compare the real
    // requested decay and quality with the immutable applied diagnostics too.
    for (int i = 0; i < 3; ++i)
        if (std::abs(d->multipliers[(size_t)i] - (modeParam->load() >= .5f ? decayParam[i]->load() : 1.)) > 1e-6) return false;
    const auto quality = getTailSettings(); // short dispLock, never engineLock
    return d->baseRays == quality.rays && d->adaptive == quality.adaptive;
}

bool VerbProcessor::startPathBake(int object) {
    if(getLoadState()!=2||object < -1||object>verbp::kListener)return false;
    if(object>=0){
        auto path=getMovementPath(object);
        if(!path.enabled||path.points.size()<2){
            verb::Engine::RecordedPassStats pass;
            {juce::SpinLock::ScopedLockType el(engineLock);if(bundle&&bundle->engine)pass=bundle->engine->recordedPassStats();}
            if(!((pass.state==3||pass.state==4)&&pass.samples>=2))return false;
        }
    }
    const auto logAt = bakeLogEnabled() ? juce::Time::getMillisecondCounterHiRes() : -1.;
    webBakePercent=0;webBakeCancel=false;webBakeStart=object;wake.signal();
    if (logAt >= 0.) writeBakeLog(this, logAt, "request object=" + juce::String(object)
        + " editorOpen=" + juce::String(int(bakeLogEditors.load(std::memory_order_relaxed) > 0)));
    return true;
}
void VerbProcessor::bakeDiagnosticEditorOpened(bool opened) noexcept {
    if (bakeLogEnabled()) bakeLogEditors.fetch_add(opened ? 1 : -1, std::memory_order_relaxed);
}
void VerbProcessor::cancelPathBake(){webBakeStart=-2;webBakeCancel=true;wake.signal();}
void VerbProcessor::clearPathBake(){webBakeStart=-2;webBakeClear=true;wake.signal();}
void VerbProcessor::recordPathPass(bool stop) {
    std::shared_ptr<verb::Engine> engine;
    { juce::SpinLock::ScopedLockType lock(engineLock); if (bundle) engine = bundle->engine; }
    if (engine) {
        if (stop) engine->stopRecordedPass();
        else engine->armRecordedPass();
    }
    wake.signal();
}
void VerbProcessor::clearPathRecording() {
    std::shared_ptr<verb::Engine> engine;
    { juce::SpinLock::ScopedLockType lock(engineLock); if (bundle) engine = bundle->engine; }
    if (engine) engine->clearRecordedPass();
    {
        juce::ScopedLock lock(dispLock);
        savedRecordedPass = {};
        savedBakeTables.erase(std::remove_if(savedBakeTables.begin(), savedBakeTables.end(),
            [](const auto& table) { return table.path.recorded; }), savedBakeTables.end());
    }
    wake.signal();
}
juce::var VerbProcessor::pathBakeStatus() const {juce::ScopedLock lock(webBakeLock);return webBakeStatus;}
void VerbProcessor::publishWebDiagnostics() {
    if (!bundle) { webTailCurrent = false; return; }
    // E9/E16 control-side descriptions. The PathMailbox remains audio-only SPSC.
    std::vector<verb::BakedPath> paths;
    const int NS = (int)bundle->cfg.size();
    std::array<verbp::MovementPath, verbp::kMaxSources + 1> rawPaths;
    for (int o = 0; o <= verbp::kMaxSources; ++o) {
        rawPaths[(size_t)o] = getMovementPath(o);
        if (rawPaths[(size_t)o].points.empty()) webBakeTouched[(size_t)o] = false;
    }
    std::vector<int> activeObjects;
    for (int j = 0; j < NS; ++j)
        if (rawPaths[(size_t)j].enabled && rawPaths[(size_t)j].points.size() >= 2) activeObjects.push_back(j);
    const bool listenerActive = rawPaths[verbp::kListener].enabled && rawPaths[verbp::kListener].points.size() >= 2;
    if (listenerActive) activeObjects.push_back(verbp::kListener);
    const double scale=engineSize.load()*.01,top=webMaximumZ();
    auto yawMatchFor=[&](int o,double x,double y)->verb::BakedYawMatch{
        const auto& rp=rawPaths[(size_t)o];
        const bool hasActiveKeys=rp.enabled&&rp.points.size()>=2&&!rp.azKeys.empty();
        if(verbp::orientModeOf(rp)!=verbp::OrientMode::Manual||hasActiveKeys)return {};
        return {true,x*scale,y*scale};
    };
    auto groupMemberCount=[&](int g)->int{
        if(g<0)return 1;
        int c=0;
        for(int o:activeObjects)if(rawPaths[(size_t)o].group==g)++c;
        return c;
    };
    // E19: orientation dependencies between objects. A static object that looks at a moving one "rides" on the target's path
    // (its yaw is a function of the target's parameter, so it joins the target's group). A moving object that looks at a moving
    // target is representable inside one coupling group (E28: any Start/End/keys/laps - the owner is then indexed by the Position u); anything else needs a recorded pass.
    auto movingObj=[&](int o)->bool{return o>=0&&o<=verbp::kMaxSources&&(o<NS||o==verbp::kListener)&&rawPaths[(size_t)o].enabled&&rawPaths[(size_t)o].points.size()>=2;};
    // Same Position -> arc-fraction mapping. Compares the compiled time maps, not the raw per-point vectors: two paths with different
    // point counts and no custom timing map identically (the raw vectors differ in length and must not count as a mismatch).
    auto timingEqual=[](const verbp::MovementPath& x,const verbp::MovementPath& y){
        if(x.closed!=y.closed||x.laps!=y.laps)return false;
        if(!x.hasCustomTiming()&&!y.hasCustomTiming())return true;
        if(x.start!=y.start||x.end!=y.end)return false;
        verbp::CompiledPath a,c;a.build(x);c.build(y);
        if(a.hasTiming!=c.hasTiming||a.timeKeys.size()!=c.timeKeys.size())return false;
        for(size_t i=0;i<a.timeKeys.size();++i)
            if(a.timeKeys[i].t!=c.timeKeys[i].t||a.timeKeys[i].f!=c.timeKeys[i].f||a.timeKeys[i].easing!=c.timeKeys[i].easing)return false;
        return true;
    };
    std::array<int,verbp::kMaxSources+1> riderOf;riderOf.fill(-1);
    std::array<bool,verbp::kMaxSources+1> ridden{};
    bool orientConflict=false;int conflictOwner=-1,conflictTarget=-1;   // E21 C: first moving object that looks at an uncoupled moving target
    for(int o=0;o<=verbp::kMaxSources;++o){
        if(o>=NS&&o!=verbp::kListener)continue;
        const auto& rp=rawPaths[(size_t)o];
        if(!rp.lookAt||rp.lookPoint)continue;
        const int t=rp.lookTarget;
        if(t<0||t==o||t>verbp::kListener||(t>=NS&&t!=verbp::kListener)||!movingObj(t))continue;
        if(movingObj(o)){if(!(rp.group>=0&&rp.group==rawPaths[(size_t)t].group)){orientConflict=true;if(conflictOwner<0){conflictOwner=o;conflictTarget=t;}}}
        else{riderOf[(size_t)o]=t;ridden[(size_t)t]=true;}
    }
    auto engGroupOf=[&](int o)->int{const auto& r=rawPaths[(size_t)o];return r.group>=0?1000+r.group:(o==verbp::kListener?(ridden[(size_t)o]?3000:-1):o);};
    double uRelaxWorst=1.0;   // E28: tolerance factor the u-indexed sampling had to relax to stay within the station cap (1 = none)
    auto appendPath=[&](const verbp::MovementPath& raw,int object,int engGroup,int logical,double ox,double oy,double oz,bool rider=false){
        verbp::CompiledPath compiled;compiled.build(raw);
        const int self=logical<0?verbp::kListener:logical;
        const auto& own=rawPaths[(size_t)self];
        const bool coupled=rider||groupMemberCount(raw.group)>1||ridden[(size_t)self];
        const int origLaps=compiled.laps;
        compiled.laps=1;
        verb::BakedPath p;p.object=object;p.group=engGroup;p.closed=compiled.closed;
        p.yawMatch=yawMatchFor(self,ox,oy);
        const double baseYaw=pv[self][3]->load();
        const bool dyn=own.lookAt;
        verbp::CompiledPath tgt;const verbp::CompiledPath* tgtPath=nullptr;
        if(dyn&&!own.lookPoint&&movingObj(own.lookTarget)){if(rider)tgtPath=&compiled;else{tgt.build(rawPaths[(size_t)own.lookTarget]);tgt.laps=1;tgtPath=&tgt;}}
        // E28: owner that looks at a moving member of the SAME Group with a different time map (Start/End/keys/easing/laps). Inside one Group
        // everything is a function of the single Position u, so the owner is described indexed by u itself (identity map: startU=0,endU=1,
        // laps=1, distance[i]=u_i*D). Stations are the exact joint states: position = own.evaluate(u), yaw from the target's evaluate(u).
        // E28 / G1: owner looking at a moving Group member with different timing, OR Manual path with active azimuth keys.
        // Everything is a function of Position u in [0, 1], described with uIndexed=true and distance[i]=u_i*D.
        const bool lookDiffTiming=dyn&&!rider&&!own.lookPoint&&movingObj(own.lookTarget)&&own.group>=0&&own.group==rawPaths[(size_t)own.lookTarget].group&&!timingEqual(own,rawPaths[(size_t)own.lookTarget]);
        const bool hasActiveKeys=!rider&&verbp::orientModeOf(own)==verbp::OrientMode::Manual&&own.enabled&&own.points.size()>=2&&!own.azKeys.empty();
        if(lookDiffTiming||hasActiveKeys){
            verbp::CompiledPath liveO=compiled;liveO.laps=origLaps;
            verbp::CompiledPath liveT;
            if(lookDiffTiming)liveT.build(rawPaths[(size_t)own.lookTarget]);
            struct US{double u;verbp::PathPoint q;double yaw;};
            auto stAt=[&](double u)->US{
                const auto q=liveO.evaluate(u);
                if(hasActiveKeys)
                    return {u,q,liveO.evaluateAzimuth(u,baseYaw)};
                const auto tp=liveT.evaluate(u);double look=0;
                const bool ok=verbp::lookAtYaw(q[0],q[1],tp[0],tp[1],look);
                return {u,q,verbp::orientedAzimuth(verbp::OrientMode::LookAt,baseYaw,0.0,ok,look,own.orientOffset)};
            };
            std::vector<double> bp{0.0,1.0};
            std::vector<double> keyBp;
            if(hasActiveKeys){
                for(size_t ki=0;ki<own.azKeys.size();++ki){
                    const double tk=std::clamp(own.azKeys[ki].t*0.01,0.0,1.0);
                    keyBp.push_back(tk);
                    if(ki>0&&own.azKeys[ki-1].easing==verbp::AzimuthEasing::Hold){
                        const double prevTk=std::clamp(own.azKeys[ki-1].t*0.01,0.0,1.0);
                        if(tk-prevTk>5e-9)keyBp.push_back(tk-std::min(1e-8,0.25*(tk-prevTk)));
                    }
                }
            }
            auto nearKeyBp=[&](double u){
                for(double kv:keyBp)if(std::abs(u-kv)<=2e-8)return true;
                return false;
            };
            auto addBreaks=[&](const verbp::CompiledPath& c){
                if(c.hasTiming){
                    for(double u:{c.startNorm,c.endNorm})if(!nearKeyBp(u))bp.push_back(u);
                    for(const auto& k:c.timeKeys){const double u=c.startNorm+k.t*(c.endNorm-c.startNorm);if(!nearKeyBp(u))bp.push_back(u);}
                }
                if(c.closed&&c.laps>1)for(int l=1;l<c.laps;++l){   // u where the arc fraction completes lap l (time map is monotone)
                    const double target=(double)l/c.laps;double lo=0,hi=1;
                    for(int it=0;it<60;++it){const double mid=.5*(lo+hi);if(c.evaluateTimeMap(mid)<target)lo=mid;else hi=mid;}
                    if(!nearKeyBp(hi))bp.push_back(hi);
                }
            };
            addBreaks(liveO);
            if(lookDiffTiming)addBreaks(liveT);
            for(int i=1;i<64;++i){const double u=(double)i/64.;if(!nearKeyBp(u))bp.push_back(u);}
            for(double kv:keyBp)bp.push_back(kv);
            for(auto& v:bp)v=std::clamp(v,0.0,1.0);
            std::sort(bp.begin(),bp.end());
            std::vector<double> grid;for(double v:bp)if(grid.empty()||v-grid.back()>1e-9)grid.push_back(v);
            const size_t kCap=8000;
            std::vector<US> st;
            double relax=1.0;bool overflow=false;
            std::function<void(const US&,const US&,int)> refineU=[&](const US& a,const US& b,int depth){
                if(depth>=18||b.u-a.u<1e-7)return;
                if(st.size()>=kCap){overflow=true;return;}
                const double span=std::remainder(b.yaw-a.yaw,360.);
                bool ok=std::abs(span)<=1.*relax;
                US mid{};
                for(int k=0;k<3;++k){
                    const double f=k==0?.5:k==1?.25:.75;
                    const US m=stAt(a.u+f*(b.u-a.u));if(k==0)mid=m;
                    double e=0;for(int c=0;c<3;++c){const double d=m.q[(size_t)c]-(a.q[(size_t)c]+f*(b.q[(size_t)c]-a.q[(size_t)c]));e+=d*d;}
                    if(std::sqrt(e)>.01*relax)ok=false;
                    if(std::abs(std::remainder(m.yaw-(a.yaw+span*f),360.))>.25*relax)ok=false;
                    if(!ok)break;
                }
                if(ok)return;
                refineU(a,mid,depth+1);st.push_back(mid);refineU(mid,b,depth+1);
            };
            for(int attempt=0;attempt<12;++attempt){
                st.clear();overflow=false;
                st.push_back(stAt(grid.front()));
                for(size_t i=1;i<grid.size();++i){const US cur=stAt(grid[i]);const US prev=st.back();refineU(prev,cur,0);st.push_back(cur);}
                if(!overflow&&st.size()<=kCap)break;
                relax*=2.;
            }
            if(relax>uRelaxWorst)uRelaxWorst=relax;
            double D=0;for(size_t i=1;i<st.size();++i)D+=verbp::pathDistance(st[i-1].q,st[i].q);
            D=std::max(D*scale,1e-3);
            p.closed=false;p.uIndexed=true;
            for(const auto& sm:st){const double a=sm.yaw*verb::kPi/180.;
                p.poses.push_back({(sm.q[0]+std::cos(a)*ox-std::sin(a)*oy)*scale,(sm.q[1]+std::sin(a)*ox+std::cos(a)*oy)*scale,std::clamp(std::clamp(sm.q[2],0.,top)+oz,0.,top)*scale,sm.yaw});
                p.distance.push_back(sm.u*D);}
            if(p.distance.back()>0)paths.push_back(std::move(p));
            return;
        }
        const double L=compiled.length();
                // Orientation of the object at arc fraction f (own plan position sx,sy; own path tangent): the same function the audio thread uses.
        auto orientAt=[&](double f,double sx,double sy,double tangent)->double{
            if(!dyn){
                if(!compiled.orient)return std::remainder(baseYaw,360.);
                return std::remainder(own.orientOffset!=0.0?baseYaw+tangent+own.orientOffset:baseYaw+tangent,360.);
            }
            double tx=0,ty=0,look=0;bool ok=false;
            if(own.lookPoint){tx=own.lookPos[0];ty=own.lookPos[1];ok=true;}
            else{const int t=own.lookTarget;
                if(t>=0&&t!=self&&t<=verbp::kListener&&(t<NS||t==verbp::kListener)){
                    if(tgtPath){const auto tp=tgtPath->evaluateArcFraction(f);tx=tp[0];ty=tp[1];}
                    else{tx=pv[t][0]->load();ty=pv[t][1]->load();}
                    ok=true;
                }}
            ok=ok&&verbp::lookAtYaw(sx,sy,tx,ty,look);
            return verbp::orientedAzimuth(verbp::OrientMode::LookAt,baseYaw,0.0,ok,look,own.orientOffset);
        };
        struct Sample{double d;verbp::PathPoint q;double yaw;};
        const verbp::PathPoint restPos{pv[self][0]->load(),pv[self][1]->load(),pv[self][2]->load()};
        auto sampleAt=[&](double d,const verbp::PathPoint& q,int lap=0)->Sample{double tangent=0;const double f=L>0?d/L:0;compiled.evaluateArcFraction(f,&tangent);return {lap*L+d,q,orientAt(f,q[0],q[1],tangent)};};
        std::vector<Sample> samples;
        const size_t nPts=compiled.points.size();
        const int refineLap=0;
        std::function<void(const Sample&,const Sample&,int)> refine=[&](const Sample& a,const Sample& b,int depth){
            if(depth>=6||samples.size()+nPts>=8000)return;
            const double dm=.5*(a.d+b.d),dl=dm-refineLap*L;
            const Sample m=sampleAt(dl,rider?restPos:compiled.evaluateArcFraction(L>0?dl/L:0),refineLap);
            const double span=std::remainder(b.yaw-a.yaw,360.);
            if(std::abs(span)<=1.&&std::abs(std::remainder(m.yaw-(a.yaw+span*.5),360.))<=.25)return;
            refine(a,m,depth+1);samples.push_back(m);refine(m,b,depth+1);
        };
        for(size_t j=0;j<nPts;++j){
            const Sample cur=sampleAt(compiled.lengths[j],rider?restPos:compiled.points[j],0);
            if(dyn&&!samples.empty()&&cur.d>samples.back().d+1e-9){const Sample prev=samples.back();refine(prev,cur,0);}
            samples.push_back(cur);
        }
        for(const auto& smp:samples){const double yaw=smp.yaw,a=yaw*verb::kPi/180.;
            const double dx=p.yawMatch.free?0.:std::cos(a)*ox-std::sin(a)*oy;
            const double dy=p.yawMatch.free?0.:std::sin(a)*ox+std::cos(a)*oy;
            p.poses.push_back({(smp.q[0]+dx)*scale,(smp.q[1]+dy)*scale,std::clamp(std::clamp(smp.q[2],0.,top)+oz,0.,top)*scale,yaw});p.distance.push_back(smp.d*scale);}
        if(compiled.hasTiming||(compiled.closed&&origLaps>1)){
            uint64_t sig=1469598103934665603ull;
            auto mix=[&](const void* v,size_t n){auto ptr=(const unsigned char*)v;for(size_t k=0;k<n;++k){sig^=ptr[k];sig*=1099511628211ull;}};
            mix(&raw.start,sizeof raw.start);mix(&raw.end,sizeof raw.end);mix(&raw.laps,sizeof raw.laps);
            for(const auto& k:compiled.timeKeys){uint8_t e=(uint8_t)k.easing;mix(&k.t,sizeof k.t);mix(&k.f,sizeof k.f);mix(&e,sizeof e);}
            p.timingSig=sig;
            if(!compiled.closed||origLaps==1||coupled){
                p.startU=compiled.startNorm;p.endU=compiled.endNorm;p.laps=compiled.closed?origLaps:1;
                for(const auto& k:compiled.timeKeys)p.timingKeys.push_back({k.t,k.f,(uint8_t)k.easing});
            }
        }
        if(!p.distance.empty()&&p.distance.back()>0)paths.push_back(std::move(p));
    };
    if(listenerActive) {
        const auto& lp = rawPaths[verbp::kListener];
        appendPath(lp, -1, engGroupOf(verbp::kListener), -1, 0, 0, 0);
    }
    for(int logical=0;logical<NS;++logical){
        const auto& raw=rawPaths[(size_t)logical];
        if(!raw.enabled||raw.points.size()<2)continue;
        const int engGroup = engGroupOf(logical);
        for(size_t j=0;j<bundle->points.size();++j)
            if(bundle->points[j].source==logical)
                appendPath(raw,(int)j,engGroup,logical,ptOff[j][0].load(),ptOff[j][1].load(),ptOff[j][2].load());
    }
    for(int o=0;o<=verbp::kMaxSources;++o){  // E19 riders: static objects that look at a moving target
        const int t=riderOf[(size_t)o];
        if(t<0)continue;
        const auto& draw=rawPaths[(size_t)t];const int eg=engGroupOf(t);
        if(o==verbp::kListener)appendPath(draw,-1,eg,-1,0,0,0,true);
        else for(size_t j=0;j<bundle->points.size();++j)
            if(bundle->points[j].source==o)appendPath(draw,(int)j,eg,o,ptOff[j][0].load(),ptOff[j][1].load(),ptOff[j][2].load(),true);
    }
    bundle->engine->setBakeForcedBlock(orientConflict);
    std::vector<verb::BakedYawMatch> recordedYawMatch;
    recordedYawMatch.reserve(bundle->points.size()+1);
    recordedYawMatch.push_back(yawMatchFor(verbp::kListener,0.,0.));
    for(size_t j=0;j<bundle->points.size();++j)
        recordedYawMatch.push_back(yawMatchFor(bundle->points[j].source,ptOff[j][0].load(),ptOff[j][1].load()));
    bundle->engine->setBakedPaths(paths,recordedYawMatch);
    const int object = activeObjects.size() == 1 ? activeObjects.front() : -1;
    if(webBakeClear.exchange(false)){webBakeTouched.fill(false);webRecordedTouched=false;bundle->engine->clearPathBake();}
    else if(webBakeCancel.exchange(false)){bundle->engine->cancelPathBake();}
    else if(isWebSceneCurrent() && !qualityDirty.load()) {
        const int requested=webBakeStart.exchange(-2);
        if(requested!=-2){
            for(int o:activeObjects)webBakeTouched[(size_t)o]=true;
            const auto ps=bundle->engine->recordedPassStats();
            if((ps.state==3||ps.state==4)&&ps.samples>=2)webRecordedTouched=true;
            bundle->engine->requestPathBake();
            if (bakeLogEnabled()) {
                bakeLogDispatchMs = juce::Time::getMillisecondCounterHiRes();
                bakeLogStartBlocks = webAudioBlockCount();
                bakeLogWaitReason = 0;
                const auto quality = getTailSettings();
                const auto profiled = std::count_if(bundle->cfg.begin(), bundle->cfg.end(),
                    [](const auto& source) { return source.directivity != nullptr; });
                writeBakeLog(this, bakeLogDispatchMs, "dispatch layout=" + juce::String(effectiveLayout())
                    + " outputs=" + juce::String(bundle->R) + " points=" + juce::String((juce::uint64)bundle->points.size())
                    + " profiledSources=" + juce::String((int)profiled) + " sr=" + juce::String(builtSr.load(), 0)
                    + " block=" + juce::String(webLastBlockSize.load()) + " mode=" + juce::String(modeParam->load(), 0)
                    + " early=" + juce::String(earlyParam->load(), 3) + " tail=" + juce::String(tailParam->load(), 3)
                    + " baseRays=" + juce::String(quality.rays) + " adaptive=" + juce::String((int)quality.adaptive)
                    + " editorOpen=" + juce::String(int(bakeLogEditors.load(std::memory_order_relaxed) > 0)));
            }
        }
    }
    auto bake=bundle->engine->bakeStats();
    const bool bakeQueued=webBakeStart.load()!=-2;
    if(bakeQueued){bake.state=verb::Engine::BakeState::Baking;bake.waitReason=2;bake.percent=0;bake.done=0;bake.total=0;}
    webBakePercent=bake.percent;
    const auto pass=bundle->engine->recordedPassStats();
    using BakeSource = verb::Engine::BakeStats::Source;
    const bool usesRecording=bake.source==BakeSource::Recorded;
    const bool drawnActive=bake.source==BakeSource::Drawn;
    const bool recordedActive=!bake.groups.empty()&&bake.groups.front().group==2000;
    if(pass.state==0)webRecordedTouched=false;
    if((bake.state==verb::Engine::BakeState::Baked||bake.state==verb::Engine::BakeState::Approximate)&&!bake.groups.empty()&&bake.groups[0].group==2000)webRecordedTouched=true;
    bool anyTouched=recordedActive&&webRecordedTouched;
    for(int o:activeObjects)anyTouched|=webBakeTouched[(size_t)o];
    const char* bakeStateText="notBaked";juce::String reason;
    // P4: ASCII restore hints, shown only while a restored table is actually valid (same text for web and classic).
    juce::String bakeHint;
    if(bake.state==verb::Engine::BakeState::Baked||bake.state==verb::Engine::BakeState::Approximate){
        if(bake.restored==1)bakeHint="restored from the saved project";
        else if(bake.restored==2)bakeHint="restored after undo";
        if(bakeTooLargeToStore.load())bakeHint+=juce::String(bakeHint.isEmpty()?"":"; ")+"bake not stored: too large";
    }
    // ASCII-only object name for web diagnostics: a name with non-ASCII or control characters falls back to "Source N".
    auto objName=[&](int o)->juce::String{
        if(o==verbp::kListener)return "Listener";
        if(o>=0&&o<(int)bundle->cfg.size()){
            const auto& n=bundle->cfg[(size_t)o].name;bool ascii=!n.empty();
            for(const char ch:n)if(ch<32||ch>126)ascii=false;
            if(ascii)return juce::String(n).substring(0,32);
        }
        return "Source "+juce::String(o+1);
    };
    const auto bakeProgressText = [&](int percent, int done, int total) -> juce::String {
        switch (bake.waitReason) {
            case 1: return "Updating the tail after a timeline jump";
            case 2: case 5: return "Rebuilding the room";
            case 7: return "Updating the tail for settled poses";
            case 14: return "Waiting for listener and source poses to settle";
            case 16: return "Waiting for poses at the new room size";
            default: return (percent<45?"Tracing stations":"Refining path")+juce::String(" - ")+juce::String(done)+"/"+juce::String(total)+" stations";
        }
    };
    switch(bake.state){
        case verb::Engine::BakeState::Baking:bakeStateText="baking";reason=bakeProgressText(bake.percent,bake.done,bake.total);break;
        case verb::Engine::BakeState::Baked:bakeStateText="baked";reason=bakeHint;break;
        case verb::Engine::BakeState::Approximate:bakeStateText="approximate";reason=juce::String(bake.discontinuities)+" intervals above tolerance";if(bakeHint.isNotEmpty())reason+="; "+bakeHint;break;
        case verb::Engine::BakeState::Outdated:bakeStateText=anyTouched?"outdated":"notBaked";reason=anyTouched?(recordedActive?"Room, quality or recorded pass changed - bake again":"Room, quality or path changed - bake again"):"";break;
        case verb::Engine::BakeState::Cancelled:bakeStateText="cancelled";reason="Cancelled";break;
        case verb::Engine::BakeState::Blocked:bakeStateText="blocked";
            if(paths.empty())reason="Enable a path or record a pass before baking";
            else if(orientConflict||riderOf[(size_t)verbp::kListener]>=0){
                const int owner=orientConflict?conflictOwner:verbp::kListener,target=orientConflict?conflictTarget:riderOf[(size_t)verbp::kListener];
                reason="Look at target moves independently: "+objName(owner)+" looks at "+objName(target)+"; put both paths in one Group or record a pass";
            }
            else if(listenerActive&&rawPaths[verbp::kListener].group<0)reason="Cannot bake uncoupled moving listener with sources - assign them to one Group or record a pass";
            else reason="Cannot bake uncoupled paths alongside a moving listener - couple all moving paths in one Group or record a pass";
            break;
        case verb::Engine::BakeState::Failed:bakeStateText="failed";reason=bake.waitReason==15?"Bake failed - resource or tracing error":"Late energy unavailable - bake could not finish";break;
        default:break;
    }
    if(uRelaxWorst>1.&&(bake.state==verb::Engine::BakeState::Baked||bake.state==verb::Engine::BakeState::Approximate))
        reason+=(reason.isEmpty()?"":"; ")+juce::String("Look at path sampling relaxed x")+juce::String(uRelaxWorst,0)+" (station cap)";
    juce::String name;
    if(!bake.groups.empty()&&bake.groups[0].group==2000)name="Recorded pass";
    else if(object==verbp::kListener)name="Listener";
    else if(object>=0&&object<NS)name=juce::String(bundle->cfg[(size_t)object].name);
    else if(activeObjects.size()>1){
        const int g0=rawPaths[(size_t)activeObjects.front()].group;
        bool sameGrp=g0>=0;for(int o:activeObjects)sameGrp&=(rawPaths[(size_t)o].group==g0);
        name=sameGrp?("Group "+juce::String(g0+1)+" ("+juce::String((int)activeObjects.size())+" paths)")
                    :(juce::String((int)activeObjects.size())+" paths");
    }else name="Multiple paths";

    juce::Array<juce::var> perPathArr;
    for(int o:activeObjects){
        const auto& raw=rawPaths[(size_t)o];
        const int expectedGrp=engGroupOf(o);
        const verb::Engine::PathGroupStats* grpStat=nullptr;
        for(const auto& gs:bake.groups)if(gs.group==expectedGrp){grpStat=&gs;break;}
        const char* pState=bakeStateText;
        int pPercent=bake.percent,pDone=bake.done,pTotal=bake.total;
        juce::String pReason=reason;
        if(!bakeQueued&&bake.state!=verb::Engine::BakeState::Blocked&&bake.state!=verb::Engine::BakeState::Cancelled&&bake.state!=verb::Engine::BakeState::Failed&&grpStat){
            pPercent=grpStat->percent;pDone=grpStat->done;pTotal=grpStat->total;pReason="";
            switch(grpStat->state){
                case verb::Engine::BakeState::Baking:pState="baking";pReason=bakeProgressText(pPercent,pDone,pTotal);break;
                case verb::Engine::BakeState::Baked:pState="baked";pReason=bakeHint;break;
                case verb::Engine::BakeState::Approximate:pState="approximate";pReason=juce::String(grpStat->discontinuities)+" intervals above tolerance";if(bakeHint.isNotEmpty())pReason+="; "+bakeHint;break;
                case verb::Engine::BakeState::Outdated:pState=webBakeTouched[(size_t)o]?"outdated":"notBaked";pReason=webBakeTouched[(size_t)o]?"Room, quality or path changed - bake again":"";break;
                case verb::Engine::BakeState::NotBaked:default:pState="notBaked";break;
            }
        }
        auto* item=new juce::DynamicObject();
        item->setProperty("object",o);
        item->setProperty("group",raw.group);
        item->setProperty("drivenBy",verbp::pathGroupDriver(rawPaths,NS,o));
        item->setProperty("state",pState);
        item->setProperty("percent",pPercent);
        item->setProperty("reason",pReason);
        item->setProperty("done",pDone);
        item->setProperty("total",pTotal);
        perPathArr.add(juce::var(item));
    }

    auto* status=new juce::DynamicObject();status->setProperty("state",bakeStateText);status->setProperty("object",object);status->setProperty("name",name);status->setProperty("percent",bake.percent);status->setProperty("reason",reason);status->setProperty("done",bake.done);status->setProperty("total",bake.total);status->setProperty("workers",bake.workers);status->setProperty("paths",perPathArr);
    status->setProperty("passState",pass.state);status->setProperty("passSamples",pass.samples);
    status->setProperty("bakeSource",usesRecording?"recorded":drawnActive?"drawn":"none");
    if(drawnActive&&bake.groups.size()==1&&!activeObjects.empty()){
        const int group=rawPaths[(size_t)activeObjects.front()].group;
        if(group>=0&&bake.groups.front().group==engGroupOf(activeObjects.front()))
            status->setProperty("bakeSourceGroup",group+1);
    }
    if(drawnActive&&(pass.state==3||pass.state==4)&&pass.samples>=2)
        status->setProperty("recordingNote","Recording kept, not used: drawn paths are baked instead. Press Discard if you do not need it.");
    if(usesRecording&&pass.offPass&&(bake.state==verb::Engine::BakeState::Baked||bake.state==verb::Engine::BakeState::Approximate)){
        const juce::String warning="Live motion is outside the baked recording - estimating the tail";
        status->setProperty("reason",reason.isEmpty()?warning:reason+"; "+warning);
        for(auto& item:perPathArr)if(auto* o=item.getDynamicObject()){
            const auto previous=o->getProperty("reason").toString();
            o->setProperty("reason",previous.isEmpty()?warning:previous+"; "+warning);
        }
    }
    {juce::ScopedLock lock(webBakeLock);webBakeStatus=juce::var(status);}
    if (bakeLogDispatchMs >= 0.) {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (bake.state == verb::Engine::BakeState::Baking) {
            if (bake.waitReason != bakeLogWaitReason) {
                bakeLogWaitReason = bake.waitReason;
                writeBakeLog(this, now, "wait reason=" + juce::String(bake.waitReason) + " stations="
                    + juce::String((juce::uint64)bake.stations));
            }
        } else {
            const auto trace = bundle->engine->bakeTrace();
            double lastInitial = 0., lastStation = 0.;
            for (const auto& station : trace.stations) {
                if (station.kind == 0) lastInitial = std::max(lastInitial, station.doneMs);
                lastStation = std::max(lastStation, station.doneMs);
            }
            juce::String message = juce::String("published state=") + bakeStateText
                + " dispatchToPublishMs=" + juce::String(now - bakeLogDispatchMs, 1)
                + " traceMs=" + juce::String(bake.percent == 100 ? bake.milliseconds : 0., 1)
                + " lastInitialMs=" + juce::String(lastInitial, 1) + " lastStationMs=" + juce::String(lastStation, 1)
                + " workers=" + juce::String(trace.workers) + " activeLimit=" + juce::String(trace.activeLimit)
                + " stations=" + juce::String((juce::uint64)trace.stations.size())
                + " maxStepGapMs=" + juce::String(trace.maxStepGapMs, 1)
                + " callbacks=" + juce::String((juce::uint64)(webAudioBlockCount() - bakeLogStartBlocks))
                + " editorOpen=" + juce::String(int(bakeLogEditors.load(std::memory_order_relaxed) > 0));
            if (bake.percent == 100)
                message += " hash=" + juce::String::toHexString((juce::uint64)bake.hash).paddedLeft('0', 16);
            if (bake.percent == 100) for (const auto& table : bundle->engine->bakedTablesSnapshot())
                message += " group=" + juce::String(table->path.group) + "/v" + juce::String(table->estimator)
                    + "/" + juce::String(table->selectedRays) + "rays/" + juce::String((int)table->u.size()) + "nodes";
            bakeLogDispatchMs = -1.;
            const auto diagnostics = bundle->engine->bakeWorkerDiagnostics();
            if (!diagnostics.empty()) message += "\n" + juce::String(diagnostics);
            writeBakeLog(this, now, message);
        }
    }
    const auto modeled = bundle->engine->sceneSnapshot();
    if (std::atomic_load(&webModeledScene) != modeled) { std::atomic_store(&webModeledScene, modeled); ++webGeometrySerial; }
    std::atomic_store(&webTailDiagnostics, bundle->engine->tailDiagnostics());
    webTailCurrent = state.load() == 2 && !geomDirty.load() && !qualityDirty.load()
        && geometryEpoch.load() == appliedGeometryEpoch.load() && engineAbsorption.load() == absorptionParam->load() && engineSize.load()==sizeParam->load()
        && bundle->engine->tailIsCurrent();
}

// ---------- loader thread ----------
void VerbProcessor::run() {
    double appliedAbsorption = -1.;
    double observedScale = -1., observedSize=-1.;
    uint64_t observedEpoch = 0;
    auto stableSince = std::chrono::steady_clock::now();
    uint64_t directivitySeen = ~0ull;
    auto canonicalPattern = [](const verbp::SourceCfg& source) {
        const double p = verbp::effectiveAnalyticPattern(source);
        return std::isfinite(p) ? std::min(std::max((double)(float)p, 0.0), 1.0) : 0.0;
    };
    auto snapshotSourceStates = [&](const std::vector<verbp::SourceCfg>& sources) {
        std::vector<verb::DirectivityState> states;
        states.reserve(sources.size());
        for (const auto& source : sources)
            states.push_back({source.directivity, source.instrumentAmount, canonicalPattern(source), true, source.aimAzDeg, source.aimElDeg, source.aimRollDeg});
        return states;
    };
    std::vector<verb::DirectivityState> publishedSourceStates; // loader-owned snapshots
    while (!threadShouldExit()) {
        wake.wait(250);
        if (threadShouldExit()) break;
        const auto directivityEpoch = srcDirectivityEpoch.load(std::memory_order_acquire);
        if (bundle && directivityEpoch != directivitySeen) {
            const auto sources = getSources(); // locks/copies exclusively on this loader thread
            for (size_t p=0;p<bundle->points.size();++p) {
                const auto logical=(size_t)bundle->points[p].source;
                if (logical < sources.size()) {
                    const auto& source=sources[logical];
                    const double pattern = canonicalPattern(source);
                    if(logical>=publishedSourceStates.size())publishedSourceStates.resize(logical+1);
                    const auto& previous=publishedSourceStates[logical];
                    if(previous.profile!=source.directivity || previous.amount!=source.instrumentAmount || previous.pattern!=pattern
                        || previous.aimAzDeg!=source.aimAzDeg || previous.aimElDeg!=source.aimElDeg || previous.aimRollDeg!=source.aimRollDeg)
                        bundle->engine->setSourceDirectivity((int)p,source.directivity,source.instrumentAmount,pattern,
                            source.aimAzDeg,source.aimElDeg,source.aimRollDeg);
                }
            }
            publishedSourceStates = snapshotSourceStates(sources);
            directivitySeen = directivityEpoch;
        }
        publishWebDiagnostics();
        if (qualityDirty.exchange(false) && bundle) {
            const auto settings = getTailSettings();
            bundle->engine->setTailQuality(settings.rays, settings.adaptive);
        }
        const double scale = absorptionParam->load();
        const double size=sizeParam->load();
        const bool scaleChanged = scale != appliedAbsorption || size!=engineSize.load();

        const auto epochNow=geometryEpoch.load();
        const auto now=std::chrono::steady_clock::now();
        if(scale!=observedScale || size!=observedSize || epochNow!=observedEpoch) {
            observedSize=size;observedScale=scale;observedEpoch=epochNow;stableSince=now;
        }
        bool scenePending;
        {juce::ScopedLock l(reqLock);scenePending=hasReq;}
        // Geometry/absorption automation settles for at least 150 ms. New room loads
        // take priority; their cancellation token also aborts an in-flight probe.
        if (!scenePending && now-stableSince>=std::chrono::milliseconds(150) && (geomDirty.load() || scaleChanged)) {
            geomDirty=false;
            verb::Scene s; uint64_t epoch;
            {
                juce::ScopedLock l(dispLock);
                s = display; epoch = geometryEpoch.load();
            }
            verb::scaleRoomGeometry(s,size*.01);
            const auto serial=reqSerial.load();
            try {
                if (bundle && bundle->engine->setScene(s, scale, [&]{return threadShouldExit() || reqSerial.load()!=serial || geometryEpoch.load()!=epoch || absorptionParam->load()!=scale || sizeParam->load()!=size;},size/bundle->constructionSize)) {
                    engineSize=size;engineAbsorption=scale;appliedAbsorption=scale;appliedGeometryEpoch=epoch; publishWebDiagnostics();
                } else if(bundle) geomDirty=true;
            } catch(...) {geomDirty=true;}
        }
        Request r;
        uint64_t myRequest;
        {
            juce::ScopedLock l(reqLock);
            if (!hasReq) continue;
            r = req;
            hasReq = false;
            myRequest = reqSerial.load();
        }
        try {
           #if VERB_REQUIRE_AVX2
            if (!(juce::SystemStats::hasAVX2() && juce::SystemStats::hasFMA3()))
                throw std::runtime_error("This CPU has no AVX2/FMA support (needed by the room engine)");
           #endif
            std::istringstream ss(r.text);
            verb::Scene sc = verb::parseScene(ss, r.name);
            sc.sr = r.sr;
            double home[4];
            if (sc.hasListener) { home[0] = sc.listenerPos.x; home[1] = sc.listenerPos.y; home[2] = sc.listenerPos.z; home[3] = sc.listenerYaw; }
            else {
                verb::Vec3 c;
                for (auto& rc : sc.receivers) c = c + rc.pos;
                if (!sc.receivers.empty()) c = c * (1.0 / (double)sc.receivers.size());
                home[0] = c.x; home[1] = c.y; home[2] = c.z; home[3] = 0;
            }
            const bool useDefaults = defaultsOwed.load();

            // sources of the setup: a new scene starts with one mono source per scene source
            std::vector<verbp::SourceCfg> cfg;
            verbp::OutputCfg ocfg;
            {
                juce::ScopedLock l(dispLock);
                if (reqSerial.load() != myRequest) continue;
                if (useDefaults && !r.keepRouting && !r.setup.empty()) {  // the room brings its own sources (e.g. the car's engine stems)
                    srcCfg = r.setup;
                } else if ((useDefaults && !r.keepRouting) || srcCfg.empty()) {
                    srcCfg.clear();
                    for (size_t i = 0; i < sc.sources.size() && (int)i < verbp::kMaxSources; ++i) {
                        verbp::SourceCfg c;
                        c.name = sc.sources[i].name;
                        c.firstIn = (int)i;
                        if (!sc.sources[i].instrument.empty() && sc.sources[i].instrument != "none") {
                            c.instrument = sc.sources[i].instrument;
                            c.instrumentAmount = sc.sources[i].instrumentAmount;
                            c.aimAzDeg = sc.sources[i].aimAzDeg;
                            c.aimElDeg = sc.sources[i].aimElDeg;
                            c.aimRollDeg = sc.sources[i].aimRollDeg;
                        }
                        srcCfg.push_back(c);
                    }
                    if (srcCfg.empty()) srcCfg.push_back(verbp::SourceCfg{});
                }
                syncPatterns();
                if (useDefaults && !r.keepRouting && r.micPattern >= 0) outCfg.micPattern = std::min(std::max(r.micPattern, 0), (int)verb::MicPattern::HumanEars);
                if (useDefaults && !r.keepRouting && r.micPattern == (int)verb::MicPattern::HumanEars) outCfg.radius = 0.0875;  // ears at the head half-width
                cfg = srcCfg;
                ocfg = outCfg;
            }
            const auto points = verbp::buildPoints(cfg);
            if ((int)points.size() > verbp::kMaxPoints) throw std::runtime_error("too many engine points (" + std::to_string(points.size()) + ", max " + std::to_string(verbp::kMaxPoints) + ")");
            if (points.empty()) throw std::runtime_error("no sources");

            // positions used to build: the scene's own when its positions are being applied, else the current parameters
            auto srcState = [&](int i, int k) -> double {
                if (useDefaults && i < (int)sc.sources.size()) {
                    const auto& t = sc.sources[(size_t)i];
                    return k == 0 ? t.pos.x : k == 1 ? t.pos.y : k == 2 ? t.pos.z : t.azimuthDeg;
                }
                return pv[i][k]->load();
            };
            verb::Vec3 center = {home[0], home[1], home[2]};
            if (!useDefaults) center = {pv[verbp::kListener][0]->load(), pv[verbp::kListener][1]->load(), pv[verbp::kListener][2]->load()};

            const int hostCh = std::max(1, hostOutCh.load());
            const verb::Layout lay = ocfg.layout < 0 ? verb::layoutForChannels(hostCh) : (verb::Layout)ocfg.layout;
            const double requestedSize=sizeParam->load();
            const double sizeFactor=requestedSize*.01;
            center=center*sizeFactor;
            verb::Scene es = sc;
            verb::scaleRoomGeometry(es,sizeFactor);
            es.sources.clear();
            for (const auto& pm : points) {
                verb::Transducer t;
                const auto& scfg = cfg[(size_t)pm.source];
                t.name = scfg.name + (pm.channel >= 0 ? "." + std::to_string(pm.channel + 1) : "");
                const double az = srcState(pm.source, 3), a = (az + pm.angleDeg) * verb::kPi / 180.0;
                t.pos = {srcState(pm.source, 0) + pm.radius * std::cos(a), srcState(pm.source, 1) + pm.radius * std::sin(a), srcState(pm.source, 2)};
                t.azimuthDeg = az;
                t.pattern = canonicalPattern(scfg);
                t.instrument = scfg.instrument;
                t.instrumentAmount = std::isfinite(scfg.instrumentAmount) ? std::min(std::max(scfg.instrumentAmount, 0.0), 1.0) : 1.0;
                t.aimAzDeg = (scfg.directivity && std::isfinite(scfg.aimAzDeg)) ? std::min(std::max(scfg.aimAzDeg, -180.0), 180.0) : 0.0;
                t.aimElDeg = (scfg.directivity && std::isfinite(scfg.aimElDeg)) ? std::min(std::max(scfg.aimElDeg, -90.0), 90.0) : 0.0;
                t.aimRollDeg = (scfg.directivity && std::isfinite(scfg.aimRollDeg)) ? std::min(std::max(scfg.aimRollDeg, -180.0), 180.0) : 0.0;
                t.directivity = scfg.directivity;
                t.pos=t.pos*sizeFactor;
                es.sources.push_back(t);
            }
            es.receivers = verb::makeReceiverArray(lay, center, ocfg.radius, std::min(hostCh, verbp::kMaxReceivers));
            if (!verb::isAmbisonicLayout(lay))
                verb::applyMicPattern(es.receivers, (verb::MicPattern)ocfg.micPattern, ocfg.radius);
            if ((int)es.receivers.size() > verbp::kMaxReceivers) es.receivers.resize(verbp::kMaxReceivers);
            // Channels the track cannot carry are not rendered.
            if ((int)es.receivers.size() > hostCh) es.receivers.resize((size_t)hostCh);
            es.hasListener = true;
            es.listenerPos = center;
            es.listenerYaw = 0;

            verb::EngineConfig ec;
            ec.erOrder = 1;  // coherent specular taps only for first-order reflections; the rest is the noise-like tail
            ec.ctrlInterval = 128;
            ec.tailBlock = 512;
            ec.tailThread = !offlineRender.load();
            ec.sharedTail = points.size() > 4;  // many sources: one late-field convolver per output channel
            const auto quality = getTailSettings();
            ec.tailRays = quality.rays;
            ec.adaptiveTail = quality.adaptive;
            ec.maxEarlyDelaySec = ec.sharedTail ? 0.4 : 0.75;

            if (modeParam->load() >= .5f) for (int g=0;g<3;++g) ec.decayMultipliers[g] = decayParam[g]->load();
            ec.absorptionScale = absorptionParam->load();
            ec.cancelled=[&,myRequest]{return threadShouldExit() || reqSerial.load()!=myRequest;};
            auto nb = std::make_unique<Bundle>();
            nb->engine = std::make_shared<verb::Engine>(es, ec);
            if(ec.cancelled())continue;
            // Canonical fixedPattern=true directivity state before publishing the engine,
            // matching live setSourcePattern/setSourceDirectivity without changing initial tail revision.
            for (size_t p = 0; p < points.size(); ++p) {
                const auto& scfg = cfg[(size_t)points[p].source];
                nb->engine->setSourceDirectivity((int)p, scfg.directivity, scfg.instrumentAmount,
                    canonicalPattern(scfg), scfg.aimAzDeg, scfg.aimElDeg, scfg.aimRollDeg);
            }
            std::vector<verb::DirectivityState> nextSourceStates = snapshotSourceStates(cfg);
            appliedAbsorption=ec.absorptionScale;
            nb->constructionSize=requestedSize;
            nb->cfg = cfg;
            nb->points = points;
            nb->pointStart.assign(cfg.size(), 0);
            nb->pointCount.assign(cfg.size(), 0);
            for (size_t p = 0; p < points.size(); ++p) {
                if (nb->pointCount[(size_t)points[p].source] == 0) nb->pointStart[(size_t)points[p].source] = (int)p;
                ++nb->pointCount[(size_t)points[p].source];
            }
            nb->R = nb->engine->numReceivers();
            nb->ambisonic = verb::isAmbisonicLayout(lay);
            nb->maxBlock = std::max(maxBlock, 128);
            nb->in.assign(points.size(), std::vector<float>((size_t)nb->maxBlock, 0.f));
            nb->out.assign((size_t)nb->R, std::vector<float>((size_t)nb->maxBlock, 0.f));
            nb->detector.assign((size_t)nb->maxBlock,0.f);
            nb->lfe.assign((size_t)nb->maxBlock, 0.f);
            nb->wetMix.assign((size_t)nb->maxBlock, 1.f);
            nb->dryMix.assign((size_t)nb->maxBlock, 0.f);
            const auto spk = verb::speakersFor(lay, std::min(hostCh, verbp::kMaxReceivers));
            for (size_t i = 0; i < spk.size() && i < (size_t)nb->R; ++i)
                if (spk[i].lfe) nb->lfeOut = (int)i;

            for (size_t p = 0; p < points.size(); ++p) {  // live offsets: free placement, or the format's circle
                const verbp::PointMap& pm = points[p];
                const verbp::SourceCfg& c = cfg[(size_t)pm.source];
                float ox = 0, oy = 0, oz = 0;
                if (c.freePoints && pm.channel >= 0 && pm.channel < (int)c.ox.size() && pm.channel < (int)c.oy.size()) {
                    ox = c.ox[(size_t)pm.channel]; oy = c.oy[(size_t)pm.channel];
                    oz = pm.channel < (int)c.oz.size() ? c.oz[(size_t)pm.channel] : 0.f;
                } else if (pm.radius > 0) {
                    const double a = pm.angleDeg * verb::kPi / 180.0;
                    ox = (float)(pm.radius * std::cos(a)); oy = (float)(pm.radius * std::sin(a));
                }
                ptOff[p][0] = ox; ptOff[p][1] = oy; ptOff[p][2] = oz;
            }
            {
                juce::ScopedLock l(dispLock);
                livePoints = points;
            }

            if (useDefaults && reqSerial.load() == myRequest && defaultsOwed.exchange(false)) {
                if (!r.keepRouting) for (int s = 0; s < verbp::kMaxSources; ++s) {
                    setParamValue(verbp::sourceSwitchId(s, false), 0.0);
                    setParamValue(verbp::sourceSwitchId(s, true), 0.0);
                }
                applyDefaultsFrom(sc, home);
            }
            {
                verb::Engine::RecordedPassData pendingPass;
                {
                    juce::ScopedLock l(dispLock);
                    if (r.gen == roomGen && savedRecordedPass.samples >= 2 && savedRecordedPass.objects == nb->engine->numSources() + 1)
                        pendingPass = savedRecordedPass;
                }
                if (pendingPass.samples >= 2)
                    nb->engine->importRecordedPass(pendingPass);
            }
            {
                juce::SpinLock::ScopedLockType l(engineLock);
                bundle.swap(nb);
                publishedSourceStates.swap(nextSourceStates);
                directivitySeen = ~0ull; wake.signal();
                webBakeTouched.fill(false);webRecordedTouched=false;webBakeStart=-2;webBakeCancel=false;webBakePercent=0;
                engineSize=requestedSize;engineAbsorption=ec.absorptionScale; appliedGeometryEpoch=geometryEpoch.load();
                updateBounds(sc);
                ++loadSerial;
            }
            publishWebDiagnostics();
            // P4: install the tables saved with this state AFTER the scene/paths are applied. The engine
            // verifies every content signature against the freshly rebuilt world; a mismatch leaves notBaked.
            {
                std::vector<verb::BakedTable> tables;
                { juce::ScopedLock l(dispLock); if (r.gen == roomGen) tables = std::move(savedBakeTables); savedBakeTables.clear(); }
                if (!tables.empty()) {
                    std::shared_ptr<verb::Engine> eng;
                    { juce::SpinLock::ScopedLockType el(engineLock); if (bundle) eng = bundle->engine; }
                    if (eng && eng->installBakedTables(tables)) publishWebDiagnostics();  // status reflects the restore before ready
                }
            }
            nb.reset();  // old engine destroyed here, off the audio thread
            builtSr = r.sr;
            numPts = (int)points.size();
            builtOutCh = std::min(hostCh, (int)es.receivers.size());
            builtLayout = (int)lay;
            builtForHostCh = hostCh;
            {
                juce::ScopedLock l(dispLock);
                if (r.rebuild && displayGen == r.gen && displayEdits != r.editsAtRequest) {
                    // The engine was rebuilt for the SAME room from text that is older than the display scene: a lock / hide /
                    // layer flag (or a geometry edit, which also re-sends itself to the engine) changed meanwhile.
                    // The display scene is newer than `sc`; copying `sc` over it would lose those edits.
                    display.sr = sc.sr;
                    updateBounds(display);
                } else {
                    display = sc;
                }
                displayGen = r.gen;
                if (r.gen == roomGen) savedRecordedPass = {};
                ++sceneSerial;
                if (!r.rebuild) ++structRevision;  // a new room is now displayed (a same-room rebuild changes no document state)
                for (int k = 0; k < 4; ++k) displayHome[k] = home[k];
                const auto& ld = verb::layoutDefs();
                juce::String ln = "?";
                for (auto& d : ld) if (d.id == lay) ln = d.name;
                status = juce::String(r.name) + ": " + juce::String((int)sc.walls.size()) + " walls, " + juce::String((int)cfg.size()) + " source(s) = " +
                         juce::String((int)points.size()) + " point(s), output " + ln + " (" + juce::String((int)es.receivers.size()) + " ch)" +
                         (ec.sharedTail ? ", shared late field" : "");
            }
            state = reqSerial.load() == myRequest ? 2 : 1;  // a newer request is already waiting: not ready yet
        } catch (const std::exception& ex) {
            if(threadShouldExit() || reqSerial.load()!=myRequest)continue;
            juce::ScopedLock l(dispLock);
            status = juce::String("Error: ") + ex.what();
            state = 3;
        }
    }
}

juce::String VerbProcessor::getStatus() const {
    juce::ScopedLock l(dispLock);
    return status;
}
juce::String VerbProcessor::getSceneName() const {
    juce::ScopedLock l(dispLock);
    return sceneName;
}
verb::Scene VerbProcessor::getDisplayScene(double* home) const {
    juce::ScopedLock l(dispLock);
    if (home)
        for (int k = 0; k < 4; ++k) home[k] = displayHome[k];
    return display;
}

std::shared_ptr<verb::BakePool> VerbProcessor::getAnalysisPool() const { juce::SpinLock::ScopedLockType lock(engineLock); return bundle ? bundle->engine->analysisPool() : nullptr; }
verb::Scene VerbProcessor::getEnergyScene(int source, bool webSnapshot) const {
    verb::Scene sc;
    std::vector<verbp::SourceCfg> cfg;
    { juce::ScopedLock l(dispLock); sc = display; cfg = srcCfg; }
    std::shared_ptr<const verb::Scene> modeled;
    if (webSnapshot) modeled = std::atomic_load(&webModeledScene);
    else { juce::SpinLock::ScopedLockType lock(engineLock); if (bundle) modeled = bundle->engine->sceneSnapshot(); }
    if (modeled) {
        sc = *modeled; // copy after releasing the processor's short pointer lock
        sc.clutter.count = 0; sc.roughness = 0; // already present in modeled geometry
    }
    sc.sources.clear(); sc.receivers.clear();
    bool anySolo = false;
    for (size_t s = 0; s < cfg.size(); ++s) anySolo |= sourceSolo[s]->load() >= .5f;
    const double sf=modeled?engineSize.load()*.01:sizeParam->load()*.01;
    if(!modeled)verb::scaleRoomGeometry(sc,sf);
    const double top = (sc.ceilMat < 0 ? 30.*sf : sc.height)/sf;
    std::array<verbp::MovementPath, verbp::kMaxSources + 1> rawPaths;
    for (int o = 0; o < (int)cfg.size(); ++o) rawPaths[(size_t)o] = getMovementPath(o);
    rawPaths[(size_t)verbp::kListener] = getMovementPath(verbp::kListener);
    // E19: resolve every azimuth from the same positions as the audio thread (Look at depends on the target's position).
    double orient[verbp::kMaxSources + 1][4] = {};
    {
        double tan[verbp::kMaxSources + 1] = {}; bool act[verbp::kMaxSources + 1] = {};
        for (int o = 0; o <= verbp::kMaxSources; ++o) {
            if (o >= (int)cfg.size() && o != verbp::kListener) continue;
            for (int k = 0; k < 4; ++k) orient[o][k] = pv[o][k]->load();
            const auto& rp = rawPaths[(size_t)o];
            if (rp.enabled && rp.points.size() >= 2) {
                verbp::CompiledPath c; c.build(rp);
                const int driver = verbp::pathGroupDriver(rawPaths, (int)cfg.size(), o);
                const double u = pathParams[driver]->load();
                double yaw = 0; const auto q = c.evaluate(u, &yaw);
                for (int k = 0; k < 3; ++k) orient[o][k] = q[(size_t)k];
                orient[o][3] = c.evaluateAzimuth(u, orient[o][3]);
                tan[o] = yaw; act[o] = true;
            }
        }
        verbp::applyOrientation((int)cfg.size(), orient, tan, act, [&](int o) -> const verbp::MovementPath& { return rawPaths[(size_t)o]; });
    }
    for (const auto& pm : verbp::buildPoints(cfg)) {
        const int s = pm.source; const auto& conf = cfg[(size_t)s];
        if ((source >= 0 && source != s) || sourceMute[s]->load() >= .5f
            || (anySolo && sourceSolo[s]->load() < .5f) || pv[s][4]->load() <= -60.f) continue;
        double ox = pm.radius * std::cos(pm.angleDeg * verb::kPi / 180.);
        double oy = pm.radius * std::sin(pm.angleDeg * verb::kPi / 180.), oz = 0;
        if (conf.freePoints && pm.channel >= 0 && conf.ox.size() > (size_t)pm.channel
            && conf.oy.size() > (size_t)pm.channel && conf.oz.size() > (size_t)pm.channel) {
            ox = conf.ox[(size_t)pm.channel]; oy = conf.oy[(size_t)pm.channel]; oz = conf.oz[(size_t)pm.channel];
        }
        double sx = pv[s][0]->load(), sy = pv[s][1]->load(), sz = pv[s][2]->load(), saz = pv[s][3]->load();
        const auto& sp = rawPaths[(size_t)s];
        if (sp.enabled && sp.points.size() >= 2) {
            verbp::CompiledPath csp; csp.build(sp);
            const int driver = verbp::pathGroupDriver(rawPaths, (int)cfg.size(), s);
            double yaw = 0; const auto q = csp.evaluate(pathParams[driver]->load(), &yaw);
            sx = q[0]; sy = q[1]; sz = q[2];
            (void)yaw;
        }
        saz = orient[s][3];
        verb::Transducer t; t.name = conf.name;
        t.azimuthDeg = saz;
        t.pattern = verbp::effectiveAnalyticPattern(conf);
        t.instrument = conf.instrument;
        t.instrumentAmount = conf.instrumentAmount;
        t.aimAzDeg = conf.aimAzDeg;
        t.aimElDeg = conf.aimElDeg;
        t.aimRollDeg = conf.aimRollDeg;
        t.directivity = conf.directivity;
        t.gainDb = pv[s][4]->load();
        const double a = t.azimuthDeg * verb::kPi / 180., ca = std::cos(a), sa = std::sin(a);
        t.pos = {sx + ca * ox - sa * oy, sy + sa * ox + ca * oy,
            std::clamp(sz + oz, 0., top)};
        t.pos=t.pos*sf;
        sc.sources.push_back(t);
    }
    // Omni coverage probes have no dependency on the host's receiver layout.
    sc.hasListener = false; sc.listenerPos = {}; sc.listenerYaw = 0;
    sc.floorEditor = {}; sc.ceilEditor = {}; sc.layerModes = {};
    for (auto& w : sc.walls) w.editor = {};
    for (auto& z : sc.floorZones) z.editor = {};
    for (auto& z : sc.ceilZones) z.editor = {};
    return sc;
}

bool VerbProcessor::autoMakeupDb(float& value) const {
    std::shared_ptr<const verb::AutoLevelEnergy> energy;
    { juce::SpinLock::ScopedLockType lock(engineLock); if(bundle) { if(bundle->engine->autoLevelMoving())return false; energy=bundle->engine->autoLevelEnergy(); } }
    if(!energy) return false;
    const auto cfg=getSources(); const auto points=verbp::buildPoints(cfg);
    if(energy->points.size()!=points.size()) return false;
    bool solo=false; for(size_t s=0;s<cfg.size();++s) solo |= sourceSolo[s]->load()>=.5f;
    double sum=0; int count=0;
    std::vector<double> perSource(cfg.size(),0.); std::vector<int> perSourceCount(cfg.size(),0);
    const bool creative=modeParam->load()>=.5f;
    const double eg=creative?std::pow(10.,earlyParam->load()/10.):1., tg=creative?std::pow(10.,tailParam->load()/10.):1.;
    for(size_t i=0;i<points.size();++i) {
        const int s=points[i].source;
        if(sourceMute[s]->load()>=.5f || (solo&&sourceSolo[s]->load()<.5f)) continue;
        const auto& p=energy->points[i]; double e=0;
        for(int b=0;b<verb::NB;++b)e+=p.direct[b]+eg*p.early[b]+tg*p.late[b];
        perSource[s]+=e/verb::NB*std::pow(10.,pv[s][4]->load()/10.); ++perSourceCount[s];
    }
    for(size_t s=0;s<cfg.size();++s)if(perSourceCount[s]) {sum+=perSource[s]/perSourceCount[s];++count;}
    if(!count) return false;
    value=(float)std::clamp(-10.*std::log10(std::max(sum/count,1e-30)),0.,36.);
    return true;
}

// ---------- room library / IR export ----------
juce::String VerbProcessor::getRoomText() const {
    verb::Scene sc;
    std::vector<verbp::SourceCfg> cfg;
    {
        juce::ScopedLock l(dispLock);
        sc = display;
        cfg = srcCfg;
    }
    sc.sources.clear();
    for (size_t i = 0; i < cfg.size(); ++i) {  // the current positions, not the ones the scene file started with
        verb::Transducer t;
        t.name = cfg[i].name;
        t.pos = {pv[i][0]->load(), pv[i][1]->load(), pv[i][2]->load()};
        t.azimuthDeg = pv[i][3]->load();
        t.pattern = cfg[i].pattern;
        t.instrument = cfg[i].instrument;
        t.instrumentAmount = cfg[i].instrumentAmount;
        t.aimAzDeg = cfg[i].aimAzDeg;
        t.aimElDeg = cfg[i].aimElDeg;
        t.aimRollDeg = cfg[i].aimRollDeg;
        t.directivity = cfg[i].directivity;
        t.gainDb = pv[i][4]->load();
        sc.sources.push_back(t);
    }
    sc.hasListener = true;
    sc.listenerPos = {pv[verbp::kListener][0]->load(), pv[verbp::kListener][1]->load(), pv[verbp::kListener][2]->load()};
    sc.listenerYaw = pv[verbp::kListener][3]->load();
    return juce::String(verb::serializeScene(sc, 6, std::numeric_limits<double>::max_digits10));
}

juce::String VerbProcessor::getRoomPresetText() {
    if (getLoadState() != 2) return {};
    juce::MemoryBlock data;
    getStateInformation(data);
    auto stateXml = getXmlFromBinary(data.getData(), (int)data.getSize());
    if (!stateXml) return {};
    while (auto* rp = stateXml->getChildByName("RecordedPass")) stateXml->removeChildElement(rp, true);
    stateXml->setAttribute("sceneText", getRoomText());
    juce::XmlElement preset("VerbRoomPreset");
    preset.setAttribute("formatVersion", 2);
   #ifdef JucePlugin_VersionString
    preset.setAttribute("pluginVersion", JucePlugin_VersionString);
   #endif
    preset.addChildElement(stateXml.release());
    return preset.toString();
}

bool VerbProcessor::loadRoomPresetText(const juce::String& text, juce::String& error) {
    error.clear();
    auto xml = juce::parseXML(text);
    const int format = xml ? xml->getIntAttribute("formatVersion") : 0;
    if (!xml || !xml->hasTagName("VerbRoomPreset") || (format != 1 && format != 2)) {
        error = "Unsupported or damaged Verb Room preset (expected format version 1 or 2).";
        return false;
    }
    auto* payload = xml->getChildByName("VerbState");
    auto st = payload ? juce::ValueTree::fromXml(*payload) : juce::ValueTree();
    const auto sources = st.getChildWithName("Sources");
    if (!st.isValid() || !st.getChildWithName("Output").isValid() || sources.getNumChildren() < 1
        || sources.getNumChildren() > verbp::kMaxSources) {
        error = "The preset is missing its source or output setup.";
        return false;
    }
    std::vector<verbp::SourceCfg> setup;
    for (auto source : sources) {
        verbp::SourceCfg c;
        c.format = (int)source.getProperty("format", -1);
        c.channels = (int)source.getProperty("channels", 0);
        c.firstIn = (int)source.getProperty("firstIn", -1);
        c.sum = (bool)source.getProperty("sum", false);
        if (!source.hasType("Src") || c.format < 0 || c.format >= (int)verbp::sourceFormats().size()
            || c.channels < 1 || c.channels > 16 || c.firstIn < 0
            || c.firstIn + c.effectiveChannels() > verbp::kMaxInputChannels) {
            error = "The preset contains an invalid source format or input channel range.";
            return false;
        }
        const auto customData = source.getProperty("customDirectivityData", "").toString().toStdString();
        if (customData.size() > verb::kMaxVerbdirBytes) {
            error = "The preset's embedded custom directivity exceeds the 64 KB limit.";
            return false;
        }
        setup.push_back(c);
    }
    if (verbp::buildPoints(setup).size() > verbp::kMaxPoints) {
        error = "The preset exceeds the limit of 64 spatial points.";
        return false;
    }
    try {
        std::istringstream input(st.getProperty("sceneText").toString().toStdString());
        const auto sc = verb::parseScene(input, "preset");
        if (sc.sources.size() != (size_t)sources.getNumChildren() || sc.walls.size() > 128)
            throw std::runtime_error("Scene and source setup do not match, or there are more than 128 walls.");
    } catch (const std::exception& e) {
        error = juce::String(e.what());
        return false;
    }
    // Version 0.1.10 presets predate positioned dry; retain their original bypass behaviour.
    if (format == 1 && !st.getChildWithProperty("id", "spatialDry").isValid()) {
        juce::ValueTree param("PARAM"); param.setProperty("id", "spatialDry", nullptr);
        param.setProperty("value", 0.f, nullptr); st.appendChild(param, nullptr);
    }
    // Reject incomplete parameter lists instead of inheriting arbitrary values from the previous room.
    if(!st.getChildWithProperty("id","mode").isValid()&&!st.getChildWithProperty("id","early").isValid()) {
        juce::ValueTree param("PARAM");param.setProperty("id","early",nullptr);param.setProperty("value",-9.f,nullptr);st.appendChild(param,nullptr);
    }
    const auto current = apvts.copyState();
    for (auto param : current) {
        const auto id = param.getProperty("id");
        if (id.isVoid()) continue;
        auto saved = st.getChildWithProperty("id", id);
        const auto name = id.toString();
        if (!saved.isValid() && (name == "mode" || name == "absorption" || name == "decayLow" || name == "decayMid" || name == "decayHigh" || name == "size" || name == "predelay" || name == "stereoWidth" || name == "freeze" || name == "duck" || name == "duckRelease" || name == "makeup" || name.endsWith("path"))) continue;
        if (!saved.isValid() || !saved.hasProperty("value") || !std::isfinite((double)saved.getProperty("value"))) {
            error = "The preset is missing a valid parameter: " + id.toString();
            return false;
        }
    }
    juce::MemoryBlock localData;
    getStateInformation(localData);
    auto localXml = getXmlFromBinary(localData.getData(), (int)localData.getSize());
    const auto local = juce::ValueTree::fromXml(*localXml);
    for (const char* key : {"keepRouting", "inputMeters", "mixerCompact", "navLocked", "showOutliner"})
        st.setProperty(key, local.getProperty(key), nullptr);
    if (local.hasProperty("keepProcessing")) st.setProperty("keepProcessing", true, nullptr); else st.removeProperty("keepProcessing", nullptr);   // host-level preference stays local
    if (keepRoutingEnabled()) {
        for (const char* child : {"Sources", "Output"}) {
            st.removeChild(st.getChildWithName(child), nullptr);
            st.appendChild(local.getChildWithName(child).createCopy(), nullptr);
        }
        {
            const auto localSourcesCount = (int)getSources().size();
            const auto localMod = modRoutesFromValueTree(local.getChildWithName("Modulation"));
            const auto presetMod = modRoutesFromValueTree(st.getChildWithName("Modulation"));
            std::vector<verbp::ModulationRoute> merged;
            for (const auto& r : localMod) {
                if ((int)merged.size() >= verbp::kMaxModRoutes) break;
                if (verbp::modTargetIsRoomGlobal(r.target)) continue;
                const bool objOk = (r.object == verbp::kListener) || (r.object >= 0 && r.object < localSourcesCount);
                const bool tgtOk = verbp::modTargetIsGlobal(r.target)
                                || (r.targetObject < 0 && r.object < localSourcesCount)
                                || (r.targetObject >= 0 && r.targetObject < localSourcesCount);
                if (objOk && tgtOk) merged.push_back(r);
            }
            for (const auto& r : presetMod) {
                if ((int)merged.size() >= verbp::kMaxModRoutes) break;
                if (!verbp::modTargetIsRoomGlobal(r.target)) continue;
                const bool objOk = (r.object == verbp::kListener) || (r.object >= 0 && r.object < localSourcesCount);
                if (objOk) merged.push_back(r);
            }
            st.removeChild(st.getChildWithName("Modulation"), nullptr);
            if (!merged.empty()) st.appendChild(modRoutesToValueTree(merged), nullptr);
        }
        if (st.hasProperty("autoXMin") && st.hasProperty("autoXMax")
            && st.hasProperty("autoYMin") && st.hasProperty("autoYMax")
            && st.hasProperty("autoZMin") && st.hasProperty("autoZMax")) {
            verbp::AutomationRange ar;
            ar.xMin = (double)st.getProperty("autoXMin", ar.xMin);
            ar.xMax = (double)st.getProperty("autoXMax", ar.xMax);
            ar.yMin = (double)st.getProperty("autoYMin", ar.yMin);
            ar.yMax = (double)st.getProperty("autoYMax", ar.yMax);
            ar.zMin = (double)st.getProperty("autoZMin", ar.zMin);
            ar.zMax = (double)st.getProperty("autoZMax", ar.zMax);
            ar = verbp::AutomationRange::sanitised(ar);
            if (!ar.isDefault()) {
                const auto localSources = getSources();
                const auto localOutput = getOutputCfg();
                auto expandObjExtents = [&](int obj, double x, double y, double z, double azDeg, const verbp::SourceCfg* cfg) {
                    ar.expandPoint(x, y, z);
                    if (cfg && !cfg->sum) {
                        const int nCh = cfg->effectiveChannels();
                        if (cfg->freePoints && nCh > 0) {
                            const double rad = azDeg * verb::kPi / 180.0;
                            const double cs = std::cos(rad), sn = std::sin(rad);
                            const auto pts = verbp::buildPoints(std::vector<verbp::SourceCfg>{*cfg});
                            for (const auto& pt : pts) {
                                if (pt.channel < 0 || pt.channel >= (int)cfg->ox.size()) continue;
                                const double ox = (double)cfg->ox[(size_t)pt.channel];
                                const double oy = (double)cfg->oy[(size_t)pt.channel];
                                const double oz = (double)cfg->oz[(size_t)pt.channel];
                                ar.expandPoint(x + ox * cs - oy * sn, y + ox * sn + oy * cs, z + oz);
                            }
                        } else if (nCh > 1 && cfg->spread > 0.0) {
                            ar.expandPoint(x - cfg->spread, y - cfg->spread, z);
                            ar.expandPoint(x + cfg->spread, y + cfg->spread, z);
                        }
                    } else if (!cfg && localOutput.radius > 0.0) {
                        ar.expandPoint(x - localOutput.radius, y - localOutput.radius, z);
                        ar.expandPoint(x + localOutput.radius, y + localOutput.radius, z);
                    }
                };
                for (int i = 0; i < (int)localSources.size() && i < verbp::kMaxSources; ++i) {
                    expandObjExtents(i, pv[i][0]->load(), pv[i][1]->load(), pv[i][2]->load(), pv[i][3]->load(), &localSources[(size_t)i]);
                    const auto lp = getMovementPath(i);
                    for (const auto& pt : lp.points) ar.expandPoint(pt[0], pt[1], pt[2]);
                    if (lp.lookAt && lp.lookPoint) ar.expandPoint(lp.lookPos[0], lp.lookPos[1], lp.lookPos[2]);
                }
                expandObjExtents(verbp::kListener, pv[verbp::kListener][0]->load(), pv[verbp::kListener][1]->load(), pv[verbp::kListener][2]->load(), pv[verbp::kListener][3]->load(), nullptr);
                {
                    const auto lp = getMovementPath(verbp::kListener);
                    for (const auto& pt : lp.points) ar.expandPoint(pt[0], pt[1], pt[2]);
                    if (lp.lookAt && lp.lookPoint) ar.expandPoint(lp.lookPos[0], lp.lookPos[1], lp.lookPos[2]);
                }
                auto paramVal = [&](const juce::String& id, double fallback) -> double {
                    auto p = st.getChildWithProperty("id", id);
                    return p.isValid() && p.hasProperty("value") ? (double)p.getProperty("value") : fallback;
                };
                for (int i = 0; i < (int)localSources.size() && i < verbp::kMaxSources; ++i) {
                    expandObjExtents(i, paramVal(verbp::paramId(i, 0), 0.0), paramVal(verbp::paramId(i, 1), 0.0),
                                     paramVal(verbp::paramId(i, 2), 1.5), paramVal(verbp::paramId(i, 3), 0.0), &localSources[(size_t)i]);
                }
                expandObjExtents(verbp::kListener, paramVal("lx", 0.0), paramVal("ly", 0.0), paramVal("lz", 1.5), paramVal("laz", 0.0), nullptr);
                if (auto pathsTree = st.getChildWithName("MovementPaths"); pathsTree.isValid()) {
                    for (auto pathNode : pathsTree) {
                        for (auto pt : pathNode)
                            if (pt.hasType("Point"))
                                ar.expandPoint((double)pt.getProperty("x", 0.0), (double)pt.getProperty("y", 0.0), (double)pt.getProperty("z", 0.0));
                        if ((bool)pathNode.getProperty("lookAt", false) && (bool)pathNode.getProperty("lookPoint", false))
                            ar.expandPoint((double)pathNode.getProperty("lookX", 0.0), (double)pathNode.getProperty("lookY", 0.0), (double)pathNode.getProperty("lookZ", 1.5));
                    }
                }
                ar = verbp::AutomationRange::sanitised(ar);
                if (ar.isDefault()) {
                    for (const char* prop : {"autoXMin", "autoXMax", "autoYMin", "autoYMax", "autoZMin", "autoZMax"})
                        st.removeProperty(prop, nullptr);
                } else {
                    st.setProperty("autoXMin", ar.xMin, nullptr);
                    st.setProperty("autoXMax", ar.xMax, nullptr);
                    st.setProperty("autoYMin", ar.yMin, nullptr);
                    st.setProperty("autoYMax", ar.yMax, nullptr);
                    st.setProperty("autoZMin", ar.zMin, nullptr);
                    st.setProperty("autoZMax", ar.zMax, nullptr);
                }
            }
        }
    }
    juce::MemoryBlock data;
    auto stateXml = st.createXml();
    copyXmlToBinary(*stateXml, data);
    setStateInformation(data.getData(), (int)data.getSize());
    return true;
}

juce::String VerbProcessor::getExportMessage() const {
    juce::ScopedLock l(dispLock);
    return exportMsg;
}

void VerbProcessor::startExportIR(const juce::File& dir) {
    if (exportState.load() == 1) return;
    if (exportThread.joinable()) exportThread.join();
    exportCancel = false;
    exportState = 1;
    {
        juce::ScopedLock l(dispLock);
        exportMsg = "Exporting impulse responses ...";
    }
    exportThread = std::thread([this, dir] { runExport(dir); });
}

void VerbProcessor::runExport(juce::File dir) {
    auto setMsg = [&](const juce::String& m) {
        juce::ScopedLock l(dispLock);
        exportMsg = m;
    };
    try {
        std::vector<verbp::SourceCfg> cfg;
        juce::String name;
        {
            juce::ScopedLock l(dispLock);
            cfg = srcCfg;
            name = sceneName;
        }
        auto points = verbp::buildPoints(cfg);
        if (points.empty()) throw std::runtime_error("no sources");
        const double sr = builtSr.load() > 0 ? builtSr.load() : (hostSr > 0 ? hostSr : 48000.0);
        const int hostCh = std::max(1, builtForHostCh.load() > 0 ? builtForHostCh.load() : hostOutCh.load());
        if (!dir.createDirectory()) throw std::runtime_error("cannot create " + dir.getFullPathName().toStdString());
        juce::String base = juce::File::createLegalFileName(juce::File(name).getFileNameWithoutExtension());
        if (base.isEmpty()) base = "room";
        // A private processor reproduces the same routing, seeded furniture, shared
        // field, current listener pose and all mix controls as the live state.
        // Rendering a static engine from one source used to change both the shared
        // tail and the listener's orientation, and started before level ramps settled.
        juce::MemoryBlock snapshot;getStateInformation(snapshot);
        std::shared_ptr<verb::Engine> liveEngine;
        {juce::SpinLock::ScopedLockType lock(engineLock);if(bundle)liveEngine=bundle->engine;}
        if(!liveEngine)throw std::runtime_error("room is not ready");
        VerbProcessor offline;
        offline.offlineRender=true;
        offline.setPlayConfigDetails(std::max(1,hostInCh.load()),hostCh,sr,512);
        offline.prepareToPlay(sr,512);
        offline.setStateInformation(snapshot.getData(),(int)snapshot.getSize());
        const auto timeout=std::chrono::steady_clock::now()+std::chrono::seconds(120);
        juce::AudioBuffer<float> warm(std::max({hostCh,hostInCh.load(),1}),512);juce::MidiBuffer midi;
        auto cancelled=[&]{if(exportCancel.load())throw std::runtime_error("cancelled");};
        while(offline.getLoadState()!=2){cancelled();if(std::chrono::steady_clock::now()>timeout)throw std::runtime_error("offline room timed out");if(offline.getLoadState()==3)throw std::runtime_error(offline.getStatus().toStdString());juce::Thread::sleep(10);}
        const auto poseUntil=std::chrono::steady_clock::now()+std::chrono::milliseconds(650);
        do {cancelled();if(std::chrono::steady_clock::now()>timeout)throw std::runtime_error("offline tail timed out");warm.clear();offline.processBlock(warm,midi);juce::Thread::sleep(10);}
        while(std::chrono::steady_clock::now()<poseUntil||!offline.isTailCurrent());
        auto& eng=*offline.bundle->engine;
        if(!eng.copyTailForOffline(*liveEngine))throw std::runtime_error("room topology changed during export; retry");
        const int R=eng.numReceivers();
        const int exportedChannels = std::min(R, hostCh);
        const auto output = offline.getOutputCfg();
        const bool ambisonic = offline.bundle->ambisonic;
        const int format = ambisonic ? offline.ambisonicsFormat.load(std::memory_order_relaxed) : 0;
        const verbp::AmbisonicsOutput conversion(format);
        const size_t total=(size_t)std::ceil(eng.tailSeconds()*sr);
        std::vector<std::vector<float>> pointInput(points.size(),std::vector<float>(512));
        std::vector<const float*> ip;for(const auto& ch:pointInput)ip.push_back(ch.data());
        std::vector<std::vector<float>> block((size_t)R,std::vector<float>(512));
        std::vector<float*> op;for(auto& ch:block)op.push_back(ch.data());
        const double master=juce::Decibels::decibelsToGain(offline.gainParam->load()+offline.makeupParam->load());
        int solo=0;for(int k=0;k<3;++k)if(offline.soloParam[k]->load()>=.5f)solo|=1<<k;
        const bool positioned=offline.spatialDryParam->load()>=.5f;
        const double mix=solo?1.:std::clamp(offline.mixParam->load()*.01,0.,1.);
        const double wet=positioned?1.:std::sin(mix*verb::kPi*.5),dry=positioned?0.:std::cos(mix*verb::kPi*.5);
        bool anySolo=false;for(size_t s=0;s<cfg.size();++s)anySolo|=offline.sourceSolo[s]->load()>=.5f;
        juce::StringArray used;
        int written = 0;
        for (size_t p = 0; p < points.size(); ++p) {
            if (exportCancel.load()) throw std::runtime_error("cancelled");
            setMsg("Exporting impulse responses: " + juce::String((int)p + 1) + " / " + juce::String((int)points.size()) + " ...");
            const verbp::PointMap& pm = points[p];
            const verbp::SourceCfg& c = cfg[(size_t)pm.source];
            std::vector<std::vector<float>> out((size_t)R, std::vector<float>(total, 0.f));
            // Flush the previous point without rebuilding the room or changing RNG seeds.
            for(size_t pos=0;pos<total+512;pos+=512){cancelled();eng.process(ip.data(),op.data(),512);}
            const bool enabled=offline.sourceMute[pm.source]->load()<.5f&&(!anySolo||offline.sourceSolo[pm.source]->load()>=.5f);
            const float sourceGain=enabled?juce::Decibels::decibelsToGain(offline.pv[pm.source][4]->load(),-60.f):0.f;
            for (size_t pos = 0; pos < total; pos += 512) {
                cancelled();
                const int n = (int)std::min<size_t>(512, total - pos);
                pointInput[p][0]=pos==0?sourceGain:0.f;
                eng.process(ip.data(), op.data(), n);
                for(int r=0;r<R;++r)for(int i=0;i<n;++i)out[(size_t)r][pos+i]=(float)(block[(size_t)r][i]*master*wet);
            }
            pointInput[p][0]=0.f;
            // The point IR is driven at the engine-point input (after an optional
            // channel sum); raw bypass goes to that point's corresponding host channel.
            const int rawChannel=c.firstIn+std::max(0,pm.channel);
            if(enabled&&rawChannel<exportedChannels&&!solo)out[(size_t)rawChannel][0]+=(float)dry;
            juce::String fname = base + "_" + juce::File::createLegalFileName(juce::String(c.name));
            if (pm.channel >= 0 && c.effectiveChannels() > 1) fname += "_ch" + juce::String(pm.channel + 1);
            for (int k = 2; used.contains(fname); ++k) fname = fname.upToLastOccurrenceOf("_#", false, false) + "_#" + juce::String(k);
            used.add(fname);
            juce::File f = dir.getChildFile(fname + ".wav");
            f.deleteFile();
            std::unique_ptr<juce::FileOutputStream> fos(f.createOutputStream());
            if (!fos) throw std::runtime_error("cannot write " + f.getFullPathName().toStdString());
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(fos.get(), sr, (unsigned)exportedChannels, 32, {}, 0));
            if (!w) throw std::runtime_error("cannot create the WAV writer");
            fos.release();  // the writer owns the stream now
            juce::AudioBuffer<float> ab(exportedChannels, (int)total);
            for (int r = 0; r < exportedChannels; ++r) {
                const int source = conversion.acn(r);
                if (source >= R) ab.clear(r, 0, (int)total);
                else {
                    ab.copyFrom(r, 0, out[(size_t)source].data(), (int)total);
                    if (format != 0) ab.applyGain(r, 0, (int)total, conversion.gain(r));
                }
            }
            w->writeFromAudioSampleBuffer(ab, 0, (int)total);
            w.reset();
            ++written;
        }
        auto description = offline.getRoomText();
        if (ambisonic) {
            description += "\n# Ambisonics normalization: ";
            description += verbp::ambisonicsNormalizationNames[(size_t)output.ambisonicsNormalization];
            description += "\n# Ambisonics level: " + juce::String(output.ambisonicsLevelDb) + " dB\n";
        }
        dir.getChildFile(base + "_room.txt").replaceWithText(description);
        setMsg("Exported " + juce::String(written) + " impulse response file(s) and the room to " + dir.getFullPathName());
        exportState = 2;
    } catch (const std::exception& ex) {
        setMsg(juce::String("Export failed: ") + ex.what());
        exportState = 3;
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VerbProcessor(); }

verbp::MovementPath VerbProcessor::getMovementPath(int object) const {
    juce::ScopedLock lock(pathLock);return object>=0 && object<=verbp::kMaxSources?movementPaths[(size_t)object]:verbp::MovementPath{};
}
bool VerbProcessor::setMovementPath(int object,const verbp::MovementPath& path) {
    if(object<0 || object>verbp::kMaxSources || !verbp::validPath(path))return false;
    {juce::ScopedLock lock(pathLock);movementPaths[(size_t)object]=path;pathMailboxes[(size_t)object].publish(path);}
    bumpRevision();updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));return true;
}
std::vector<verbp::ModulationRoute> VerbProcessor::getModulationRoutes() const {
    juce::ScopedLock lock(modLock);
    return modRoutes;
}
bool VerbProcessor::setModulationRoutes(const std::vector<verbp::ModulationRoute>& routes) {
    if ((int)routes.size() > verbp::kMaxModRoutes) return false;
    for (const auto& r : routes)
        if (!verbp::validModRoute(r)) return false;
    {
        juce::ScopedLock lock(modLock);
        modRoutes = routes;
        modMailbox->publish(modRoutes.data(), (int)modRoutes.size());
        modConfiguredCount.store((int)modRoutes.size(), std::memory_order_release);
    }
    bumpRevision();
    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
    return true;
}

bool VerbProcessor::webModTelemetry(ModLiveSample* out, int maxCount, int& outCount) const noexcept {
    const int cfgCount = std::clamp(modConfiguredCount.load(std::memory_order_acquire), 0, verbp::kMaxModRoutes);
    outCount = cfgCount;
    if (!out || maxCount <= 0 || cfgCount == 0) return false;
    const uint64_t liveGen = modLiveGen.load(std::memory_order_acquire);
    const uint64_t wantGen = modMailbox ? modMailbox->publishedGeneration() : 0;
    const int liveCount = modLiveCount.load(std::memory_order_relaxed);
    const bool fresh = isCreativeMode() && (wantGen != 0 && liveGen == wantGen && liveCount == cfgCount);
    const int nCopy = std::min(cfgCount, maxCount);
    for (int i = 0; i < nCopy; ++i) {
        if (fresh) {
            out[i].raw = modLiveRaw[(size_t)i].load(std::memory_order_relaxed);
            out[i].value = modLiveValue[(size_t)i].load(std::memory_order_relaxed);
            out[i].active = modLiveActive[(size_t)i].load(std::memory_order_relaxed) != 0;
        } else {
            out[i] = ModLiveSample{};
        }
    }
    return fresh;
}
