#pragma once
// P7: CPU load & underrun harness for many sources with listener / source motion.
// Env:
//   VERB_ONLY_P7=1            run P7 suite
//   VERB_P7_CTEST=1           fast smoke + click check for default ctest
//   VERB_P7_TAG=baseline      tag for output CSV (out/P7/p7_<tag>.csv)
//   VERB_P7_MODE=all|unrestricted|4core|accept|click
//   VERB_P7_SECONDS=2.0       audio seconds per trial (bs=512 uses max(sec, 2.5))
//   VERB_P7_TRIALS=3          trials per cell (medians reported)
//   VERB_P7_BAKE=0|1          include baked listener move (N=1, N=20 at bs=256)
//   VERB_P7_ACCEPT_SEC=60     duration for 4-core N=20 bs=256 lmove acceptance run
//   VERB_P7_N_LIST=1,5,10,20  comma-separated N list
//   VERB_P7_BS_LIST=128,256,512

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <tlhelp32.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace verb_p7 {

struct HighResPacer {
#ifdef _WIN32
    HANDLE timer = nullptr;
    HighResPacer() {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
    ~HighResPacer() {
        if (timer) CloseHandle(timer);
    }
    void waitUntil(std::chrono::steady_clock::time_point target) const {
        auto now = std::chrono::steady_clock::now();
        if (now >= target) return;
        const auto remUs = std::chrono::duration_cast<std::chrono::microseconds>(target - now).count();
        if (timer && remUs > 350) {
            LARGE_INTEGER li{};
            li.QuadPart = -(int64_t)(remUs - 150) * 10; // 100-ns units, relative
            if (SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, 100);
            }
        }
        while (std::chrono::steady_clock::now() < target) {
            const auto leftUs = std::chrono::duration_cast<std::chrono::microseconds>(target - std::chrono::steady_clock::now()).count();
            if (leftUs > 1000) std::this_thread::sleep_for(std::chrono::microseconds(250));
            else std::this_thread::yield();
        }
    }
#else
    void waitUntil(std::chrono::steady_clock::time_point target) const {
        std::this_thread::sleep_until(target);
    }
#endif
};

struct CpuSnapshot {
    double audioSec = 0.0;
    std::map<unsigned long, double> otherThreadsSec;
};

#ifdef _WIN32
inline double fileTimeSec(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)u.QuadPart * 1e-7;
}
#endif

inline CpuSnapshot takeCpuSnapshot() {
    CpuSnapshot snap;
#ifdef _WIN32
    const DWORD pid = GetCurrentProcessId();
    const DWORD selfTid = GetCurrentThreadId();
    {
        FILETIME ct{}, et{}, kt{}, ut{};
        if (GetThreadTimes(GetCurrentThread(), &ct, &et, &kt, &ut)) {
            snap.audioSec = fileTimeSec(kt) + fileTimeSec(ut);
        }
    }
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(hSnap, &te)) {
            do {
                if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID)
                    && te.th32OwnerProcessID == pid && te.th32ThreadID != selfTid) {
                    HANDLE hThr = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                    if (hThr) {
                        FILETIME ct{}, et{}, kt{}, ut{};
                        if (GetThreadTimes(hThr, &ct, &et, &kt, &ut)) {
                            snap.otherThreadsSec[te.th32ThreadID] = fileTimeSec(kt) + fileTimeSec(ut);
                        }
                        CloseHandle(hThr);
                    }
                }
                te.dwSize = sizeof(te);
            } while (Thread32Next(hSnap, &te));
        }
        CloseHandle(hSnap);
    }
#endif
    return snap;
}

inline void diffCpu(const CpuSnapshot& a, const CpuSnapshot& b, double& audioSecOut, double& workerSecOut, int& activeWorkerThreads, int& totalOtherThreads) {
    audioSecOut = std::max(0.0, b.audioSec - a.audioSec);
    workerSecOut = 0.0;
    activeWorkerThreads = 0;
    totalOtherThreads = (int)b.otherThreadsSec.size();
    for (const auto& kv : b.otherThreadsSec) {
        auto it = a.otherThreadsSec.find(kv.first);
        const double prev = (it != a.otherThreadsSec.end()) ? it->second : 0.0;
        const double dt = std::max(0.0, kv.second - prev);
        workerSecOut += dt;
        if (dt > 0.0005) ++activeWorkerThreads;
    }
}

struct TrialMetrics {
    double meanPct = 0, p50Pct = 0, p99Pct = 0, maxPct = 0;
    double meanUs = 0, p50Us = 0, p99Us = 0, maxUs = 0;
    int underruns = 0;
    int totalBlocks = 0;
    double audioCpuPerAudioSec = 0;
    double workerCpuPerAudioSec = 0;
    int activeWorkers = 0;
    int totalOtherThreads = 0;
    int poolWorkersReported = 0;
    uint64_t motionEstimates = 0;
    uint64_t retraces = 0;
    bool finiteAndNonSilent = true;
    verb::Engine::P7ProfileStats prof{};
};

inline double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

inline std::vector<float> makeNoiseCycle(size_t n) {
    std::vector<float> out(n, 0.f);
    uint32_t s = 0x12345678u;
    for (size_t i = 0; i < n; ++i) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const float u = ((float)(s & 0xFFFFFFu) / 16777216.0f) * 2.0f - 1.0f;
        out[i] = u * 0.1f;
    }
    return out;
}

inline bool loadVslHallScene(VerbProcessor& proc, int nSources, int bs, double sr = 48000.0) {
    const juce::File presetFile = repoFile("out/vsl-hall/presets/VSL_Grand_Hall_Audience.verbroom");
    if (!presetFile.existsAsFile()) {
        std::printf("FAIL P7: missing preset %s\n", presetFile.getFullPathName().toRawUTF8());
        return false;
    }
    auto xml = juce::parseXML(presetFile.loadFileAsString());
    if (!xml) {
        std::printf("FAIL P7: cannot parse VSL_Grand_Hall_Audience.verbroom\n");
        return false;
    }
    auto* st = xml->getChildByName("VerbState");
    if (!st) return false;
    if (auto* out = st->getChildByName("Output")) {
        out->setAttribute("layout", (int)verb::Layout::LCR);
        out->setAttribute("radius", 0.1);
        out->setAttribute("micPattern", 0);
    }
    if (auto* srcs = st->getChildByName("Sources")) {
        while (srcs->getNumChildElements() > nSources) {
            srcs->removeChildElement(srcs->getChildElement(srcs->getNumChildElements() - 1), true);
        }
    }
    {
        std::istringstream is(st->getStringAttribute("sceneText").toStdString());
        verb::Scene sc = verb::parseScene(is, "VSL");
        if ((int)sc.sources.size() > nSources) sc.sources.resize((size_t)nSources);
        st->setAttribute("sceneText", juce::String(verb::serializeScene(sc)));
    }

    proc.setPlayConfigDetails(64, 3, sr, bs);
    proc.prepareToPlay(sr, bs);
    if (!waitReady(proc, 90)) {
        std::printf("FAIL P7: initial processor prepare failed\n");
        return false;
    }
    juce::String err;
    if (!proc.loadRoomPresetText(xml->toString(), err) || !waitReady(proc, 120)) {
        std::printf("FAIL P7: loadRoomPresetText failed: %s\n", err.toRawUTF8());
        return false;
    }
    setP(proc, "mode", 0.0); // Physical mode
    setP(proc, "mix", 100.0);
    return true;
}

inline TrialMetrics runScenarioTrial(
    VerbProcessor& proc,
    int nSources,
    int bs,
    const std::string& scenario,
    double seconds,
    const std::vector<float>& noise,
    double lx0, double ly0, double sx0, double sy0,
    bool realTimePace = true)
{
    TrialMetrics m{};
    const double sr = 48000.0;
    const double budgetUs = 1e6 * (double)bs / sr;
    const int nChannels = std::min(64, nSources * 2);
    juce::AudioBuffer<float> buf(64, bs);
    juce::MidiBuffer midi;

    auto setPoseAt = [&](double tSec) {
        if (scenario == "lmove") {
            // Triangle wave +-0.75 m over 3.0 s => 1.0 m/s continuous back-and-forth
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
        } else if (scenario == "bake") {
            // Closed loop 0..1 at ~1 m/s (3.0 m loop => 1/3 lap/s)
            const double u = std::fmod(tSec / 3.0, 1.0);
            setP(proc, "lpath", u);
        }
    };

    // Restore rest position & warm up 0.4 s
    setP(proc, "lx", lx0);
    setP(proc, "ly", ly0);
    setP(proc, "s1x", sx0);
    setP(proc, "s1y", sy0);
    if (scenario == "bake") setP(proc, "lpath", 0.0);

    size_t noisePos = 0;
    auto fillNoise = [&]() {
        buf.clear();
        for (int ch = 0; ch < nChannels; ++ch) {
            float* dst = buf.getWritePointer(ch);
            for (int i = 0; i < bs; ++i) {
                dst[i] = noise[(noisePos + (size_t)i + (size_t)ch * 997u) % noise.size()];
            }
        }
        noisePos = (noisePos + (size_t)bs) % noise.size();
    };

    const int warmBlocks = std::max(8, (int)std::llround(0.4 * sr / bs));
    for (int b = 0; b < warmBlocks; ++b) {
        fillNoise();
        proc.processBlock(buf, midi);
    }

    auto engine = E9bAccess::engine(proc);
    if (engine) engine->resetP7ProfileStats();
    const auto mBefore = engine ? engine->motionStats() : verb::Engine::MotionStats{};
    const CpuSnapshot cpuBefore = takeCpuSnapshot();
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

    const bool spikeTrace = (std::getenv("VERB_P7_SPIKE_TRACE") != nullptr);
    const auto wallStart = std::chrono::steady_clock::now();
    for (int b = 0; b < nBlocks; ++b) {
        const double tAudio = (double)b * bs / sr;
        if (scenario != "rest" && tAudio + 1e-9 >= nextPoseSec) {
            setPoseAt(tAudio);
            nextPoseSec += 1.0 / 60.0;
        }
        fillNoise();
        const auto profBefore = (spikeTrace && engine) ? engine->p7ProfileStats() : verb::Engine::P7ProfileStats{};
        uint64_t cyc0 = 0, cyc1 = 0;
        double thrCpu0 = 0, thrCpu1 = 0;
        if (spikeTrace) {
#ifdef _WIN32
            ULONG64 rawCyc0 = 0;
            if (QueryThreadCycleTime(GetCurrentThread(), &rawCyc0)) cyc0 = (uint64_t)rawCyc0;
            FILETIME ct{}, et{}, kt{}, ut{};
            if (GetThreadTimes(GetCurrentThread(), &ct, &et, &kt, &ut)) thrCpu0 = fileTimeSec(kt) + fileTimeSec(ut);
#endif
        }
        const auto t0 = std::chrono::steady_clock::now();
        proc.processBlock(buf, midi);
        const auto t1 = std::chrono::steady_clock::now();
        const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        if (spikeTrace && us > 0.7 * budgetUs) {
#ifdef _WIN32
            ULONG64 rawCyc1 = 0;
            if (QueryThreadCycleTime(GetCurrentThread(), &rawCyc1)) cyc1 = (uint64_t)rawCyc1;
            FILETIME ct{}, et{}, kt{}, ut{};
            if (GetThreadTimes(GetCurrentThread(), &ct, &et, &kt, &ut)) thrCpu1 = fileTimeSec(kt) + fileTimeSec(ut);
#endif
            const auto profAfter = engine ? engine->p7ProfileStats() : verb::Engine::P7ProfileStats{};
            const double dProcUs = (double)(profAfter.processTotalNs - profBefore.processTotalNs) * 1e-3;
            const double dSpUs = (double)(profAfter.startPeriodTotalNs - profBefore.startPeriodTotalNs) * 1e-3;
            const double dRtUs = (double)(profAfter.renderTapsNs - profBefore.renderTapsNs) * 1e-3;
            const double dTbUs = (double)(profAfter.tailBlockNs - profBefore.tailBlockNs) * 1e-3;
            const double dCpuMs = (thrCpu1 - thrCpu0) * 1e3;
            const double dMcyc = (double)(cyc1 - cyc0) * 1e-6;
            std::printf("  [SPIKE] b=%d t=%.2fs wall=%.1fus (%.1f%%) thrCpu=%.2fms cyc=%.2fM | eng=%.1fus sp=%.1fus rt=%.1fus tb=%.1fus solves=%llu skips=%llu\n",
                        b, tAudio, us, 100.0 * us / budgetUs, dCpuMs, dMcyc, dProcUs, dSpUs, dRtUs, dTbUs,
                        (unsigned long long)(profAfter.spSourceSolves - profBefore.spSourceSolves),
                        (unsigned long long)(profAfter.spSourceSkips - profBefore.spSourceSkips));
            std::fflush(stdout);
        }
        usList.push_back(us);
        if (us > budgetUs) ++m.underruns;

        for (int ch = 0; ch < 3; ++ch) {
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
    const double wallSec = std::max(1e-6, std::chrono::duration<double>(wallEnd - wallStart).count());
    const CpuSnapshot cpuAfter = takeCpuSnapshot();
    const auto mAfter = engine ? engine->motionStats() : verb::Engine::MotionStats{};
    if (engine) m.prof = engine->p7ProfileStats();

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

inline int countMaskBits(uintptr_t mask) {
    int c = 0;
    while (mask) { c += (int)(mask & 1u); mask >>= 1; }
    return c;
}

inline bool setAffinity4Cores(bool restrict4, std::string& descOut, uintptr_t& origProcMaskOut) {
#ifdef _WIN32
    DWORD_PTR procMask = 0, sysMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)) return false;
    if (origProcMaskOut == 0) origProcMaskOut = (uintptr_t)procMask;
    if (restrict4) {
        DWORD_PTR target = 0;
        int picked = 0;
        for (int b = 0; b < 64 && picked < 4; ++b) {
            const DWORD_PTR bit = ((DWORD_PTR)1) << b;
            if (sysMask & bit) { target |= bit; ++picked; }
        }
        if (!target || !SetProcessAffinityMask(GetCurrentProcess(), target)) return false;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        std::ostringstream os;
        os << "4core_mask=0x" << std::hex << target << std::dec << " (audio=THREAD_PRIORITY_NORMAL)";
        descOut = os.str();
    } else {
        SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)origProcMaskOut);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        std::ostringstream os;
        os << "unrestricted_cores=" << countMaskBits(origProcMaskOut);
        descOut = os.str();
    }
    return true;
#else
    (void)restrict4;
    (void)origProcMaskOut;
    descOut = "non-windows";
    return true;
#endif
}

inline std::vector<int> parseIntList(const char* envName, std::vector<int> def) {
    const char* v = std::getenv(envName);
    if (!v || !*v) return def;
    std::vector<int> out;
    juce::StringArray sa;
    sa.addTokens(v, ",", "");
    for (auto& s : sa) {
        const int x = s.trim().getIntValue();
        if (x > 0) out.push_back(x);
    }
    return out.empty() ? def : out;
}

inline bool runListenerDragClickCheck(int bs, double seconds = 6.0) {
    const double sr = 48000.0;
    VerbProcessor proc;
    if (!loadVslHallScene(proc, 20, bs, sr)) return false;
    const double lx0 = proc.apvts.getRawParameterValue("lx")->load();
    const double ly0 = proc.apvts.getRawParameterValue("ly")->load();
    const int nBlocks = (int)std::llround(seconds * sr / bs);
    const size_t total = (size_t)nBlocks * (size_t)bs;

    // 220 Hz steady sine on source 0 (ch 0 & 1)
    std::vector<float> yL(total, 0.f), yR(total, 0.f), yC(total, 0.f);
    juce::AudioBuffer<float> buf(64, bs);
    juce::MidiBuffer midi;

    // Pre-roll 0.5 s at rest
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
            const double p = std::fmod(tAudio, 3.0) / 3.0;
            const double tri = p < 0.25 ? (4.0 * p) : (p < 0.75 ? (2.0 - 4.0 * p) : (-4.0 + 4.0 * p));
            setP(proc, "lx", lx0 + 0.75 * tri);
            setP(proc, "ly", ly0 + 0.25 * std::sin(2.0 * 3.14159265358979323846 * tAudio / 3.0));
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
        for (int i = 0; i < bs; ++i) {
            const size_t idx = (size_t)b * (size_t)bs + (size_t)i;
            yL[idx] = buf.getSample(0, i);
            yR[idx] = buf.getSample(1, i);
            yC[idx] = buf.getSample(2, i);
        }
    }

    auto checkCh = [&](const std::vector<float>& y, int ch) {
        auto r = bakeclick::detect(y, ch, 15.0, 5e-5);
        int kept = 0;
        double maxRatio = 0.0, maxD2 = 0.0;
        for (const auto& c : r.clicks) {
            if (c.pos > (size_t)(0.5 * sr)) {
                ++kept;
                maxRatio = std::max(maxRatio, c.ratio);
                maxD2 = std::max(maxD2, c.abs2);
            }
        }
        std::printf("  P7_CLICK bs=%d ch=%d clicks=%d peakRatio=%.1f peakD2=%.3e rms=%.4f\n",
                    bs, ch, kept, maxRatio, maxD2, r.rmsOut);
        return kept == 0 && r.rmsOut > 1e-4;
    };
    bool ok = checkCh(yL, 0) & checkCh(yR, 1) & checkCh(yC, 2);
    return ok;
}

inline int run() {
    std::printf("=== P7: Listener Motion CPU & Underrun Benchmark ===\n");
    std::fflush(stdout);

    const bool ctestSmoke = std::getenv("VERB_P7_CTEST") != nullptr;
    const char* modeEnv = std::getenv("VERB_P7_MODE");
    const std::string mode = ctestSmoke ? "ctest" : (modeEnv && *modeEnv ? modeEnv : "all");
    const char* tagEnv = std::getenv("VERB_P7_TAG");
    const std::string tag = (tagEnv && *tagEnv) ? tagEnv : "baseline";
    const char* outEnv = std::getenv("VERB_P7_OUT");
    const std::filesystem::path outDir(outEnv && *outEnv ? outEnv : "out/P7");
    std::filesystem::create_directories(outDir);

    const double trialSec = std::getenv("VERB_P7_SECONDS") ? std::atof(std::getenv("VERB_P7_SECONDS")) : 2.0;
    const int nTrials = std::getenv("VERB_P7_TRIALS") ? std::max(1, std::atoi(std::getenv("VERB_P7_TRIALS"))) : 3;
    const bool doBake = std::getenv("VERB_P7_BAKE") ? (std::atoi(std::getenv("VERB_P7_BAKE")) != 0) : true;
    const double acceptSec = std::getenv("VERB_P7_ACCEPT_SEC") ? std::atof(std::getenv("VERB_P7_ACCEPT_SEC")) : 60.0;

    uintptr_t origMask = 0;
    std::string affDesc;

    if (mode == "ctest") {
        setAffinity4Cores(true, affDesc, origMask);
        std::printf("P7_CTEST affinity: %s\n", affDesc.c_str());
        const auto noise = makeNoiseCycle(48000);
        int bad = 0;
        for (int nSrc : {1, 20}) {
            VerbProcessor proc;
            if (!loadVslHallScene(proc, nSrc, 256, 48000.0)) return 1;
            const double lx0 = proc.apvts.getRawParameterValue("lx")->load();
            const double ly0 = proc.apvts.getRawParameterValue("ly")->load();
            const double sx0 = proc.apvts.getRawParameterValue("s1x")->load();
            const double sy0 = proc.apvts.getRawParameterValue("s1y")->load();
            auto m = runScenarioTrial(proc, nSrc, 256, "lmove", 1.5, noise, lx0, ly0, sx0, sy0, true);
            std::printf("P7_CTEST N=%d bs=256 lmove mean=%.1f%% p99=%.1f%% max=%.1f%% over100=%d/%d finite=%d\n",
                        nSrc, m.meanPct, m.p99Pct, m.maxPct, m.underruns, m.totalBlocks, (int)m.finiteAndNonSilent);
            if (!m.finiteAndNonSilent) ++bad;
        }
        if (!runListenerDragClickCheck(256, 4.0)) ++bad;
        setAffinity4Cores(false, affDesc, origMask);
        return bad ? 1 : 0;
    }

    if (mode == "click") {
        int bad = 0;
        for (int bs : {128, 256}) {
            if (!runListenerDragClickCheck(bs, 6.0)) ++bad;
        }
        return bad ? 1 : 0;
    }

    if (mode == "accept") {
        setAffinity4Cores(true, affDesc, origMask);
        std::printf("P7_ACCEPT affinity: %s duration=%.1fs\n", affDesc.c_str(), acceptSec);
        const auto noise = makeNoiseCycle(48000);
        int bad = 0;
        {
            VerbProcessor proc;
            if (!loadVslHallScene(proc, 20, 256, 48000.0)) return 1;
            const double lx0 = proc.apvts.getRawParameterValue("lx")->load();
            const double ly0 = proc.apvts.getRawParameterValue("ly")->load();
            const double sx0 = proc.apvts.getRawParameterValue("s1x")->load();
            const double sy0 = proc.apvts.getRawParameterValue("s1y")->load();
            auto m = runScenarioTrial(proc, 20, 256, "lmove", acceptSec, noise, lx0, ly0, sx0, sy0, true);
            std::printf("P7_ACCEPT N=20 bs=256 lmove %.0fs: mean=%.2f%% p50=%.2f%% p99=%.2f%% max=%.2f%% underruns=%d/%d audioCpu=%.3f workerCpu=%.3f actW=%d totOther=%d est=%llu\n",
                        acceptSec, m.meanPct, m.p50Pct, m.p99Pct, m.maxPct, m.underruns, m.totalBlocks,
                        m.audioCpuPerAudioSec, m.workerCpuPerAudioSec, m.activeWorkers, m.totalOtherThreads,
                        (unsigned long long)m.motionEstimates);
            if (!m.finiteAndNonSilent || m.p99Pct > 70.0 || m.underruns > 0) ++bad;
        }
        {
            VerbProcessor proc;
            if (!loadVslHallScene(proc, 20, 128, 48000.0)) return 1;
            const double lx0 = proc.apvts.getRawParameterValue("lx")->load();
            const double ly0 = proc.apvts.getRawParameterValue("ly")->load();
            const double sx0 = proc.apvts.getRawParameterValue("s1x")->load();
            const double sy0 = proc.apvts.getRawParameterValue("s1y")->load();
            auto m = runScenarioTrial(proc, 20, 128, "lmove", 15.0, noise, lx0, ly0, sx0, sy0, true);
            std::printf("P7_ACCEPT_128 N=20 bs=128 lmove 15s: mean=%.2f%% p50=%.2f%% p99=%.2f%% max=%.2f%% underruns=%d/%d audioCpu=%.3f workerCpu=%.3f actW=%d\n",
                        m.meanPct, m.p50Pct, m.p99Pct, m.maxPct, m.underruns, m.totalBlocks,
                        m.audioCpuPerAudioSec, m.workerCpuPerAudioSec, m.activeWorkers);
            if (!m.finiteAndNonSilent) ++bad;
        }
        setAffinity4Cores(false, affDesc, origMask);
        return bad ? 1 : 0;
    }

    if (mode == "profile") {
        setAffinity4Cores(true, affDesc, origMask);
        std::printf("P7_PROFILE affinity: %s\n", affDesc.c_str());
        const auto noise = makeNoiseCycle(48000);
        struct ProfCase { int n; int bs; std::string sc; double sec; };
        const std::vector<ProfCase> cases = {
            {1,  256, "rest",  2.0},
            {1,  256, "lmove", 2.0},
            {20, 256, "rest",  2.0},
            {20, 256, "smove", 2.0},
            {20, 256, "lmove", 8.0},
            {20, 128, "rest",  2.0},
            {20, 128, "lmove", 4.0},
        };
        for (const auto& c : cases) {
            VerbProcessor proc;
            if (!loadVslHallScene(proc, c.n, c.bs, 48000.0)) return 1;
            const double lx0 = proc.apvts.getRawParameterValue("lx")->load();
            const double ly0 = proc.apvts.getRawParameterValue("ly")->load();
            const double sx0 = proc.apvts.getRawParameterValue("s1x")->load();
            const double sy0 = proc.apvts.getRawParameterValue("s1y")->load();
            auto m = runScenarioTrial(proc, c.n, c.bs, c.sc, c.sec, noise, lx0, ly0, sx0, sy0, true);
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
            std::printf("\n[PROFILE] N=%d bs=%d %s sec=%.1f (blocks=%d, budget=%.1fus)\n",
                        c.n, c.bs, c.sc.c_str(), c.sec, m.totalBlocks, budgetUs);
            std::printf("  procBlock: mean=%.1fus (%.2f%%) p99=%.1fus (%.2f%%) max=%.1fus (%.2f%%) over100=%d | engProcess=%.1fus (%.2f%%) procOverhead=%.1fus (%.2f%%)\n",
                        m.meanUs, m.meanPct, m.p99Us, m.p99Pct, m.maxUs, m.maxPct, m.underruns,
                        engUs, 100.0 * engUs / budgetUs, ovhUs, 100.0 * ovhUs / budgetUs);
            std::printf("  process phases (us/blk | %% budget): startPeriod=%.1f (%.1f%%) bandSplit=%.1f (%.1f%%) lateFeed=%.1f (%.1f%%) renderTaps=%.1f (%.1f%%) diffuse=%.1f (%.1f%%) creativeWet=%.1f (%.1f%%) endPeriod=%.1f (%.1f%%) tailBlock=%.1f (%.1f%%) other=%.1f\n",
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
            std::printf("  validate: calls=%llu supportEvals=%llu softLegEvals=%llu | ampMemo hit=%llu eval=%llu def=%llu\n",
                        (unsigned long long)p.validateCalls,
                        (unsigned long long)p.validateSupportEvals,
                        (unsigned long long)p.validateSoftLegEvals,
                        (unsigned long long)p.validateAmpMemoHit,
                        (unsigned long long)p.validateAmpMemoEval,
                        (unsigned long long)p.validateAmpMemoDeferred);
            std::printf("  renderTaps/blk: skip=%.1f stat=%.1f moving=%.1f\n",
                        (double)p.tapsSkipped / nb, (double)p.tapsStat / nb, (double)p.tapsMoving / nb);
            std::printf("  worker: stepCalls=%llu motionEsts=%llu (%.1fms) rebuilds=%llu (cancel=%llu, %.1fms) bakeStep=%.1fms | trg settle=%llu legacy=%llu jump=%llu | wrkCpu=%.3f actW=%d poolW=%d\n",
                        (unsigned long long)p.workerStepCalls,
                        (unsigned long long)p.workerMotionEsts, (double)p.workerMotionEstNs * 1e-6,
                        (unsigned long long)p.workerRebuilds, (unsigned long long)p.workerRebuildsCancelled, (double)p.workerRebuildNs * 1e-6,
                        (double)p.workerBakeStepNs * 1e-6,
                        (unsigned long long)p.workerSettleTriggers, (unsigned long long)p.workerLegacyTriggers, (unsigned long long)p.workerJumpTriggers,
                        m.workerCpuPerAudioSec, m.activeWorkers, m.poolWorkersReported);
            std::fflush(stdout);
        }
        setAffinity4Cores(false, affDesc, origMask);
        return 0;
    }

    const std::vector<int> nList = parseIntList("VERB_P7_N_LIST", {1, 5, 10, 20});
    const std::vector<int> bsList = parseIntList("VERB_P7_BS_LIST", {128, 256, 512});
    const std::vector<std::string> scenarios = {"rest", "lmove", "smove"};
    const auto noise = makeNoiseCycle(48000);

    const std::filesystem::path csvPath = outDir / ("p7_" + tag + ".csv");
    std::ofstream csv(csvPath);
    csv << "affinity,N,bs,scenario,trials,secPerTrial,meanPct,p50Pct,p99Pct,maxPct,meanUs,p99Us,maxUs,underrunsSum,totalBlocksSum,audioCpuPerSec,workerCpuPerSec,activeWorkers,totalOtherThreads,motionEstimates\n";

    std::vector<bool> affModes;
    if (mode == "unrestricted") affModes = {false};
    else if (mode == "4core") affModes = {true};
    else affModes = {false, true};

    int bad = 0;
    for (bool restrict4 : affModes) {
        if (!setAffinity4Cores(restrict4, affDesc, origMask)) {
            std::printf("FAIL P7: SetProcessAffinityMask failed\n");
            return 1;
        }
        const char* affLabel = restrict4 ? "4core" : "unrestricted";
        std::printf("\n--- Affinity mode: %s (%s) ---\n", affLabel, affDesc.c_str());

        for (int nSrc : nList) {
            for (int bs : bsList) {
                VerbProcessor proc;
                if (!loadVslHallScene(proc, nSrc, bs, 48000.0)) return 1;
                const double lx0 = proc.apvts.getRawParameterValue("lx")->load();
                const double ly0 = proc.apvts.getRawParameterValue("ly")->load();
                const double sx0 = proc.apvts.getRawParameterValue("s1x")->load();
                const double sy0 = proc.apvts.getRawParameterValue("s1y")->load();
                const double sec = (bs >= 512) ? std::max(trialSec, 2.5) : trialSec;

                for (const auto& sc : scenarios) {
                    std::vector<double> meanP, p50P, p99P, maxP, meanU, p99U, maxU, aCpu, wCpu;
                    int overSum = 0, blkSum = 0, actW = 0, totOther = 0;
                    uint64_t estSum = 0;
                    for (int tr = 0; tr < nTrials; ++tr) {
                        auto m = runScenarioTrial(proc, nSrc, bs, sc, sec, noise, lx0, ly0, sx0, sy0, true);
                        meanP.push_back(m.meanPct); p50P.push_back(m.p50Pct); p99P.push_back(m.p99Pct); maxP.push_back(m.maxPct);
                        meanU.push_back(m.meanUs); p99U.push_back(m.p99Us); maxU.push_back(m.maxUs);
                        aCpu.push_back(m.audioCpuPerAudioSec); wCpu.push_back(m.workerCpuPerAudioSec);
                        overSum += m.underruns; blkSum += m.totalBlocks;
                        actW = std::max(actW, m.activeWorkers);
                        totOther = std::max(totOther, m.totalOtherThreads);
                        estSum += m.motionEstimates;
                        if (!m.finiteAndNonSilent) ++bad;
                    }
                    const double medMean = medianOf(meanP), medP50 = medianOf(p50P), medP99 = medianOf(p99P), medMax = medianOf(maxP);
                    const double medMeanUs = medianOf(meanU), medP99Us = medianOf(p99U), medMaxUs = medianOf(maxU);
                    const double medACpu = medianOf(aCpu), medWCpu = medianOf(wCpu);
                    std::printf("P7 %-12s N=%-2d bs=%-3d %-5s | mean=%6.2f%% p50=%6.2f%% p99=%6.2f%% max=%7.2f%% | >100%%=%3d/%-4d | audCpu=%.3f wrkCpu=%.3f actW=%d totO=%d est=%llu\n",
                                affLabel, nSrc, bs, sc.c_str(),
                                medMean, medP50, medP99, medMax, overSum, blkSum,
                                medACpu, medWCpu, actW, totOther, (unsigned long long)estSum);
                    csv << affLabel << "," << nSrc << "," << bs << "," << sc << "," << nTrials << "," << sec << ","
                        << std::fixed << std::setprecision(3)
                        << medMean << "," << medP50 << "," << medP99 << "," << medMax << ","
                        << medMeanUs << "," << medP99Us << "," << medMaxUs << ","
                        << overSum << "," << blkSum << "," << medACpu << "," << medWCpu << ","
                        << actW << "," << totOther << "," << (unsigned long long)estSum << "\n";
                    csv.flush();
                }

                // Baked listener-move scenario for N=1 and N=20 at bs=256
                if (doBake && bs == 256 && (nSrc == 1 || nSrc == 20)) {
                    verbp::MovementPath lp;
                    lp.enabled = true;
                    lp.smooth = true;
                    lp.closed = true;
                    lp.points = {{{lx0 - 0.75, ly0, 1.2}}, {{lx0, ly0 + 0.25, 1.2}}, {{lx0 + 0.75, ly0, 1.2}}, {{lx0, ly0 - 0.25, 1.2}}};
                    proc.setMovementPath(verbp::kListener, lp);
                    if (proc.startPathBake(verbp::kListener)) {
                        juce::AudioBuffer<float> bb(64, bs);
                        juce::MidiBuffer bm;
                        const auto b0 = std::chrono::steady_clock::now();
                        uint64_t ticks = 0;
                        while (proc.pathBakePercent() < 100 && std::chrono::duration<double>(std::chrono::steady_clock::now() - b0).count() < 180.0) {
                            bb.clear();
                            proc.processBlock(bb, bm);
                            if (++ticks % 16 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
                            juce::Thread::sleep(1);
                        }
                        const double bakeSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - b0).count();
                        if (proc.pathBakePercent() == 100) {
                            std::printf("  P7 bake ready for N=%d in %.1fs (workers=%d)\n", nSrc, bakeSec, E9bAccess::engine(proc)->bakeStats().workers);
                            std::vector<double> meanP, p50P, p99P, maxP, meanU, p99U, maxU, aCpu, wCpu;
                            int overSum = 0, blkSum = 0, actW = 0, totOther = 0;
                            uint64_t estSum = 0;
                            for (int tr = 0; tr < nTrials; ++tr) {
                                auto m = runScenarioTrial(proc, nSrc, bs, "bake", sec, noise, lx0, ly0, sx0, sy0, true);
                                meanP.push_back(m.meanPct); p50P.push_back(m.p50Pct); p99P.push_back(m.p99Pct); maxP.push_back(m.maxPct);
                                meanU.push_back(m.meanUs); p99U.push_back(m.p99Us); maxU.push_back(m.maxUs);
                                aCpu.push_back(m.audioCpuPerAudioSec); wCpu.push_back(m.workerCpuPerAudioSec);
                                overSum += m.underruns; blkSum += m.totalBlocks;
                                actW = std::max(actW, m.activeWorkers);
                                totOther = std::max(totOther, m.totalOtherThreads);
                                estSum += m.motionEstimates;
                                if (!m.finiteAndNonSilent) ++bad;
                            }
                            const double medMean = medianOf(meanP), medP50 = medianOf(p50P), medP99 = medianOf(p99P), medMax = medianOf(maxP);
                            const double medMeanUs = medianOf(meanU), medP99Us = medianOf(p99U), medMaxUs = medianOf(maxU);
                            const double medACpu = medianOf(aCpu), medWCpu = medianOf(wCpu);
                            std::printf("P7 %-12s N=%-2d bs=%-3d %-5s | mean=%6.2f%% p50=%6.2f%% p99=%6.2f%% max=%7.2f%% | >100%%=%3d/%-4d | audCpu=%.3f wrkCpu=%.3f actW=%d totO=%d est=%llu\n",
                                        affLabel, nSrc, bs, "bake",
                                        medMean, medP50, medP99, medMax, overSum, blkSum,
                                        medACpu, medWCpu, actW, totOther, (unsigned long long)estSum);
                            csv << affLabel << "," << nSrc << "," << bs << ",bake," << nTrials << "," << sec << ","
                                << std::fixed << std::setprecision(3)
                                << medMean << "," << medP50 << "," << medP99 << "," << medMax << ","
                                << medMeanUs << "," << medP99Us << "," << medMaxUs << ","
                                << overSum << "," << blkSum << "," << medACpu << "," << medWCpu << ","
                                << actW << "," << totOther << "," << (unsigned long long)estSum << "\n";
                            csv.flush();
                        } else {
                            std::printf("  P7 bake timeout for N=%d (pct=%d)\n", nSrc, proc.pathBakePercent());
                        }
                        proc.clearPathBake();
                    }
                }
            }
        }
    }

    setAffinity4Cores(false, affDesc, origMask);
    std::printf("P7 CSV written to %s\n", csvPath.string().c_str());
    return bad ? 1 : 0;
}

} // namespace verb_p7

static int testP7() { return verb_p7::run(); }
