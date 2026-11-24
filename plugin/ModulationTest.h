#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include "Modulation.h"
#include "MovementPath.h"
#include "P4Test.h"

namespace g4test {

static const char* kBoxScene =
    "sr 48000\nlength 1.0\nheight 4.0\n"
    "material plaster 0.05 0.05 0.06 0.07 0.08 0.09 0.10 0.05 0.30\n"
    "floor plaster\nceiling plaster\n"
    "wall 0 0 12 0 plaster\nwall 12 0 12 10 plaster\n"
    "wall 12 10 0 10 plaster\nwall 0 10 0 0 plaster\n"
    "listener 2.0 5.0 1.5 0\n"
    "source S1 3.5 5.0 1.5 0 0 0 0\n"
    "receiver L 2.0 5.0 1.5 55 0.1\n"
    "receiver R 2.0 5.0 1.5 -55 0.1\n";

struct PlayHeadMock final : juce::AudioPlayHead {
    bool playing = false;
    int64_t samples = 0;
    double sr = 48000.0;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo info;
        info.setIsPlaying(playing);
        info.setTimeInSamples(samples);
        info.setTimeInSeconds((double)samples / sr);
        return info;
    }
};

static verbp::ModulationRoute makeRoute(
    int obj, verbp::ModDriver drv, double inMin, double inMax,
    verbp::ModTarget tgt, double outMin, double outMax,
    double attMs = 0.0, double relMs = 0.0, int tgtObj = -1)
{
    verbp::ModulationRoute r;
    r.enabled = true;
    r.object = obj;
    r.driver = drv;
    r.targetObject = tgtObj;
    r.target = tgt;
    r.attackMs = attMs;
    r.releaseMs = relMs;
    r.curve.inputMin = inMin;
    r.curve.inputMax = inMax;
    r.curve.outputMin = outMin;
    r.curve.outputMax = outMax;
    r.curve.invert = false;
    r.curve.clamp = true;
    r.curve.nodeCount = 2;
    r.curve.nodes[0] = {0.0, 0.0, false};
    r.curve.nodes[1] = {1.0, 1.0, false};
    return r;
}

static double goertzelMag(const std::vector<float>& x, double sr, double freqHz) {
    const double w = 2.0 * verb::kPi * freqHz / sr;
    const double cosw = std::cos(w), sinw = std::sin(w);
    const double coeff = 2.0 * cosw;
    double q0 = 0.0, q1 = 0.0, q2 = 0.0;
    for (float s : x) {
        q0 = coeff * q1 - q2 + (double)s;
        q2 = q1;
        q1 = q0;
    }
    const double re = q1 - q2 * cosw;
    const double im = q2 * sinw;
    return std::sqrt(re * re + im * im);
}

static double butterworthLpfMagDb(double f, double fc) {
    const double r = f / fc;
    return -10.0 * std::log10(1.0 + r * r * r * r);
}

static double rbjLpfMagDb(double f, double fc, double sr) {
    double b0, b1, b2, a1, a2;
    verbp::BiquadFilter::makeLowPass(sr, fc, b0, b1, b2, a1, a2);
    const double w = 2.0 * verb::kPi * f / sr;
    const std::complex<double> z(std::cos(-w), std::sin(-w));
    const std::complex<double> num = b0 + b1 * z + b2 * z * z;
    const std::complex<double> den = 1.0 + a1 * z + a2 * z * z;
    return 20.0 * std::log10(std::max(1e-12, std::abs(num / den)));
}

static std::vector<float> captureImpulseCh0(VerbProcessor& p, int totalBlocks = 16, int warmupBlocks = 0, double amp = 1.0f) {
    const int n = 128;
    const int inCh = std::max(2, p.getTotalNumInputChannels());
    juce::AudioBuffer<float> buf(inCh, n);
    juce::MidiBuffer midi;
    for (int i = 0; i < warmupBlocks; ++i) {
        buf.clear();
        stage2Process(p, buf, midi);
    }
    std::vector<float> out;
    out.reserve((size_t)(totalBlocks * n));
    for (int b = 0; b < totalBlocks; ++b) {
        buf.clear();
        if (b == 0) {
            for (int c = 0; c < inCh; ++c) buf.setSample(c, 0, (float)amp);
        }
        stage2Process(p, buf, midi);
        for (int i = 0; i < n; ++i) out.push_back(buf.getSample(0, i));
    }
    return out;
}

static void drainTail(VerbProcessor& p, int blocks = 128) {
    const int inCh = std::max(2, p.getTotalNumInputChannels());
    juce::AudioBuffer<float> buf(inCh, 128);
    juce::MidiBuffer midi;
    auto eng = E9bAccess::engine(p);
    const auto until = juce::Time::getMillisecondCounterHiRes() + 5000.0;
    while (eng && !eng->tailIsCurrent() && juce::Time::getMillisecondCounterHiRes() < until) {
        buf.clear();
        stage2Process(p, buf, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    }
    for (int i = 0; i < blocks; ++i) {
        buf.clear();
        stage2Process(p, buf, midi);
    }
}

static void primeCreativeAndSettle(VerbProcessor& p, int blocks = 12) {
    juce::AudioBuffer<float> buf(std::max(2, p.getTotalNumInputChannels()), 128);
    juce::MidiBuffer midi;
    for (int i = 0; i < blocks; ++i) {
        buf.clear();
        stage2Process(p, buf, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    }
}

static double peakAbs(const std::vector<float>& v) {
    double m = 0.0;
    for (float x : v) m = std::max(m, (double)std::abs(x));
    return m;
}

static double rmsEnergy(const std::vector<float>& v, size_t start = 0, size_t end = 0) {
    if (end == 0 || end > v.size()) end = v.size();
    double e = 0.0;
    for (size_t i = start; i < end; ++i) e += (double)v[i] * (double)v[i];
    return std::sqrt(e / std::max<size_t>(1, end - start));
}

}  // namespace g4test

static int testG4ModulationFast() {
    const int before = fails;
    using namespace verbp;
    std::printf("G4 fast: curve/LFO math, zero routes bit-identity, legacy compat & XML/KeepIO\n");

    // 1. Curve & LFO unit math and validation
    {
        ModCurve c;
        c.inputMin = 2.0; c.inputMax = 10.0;
        c.outputMin = -24.0; c.outputMax = 0.0;
        c.invert = false; c.clamp = true;
        c.nodeCount = 3;
        c.nodes[0] = {0.0, 0.0, false};
        c.nodes[1] = {0.5, 0.25, true};
        c.nodes[2] = {1.0, 1.0, false};
        check(std::abs(c.evaluate(0.0) - (-24.0)) < 1e-12, "G4 ModCurve clamps below inputMin");
        check(std::abs(c.evaluate(6.0) - (-18.0)) < 1e-12, "G4 ModCurve evaluates interior multi-node accurately");
        check(std::abs(c.evaluate(12.0) - 0.0) < 1e-12, "G4 ModCurve clamps above inputMax");

        c.invert = true;
        check(std::abs(c.evaluate(2.0) - 0.0) < 1e-12 && std::abs(c.evaluate(10.0) - (-24.0)) < 1e-12,
              "G4 ModCurve invert flips mapping endpoints");

        c.invert = false; c.clamp = false; c.nodeCount = 2;
        c.nodes[0] = {0.0, 0.0, false}; c.nodes[1] = {1.0, 1.0, false};
        check(std::abs(c.evaluate(14.0) - 12.0) < 1e-12, "G4 ModCurve unclamped extrapolates linearly above inputMax");

        c.nodes[1] = {0.5, 0.5, true};
        c.nodes[2] = {1.0, 1.0, false};
        c.nodeCount = 3;
        c.clamp = true;
        check(std::abs(c.evaluate(8.0) - (-6.0)) < 1e-12, "G4 ModCurve smoothstep midpoint stays symmetric");

        // Validation guards
        ModulationRoute r = g4test::makeRoute(0, ModDriver::DistanceToListener, 0, 10, ModTarget::SourceGain, -12, 0);
        check(validModRoute(r), "G4 validModRoute accepts nominal route");
        auto bad = r; bad.curve.nodeCount = 1; check(!validModRoute(bad), "G4 validModRoute rejects < 2 nodes");
        bad = r; bad.curve.nodeCount = 9; check(!validModRoute(bad), "G4 validModRoute rejects > 8 nodes");
        bad = r; bad.curve.nodes[1].x = 0.2; bad.curve.nodes[2].x = 0.1; bad.curve.nodeCount = 3; check(!validModRoute(bad), "G4 rejects decreasing node x");
        bad = r; bad.object = kModListenerObject; bad.targetObject = -1; bad.target = ModTarget::SourceGain;
        check(!validModRoute(bad), "G4 rejects listener self target for sourceGain");
        bad = r; bad.target = ModTarget::Mix; bad.targetObject = 0;
        check(!validModRoute(bad), "G4 rejects explicit source targetObject for global mix");
        bad = r; bad.attackMs = 2001.0; check(!validModRoute(bad), "G4 rejects attackMs > 2000");

        // PathLfo evaluation & wrap/clamp
        PathLfo lfo;
        lfo.enabled = true; lfo.rateHz = 2.0; lfo.depth = 0.25; lfo.phaseDeg = 0.0; lfo.seed = 42;
        lfo.shape = LfoShape::Sine;
        check(std::abs(lfo.evaluate(0.125) - 1.0) < 1e-12 && std::abs(lfo.evaluate(0.375) - (-1.0)) < 1e-12,
              "G4 PathLfo sine hits +1 and -1 at quarter periods");
        lfo.shape = LfoShape::Triangle;
        check(std::abs(lfo.evaluate(0.125) - 1.0) < 1e-12 && std::abs(lfo.evaluate(0.25)) < 1e-12,
              "G4 PathLfo triangle shape exact");
        lfo.shape = LfoShape::Saw;
        check(std::abs(lfo.evaluate(0.0) - (-1.0)) < 1e-12 && lfo.evaluate(0.49) > 0.9,
              "G4 PathLfo saw wave spans [-1, 1]");
        lfo.shape = LfoShape::Square;
        check(lfo.evaluate(0.1) == 1.0 && lfo.evaluate(0.3) == -1.0,
              "G4 PathLfo square flips at half period");
        lfo.shape = LfoShape::SmoothRandom;
        check(lfo.evaluate(0.33) == lfo.evaluate(0.33) && std::abs(lfo.evaluate(0.33)) <= 1.0,
              "G4 PathLfo smoothRandom is deterministic from time + seed and bounded [-1, 1]");

        lfo.shape = LfoShape::Sine;
        check(std::abs(lfo.applyToU(0.1, 0.375, false) - 0.0) < 1e-12, "G4 open path clamps u - depth to 0");
        const double wrapped = lfo.applyToU(0.1, 0.375, true); // 0.1 - 0.25 = -0.15 -> 0.85
        check(std::abs(wrapped - 0.85) < 1e-12, "G4 closed path wraps negative u into [0, 1)");

        MovementPath mp;
        check(!validPathLfo(PathLfo{true, LfoShape::Sine, 8.5, 0.2, 0.0, 1}), "G4 validPathLfo rejects rateHz > 8 Hz");
        mp.lfo.enabled = true; mp.lfo.depth = 0.2;
        check(!validPath(mp), "G4 validPath rejects non-default LFO when points.empty()");
        mp.points = {{1, 1, 1.5}, {4, 1, 1.5}};
        check(validPath(mp), "G4 validPath accepts LFO on >= 2-point path");
    }

    // 2. Zero routes XML omission, bit-exact audio vs no routes, and legacy compatibility
    {
        auto procA = std::make_unique<VerbProcessor>();
        auto& pA = *procA;
        pA.setTailSettings({8000, false});
        pA.setPlayConfigDetails(2, 2, 48000, 128);
        pA.prepareToPlay(48000, 128);
        check(waitReady(pA, 90), "G4 fast proc A ready");
        pA.loadSceneText(g4test::kBoxScene, "G4Fast", true);
        check(waitReady(pA, 90), "G4 fast room loads");

        const juce::String xmlClean = p4test::stateXml(pA);
        check(!xmlClean.contains("<Modulation") && !xmlClean.contains("<Lfo"),
              "G4 XML omits <Modulation> and <Lfo> when empty/default");

        setP(pA, "mode", 1); // Creative
        auto procB = std::make_unique<VerbProcessor>();
        auto& pB = *procB;
        pB.setTailSettings({8000, false});
        pB.setPlayConfigDetails(2, 2, 48000, 128);
        pB.prepareToPlay(48000, 128);
        waitReady(pB, 90);
        p4test::loadStateXml(pB, p4test::stateXml(pA));
        check(waitReady(pB, 90), "G4 fast proc B ready from clean XML");

        auto renderClean = [](VerbProcessor& vp) {
            vp.prepareToPlay(48000, 128);
            setP(vp, "soloDirect", 1);
            return g4test::captureImpulseCh0(vp, 8, 2);
        };
        const auto irA = renderClean(pA);
        const auto irB = renderClean(pB);
        check(irA.size() == irB.size() && std::memcmp(irA.data(), irB.data(), irA.size() * sizeof(float)) == 0,
              "G4 zero routes & default LFO processBlock is bit-identical across reload");

        // Legacy 0.2.x .verbroom fixture loads cleanly
        const auto legacyFile = repoFile("plugin/fixtures/stage2/legacy.verbroom");
        if (legacyFile.existsAsFile()) {
            juce::String err;
            check(pB.loadRoomPresetText(legacyFile.loadFileAsString(), err) && waitReady(pB, 90),
                  "G4 stage2/legacy.verbroom loads cleanly");
            check(pB.getModulationRoutes().empty(), "G4 legacy.verbroom has 0 modulation routes");
        }
    }

    // 3. State & Keep I/O round-trip
    {
        auto proc = std::make_unique<VerbProcessor>();
        auto& p = *proc;
        p.setTailSettings({8000, false});
        p.setPlayConfigDetails(4, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "G4 state proc ready");
        p.loadSceneText(g4test::kBoxScene, "G4Persist", true);
        waitReady(p, 90);

        auto cfgs = p.getSources();
        cfgs.resize(2);
        cfgs[0].name = "LocalVoice"; cfgs[0].firstIn = 0;
        cfgs[1].name = "LocalKeys"; cfgs[1].firstIn = 1;
        p.setSources(cfgs);
        waitReady(p, 90);

        MovementPath mp;
        mp.points = {{2, 2, 1.5}, {6, 2, 1.5}};
        mp.closed = false;
        mp.lfo = {true, LfoShape::Triangle, 3.0, 0.25, 90.0, 77};
        check(p.setMovementPath(0, mp), "G4 LFO path set on source 0");

        auto r0 = g4test::makeRoute(0, ModDriver::DistanceToListener, 1.0, 8.0, ModTarget::SourceLowPass, 20000.0, 800.0, 20.0, 90.0);
        r0.curve.nodeCount = 3;
        r0.curve.nodes[0] = {0.0, 0.0, false};
        r0.curve.nodes[1] = {0.4, 0.7, true};
        r0.curve.nodes[2] = {1.0, 1.0, false};
        auto r1 = g4test::makeRoute(kModListenerObject, ModDriver::X, 0.0, 12.0, ModTarget::Mix, 0.0, -30.0, 5.0, 50.0);
        auto r2 = g4test::makeRoute(1, ModDriver::DistanceToPoint, 0.0, 5.0, ModTarget::SourceGain, -12.0, 3.0);
        r2.pointX = 4.0; r2.pointY = 5.0; r2.pointZ = 1.5;
        check(p.setModulationRoutes({r0, r1, r2}), "G4 setModulationRoutes accepted");

        const juce::String xmlSaved = p4test::stateXml(p);
        check(xmlSaved.contains("<Modulation") && xmlSaved.contains("<Lfo"), "G4 XML includes <Modulation> and <Lfo>");

        // Build preset containing global tail route and local source route
        p.setModulationRoutes({r0, g4test::makeRoute(kModListenerObject, ModDriver::X, 0.0, 12.0, ModTarget::TailLevel, -6.0, 0.0)});
        const juce::String presetXml = p.getRoomPresetText();

        // Reload saved state into fresh processor
        auto qProc = std::make_unique<VerbProcessor>();
        auto& q = *qProc;
        q.setTailSettings({8000, false});
        q.setPlayConfigDetails(4, 2, 48000, 128);
        q.prepareToPlay(48000, 128);
        waitReady(q, 90);
        check(p4test::loadStateXml(q, xmlSaved) && waitReady(q, 90), "G4 state XML with routes + LFO reloads");
        check(q.getMovementPath(0).lfo == mp.lfo, "G4 PathLfo round-trips identically through state XML");
        const auto qr = q.getModulationRoutes();
        check(qr.size() == 3 && qr[0] == r0 && qr[1] == r1 && qr[2] == r2,
              "G4 all ModulationRoutes (point, multi-node, smooth, global) round-trip identically");

        // Unknown child inside <Modulation> / <Path> ignored safely
        {
            auto parsed = juce::parseXML(xmlSaved);
            if (parsed) {
                if (auto* mt = parsed->getChildByName("Modulation")) {
                    auto* extra = new juce::XmlElement("FutureField");
                    extra->setAttribute("x", 1);
                    mt->addChildElement(extra);
                }
            }
            check(parsed && p4test::loadStateXml(q, parsed->toString()) && waitReady(q, 90) && q.getModulationRoutes().size() == 3,
                  "G4 unknown child element inside <Modulation> is ignored safely");
        }

        // Keep I/O test: q keeps its local r0/r2 source routes, drops its mix route, imports preset's tailLevel route
        q.setKeepRoutingEnabled(true);
        juce::String err;
        check(q.loadRoomPresetText(presetXml, err) && waitReady(q, 90), "G4 Keep I/O loads .verbroom preset cleanly");
        const auto keepR = q.getModulationRoutes();
        check(keepR.size() == 3 && keepR[0] == r0 && keepR[1] == r2 && keepR[2].target == ModTarget::TailLevel,
              "G4 Keep I/O retains local source routes and imports global preset route");
    }

    return fails == before ? 0 : 1;
}

static int testG4Modulation() {
    const int before = fails;
    using namespace verbp;
    if (testG4ModulationFast() != 0) {}

    std::printf("G4 full: audio spectrum (LPF/HPF/Shelves), dB curves, 11 drivers, zero-alloc, clicks, LFO+PlayHead+Bake, perf\n");

    // 4. Audio DSP verification: LPF spectrum vs curve at 3 distances, Gain <= 0.1 dB, additive + clamp, HPF/Shelves/Early/Tail/Mix
    {
        auto proc = std::make_unique<VerbProcessor>();
        auto& p = *proc;
        p.setTailSettings({8000, false});
        p.setPlayConfigDetails(4, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "G4 DSP proc ready");
        p.loadSceneText(g4test::kBoxScene, "G4DSP", true);
        check(waitReady(p, 90), "G4 DSP scene ready");

        setP(p, "mode", 1); // Creative
        setP(p, "spatialDry", 1);
        setP(p, "soloDirect", 1);
        setP(p, "lx", 2.0); setP(p, "ly", 5.0); setP(p, "lz", 1.5);
        g4test::primeCreativeAndSettle(p, 16);

        // 4A. distanceToListener -> sourceLowPass across 3 distances
        // Capture 16 blocks (2048 samples = 42.67 ms > 9m / 343.2m/s = 26.2ms direct delay) after 48 warmup blocks (128ms)
        auto lpfRoute = g4test::makeRoute(0, ModDriver::DistanceToListener, 1.0, 9.0, ModTarget::SourceLowPass, 12000.0, 1000.0, 0.0, 0.0);
        for (double dist : {1.0, 5.0, 9.0}) {
            const double expectedFc = 12000.0 + ((dist - 1.0) / 8.0) * (1000.0 - 12000.0); // 12000, 6500, 1000 Hz
            p.setModulationRoutes({});
            setP(p, "s1x", 2.0 + dist); setP(p, "s1y", 5.0); setP(p, "s1z", 1.5);
            g4test::drainTail(p, 48);
            const auto irBase = g4test::captureImpulseCh0(p, 16, 0);

            p.setModulationRoutes({lpfRoute});
            g4test::drainTail(p, 48);
            const auto irLpf = g4test::captureImpulseCh0(p, 16, 0);

            const double lowBase = g4test::goertzelMag(irBase, 48000.0, 200.0);
            const double lowLpf = g4test::goertzelMag(irLpf, 48000.0, 200.0);
            const double lfErrDb = 20.0 * std::log10((lowLpf + 1e-12) / (lowBase + 1e-12));

            const double testFreq = 5000.0;
            const double hfBase = g4test::goertzelMag(irBase, 48000.0, testFreq);
            const double hfLpf = g4test::goertzelMag(irLpf, 48000.0, testFreq);
            const double measDb = 20.0 * std::log10((hfLpf + 1e-12) / (hfBase + 1e-12));
            const double wantDb = g4test::rbjLpfMagDb(testFreq, expectedFc, 48000.0);
            std::printf("  [G4 LPF d=%.1fm fc=%.0fHz] LF=%.3fdB, 5kHz meas=%.2fdB vs rbj=%.2fdB\n",
                        dist, expectedFc, lfErrDb, measDb, wantDb);
            check(std::abs(lfErrDb) < 0.25 && std::abs(measDb - wantDb) < 0.4,
                  "G4 LPF transfer at 200Hz and 5kHz matches 2nd-order Butterworth/RBJ curve");
        }

        // 4B. sourceGain curve accuracy <= 0.1 dB + additive clamping + Physical mode bit-exact
        {
            setP(p, "s1x", 3.5); setP(p, "s1y", 5.0); setP(p, "s1z", 1.5);
            setP(p, "s1g", 0.0);
            p.setModulationRoutes({});
            g4test::drainTail(p, 48);
            const auto irFlat = g4test::captureImpulseCh0(p, 8, 0);
            const double basePk = g4test::peakAbs(irFlat);

            // Distance = 1.5m: route maps [0, 3] -> [0, -24] dB => -12.0 dB
            auto gRoute = g4test::makeRoute(0, ModDriver::DistanceToListener, 0.0, 3.0, ModTarget::SourceGain, 0.0, -24.0);
            p.setModulationRoutes({gRoute});
            g4test::drainTail(p, 48);
            const double measDb = 20.0 * std::log10((g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0)) + 1e-12) / (basePk + 1e-12));
            std::printf("  [G4 Gain -12dB curve] measured=%.4f dB\n", measDb);
            check(std::abs(measDb - (-12.0)) <= 0.1, "G4 sourceGain route matches curve within 0.1 dB");

            // Two additive gain routes: -12 dB + -6 dB = -18 dB
            auto gRoute2 = g4test::makeRoute(0, ModDriver::Z, 0.0, 3.0, ModTarget::SourceGain, 0.0, -12.0); // z=1.5 -> -6 dB
            p.setModulationRoutes({gRoute, gRoute2});
            g4test::drainTail(p, 48);
            const double addDb = 20.0 * std::log10((g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0)) + 1e-12) / (basePk + 1e-12));
            std::printf("  [G4 Additive Gain -12+-6] measured=%.4f dB\n", addDb);
            check(std::abs(addDb - (-18.0)) <= 0.1, "G4 multiple sourceGain routes sum in dB accurately");

            // Additive LPF in octaves: two routes each targeting 10000 Hz (-1 oct from 20k) -> sum = -2 oct = 5000 Hz
            auto lpfOct = g4test::makeRoute(0, ModDriver::X, 0.0, 10.0, ModTarget::SourceLowPass, 10000.0, 10000.0);
            p.setModulationRoutes({lpfOct, lpfOct});
            g4test::drainTail(p, 48);
            const auto irAddLpf = g4test::captureImpulseCh0(p, 8, 0);
            const double db5k = 20.0 * std::log10((g4test::goertzelMag(irAddLpf, 48000.0, 5000.0) + 1e-12) / (g4test::goertzelMag(irFlat, 48000.0, 5000.0) + 1e-12));
            std::printf("  [G4 Additive LPF 10k+10k oct->5k] 5kHz=%.2f dB (expect -3.01 dB)\n", db5k);
            check(std::abs(db5k - (-3.01)) < 0.5, "G4 two LPF routes combine additively in log2 octaves");

            // Physical mode must be bit-identical (memcmp == 0) to zero routes even with routes & LFO present!
            auto mkPhys = [&](bool withRoutes, bool withPath, bool withLfo) {
                auto pp = std::make_unique<VerbProcessor>();
                pp->setTailSettings({8000, false});
                pp->setPlayConfigDetails(2, 2, 48000, 128);
                pp->prepareToPlay(48000, 128);
                waitReady(*pp, 90);
                pp->loadSceneText(g4test::kBoxScene, "G4PhysBit", true);
                waitReady(*pp, 90);
                setP(*pp, "mode", 0);
                setP(*pp, "soloDirect", 1);
                if (withPath) {
                    MovementPath lp; lp.points = {{3.5, 5.0, 1.5}, {6.0, 5.0, 1.5}};
                    if (withLfo) lp.lfo = {true, LfoShape::Sine, 4.0, 0.5, 0.0, 1};
                    pp->setMovementPath(0, lp);
                }
                if (withRoutes) pp->setModulationRoutes({gRoute, lpfRoute});
                pp->prepareToPlay(48000, 128);
                return g4test::captureImpulseCh0(*pp, 6, 0);
            };
            const auto physCleanA = mkPhys(false, false, false);
            const auto physRoutesOnly = mkPhys(true, false, false);
            const auto physPathNoLfo = mkPhys(false, true, false);
            const auto physWithRoutes = mkPhys(true, true, true);
            check(std::memcmp(physCleanA.data(), physRoutesOnly.data(), physCleanA.size() * sizeof(float)) == 0
                  && std::memcmp(physPathNoLfo.data(), physWithRoutes.data(), physPathNoLfo.size() * sizeof(float)) == 0,
                  "G4 Physical mode with routes + LFO renders bit-identically (memcmp==0) to no routes");
            setP(p, "mode", 1); // back to Creative
        }

        // 4C. HPF, LowShelf, HighShelf & Sum/Free-points stereo source
        {
            setP(p, "mode", 1);
            setP(p, "soloDirect", 1);
            setP(p, "s1x", 3.5); setP(p, "s1y", 5.0); setP(p, "s1z", 1.5);
            p.setModulationRoutes({});
            p.prepareToPlay(48000, 128);
            g4test::drainTail(p, 48);
            const auto irBase = g4test::captureImpulseCh0(p, 12, 0);

            auto hpf = g4test::makeRoute(0, ModDriver::X, 0.0, 10.0, ModTarget::SourceHighPass, 2000.0, 2000.0);
            p.setModulationRoutes({hpf});
            g4test::drainTail(p, 48);
            const auto irHpf = g4test::captureImpulseCh0(p, 12, 0);
            const double hpf500 = 20.0 * std::log10((g4test::goertzelMag(irHpf, 48000.0, 500.0) + 1e-12) / (g4test::goertzelMag(irBase, 48000.0, 500.0) + 1e-12));
            const double hpf8k = 20.0 * std::log10((g4test::goertzelMag(irHpf, 48000.0, 8000.0) + 1e-12) / (g4test::goertzelMag(irBase, 48000.0, 8000.0) + 1e-12));
            check(hpf500 < -20.0 && std::abs(hpf8k) < 0.3, "G4 sourceHighPass @ 2kHz cuts 500Hz deeply and passes 8kHz");

            auto ls = g4test::makeRoute(0, ModDriver::X, 0.0, 10.0, ModTarget::SourceLowShelf, -9.0, -9.0);
            p.setModulationRoutes({ls});
            g4test::drainTail(p, 48);
            const auto irLs = g4test::captureImpulseCh0(p, 12, 0);
            const double ls60 = 20.0 * std::log10((g4test::goertzelMag(irLs, 48000.0, 60.0) + 1e-12) / (g4test::goertzelMag(irBase, 48000.0, 60.0) + 1e-12));
            const double ls8k = 20.0 * std::log10((g4test::goertzelMag(irLs, 48000.0, 8000.0) + 1e-12) / (g4test::goertzelMag(irBase, 48000.0, 8000.0) + 1e-12));
            check(std::abs(ls60 - (-9.0)) < 0.8 && std::abs(ls8k) < 0.3, "G4 sourceLowShelf -9dB lowers 60Hz cleanly without affecting 8kHz");

            auto hs = g4test::makeRoute(0, ModDriver::X, 0.0, 10.0, ModTarget::SourceHighShelf, -9.0, -9.0);
            p.setModulationRoutes({hs});
            g4test::drainTail(p, 48);
            const auto irHs = g4test::captureImpulseCh0(p, 12, 0);
            const double hs100 = 20.0 * std::log10((g4test::goertzelMag(irHs, 48000.0, 100.0) + 1e-12) / (g4test::goertzelMag(irBase, 48000.0, 100.0) + 1e-12));
            const double hs12k = 20.0 * std::log10((g4test::goertzelMag(irHs, 48000.0, 12000.0) + 1e-12) / (g4test::goertzelMag(irBase, 48000.0, 12000.0) + 1e-12));
            check(std::abs(hs100) < 0.3 && std::abs(hs12k - (-9.0)) < 1.0, "G4 sourceHighShelf -9dB lowers 12kHz cleanly without affecting 100Hz");

            // Global Early / Tail / Mix targets (drain tail fully between impulses!)
            setP(p, "soloDirect", 0); setP(p, "soloEarly", 1); setP(p, "early", 0.0);
            p.setModulationRoutes({});
            p.prepareToPlay(48000, 128);
            g4test::drainTail(p, 120);
            const double earlyBaseRms = g4test::rmsEnergy(g4test::captureImpulseCh0(p, 32, 0));
            p.setModulationRoutes({g4test::makeRoute(kModListenerObject, ModDriver::X, 0, 10, ModTarget::EarlyLevel, -12.0, -12.0)});
            p.prepareToPlay(48000, 128);
            g4test::drainTail(p, 120);
            const double earlyModRms = g4test::rmsEnergy(g4test::captureImpulseCh0(p, 32, 0));
            const double earlyDiffDb = 20.0 * std::log10((earlyModRms + 1e-12) / (earlyBaseRms + 1e-12));
            std::printf("  [G4 EarlyLevel -12dB] measured=%.2f dB\n", earlyDiffDb);
            check(std::abs(earlyDiffDb - (-12.0)) < 0.6, "G4 earlyLevel live modulation scales early reflections");

            setP(p, "soloEarly", 0); setP(p, "soloTail", 1); setP(p, "tail", 0.0);
            p.setModulationRoutes({});
            p.prepareToPlay(48000, 128);
            g4test::drainTail(p, 420);
            const double tailBaseRms = g4test::rmsEnergy(g4test::captureImpulseCh0(p, 64, 0));
            p.setModulationRoutes({g4test::makeRoute(kModListenerObject, ModDriver::X, 0, 10, ModTarget::TailLevel, -12.0, -12.0)});
            p.prepareToPlay(48000, 128);
            g4test::drainTail(p, 420);
            const double tailModRms = g4test::rmsEnergy(g4test::captureImpulseCh0(p, 64, 0));
            const double tailDiffDb = 20.0 * std::log10((tailModRms + 1e-12) / (tailBaseRms + 1e-12));
            std::printf("  [G4 TailLevel -12dB] measured=%.2f dB\n", tailDiffDb);
            check(std::abs(tailDiffDb - (-12.0)) < 1.0, "G4 tailLevel live modulation scales late tail");

            setP(p, "soloTail", 0); setP(p, "spatialDry", 0); setP(p, "mix", 100.0);
            p.setModulationRoutes({});
            p.prepareToPlay(48000, 128);
            g4test::drainTail(p, 420);
            const float dry0 = g4test::captureImpulseCh0(p, 1, 0)[0];
            p.setModulationRoutes({g4test::makeRoute(kModListenerObject, ModDriver::X, 0, 10, ModTarget::Mix, -100.0, -100.0)});
            p.prepareToPlay(48000, 128);
            g4test::drainTail(p, 16);
            const float dryMod = g4test::captureImpulseCh0(p, 1, 0)[0];
            check(std::abs(dry0) < 1e-4f && std::abs(dryMod - 1.0f) < 0.02f, "G4 mix modulation -100% passes pure unspatialized dry");
            setP(p, "spatialDry", 1); setP(p, "soloDirect", 1);

            // Stereo source Sum and Free-points
            auto cfgs = p.getSources();
            cfgs[0].format = 1; // Stereo
            cfgs[0].sum = true;
            p.setSources(cfgs); waitReady(p, 90);
            p.setModulationRoutes({g4test::makeRoute(0, ModDriver::X, 0, 10, ModTarget::SourceGain, -12.0, -12.0)});
            p.prepareToPlay(48000, 128);
            g4test::drainTail(p, 32);
            const double pkSum = g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0));
            cfgs[0].sum = false; cfgs[0].freePoints = true;
            cfgs[0].ox = {0.5f, -0.5f}; cfgs[0].oy = {0.f, 0.f}; cfgs[0].oz = {0.f, 0.f};
            p.setSources(cfgs); waitReady(p, 90);
            p.prepareToPlay(48000, 128);
            g4test::drainTail(p, 32);
            const double pkFree = g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0));
            check(pkSum > 1e-4 && pkFree > 1e-4, "G4 modulation operates cleanly on both Sum and Free-points stereo sources");
            cfgs[0].format = 0; cfgs[0].sum = false; cfgs[0].freePoints = false;
            p.setSources(cfgs); waitReady(p, 90);
        }

        // 4D. Remaining Phase 1 drivers (distanceToPoint, azimuthToListener, elevation, y, speed, radialSpeed, pathPosition, distanceToNearestWall)
        {
            setP(p, "mode", 1);
            setP(p, "soloDirect", 1);
            setP(p, "lx", 4.0); setP(p, "ly", 5.0); setP(p, "lz", 1.5); setP(p, "laz", 0.0);
            setP(p, "s1x", 6.0); setP(p, "s1y", 5.0); setP(p, "s1z", 1.5); setP(p, "s1g", 0.0);
            p.setModulationRoutes({});
            g4test::drainTail(p, 64);
            const double basePk = g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0));

            auto checkGainRoute = [&](verbp::ModulationRoute r, double expectDb, const char* msg) {
                p.setModulationRoutes({r});
                g4test::drainTail(p, 48);
                const double db = 20.0 * std::log10((g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0)) + 1e-12) / (basePk + 1e-12));
                check(std::abs(db - expectDb) <= 0.2, msg);
            };

            auto rPt = g4test::makeRoute(0, ModDriver::DistanceToPoint, 0.0, 4.0, ModTarget::SourceGain, 0.0, -24.0);
            rPt.pointX = 6.0; rPt.pointY = 3.0; rPt.pointZ = 1.5; // dist = 2.0m -> -12 dB
            checkGainRoute(rPt, -12.0, "G4 driver distanceToPoint exact");

            // s1 is at (6, 5) relative to listener (4, 5, laz=0) -> azimuthToListener = 0 deg
            checkGainRoute(g4test::makeRoute(0, ModDriver::AzimuthToListener, -90.0, 90.0, ModTarget::SourceGain, -24.0, 0.0), -12.0,
                           "G4 driver azimuthToListener 0 deg in front of listener");
            setP(p, "laz", 90.0); // listener faces +Y -> source at +X is -90 deg (right) -> -24 dB relative to laz=90 baseline
            p.setModulationRoutes({});
            g4test::drainTail(p, 48);
            const double az90Base = g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0));
            p.setModulationRoutes({g4test::makeRoute(0, ModDriver::AzimuthToListener, -90.0, 90.0, ModTarget::SourceGain, -24.0, 0.0)});
            g4test::drainTail(p, 48);
            const double az90Db = 20.0 * std::log10((g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0)) + 1e-12) / (az90Base + 1e-12));
            check(std::abs(az90Db - (-24.0)) <= 0.2, "G4 driver azimuthToListener responds to listener yaw rotation");
            setP(p, "laz", 0.0);

            setP(p, "s1z", 3.5); // dz=2, dxy=2 -> elev = +45 deg
            p.setModulationRoutes({});
            g4test::drainTail(p, 48);
            const double elevBase = g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0));
            p.setModulationRoutes({g4test::makeRoute(0, ModDriver::Elevation, 0.0, 90.0, ModTarget::SourceGain, 0.0, -24.0)});
            g4test::drainTail(p, 48);
            const double elevDb = 20.0 * std::log10((g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0)) + 1e-12) / (elevBase + 1e-12));
            check(std::abs(elevDb - (-12.0)) <= 0.2, "G4 driver elevation 45 deg exact");
            setP(p, "s1z", 1.5);
            g4test::drainTail(p, 48);

            checkGainRoute(g4test::makeRoute(0, ModDriver::Y, 0.0, 10.0, ModTarget::SourceGain, 0.0, -24.0), -12.0,
                           "G4 driver y coordinate exact");

            // distanceToNearestWall: room y=0..10, x=0..12. Move to (6, 1) -> dist = 1.0m
            setP(p, "s1x", 6.0); setP(p, "s1y", 1.0);
            p.setModulationRoutes({});
            g4test::drainTail(p, 48);
            const double wallBase = g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0));
            p.setModulationRoutes({g4test::makeRoute(0, ModDriver::DistanceToNearestWall, 0.0, 4.0, ModTarget::SourceGain, 0.0, -24.0)});
            g4test::drainTail(p, 48);
            const double wallDb = 20.0 * std::log10((g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0)) + 1e-12) / (wallBase + 1e-12));
            check(std::abs(wallDb - (-6.0)) <= 0.2, "G4 driver distanceToNearestWall 1.0m exact");

            // pathPosition
            MovementPath mp; mp.points = {{5.0, 5.0, 1.5}, {9.0, 5.0, 1.5}};
            p.setMovementPath(0, mp);
            setP(p, "s1path", 0.5);
            p.setModulationRoutes({});
            g4test::drainTail(p, 48);
            const double pathBase = g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0));
            p.setModulationRoutes({g4test::makeRoute(0, ModDriver::PathPosition, 0.0, 1.0, ModTarget::SourceGain, 0.0, -24.0)});
            g4test::drainTail(p, 48);
            const double pathDb = 20.0 * std::log10((g4test::peakAbs(g4test::captureImpulseCh0(p, 8, 0)) + 1e-12) / (pathBase + 1e-12));
            check(std::abs(pathDb - (-12.0)) <= 0.2, "G4 driver pathPosition u=0.5 exact");
            p.setMovementPath(0, {});

            // speed & radialSpeed while moving source away (+x) from (5,5) vs approaching from (8,5)
            // Note: direct sound arrives after ~14 ms (~5 blocks), so send continuous unit tone on sample 0 of each block or keep impulse ringing while measuring block energy!
            setP(p, "lx", 2.0); setP(p, "ly", 5.0);
            setP(p, "s1x", 5.0); setP(p, "s1y", 5.0);
            auto rSpd = g4test::makeRoute(0, ModDriver::Speed, 0.0, 40.0, ModTarget::SourceGain, -24.0, 0.0);
            p.setModulationRoutes({rSpd});
            p.prepareToPlay(48000, 128);
            g4test::drainTail(p, 32);
            juce::AudioBuffer<float> buf(4, 128); juce::MidiBuffer midi;
            float pkStill = 0.f;
            for (int b = 0; b < 12; ++b) {
                buf.clear();
                for (int i = 0; i < 128; ++i) buf.setSample(0, i, 0.5f * (float)std::sin(2.0 * verb::kPi * 500.0 * (b * 128 + i) / 48000.0));
                stage2Process(p, buf, midi);
                if (b >= 8) pkStill = std::max(pkStill, buf.getMagnitude(0, 0, 128));
            }
            float pkMoving = 0.f;
            for (int step = 1; step <= 20; ++step) {
                setP(p, "s1x", 5.0 + 0.1 * step); // ~37.5 m/s
                buf.clear();
                for (int i = 0; i < 128; ++i) buf.setSample(0, i, 0.5f * (float)std::sin(2.0 * verb::kPi * 500.0 * (step * 128 + i) / 48000.0));
                stage2Process(p, buf, midi);
                if (step >= 12) pkMoving = std::max(pkMoving, buf.getMagnitude(0, 0, 128));
            }
            check(pkMoving > pkStill * 1.5f, "G4 driver speed boosts gain while source moves vs stationary");

            setP(p, "s1x", 5.0);
            auto rRad = g4test::makeRoute(0, ModDriver::RadialSpeed, -40.0, 40.0, ModTarget::SourceGain, -24.0, 0.0);
            p.setModulationRoutes({rRad});
            p.prepareToPlay(48000, 128);
            g4test::drainTail(p, 32);
            float pkRecede = 0.f;
            for (int step = 1; step <= 20; ++step) {
                setP(p, "s1x", 5.0 + 0.1 * step); // receding (+radialSpeed -> ~0 dB)
                buf.clear();
                for (int i = 0; i < 128; ++i) buf.setSample(0, i, 0.5f * (float)std::sin(2.0 * verb::kPi * 500.0 * (step * 128 + i) / 48000.0));
                stage2Process(p, buf, midi);
                if (step >= 14) pkRecede = std::max(pkRecede, buf.getMagnitude(0, 0, 128));
            }
            setP(p, "s1x", 8.0); p.prepareToPlay(48000, 128); g4test::drainTail(p, 32);
            float pkApproach = 0.f;
            for (int step = 1; step <= 20; ++step) {
                setP(p, "s1x", 8.0 - 0.1 * step); // approaching (-radialSpeed -> ~-24 dB)
                buf.clear();
                for (int i = 0; i < 128; ++i) buf.setSample(0, i, 0.5f * (float)std::sin(2.0 * verb::kPi * 500.0 * (step * 128 + i) / 48000.0));
                stage2Process(p, buf, midi);
                if (step >= 14) pkApproach = std::max(pkApproach, buf.getMagnitude(0, 0, 128));
            }
            check(pkRecede > pkApproach * 2.0f, "G4 driver radialSpeed distinguishes receding vs approaching");
        }
    }

    // 5. Determinism (two offline renders bit-identical), zero audio-thread allocations, click-free fast motion & Physical<->Creative transitions
    {
        auto renderRun = [](bool measureAllocsAndClicks) {
            auto proc = std::make_unique<VerbProcessor>();
            auto& p = *proc;
            p.setTailSettings({8000, false});
            p.setPlayConfigDetails(2, 2, 48000, 128);
            p.prepareToPlay(48000, 128);
            waitReady(p, 90);
            p.loadSceneText(g4test::kBoxScene, "G4Det", true);
            waitReady(p, 90);
            setP(p, "mode", 1);
            setP(p, "soloDirect", 1);

            MovementPath mp;
            mp.points = {{3.0, 3.0, 1.5}, {9.0, 7.0, 1.5}};
            mp.lfo = {true, LfoShape::SmoothRandom, 5.0, 0.35, 30.0, 19};
            p.setMovementPath(0, mp);
            auto r1 = g4test::makeRoute(0, ModDriver::DistanceToListener, 1.0, 10.0, ModTarget::SourceLowPass, 18000.0, 800.0, 15.0, 60.0);
            auto r2 = g4test::makeRoute(0, ModDriver::PathPosition, 0.0, 1.0, ModTarget::SourceGain, 0.0, -12.0, 10.0, 40.0);
            p.setModulationRoutes({r1, r2});
            p.prepareToPlay(48000, 128);

            juce::AudioBuffer<float> buf(2, 128);
            juce::MidiBuffer midi;
            std::vector<float> all;
            all.reserve(60 * 128);
            if (measureAllocsAndClicks) stage2AudioAllocations = 0;
            size_t sampleIdx = 0;
            float maxDelta = 0.f, prevOut = 0.f;
            for (int b = 0; b < 60; ++b) {
                if (measureAllocsAndClicks && b == 20) setP(p, "s1path", 0.9); // rapid driver jump
                if (measureAllocsAndClicks && b == 35) setP(p, "mode", 0);     // Creative -> Physical live switch
                if (measureAllocsAndClicks && b == 50) setP(p, "mode", 1);     // Physical -> Creative live switch
                buf.clear();
                for (int i = 0; i < 128; ++i) {
                    const float s = 0.25f * (float)std::sin(2.0 * verb::kPi * 440.0 * (double)sampleIdx++ / 48000.0);
                    buf.setSample(0, i, s);
                }
                stage2Process(p, buf, midi);
                for (int i = 0; i < 128; ++i) {
                    const float y = buf.getSample(0, i);
                    if (b > 2) maxDelta = std::max(maxDelta, std::abs(y - prevOut));
                    prevOut = y;
                    all.push_back(y);
                }
            }
            if (measureAllocsAndClicks) {
                std::printf("  [G4 Audio Safety] allocations=%zu maxSampleDelta=%.4f\n", stage2AudioAllocations, maxDelta);
                check(stage2AudioAllocations == 0, "G4 active routes + filters + LFO allocate 0 C++ bytes in processBlock");
                check(maxDelta < 0.25f, "G4 rapid driver jump & Physical<->Creative switches stay clickless");
            }
            return all;
        };
        const auto run1 = renderRun(false);
        const auto run2 = renderRun(false);
        check(run1.size() == run2.size() && std::memcmp(run1.data(), run2.data(), run1.size() * sizeof(float)) == 0,
              "G4 two offline renders with routes + LFO + smoothRandom are bit-identical");
        (void)renderRun(true);
    }

    // 6. Path LFO M2 + AudioPlayHead + Bake compatibility
    {
        auto proc = std::make_unique<VerbProcessor>();
        auto& p = *proc;
        p.setTailSettings({8000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        waitReady(p, 90);
        p.loadSceneText(p4test::kScene, "G4BakeLfo", true);
        waitReady(p, 90);

        MovementPath mp;
        mp.points = {{1.0, 2.0, 1.5}, {3.0, 6.0, 1.5}};
        mp.orient = true; // AlongPath
        p.setMovementPath(0, mp);
        check(p.startPathBake(0) && p4test::waitBaked(p, 90), "G4 baseline path bakes cleanly");
        auto eng = E9bAccess::engine(p);
        const uint64_t bakedHash = eng->bakeStats().hash;

        // Add LFO -> MUST NOT invalidate Bake!
        mp.lfo = {true, LfoShape::Sine, 4.0, 0.3, 0.0, 1};
        check(p.setMovementPath(0, mp), "G4 add LFO to baked path");
        p4test::pump(p, 4);
        const auto stAfter = eng->bakeStats();
        check(stAfter.state == verb::Engine::BakeState::Baked && stAfter.hash == bakedHash,
              "G4 enabling Path LFO keeps baked table valid (!Outdated, identical hash)");

        // Run active LFO with the already-baked table -> lookups increment cleanly with 0 discontinuities
        setP(p, "mode", 1); // Creative
        setP(p, "s1path", 0.5);
        g4test::primeCreativeAndSettle(p, 16);
        const auto lookupsBefore = eng->bakeStats().lookups;
        g4test::PlayHeadMock phBake; phBake.playing = true;
        p.setPlayHead(&phBake);
        juce::AudioBuffer<float> buf(2, 128); juce::MidiBuffer midi;
        for (int b = 0; b < 24; ++b) {
            phBake.samples = b * 128;
            buf.clear(); stage2Process(p, buf, midi);
        }
        p.setPlayHead(nullptr);
        const auto stRun = eng->bakeStats();
        std::printf("  [G4 LFO+Bake] lookups %llu -> %llu, disc=%d, state=%d\n",
                    (unsigned long long)lookupsBefore, (unsigned long long)stRun.lookups, stRun.discontinuities, (int)stRun.state);
        check(stRun.state == verb::Engine::BakeState::Baked && stRun.lookups > lookupsBefore,
              "G4 Bake table applies continuously during Path LFO oscillation without invalidation");

        // PlayHead test with AzKeys: paused stays at phase 0 (uBase = 0.5 -> AzKey readout), running sweeps at 4 Hz
        mp.orient = false;
        mp.azKeys = {{0.0, -90.0, AzimuthEasing::Linear}, {100.0, 90.0, AzimuthEasing::Linear}};
        mp.lfo = {true, LfoShape::Sine, 4.0, 0.25, 0.0, 1};
        p.setMovementPath(0, mp);
        setP(p, "mode", 1);
        setP(p, "s1path", 0.5);
        g4test::primeCreativeAndSettle(p, 16);

        g4test::PlayHeadMock ph;
        p.setPlayHead(&ph);
        ph.playing = false; ph.samples = 9600;
        buf.clear(); stage2Process(p, buf, midi);
        const float azStopped = p.liveAzimuth(0); // u = 0.5 -> az = 0.0

        // Seek to peak of 4 Hz sine (quarter period = 0.0625 s = 3000 samples): u = 0.5 + 0.25 = 0.75 -> az = +45 deg
        ph.playing = true; ph.samples = 3000;
        buf.clear(); stage2Process(p, buf, midi);
        const float azPeak = p.liveAzimuth(0);

        // Seek to trough of 4 Hz sine (3/4 period = 0.1875 s = 9000 samples): u = 0.5 - 0.25 = 0.25 -> az = -45 deg
        ph.samples = 9000;
        buf.clear(); stage2Process(p, buf, midi);
        const float azTrough = p.liveAzimuth(0);
        p.setPlayHead(nullptr);

        std::printf("  [G4 LFO PlayHead] stopped=%.2f peak=%.2f trough=%.2f (expect 0 / +45 / -45)\n",
                    azStopped, azPeak, azTrough);
        check(std::abs(azStopped) < 0.5f && std::abs(azPeak - 45.0f) < 1.0f && std::abs(azTrough - (-45.0f)) < 1.0f,
              "G4 PathLfo stands at phase 0 when stopped and tracks exact frequency & depth from PlayHead");
    }

    // 7. Performance benchmark: 32 sources x 4 routes (with filters) vs 0 routes
    {
        auto benchProc = std::make_unique<VerbProcessor>();
        auto& p = *benchProc;
        p.setTailSettings({8000, false});
        p.setPlayConfigDetails(32, 2, 48000, 512);
        p.prepareToPlay(48000, 512);
        waitReady(p, 90);
        p.loadSceneText(g4test::kBoxScene, "G4Bench", true);
        waitReady(p, 90);
        std::vector<SourceCfg> srcs(32);
        for (int i = 0; i < 32; ++i) {
            srcs[(size_t)i].name = "S" + std::to_string(i + 1);
            srcs[(size_t)i].firstIn = i;
            srcs[(size_t)i].format = 0;
        }
        p.setSources(srcs);
        check(waitReady(p, 90), "G4 32-source benchmark instance ready");
        setP(p, "mode", 1);
        g4test::primeCreativeAndSettle(p, 8);

        auto measureAvgMs = [&](int blocks) {
            juce::AudioBuffer<float> buf(32, 512);
            juce::MidiBuffer midi;
            for (int w = 0; w < 4; ++w) { buf.clear(); stage2Process(p, buf, midi); }
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < blocks; ++b) {
                buf.clear();
                for (int c = 0; c < 32; ++c) buf.setSample(c, 0, 0.1f);
                stage2Process(p, buf, midi);
            }
            const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return totalMs / (double)blocks;
        };

        const double msNoRoutes = measureAvgMs(32);
        std::vector<ModulationRoute> routes64;
        // 64 routes across first 16 sources x 4 routes each (LPF + HPF + LowShelf + Gain) -> exercises max cap + filters on 16 sources (or 32 sources * 2)
        // Wait: brief says "32 sources x 4 routes + filters vs without": note route cap is 64 total, so test 64 active routes across 32 sources (e.g. LPF + Gain across 32 sources) AND verify within cap!
        // Let's also benchmark direct ModulationProcessor 32 sources * 4 routes (128 internal test vs 64 cap) or 64 routes across 32 sources (LPF+HShelf alternating across all 32 sources):
        for (int i = 0; i < 32; ++i) {
            routes64.push_back(g4test::makeRoute(i, ModDriver::DistanceToListener, 0, 12, ModTarget::SourceLowPass, 20000, 1200));
            routes64.push_back(g4test::makeRoute(i, ModDriver::DistanceToNearestWall, 0, 10, ModTarget::SourceGain, 0, -12));
        }
        p.setModulationRoutes(routes64);
        const double msWith64Routes = measureAvgMs(32);

        // Direct DSP cost of 32 sources x 64 active routes + 4 active biquads on all 32 sources (4 segments of 128 @ 48 kHz)
        double dsp32x4Ms = 0.0;
        {
            ModulationProcessor mp;
            mp.reset();
            ModRouteSet mrs;
            mrs.generation = 1;
            mrs.count = 64;
            for (int i = 0; i < 32; ++i) {
                mrs.routes[(size_t)(2 * i)] = g4test::makeRoute(i, ModDriver::DistanceToListener, 0, 12, ModTarget::SourceLowPass, 20000, 1200);
                mrs.routes[(size_t)(2 * i + 1)] = g4test::makeRoute(i, ModDriver::DistanceToNearestWall, 0, 10, ModTarget::SourceGain, 0, -12);
            }
            double poses[kModMaxSources + 1][4] = {};
            for (int s = 0; s < 32; ++s) { poses[s][0] = 3.5 + 0.1 * s; poses[s][1] = 4.0; poses[s][2] = 1.5; }
            poses[kModListenerObject][0] = 2.0; poses[kModListenerObject][1] = 5.0; poses[kModListenerObject][2] = 1.5;
            double uArr[kModMaxSources + 1] = {};
            bool jumped[kModMaxSources + 1] = {};
            WallSnapshot ws;
            ws.count = 4;
            ws.segs[0] = {0, 0, 12, 0, 0.0, 4.0};
            ws.segs[1] = {12, 0, 12, 10, 0.0, 4.0};
            ws.segs[2] = {12, 10, 0, 10, 0.0, 4.0};
            ws.segs[3] = {0, 10, 0, 0, 0.0, 4.0};
            std::vector<std::array<float, 128>> ptBufs(32);
            for (auto& pt : ptBufs) pt.fill(0.05f);
            std::array<SourceModState, kModMaxSources> sm{};
            for (int s = 0; s < 32; ++s) {
                sm[(size_t)s].hasLpf = true; sm[(size_t)s].lpfHz = 2500.0;
                sm[(size_t)s].hasHpf = true; sm[(size_t)s].hpfHz = 120.0;
                sm[(size_t)s].hasLowShelf = true; sm[(size_t)s].lowShelfDb = -4.0;
                sm[(size_t)s].hasHighShelf = true; sm[(size_t)s].highShelfDb = 3.0;
            }
            GlobalModState gm{};
            for (int w = 0; w < 4; ++w)
                for (int seg = 0; seg < 4; ++seg) {
                    std::array<SourceModState, kModMaxSources> tmp{};
                    mp.evaluateSegment(mrs, 32, poses, uArr, jumped, ws, true, 1.0, 128, 48000.0, tmp, gm);
                    for (int s = 0; s < 32; ++s) mp.processPointFilters(s, ptBufs[(size_t)s].data(), 128, 48000.0, sm[(size_t)s], true, false);
                }
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < 64; ++b) {
                for (int seg = 0; seg < 4; ++seg) {
                    std::array<SourceModState, kModMaxSources> tmp{};
                    mp.evaluateSegment(mrs, 32, poses, uArr, jumped, ws, true, 1.0, 128, 48000.0, tmp, gm);
                    for (int s = 0; s < 32; ++s) mp.processPointFilters(s, ptBufs[(size_t)s].data(), 128, 48000.0, sm[(size_t)s], true, false);
                }
            }
            dsp32x4Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 64.0;
        }
        std::printf("  [G4 PERF 512@48kHz budget=10.67ms] 32src 0-routes=%.3f ms/blk; 64-routes+LPF/Gain=%.3f ms/blk (delta=%.3f ms/blk); ModDSP(32src*4biquads)=%.3f ms/blk\n",
                    msNoRoutes, msWith64Routes, msWith64Routes - msNoRoutes, dsp32x4Ms);
        check(msWith64Routes < 10.67 && dsp32x4Ms < 1.0, "G4 32-source 64-route+filter benchmark fits within 512-sample @ 48kHz real-time budget");
    }

    // 8. G5 lock-free live driver/target telemetry (ModulationProcessor + VerbProcessor)
    {
        ModulationProcessor mp;
        mp.reset();
        ModRouteSet mrs;
        mrs.generation = 1;
        mrs.count = 3;
        mrs.routes[0] = g4test::makeRoute(0, ModDriver::DistanceToListener, 0.0, 10.0, ModTarget::SourceLowPass, 20000.0, 2000.0, 0.0, 0.0);
        mrs.routes[1] = g4test::makeRoute(0, ModDriver::X, 0.0, 10.0, ModTarget::SourceGain, 0.0, -10.0, 0.0, 0.0);
        mrs.routes[2] = g4test::makeRoute(0, ModDriver::DistanceToPoint, 0.0, 10.0, ModTarget::Mix, 0.0, 50.0, 0.0, 0.0);
        mrs.routes[2].enabled = false;

        double poses[kModMaxSources + 1][4] = {};
        poses[0][0] = 7.0; poses[0][1] = 5.0; poses[0][2] = 1.5; // x=7, dist to listener (2,5,1.5) = 5.0
        poses[kModListenerObject][0] = 2.0; poses[kModListenerObject][1] = 5.0; poses[kModListenerObject][2] = 1.5;
        double uArr[kModMaxSources + 1] = {};
        bool jumped[kModMaxSources + 1] = {};
        WallSnapshot ws;
        std::array<SourceModState, kModMaxSources> sm{};
        GlobalModState gm{};
        float rawOut[kMaxModRoutes] = {};
        float valOut[kMaxModRoutes] = {};
        uint8_t actOut[kMaxModRoutes] = {};

        mp.evaluateSegment(mrs, 1, poses, uArr, jumped, ws, true, 1.0, 128, 48000.0, sm, gm, rawOut, valOut, actOut);
        check(actOut[0] == 1 && std::abs(rawOut[0] - 5.0f) < 1e-3f, "G5 evaluateSegment exports active raw driver");
        check(std::abs(valOut[0] - (float)sm[0].lpfHz) < 1.0f && valOut[0] > 10500.f && valOut[0] < 11500.f,
              "G5 evaluateSegment exports LPF target in Hz matching SourceModState::lpfHz");
        check(actOut[1] == 1 && std::abs(rawOut[1] - 7.0f) < 1e-3f && std::abs(valOut[1] + 7.0f) < 1e-3f,
              "G5 evaluateSegment exports SourceGain target delta in dB");
        check(actOut[2] == 0, "G5 disabled route reports inactive telemetry");

        auto telProc = std::make_unique<VerbProcessor>();
        auto& p = *telProc;
        p.setTailSettings({8000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        waitReady(p, 90);
        p.loadSceneText(g4test::kBoxScene, "G5Tel", true);
        waitReady(p, 90);

        check(p.webConfiguredModRouteCount() == 0, "G5 processor telemetry route count starts at 0");
        auto rt = g4test::makeRoute(0, ModDriver::DistanceToListener, 0.0, 10.0, ModTarget::SourceGain, 0.0, -20.0, 0.0, 0.0);
        p.setModulationRoutes({rt});
        check(p.webConfiguredModRouteCount() == 1, "G5 processor telemetry reflects configured route count");

        setP(p, "mode", 1); // Creative
        g4test::primeCreativeAndSettle(p, 8);
        VerbProcessor::ModLiveSample samples[kMaxModRoutes]{};
        int cnt = 0;
        const bool fresh = p.webModTelemetry(samples, kMaxModRoutes, cnt);
        check(fresh && cnt == 1 && samples[0].active, "G5 VerbProcessor::webModTelemetry active after Creative processBlock");
        check(std::abs(samples[0].raw - 1.5f) < 0.1f && std::abs(samples[0].value + 3.0f) < 0.2f,
              "G5 VerbProcessor::webModTelemetry reports exact raw (1.5m) & gain (-3dB)");

        setP(p, "mode", 0); // Back to Physical; pump until creative fade finishes -> fast path
        juce::AudioBuffer<float> buf(2, 128);
        juce::MidiBuffer midi;
        for (int b = 0; b < 32; ++b) { buf.clear(); stage2Process(p, buf, midi); }
        const bool freshPhys = p.webModTelemetry(samples, kMaxModRoutes, cnt);
        check(!freshPhys && !samples[0].active, "G5 Physical mode returns inactive telemetry without blocking fast path");
    }

    return fails == before ? 0 : 1;
}

static int testG8ModulationHostOutputs() {
    using namespace verbp;
    const int before = fails;

    // 1. Parameter indices, read-only metadata, APVTS/state isolation, host echo immunity
    {
        VerbProcessor p;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "G8 processor ready");

        const auto& params = p.getParameters();
        check(params.size() == 283 + kMaxModHostOuts, "G8 total parameter count is 283 + 16 = 299");
        check(p.apvts.getParameter("s1x") && p.apvts.getParameter("s1x")->getParameterIndex() == 0
              && p.apvts.getParameter("makeup") && p.apvts.getParameter("makeup")->getParameterIndex() == 249
              && p.apvts.getParameter("s1path") && p.apvts.getParameter("s1path")->getParameterIndex() == 250
              && p.apvts.getParameter("lpath") && p.apvts.getParameter("lpath")->getParameterIndex() == 282,
              "G8 keeps first 283 APVTS parameter indices completely unchanged");

        bool idsAndFlagsOk = true;
        for (int s = 0; s < kMaxModHostOuts; ++s) {
            auto* prm = p.modOutParameter(s);
            const auto expectedId = VerbProcessor::modOutParamId(s);
            const auto expectedName = "Mod Out " + juce::String(s + 1);
            if (!prm
                || prm->getParameterIndex() != 283 + s
                || p.getParameterID(283 + s) != expectedId
                || prm->getName(128) != expectedName
                || !prm->isAutomatable()
                || prm->getCategory() != juce::AudioProcessorParameter::genericParameter
                || std::abs(prm->getDefaultValue() - 0.5f) > 1e-6f
                || std::abs(prm->getValue() - 0.5f) > 1e-6f
                || p.apvts.getParameter(expectedId) != nullptr) {
                idsAndFlagsOk = false;
                break;
            }
        }
        check(idsAndFlagsOk, "G9 Mod Out 1..16 appended at 283..298, genericParameter, isAutomatable()==true, neutral 0.5, excluded from APVTS");

        struct HostEchoListener final : juce::AudioProcessorParameter::Listener {
            int changes = 0;
            float last = -1.f;
            void parameterValueChanged(int, float v) override { ++changes; last = v; }
            void parameterGestureChanged(int, bool) override {}
        } listener;
        auto* mo0 = p.modOutParameter(0);
        mo0->addListener(&listener);
        mo0->setValue(0.123f);
        mo0->setValue(0.987f);
        check(std::abs(mo0->getValue() - 0.5f) < 1e-6f && listener.changes == 0,
              "G8 Mod Out ignores host/echo setValue without triggering feedback notifications");
        mo0->removeListener(&listener);

        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        const auto xml = juce::AudioProcessor::getXmlFromBinary(mb.getData(), (int)mb.getSize());
        bool stateClean = (xml != nullptr);
        if (xml) {
            for (int s = 0; s < kMaxModHostOuts; ++s) {
                if (xml->getChildByAttribute("id", VerbProcessor::modOutParamId(s)) != nullptr) {
                    stateClean = false;
                    break;
                }
            }
        }
        const auto preset = p.getRoomPresetText();
        check(stateClean && !preset.contains("\"mo1\"") && !preset.contains("\"mo16\""),
              "G8 Mod Out parameters never enter project state or .verbroom preset XML");
    }

    // 2. Accuracy (0-smoothing < 1e-6 & smoothed exponential tracking), clamping [0, 1], ModTarget::None audio bit-identity
    {
        auto procA = std::make_unique<VerbProcessor>();
        auto procB = std::make_unique<VerbProcessor>();
        for (VerbProcessor* pr : {procA.get(), procB.get()}) {
            pr->setTailSettings({4000, false});
            pr->setPlayConfigDetails(2, 2, 48000, 128);
            pr->prepareToPlay(48000, 128);
            waitReady(*pr, 90);
            pr->loadSceneText(g4test::kBoxScene, "G8BitId", true);
            waitReady(*pr, 90);
            setP(*pr, "mode", 1);
            setP(*pr, "soloDirect", 1);
        }

        // Non-monotonic V-shaped curve + clamp=false (extrapolates beyond [0,1]) with ModTarget::None -> Host Out 1
        auto r = g4test::makeRoute(0, ModDriver::X, 2.0, 10.0, ModTarget::None, 0.0, 1.0, 0.0, 0.0);
        r.hostOut = 1;
        r.curve.clamp = false;
        r.curve.nodeCount = 3;
        r.curve.nodes[0] = {0.0, 0.8, false};
        r.curve.nodes[1] = {0.5, 0.1, false};
        r.curve.nodes[2] = {1.0, 0.9, false};
        check(procB->setModulationRoutes({r}), "G8 accepts ModTarget::None route with hostOut=1");

        // Exact core curve precision (< 1e-6) directly on ModulationProcessor::evaluateSegment
        {
            ModulationProcessor exactMp;
            exactMp.reset();
            ModRouteSet mrs;
            mrs.generation = 1;
            mrs.count = 1;
            mrs.routes[0] = r;
            double poses[kModMaxSources + 1][4] = {};
            double uArr[kModMaxSources + 1] = {};
            bool jumped[kModMaxSources + 1] = {};
            WallSnapshot ws;
            std::array<SourceModState, kModMaxSources> sm{};
            GlobalModState gm{};
            double hostOutSlots[kMaxModHostOuts]{};
            double exactErr = 0.0;
            for (double x : {2.0, 4.0, 6.0, 8.0, 10.0, 14.0, 0.5}) {
                poses[0][0] = x;
                exactMp.evaluateSegment(mrs, 1, poses, uArr, jumped, ws, true, 1.0, 128, 48000.0, sm, gm, nullptr, nullptr, nullptr, hostOutSlots);
                const double expN = std::clamp(r.curve.evaluateNormalized(x), 0.0, 1.0);
                exactErr = std::max(exactErr, std::abs(expN - hostOutSlots[0]));
            }
            std::printf("  [G8 Core Math Err] max=%.12f\n", exactErr);
            check(exactErr < 1e-6, "G8 unsmoothed core hostOut matches clamped curve.evaluateNormalized(driver) within 1e-6");
        }

        std::vector<float> audioA, audioB;
        juce::AudioBuffer<float> bufA(2, 128), bufB(2, 128);
        juce::MidiBuffer midi;
        double maxCurveErr = 0.0;
        const double testXs[] = {2.0, 4.0, 6.0, 8.0, 10.0, 14.0, 0.5};
        for (int k = 0; k < 7; ++k) {
            const double x = testXs[k];
            setP(*procA, "s1x", x);
            setP(*procB, "s1x", x);
            bufA.clear(); bufB.clear();
            if (k == 0) { bufA.setSample(0, 0, 0.7f); bufB.setSample(0, 0, 0.7f); }
            for (int i = 0; i < 128 && k > 0; ++i) {
                const float s = 0.2f * (float)std::sin(2.0 * verb::kPi * 330.0 * (k * 128 + i) / 48000.0);
                bufA.setSample(0, i, s);
                bufB.setSample(0, i, s);
            }
            stage2Process(*procA, bufA, midi);
            stage2Process(*procB, bufB, midi);
            for (int i = 0; i < 128; ++i) {
                audioA.push_back(bufA.getSample(0, i));
                audioB.push_back(bufB.getSample(0, i));
            }
            const double expected = std::clamp(r.curve.evaluateNormalized(x), 0.0, 1.0);
            const double got = (double)procB->modOutParameter(0)->getValue();
            maxCurveErr = std::max(maxCurveErr, std::abs(expected - got));
        }
        std::printf("  [G8 Accuracy & BitId] maxCurveErr=%.8f, memcmpAB=%d\n",
                    maxCurveErr, std::memcmp(audioA.data(), audioB.data(), audioA.size() * sizeof(float)));
        check(maxCurveErr <= 1.5e-4, "G8 processor hostOut matches clamped curve.evaluateNormalized within epsilon tolerance (1.5e-4)");
        check(std::memcmp(audioA.data(), audioB.data(), audioA.size() * sizeof(float)) == 0,
              "G8 ModTarget::None leaves Verb audio 100% bit-identical in Creative mode during movement");

        // Verify smoothed envelope matches exponential normalized tracking (first settle low with fast release)
        r.attackMs = 40.0;
        r.releaseMs = 15.0;
        r.curve.clamp = true;
        r.curve.nodeCount = 2;
        r.curve.nodes[0] = {0.0, 0.0, false};
        r.curve.nodes[1] = {1.0, 1.0, false};
        procB->setModulationRoutes({r});
        setP(*procB, "s1x", 2.0); // curve = 0.0 (15ms release settles completely over 80 blocks = 213ms)
        for (int b = 0; b < 80; ++b) { bufB.clear(); stage2Process(*procB, bufB, midi); }
        const float settledLow = procB->modOutParameter(0)->getValue();
        setP(*procB, "s1x", 10.0); // step to 1.0 with 40ms attack
        const double dtBlk = 128.0 / 48000.0;
        double sim = (double)settledLow;
        const double alphaAtt = 1.0 - std::exp(-dtBlk / 0.040);
        double maxSmoothErr = 0.0;
        for (int b = 0; b < 16; ++b) {
            bufB.clear();
            stage2Process(*procB, bufB, midi);
            sim += (1.0 - sim) * alphaAtt;
            maxSmoothErr = std::max(maxSmoothErr, std::abs((double)procB->modOutParameter(0)->getValue() - sim));
        }
        check(settledLow < 0.01f && maxSmoothErr < 0.01, "G8 smoothed hostOut follows exponential normalized attack/release trajectory");
    }

    // 3. Creative-only neutral 0.5f in Physical, smooth clickless transition, epsilon gating & 0 audio-thread allocations
    {
        auto proc = std::make_unique<VerbProcessor>();
        auto& p = *proc;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        waitReady(p, 90);
        p.loadSceneText(g4test::kBoxScene, "G8Rules", true);
        waitReady(p, 90);

        struct CountListener final : juce::AudioProcessorParameter::Listener {
            int count = 0;
            float lastVal = -1.f;
            void parameterValueChanged(int, float v) override {
                ++count;
                lastVal = v;
            }
            void parameterGestureChanged(int, bool) override {}
        } lst;
        p.modOutParameter(0)->addListener(&lst);

        auto r = g4test::makeRoute(0, ModDriver::X, 0.0, 10.0, ModTarget::SourceGain, 0.0, -18.0, 0.0, 0.0);
        r.hostOut = 1;
        p.setModulationRoutes({r});
        setP(p, "mode", 1); // Creative
        setP(p, "s1x", 9.5); // 0.95
        juce::AudioBuffer<float> buf(2, 128); juce::MidiBuffer midi;
        for (int b = 0; b < 12; ++b) { buf.clear(); stage2Process(p, buf, midi); }
        check(std::abs(p.modOutParameter(0)->getValue() - 0.95f) < 1e-4f, "G8 simultaneous SourceGain + hostOut delivers 0.95 in Creative");

        // Static scene -> 0 notifications across 30 blocks; sub-epsilon (<1e-4) step -> 0 notifications
        lst.count = 0;
        for (int b = 0; b < 30; ++b) { buf.clear(); stage2Process(p, buf, midi); }
        check(lst.count == 0, "G8 static position triggers 0 host notifications");

        setP(p, "s1x", 9.5 + 0.0005); // +5e-5 normalized (< 1e-4 epsilon)
        buf.clear(); stage2Process(p, buf, midi);
        check(lst.count == 0, "G8 sub-epsilon (<1e-4) movement triggers 0 host notifications");

        // Large step (> 1e-4) -> exactly 1 block-rate notification
        setP(p, "s1x", 8.5); // 0.85
        buf.clear(); stage2Process(p, buf, midi);
        check(lst.count == 1 && std::abs(lst.lastVal - 0.85f) < 1e-4f, "G8 >epsilon change triggers exactly 1 block-rate host update");

        // Host write-back / automation ignored while modulation output is active (0 feedback notifications)
        lst.count = 0;
        p.modOutParameter(0)->setValue(0.11f);
        p.modOutParameter(0)->setValue(0.99f);
        check(std::abs(p.modOutParameter(0)->getValue() - 0.85f) < 1e-4f && lst.count == 0,
              "G9 active Mod Out ignores host write-back setValue with 0 feedback notifications");

        // 0 audio-thread allocations across 16 active slots with motion
        std::vector<ModulationRoute> sixteen;
        for (int s = 0; s < kMaxModHostOuts; ++s) {
            auto rs = g4test::makeRoute(0, ModDriver::X, 0.0, 10.0, ModTarget::None, 0.0, 1.0, 0.0, 0.0);
            rs.hostOut = s + 1;
            sixteen.push_back(rs);
        }
        p.setModulationRoutes(sixteen);
        buf.clear(); stage2Process(p, buf, midi);
        stage2AudioAllocations = 0;
        for (int b = 0; b < 64; ++b) {
            setP(p, "s1x", 1.0 + 8.0 * (b / 63.0));
            buf.clear();
            stage2Process(p, buf, midi);
        }
        check(stage2AudioAllocations == 0, "G8 16 active hostOut routes during motion perform 0 audio-thread allocations");

        // Creative -> Physical smooth ramp back to 0.5 neutral (20ms fade settles <1e-6 in ~280ms = ~105 blocks)
        p.setModulationRoutes({r});
        setP(p, "s1x", 9.5);
        for (int b = 0; b < 16; ++b) { buf.clear(); stage2Process(p, buf, midi); }
        lst.count = 0;
        setP(p, "mode", 0); // Physical
        float maxHostJump = 0.f;
        float prevV = p.modOutParameter(0)->getValue();
        for (int b = 0; b < 128; ++b) {
            buf.clear();
            stage2Process(p, buf, midi);
            const float curV = p.modOutParameter(0)->getValue();
            maxHostJump = std::max(maxHostJump, std::abs(curV - prevV));
            prevV = curV;
        }
        check(maxHostJump < 0.15f && std::abs(p.modOutParameter(0)->getValue() - 0.5f) < 1e-6f,
              "G8 Creative->Physical ramps Mod Out smoothly to exact 0.5 neutral without jumps");
        lst.count = 0;
        for (int b = 0; b < 16; ++b) { buf.clear(); stage2Process(p, buf, midi); }
        check(lst.count == 0, "G8 settled Physical mode emits 0 host notifications");

        // Duplicate hostOut rule: last enabled route wins (let creativeBlend reach 1.0 over 120 blocks = 320ms)
        setP(p, "mode", 1);
        setP(p, "s1x", 2.0);
        auto dupA = g4test::makeRoute(0, ModDriver::X, 0.0, 10.0, ModTarget::None, 0.0, 1.0, 0.0, 0.0);
        dupA.hostOut = 1;
        auto dupB = g4test::makeRoute(0, ModDriver::X, 0.0, 10.0, ModTarget::None, 0.0, 1.0, 0.0, 0.0);
        dupB.hostOut = 1;
        dupB.curve.invert = true; // 1 - 0.2 = 0.8
        p.setModulationRoutes({dupA, dupB});
        for (int b = 0; b < 120; ++b) { buf.clear(); stage2Process(p, buf, midi); }
        check(std::abs(p.modOutParameter(0)->getValue() - 0.8f) < 1e-4f, "G8 duplicate hostOut rule: last enabled route wins (0.8)");
        dupB.enabled = false;
        p.setModulationRoutes({dupA, dupB});
        for (int b = 0; b < 8; ++b) { buf.clear(); stage2Process(p, buf, midi); }
        check(std::abs(p.modOutParameter(0)->getValue() - 0.2f) < 1e-4f, "G8 duplicate hostOut rule: disabling last falls back to earlier enabled route (0.2)");

        p.modOutParameter(0)->removeListener(&lst);
    }

    // 4. XML / State / .verbroom Keep I/O / old state compat
    {
        auto proc = std::make_unique<VerbProcessor>();
        auto& p = *proc;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        waitReady(p, 90);
        p.loadSceneText(g4test::kBoxScene, "G8State", true);
        waitReady(p, 90);

        // Route without hostOut omits hostOut attribute from XML
        auto rPlain = g4test::makeRoute(0, ModDriver::DistanceToListener, 1.0, 10.0, ModTarget::SourceGain, 0.0, -12.0, 15.0, 80.0);
        p.setModulationRoutes({rPlain});
        juce::MemoryBlock plainState;
        p.getStateInformation(plainState);
        const auto plainXml = juce::AudioProcessor::getXmlFromBinary(plainState.getData(), (int)plainState.getSize());
        const auto* plainModEl = plainXml ? plainXml->getChildByName("Modulation") : nullptr;
        const auto* plainRtEl = plainModEl ? plainModEl->getChildByName("Route") : nullptr;
        check(plainRtEl != nullptr && !plainRtEl->hasAttribute("hostOut"), "G8 XML omits hostOut attribute when hostOut == 0");

        // Routes with None + hostOut round-trip through state and .verbroom
        auto rSrc = g4test::makeRoute(0, ModDriver::DistanceToListener, 1.0, 10.0, ModTarget::SourceGain, 0.0, -12.0, 15.0, 80.0);
        rSrc.hostOut = 3;
        auto rNone = g4test::makeRoute(0, ModDriver::X, 0.0, 10.0, ModTarget::None, 0.0, 1.0, 5.0, 20.0);
        rNone.hostOut = 5;
        auto rRoom = g4test::makeRoute(kListener, ModDriver::X, 0.0, 10.0, ModTarget::Mix, 0.0, -25.0, 15.0, 80.0);
        rRoom.hostOut = 7;

        p.setModulationRoutes({rSrc, rNone, rRoom});
        juce::MemoryBlock g8State;
        p.getStateInformation(g8State);
        const auto g8Preset = p.getRoomPresetText();

        p.setModulationRoutes({});
        p.setStateInformation(g8State.getData(), (int)g8State.getSize());
        waitReady(p, 90);
        check(p.getModulationRoutes() == std::vector<ModulationRoute>({rSrc, rNone, rRoom}),
              "G8 project state round-trip preserves target=none and hostOut slots");

        // Build preset containing room-global earlyLevel(hostOut=9) + sourceLowPass(hostOut=11) + none(hostOut=12)
        auto rPresetGlobal = g4test::makeRoute(kListener, ModDriver::Z, 0.0, 4.0, ModTarget::EarlyLevel, 0.0, -6.0, 15.0, 80.0);
        rPresetGlobal.hostOut = 9;
        auto rPresetSrc = g4test::makeRoute(0, ModDriver::X, 0.0, 10.0, ModTarget::SourceLowPass, 20000.0, 1500.0, 15.0, 80.0);
        rPresetSrc.hostOut = 11;
        auto rPresetNone = g4test::makeRoute(0, ModDriver::Y, 0.0, 10.0, ModTarget::None, 0.0, 1.0, 15.0, 80.0);
        rPresetNone.hostOut = 12;
        p.setModulationRoutes({rPresetGlobal, rPresetSrc, rPresetNone});
        const auto mergePreset = p.getRoomPresetText();

        // Reload local routes with Keep I/O ON -> load mergePreset
        p.setModulationRoutes({rSrc, rNone, rRoom});
        p.setKeepRoutingEnabled(true);
        juce::String err;
        check(p.loadRoomPresetText(mergePreset, err) && waitReady(p, 90), "G8 Keep I/O .verbroom reload succeeds");
        check(p.getModulationRoutes() == std::vector<ModulationRoute>({rSrc, rNone, rPresetGlobal}),
              "G8 Keep I/O preserves local source* + local none routes (with hostOut) and imports preset room-global route (with hostOut)");

        // Loading preset with Keep I/O OFF loads all preset routes including none + hostOut
        p.setKeepRoutingEnabled(false);
        check(p.loadRoomPresetText(mergePreset, err) && waitReady(p, 90), "G8 full .verbroom load without Keep I/O succeeds");
        check(p.getModulationRoutes() == std::vector<ModulationRoute>({rPresetGlobal, rPresetSrc, rPresetNone}),
              "G8 full .verbroom load without Keep I/O restores preset routes including target=none + hostOut");

        // Old state without Modulation / hostOut cleanly resets Mod Out to 0.5
        p.setStateInformation(plainState.getData(), (int)plainState.getSize());
        waitReady(p, 90);
        check(p.getModulationRoutes().size() == 1 && p.getModulationRoutes()[0].hostOut == 0
              && std::abs(p.modOutParameter(0)->getValue() - 0.5f) < 1e-6f,
              "G8 old state without hostOut loads cleanly with neutral Mod Out 0.5");
    }

    return fails == before ? 0 : 1;
}
