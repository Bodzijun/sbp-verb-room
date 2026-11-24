#pragma once
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include "MovementPath.h"

namespace g1test {
static bool nearAz(double a, double b, double tol = 1e-6) {
    return std::abs(std::remainder(a - b, 360.0)) < tol;
}
}  // namespace g1test

static int testAzimuthKeysFast() {
    const int before = fails;
    using namespace verbp;
    std::printf("G1 azimuth keys: math, validation, XML/preset/KeepIO compatibility\n");

    // 1. evaluateAzimuthKeys math
    {
        std::vector<AzimuthKey> empty;
        check(!evaluateAzimuthKeys(empty, 0.5).has_value(), "G1 empty azKeys returns nullopt");

        std::vector<AzimuthKey> single{{40.0, 75.0, AzimuthEasing::Hold}};
        for (double u : {0.0, 0.4, 1.0}) {
            auto v = evaluateAzimuthKeys(single, u);
            check(v.has_value() && std::abs(*v - 75.0) < 1e-12, "G1 single key holds constant angle everywhere");
        }

        std::vector<AzimuthKey> mix{
            {20.0, -30.0, AzimuthEasing::Hold},
            {40.0, 90.0, AzimuthEasing::Linear},
            {60.0, 170.0, AzimuthEasing::EaseInOut},
            {80.0, -170.0, AzimuthEasing::EaseIn},
            {90.0, -90.0, AzimuthEasing::EaseOut},
            {100.0, -10.0, AzimuthEasing::Linear}
        };
        check(g1test::nearAz(*evaluateAzimuthKeys(mix, 0.0), -30.0), "G1 clamp before first key");
        check(g1test::nearAz(*evaluateAzimuthKeys(mix, 0.2), -30.0), "G1 exact Hold start");
        check(g1test::nearAz(*evaluateAzimuthKeys(mix, 0.3999), -30.0), "G1 Hold stays flat right up to next key");
        check(g1test::nearAz(*evaluateAzimuthKeys(mix, 0.4), 90.0), "G1 Hold steps immediately at next key t");
        check(g1test::nearAz(*evaluateAzimuthKeys(mix, 0.5), 130.0), "G1 Linear midpoint 90->170 = 130");

        // EaseInOut across 170 -> -170 (+20 deg shortest arc through +-180)
        check(g1test::nearAz(*evaluateAzimuthKeys(mix, 0.7), 180.0), "G1 170->-170 midpoint crosses +-180");
        const double s1 = 0.25, eInOut = s1 * s1 * (3.0 - 2.0 * s1);
        check(g1test::nearAz(*evaluateAzimuthKeys(mix, 0.65), std::remainder(170.0 + 20.0 * eInOut, 360.0)), "G1 EaseInOut quarter shortest arc 170->-170");
        check(g1test::nearAz(*evaluateAzimuthKeys(mix, 0.85), -170.0 + 80.0 * (0.5 * 0.5)), "G1 EaseIn midpoint -170->-90 (s=0.5 -> 0.25)");
        const double inv = 1.0 - 0.5;
        check(g1test::nearAz(*evaluateAzimuthKeys(mix, 0.95), -90.0 + 80.0 * (1.0 - inv * inv)), "G1 EaseOut midpoint -90->-10 (s=0.5 -> 0.75)");
        check(g1test::nearAz(*evaluateAzimuthKeys(mix, 1.0), -10.0), "G1 last key at 100%");

        // Reverse wrap -170 -> 170 (-20 deg shortest arc)
        std::vector<AzimuthKey> rev{{0.0, -170.0, AzimuthEasing::Linear}, {100.0, 170.0, AzimuthEasing::Linear}};
        check(g1test::nearAz(*evaluateAzimuthKeys(rev, 0.25), -175.0), "G1 reverse wrap -170->170 quarter = -175");
        check(g1test::nearAz(*evaluateAzimuthKeys(rev, 0.75), 175.0), "G1 reverse wrap -170->170 three-quarters = +175");
    }

    // 2. validPath rejects & normalizePathAzKeys
    {
        MovementPath p;
        p.points = {{1, 1, 1.5}, {3, 1, 1.5}};
        p.azKeys = {{10, 0, AzimuthEasing::Linear}, {50, 90, AzimuthEasing::Hold}};
        check(validPath(p), "G1 validPath accepts sorted azKeys on moving path");

        MovementPath noPts;
        noPts.azKeys = {{10, 0, AzimuthEasing::Linear}};
        check(!validPath(noPts), "G1 validPath rejects azKeys without path points");

        MovementPath singlePt;
        singlePt.points = {{1, 1, 1.5}};
        singlePt.azKeys = {{10, 0, AzimuthEasing::Linear}};
        check(!validPath(singlePt), "G1 validPath rejects azKeys with < 2 points");

        MovementPath badT = p; badT.azKeys[0].t = -0.1; check(!validPath(badT), "G1 rejects t < 0");
        badT = p; badT.azKeys[1].t = 100.1; check(!validPath(badT), "G1 rejects t > 100");
        badT = p; badT.azKeys[0].t = std::nan(""); check(!validPath(badT), "G1 rejects NaN t");
        badT = p; badT.azKeys = {{50, 0}, {50, 45}}; check(!validPath(badT), "G1 rejects equal t");
        badT = p; badT.azKeys = {{60, 0}, {40, 45}}; check(!validPath(badT), "G1 rejects non-increasing t");
        badT = p; badT.azKeys[0].azDeg = 180.1; check(!validPath(badT), "G1 rejects az > 180");
        badT = p; badT.azKeys[0].azDeg = -180.1; check(!validPath(badT), "G1 rejects az < -180");
        badT = p; badT.azKeys[0].azDeg = std::nan(""); check(!validPath(badT), "G1 rejects NaN az");
        badT = p; badT.azKeys[0].easing = (AzimuthEasing)5; check(!validPath(badT), "G1 rejects easing > Hold");

        MovementPath atCap = p;
        atCap.azKeys.clear();
        for (size_t i = 0; i < kMaxAzKeys; ++i) atCap.azKeys.push_back({100.0 * (double)i / (double)(kMaxAzKeys - 1), 0.0, AzimuthEasing::Linear});
        check(validPath(atCap), "G1 accepts exactly kMaxAzKeys (128)");
        atCap.azKeys.back().t = 99.0;
        atCap.azKeys.push_back({100.0, 0.0, AzimuthEasing::Linear});
        check(!validPath(atCap), "G1 rejects 129th azKey");

        MovementPath norm = p;
        norm.azKeys = {{50.0, 20.0, AzimuthEasing::EaseOut}, {20.0, -10.0, AzimuthEasing::Hold}, {50.0 + 5e-7, 77.0, AzimuthEasing::EaseInOut}};
        normalizePathAzKeys(norm);
        check(norm.azKeys.size() == 2 && norm.azKeys[0].t == 20.0 && norm.azKeys[1].azDeg == 77.0 && norm.azKeys[1].easing == AzimuthEasing::EaseInOut,
              "G1 normalizePathAzKeys sorts and collapses < 1e-6 duplicates keeping newer key");
    }

    // 3. XML / .verbroom round-trip, byte-identical XML without keys, Keep I/O, old-state & stage2 compatibility
    {
        auto proc = std::make_unique<VerbProcessor>();
        auto& p = *proc;
        p.setTailSettings({8000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "G1 state test processor ready");
        p.loadSceneText(p4test::kScene, "G1 state room", true);
        check(waitReady(p, 90), "G1 state room loads");

        MovementPath base;
        base.points = {{1, 1, 1.5}, {3, 1, 1.5}};
        check(p.setMovementPath(0, base), "G1 base path without keys accepted");
        const juce::String xmlNoKeys = p4test::stateXml(p);
        check(!xmlNoKeys.contains("<AzKey"), "G1 state without azKeys writes no AzKey elements");

        MovementPath withKeys = base;
        withKeys.azKeys = {
            {0.0, -45.0, AzimuthEasing::Linear},
            {25.0, 90.0, AzimuthEasing::Hold},
            {50.0, 170.0, AzimuthEasing::EaseInOut},
            {75.0, -170.0, AzimuthEasing::EaseIn},
            {100.0, 15.0, AzimuthEasing::EaseOut}
        };
        check(p.setMovementPath(0, withKeys), "G1 path with 5 azKeys accepted");
        const juce::String xmlWithKeys = p4test::stateXml(p);
        const juce::String presetWithKeys = p.getRoomPresetText();
        check(xmlWithKeys.contains("<AzKey") && presetWithKeys.contains("<AzKey"), "G1 state & preset serialize AzKey children");

        // Removing keys yields byte-identical state XML
        check(p.setMovementPath(0, base), "G1 clear azKeys on processor");
        const juce::String xmlCleared = p4test::stateXml(p);
        check(xmlNoKeys == xmlCleared, "G1 empty azKeys state XML stays byte-identical to 0.2.3");

        // Reload stateWithKeys into fresh processor
        {
            auto qStorage = std::make_unique<VerbProcessor>();
            auto& q = *qStorage;
            q.setTailSettings({8000, false});
            q.setPlayConfigDetails(2, 2, 48000, 128);
            q.prepareToPlay(48000, 128);
            check(waitReady(q, 90) && p4test::loadStateXml(q, xmlWithKeys), "G1 state with AzKey reloads cleanly");
            const auto restored = q.getMovementPath(0);
            check(restored == withKeys && restored.points.size() == 2 && restored.azKeys.size() == 5,
                  "G1 AzKey children never parsed as Points and round-trip exactly");
        }

        // Keep I/O preserves local sources & output while importing azKeys in preset
        {
            auto qStorage = std::make_unique<VerbProcessor>();
            auto& q = *qStorage;
            q.setTailSettings({8000, false});
            q.setPlayConfigDetails(4, 2, 48000, 128);
            q.prepareToPlay(48000, 128);
            check(waitReady(q, 90), "G1 Keep I/O processor ready");
            auto customSrc = q.getSources();
            customSrc[0].name = "KeepMe";
            customSrc[0].firstIn = 2;
            q.setSources(customSrc);
            check(waitReady(q, 90), "G1 Keep I/O custom source ready");
            auto customOut = q.getOutputCfg();
            customOut.micPattern = 2;
            q.setOutputCfg(customOut);
            check(waitReady(q, 90), "G1 Keep I/O custom output ready");
            q.setKeepRoutingEnabled(true);
            juce::String err;
            check(q.loadRoomPresetText(presetWithKeys, err) && waitReady(q, 90), "G1 Keep I/O loads preset with AzKey");
            check(q.getSources()[0].name == "KeepMe" && q.getSources()[0].firstIn == 2 && q.getOutputCfg().micPattern == 2,
                  "G1 Keep I/O preserves local routing and output");
            check(q.getMovementPath(0) == withKeys, "G1 Keep I/O imports scene movement path with azKeys");
        }

        // Unknown future child inside <Path> is ignored cleanly without corrupting points or azKeys
        {
            auto parsed = juce::parseXML(xmlWithKeys);
            if (parsed) {
                if (auto* pathsEl = parsed->getChildByName("MovementPaths"))
                    if (auto* pathEl = pathsEl->getChildByName("Path")) {
                        auto* extra = new juce::XmlElement("FutureTag");
                        extra->setAttribute("v", 99);
                        pathEl->addChildElement(extra);
                    }
            }
            auto qStorage = std::make_unique<VerbProcessor>();
            auto& q = *qStorage;
            q.setPlayConfigDetails(2, 2, 48000, 128);
            q.prepareToPlay(48000, 128);
            check(waitReady(q, 90) && parsed && p4test::loadStateXml(q, parsed->toString()),
                  "G1 state with unknown child tag under <Path> loads cleanly");
            check(q.getMovementPath(0) == withKeys, "G1 unknown child tag under <Path> is ignored safely");
        }

        // Old state without azKeys renders identically across reload, and stage2 fixture loads unchanged
        {
            const auto settledImpulse = [&](VerbProcessor& vp) {
                juce::AudioBuffer<float> b(2, 128); juce::MidiBuffer m;
                auto eng = E9bAccess::engine(vp);
                const auto until = juce::Time::getMillisecondCounterHiRes() + 15000;
                while (juce::Time::getMillisecondCounterHiRes() < until) {
                    b.clear(); stage2Process(vp, b, m); juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
                    if (eng && eng->tailIsCurrent() && eng->motionStats().transitionsDone > 0) break;
                }
                for (int i = 0; i < 600; ++i) { b.clear(); stage2Process(vp, b, m); }
                return p4test::impulse(vp, "s1path", 0.0);
            };
            p4test::impulse(p, "s1path", 0.0);
            const auto ir0 = settledImpulse(p);
            auto qStorage = std::make_unique<VerbProcessor>();
            auto& q = *qStorage;
            q.setTailSettings({8000, false});
            q.setPlayConfigDetails(2, 2, 48000, 128);
            q.prepareToPlay(48000, 128);
            check(waitReady(q, 90) && p4test::loadStateXml(q, xmlNoKeys), "G1 0.2.3-style state reloads");
            const auto ir1 = settledImpulse(q);
            const double diff = p4test::maxDiff(ir0, ir1);
            double e0 = 0, e1 = 0;
            for (size_t i = 0; i < ir0.size(); ++i) { e0 += ir0[i] * ir0[i]; e1 += ir1[i] * ir1[i]; }
            const double dDb = (e0 > 0 && e1 > 0) ? 10.0 * std::log10(e1 / e0) : 999.0;
            std::printf("  [G1] 0.2.3 reload diff=%.3e, dB=%.6f (e0=%.9g e1=%.9g)\n", diff, dDb, e0, e1);
            check(diff <= 2e-5 && std::abs(dDb) <= 0.001, "G1 state without keys renders identically (diff <= 2e-5, <= 0.001 dB)");

            const auto stage2File = repoFile("plugin/fixtures/stage2/legacy.verbroom");
            check(stage2File.existsAsFile(), "G1 stage2 legacy.verbroom fixture exists");
            if (stage2File.existsAsFile()) {
                juce::String err;
                check(q.loadRoomPresetText(stage2File.loadFileAsString(), err) && waitReady(q, 90),
                      "G1 stage2/legacy.verbroom loads cleanly");
                bool allEmpty = true;
                for (int o = 0; o <= kMaxSources; ++o) allEmpty &= q.getMovementPath(o).azKeys.empty();
                const auto irStage2 = p4test::impulse(q, "s1path", 0.0);
                double es = 0; bool fin = true;
                for (float v : irStage2) { es += (double)v * v; fin &= std::isfinite(v); }
                check(allEmpty && fin && es > 1e-8, "G1 stage2/legacy.verbroom has empty azKeys and finite audio");
            }
        }
    }
    return fails == before ? 0 : 1;
}

static int testAzimuthKeys() {
    const int before = fails;
    using namespace verbp;
    if (testAzimuthKeysFast() != 0) {}

    std::printf("G1 azimuth keys: live processor, Hold jump vs XYZ tracker, bake accuracy & perf, F3/P4\n");

    // 4. Processor liveAzimuth, Energy scene, mode priorities (Manual vs AlongPath/LookAt/disabled), group coupling, laps > 1
    {
        auto proc = std::make_unique<VerbProcessor>();
        auto& p = *proc;
        p.setTailSettings({8000, false});
        p.setPlayConfigDetails(4, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "G1 live processor ready");
        {
            auto cfg = p.getSources();
            cfg.resize(2);
            cfg[0].name = "A"; cfg[0].firstIn = 0; cfg[0].format = 0; cfg[0].pattern = 0.8f;
            cfg[1].name = "B"; cfg[1].firstIn = 1; cfg[1].format = 1; cfg[1].freePoints = true;
            cfg[1].ox = {0.5f, -0.5f}; cfg[1].oy = {0.f, 0.f}; cfg[1].oz = {0.f, 0.f};
            p.setSources(cfg);
            check(waitReady(p, 90), "G1 two sources ready");
        }
        juce::AudioBuffer<float> buf(4, 128); juce::MidiBuffer midi;
        auto block = [&]() { buf.clear(); p.processBlock(buf, midi); };

        MovementPath p0;
        p0.points = {{2, 2, 1.5}, {6, 2, 1.5}};
        p0.group = 0;
        p0.azKeys = {
            {0.0, 0.0, AzimuthEasing::Linear},
            {50.0, 90.0, AzimuthEasing::Hold},
            {75.0, -90.0, AzimuthEasing::EaseInOut},
            {100.0, 0.0, AzimuthEasing::Linear}
        };
        MovementPath p1;
        p1.points = {{2, 5, 1.5}, {6, 5, 1.5}};
        p1.group = 0;
        p1.azKeys = {{0.0, 170.0, AzimuthEasing::Linear}, {100.0, -170.0, AzimuthEasing::Linear}};
        check(p.setMovementPath(0, p0) && p.setMovementPath(1, p1), "G1 coupled paths with azKeys accepted");
        setP(p, "s1az", 123.0);
        setP(p, "s2az", -77.0);

        for (double u : {0.0, 0.25, 0.5, 0.65, 0.75, 0.9, 1.0}) {
            setP(p, "s1path", u);
            block(); block();
            const double want0 = *evaluateAzimuthKeys(p0.azKeys, u);
            const double want1 = *evaluateAzimuthKeys(p1.azKeys, u);
            check(g1test::nearAz(p.liveAzimuth(0), want0, 1e-3), "G1 Manual+azKeys liveAzimuth overrides s1az");
            check(g1test::nearAz(p.liveAzimuth(1), want1, 1e-3), "G1 coupled group follower evaluates azKeys at driver s1path");
        }
        // Check EnergyScene matches key rotation
        setP(p, "s1path", 0.5); block();
        const auto es = p.getEnergyScene(0);
        check(!es.sources.empty() && g1test::nearAz(es.sources[0].azimuthDeg, 90.0, 1e-2),
              "G1 getEnergyScene reflects active azKeys");

        // AlongPath / LookAt / disabled keep azKeys stored but inactive
        p0.orient = true;
        check(p.setMovementPath(0, p0), "G1 switch to AlongPath");
        setP(p, "s1path", 0.5); setP(p, "s1az", 15.0);
        block(); block();
        check(p.getMovementPath(0).azKeys.size() == 4 && g1test::nearAz(p.liveAzimuth(0), 15.0, 1e-2),
              "G1 AlongPath preserves azKeys stored while using tangent(0) + host azimuth(15)");

        p0.orient = false; p0.lookAt = true; p0.lookPoint = true; p0.lookPos = {4, 6, 1.5};
        check(p.setMovementPath(0, p0), "G1 switch to LookAt fixed point");
        block(); block();
        const double lookBear = std::atan2(6.0 - 2.0, 4.0 - 4.0) * 180.0 / verb::kPi;
        check(p.getMovementPath(0).azKeys.size() == 4 && g1test::nearAz(p.liveAzimuth(0), lookBear + 15.0, 1e-2),
              "G1 LookAt preserves azKeys stored while using look bearing + host azimuth");

        p0.lookAt = false; p0.enabled = false;
        check(p.setMovementPath(0, p0), "G1 disabled path preserves azKeys stored while inactive");
        setP(p, "s1az", -42.0);
        block(); block();
        check(p.getMovementPath(0).azKeys.size() == 4 && g1test::nearAz(p.liveAzimuth(0), -42.0, 1e-2),
              "G1 disabled path keeps azKeys inactive and uses host s1az");

        // Closed path with laps=2 gets distinct azimuths on lap 1 vs lap 2
        MovementPath loop;
        loop.points = {{2, 2, 1.5}, {5, 2, 1.5}, {5, 5, 1.5}, {2, 5, 1.5}};
        loop.closed = true; loop.laps = 2; loop.group = -1;
        loop.azKeys = {{25.0, 45.0, AzimuthEasing::Hold}, {75.0, -45.0, AzimuthEasing::Linear}};
        check(p.setMovementPath(0, loop), "G1 closed 2-lap path with keys");
        setP(p, "s1path", 0.25); block(); block();
        const double lap1Az = p.liveAzimuth(0);
        setP(p, "s1path", 0.75); block(); block();
        const double lap2Az = p.liveAzimuth(0);
        check(g1test::nearAz(lap1Az, 45.0, 1e-2) && g1test::nearAz(lap2Az, -45.0, 1e-2),
              "G1 keys across laps=2 give distinct azimuths at identical path geometry");
    }

    // 5. Hold jump without XYZ-tracker reset + audio continuity
    {
        auto runSweep = [&](bool withHoldJump, std::array<double, 3>& velAtJump, std::vector<float>& jumpOut) {
            auto proc = std::make_unique<VerbProcessor>();
            auto& p = *proc;
            p.setTailSettings({8000, false});
            p.setPlayConfigDetails(2, 2, 48000, 128);
            p.prepareToPlay(48000, 128);
            waitReady(p, 90);
            p.loadSceneText(p4test::kScene, "HoldJump", true);
            waitReady(p, 90);
            setP(p, "lx", 2.0); setP(p, "ly", 5.0); setP(p, "laz", 0.0);
            MovementPath sp;
            sp.points = {{1.0, 2.0, 1.5}, {3.0, 2.0, 1.5}};
            if (withHoldJump) sp.azKeys = {{0.0, 0.0, AzimuthEasing::Hold}, {50.0, 180.0, AzimuthEasing::Linear}};
            p.setMovementPath(0, sp);
            setP(p, "s1az", 0.0);
            if (withHoldJump) p.setSourcePattern(0, 0.0f);  // first check omni bit-identity of XYZ tracker

            juce::AudioBuffer<float> buf(2, 128); juce::MidiBuffer midi;
            int sampleIdx = 0;
            auto pumpTone = [&](double u) {
                setP(p, "s1path", u);
                buf.clear();
                for (int i = 0; i < 128; ++i)
                    buf.setSample(0, i, 0.2f * (float)std::sin(2.0 * verb::kPi * 440.0 * sampleIdx++ / 48000.0));
                stage2Process(p, buf, midi);
                return buf;
            };
            for (int k = 0; k < 20; ++k) pumpTone(0.30 + 0.01 * k);  // 0.30 .. 0.49
            auto outAtJump = pumpTone(0.50);                          // Hold step happens here!
            velAtJump = E9bAccess::hermiteVel(p, 0);
            jumpOut.assign(256, 0.f);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i) jumpOut[(size_t)c * 128 + (size_t)i] = outAtJump.getSample(c, i);
            return p.liveAzimuth(0);
        };
        std::array<double, 3> vNoJump{}, vHold{};
        std::vector<float> outNoJump, outHold;
        const double az0 = runSweep(false, vNoJump, outNoJump);
        const double az1 = runSweep(true, vHold, outHold);
        double maxVelDiff = 0;
        for (int k = 0; k < 3; ++k) maxVelDiff = std::max(maxVelDiff, std::abs(vNoJump[(size_t)k] - vHold[(size_t)k]));
        check(g1test::nearAz(az0, 0.0, 1e-3) && g1test::nearAz(az1, 180.0, 1e-3) && vHold[0] > 1e-5 && maxVelDiff < 1e-12,
              "G1 180-deg Hold jump snaps azimuth instantaneously without resetting Hermite XYZ velocity");

        // Now directional cardioid source sweeping continuously across Hold step: finite, no clipping spike
        auto procDir = std::make_unique<VerbProcessor>();
        auto& p = *procDir;
        p.setTailSettings({8000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "G1 cardioid Hold audio proc ready");
        p.loadSceneText(p4test::kScene, "HoldJumpDir", true);
        check(waitReady(p, 90), "G1 cardioid Hold audio room ready");
        p.setSourcePattern(0, 1.0f);
        MovementPath sp;
        sp.points = {{1.0, 2.0, 1.5}, {3.0, 2.0, 1.5}};
        sp.azKeys = {{0.0, 90.0, AzimuthEasing::Hold}, {50.0, -90.0, AzimuthEasing::Linear}};  // towards listener vs away
        p.setMovementPath(0, sp);
        juce::AudioBuffer<float> buf(2, 128); juce::MidiBuffer midi;
        int sampleIdx = 0; bool fin = true; float peak = 0.f;
        for (int k = 0; k <= 100; ++k) {
            setP(p, "s1path", k / 100.0);
            buf.clear();
            for (int i = 0; i < 128; ++i)
                buf.setSample(0, i, 0.25f * (float)std::sin(2.0 * verb::kPi * 440.0 * sampleIdx++ / 48000.0));
            stage2Process(p, buf, midi);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i) {
                const float s = buf.getSample(c, i);
                fin &= std::isfinite(s);
                peak = std::max(peak, std::abs(s));
            }
        }
        check(fin && peak > 1e-4f && peak < 2.0f, "G1 directional cardioid 180-deg Hold jump produces finite clickless audio");
    }

    // 6. Bake accuracy <= 1.0 dB vs traceTailEnergy (directional source + cardioid mics, Hold + EaseInOut)
    e19Accuracy("G1 Hold + EaseInOut directional source & listener mics", [](VerbProcessor& p) {
        p.setSourcePattern(0, 0.7f);
        auto out = p.getOutputCfg();
        out.micPattern = 1;  // cardioid stereo mics
        p.setOutputCfg(out);
        waitReady(p, 90);

        MovementPath src;
        src.points = {{2.0, 4.5, 1.5}, {5.0, 5.5, 1.5}};
        src.group = 0;
        src.azKeys = {
            {0.0, 170.0, AzimuthEasing::Hold},
            {45.0, -170.0, AzimuthEasing::EaseInOut},
            {85.0, -30.0, AzimuthEasing::Linear}
        };
        MovementPath lis;
        lis.points = {{3.0, 2.0, 1.5}, {4.0, 2.0, 1.5}};
        lis.group = 0;
        lis.azKeys = {
            {0.0, 45.0, AzimuthEasing::Hold},
            {55.0, 135.0, AzimuthEasing::EaseInOut},
            {100.0, 90.0, AzimuthEasing::Linear}
        };
        p.setMovementPath(0, src);
        p.setMovementPath(kListener, lis);
    });

    // 7. F3 0.2.2 regression (Manual without keys -> yawMatch.free=true) + P4 Undo / XML BakeCache invalidation & restore
    {
        auto proc = std::make_unique<VerbProcessor>();
        auto& p = *proc;
        p.setTailSettings({8000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "G1 F3/P4 proc ready");
        p.loadSceneText(p4test::kScene, "G1 corridor", true);
        check(waitReady(p, 90), "G1 F3/P4 corridor loads");
        {
            auto cfg = p.getSources(); cfg[0].pattern = 0.7; p.setSources(cfg);
            auto out = p.getOutputCfg(); out.micPattern = 1; p.setOutputCfg(out);
            check(waitReady(p, 90), "G1 F3/P4 directional source & mics applied");
        }

        // Part A: Manual WITHOUT keys keeps yawMatch.free == true; s1az rotation does NOT invalidate bake
        MovementPath plain;
        plain.points = {{1.0, 2.0, 1.5}, {3.0, 6.0, 1.5}};
        check(p.setMovementPath(0, plain), "G1 plain Manual path set");
        check(p.startPathBake(0) && p4test::waitBaked(p, 90), "G1 plain Manual path bakes");
        auto eng = E9bAccess::engine(p);
        auto tPlain = eng->bakedTableSnapshot();
        check(tPlain && !tPlain->movingPaths.empty() && tPlain->movingPaths[0].yawMatch.free,
              "G1 Manual without azKeys preserves yawMatch.free = true (0.2.2)");
        const uint64_t hashBeforeTurn = eng->bakeStats().hash;
        setP(p, "s1az", 95.0);
        p4test::pump(p, 6);
        check(p.pathBakeStatus()["state"].toString() == "baked" && eng->bakeStats().hash == hashBeforeTurn,
              "G1 Manual without azKeys stays baked after 95-deg s1az turn");

        // Part B: Manual WITH keys has yawMatch.free = false; key edit invalidates bake; Undo restores BakeCache
        check(p.getEditorSession().run(juce::JSON::parse(R"({"op":"path.setAzimuthKeys","object":0,"keys":[{"t":0,"az":0,"easing":"hold"},{"t":50,"az":180,"easing":"easeInOut"},{"t":100,"az":0}]})")).ok,
              "G1 add azKeys via command");
        check(p4test::waitState(p, "outdated", nullptr, 15), "G1 adding azKeys invalidates previous Manual bake");
        check(p.startPathBake(0) && p4test::waitBaked(p, 90), "G1 path with azKeys bakes");
        auto tKeys = eng->bakedTableSnapshot();
        check(tKeys && !tKeys->movingPaths.empty() && !tKeys->movingPaths[0].yawMatch.free && tKeys->discontinuities == 0,
              "G1 Manual with azKeys sets yawMatch.free = false and 0 discontinuities");
        const auto stBeforeEdit = eng->bakeStats();
        const juce::String xmlBaked = p4test::stateXml(p);
        const auto irBaked = p4test::impulse(p, "s1path", 0.6);

        check(p.getEditorSession().run(juce::JSON::parse(R"({"op":"path.setAzimuthKey","object":0,"index":1,"az":90})")).ok,
              "G1 edit azKey invalidates baked table");
        check(p4test::waitState(p, "outdated", nullptr, 15), "G1 edited azKey transitions bake status to outdated");
        check(p.getEditorSession().undo().ok && p4test::waitBaked(p, 15), "G1 Undo restores previous azKeys and BakeCache");
        check(eng->bakeStats().hash == stBeforeEdit.hash && eng->bakeStats().restored >= 1,
              "G1 Undo restores exact baked table hash without retracing");

        // Part C: XML BakeCache reload with keys (exact match) and mutated AzKey XML (rejected safely)
        {
            auto qStorage = std::make_unique<VerbProcessor>();
            auto& q = *qStorage;
            q.setTailSettings({8000, false});
            q.setPlayConfigDetails(2, 2, 48000, 128);
            q.prepareToPlay(48000, 128);
            const bool loadedQ = waitReady(q, 90) && p4test::loadStateXml(q, xmlBaked);
            const bool bakedQ = loadedQ && p4test::waitBaked(q, 15);
            if (!bakedQ) {
                const auto st = q.pathBakeStatus();
                auto qe = E9bAccess::engine(q);
                const auto qsigs = qe ? qe->bakeContentSignatures() : std::vector<uint64_t>{};
                std::printf("  [G1 DBG] reload bakedQ=0 state=%s reason=%s cache=%d qsig=%llx origSig=%llu\n",
                            st["state"].toString().toRawUTF8(), st["reason"].toString().toRawUTF8(),
                            qe ? qe->bakedCacheEntries() : -1,
                            qsigs.empty() ? 0ull : (unsigned long long)qsigs[0],
                            (unsigned long long)stBeforeEdit.hash);
            }
            check(bakedQ, "G1 project state with AzKey + BakeCache restores Baked");
            auto qe = E9bAccess::engine(q);
            if (qe) {
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000.0;
                while (juce::Time::getMillisecondCounterHiRes() < deadline
                       && !(qe->tailIsCurrent() && qe->motionStats().transitionsDone > 0))
                    p4test::pump(q, 8);
                p4test::pump(q, 8);
            }
            const auto irReloaded = p4test::impulse(q, "s1path", 0.6);
            const double dReload = p4test::maxDiff(irBaked, irReloaded);
            std::printf("  [G1] BakeCache reload maxDiff=%.3e qHash=%llu origHash=%llu restored=%d\n",
                        dReload, qe ? (unsigned long long)qe->bakeStats().hash : 0ull,
                        (unsigned long long)stBeforeEdit.hash, qe ? qe->bakeStats().restored : -1);
            check(dReload <= 1e-5 && qe && qe->bakeStats().hash == stBeforeEdit.hash && qe->bakeStats().restored == 1,
                  "G1 reloaded BakeCache hash & audio match within 1e-5");

            // Mutate an AzKey azimuth in saved XML while keeping old BakeCache blob -> must reject stale table!
            juce::String xmlMutated = xmlBaked.replace("az=\"180\"", "az=\"45\"");
            check(xmlMutated != xmlBaked, "G1 mutated AzKey XML constructed");
            auto rStorage = std::make_unique<VerbProcessor>();
            auto& r = *rStorage;
            r.setTailSettings({8000, false});
            r.setPlayConfigDetails(2, 2, 48000, 128);
            r.prepareToPlay(48000, 128);
            check(waitReady(r, 90) && p4test::loadStateXml(r, xmlMutated), "G1 mutated AzKey + old BakeCache loads safely");
            p4test::pump(r, 6);
            check(r.pathBakeStatus()["state"].toString() != "baked",
                  "G1 stale BakeCache from different azKeys is rejected");
        }
    }

    // 8. Bake time benchmark: normal scene vs 6 Hold keys on identical room & ray budget
    {
        auto measureBake = [&](bool sixHolds, size_t& nodesOut, int& discOut, double& msOut) {
            auto proc = std::make_unique<VerbProcessor>();
            auto& p = *proc;
            p.setTailSettings({12000, true});
            p.setPlayConfigDetails(2, 2, 48000, 128);
            p.prepareToPlay(48000, 128);
            waitReady(p, 90);
            p.loadSceneText(p4test::kScene, sixHolds ? "BenchHold6" : "BenchNormal", true);
            waitReady(p, 90);
            p.setSourcePattern(0, 0.7f);
            auto out = p.getOutputCfg(); out.micPattern = 1; p.setOutputCfg(out);
            waitReady(p, 90);
            MovementPath mp;
            mp.points = {{1.0, 1.5, 1.5}, {3.0, 4.0, 1.5}, {1.5, 6.5, 1.5}};
            mp.smooth = true;
            if (sixHolds) {
                mp.azKeys = {
                    {10.0, 0.0, AzimuthEasing::Hold},
                    {25.0, 180.0, AzimuthEasing::Hold},
                    {40.0, -90.0, AzimuthEasing::Hold},
                    {55.0, 90.0, AzimuthEasing::Hold},
                    {70.0, -45.0, AzimuthEasing::Hold},
                    {85.0, 135.0, AzimuthEasing::Hold},
                    {100.0, 0.0, AzimuthEasing::Linear}
                };
            }
            p.setMovementPath(0, mp);
            const auto t0 = std::chrono::steady_clock::now();
            const bool started = p.startPathBake(0);
            const bool done = started && p4test::waitBaked(p, 180);
            msOut = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            auto eng = E9bAccess::engine(p);
            auto tbl = eng->bakedTableSnapshot();
            nodesOut = tbl ? tbl->u.size() : 0;
            discOut = eng->bakeStats().discontinuities;
            return done;
        };
        size_t nodesNorm = 0, nodesHold6 = 0;
        int discNorm = -1, discHold6 = -1;
        double msNorm = 0.0, msHold6 = 0.0;
        const bool okNorm = measureBake(false, nodesNorm, discNorm, msNorm);
        const bool okHold6 = measureBake(true, nodesHold6, discHold6, msHold6);
        std::printf("  [G1 BENCH] normal: %.1f ms (nodes=%zu, disc=%d); 6 Hold keys: %.1f ms (nodes=%zu, disc=%d)\n",
                    msNorm, nodesNorm, discNorm, msHold6, nodesHold6, discHold6);
        check(okNorm && okHold6 && discNorm == 0 && discHold6 == 0 && nodesHold6 <= verb::kBakeNodeCap,
              "G1 6-Hold-keys bake completes cleanly within kBakeNodeCap with 0 discontinuities");
    }

    return fails == before ? 0 : 1;
}
