#pragma once
// P8: CPU load in Creative mode and with Ambisonics / many output channels.
// Env:
//   VERB_ONLY_P8=1              run P8 suite
//   VERB_P8_CTEST=1             fast smoke + Creative click check for default ctest
//   VERB_P8_TAG=baseline        tag for output CSV / impulse dumps (out/P8/p8_<tag>.csv)
//   VERB_P8_MODE=matrix|accept|profile|click|impulse|switch|flate|mono|all
//   VERB_P8_SECONDS=1.0         audio seconds per trial
//   VERB_P8_TRIALS=3            trials per cell (medians reported)
//   VERB_P8_ACCEPT_SEC=60       duration for 4-core N=20 bs=256 AmbiX3 acceptance runs
//   VERB_P8_N_LIST=1,5,10,20    comma-separated N list
//   VERB_P8_BS_LIST=128,256,512 comma-separated buffer size list
//   VERB_P8_LAYOUT_LIST=Stereo,LCR,AmbiX1,5.1,AmbiX2,AmbiX3
//   VERB_P8_CREATIVE_LIST=Physical,Creative
//   VERB_P8_SCENARIO_LIST=rest,lmove,smove,cauto

#include "P7Test.h"

namespace verb_p8 {

struct LayoutSpec {
    verb::Layout layout;
    const char* name;
    int channels;
    int hostChannels;
};

inline const std::vector<LayoutSpec>& allLayouts() {
    static const std::vector<LayoutSpec> v = {
        {verb::Layout::StereoORTF, "Stereo", 2,  2},
        {verb::Layout::LCR,        "LCR",    3,  3},
        {verb::Layout::AmbiX1,     "AmbiX1", 4,  4},
        {verb::Layout::Surround51, "5.1",    6,  6},
        {verb::Layout::AmbiX2,     "AmbiX2", 9,  10}, // REAPER even track channels or 9 direct; engine renders 9
        {verb::Layout::AmbiX3,     "AmbiX3", 16, 16},
    };
    return v;
}

inline const LayoutSpec* findLayout(const std::string& name) {
    for (const auto& l : allLayouts()) {
        if (juce::String(l.name).equalsIgnoreCase(name.c_str())) return &l;
    }
    return nullptr;
}

inline std::vector<std::string> parseStrList(const char* envName, std::vector<std::string> def) {
    const char* v = std::getenv(envName);
    if (!v || !*v) return def;
    std::vector<std::string> out;
    juce::StringArray sa;
    sa.addTokens(v, ",", "");
    for (auto& s : sa) {
        auto t = s.trim().toStdString();
        if (!t.empty()) out.push_back(t);
    }
    return out.empty() ? def : out;
}

inline bool waitTailSettled(VerbProcessor& proc, int nInCh, int nOutCh, int bs, double maxWaitSec = 30.0) {
    juce::AudioBuffer<float> buf(std::max({64, nInCh, nOutCh}), bs);
    juce::MidiBuffer midi;
    const auto t0 = std::chrono::steady_clock::now();
    // Need >= 0.55 s of audio stream time AND wall time so motionSettleSec (0.4 s),
    // tailRefreshMs (0.3 s), Hermite tracker (20 ms) and 20 ms parameter glides all settle.
    const int minSettleBlocks = std::max(24, (int)std::ceil(0.55 * 48000.0 / (double)bs));
    int settledBlocks = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < maxWaitSec) {
        buf.clear();
        proc.processBlock(buf, midi);
        auto eng = E9bAccess::engine(proc);
        if (eng && eng->tailIsCurrent() && eng->motionStats().transState == 0) {
            ++settledBlocks;
            if (settledBlocks >= minSettleBlocks) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        } else {
            settledBlocks = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return false;
}

inline void applyModeSettings(VerbProcessor& proc, bool creative, int nInCh, int nOutCh, int bs, bool waitTail = true) {
    if (!creative) {
        setP(proc, "mode", 0.0);
        setP(proc, "decayLow", 1.0);
        setP(proc, "decayMid", 1.0);
        setP(proc, "decayHigh", 1.0);
        setP(proc, "early", 0.0);
        setP(proc, "tail", 0.0);
        setP(proc, "diffusion", 0.5);
        setP(proc, "doppler", 1.0);
        setP(proc, "predelay", 0.0);
        setP(proc, "stereoWidth", 100.0);
        setP(proc, "duck", 0.0);
        setP(proc, "duckRelease", 300.0);
        setP(proc, "freeze", 0.0);
    } else {
        setP(proc, "mode", 1.0);
        setP(proc, "decayLow", 0.5);
        setP(proc, "decayMid", 1.5);
        setP(proc, "decayHigh", 2.0);
        setP(proc, "early", -3.0);
        setP(proc, "tail", 3.0);
        setP(proc, "diffusion", 0.75);
        setP(proc, "doppler", 1.4);
        setP(proc, "predelay", 10.0);
        setP(proc, "stereoWidth", 125.0);
        setP(proc, "duck", 3.0);
        setP(proc, "duckRelease", 250.0);
        setP(proc, "freeze", 0.0);
    }
    if (waitTail) {
        waitTailSettled(proc, nInCh, nOutCh, bs, 30.0);
    }
}

inline bool loadFixtureScene(
    VerbProcessor& proc,
    const std::string& fixture,
    int nSources,
    const LayoutSpec& lay,
    int prepareMaxBs,
    bool stereoSources = true,
    double sr = 48000.0)
{
    const bool isFlate = (fixture == "flate");
    const juce::File presetFile = repoFile(isFlate ? "test/Flate.verbroom" : "out/vsl-hall/presets/VSL_Grand_Hall_Audience.verbroom");
    if (!presetFile.existsAsFile()) {
        std::printf("FAIL P8: missing preset %s\n", presetFile.getFullPathName().toRawUTF8());
        return false;
    }
    auto xml = juce::parseXML(presetFile.loadFileAsString());
    if (!xml) {
        std::printf("FAIL P8: cannot parse %s\n", presetFile.getFullPathName().toRawUTF8());
        return false;
    }
    auto* st = xml->getChildByName("VerbState");
    if (!st) return false;
    while (auto* mp = st->getChildByName("MovementPaths")) st->removeChildElement(mp, true);

    if (auto* out = st->getChildByName("Output")) {
        out->setAttribute("layout", (int)lay.layout);
        out->setAttribute("radius", 0.1);
        out->setAttribute("micPattern", 0);
    }

    std::istringstream is(st->getStringAttribute("sceneText").toStdString());
    verb::Scene sc = verb::parseScene(is, isFlate ? "Flate" : "VSL");

    if (auto* srcs = st->getChildByName("Sources")) {
        if (!isFlate) {
            while (srcs->getNumChildElements() > nSources) {
                srcs->removeChildElement(srcs->getChildElement(srcs->getNumChildElements() - 1), true);
            }
            if ((int)sc.sources.size() > nSources) sc.sources.resize((size_t)nSources);
            for (int i = 0; i < srcs->getNumChildElements(); ++i) {
                auto* sEl = srcs->getChildElement(i);
                if (stereoSources) {
                    sEl->setAttribute("format", 1);
                    sEl->setAttribute("channels", 2);
                    sEl->setAttribute("firstIn", i * 2);
                    sEl->setAttribute("sum", 1);
                } else {
                    sEl->setAttribute("format", 0);
                    sEl->setAttribute("channels", 1);
                    sEl->setAttribute("firstIn", i);
                    sEl->setAttribute("sum", 0);
                }
            }
        } else {
            // Flate has 1 source; clone programmatically up to nSources
            const auto* proto = srcs->getChildElement(0);
            const verb::Transducer protoT = sc.sources.empty() ? verb::Transducer{} : sc.sources[0];
            while (srcs->getNumChildElements() < nSources && proto) {
                const int idx = srcs->getNumChildElements();
                auto* copy = new juce::XmlElement(*proto);
                copy->setAttribute("name", "Voice_" + juce::String(idx + 1));
                if (stereoSources) {
                    copy->setAttribute("format", 1);
                    copy->setAttribute("channels", 2);
                    copy->setAttribute("firstIn", idx * 2);
                    copy->setAttribute("sum", 1);
                } else {
                    copy->setAttribute("format", 0);
                    copy->setAttribute("channels", 1);
                    copy->setAttribute("firstIn", idx);
                    copy->setAttribute("sum", 0);
                }
                srcs->addChildElement(copy);

                const double sx = 8.2 + 0.7 * (idx % 5);
                const double sy = -3.2 + 1.1 * (idx / 5);
                const double sz = 1.5;
                const double saz = -112.648;
                verb::Transducer nt = protoT;
                nt.name = "Voice_" + std::to_string(idx + 1);
                nt.pos = {sx, sy, sz};
                nt.azimuthDeg = saz;
                sc.sources.push_back(nt);

                auto setXmlParam = [&](const juce::String& id, double val) {
                    for (auto* ch : st->getChildWithTagNameIterator("PARAM")) {
                        if (ch->getStringAttribute("id") == id) {
                            ch->setAttribute("value", val);
                            return;
                        }
                    }
                };
                setXmlParam(verbp::paramId(idx, 0), sx);
                setXmlParam(verbp::paramId(idx, 1), sy);
                setXmlParam(verbp::paramId(idx, 2), sz);
                setXmlParam(verbp::paramId(idx, 3), saz);
                setXmlParam(verbp::paramId(idx, 4), 0.0);
            }
            if (srcs->getNumChildElements() > 0) {
                auto* s0 = srcs->getChildElement(0);
                if (stereoSources) {
                    s0->setAttribute("format", 1);
                    s0->setAttribute("channels", 2);
                    s0->setAttribute("firstIn", 0);
                    s0->setAttribute("sum", 1);
                } else {
                    s0->setAttribute("format", 0);
                    s0->setAttribute("channels", 1);
                    s0->setAttribute("firstIn", 0);
                    s0->setAttribute("sum", 0);
                }
            }
        }
    }
    st->setAttribute("sceneText", juce::String(verb::serializeScene(sc)));

    proc.setPlayConfigDetails(64, lay.hostChannels, sr, prepareMaxBs);
    proc.prepareToPlay(sr, prepareMaxBs);
    if (!waitReady(proc, 90)) {
        std::printf("FAIL P8: initial processor prepare failed\n");
        return false;
    }
    juce::String err;
    if (!proc.loadRoomPresetText(xml->toString(), err) || !waitReady(proc, 120)) {
        std::printf("FAIL P8: loadRoomPresetText failed: %s\n", err.toRawUTF8());
        return false;
    }
    if (proc.numOutputChannels() != lay.channels || proc.effectiveLayout() != (int)lay.layout) {
        std::printf("FAIL P8: layout mismatch got ch=%d lay=%d expected ch=%d lay=%d\n",
                    proc.numOutputChannels(), proc.effectiveLayout(), lay.channels, (int)lay.layout);
        return false;
    }
    setP(proc, "mix", 100.0);
    return true;
}

struct P8Metrics : verb_p7::TrialMetrics {
    int engineUnderruns = 0;
    int preemptSpikes = 0;
    double maxCycPct = 0.0;
    double sysCpuPct = 0.0;
};

struct SysCpuSnap {
    double idleSec = 0.0;
    double kernelSec = 0.0;
    double userSec = 0.0;
};

inline SysCpuSnap takeSysCpuSnap() {
    SysCpuSnap s{};
#ifdef _WIN32
    FILETIME id{}, kr{}, us{};
    if (GetSystemTimes(&id, &kr, &us)) {
        s.idleSec = verb_p7::fileTimeSec(id);
        s.kernelSec = verb_p7::fileTimeSec(kr);
        s.userSec = verb_p7::fileTimeSec(us);
    }
#endif
    return s;
}

inline double diffSysCpuPct(const SysCpuSnap& a, const SysCpuSnap& b) {
    const double dIdle = std::max(0.0, b.idleSec - a.idleSec);
    const double dTot = std::max(0.0, (b.kernelSec - a.kernelSec) + (b.userSec - a.userSec));
    if (dTot <= 1e-9) return 0.0;
    return std::clamp(100.0 * (dTot - dIdle) / dTot, 0.0, 100.0);
}

inline P8Metrics runP8Trial(
    VerbProcessor& proc,
    int nSources,
    const LayoutSpec& lay,
    int bs,
    bool creative,
    const std::string& scenario,
    double seconds,
    const std::vector<float>& noise,
    double lx0, double ly0, double sx0, double sy0,
    bool stereoSources = true,
    bool realTimePace = true,
    bool traceSpikes = false)
{
    using namespace verb_p7;
    P8Metrics m{};
    const double sr = 48000.0;
    const double budgetUs = 1e6 * (double)bs / sr;
    const int nInChannels = std::min(64, nSources * (stereoSources ? 2 : 1));
    const int nBufChannels = std::max(64, lay.hostChannels);
    proc.prepareToPlay(sr, bs);

    juce::AudioBuffer<float> buf(nBufChannels, bs);
    juce::MidiBuffer midi;

    auto setControlAt = [&](double tSec) {
        if (scenario == "lmove") {
            const double ph = std::fmod(tSec, 3.0) / 3.0;
            const double tri = ph < 0.25 ? (4.0 * ph) : (ph < 0.75 ? (2.0 - 4.0 * ph) : (-4.0 + 4.0 * ph));
            const double dx = 0.75 * tri;
            const double dy = 0.25 * std::sin(2.0 * 3.14159265358979323846 * tSec / 3.0);
            setP(proc, "lx", lx0 + dx);
            setP(proc, "ly", ly0 + dy);
        } else if (scenario == "smove") {
            const double ph = std::fmod(tSec, 3.0) / 3.0;
            const double tri = ph < 0.25 ? (4.0 * ph) : (ph < 0.75 ? (2.0 - 4.0 * ph) : (-4.0 + 4.0 * ph));
            const double dx = 0.75 * tri;
            const double dy = 0.25 * std::sin(2.0 * 3.14159265358979323846 * tSec / 3.0);
            setP(proc, "s1x", sx0 + dx);
            setP(proc, "s1y", sy0 + dy);
        } else if (scenario == "cauto") {
            // Creative automation: decay multiplier sweeping +-20% at 5 Hz + Early/Tail gain sweep
            const double s5 = std::sin(2.0 * 3.14159265358979323846 * 5.0 * tSec);
            const double c2 = std::cos(2.0 * 3.14159265358979323846 * 2.0 * tSec);
            setP(proc, "decayMid", 1.5 * (1.0 + 0.20 * s5));
            setP(proc, "decayHigh", 2.0 * (1.0 - 0.15 * s5));
            setP(proc, "early", -3.0 + 2.0 * s5);
            setP(proc, "tail", 3.0 + 2.0 * c2);
        } else if (scenario == "cauto_decay") {
            const double s5 = std::sin(2.0 * 3.14159265358979323846 * 5.0 * tSec);
            setP(proc, "decayMid", 1.5 * (1.0 + 0.20 * s5));
            setP(proc, "decayHigh", 2.0 * (1.0 - 0.15 * s5));
        } else if (scenario == "cauto_gain") {
            const double s5 = std::sin(2.0 * 3.14159265358979323846 * 5.0 * tSec);
            const double c2 = std::cos(2.0 * 3.14159265358979323846 * 2.0 * tSec);
            setP(proc, "early", -3.0 + 2.0 * s5);
            setP(proc, "tail", 3.0 + 2.0 * c2);
        }
    };

    // Restore rest position & mode parameters
    setP(proc, "lx", lx0);
    setP(proc, "ly", ly0);
    setP(proc, "s1x", sx0);
    setP(proc, "s1y", sy0);
    if (creative) {
        setP(proc, "decayLow", 0.5);
        setP(proc, "decayMid", 1.5);
        setP(proc, "decayHigh", 2.0);
        setP(proc, "early", -3.0);
        setP(proc, "tail", 3.0);
    }

    size_t noisePos = 0;
    auto fillNoise = [&]() {
        buf.clear();
        for (int ch = 0; ch < nInChannels; ++ch) {
            float* dst = buf.getWritePointer(ch);
            for (int i = 0; i < bs; ++i) {
                dst[i] = noise[(noisePos + (size_t)i + (size_t)ch * 997u) % noise.size()];
            }
        }
        noisePos = (noisePos + (size_t)bs) % noise.size();
    };

    const int warmBlocks = std::max(16, (int)std::llround(0.25 * sr / bs));
    std::vector<double> warmCycPerUs;
    warmCycPerUs.reserve((size_t)warmBlocks);
    for (int b = 0; b < warmBlocks; ++b) {
        fillNoise();
        uint64_t c0 = 0, c1 = 0;
#ifdef _WIN32
        ULONG64 rc0 = 0, rc1 = 0;
        if (QueryThreadCycleTime(GetCurrentThread(), &rc0)) c0 = (uint64_t)rc0;
#endif
        const auto wt0 = std::chrono::steady_clock::now();
        proc.processBlock(buf, midi);
        const auto wt1 = std::chrono::steady_clock::now();
#ifdef _WIN32
        if (QueryThreadCycleTime(GetCurrentThread(), &rc1)) c1 = (uint64_t)rc1;
#endif
        const double wUs = std::chrono::duration<double, std::micro>(wt1 - wt0).count();
        if (wUs > 15.0 && wUs < 0.75 * budgetUs && c1 > c0) {
            warmCycPerUs.push_back((double)(c1 - c0) / wUs);
        }
    }
    double nomCycPerUs = 3418.0;
    if (!warmCycPerUs.empty()) {
        std::sort(warmCycPerUs.begin(), warmCycPerUs.end());
        const size_t p95Idx = std::min(warmCycPerUs.size() - 1, (size_t)std::floor(0.95 * (double)(warmCycPerUs.size() - 1)));
        nomCycPerUs = warmCycPerUs[p95Idx];
    }

    auto engine = E9bAccess::engine(proc);
    if (engine) engine->resetP7ProfileStats();
    const auto mBefore = engine ? engine->motionStats() : verb::Engine::MotionStats{};
    const CpuSnapshot cpuBefore = takeCpuSnapshot();
    const SysCpuSnap sysBefore = takeSysCpuSnap();
    const int nBlocks = std::max(10, (int)std::llround(seconds * sr / bs));
    const double actualAudioSec = (double)nBlocks * bs / sr;
    std::vector<double> usList;
    usList.reserve((size_t)nBlocks);

    HighResPacer pacer;
    const auto blockDur = std::chrono::nanoseconds((int64_t)std::llround(1e9 * (double)bs / sr));
    auto nextDeadline = std::chrono::steady_clock::now() + blockDur;
    double nextPoseSec = 0.0;
    double outEnergy = 0.0;
    bool allFinite = true;
    const bool spikeEnv = traceSpikes || (std::getenv("VERB_P8_SPIKE_TRACE") != nullptr) || (std::getenv("VERB_P7_SPIKE_TRACE") != nullptr);
    double phaseWorkPctEwma[4] = {0.0, 0.0, 0.0, 0.0};
    uint64_t prevSwaps = engine ? (engine->motionStats().legacySwaps + engine->motionStats().transitionsDone) : 0;

    const auto wallStart = std::chrono::steady_clock::now();
    for (int b = 0; b < nBlocks; ++b) {
        const double tAudio = (double)b * bs / sr;
        if (scenario != "rest" && tAudio + 1e-9 >= nextPoseSec) {
            setControlAt(tAudio);
            nextPoseSec += 1.0 / 60.0;
        }
        fillNoise();
        uint64_t cyc0 = 0, cyc1 = 0;
#ifdef _WIN32
        ULONG64 rawCyc0 = 0;
        if (QueryThreadCycleTime(GetCurrentThread(), &rawCyc0)) cyc0 = (uint64_t)rawCyc0;
#endif
        const auto t0 = std::chrono::steady_clock::now();
        proc.processBlock(buf, midi);
        const auto t1 = std::chrono::steady_clock::now();
#ifdef _WIN32
        ULONG64 rawCyc1 = 0;
        if (QueryThreadCycleTime(GetCurrentThread(), &rawCyc1)) cyc1 = (uint64_t)rawCyc1;
#endif
        const uint64_t curSwaps = engine ? (engine->motionStats().legacySwaps + engine->motionStats().transitionsDone) : 0;
        const bool hadSwap = (curSwaps != prevSwaps);
        prevSwaps = curSwaps;

        const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        const double cycUs = (cyc1 > cyc0 && nomCycPerUs > 100.0) ? std::min(us, (double)(cyc1 - cyc0) / nomCycPerUs) : us;
        const double cycPct = 100.0 * cycUs / budgetUs;
        const int phIdx = b & 3;
        const double ewmaBefore = phaseWorkPctEwma[phIdx] > 0.0 ? phaseWorkPctEwma[phIdx] : cycPct;
        m.maxCycPct = std::max(m.maxCycPct, cycPct);
        usList.push_back(us);
        if (us > budgetUs) {
            ++m.underruns;
            const bool isOffCpuPreempt = (cycPct < 100.0);
            const bool isDpcSmiStall = (!isOffCpuPreempt && !hadSwap && ewmaBefore < 70.0 && cycPct > 1.75 * ewmaBefore);
            const bool isPreempt = isOffCpuPreempt || isDpcSmiStall;
            if (isPreempt) ++m.preemptSpikes;
            else ++m.engineUnderruns;
            if (spikeEnv) {
                const char* kind = isOffCpuPreempt ? "OS_PREEMPTION" : (isDpcSmiStall ? "OS_DPC_SMI_STALL" : "ENGINE_WORK");
                std::printf("  [P8_SPIKE] %s %s N=%d bs=%d %s b=%d t=%.2fs wall=%.1fus (%.1f%%) cyc=%.2fM (work=%.1fus / %.1f%%, ewma=%.1f%%, offCpu=%.1fus) -> %s\n",
                            lay.name, creative ? "Creative" : "Physical", nSources, bs, scenario.c_str(),
                            b, tAudio, us, 100.0 * us / budgetUs, (double)(cyc1 - cyc0) * 1e-6,
                            cycUs, cycPct, ewmaBefore, std::max(0.0, us - cycUs), kind);
                std::fflush(stdout);
            }
        } else if (!hadSwap) {
            phaseWorkPctEwma[phIdx] = (phaseWorkPctEwma[phIdx] <= 0.0) ? cycPct : (0.92 * phaseWorkPctEwma[phIdx] + 0.08 * cycPct);
        }

        for (int ch = 0; ch < lay.channels; ++ch) {
            if (lay.layout == verb::Layout::Surround51 && ch == 3) continue; // LFE placeholder
            const float* rp = buf.getReadPointer(ch);
            for (int i = 0; i < bs; ++i) {
                const float v = rp[i];
                allFinite &= std::isfinite(v);
                outEnergy += (double)v * v;
            }
        }

        if (realTimePace) {
            pacer.waitUntil(nextDeadline);
            const auto after = std::chrono::steady_clock::now();
            if (after > nextDeadline + 4 * blockDur) nextDeadline = after + blockDur;
            else nextDeadline += blockDur;
        }
    }
    const auto wallEnd = std::chrono::steady_clock::now();
    const SysCpuSnap sysAfter = takeSysCpuSnap();
    const CpuSnapshot cpuAfter = takeCpuSnapshot();
    const auto mAfter = engine ? engine->motionStats() : verb::Engine::MotionStats{};
    if (engine) m.prof = engine->p7ProfileStats();
    m.sysCpuPct = diffSysCpuPct(sysBefore, sysAfter);

    // Restore rest position smoothly at end of trial (avoid >0.45m fastStep jump) and pump 24 blocks so Hermite tracker settles at rest
    if (scenario != "rest") {
        const double curLx = proc.apvts.getRawParameterValue("lx")->load();
        const double curLy = proc.apvts.getRawParameterValue("ly")->load();
        const double curSx = proc.apvts.getRawParameterValue("s1x")->load();
        const double curSy = proc.apvts.getRawParameterValue("s1y")->load();
        for (int step = 1; step <= 8; ++step) {
            const double a = (double)step / 8.0;
            setP(proc, "lx", curLx + (lx0 - curLx) * a);
            setP(proc, "ly", curLy + (ly0 - curLy) * a);
            setP(proc, "s1x", curSx + (sx0 - curSx) * a);
            setP(proc, "s1y", curSy + (sy0 - curSy) * a);
            buf.clear();
            proc.processBlock(buf, midi);
        }
        if (creative) {
            setP(proc, "decayLow", 0.5);
            setP(proc, "decayMid", 1.5);
            setP(proc, "decayHigh", 2.0);
            setP(proc, "early", -3.0);
            setP(proc, "tail", 3.0);
        }
        for (int b = 0; b < 24; ++b) {
            buf.clear();
            proc.processBlock(buf, midi);
        }
    }
    const double wallSec = std::max(1e-6, std::chrono::duration<double>(wallEnd - wallStart).count());

    double aCpu = 0, wCpu = 0;
    int actW = 0, totOther = 0;
    diffCpu(cpuBefore, cpuAfter, aCpu, wCpu, actW, totOther);

    double sumUs = 0, maxUs = 0;
    for (double v : usList) { sumUs += v; maxUs = std::max(maxUs, v); }
    std::sort(usList.begin(), usList.end());
    const double p50Us = usList[usList.size() / 2];
    const double p99Us = usList[std::min(usList.size() - 1, (size_t)std::floor(0.99 * (double)(usList.size() - 1)))];

    m.meanUs = sumUs / (double)usList.size();
    m.p50Us = p50Us;
    m.p99Us = p99Us;
    m.maxUs = maxUs;
    m.meanPct = 100.0 * m.meanUs / budgetUs;
    m.p50Pct = 100.0 * m.p50Us / budgetUs;
    m.p99Pct = 100.0 * m.p99Us / budgetUs;
    m.maxPct = 100.0 * m.maxUs / budgetUs;
    m.totalBlocks = nBlocks;
    const double denomSec = realTimePace ? std::max(actualAudioSec, wallSec) : actualAudioSec;
    m.audioCpuPerAudioSec = aCpu / actualAudioSec;
    m.workerCpuPerAudioSec = wCpu / denomSec;
    m.activeWorkers = actW;
    m.totalOtherThreads = totOther;
    m.poolWorkersReported = engine ? engine->bakeStats().workers : 0;
    m.motionEstimates = mAfter.estimates - mBefore.estimates;
    m.retraces = (mAfter.transitionsDone - mBefore.transitionsDone) + (mAfter.legacySwaps - mBefore.legacySwaps);
    m.finiteAndNonSilent = allFinite && outEnergy > 1e-12;
    return m;
}

inline bool runCreativeDragClickCheck(const LayoutSpec& lay, int bs, double seconds = 5.0) {
    const double sr = 48000.0;
    VerbProcessor proc;
    if (!loadFixtureScene(proc, "vsl", 20, lay, std::max(bs, 512), true, sr)) return false;
    applyModeSettings(proc, true, 40, lay.hostChannels, bs, true);
    proc.prepareToPlay(sr, bs);

    const int nBlocks = (int)std::llround(seconds * sr / bs);
    const size_t total = (size_t)nBlocks * (size_t)bs;
    std::vector<std::vector<float>> yCh((size_t)lay.channels, std::vector<float>(total, 0.f));
    juce::AudioBuffer<float> buf(std::max(64, lay.hostChannels), bs);
    juce::MidiBuffer midi;

    // Pre-roll 0.5 s at rest with 220 Hz steady sine on source 0
    double ph = 0.0;
    const double dph = 2.0 * 3.14159265358979323846 * 220.0 / sr;
    const int preBlocks = (int)std::llround(0.5 * sr / bs);
    for (int b = 0; b < preBlocks; ++b) {
        buf.clear();
        for (int i = 0; i < bs; ++i) {
            const float s = 0.25f * (float)std::sin(ph);
            ph += dph;
            buf.setSample(0, i, s);
            buf.setSample(1, i, s);
        }
        proc.processBlock(buf, midi);
    }

    double nextPoseSec = 0.0;
    for (int b = 0; b < nBlocks; ++b) {
        const double tAudio = (double)b * bs / sr;
        if (tAudio + 1e-9 >= nextPoseSec) {
            // Smooth slider drag of Creative decay & Early/Tail levels
            const double s1 = std::sin(2.0 * 3.14159265358979323846 * 0.5 * tAudio);
            const double c1 = std::cos(2.0 * 3.14159265358979323846 * 0.5 * tAudio);
            setP(proc, "decayMid", 1.5 + 0.3 * s1);
            setP(proc, "early", -3.0 + 2.0 * s1);
            setP(proc, "tail", 2.0 * c1);
            nextPoseSec += 1.0 / 60.0;
        }
        buf.clear();
        for (int i = 0; i < bs; ++i) {
            const float s = 0.25f * (float)std::sin(ph);
            ph += dph;
            buf.setSample(0, i, s);
            buf.setSample(1, i, s);
        }
        proc.processBlock(buf, midi);
        for (int ch = 0; ch < lay.channels; ++ch) {
            const float* rp = buf.getReadPointer(ch);
            for (int i = 0; i < bs; ++i) {
                yCh[(size_t)ch][(size_t)b * (size_t)bs + (size_t)i] = rp[i];
            }
        }
    }

    bool allOk = true;
    int maxClicks = 0;
    double worstRatio = 0.0, worstD2 = 0.0;
    for (int ch = 0; ch < lay.channels; ++ch) {
        if (lay.layout == verb::Layout::Surround51 && ch == 3) continue;
        auto r = bakeclick::detect(yCh[(size_t)ch], ch, 15.0, 5e-5);
        int kept = 0;
        for (const auto& c : r.clicks) {
            if (c.pos > (size_t)(0.5 * sr)) {
                ++kept;
                worstRatio = std::max(worstRatio, c.ratio);
                worstD2 = std::max(worstD2, c.abs2);
            }
        }
        maxClicks = std::max(maxClicks, kept);
        if (kept > 0) allOk = false;
    }
    std::printf("  P8_CLICK layout=%s(%dch) bs=%d maxClicks=%d worstRatio=%.1f worstD2=%.3e ok=%d\n",
                lay.name, lay.channels, bs, maxClicks, worstRatio, worstD2, (int)allOk);
    return allOk;
}

inline int runImpulseCheck(const std::filesystem::path& outDir, const std::string& tag) {
    struct ImpCase {
        std::string id;
        std::string fixture;
        int nSrc;
        std::string layName;
        bool creative;
    };
    const std::vector<ImpCase> cases = {
        {"vsl_ambix3_creative", "vsl",   10, "AmbiX3", true},
        {"vsl_ambix3_physical", "vsl",   10, "AmbiX3", false},
        {"flate_ambix2_creative", "flate", 5, "AmbiX2", true},
    };
    int bad = 0;
    const double sr = 48000.0;
    const int bs = 256;
    const int nBlocks = 375; // 2.0 s impulse response
    const size_t totalSamples = (size_t)nBlocks * (size_t)bs;

    auto renderOnce = [&](VerbProcessor& proc, int nCh) {
        std::vector<float> flat((size_t)nCh * totalSamples, 0.f);
        juce::AudioBuffer<float> buf(std::max(64, nCh), bs);
        juce::MidiBuffer midi;
        for (int b = 0; b < nBlocks; ++b) {
            buf.clear();
            if (b == 0) {
                buf.setSample(0, 0, 0.5f);
                buf.setSample(1, 0, 0.5f);
            }
            proc.processBlock(buf, midi);
            for (int c = 0; c < nCh; ++c) {
                const float* rp = buf.getReadPointer(c);
                std::memcpy(&flat[((size_t)c * totalSamples) + (size_t)b * (size_t)bs], rp, sizeof(float) * (size_t)bs);
            }
        }
        return flat;
    };

    for (const auto& ic : cases) {
        const auto* lay = findLayout(ic.layName);
        if (!lay) return 1;
        VerbProcessor proc;
        if (!loadFixtureScene(proc, ic.fixture, ic.nSrc, *lay, 512, true, sr)) return 1;
        applyModeSettings(proc, ic.creative, ic.nSrc * 2, lay->hostChannels, bs, true);
        proc.prepareToPlay(sr, bs);

        // Save preset XML and reload into a second processor to verify preset round-trip equivalence
        const juce::String savedPreset = proc.getRoomPresetText();
        const auto irA = renderOnce(proc, lay->channels);

        VerbProcessor procReload;
        procReload.setPlayConfigDetails(64, lay->hostChannels, sr, 512);
        procReload.prepareToPlay(sr, 512);
        waitReady(procReload, 90);
        juce::String err;
        if (!procReload.loadRoomPresetText(savedPreset, err) || !waitReady(procReload, 120)) {
            std::printf("FAIL P8 impulse preset reload (%s): %s\n", ic.id.c_str(), err.toRawUTF8());
            ++bad;
            continue;
        }
        waitTailSettled(procReload, ic.nSrc * 2, lay->hostChannels, bs, 30.0);
        const juce::String savedPreset2 = procReload.getRoomPresetText();
        procReload.prepareToPlay(sr, bs);
        const auto irReload = renderOnce(procReload, lay->channels);

        VerbProcessor procReload2;
        procReload2.setPlayConfigDetails(64, lay->hostChannels, sr, 512);
        procReload2.prepareToPlay(sr, 512);
        waitReady(procReload2, 90);
        if (!procReload2.loadRoomPresetText(savedPreset2, err) || !waitReady(procReload2, 120)) {
            std::printf("FAIL P8 impulse preset reload2 (%s): %s\n", ic.id.c_str(), err.toRawUTF8());
            ++bad;
            continue;
        }
        waitTailSettled(procReload2, ic.nSrc * 2, lay->hostChannels, bs, 30.0);
        procReload2.prepareToPlay(sr, bs);
        const auto irReload2 = renderOnce(procReload2, lay->channels);

        double maxReloadDiff = 0.0, maxRoundTripDiff = 0.0, energyA = 0.0;
        for (size_t i = 0; i < irA.size(); ++i) {
            maxReloadDiff = std::max(maxReloadDiff, (double)std::fabs(irA[i] - irReload[i]));
            maxRoundTripDiff = std::max(maxRoundTripDiff, (double)std::fabs(irReload[i] - irReload2[i]));
            energyA += (double)irA[i] * irA[i];
        }
        std::printf("  P8_IMPULSE %-22s ch=%2d energy=%.4e reloadMaxDiff=%.3e roundTripDiff=%.3e\n",
                    ic.id.c_str(), lay->channels, energyA, maxReloadDiff, maxRoundTripDiff);
        if (!(energyA > 1e-6) || maxRoundTripDiff > 1e-6) ++bad;

        const auto binPath = outDir / ("impulse_" + tag + "_" + ic.id + ".bin");
        {
            std::ofstream out(binPath, std::ios::binary);
            out.write(reinterpret_cast<const char*>(irA.data()), (std::streamsize)(irA.size() * sizeof(float)));
        }

        // If baseline file exists and tag != "baseline", compare against baseline
        const auto basePath = outDir / ("impulse_baseline_" + ic.id + ".bin");
        if (tag != "baseline" && std::filesystem::exists(basePath)) {
            std::vector<float> baseIr(irA.size(), 0.f);
            std::ifstream in(basePath, std::ios::binary);
            in.read(reinterpret_cast<char*>(baseIr.data()), (std::streamsize)(baseIr.size() * sizeof(float)));
            double maxDiff = 0.0, diffSq = 0.0;
            for (size_t i = 0; i < irA.size(); ++i) {
                const double d = (double)irA[i] - (double)baseIr[i];
                maxDiff = std::max(maxDiff, std::fabs(d));
                diffSq += d * d;
            }
            const double snrDb = diffSq > 0.0 ? 10.0 * std::log10(std::max(energyA, 1e-20) / diffSq) : 200.0;
            std::printf("    vs baseline: maxAbsDiff=%.3e snr=%.1f dB\n", maxDiff, snrDb);
        }
    }
    return bad;
}

inline void runSwitchPeaks(const std::filesystem::path& outDir, const std::string& tag) {
    using namespace verb_p7;
    const double sr = 48000.0;
    const auto noise = makeNoiseCycle(48000);
    const std::filesystem::path csvPath = outDir / ("p8_switch_" + tag + ".csv");
    std::ofstream csv(csvPath);
    csv << "layout,channels,N,bs,event,peakPct,peakUs,meanPct,p99Pct,underruns,totalBlocks,workerCpuPerSec\n";

    for (const char* layName : {"LCR", "AmbiX3"}) {
        const auto* lay = findLayout(layName);
        if (!lay) continue;
        for (int bs : {128, 256, 512}) {
            const double budgetUs = 1e6 * (double)bs / sr;
            VerbProcessor proc;
            if (!loadFixtureScene(proc, "vsl", 20, *lay, 512, true, sr)) continue;
            // Store Creative overrides first, then switch back to Physical and settle
            applyModeSettings(proc, true, 40, lay->hostChannels, bs, true);
            setP(proc, "mode", 0.0);
            waitTailSettled(proc, 40, lay->hostChannels, bs, 30.0);
            proc.prepareToPlay(sr, bs);

            const juce::String presetXml = proc.getRoomPresetText();
            juce::AudioBuffer<float> buf(std::max(64, lay->hostChannels), bs);
            juce::MidiBuffer midi;
            size_t noisePos = 0;
            auto fillNoise = [&]() {
                buf.clear();
                for (int ch = 0; ch < 40; ++ch) {
                    float* dst = buf.getWritePointer(ch);
                    for (int i = 0; i < bs; ++i) dst[i] = noise[(noisePos + (size_t)i + (size_t)ch * 997u) % noise.size()];
                }
                noisePos = (noisePos + (size_t)bs) % noise.size();
            };

            // Event 1: Physical -> Creative -> Physical over 2.0 s
            {
                const int nBlocks = std::max(20, (int)std::llround(2.0 * sr / bs));
                std::vector<double> usList;
                int over = 0;
                HighResPacer pacer;
                const auto blockDur = std::chrono::nanoseconds((int64_t)std::llround(1e9 * (double)bs / sr));
                auto nextDeadline = std::chrono::steady_clock::now() + blockDur;
                const CpuSnapshot cpu0 = takeCpuSnapshot();
                const auto w0 = std::chrono::steady_clock::now();
                for (int b = 0; b < nBlocks; ++b) {
                    if (b == 4) setP(proc, "mode", 1.0);
                    if (b == nBlocks / 2) setP(proc, "mode", 0.0);
                    fillNoise();
                    const auto t0 = std::chrono::steady_clock::now();
                    proc.processBlock(buf, midi);
                    const auto t1 = std::chrono::steady_clock::now();
                    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                    usList.push_back(us);
                    if (us > budgetUs) ++over;
                    pacer.waitUntil(nextDeadline);
                    nextDeadline += blockDur;
                }
                const double wallSec = std::max(1e-6, std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count());
                const CpuSnapshot cpu1 = takeCpuSnapshot();
                double aCpu = 0, wCpu = 0; int actW = 0, totO = 0;
                diffCpu(cpu0, cpu1, aCpu, wCpu, actW, totO);
                double sum = 0, mx = 0;
                for (double v : usList) { sum += v; mx = std::max(mx, v); }
                std::sort(usList.begin(), usList.end());
                const double p99 = usList[std::min(usList.size() - 1, (size_t)std::floor(0.99 * (double)(usList.size() - 1)))];
                const double meanPct = 100.0 * (sum / usList.size()) / budgetUs;
                const double p99Pct = 100.0 * p99 / budgetUs;
                const double maxPct = 100.0 * mx / budgetUs;
                std::printf("P8_SWITCH %-6s N=20 bs=%-3d mode_toggle   | mean=%6.2f%% p99=%6.2f%% peak=%7.2f%% (%.1fus) >100%%=%d/%d wrkCpu=%.3f\n",
                            lay->name, bs, meanPct, p99Pct, maxPct, mx, over, nBlocks, wCpu / wallSec);
                csv << lay->name << "," << lay->channels << ",20," << bs << ",mode_toggle,"
                    << std::fixed << std::setprecision(3) << maxPct << "," << mx << "," << meanPct << "," << p99Pct << ","
                    << over << "," << nBlocks << "," << (wCpu / wallSec) << "\n";
            }

            // Event 2: Preset reload while audio thread processes blocks
            {
                const int nBlocks = std::max(20, (int)std::llround(2.0 * sr / bs));
                std::vector<double> usList;
                int over = 0;
                HighResPacer pacer;
                const auto blockDur = std::chrono::nanoseconds((int64_t)std::llround(1e9 * (double)bs / sr));
                auto nextDeadline = std::chrono::steady_clock::now() + blockDur;
                const CpuSnapshot cpu0 = takeCpuSnapshot();
                const auto w0 = std::chrono::steady_clock::now();
                std::thread reloadThr([&]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    juce::String err;
                    proc.loadRoomPresetText(presetXml, err);
                });
                for (int b = 0; b < nBlocks; ++b) {
                    fillNoise();
                    const auto t0 = std::chrono::steady_clock::now();
                    proc.processBlock(buf, midi);
                    const auto t1 = std::chrono::steady_clock::now();
                    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                    usList.push_back(us);
                    if (us > budgetUs) ++over;
                    pacer.waitUntil(nextDeadline);
                    nextDeadline += blockDur;
                }
                if (reloadThr.joinable()) reloadThr.join();
                waitReady(proc, 120);
                const double wallSec = std::max(1e-6, std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count());
                const CpuSnapshot cpu1 = takeCpuSnapshot();
                double aCpu = 0, wCpu = 0; int actW = 0, totO = 0;
                diffCpu(cpu0, cpu1, aCpu, wCpu, actW, totO);
                double sum = 0, mx = 0;
                for (double v : usList) { sum += v; mx = std::max(mx, v); }
                std::sort(usList.begin(), usList.end());
                const double p99 = usList[std::min(usList.size() - 1, (size_t)std::floor(0.99 * (double)(usList.size() - 1)))];
                const double meanPct = 100.0 * (sum / usList.size()) / budgetUs;
                const double p99Pct = 100.0 * p99 / budgetUs;
                const double maxPct = 100.0 * mx / budgetUs;
                std::printf("P8_SWITCH %-6s N=20 bs=%-3d preset_reload | mean=%6.2f%% p99=%6.2f%% peak=%7.2f%% (%.1fus) >100%%=%d/%d wrkCpu=%.3f\n",
                            lay->name, bs, meanPct, p99Pct, maxPct, mx, over, nBlocks, wCpu / wallSec);
                csv << lay->name << "," << lay->channels << ",20," << bs << ",preset_reload,"
                    << std::fixed << std::setprecision(3) << maxPct << "," << mx << "," << meanPct << "," << p99Pct << ","
                    << over << "," << nBlocks << "," << (wCpu / wallSec) << "\n";
            }
        }
    }
}

inline int run() {
    using namespace verb_p7;
    std::printf("=== P8: Creative Mode & Ambisonics / Multi-Channel CPU Benchmark ===\n");
    std::fflush(stdout);

    const bool ctestSmoke = std::getenv("VERB_P8_CTEST") != nullptr;
    const char* modeEnv = std::getenv("VERB_P8_MODE");
    const std::string mode = ctestSmoke ? "ctest" : (modeEnv && *modeEnv ? modeEnv : "matrix");
    const char* tagEnv = std::getenv("VERB_P8_TAG");
    const std::string tag = (tagEnv && *tagEnv) ? tagEnv : "baseline";
    const char* outEnv = std::getenv("VERB_P8_OUT");
    const std::filesystem::path outDir(outEnv && *outEnv ? outEnv : "out/P8");
    std::filesystem::create_directories(outDir);

    const double trialSec = std::getenv("VERB_P8_SECONDS") ? std::atof(std::getenv("VERB_P8_SECONDS")) : 1.0;
    const int nTrials = std::getenv("VERB_P8_TRIALS") ? std::max(1, std::atoi(std::getenv("VERB_P8_TRIALS"))) : 3;
    const double acceptSec = std::getenv("VERB_P8_ACCEPT_SEC") ? std::atof(std::getenv("VERB_P8_ACCEPT_SEC")) : 60.0;
    const std::string fixture = std::getenv("VERB_P8_FIXTURE") ? std::getenv("VERB_P8_FIXTURE") : "vsl";
    const bool stereoSrc = !std::getenv("VERB_P8_SRC_FMT") || std::string(std::getenv("VERB_P8_SRC_FMT")) != "mono";

    uintptr_t origMask = 0;
    std::string affDesc;

    if (mode == "ctest") {
        setAffinity4Cores(true, affDesc, origMask);
        std::printf("P8_CTEST affinity: %s\n", affDesc.c_str());
        const auto noise = makeNoiseCycle(48000);
        const auto* amb3 = findLayout("AmbiX3");
        int bad = 0;
        for (int nSrc : {1, 20}) {
            VerbProcessor proc;
            if (!loadFixtureScene(proc, "vsl", nSrc, *amb3, 512, true, 48000.0)) return 1;
            applyModeSettings(proc, true, nSrc * 2, amb3->hostChannels, 256, true);
            const double lx0 = proc.apvts.getRawParameterValue("lx")->load();
            const double ly0 = proc.apvts.getRawParameterValue("ly")->load();
            const double sx0 = proc.apvts.getRawParameterValue("s1x")->load();
            const double sy0 = proc.apvts.getRawParameterValue("s1y")->load();
            auto m = runP8Trial(proc, nSrc, *amb3, 256, true, "lmove", 1.2, noise, lx0, ly0, sx0, sy0, true, true);
            std::printf("P8_CTEST AmbiX3(16ch) Creative N=%d bs=256 lmove mean=%.1f%% p99=%.1f%% max=%.1f%% over100=%d/%d finite=%d\n",
                        nSrc, m.meanPct, m.p99Pct, m.maxPct, m.underruns, m.totalBlocks, (int)m.finiteAndNonSilent);
            if (!m.finiteAndNonSilent) ++bad;
        }
        if (!runCreativeDragClickCheck(*amb3, 256, 3.0)) ++bad;
        setAffinity4Cores(false, affDesc, origMask);
        return bad ? 1 : 0;
    }

    if (mode == "click") {
        int bad = 0;
        for (const char* ln : {"LCR", "AmbiX3"}) {
            const auto* lay = findLayout(ln);
            if (!lay) continue;
            for (int bs : {128, 256}) {
                if (!runCreativeDragClickCheck(*lay, bs, 5.0)) ++bad;
            }
        }
        return bad ? 1 : 0;
    }

    if (mode == "impulse") {
        return runImpulseCheck(outDir, tag);
    }

    if (mode == "switch") {
        setAffinity4Cores(true, affDesc, origMask);
        std::printf("P8_SWITCH affinity: %s\n", affDesc.c_str());
        runSwitchPeaks(outDir, tag);
        setAffinity4Cores(false, affDesc, origMask);
        return 0;
    }

    if (mode == "accept") {
        const int acceptTrials = std::getenv("VERB_P8_ACCEPT_TRIALS") ? std::max(1, std::atoi(std::getenv("VERB_P8_ACCEPT_TRIALS"))) : 3;
        setAffinity4Cores(true, affDesc, origMask);
        std::printf("P8_ACCEPT affinity: %s duration=%.1fs trials=%d\n", affDesc.c_str(), acceptSec, acceptTrials);
        const auto noise = makeNoiseCycle(48000);
        const auto* amb3 = findLayout("AmbiX3");
        const auto* lcr = findLayout("LCR");
        int bad = 0;

        {
            VerbProcessor proc;
            if (!loadFixtureScene(proc, "vsl", 20, *amb3, 512, true, 48000.0)) return 1;
            const double lx0 = proc.apvts.getRawParameterValue("lx")->load();
            const double ly0 = proc.apvts.getRawParameterValue("ly")->load();
            const double sx0 = proc.apvts.getRawParameterValue("s1x")->load();
            const double sy0 = proc.apvts.getRawParameterValue("s1y")->load();

            for (bool cr : {false, true}) {
                const char* mName = cr ? "Creative" : "Physical";
                applyModeSettings(proc, cr, 40, amb3->hostChannels, 256, true);
                for (const char* sc : {"rest", "lmove"}) {
                    std::vector<double> vMean, vP50, vP99, vMax, vMaxCyc, vACpu, vWCpu, vSysCpu;
                    int sumWallOver = 0, sumEngOver = 0, sumPreempt = 0, sumBlocks = 0;
                    for (int tr = 0; tr < acceptTrials; ++tr) {
                        auto m = runP8Trial(proc, 20, *amb3, 256, cr, sc, acceptSec, noise, lx0, ly0, sx0, sy0, true, true, true);
                        vMean.push_back(m.meanPct); vP50.push_back(m.p50Pct); vP99.push_back(m.p99Pct);
                        vMax.push_back(m.maxPct); vMaxCyc.push_back(m.maxCycPct);
                        vACpu.push_back(m.audioCpuPerAudioSec); vWCpu.push_back(m.workerCpuPerAudioSec);
                        vSysCpu.push_back(m.sysCpuPct);
                        sumWallOver += m.underruns; sumEngOver += m.engineUnderruns; sumPreempt += m.preemptSpikes;
                        sumBlocks += m.totalBlocks;
                        std::printf("  P8_ACCEPT_RUN[%d/%d] AmbiX3(16ch) %-8s N=20 bs=256 %-5s %.0fs: mean=%.2f%% p50=%.2f%% p99=%.2f%% max=%.2f%% (maxCyc=%.2f%%) wallOver=%d/%d (engOver=%d preempt=%d) audCpu=%.3f wrkCpu=%.3f actW=%d sysCpu=%.1f%%\n",
                                    tr + 1, acceptTrials, mName, sc, acceptSec,
                                    m.meanPct, m.p50Pct, m.p99Pct, m.maxPct, m.maxCycPct,
                                    m.underruns, m.totalBlocks, m.engineUnderruns, m.preemptSpikes,
                                    m.audioCpuPerAudioSec, m.workerCpuPerAudioSec, m.activeWorkers, m.sysCpuPct);
                        std::fflush(stdout);
                        if (!m.finiteAndNonSilent) ++bad;
                    }
                    const double medMean = medianOf(vMean), medP50 = medianOf(vP50), medP99 = medianOf(vP99);
                    const double medMax = medianOf(vMax), medMaxCyc = medianOf(vMaxCyc);
                    const double medACpu = medianOf(vACpu), medWCpu = medianOf(vWCpu), medSys = medianOf(vSysCpu);
                    std::printf("P8_ACCEPT_MEDIAN AmbiX3(16ch) %-8s N=20 bs=256 %-5s %.0fs (x%d): mean=%.2f%% p50=%.2f%% p99=%.2f%% max=%.2f%% (maxCyc=%.2f%%) wallOverSum=%d/%d (engOverSum=%d preemptSum=%d) audCpu=%.3f wrkCpu=%.3f sysCpu=%.1f%%\n",
                                mName, sc, acceptSec, acceptTrials,
                                medMean, medP50, medP99, medMax, medMaxCyc,
                                sumWallOver, sumBlocks, sumEngOver, sumPreempt,
                                medACpu, medWCpu, medSys);
                    std::fflush(stdout);
                    if (medP99 > 70.0 || sumEngOver > 0) ++bad;
                    if (std::string(sc) != "rest") {
                        waitTailSettled(proc, 40, amb3->hostChannels, 256, 30.0);
                    }
                }
            }

            // Buffer 128 report (15 s x acceptTrials, informational per brief)
            for (bool cr : {false, true}) {
                const char* mName = cr ? "Creative" : "Physical";
                applyModeSettings(proc, cr, 40, amb3->hostChannels, 128, true);
                for (const char* sc : {"rest", "lmove"}) {
                    std::vector<double> vMean, vP50, vP99, vMax, vMaxCyc, vACpu, vWCpu, vSysCpu;
                    int sumWallOver = 0, sumEngOver = 0, sumPreempt = 0, sumBlocks = 0;
                    for (int tr = 0; tr < acceptTrials; ++tr) {
                        auto m = runP8Trial(proc, 20, *amb3, 128, cr, sc, 15.0, noise, lx0, ly0, sx0, sy0, true, true, true);
                        vMean.push_back(m.meanPct); vP50.push_back(m.p50Pct); vP99.push_back(m.p99Pct);
                        vMax.push_back(m.maxPct); vMaxCyc.push_back(m.maxCycPct);
                        vACpu.push_back(m.audioCpuPerAudioSec); vWCpu.push_back(m.workerCpuPerAudioSec);
                        vSysCpu.push_back(m.sysCpuPct);
                        sumWallOver += m.underruns; sumEngOver += m.engineUnderruns; sumPreempt += m.preemptSpikes;
                        sumBlocks += m.totalBlocks;
                        std::printf("  P8_ACCEPT_128_RUN[%d/%d] AmbiX3(16ch) %-8s N=20 bs=128 %-5s 15s: mean=%.2f%% p50=%.2f%% p99=%.2f%% max=%.2f%% (maxCyc=%.2f%%) wallOver=%d/%d (engOver=%d preempt=%d) audCpu=%.3f wrkCpu=%.3f actW=%d sysCpu=%.1f%%\n",
                                    tr + 1, acceptTrials, mName, sc,
                                    m.meanPct, m.p50Pct, m.p99Pct, m.maxPct, m.maxCycPct,
                                    m.underruns, m.totalBlocks, m.engineUnderruns, m.preemptSpikes,
                                    m.audioCpuPerAudioSec, m.workerCpuPerAudioSec, m.activeWorkers, m.sysCpuPct);
                        std::fflush(stdout);
                        if (!m.finiteAndNonSilent) ++bad;
                    }
                    const double medMean = medianOf(vMean), medP50 = medianOf(vP50), medP99 = medianOf(vP99);
                    const double medMax = medianOf(vMax), medMaxCyc = medianOf(vMaxCyc);
                    const double medACpu = medianOf(vACpu), medWCpu = medianOf(vWCpu), medSys = medianOf(vSysCpu);
                    std::printf("P8_ACCEPT_128 AmbiX3(16ch) %-8s N=20 bs=128 %-5s 15s (x%d): mean=%.2f%% p50=%.2f%% p99=%.2f%% max=%.2f%% (maxCyc=%.2f%%) wallOverSum=%d/%d (engOverSum=%d preemptSum=%d) audioCpu=%.3f workerCpu=%.3f sysCpu=%.1f%%\n",
                                mName, sc, acceptTrials,
                                medMean, medP50, medP99, medMax, medMaxCyc,
                                sumWallOver, sumBlocks, sumEngOver, sumPreempt,
                                medACpu, medWCpu, medSys);
                    std::fflush(stdout);
                    if (std::string(sc) != "rest") {
                        waitTailSettled(proc, 40, amb3->hostChannels, 128, 30.0);
                    }
                }
            }
        }

        // Reference LCR N=1 check (no regression vs P7)
        {
            VerbProcessor proc;
            if (!loadFixtureScene(proc, "vsl", 1, *lcr, 512, true, 48000.0)) return 1;
            const double lx0 = proc.apvts.getRawParameterValue("lx")->load();
            const double ly0 = proc.apvts.getRawParameterValue("ly")->load();
            const double sx0 = proc.apvts.getRawParameterValue("s1x")->load();
            const double sy0 = proc.apvts.getRawParameterValue("s1y")->load();
            for (bool cr : {false, true}) {
                const char* mName = cr ? "Creative" : "Physical";
                applyModeSettings(proc, cr, 2, lcr->hostChannels, 256, true);
                for (const char* sc : {"rest", "lmove"}) {
                    auto m = runP8Trial(proc, 1, *lcr, 256, cr, sc, 5.0, noise, lx0, ly0, sx0, sy0, true, true);
                    std::printf("P8_REF_LCR_N1 LCR(3ch) %-8s N=1 bs=256 %-5s 5s: mean=%.2f%% p50=%.2f%% p99=%.2f%% max=%.2f%% underruns=%d/%d\n",
                                mName, sc, m.meanPct, m.p50Pct, m.p99Pct, m.maxPct, m.underruns, m.totalBlocks);
                    std::fflush(stdout);
                }
            }
        }

        setAffinity4Cores(false, affDesc, origMask);
        return bad ? 1 : 0;
    }

    if (mode == "profile") {
        setAffinity4Cores(true, affDesc, origMask);
        std::printf("P8_PROFILE affinity: %s\n", affDesc.c_str());
        const auto noise = makeNoiseCycle(48000);
        struct ProfCase {
            std::string layName;
            int n;
            int bs;
            bool creative;
            std::string sc;
            double sec;
        };
        const std::vector<ProfCase> cases = {
            {"LCR",    1,  256, false, "rest",        2.0},
            {"LCR",    1,  256, true,  "rest",        2.0},
            {"LCR",    20, 256, false, "rest",        2.0},
            {"LCR",    20, 256, false, "lmove",       4.0},
            {"LCR",    20, 256, true,  "rest",        2.0},
            {"LCR",    20, 256, true,  "lmove",       4.0},
            {"LCR",    20, 256, true,  "cauto",       4.0},
            {"AmbiX1", 20, 256, true,  "lmove",       3.0},
            {"5.1",    20, 256, true,  "lmove",       3.0},
            {"AmbiX2", 20, 256, true,  "lmove",       3.0},
            {"AmbiX3", 1,  256, false, "rest",        2.0},
            {"AmbiX3", 1,  256, true,  "rest",        2.0},
            {"AmbiX3", 20, 256, false, "rest",        3.0},
            {"AmbiX3", 20, 256, false, "lmove",       4.0},
            {"AmbiX3", 20, 256, true,  "rest",        3.0},
            {"AmbiX3", 20, 256, true,  "smove",       3.0},
            {"AmbiX3", 20, 256, true,  "lmove",       4.0},
            {"AmbiX3", 20, 256, true,  "cauto",       4.0},
            {"AmbiX3", 20, 256, true,  "cauto_decay", 4.0},
            {"AmbiX3", 20, 256, true,  "cauto_gain",  4.0},
            {"AmbiX3", 20, 128, false, "lmove",       4.0},
            {"AmbiX3", 20, 128, true,  "rest",        3.0},
            {"AmbiX3", 20, 128, true,  "lmove",       4.0},
        };

        std::string curLay = "";
        int curN = -1;
        std::unique_ptr<VerbProcessor> procPtr;
        double lx0 = 0, ly0 = 0, sx0 = 0, sy0 = 0;

        for (const auto& c : cases) {
            const auto* lay = findLayout(c.layName);
            if (!lay) continue;
            if (!procPtr || curLay != c.layName || curN != c.n) {
                procPtr = std::make_unique<VerbProcessor>();
                if (!loadFixtureScene(*procPtr, "vsl", c.n, *lay, 512, true, 48000.0)) return 1;
                lx0 = procPtr->apvts.getRawParameterValue("lx")->load();
                ly0 = procPtr->apvts.getRawParameterValue("ly")->load();
                sx0 = procPtr->apvts.getRawParameterValue("s1x")->load();
                sy0 = procPtr->apvts.getRawParameterValue("s1y")->load();
                curLay = c.layName;
                curN = c.n;
            }
            applyModeSettings(*procPtr, c.creative, c.n * 2, lay->hostChannels, c.bs, true);
            auto m = runP8Trial(*procPtr, c.n, *lay, c.bs, c.creative, c.sc, c.sec, noise, lx0, ly0, sx0, sy0, true, true);
            const auto& p = m.prof;
            const double budgetUs = 1e6 * (double)c.bs / 48000.0;
            const double nb = std::max(1.0, (double)m.totalBlocks);
            auto usB = [&](uint64_t ns) { return (double)ns / (1000.0 * nb); };
            auto pctB = [&](uint64_t ns) { return 100.0 * usB(ns) / budgetUs; };
            const double engUs = usB(p.processTotalNs);
            const double ovhUs = std::max(0.0, m.meanUs - engUs);
            const uint64_t accountedProcNs = p.startPeriodTotalNs + p.bandSplitNs + p.lateFeedNs
                + p.renderTapsNs + p.diffuseNs + p.creativeWetNs + p.endPeriodNs + p.tailBlockNs;
            const uint64_t otherProcNs = p.processTotalNs > accountedProcNs ? (p.processTotalNs - accountedProcNs) : 0;
            auto eng = E9bAccess::engine(*procPtr);
            const double tailSec = eng ? eng->tailSeconds() : 0.0;
            std::printf("\n[P8_PROFILE] %s(%dch) %-8s N=%-2d bs=%-3d %-11s sec=%.1f (blocks=%d, budget=%.1fus, tailSec=%.2fs)\n",
                        lay->name, lay->channels, c.creative ? "Creative" : "Physical", c.n, c.bs, c.sc.c_str(),
                        c.sec, m.totalBlocks, budgetUs, tailSec);
            std::printf("  procBlock: mean=%.1fus (%.2f%%) p50=%.1fus (%.2f%%) p99=%.1fus (%.2f%%) max=%.1fus (%.2f%%) over100=%d | engProcess=%.1fus (%.2f%%) procOverhead=%.1fus (%.2f%%)\n",
                        m.meanUs, m.meanPct, m.p50Us, m.p50Pct, m.p99Us, m.p99Pct, m.maxUs, m.maxPct, m.underruns,
                        engUs, 100.0 * engUs / budgetUs, ovhUs, 100.0 * ovhUs / budgetUs);
            std::printf("  phases (us/blk | %% budget): startPeriod=%.1f (%.1f%%) bandSplit=%.1f (%.1f%%) lateFeed=%.1f (%.1f%%) renderTaps=%.1f (%.1f%%) diffuse=%.1f (%.1f%%) creativeWet=%.1f (%.1f%%) endPeriod=%.1f (%.1f%%) tailBlock=%.1f (%.1f%%) other=%.1f\n",
                        usB(p.startPeriodTotalNs), pctB(p.startPeriodTotalNs),
                        usB(p.bandSplitNs), pctB(p.bandSplitNs),
                        usB(p.lateFeedNs), pctB(p.lateFeedNs),
                        usB(p.renderTapsNs), pctB(p.renderTapsNs),
                        usB(p.diffuseNs), pctB(p.diffuseNs),
                        usB(p.creativeWetNs), pctB(p.creativeWetNs),
                        usB(p.endPeriodNs), pctB(p.endPeriodNs),
                        usB(p.tailBlockNs), pctB(p.tailBlockNs),
                        usB(otherProcNs));
            std::printf("  startPeriod (%llu calls): setup=%.1fus pose=%.1fus buildImages=%.1fus solveDirect=%.1fus solveValidate=%.1fus solveTaps=%.1fus diffJobs=%.1fus | srcSkips=%llu srcSolves=%llu\n",
                        (unsigned long long)p.startPeriodCalls,
                        usB(p.spSetupNs), usB(p.spPoseNs), usB(p.spBuildImagesNs),
                        usB(p.spSolveDirectNs), usB(p.spSolveValidateNs), usB(p.spSolveTapsNs),
                        usB(p.spDiffJobsNs),
                        (unsigned long long)p.spSourceSkips, (unsigned long long)p.spSourceSolves);
            std::printf("  renderTaps/blk: skip=%.1f stat=%.1f moving=%.1f | validateCalls=%llu\n",
                        (double)p.tapsSkipped / nb, (double)p.tapsStat / nb, (double)p.tapsMoving / nb,
                        (unsigned long long)p.validateCalls);
            std::printf("  worker: stepCalls=%llu motionEsts=%llu (%.1fms) rebuilds=%llu (cancel=%llu, %.1fms) | trg settle=%llu legacy=%llu jump=%llu | wrkCpu=%.3f actW=%d\n",
                        (unsigned long long)p.workerStepCalls,
                        (unsigned long long)p.workerMotionEsts, (double)p.workerMotionEstNs * 1e-6,
                        (unsigned long long)p.workerRebuilds, (unsigned long long)p.workerRebuildsCancelled, (double)p.workerRebuildNs * 1e-6,
                        (unsigned long long)p.workerSettleTriggers, (unsigned long long)p.workerLegacyTriggers, (unsigned long long)p.workerJumpTriggers,
                        m.workerCpuPerAudioSec, m.activeWorkers);
            std::fflush(stdout);
        }
        setAffinity4Cores(false, affDesc, origMask);
        return 0;
    }

    // Matrix measurement mode (4-core restricted)
    if (!setAffinity4Cores(true, affDesc, origMask)) {
        std::printf("FAIL P8: SetProcessAffinityMask failed\n");
        return 1;
    }
    std::printf("\n--- P8 Matrix (fixture=%s, srcFmt=%s, %s) ---\n",
                fixture.c_str(), stereoSrc ? "stereo_sum1" : "mono", affDesc.c_str());

    const std::vector<int> nList = parseIntList("VERB_P8_N_LIST", {1, 5, 10, 20});
    const std::vector<int> bsList = parseIntList("VERB_P8_BS_LIST", {128, 256, 512});
    const std::vector<std::string> layList = parseStrList("VERB_P8_LAYOUT_LIST", {"Stereo", "LCR", "AmbiX1", "5.1", "AmbiX2", "AmbiX3"});
    const std::vector<std::string> crList = parseStrList("VERB_P8_CREATIVE_LIST", {"Physical", "Creative"});
    const std::vector<std::string> scList = parseStrList("VERB_P8_SCENARIO_LIST", {"rest", "lmove", "smove", "cauto"});
    const bool fullCartesian = (std::getenv("VERB_P8_FULL_CARTESIAN") != nullptr);
    const auto noise = makeNoiseCycle(48000);

    const std::filesystem::path csvPath = outDir / ("p8_" + tag + ".csv");
    const bool appendCsv = (std::getenv("VERB_P8_APPEND") != nullptr) && std::filesystem::exists(csvPath);
    std::ofstream csv(csvPath, appendCsv ? (std::ios::out | std::ios::app) : std::ios::out);
    if (!appendCsv) {
        csv << "fixture,srcFmt,layout,channels,mode,N,bs,scenario,trials,secPerTrial,meanPct,p50Pct,p99Pct,maxPct,meanUs,p99Us,maxUs,underrunsSum,totalBlocksSum,audioCpuPerSec,workerCpuPerSec,activeWorkers,tailSec\n";
    }

    int bad = 0;
    for (const auto& layName : layList) {
        const auto* lay = findLayout(layName);
        if (!lay) continue;
        const bool isPrimaryLayout = (lay->layout == verb::Layout::LCR || lay->layout == verb::Layout::AmbiX3);

        for (int nSrc : nList) {
            VerbProcessor proc;
            if (!loadFixtureScene(proc, fixture, nSrc, *lay, 512, stereoSrc, 48000.0)) return 1;
            const double lx0 = proc.apvts.getRawParameterValue("lx")->load();
            const double ly0 = proc.apvts.getRawParameterValue("ly")->load();
            const double sx0 = proc.apvts.getRawParameterValue("s1x")->load();
            const double sy0 = proc.apvts.getRawParameterValue("s1y")->load();
            const int nInCh = std::min(64, nSrc * (stereoSrc ? 2 : 1));

            for (const auto& crName : crList) {
                const bool creative = juce::String(crName).equalsIgnoreCase("Creative");
                applyModeSettings(proc, creative, nInCh, lay->hostChannels, 256, true);
                auto eng = E9bAccess::engine(proc);
                const double tailSec = eng ? eng->tailSeconds() : 0.0;

                for (int bs : bsList) {
                    // For primary layouts (LCR, AmbiX3): test all (N, bs).
                    // For secondary layouts (Stereo, AmbiX1, 5.1, AmbiX2): test all N at bs=256 and all bs at N=1,20 (unless VERB_P8_FULL_CARTESIAN=1).
                    if (!fullCartesian && !isPrimaryLayout && bs != 256 && nSrc != 1 && nSrc != 20) continue;

                    const double sec = (bs >= 512) ? std::max(trialSec, 1.1) : trialSec;
                    for (const auto& sc : scList) {
                        if (!creative && sc == "cauto") continue; // Creative automation only applies in Creative mode
                        if (!fullCartesian && !isPrimaryLayout && !creative && sc == "smove" && bs != 256) continue;

                        std::vector<double> meanP, p50P, p99P, maxP, meanU, p99U, maxU, aCpu, wCpu;
                        int overSum = 0, blkSum = 0, actW = 0;
                        for (int tr = 0; tr < nTrials; ++tr) {
                            auto m = runP8Trial(proc, nSrc, *lay, bs, creative, sc, sec, noise, lx0, ly0, sx0, sy0, stereoSrc, true);
                            meanP.push_back(m.meanPct); p50P.push_back(m.p50Pct); p99P.push_back(m.p99Pct); maxP.push_back(m.maxPct);
                            meanU.push_back(m.meanUs); p99U.push_back(m.p99Us); maxU.push_back(m.maxUs);
                            aCpu.push_back(m.audioCpuPerAudioSec); wCpu.push_back(m.workerCpuPerAudioSec);
                            overSum += m.underruns; blkSum += m.totalBlocks;
                            actW = std::max(actW, m.activeWorkers);
                            if (!m.finiteAndNonSilent) ++bad;
                        }
                        if (sc == "cauto") {
                            // Restore base Creative multipliers after automation sweep
                            applyModeSettings(proc, true, nInCh, lay->hostChannels, bs, true);
                        }
                        const double medMean = medianOf(meanP), medP50 = medianOf(p50P), medP99 = medianOf(p99P), medMax = medianOf(maxP);
                        const double medMeanUs = medianOf(meanU), medP99Us = medianOf(p99U), medMaxUs = medianOf(maxU);
                        const double medACpu = medianOf(aCpu), medWCpu = medianOf(wCpu);
                        std::printf("P8 %-6s(%2dch) %-8s N=%-2d bs=%-3d %-5s | mean=%6.2f%% p50=%6.2f%% p99=%6.2f%% max=%7.2f%% | >100%%=%3d/%-4d | audCpu=%.3f wrkCpu=%.3f actW=%d tail=%.1fs\n",
                                    lay->name, lay->channels, creative ? "Creative" : "Physical", nSrc, bs, sc.c_str(),
                                    medMean, medP50, medP99, medMax, overSum, blkSum,
                                    medACpu, medWCpu, actW, tailSec);
                        std::fflush(stdout);
                        csv << fixture << "," << (stereoSrc ? "stereo_sum1" : "mono") << ","
                            << lay->name << "," << lay->channels << "," << (creative ? "Creative" : "Physical") << ","
                            << nSrc << "," << bs << "," << sc << "," << nTrials << "," << sec << ","
                            << std::fixed << std::setprecision(3)
                            << medMean << "," << medP50 << "," << medP99 << "," << medMax << ","
                            << medMeanUs << "," << medP99Us << "," << medMaxUs << ","
                            << overSum << "," << blkSum << "," << medACpu << "," << medWCpu << ","
                            << actW << "," << tailSec << "\n";
                        csv.flush();
                    }
                }
            }
        }
    }

    setAffinity4Cores(false, affDesc, origMask);
    std::printf("P8 CSV written to %s\n", csvPath.string().c_str());
    return bad ? 1 : 0;
}

} // namespace verb_p8

static int testP8() { return verb_p8::run(); }
