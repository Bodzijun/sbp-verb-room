#include "layouts.hpp"
#include "rt_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
thread_local bool measuringAllocations = false;
thread_local size_t allocations = 0;
}

void* operator new(size_t size) {
    if (measuringAllocations) ++allocations;
    if (auto* p = std::malloc(std::max(size_t{1}, size))) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

using namespace verb;
using Clock = std::chrono::steady_clock;
constexpr int block = 128;
constexpr double rate = 48000;
int failures = 0;

static void check(bool ok, const char* text) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}

static Scene room(int sources, int receivers) {
    std::istringstream description(R"(
sr 48000
length 0.8
height 3
material hard 0.12 0.15 0.20 0.25 0.30 0.35 0.40 0.10 0.70
material soft 0.05 0.10 0.25 0.45 0.65 0.70 0.70 0.20 0.60
floor soft
ceiling hard
wall 0 8 0 0 hard
wall 0 0 6 0 hard
wall 6 0 6 8 hard
wall 6 8 0 8 hard
listener 3 2 1.5 90
source Voice 3 6 1.5 -90
receiver L 3 2 1.5 55 0.5
receiver R 3 2 1.5 -55 0.5
)");
    auto scene = parseScene(description, "Motion test room");
    scene.sources.resize((size_t)sources, scene.sources.front());
    for (int s = 0; s < sources; ++s) {
        scene.sources[(size_t)s].pos.x += .1 * (s % 7 - 3);
        scene.sources[(size_t)s].pattern = (s % 3 == 0) ? .5 : 0.;
    }
    scene.receivers = makeReceiverArray(receivers == 16 ? Layout::AmbiX3 : Layout::StereoORTF,
                                        scene.listenerPos, .1, receivers);
    return scene;
}

static EngineConfig config(bool motion, bool shared) {
    EngineConfig cfg;
    cfg.erOrder = 1;
    cfg.tailRays = 6000;
    cfg.tailThread = false;
    cfg.manualWorker = true;
    cfg.sharedTail = shared;
    cfg.motionTail = motion;
    cfg.rebuildWorkers = 2;
    return cfg;
}

struct Audio {
    std::vector<std::array<float, block>> input, output;
    std::vector<const float*> in;
    std::vector<float*> out;
    std::array<float, block> detector{};
    int sample = 0;
    explicit Audio(int s, int r) : input((size_t)s), output((size_t)r) {
        for (auto& ch : input) in.push_back(ch.data());
        for (auto& ch : output) out.push_back(ch.data());
    }
    void tone(bool silent = false) {
        for (size_t s = 0; s < input.size(); ++s)
            for (int i = 0; i < block; ++i)
                input[s][i] = silent ? 0.f : float(.08 / std::sqrt(double(input.size())) *
                    std::sin(2. * kPi * (330. + 55. * (s % 6)) * (sample + i) / rate + double(s)));
    }
    void run(Engine& engine, int split = block, bool sidechain = false) {
        if (sidechain) detector.fill(.7f);
        measuringAllocations = true;
        for (int i = 0; i < block; i += split) {
            for (size_t s = 0; s < in.size(); ++s) in[s] = input[s].data() + i;
            for (size_t r = 0; r < out.size(); ++r) out[r] = output[r].data() + i;
            engine.process(in.data(), out.data(), std::min(split, block - i), sidechain ? detector.data() + i : nullptr);
        }
        measuringAllocations = false;
        sample += block;
    }
    double energy() const {
        double e = 0.;
        for (const auto& ch : output)
            for (float x : ch) e += x * x;
        return e;
    }
};

static void behavior(int sources, int channels) {
    Scene scene = room(sources, channels);
    const auto cfg = config(true, sources > 4);
    Engine engine(scene, cfg);
    Audio audio(sources, channels);
    check(engine.motionTailState() == Engine::MotionTailState::Preparing && !engine.tailIsCurrent(),
          "starts preparing");
    double early = 0., wet = 0.;
    for (int k = 0; k < 40; ++k) {
        audio.tone();
        audio.run(engine);
        early += audio.energy();
    }
    engine.setSoloMask(4);
    for (int k = 0; k < 20; ++k) {
        audio.tone();
        audio.run(engine);
        if (k >= 5) wet += audio.energy();
    }
    check(early > 0. && wet == 0., "first load plays Direct/ER only");
    engine.runWorkerStep();
    for (int k = 0; k < 1000 && (k < 80 || engine.motionStats().transitionsDone == 0); ++k) {
        audio.tone();
        audio.run(engine);
        wet += audio.energy();
    }
    check(engine.tailIsCurrent() && wet > 0. && engine.motionStats().transitionsDone == 1,
          "ready photo fades in");
    const int builds = engine.tailRefreshCount();
    for (int k = 0; k < 30; ++k) {
        engine.setSourcePosition(0, 1.2 + .02 * k, 2.5, 1.3);
        engine.setSourceAzimuth(0, 23. * k);
        engine.setListener(4.2, 1.1 + .02 * k, 1.6, 14. * k);
        if (k == 10) engine.notifyJump();
        audio.tone();
        audio.run(engine);
        engine.runWorkerStep();
    }
    check(engine.tailIsCurrent() && engine.tailRefreshCount() == builds, "pose updates and seeks keep the photo");
    Engine offlineA(scene, cfg), offlineB(scene, cfg);
    const bool copied = offlineA.copyTailForOffline(engine) && offlineB.copyTailForOffline(engine);
    offlineA.setSoloMask(4);
    offlineB.setSoloMask(4);
    Audio a(sources, channels), b(sources, channels);
    float maxOfflineDelta = 0.f;
    for (int k = 0; k < 96; ++k) {
        const double x = 2. + .4 * std::sin(k * .04);
        offlineA.setSourcePosition(0, x, 2.5, 1.3);
        offlineB.setSourcePosition(0, x, 2.5, 1.3);
        a.tone();
        b.tone();
        a.run(offlineA);
        b.run(offlineB, 32);
        for (int r = 0; r < channels; ++r)
            for (int j = 0; j < block; ++j)
                maxOfflineDelta = std::max(maxOfflineDelta, std::abs(a.output[(size_t)r][j] - b.output[(size_t)r][j]));
    }
    std::printf("offline %d/%d: copied %d, max delta %.9g FS\n", sources, channels, copied, maxOfflineDelta);
    check(copied && maxOfflineDelta < 1e-6f, "offline photo uses the same DSP at different host block boundaries");

    auto changed = scene;
    for (double& alpha : changed.mats[0].alpha) alpha = std::min(.9, alpha + .18);
    for (auto& wall : changed.walls) {
        if (wall.ax == 6.) wall.ax = 6.4;
        if (wall.bx == 6.) wall.bx = 6.4;
    }
    check(engine.setScene(changed) && !engine.tailIsCurrent(), "geometry/material edit marks photo as preparing");
    double retained = 0.;
    for (int k = 0; k < 50; ++k) {
        audio.tone();
        audio.run(engine);
        retained += audio.energy();
    }
    check(retained > 0. && !engine.tailIsCurrent(), "geometry/material edit retains the previous wet");
    engine.runWorkerStep();
    engine.notifyJump(); // a jump at the publication must not abort the handover
    float previous = audio.output[0].back(), maxJump = 0.f, atSwap = 0.f;
    for (int k = 0; k < 1000 && (k < 80 || engine.motionStats().transitionsDone == 1); ++k) {
        audio.tone();
        audio.run(engine);
        if (k == 0) atSwap = std::abs(audio.output[0][0] - previous);
        for (float x : audio.output[0]) {
            maxJump = std::max(maxJump, std::abs(x - previous));
            previous = x;
        }
    }
    std::printf("swap %d/%d: boundary %.9f FS, transition max %.9f FS\n",
                sources, channels, atSwap, maxJump);
    check(engine.tailIsCurrent() && engine.motionStats().transitionsDone == 2 && maxJump < .25f,
          "new wet crosses over during playback");

    engine.setMotionTail(false);
    audio.tone();
    audio.run(engine);
    check(engine.motionTailState() == Engine::MotionTailState::Off, "engine switch returns to Off");
}

static void freezeAndStalePhoto() {
    auto scene = room(1, 2);
    const auto cfg = config(true, false);
    Engine edited(scene, cfg), control(scene, cfg);
    edited.runWorkerStep();
    control.runWorkerStep();
    Audio a(1, 2), b(1, 2);
    edited.setSoloMask(4);
    control.setSoloMask(4);
    const auto runPair = [&] {
        a.tone();
        b.tone();
        a.run(edited, block, true);
        b.run(control, block, true);
    };
    for (int k = 0; k < 2000 && (k < 650 || edited.motionStats().transitionsDone == 0); ++k) runPair();
    check(a.output == b.output, "independent builds of the same state render identically");
    edited.setCreativeWet(25., 65., true, 12., 150.);
    control.setCreativeWet(25., 65., true, 12., 150.);
    for (int k = 0; k < 250; ++k) runPair();
    const auto started = edited.motionStats().transitionsStarted;
    auto changed = scene;
    changed.mats[0].alpha.fill(.65);
    check(edited.setScene(changed), "queue material edit while Freeze is held");
    edited.runWorkerStep();
    edited.setTailQuality(8000, false);
    runPair();
    check(!edited.tailIsCurrent() && edited.motionStats().transitionsStarted == started,
          "a stale photo queued before a quality edit is never used");
    edited.runWorkerStep();
    double held = 0.;
    float maxDelta = 0.f;
    for (int k = 0; k < 2000 && (k < 650 || edited.motionStats().transitionsDone < 2); ++k) {
        runPair();
        held += a.energy();
        for (int r = 0; r < 2; ++r)
            for (int j = 0; j < block; ++j)
                maxDelta = std::max(maxDelta, std::abs(a.output[(size_t)r][j] - b.output[(size_t)r][j]));
    }
    std::printf("freeze A/B: energy %.9g, max delta %.9g FS, duck %.4f dB, ready %d, swaps %llu\n",
                held, maxDelta, edited.duckReductionDb(), edited.tailIsCurrent(),
                (unsigned long long)edited.motionStats().transitionsDone);
    check(held > 0. && maxDelta == 0.f && edited.tailIsCurrent() &&
          edited.motionStats().transitionsDone == 2 && edited.duckReductionDb() > 11.f,
          "held Freeze is identical to the control across a photo swap with Duck, Predelay and Width");
}

static Scene lRoomScene(int sources, const std::vector<Vec3>& poses, const std::vector<double>& azDeg) {
    std::istringstream description(R"(
sr 48000
length 3.0
height 3.0
order 3
rays 80000
material plaster 0.02 0.03 0.04 0.05 0.06 0.07 0.08 0.05
material carpet  0.05 0.10 0.25 0.45 0.65 0.70 0.70 0.15
material curtain 0.10 0.30 0.55 0.70 0.75 0.75 0.70 0.20
material glass   0.18 0.06 0.04 0.03 0.02 0.02 0.02 0.02
floor carpet
ceiling plaster
wall 0 0 10 0 plaster
wall 10 0 10 4 plaster
wall 10 4 5 4 glass
wall 5 4 5 8 plaster
wall 5 8 0 8 plaster
wall 0 8 0 0 curtain
wall 3 0 3 3 plaster
wall 3 5 3 8 plaster
source Man 2 6 1.7
receiver L 7 3 1.5 30 0.5
receiver R 7 3 1.5 -30 0.5
)");
    auto scene = parseScene(description, "l_room_5_1.txt");
    scene.sr = 48000;
    scene.hasListener = true;
    scene.listenerPos = {7.0, 2.5, 1.5};
    scene.listenerYaw = 180.0;
    scene.sources.resize((size_t)sources, scene.sources.front());
    for (int s = 0; s < sources; ++s) {
        scene.sources[(size_t)s].name = "S" + std::to_string(s + 1);
        scene.sources[(size_t)s].pos = poses[(size_t)s];
        scene.sources[(size_t)s].azimuthDeg = azDeg[(size_t)s];
        scene.sources[(size_t)s].pattern = (s % 3 == 0) ? .5 : 0.;
    }
    scene.receivers = makeReceiverArray(Layout::StereoORTF, scene.listenerPos, .1, 2);
    return scene;
}

static void sharedTailAndPerSourceRegression() {
    // 1. S=1..4 per-source convolver stability vs stored reference energy and anchor exactness
    for (int sCount : {1, 4}) {
        auto sc = room(sCount, 2);
        auto motCfg = config(true, false);
        motCfg.tailRays = 12000;
        auto exCfg = motCfg;
        exCfg.motionTail = false;

        Engine photo(sc, motCfg);
        photo.runWorkerStep();
        Audio warm(sCount, 2);
        warm.tone(true);
        for (int k = 0; k < 8; ++k) warm.run(photo);

        // Anchor check: 0 m displacement must match exact per-source engine within 1e-4 dB
        Engine motAnchor(sc, motCfg), exAnchor(sc, exCfg);
        check(motAnchor.copyTailForOffline(photo), "copy S<=4 photo for anchor check");
        motAnchor.setSoloMask(4);
        exAnchor.setSoloMask(4);
        for (int k = 0; k < 96; ++k) { warm.run(motAnchor); warm.run(exAnchor); }
        Audio aMot(sCount, 2), aEx(sCount, 2);
        double eMotAnchor = 0., eExAnchor = 0.;
        for (int k = 0; k < 150; ++k) {
            aMot.tone(); aEx.tone();
            aMot.run(motAnchor); aEx.run(exAnchor);
            eMotAnchor += aMot.energy();
            eExAnchor += aEx.energy();
        }
        const double anchorDiffDb = std::abs(10. * std::log10(std::max(eMotAnchor, 1e-24) / std::max(eExAnchor, 1e-24)));

        // Displaced check against stored reference energy
        Engine motDisp(sc, motCfg);
        motDisp.copyTailForOffline(photo);
        motDisp.setSoloMask(4);
        for (int s = 0; s < sCount; ++s) {
            motDisp.setSourcePosition(s, 1.4 + 1.1 * s, 2.2 + 0.8 * (s % 2), 1.4);
            motDisp.setSourceAzimuth(s, -45. + 30. * s);
        }
        for (int k = 0; k < 96; ++k) warm.run(motDisp);
        Audio aDisp(sCount, 2);
        double eDisp = 0.;
        for (int k = 0; k < 150; ++k) {
            aDisp.tone();
            aDisp.run(motDisp);
            eDisp += aDisp.energy();
        }
        const double dispDb = 10. * std::log10(std::max(eDisp, 1e-24));
        const double mo4bScalarDb = (sCount == 1) ? 11.1492 : 13.3639;
        const double expectedDb = (sCount == 1) ? 11.0709 : 13.6216;
        const double storedDiffDb = std::abs(dispDb - expectedDb);
        const double mo4bDiffDb = std::abs(dispDb - mo4bScalarDb);
        std::printf("S=%d per-source regression: anchorDiff=%.6f dB, displaced=%.4f dB (ref %.4f dB, diff %.4f dB, vs MO4b %.4f dB)\n",
                    sCount, anchorDiffDb, dispDb, expectedDb, storedDiffDb, mo4bDiffDb);
        check(anchorDiffDb < 1e-4 && storedDiffDb < 0.01 && mo4bDiffDb < 0.30,
              sCount == 1 ? "S=1 per-source convolver matches stored reference within 0.01 dB and MO4b within 0.30 dB"
                          : "S=4 per-source convolver matches stored reference within 0.01 dB and MO4b within 0.30 dB");
    }

    // 2. L-room 5B shared-tail regression (2 sources in main room + 6 behind partition, S=8)
    std::vector<Vec3> anchorPoses(8), targetPoses(8);
    std::vector<double> anchorAz(8, 0.), targetAz(8, -60.);
    for (int s = 0; s < 8; ++s)
        anchorPoses[(size_t)s] = {1.5 + 1.0 * s, 1.8 + 0.4 * (s % 3), 1.5};
    targetPoses[0] = {5.6, 2.2, 1.5}; targetAz[0] = 15.;
    targetPoses[1] = {8.3, 1.8, 1.5}; targetAz[1] = 155.;
    const Vec3 upper[6] = {
        {1.2, 5.2, 1.5}, {2.8, 5.5, 1.5}, {4.1, 5.3, 1.5},
        {1.1, 7.1, 1.5}, {2.6, 7.2, 1.5}, {4.0, 7.0, 1.5}
    };
    for (int k = 0; k < 6; ++k) targetPoses[(size_t)(k + 2)] = upper[k];

    Scene scInit = lRoomScene(8, anchorPoses, anchorAz);
    Scene scTarget = lRoomScene(8, targetPoses, targetAz);
    auto motCfg = config(true, true);
    motCfg.tailRays = 48000;
    motCfg.rebuildWorkers = 8;
    auto exCfg = motCfg;
    exCfg.motionTail = false;
    exCfg.sharedTail = false;

    Engine photo(scInit, motCfg);
    photo.runWorkerStep();
    Audio warm8(8, 2);
    warm8.tone(true);
    for (int k = 0; k < 8; ++k) warm8.run(photo);

    Engine exact8(scTarget, exCfg);
    exact8.setSoloMask(4);
    for (int k = 0; k < 96; ++k) warm8.run(exact8);

    constexpr int nBlocks = 600; // 1.6 s (six 250-ms windows + settle)
    auto evalCase = [&](int soloSrc, double& outBiasDb, double& outEnvRmsDb) {
        Engine mot(scInit, motCfg);
        mot.copyTailForOffline(photo);
        mot.setSoloMask(4);
        for (int s = 0; s < 8; ++s) {
            mot.setSourcePosition(s, targetPoses[(size_t)s].x, targetPoses[(size_t)s].y, targetPoses[(size_t)s].z);
            mot.setSourceAzimuth(s, targetAz[(size_t)s]);
        }
        for (int k = 0; k < 96; ++k) warm8.run(mot);
        for (int k = 0; k < 1500; ++k) warm8.run(exact8); // flush full 3.0 s tail from exact8 FDL

        Audio aM(8, 2), aE(8, 2);
        double totM = 0., totE = 0., winM = 0., winE = 0., sumSqDb = 0.;
        int winSamples = 0, winCount = 0;
        std::array<uint32_t, 8> rngState{};
        std::array<double, 8> lp1{}, lp2{};
        for (int s = 0; s < 8; ++s) rngState[(size_t)s] = 4242u + 7919u * static_cast<uint32_t>(s);
        for (int k = 0; k < nBlocks; ++k) {
            for (int s = 0; s < 8; ++s) {
                const bool active = (soloSrc < 0) || (s == soloSrc);
                const int group = s % 4;
                const double tSec = double(k * block) / rate;
                const double phrase = (soloSrc < 0 && tSec < 1.0 && (int(tSec / 0.25) % 4) != group) ? 0.08 : 1.0;
                const double a1 = 1. - std::exp(-2. * kPi * (400. + 210. * (s % 6)) / rate);
                const double a2 = 1. - std::exp(-2. * kPi * (1600. + 420. * (s % 5)) / rate);
                const double f0 = 105. + 31. * s;
                uint32_t st = rngState[(size_t)s];
                for (int i = 0; i < block; ++i) {
                    st = st * 1664525u + 1013904223u;
                    const double white = (double(int32_t(st)) / 2147483648.);
                    lp1[(size_t)s] += a1 * (white - lp1[(size_t)s]);
                    lp2[(size_t)s] += a2 * (white - lp2[(size_t)s]);
                    double sig = 0.;
                    if (active) {
                        const double t = double(k * block + i) / rate;
                        const double bandNoise = 0.50 * lp1[(size_t)s] + 0.45 * (lp2[(size_t)s] - lp1[(size_t)s]) + 0.25 * (white - lp2[(size_t)s]);
                        const double harm = 0.45 * std::sin(2. * kPi * f0 * t) + 0.28 * std::sin(4. * kPi * f0 * t + 0.7);
                        sig = phrase * 0.08 * (0.65 * bandNoise + 0.35 * harm);
                    }
                    aM.input[(size_t)s][i] = aE.input[(size_t)s][i] = static_cast<float>(sig);
                }
                rngState[(size_t)s] = st;
            }
            aM.run(mot);
            aE.run(exact8);
            const double bM = aM.energy(), bE = aE.energy();
            totM += bM; totE += bE;
            winM += bM; winE += bE;
            winSamples += block;
            if (winSamples >= 12000) {
                if (10. * std::log10(std::max(winE, 1e-30) / (2. * winSamples)) > -75.) {
                    const double d = 10. * std::log10(std::max(winM, 1e-18) / winE);
                    sumSqDb += d * d;
                    ++winCount;
                }
                winM = winE = 0.;
                winSamples = 0;
            }
        }
        outBiasDb = 10. * std::log10(std::max(totM, 1e-24) / std::max(totE, 1e-24));
        outEnvRmsDb = winCount > 0 ? std::sqrt(sumSqDb / winCount) : 0.;
    };

    double mixBias = 0., mixEnvRms = 0.;
    double s0Bias = 0., s0EnvRms = 0.;
    double s2Bias = 0., s2EnvRms = 0.;
    double s7Bias = 0., s7EnvRms = 0.;
    evalCase(-1, mixBias, mixEnvRms);
    evalCase(0, s0Bias, s0EnvRms);
    evalCase(2, s2Bias, s2EnvRms);
    evalCase(7, s7Bias, s7EnvRms);
    std::printf("L-room 5B shared-tail regression:\n"
                "  ALL_8_SUM: bias=%+.3f dB (limit +-0.35 dB), envRMS=%.3f dB (limit 0.90 dB)\n"
                "  Solo S0 (same_room):        bias=%+.3f dB (limit +-0.80 dB), envRMS=%.3f dB\n"
                "  Solo S2 (behind_partition): bias=%+.3f dB (limit +-1.30 dB), envRMS=%.3f dB\n"
                "  Solo S7 (behind_partition): bias=%+.3f dB (limit +-0.60 dB), envRMS=%.3f dB\n",
                mixBias, mixEnvRms, s0Bias, s0EnvRms, s2Bias, s2EnvRms, s7Bias, s7EnvRms);
    check(std::abs(mixBias) < 0.35 && mixEnvRms < 0.90,
          "L-room 5B shared-tail 8-source mix bias < 0.35 dB and 250-ms envelope RMS < 0.90 dB");
    check(std::abs(s0Bias) < 0.80 && std::abs(s2Bias) < 1.30 && std::abs(s7Bias) < 0.60,
          "L-room 5B shared-tail solo source level errors stay within per-source bounds");
}

static Scene partitionY4Scene(int sources, int receivers, Vec3 src0Pos, double src0Az) {
    std::istringstream description(R"(
sr 48000
length 2.5
height 3.0
temp 20
humidity 50
order 2
rays 48000
seed 17
detect_radius 0.5
material plaster 0.03 0.04 0.05 0.06 0.07 0.08 0.09 0.08 0.40
material carpet  0.05 0.10 0.25 0.45 0.65 0.70 0.70 0.20 0.60
material curtain 0.10 0.30 0.55 0.70 0.75 0.75 0.70 0.20 0.60
floor carpet
ceiling plaster
#@verbroom-editor-v2 ceilingExtent walls
wall 0 0 8 0 plaster
wall 8 0 8 8 plaster
wall 8 8 0 8 plaster
wall 0 8 0 0 curtain
wall 0 4 4.2 4 plaster
wall 6.0 4 8 4 plaster
listener 3.2 1.8 1.5 90
source Voice 3.2 2.8 1.5 -90
receiver L 3.115 1.8 1.5 145 0.5
receiver R 3.285 1.8 1.5 35 0.5
)");
    auto sc = parseScene(description, "partition_y4");
    sc.sr = 48000;
    sc.hasListener = true;
    sc.listenerPos = {3.2, 1.8, 1.5};
    sc.listenerYaw = 90.0;
    sc.sources.resize((size_t)sources, sc.sources.front());
    sc.sources[0].name = "S1";
    sc.sources[0].pos = src0Pos;
    sc.sources[0].azimuthDeg = src0Az;
    sc.sources[0].pattern = 0.0;
    for (int s = 1; s < sources; ++s) {
        const double ang = 2.0 * kPi * (double)s / (double)sources;
        sc.sources[(size_t)s].name = "S" + std::to_string(s + 1);
        sc.sources[(size_t)s].pos = {
            std::clamp(3.2 + 1.1 * std::cos(ang), 0.8, 7.2),
            std::clamp(2.8 + 0.7 * std::sin(ang), 0.6, 3.4),
            1.5
        };
        sc.sources[(size_t)s].azimuthDeg = -90.0;
        sc.sources[(size_t)s].pattern = 0.0;
    }
    sc.receivers = makeReceiverArray(receivers == 16 ? Layout::AmbiX3 : Layout::StereoORTF,
                                     sc.listenerPos, 0.1, receivers);
    if (sc.listenerYaw != 0.0) {
        const double a = sc.listenerYaw * kPi / 180.0;
        for (auto& r : sc.receivers) {
            const Vec3 d = r.pos - sc.listenerPos;
            r.pos = sc.listenerPos + Vec3{d.x * std::cos(a) - d.y * std::sin(a), d.x * std::sin(a) + d.y * std::cos(a), d.z};
            r.azimuthDeg += sc.listenerYaw;
        }
    }
    return sc;
}

static void partitionDirectionRegression() {
    const Vec3 photoPos{3.2, 2.8, 1.5};
    const double photoAz = -90.0;

    // 1. Stereo (R=2) partition direction test for S=1 (per-source) at two behind-partition poses
    // and S=8 (sharedTail) at right_1m
    struct PoseCase {
        const char* name;
        int S;
        bool shared;
        Vec3 targetPos;
        double targetAz;
        double maxIldErrDb;
    };
    const PoseCase cases[] = {
        {"partition_y4/right_1m (S=1)",     1, false, {4.8, 5.5, 1.5}, -110.0, 0.40},
        {"partition_y4/left_of_door (S=1)", 1, false, {2.2, 5.5, 1.5},  -60.0, 0.40},
        {"partition_y4/right_1m (S=8)",     8, true,  {4.8, 5.5, 1.5}, -110.0, 0.40}
    };

    for (const auto& tc : cases) {
        Scene scPhoto = partitionY4Scene(tc.S, 2, photoPos, photoAz);
        Scene scTarget = partitionY4Scene(tc.S, 2, tc.targetPos, tc.targetAz);
        auto motCfg = config(true, tc.shared);
        motCfg.tailRays = 48000;
        motCfg.rebuildWorkers = 8;
        auto exCfg = motCfg;
        exCfg.motionTail = false;
        exCfg.sharedTail = false;

        Engine photo(scPhoto, motCfg);
        photo.runWorkerStep();
        Audio warm(tc.S, 2);
        warm.tone(true);
        for (int k = 0; k < 8; ++k) warm.run(photo);

        Engine mot(scPhoto, motCfg), exact(scTarget, exCfg);
        check(mot.copyTailForOffline(photo), "copy partition_y4 photo for direction check");
        mot.setSoloMask(4);
        exact.setSoloMask(4);
        mot.setSourcePosition(0, tc.targetPos.x, tc.targetPos.y, tc.targetPos.z);
        mot.setSourceAzimuth(0, tc.targetAz);
        for (int k = 0; k < 96; ++k) { warm.run(mot); warm.run(exact); }

        Audio aM(tc.S, 2), aE(tc.S, 2);
        double eML = 0., eMR = 0., eEL = 0., eER = 0.;
        uint32_t rng = 7777u;
        for (int k = 0; k < 400; ++k) {
            for (int s = 0; s < tc.S; ++s) {
                for (int i = 0; i < block; ++i) {
                    float v = 0.f;
                    if (s == 0 && k < 8) {
                        rng = rng * 1664525u + 1013904223u;
                        v = float(int32_t(rng) / 2147483648.0) * 0.25f;
                    }
                    aM.input[(size_t)s][i] = aE.input[(size_t)s][i] = v;
                }
            }
            aM.run(mot);
            aE.run(exact);
            for (int i = 0; i < block; ++i) {
                eML += double(aM.output[0][i]) * aM.output[0][i];
                eMR += double(aM.output[1][i]) * aM.output[1][i];
                eEL += double(aE.output[0][i]) * aE.output[0][i];
                eER += double(aE.output[1][i]) * aE.output[1][i];
            }
        }
        const double ildMotDb = 10. * std::log10(std::max(eML, 1e-24) / std::max(eMR, 1e-24));
        const double ildExDb  = 10. * std::log10(std::max(eEL, 1e-24) / std::max(eER, 1e-24));
        const double errDb = std::abs(ildMotDb - ildExDb);
        const double marginDb = tc.maxIldErrDb - errDb;
        std::printf("Direction regression [%s]: exact L-R=%+.3f dB, motion L-R=%+.3f dB, |err|=%.3f dB (limit %.2f dB, margin %+.3f dB)\n",
                    tc.name, ildExDb, ildMotDb, errDb, tc.maxIldErrDb, marginDb);
        check(ildMotDb < -1.5 && errDb <= tc.maxIldErrDb,
              "behind-partition tail shifts to doorway side and L-R error stays <= 0.40 dB");
    }

    // 2. R=16 (AmbiX 3rd order) 16-channel directional distribution error behind partition
    {
        const Vec3 targetPos{4.8, 5.5, 1.5};
        const double targetAz = -110.0;
        Scene scPhoto16 = partitionY4Scene(1, 16, photoPos, photoAz);
        Scene scTarget16 = partitionY4Scene(1, 16, targetPos, targetAz);
        auto motCfg = config(true, false);
        motCfg.tailRays = 48000;
        motCfg.rebuildWorkers = 8;
        auto exCfg = motCfg;
        exCfg.motionTail = false;

        Engine photo16(scPhoto16, motCfg);
        photo16.runWorkerStep();
        Audio warm16(1, 16);
        warm16.tone(true);
        for (int k = 0; k < 8; ++k) warm16.run(photo16);

        Engine mot16(scPhoto16, motCfg), ex16(scTarget16, exCfg);
        check(mot16.copyTailForOffline(photo16), "copy R=16 AmbiX3 photo for direction check");
        mot16.setSoloMask(4);
        ex16.setSoloMask(4);
        mot16.setSourcePosition(0, targetPos.x, targetPos.y, targetPos.z);
        mot16.setSourceAzimuth(0, targetAz);
        for (int k = 0; k < 96; ++k) { warm16.run(mot16); warm16.run(ex16); }

        Audio aM(1, 16), aE(1, 16);
        std::array<double, 16> eM{}, eE{};
        uint32_t rng = 7777u;
        for (int k = 0; k < 400; ++k) {
            for (int i = 0; i < block; ++i) {
                float v = 0.f;
                if (k < 8) {
                    rng = rng * 1664525u + 1013904223u;
                    v = float(int32_t(rng) / 2147483648.0) * 0.25f;
                }
                aM.input[0][i] = aE.input[0][i] = v;
            }
            aM.run(mot16);
            aE.run(ex16);
            for (int r = 0; r < 16; ++r) {
                for (int i = 0; i < block; ++i) {
                    eM[(size_t)r] += double(aM.output[(size_t)r][i]) * aM.output[(size_t)r][i];
                    eE[(size_t)r] += double(aE.output[(size_t)r][i]) * aE.output[(size_t)r][i];
                }
            }
        }
        double sumSq = 0.;
        for (int r = 1; r < 16; ++r) {
            const double relM = 10. * std::log10(std::max(eM[(size_t)r], 1e-24) / std::max(eM[0], 1e-24));
            const double relE = 10. * std::log10(std::max(eE[(size_t)r], 1e-24) / std::max(eE[0], 1e-24));
            const double d = relM - relE;
            sumSq += d * d;
        }
        const double distRmseDb = std::sqrt(sumSq / 15.0);
        const double yOverXMot = 10. * std::log10(std::max(eM[1], 1e-24) / std::max(eM[3], 1e-24));
        const double yOverXEx  = 10. * std::log10(std::max(eE[1], 1e-24) / std::max(eE[3], 1e-24));
        const double limitDb = 0.40;
        std::printf("AmbiX16 regression [partition_y4/right_1m]: distRMSE=%.3f dB (limit %.2f dB, margin %+.3f dB), Y/X exact=%+.3f dB, motion=%+.3f dB\n",
                    distRmseDb, limitDb, limitDb - distRmseDb, yOverXEx, yOverXMot);
        check(distRmseDb <= limitDb, "R=16 AmbiX3 16-channel directional distribution RMSE stays <= 0.40 dB");
    }
}

static void motionOffBitIdentityRegression() {
    auto hashRender = [](int sources, int receivers, bool shared, uint32_t& outFirstWord, uint32_t& outLastWord) -> uint64_t {
        Scene sc = room(sources, receivers);
        auto cfg = config(false, shared);
        cfg.adaptiveTail = false;
        Engine eng(sc, cfg);
        Audio audio(sources, receivers);
        uint64_t h = 14695981039346656037ull;
        outFirstWord = 0;
        outLastWord = 0;
        for (int k = 0; k < 64; ++k) {
            audio.tone();
            audio.run(eng);
            for (int r = 0; r < receivers; ++r) {
                for (int i = 0; i < block; ++i) {
                    uint32_t bits = 0;
                    std::memcpy(&bits, &audio.output[(size_t)r][i], sizeof(uint32_t));
                    if (k == 10 && r == 0 && i == 0) outFirstWord = bits;
                    if (k == 63 && r == receivers - 1 && i == block - 1) outLastWord = bits;
                    h ^= (uint64_t)bits;
                    h *= 1099511628211ull;
                }
            }
        }
        return h;
    };

    uint32_t w0_1 = 0, w1_1 = 0;
    uint32_t w0_8 = 0, w1_8 = 0;
    const uint64_t h1 = hashRender(1, 2, false, w0_1, w1_1);
    const uint64_t h8 = hashRender(8, 2, true, w0_8, w1_8);
    // Golden bit-exact hashes verified against pre-MO3a / pre-MO5b baseline verbcore.lib
    constexpr uint64_t kExpectedH1 = 0xedd477531491536full;
    constexpr uint64_t kExpectedH8 = 0x11ee380076ca53abull;
    constexpr uint32_t kExpectedW0_1 = 0x3bf98b52u, kExpectedW1_1 = 0xbc538ca2u;
    constexpr uint32_t kExpectedW0_8 = 0xbc31d028u, kExpectedW1_8 = 0xbcf18726u;
    std::printf("Motion Off bit-identity: S=1/R=2 hash=0x%016llx (w0=0x%08x, w1=0x%08x), S=8/R=2 hash=0x%016llx (w0=0x%08x, w1=0x%08x)\n",
                (unsigned long long)h1, w0_1, w1_1, (unsigned long long)h8, w0_8, w1_8);
    check(h1 == kExpectedH1 && h8 == kExpectedH8 &&
          w0_1 == kExpectedW0_1 && w1_1 == kExpectedW1_1 &&
          w0_8 == kExpectedW0_8 && w1_8 == kExpectedW1_8,
          "Motion Off output is bit-identical to pre-port baseline for both per-source (S=1) and shared-tail (S=8)");
}

static void benchmark() {
    std::printf("sources,channels,motion,mean_us,p99_us,dropouts,over_budget,d2_db,peak_speed_m_s\n");
    for (int channels : {2, 16}) for (int sources : {1, 8, 32}) for (bool motion : {false, true}) {
        auto scene = room(sources, channels);
        Engine engine(scene, config(motion, sources > 4));
        if (motion) engine.runWorkerStep();
        Audio audio(sources, channels);
        engine.setSoloMask(4);
        for (int k = 0; k < 650; ++k) { audio.tone(); audio.run(engine); }
        std::vector<double> elapsed(1125);
        double e = 0., d2 = 0.;
        double previous = 0., previous2 = 0.;
        int dropouts = 0, overruns = 0;
        std::vector<Vec3> prevPos((size_t)sources);
        double peakSpeed = 0.;
        for (size_t k = 0; k < elapsed.size(); ++k) {
            const double time = k * block / rate;
            for (int s = 0; s < sources; ++s) {
                const double p = time * 2.16 + s * .785398;
                const Vec3 pos{3. + 1.8 * std::sin(p) + .08 * std::cos(3.1 * p),
                               4. + 2. * std::cos(.8 * p) + .09 * std::sin(2.7 * p),
                               1.5 + .15 * std::sin(1.9 * p)};
                if (k > 0) peakSpeed = std::max(peakSpeed, norm(pos - prevPos[(size_t)s]) * rate / block);
                prevPos[(size_t)s] = pos;
                engine.setSourcePosition(s, pos.x, pos.y, pos.z);
            }
            audio.tone();
            const auto begin = Clock::now();
            audio.run(engine);
            elapsed[k] = std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
            overruns += elapsed[k] > 1e6 * block / rate;
            if (audio.energy() <= 1e-12) ++dropouts;
            for (float x : audio.output[0]) {
                e += x * x;
                const double delta = x - 2. * previous + previous2;
                d2 += delta * delta;
                previous2 = previous;
                previous = x;
            }
        }
        std::sort(elapsed.begin(), elapsed.end());
        const double mean = std::accumulate(elapsed.begin(), elapsed.end(), 0.) / elapsed.size();
        const double p99 = elapsed[(size_t)(.99 * elapsed.size())];
        std::printf("%d,%d,%d,%.3f,%.3f,%d,%d,%.3f,%.3f\n",
                    sources, channels, motion, mean, p99, dropouts, overruns, 10. * std::log10(d2 / e), peakSpeed);
        if (dropouts || !std::isfinite(d2)) ++failures;
    }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::string(argv[1]) == "bench") benchmark();
    else {
        behavior(1, 2);
        behavior(8, 2);
        behavior(8, 16);
        freezeAndStalePhoto();
        sharedTailAndPerSourceRegression();
        partitionDirectionRegression();
        motionOffBitIdentityRegression();
        auto scene = room(1, 2);
        auto cfg = config(true, false);
        cfg.tailThread = true;
        cfg.manualWorker = false;
        Engine engine(scene, cfg);
        Audio audio(1, 2);
        const auto until = Clock::now() + std::chrono::seconds(30);
        while (!engine.tailIsCurrent() && Clock::now() < until) {
            audio.tone();
            audio.run(engine);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(engine.tailIsCurrent(), "background coordinator publishes without a manual worker pump");
    }
    std::printf("process() allocations = %zu\n", allocations);
    check(allocations == 0, "audio callback does not allocate");
    return failures ? 1 : 0;
}
