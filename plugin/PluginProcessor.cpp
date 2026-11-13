#include "PluginProcessor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

#include "EditorSession.h"
#include "InputOrder.h"
#include "tail.hpp"
#include "PluginEditor.h"
#if VERB_WEB_UI_ENABLED
#include "VerbWebEditor.h"
#endif

namespace {

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

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout() {
    using namespace verbp;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    auto add = [&](const juce::String& id, const juce::String& name, float lo, float hi, float def, const char* unit) {
        p.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{id, 1}, name, juce::NormalisableRange<float>(lo, hi, 0.001f), def,
                                                                juce::AudioParameterFloatAttributes().withLabel(unit)));
    };
    for (int o = 0; o <= kMaxSources; ++o) {
        const juce::String nm = o == kListener ? "Listener" : "Source " + juce::String(o + 1);
        add(paramId(o, 0), nm + " X", -200.f, 200.f, 0.f, "m");
        add(paramId(o, 1), nm + " Y", -200.f, 200.f, 0.f, "m");
        add(paramId(o, 2), nm + " Z", 0.f, 30.f, 1.5f, "m");
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
    return {p.begin(), p.end()};
}

}  // namespace

VerbProcessor::VerbProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Sources", juce::AudioChannelSet::discreteChannels(2), true)
                         .withOutput("Room", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("Verb scene loader"),
      apvts(*this, nullptr, "VerbState", makeLayout()) {
    for (int o = 0; o <= verbp::kMaxSources; ++o)
        for (int k = 0; k < (o == verbp::kListener ? 4 : 5); ++k) pv[o][k] = apvts.getRawParameterValue(verbp::paramId(o, k));
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
    return nOut >= 1 && nOut <= verbp::kMaxReceivers && nIn <= verbp::kMaxInputChannels;
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
    lfeSoloGain.reset(sr, 0.01);
    bool needRebuild;
    {
        juce::SpinLock::ScopedLockType l(engineLock);
        needRebuild = !bundle || std::fabs(builtSr.load() - sr) > 0.5 || bundle->maxBlock < maxBlock;
    }
    {
        juce::ScopedLock l(dispLock);
        if (outCfg.layout < 0 && builtForHostCh.load() != hostOutCh.load()) needRebuild = true;  // "auto" follows the track
    }
    if (needRebuild) rebuild();
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
        serialSeen = ser;
    }
    const double wallGap = haveLastProcessWall ? std::chrono::duration<double>(blockStart - lastProcessWall).count() : 0.0;
    const bool gapJump = !newEngine && haveLastProcessWall && wallGap > std::max(0.25, 8.0 * blockSec);
    lastProcessWall = blockStart;
    haveLastProcessWall = true;
    bool transportJump = false;
    if (auto* ph = getPlayHead()) {
        if (const auto posInfo = ph->getPosition()) {
            const bool isPlaying = posInfo->getIsPlaying();
            if (const auto ts = posInfo->getTimeInSamples()) {
                const int64_t curTs = *ts;
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
            if (largeStep || fastStep || angJump) { objJumped[o] = true; anyJumped = true; }
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
            if (cnt > 0) {  // source gain, ramped over the host block
                const float g0 = prevGain[pm.source], g1 = curGain[pm.source];
                for (int i = 0; i < m; ++i) d[i] *= g0 + (g1 - g0) * (float)(done + i + 1) / (float)n;
            }
        }
        if (b.lfeOut >= 0)
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
                    if (objJumped[o]) return cur[o][k];
                    return prev[o][k] + std::remainder(cur[o][k] - prev[o][k], 360.0) * t;
                }
                const HermSeg& sg = hs[o][k];
                return h00 * sg.p0 + h10 * sg.m0 + h01 * sg.p1 + h11 * sg.m1;
            };
            for (int s = 0; s < NS; ++s) {
                const double az = mix(s, 3), x = mix(s, 0), y = mix(s, 1), z = mix(s, 2);
                const double ca = std::cos(az * verb::kPi / 180.0), sa = std::sin(az * verb::kPi / 180.0);
                for (int p = b.pointStart[(size_t)s]; p < b.pointStart[(size_t)s] + b.pointCount[(size_t)s]; ++p) {
                    // live offset in the source's frame (x forward along the azimuth, y left, z up)
                    const double ox = curPtOff[p][0], oy = curPtOff[p][1], oz = curPtOff[p][2];
                    e.setSourcePosition(p, (x + ca * ox - sa * oy)*sizeScale, (y + sa * ox + ca * oy)*sizeScale, std::min(std::max(z + oz, 0.0), zTop)*sizeScale);
                    e.setSourceAzimuth(p, az);
                    e.setSourcePattern(p, srcPattern[s].load(std::memory_order_relaxed));
                }
            }
            e.setListener(mix(verbp::kListener, 0)*sizeScale, mix(verbp::kListener, 1)*sizeScale, mix(verbp::kListener, 2)*sizeScale, mix(verbp::kListener, 3));
            for (int p = 0; p < P; ++p) ip[p] = b.in[(size_t)p].data() + off;
            for (int r = 0; r < R; ++r) op[r] = b.out[(size_t)r].data() + off;
            e.acknowledgePoseScale(appliedSize/b.constructionSize);
            e.process(ip, op, len, b.detector.data()+off);
            duckReduction.store(e.duckReductionDb(),std::memory_order_relaxed);
        }
        for (int ch = 0; ch < nOut; ++ch) {
            float* d = buf.getWritePointer(ch) + done;
            bool mapped = false;
            float gate0 = 0.f, gate1 = 0.f;
            for (int s = 0; s < NS; ++s) if (ch >= b.cfg[s].firstIn && ch < b.cfg[s].firstIn + b.cfg[s].effectiveChannels()) {
                mapped = true; gate0 = std::max(gate0, prevSourceGate[s]); gate1 = std::max(gate1, sourceGate[s]);
            }
            if (!mapped) { gate0 = prevAnySourceSolo ? 0.f : 1.f; gate1 = anySourceSolo ? 0.f : 1.f; }
            const bool hasDry = ch < nIn && (dryG1 > 0.f || dryG0 > 0.f);  // d still holds this channel's input here
            auto wetAt = [&](int i) { return wetG0 + (wetG1 - wetG0) * (float)(done + i + 1) / (float)n; };
            auto dryAt = [&](int i) {
                const float t = (float)(done + i + 1) / (float)n;
                return (dryG0 + (dryG1 - dryG0) * t) * (gate0 + (gate1 - gate0) * t);
            };
            if (ch < R) {
                const float* s = b.out[(size_t)ch].data();
                if (ch == b.lfeOut) {
                    for (int i = 0; i < m; ++i) {
                        const float dry = hasDry ? (std::fabs(d[i]) < 1e6f ? d[i] : 0.f) * dryAt(i) : 0.f;
                        d[i] = (s[i] + lfeSoloGain.getNextValue() * b.lfe[(size_t)i]) * outGain * wetAt(i) + dry;
                    }
                } else {
                    for (int i = 0; i < m; ++i) {
                        const float dry = hasDry ? (std::fabs(d[i]) < 1e6f ? d[i] : 0.f) * dryAt(i) : 0.f;
                        d[i] = s[i] * outGain * wetAt(i) + dry;
                    }
                }
                for (int i = 0; i < m; ++i) if (!(std::fabs(d[i]) < 1e6f)) d[i] = 0.f;  // last line of defence: never hand NaN/Inf to the host
            } else {
                for (int i = 0; i < m; ++i) {  // beyond the layout: silent, except for the dry signal
                    const float dry = hasDry ? (std::fabs(d[i]) < 1e6f ? d[i] : 0.f) * dryAt(i) : 0.f;
                    d[i] = dry;
                }
            }
        }
        for(int ch=0;ch<nOut;++ch) juce::FloatVectorOperations::multiply(buf.getWritePointer(ch)+done,makeupGain,m);
        done += m;
    }
    prevWet = wetG1;
    prevDry = dryG1;
    for (int i = 0; i < NS; ++i) { std::memcpy(prev[i], cur[i], sizeof prev[i]); prevGain[i] = curGain[i]; prevSourceGate[i] = sourceGate[i]; }
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
        st.setProperty("sceneName", sceneName, nullptr);
        st.setProperty("inputMeters", inputMetersOn.load(), nullptr);
        st.setProperty("keepRouting", keepRouting.load(), nullptr);
        st.setProperty("mixerCompact", mixerCompact.load(), nullptr);
        st.setProperty("navLocked", navLocked.load(), nullptr);
        st.setProperty("showOutliner", showOutliner.load(), nullptr);
        st.setProperty("tailRays", tailSettings.rays, nullptr);
        st.setProperty("adaptiveTail", tailSettings.adaptive, nullptr);
        st.removeChild(st.getChildWithName("Sources"), nullptr);
        st.removeChild(st.getChildWithName("Output"), nullptr);
        juce::ValueTree srcs("Sources");
        for (const auto& c : srcCfg) {
            juce::ValueTree v("Src");
            v.setProperty("name", juce::String(c.name), nullptr);
            v.setProperty("format", c.format, nullptr);
            v.setProperty("channels", c.channels, nullptr);
            v.setProperty("firstIn", c.firstIn, nullptr);
            v.setProperty("sum", c.sum, nullptr);
            v.setProperty("spread", c.spread, nullptr);
            v.setProperty("pattern", c.pattern, nullptr);
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
        st.appendChild(srcs, nullptr);
        juce::ValueTree out("Output");
        out.setProperty("layout", outCfg.layout, nullptr);
        out.setProperty("radius", outCfg.radius, nullptr);
        out.setProperty("micPattern", outCfg.micPattern, nullptr);
        st.appendChild(out, nullptr);
    }
    if (auto xml = st.createXml()) copyXmlToBinary(*xml, dest);
}

void VerbProcessor::setStateInformation(const void* data, int size) {
    auto xml = getXmlFromBinary(data, size);
    if (!xml) return;
    auto st = juce::ValueTree::fromXml(*xml);
    if (!st.isValid()) return;
    {
        juce::ScopedLock l(dispLock);
        srcCfg.clear();
        auto srcs = st.getChildWithName("Sources");
        for (int i = 0; i < srcs.getNumChildren() && i < verbp::kMaxSources; ++i) {
            auto v = srcs.getChild(i);
            verbp::SourceCfg c;
            c.name = v.getProperty("name", "Source").toString().toStdString();
            c.format = std::min(std::max((int)v.getProperty("format", 0), 0), (int)verbp::sourceFormats().size() - 1);
            c.channels = std::min(std::max((int)v.getProperty("channels", 1), 1), 16);
            c.firstIn = std::min(std::max((int)v.getProperty("firstIn", 0), 0), verbp::kMaxInputChannels - 1);
            c.sum = (bool)v.getProperty("sum", false);
            c.spread = verb::clampTo((double)v.getProperty("spread", 0.5), 0.0, 20.0, 0.5);
            c.pattern = verb::clampTo((double)v.getProperty("pattern", 0.0), 0.0, 1.0, 0.0);
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
        outCfg.layout = std::min(std::max(outCfg.layout, -1), (int)verb::Layout::Ring);  // an unknown layout id must not reach the engine
        outCfg.radius = verb::clampTo(out.isValid() ? (double)out.getProperty("radius", 0.1) : 0.1, 0.0, 1.0, 0.1);
        outCfg.micPattern = std::min(std::max(out.isValid() ? (int)out.getProperty("micPattern", 0) : 0, 0), (int)verb::MicPattern::HumanEars);
        tailSettings.rays = std::min(std::max((int)st.getProperty("tailRays", 48000), 8000), 384000);
        tailSettings.adaptive = (bool)st.getProperty("adaptiveTail", false);
        syncPatterns();
    }
    inputMetersOn = (bool)st.getProperty("inputMeters", true);
    keepRouting = (bool)st.getProperty("keepRouting", false);
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
    { juce::ScopedLock l(reqLock); hasReq=false; defaultsOwed=false; reqSerial.fetch_add(1); }
    apvts.replaceState(st);
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
    juce::String t, n;
    uint64_t edits;
    {
        juce::ScopedLock l(dispLock);
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
        sceneText = verb::serializeScene(display);  // the project state keeps the edited room
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
        sceneText = verb::serializeScene(display);
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
    setSources(v);
}

void VerbProcessor::syncPatterns() {  // dispLock must be held
    for (int s = 0; s < verbp::kMaxSources; ++s)
        srcPattern[s].store(s < (int)srcCfg.size() ? (float)srcCfg[(size_t)s].pattern : 0.f);
}

void VerbProcessor::setSourcePattern(int index, double pattern) {
    pattern = verb::clampTo(pattern, 0.0, 1.0, 0.0);
    juce::ScopedLock l(dispLock);
    if (index < 0 || index >= (int)srcCfg.size()) return;
    srcCfg[(size_t)index].pattern = pattern;
    srcPattern[index].store((float)pattern);
    ++structRevision;  // not part of sceneSerial; the command revision must cover it
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

void VerbProcessor::setOutputCfg(const verbp::OutputCfg& o) {
    {
        juce::ScopedLock l(dispLock);
        outCfg = o;
        outCfg.layout = std::min(std::max(outCfg.layout, -1), (int)verb::Layout::Ring);
        outCfg.radius = verb::clampTo(outCfg.radius, 0.0, 1.0, 0.1);
        outCfg.micPattern = std::min(std::max(outCfg.micPattern, 0), (int)verb::MicPattern::HumanEars);
        ++sceneSerial; ++structRevision;
    }
    rebuild();
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

void VerbProcessor::publishWebDiagnostics() {
    if (!bundle) { webTailCurrent = false; return; }
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
    while (!threadShouldExit()) {
        wake.wait(250);
        if (threadShouldExit()) break;
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
                if (useDefaults && !r.keepRouting && !r.setup.empty()) {  // the room brings its own sources (e.g. the car's engine stems)
                    srcCfg = r.setup;
                } else if ((useDefaults && !r.keepRouting) || srcCfg.empty()) {
                    srcCfg.clear();
                    for (size_t i = 0; i < sc.sources.size() && (int)i < verbp::kMaxSources; ++i) {
                        verbp::SourceCfg c;
                        c.name = sc.sources[i].name;
                        c.firstIn = (int)i;
                        srcCfg.push_back(c);
                    }
                    if (srcCfg.empty()) srcCfg.push_back(verbp::SourceCfg{});
                }
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
                t.name = cfg[(size_t)pm.source].name + (pm.channel >= 0 ? "." + std::to_string(pm.channel + 1) : "");
                const double az = srcState(pm.source, 3), a = (az + pm.angleDeg) * verb::kPi / 180.0;
                t.pos = {srcState(pm.source, 0) + pm.radius * std::cos(a), srcState(pm.source, 1) + pm.radius * std::sin(a), srcState(pm.source, 2)};
                t.azimuthDeg = az;
                t.pattern = cfg[(size_t)pm.source].pattern;
                t.pos=t.pos*sizeFactor;
                es.sources.push_back(t);
            }
            es.receivers = verb::makeReceiverArray(lay, center, ocfg.radius, std::min(hostCh, verbp::kMaxReceivers));
            verb::applyMicPattern(es.receivers, (verb::MicPattern)ocfg.micPattern, ocfg.radius);
            if ((int)es.receivers.size() > verbp::kMaxReceivers) es.receivers.resize(verbp::kMaxReceivers);
            // channels the track cannot carry are not rendered (no CPU for sound nobody hears)
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
            nb->maxBlock = std::max(maxBlock, 128);
            nb->in.assign(points.size(), std::vector<float>((size_t)nb->maxBlock, 0.f));
            nb->out.assign((size_t)nb->R, std::vector<float>((size_t)nb->maxBlock, 0.f));
            nb->detector.assign((size_t)nb->maxBlock,0.f);
            nb->lfe.assign((size_t)nb->maxBlock, 0.f);
            const auto spk = verb::speakersFor(lay, hostCh);
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
                juce::SpinLock::ScopedLockType l(engineLock);
                bundle.swap(nb);
                engineSize=requestedSize;engineAbsorption=ec.absorptionScale; appliedGeometryEpoch=geometryEpoch.load();
                updateBounds(sc);
                ++loadSerial;
            }
            publishWebDiagnostics();
            nb.reset();  // old engine destroyed here, off the audio thread
            builtSr = r.sr;
            numPts = (int)points.size();
            builtOutCh = (int)es.receivers.size();
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
        verb::Transducer t; t.name = conf.name;
        t.azimuthDeg = pv[s][3]->load(); t.pattern = conf.pattern; t.gainDb = pv[s][4]->load();
        const double a = t.azimuthDeg * verb::kPi / 180., ca = std::cos(a), sa = std::sin(a);
        t.pos = {pv[s][0]->load() + ca * ox - sa * oy, pv[s][1]->load() + sa * ox + ca * oy,
            std::clamp((double)pv[s][2]->load() + oz, 0., top)};
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
        t.gainDb = pv[i][4]->load();
        sc.sources.push_back(t);
    }
    sc.hasListener = true;
    sc.listenerPos = {pv[verbp::kListener][0]->load(), pv[verbp::kListener][1]->load(), pv[verbp::kListener][2]->load()};
    sc.listenerYaw = pv[verbp::kListener][3]->load();
    return juce::String(verb::serializeScene(sc));
}

juce::String VerbProcessor::getRoomPresetText() {
    if (getLoadState() != 2) return {};
    juce::MemoryBlock data;
    getStateInformation(data);
    auto stateXml = getXmlFromBinary(data.getData(), (int)data.getSize());
    if (!stateXml) return {};
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
        if (!saved.isValid() && (name == "mode" || name == "absorption" || name == "decayLow" || name == "decayMid" || name == "decayHigh" || name == "size" || name == "predelay" || name == "stereoWidth" || name == "freeze" || name == "duck" || name == "duckRelease" || name == "makeup")) continue;
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
    if (keepRoutingEnabled()) {
        for (const char* child : {"Sources", "Output"}) {
            st.removeChild(st.getChildWithName(child), nullptr);
            st.appendChild(local.getChildWithName(child).createCopy(), nullptr);
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
            if(enabled&&rawChannel<R&&!solo)out[(size_t)rawChannel][0]+=(float)dry;
            juce::String fname = base + "_" + juce::File::createLegalFileName(juce::String(c.name));
            if (pm.channel >= 0 && c.effectiveChannels() > 1) fname += "_ch" + juce::String(pm.channel + 1);
            for (int k = 2; used.contains(fname); ++k) fname = fname.upToLastOccurrenceOf("_#", false, false) + "_#" + juce::String(k);
            used.add(fname);
            juce::File f = dir.getChildFile(fname + ".wav");
            f.deleteFile();
            std::unique_ptr<juce::FileOutputStream> fos(f.createOutputStream());
            if (!fos) throw std::runtime_error("cannot write " + f.getFullPathName().toStdString());
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(fos.get(), sr, (unsigned)R, 32, {}, 0));
            if (!w) throw std::runtime_error("cannot create the WAV writer");
            fos.release();  // the writer owns the stream now
            juce::AudioBuffer<float> ab(R, (int)total);
            for (int r = 0; r < R; ++r) ab.copyFrom(r, 0, out[(size_t)r].data(), (int)total);
            w->writeFromAudioSampleBuffer(ab, 0, (int)total);
            w.reset();
            ++written;
        }
        dir.getChildFile(base + "_room.txt").replaceWithText(offline.getRoomText());
        setMsg("Exported " + juce::String(written) + " impulse response file(s) and the room to " + dir.getFullPathName());
        exportState = 2;
    } catch (const std::exception& ex) {
        setMsg(juce::String("Export failed: ") + ex.what());
        exportState = 3;
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VerbProcessor(); }
