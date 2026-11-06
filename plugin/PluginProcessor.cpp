#include "PluginProcessor.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "InputOrder.h"
#include "tail.hpp"
#include "PluginEditor.h"

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
    add("early", "Early Reflections", -60.f, 6.f, -9.f, "dB");  // 0 dB = strict physical level
    add("diffusion", "Early Diffusion", 0.f, 1.f, 0.5f, "");
    add("doppler", "Doppler Strength", 0.f, 2.f, 1.f, "");  // 1 = physical, 0 = no pitch shift
    add("tail", "Tail Level", -48.f, 12.f, 0.f, "dB");      // the late reverberant field alone (direct and early reflections unchanged)
    add("mix", "Dry/Wet Mix", 0.f, 100.f, 100.f, "%");      // 100 = only the room (the usual setup: dry stays on the source tracks)
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
    gainParam = apvts.getRawParameterValue("gain");
    earlyParam = apvts.getRawParameterValue("early");
    diffusionParam = apvts.getRawParameterValue("diffusion");
    dopplerParam = apvts.getRawParameterValue("doppler");
    mixParam = apvts.getRawParameterValue("mix");
    tailParam = apvts.getRawParameterValue("tail");
    sceneText = kDefaultScene;
    sceneName = "Built-in demo room";
    startThread();
    loadSceneText(sceneText, sceneName, true);
}

VerbProcessor::~VerbProcessor() {
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
    juce::ScopedNoDenormals noDenormals;
    const int n = buf.getNumSamples(), nIn = getTotalNumInputChannels(), nOut = getTotalNumOutputChannels();
    for (int ch = 0; ch < verbp::kMaxInputChannels; ++ch)
        inPeaks[(size_t)ch].store(ch < nIn && inMap[(size_t)ch] < buf.getNumChannels() ? buf.getMagnitude(inMap[(size_t)ch], 0, n) : 0.f, std::memory_order_relaxed);
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
    for (int i = 0; i < NS; ++i) {
        for (int k = 0; k < 4; ++k) cur[i][k] = pv[i][k]->load(std::memory_order_relaxed);
        cur[i][2] = std::min(std::max(cur[i][2], 0.0), zTop);
        curGain[i] = juce::Decibels::decibelsToGain(pv[i][4]->load(std::memory_order_relaxed), -60.f);
    }
    for (int k = 0; k < 4; ++k) cur[verbp::kListener][k] = pv[verbp::kListener][k]->load(std::memory_order_relaxed);
    cur[verbp::kListener][2] = std::min(std::max(cur[verbp::kListener][2], 0.0), zTop);
    const uint64_t ser = loadSerial.load();
    if (ser != serialSeen) {  // new engine: start from the current values, no glide
        for (int i = 0; i < NS; ++i) { std::memcpy(prev[i], cur[i], sizeof prev[i]); prevGain[i] = curGain[i]; }
        std::memcpy(prev[verbp::kListener], cur[verbp::kListener], sizeof prev[verbp::kListener]);
        serialSeen = ser;
    }
    const float outGain = juce::Decibels::decibelsToGain(gainParam->load(std::memory_order_relaxed));
    e.setEarlyLevel(earlyParam->load(std::memory_order_relaxed));
    e.setEarlyDiffusion(diffusionParam->load(std::memory_order_relaxed));
    e.setDopplerStrength(dopplerParam->load(std::memory_order_relaxed));
    e.setTailLevel(tailParam->load(std::memory_order_relaxed));
    // dry/wet: equal-power crossfade between the input channels passed straight through (channel k -> output k) and the room
    const float mixP = std::min(std::max(mixParam->load(std::memory_order_relaxed) * 0.01f, 0.f), 1.f);
    float wetG1 = std::sin(mixP * 1.5707963f), dryG1 = std::cos(mixP * 1.5707963f);
    if (mixP >= 0.9999f) { wetG1 = 1.f; dryG1 = 0.f; }
    if (mixP <= 0.0001f) { wetG1 = 0.f; dryG1 = 1.f; }
    const float wetG0 = prevWet, dryG0 = prevDry;

    // The automation value read at the start of a host block is the position AT THE START of that block. The block is
    // rendered along a cubic Hermite curve that leaves where the previous block ended (continuous position and velocity:
    // a straight line per block makes the velocity jump at every block boundary, heard as steps in the Doppler pitch) and
    // ends at the position predicted for the end of this block (velocity and acceleration extrapolated from the last two
    // blocks: no extra latency, the old scheme lagged the automation by a whole block).
    struct HermSeg { double p0, p1, m0, m1; };
    HermSeg hs[verbp::kMaxSources + 1][3];
    {
        // An alpha-beta tracker follows position and velocity of each coordinate. The measurement is the automation value
        // at the block start; it may arrive irregularly (a mouse drag writes ~60 times a second whatever the block size, a
        // sparse envelope less often): blocks without a new value must not read as "stopped", so the gains are set by a
        // time constant, not by the block, and the velocity is a smoothed estimate.
        const double sampleRate = std::max(8000.0, builtSr.load() > 0 ? builtSr.load() : hostSr);
        const double alpha = 1.0 - std::exp(-(double)n / (0.020 * sampleRate)), beta = alpha * alpha / (2.0 - alpha);
        auto build = [&](int o) {
            for (int k = 0; k < 3; ++k) {
                const double c0 = cur[o][k];
                if (ser != hSeen || std::fabs(c0 - hPEnd[o][k]) > 20.0) {  // a new engine or a jump of the automation: start afresh here
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
                if (k >= 3) return prev[o][k] + (cur[o][k] - prev[o][k]) * t;  // azimuth/yaw: plain glide
                const HermSeg& sg = hs[o][k];
                return h00 * sg.p0 + h10 * sg.m0 + h01 * sg.p1 + h11 * sg.m1;
            };
            for (int s = 0; s < NS; ++s) {
                const double az = mix(s, 3), x = mix(s, 0), y = mix(s, 1), z = mix(s, 2);
                const double ca = std::cos(az * verb::kPi / 180.0), sa = std::sin(az * verb::kPi / 180.0);
                for (int p = b.pointStart[(size_t)s]; p < b.pointStart[(size_t)s] + b.pointCount[(size_t)s]; ++p) {
                    // live offset in the source's frame (x forward along the azimuth, y left, z up)
                    const double ox = ptOff[p][0].load(std::memory_order_relaxed), oy = ptOff[p][1].load(std::memory_order_relaxed), oz = ptOff[p][2].load(std::memory_order_relaxed);
                    e.setSourcePosition(p, x + ca * ox - sa * oy, y + sa * ox + ca * oy, std::min(std::max(z + oz, 0.0), zTop));
                    e.setSourceAzimuth(p, az);
                    e.setSourcePattern(p, srcPattern[s].load(std::memory_order_relaxed));
                }
            }
            e.setListener(mix(verbp::kListener, 0), mix(verbp::kListener, 1), mix(verbp::kListener, 2), mix(verbp::kListener, 3));
            for (int p = 0; p < P; ++p) ip[p] = b.in[(size_t)p].data() + off;
            for (int r = 0; r < R; ++r) op[r] = b.out[(size_t)r].data() + off;
            e.process(ip, op, len);
        }
        for (int ch = 0; ch < nOut; ++ch) {
            float* d = buf.getWritePointer(ch) + done;
            const bool hasDry = ch < nIn && (dryG1 > 0.f || dryG0 > 0.f);  // d still holds this channel's input here
            auto wetAt = [&](int i) { return wetG0 + (wetG1 - wetG0) * (float)(done + i + 1) / (float)n; };
            auto dryAt = [&](int i) { return dryG0 + (dryG1 - dryG0) * (float)(done + i + 1) / (float)n; };
            if (ch < R) {
                const float* s = b.out[(size_t)ch].data();
                if (ch == b.lfeOut) {
                    for (int i = 0; i < m; ++i) {
                        const float dry = hasDry ? (std::fabs(d[i]) < 1e6f ? d[i] : 0.f) * dryAt(i) : 0.f;
                        d[i] = (s[i] + b.lfe[(size_t)i]) * outGain * wetAt(i) + dry;
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
        done += m;
    }
    prevWet = wetG1;
    prevDry = dryG1;
    for (int i = 0; i < NS; ++i) { std::memcpy(prev[i], cur[i], sizeof prev[i]); prevGain[i] = curGain[i]; }
    std::memcpy(prev[verbp::kListener], cur[verbp::kListener], sizeof prev[verbp::kListener]);
}

juce::AudioProcessorEditor* VerbProcessor::createEditor() { return new VerbEditor(*this); }

// ---------- state ----------
void VerbProcessor::getStateInformation(juce::MemoryBlock& dest) {
    auto st = apvts.copyState();
    {
        juce::ScopedLock l(dispLock);
        st.setProperty("sceneText", sceneText, nullptr);
        st.setProperty("sceneName", sceneName, nullptr);
        st.setProperty("inputMeters", inputMetersOn.load(), nullptr);
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
        syncPatterns();
    }
    inputMetersOn = (bool)st.getProperty("inputMeters", true);
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
    {
        juce::ScopedLock l(reqLock);
        req.text = text.toStdString();
        req.name = name.toStdString();
        req.setup = nextSetup;
        req.micPattern = nextMic;
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
    {
        juce::ScopedLock l(dispLock);
        t = sceneText;
        n = sceneName;
    }
    loadSceneText(t, n, false, true);
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

void VerbProcessor::editScene(const std::function<void(verb::Scene&)>& fn) {
    {
        juce::ScopedLock l(dispLock);
        verb::Scene tmp = display;
        fn(tmp);
        verb::sanitizeScene(tmp);  // e.g. an endpoint dragged onto the other one would leave a zero-length wall
        verb::compactMaterials(tmp);
        if ((int)tmp.walls.size() > 128 || tmp.floorMat < 0 || tmp.floorMat >= (int)tmp.mats.size() || tmp.ceilMat >= (int)tmp.mats.size()) return;
        for (auto& w : tmp.walls)
            if (w.mat < 0 || w.mat >= (int)tmp.mats.size()) return;
        tmp.height = std::min(std::max(tmp.height, 1.0), 30.0);
        display = tmp;
        sceneText = verb::serializeScene(display);  // the project state keeps the edited room
        updateBounds(display);
        ++sceneSerial;
    }
    geomDirty = true;
    wake.signal();
}

// ---------- sources / outputs ----------
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
        ++sceneSerial;
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
    setSources(v);
    return idx;
}

void VerbProcessor::removeSource(int index) {
    std::vector<verbp::SourceCfg> v = getSources();
    if (v.size() <= 1 || index < 0 || index >= (int)v.size()) return;
    for (int i = index; i + 1 < (int)v.size(); ++i)  // later sources move up with their positions and gains
        for (int k = 0; k < 5; ++k) setParamValue(verbp::paramId(i, k), pv[i + 1][k]->load());
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
}

void VerbProcessor::setSourceSpread(int index, double radius) {
    radius = std::min(std::max(radius, 0.0), 20.0);
    juce::ScopedLock l(dispLock);
    if (index < 0 || index >= (int)srcCfg.size()) return;
    srcCfg[(size_t)index].spread = radius;
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
        for (auto& w : s.walls) {
            w.ax = cx + (w.ax - cx) * f; w.ay = cy + (w.ay - cy) * f;
            w.bx = cx + (w.bx - cx) * f; w.by = cy + (w.by - cy) * f;
        }
        if (includeObjects) {
            s.height = std::min(std::max(s.height * f, 1.0), 30.0);
            for (auto& w : s.walls) {  // partial-height walls scale with the room
                w.z0 *= f;
                if (w.z1 < 1e8) w.z1 *= f;
            }
        }
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
        ++sceneSerial;
    }
    rebuild();
}

// ---------- loader thread ----------
void VerbProcessor::run() {
    while (!threadShouldExit()) {
        wake.wait(250);
        if (threadShouldExit()) break;
        if (geomDirty.exchange(false)) {  // only this thread replaces the bundle, so reading it here is safe
            verb::Scene s;
            {
                juce::ScopedLock l(dispLock);
                s = display;
            }
            if (bundle) bundle->engine->setScene(s);
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
            if (!(juce::SystemStats::hasAVX2() && juce::SystemStats::hasFMA3()))
                throw std::runtime_error("This CPU has no AVX2/FMA support (needed by the room engine)");
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
                if (useDefaults && !r.setup.empty()) {  // the room brings its own sources (e.g. the car's engine stems)
                    srcCfg = r.setup;
                } else if (useDefaults || srcCfg.empty()) {
                    srcCfg.clear();
                    for (size_t i = 0; i < sc.sources.size() && (int)i < verbp::kMaxSources; ++i) {
                        verbp::SourceCfg c;
                        c.name = sc.sources[i].name;
                        c.firstIn = (int)i;
                        srcCfg.push_back(c);
                    }
                    if (srcCfg.empty()) srcCfg.push_back(verbp::SourceCfg{});
                }
                if (useDefaults && r.micPattern >= 0) outCfg.micPattern = std::min(std::max(r.micPattern, 0), (int)verb::MicPattern::HumanEars);
                if (useDefaults && r.micPattern == (int)verb::MicPattern::HumanEars) outCfg.radius = 0.0875;  // ears at the head half-width
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
            verb::Scene es = sc;
            es.sources.clear();
            for (const auto& pm : points) {
                verb::Transducer t;
                t.name = cfg[(size_t)pm.source].name + (pm.channel >= 0 ? "." + std::to_string(pm.channel + 1) : "");
                const double az = srcState(pm.source, 3), a = (az + pm.angleDeg) * verb::kPi / 180.0;
                t.pos = {srcState(pm.source, 0) + pm.radius * std::cos(a), srcState(pm.source, 1) + pm.radius * std::sin(a), srcState(pm.source, 2)};
                t.azimuthDeg = az;
                t.pattern = cfg[(size_t)pm.source].pattern;
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
            ec.tailThread = true;
            ec.sharedTail = points.size() > 4;  // many sources: one late-field convolver per output channel
            ec.tailRays = ec.sharedTail ? 48000 : 20000;
            ec.maxEarlyDelaySec = ec.sharedTail ? 0.4 : 0.75;

            auto nb = std::make_unique<Bundle>();
            nb->engine = std::make_unique<verb::Engine>(es, ec);
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
            if (useDefaults && reqSerial.load() == myRequest && defaultsOwed.exchange(false)) applyDefaultsFrom(sc, home);
            {
                juce::SpinLock::ScopedLockType l(engineLock);
                bundle.swap(nb);
                tailSeconds = bundle->engine->tailSeconds() + 0.25;  // the host keeps calling us until the tail has died away
                updateBounds(sc);
                ++loadSerial;
            }
            nb.reset();  // old engine destroyed here, off the audio thread
            builtSr = r.sr;
            numPts = (int)points.size();
            builtOutCh = (int)es.receivers.size();
            builtLayout = (int)lay;
            builtForHostCh = hostCh;
            {
                juce::ScopedLock l(dispLock);
                display = sc;
                ++sceneSerial;
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
        sc.sources.push_back(t);
    }
    sc.hasListener = true;
    sc.listenerPos = {pv[verbp::kListener][0]->load(), pv[verbp::kListener][1]->load(), pv[verbp::kListener][2]->load()};
    sc.listenerYaw = pv[verbp::kListener][3]->load();
    return juce::String(verb::serializeScene(sc));
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
        verb::Scene sc;
        std::vector<verbp::SourceCfg> cfg;
        verbp::OutputCfg ocfg;
        juce::String name;
        {
            juce::ScopedLock l(dispLock);
            sc = display;
            cfg = srcCfg;
            ocfg = outCfg;
            name = sceneName;
        }
        const auto points = verbp::buildPoints(cfg);
        if (points.empty()) throw std::runtime_error("no sources");
        const double sr = builtSr.load() > 0 ? builtSr.load() : (hostSr > 0 ? hostSr : 48000.0);
        sc.sr = sr;
        const int hostCh = std::max(1, builtForHostCh.load() > 0 ? builtForHostCh.load() : hostOutCh.load());
        const verb::Layout lay = (verb::Layout)builtLayout.load();
        const verb::Vec3 center{pv[verbp::kListener][0]->load(), pv[verbp::kListener][1]->load(), pv[verbp::kListener][2]->load()};
        const double lyaw = pv[verbp::kListener][3]->load();
        auto recv = verb::makeReceiverArray(lay, center, ocfg.radius, std::min(hostCh, verbp::kMaxReceivers));
        verb::applyMicPattern(recv, (verb::MicPattern)ocfg.micPattern, ocfg.radius);
        if ((int)recv.size() > verbp::kMaxReceivers) recv.resize(verbp::kMaxReceivers);
        if ((int)recv.size() > hostCh) recv.resize((size_t)hostCh);
        const double zTop = sc.ceilMat < 0 ? 30.0 : sc.height;
        if (!dir.createDirectory()) throw std::runtime_error("cannot create " + dir.getFullPathName().toStdString());
        juce::String base = juce::File::createLegalFileName(juce::File(name).getFileNameWithoutExtension());
        if (base.isEmpty()) base = "room";
        const double tailLen = verb::effectiveTailSeconds(sc, 1);
        const size_t total = (size_t)std::ceil(tailLen * sr);
        juce::StringArray used;
        int written = 0;
        for (size_t p = 0; p < points.size(); ++p) {
            if (exportCancel.load()) throw std::runtime_error("cancelled");
            setMsg("Exporting impulse responses: " + juce::String((int)p + 1) + " / " + juce::String((int)points.size()) + " ...");
            const verbp::PointMap& pm = points[p];
            const verbp::SourceCfg& c = cfg[(size_t)pm.source];
            const double sx = pv[pm.source][0]->load(), sy = pv[pm.source][1]->load(), sz = pv[pm.source][2]->load(), az = pv[pm.source][3]->load();
            const double ca = std::cos(az * verb::kPi / 180.0), sa = std::sin(az * verb::kPi / 180.0);
            const double ox = ptOff[p][0].load(), oy = ptOff[p][1].load(), oz = ptOff[p][2].load();
            verb::Transducer t;
            t.name = c.name;
            t.pos = {sx + ca * ox - sa * oy, sy + sa * ox + ca * oy, std::min(std::max(sz + oz, 0.0), zTop)};
            t.azimuthDeg = az;
            t.pattern = c.pattern;
            verb::Scene es = sc;
            es.length = tailLen;
            es.sources = {t};
            es.receivers = recv;
            es.hasListener = true;
            es.listenerPos = center;
            es.listenerYaw = 0;
            verb::EngineConfig ec;
            ec.erOrder = 1;  // the same early field the plugin plays
            ec.ctrlInterval = 128;
            ec.tailBlock = 512;
            ec.tailThread = false;  // the tail is computed once, synchronously: a much denser trace than the live refresh
            ec.sharedTail = false;
            ec.tailRays = 150000;
            verb::Engine eng(es, ec);
            eng.setListener(center.x, center.y, center.z, lyaw);
            const int R = eng.numReceivers();
            std::vector<std::vector<float>> out((size_t)R, std::vector<float>(total, 0.f));
            std::vector<float> in(512, 0.f);
            std::vector<float> blk((size_t)R * 512, 0.f);
            std::vector<float*> op((size_t)R);
            for (size_t pos = 0; pos < total; pos += 512) {
                const int n = (int)std::min<size_t>(512, total - pos);
                std::fill(in.begin(), in.end(), 0.f);
                if (pos == 0) in[0] = 1.f;
                const float* ip[1] = {in.data()};
                for (int r = 0; r < R; ++r) op[(size_t)r] = &blk[(size_t)r * 512];
                eng.process(ip, op.data(), n);
                for (int r = 0; r < R; ++r) std::copy(op[(size_t)r], op[(size_t)r] + n, out[(size_t)r].begin() + (long)pos);
            }
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
        dir.getChildFile(base + "_room.txt").replaceWithText(getRoomText());
        setMsg("Exported " + juce::String(written) + " impulse response file(s) and the room to " + dir.getFullPathName());
        exportState = 2;
    } catch (const std::exception& ex) {
        setMsg(juce::String("Export failed: ") + ex.what());
        exportState = 3;
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VerbProcessor(); }
