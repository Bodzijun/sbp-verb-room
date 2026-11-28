#pragma once
#include "PathShapes.h"

static int testV04aPathShapes() {
    std::printf("=== V04a: Closed Path Shapes & Endless Loop Tests ===\n");
    const int beforeFails = fails;

    // 1. Verify APVTS parameter count (283) + Mod Out 1..16 (indices 283..298) unchanged
    {
        VerbProcessor p;
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "V04a Default Room loads");
        bool hostOrder = p.getParameters().size() == 299;
        for (int index = 283; index < p.getParameters().size(); ++index) {
            const auto* param = dynamic_cast<const juce::AudioProcessorParameterWithID*>(p.getParameters()[index]);
            hostOrder &= param && param->paramID == "mo" + juce::String(index - 282);
        }
        check(hostOrder, "V04a APVTS count (283) + Mod Out 1..16 (indices 283..298) unchanged");
    }

    // 2. Geometry tests: Circle radial deviation (< 0.5 %), closure, Figure-8 crossing, Square/Rectangle/Triangle/Star/Spiral, room clamp
    {
        const double cx = 3.0, cy = 4.0, cz = 1.5, radius = 2.0;
        for (int nodes : {12, 16, 24, 32, 64}) {
            verbp::PathShapeParams sp;
            sp.kind = verbp::PathShapeKind::Circle;
            sp.cx = cx; sp.cy = cy; sp.z = cz;
            sp.radiusX = radius;
            sp.nodes = nodes;
            const auto mp = verbp::generateRawPathShape(sp);
            verbp::CompiledPath cp;
            cp.build(mp);
            double maxAbsErr = 0.0;
            for (const auto& q : cp.points) {
                const double r = std::hypot(q[0] - cx, q[1] - cy);
                maxAbsErr = std::max(maxAbsErr, std::abs(r - radius));
            }
            const double relPct = (maxAbsErr / radius) * 100.0;
            std::printf("  Circle N=%2d (%4zu compiled pts): max radial deviation = %.6f m (%.4f %%)\n",
                        nodes, cp.points.size(), maxAbsErr, relPct);
            check(relPct < 0.5, "V04a Circle radial deviation is strictly below 0.5 % of radius");
            if (nodes == 16) check(relPct < 0.06, "V04a Default 16-node Circle radial deviation is below 0.06 %");
            // Closure check
            check(verbp::pathDistance(cp.points.front(), cp.points.back()) < 1e-12,
                  "V04a Circle compiled path closes last->first with 0 gap");
            double yaw0 = 0.0, yawEnd = 0.0;
            const auto p0 = cp.evaluateSingleLapArcFraction(0.0005, &yaw0);
            const auto p1 = cp.evaluateSingleLapArcFraction(0.9995, &yawEnd);
            const double expectedStep = 0.001 * cp.length();
            check(std::abs(verbp::pathDistance(p0, p1) - expectedStep) < 0.02 * expectedStep
                  && std::abs(std::remainder(yaw0 - yawEnd, 360.0)) < 1.0,
                  "V04a Circle C1 position and tangent continuity across lap seam");
        }

        // Ellipse
        {
            verbp::PathShapeParams sp;
            sp.kind = verbp::PathShapeKind::Ellipse;
            sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5;
            sp.radiusX = 2.4; sp.radiusY = 1.2; sp.nodes = 16;
            const auto mp = verbp::generateRawPathShape(sp);
            verbp::CompiledPath cp; cp.build(mp);
            double maxEqErr = 0.0;
            for (const auto& q : cp.points) {
                const double norm = std::hypot((q[0] - sp.cx) / sp.radiusX, (q[1] - sp.cy) / sp.radiusY);
                maxEqErr = std::max(maxEqErr, std::abs(norm - 1.0) * 100.0);
            }
            std::printf("  Ellipse (2.4 x 1.2 m, N=16): max normalized radial deviation = %.4f %%\n", maxEqErr);
            check(mp.closed && mp.smooth && maxEqErr < 0.5, "V04a Ellipse stays within 0.5 % of true ellipse");
        }

        // Figure-8 (Lemniscate of Gerono)
        {
            verbp::PathShapeParams sp;
            sp.kind = verbp::PathShapeKind::Figure8;
            sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5;
            sp.radiusX = 2.0; sp.radiusY = 1.0; sp.nodes = 16;
            const auto mp = verbp::generateRawPathShape(sp);
            verbp::CompiledPath cp; cp.build(mp);
            double yawA = 0.0, yawB = 0.0;
            const auto crossA = cp.evaluateSingleLapArcFraction(0.0, &yawA);
            const auto crossB = cp.evaluateSingleLapArcFraction(0.5, &yawB);
            const double distA = std::hypot(crossA[0] - sp.cx, crossA[1] - sp.cy);
            const double distB = std::hypot(crossB[0] - sp.cx, crossB[1] - sp.cy);
            const double crossAngle = std::abs(std::remainder(yawA - yawB, 360.0));
            std::printf("  Figure-8 (Gerono 2.0 x 1.0 m, N=16): crossing error f=0 %.2e m, f=0.5 %.2e m, crossing angle %.1f deg\n",
                        distA, distB, crossAngle);
            check(distA < 1e-9 && distB < 1e-9 && crossAngle > 30.0 && crossAngle < 150.0,
                  "V04a Figure-8 crosses exact centre twice per lap with transverse tangents");
        }

        // Square & Rectangle corners + startPhaseDeg
        {
            verbp::PathShapeParams sp;
            sp.kind = verbp::PathShapeKind::Square;
            sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5; sp.radiusX = 1.5;
            const auto mp = verbp::generateRawPathShape(sp);
            verbp::CompiledPath cp; cp.build(mp);
            check(mp.closed && !mp.smooth && mp.points.size() == 4 && std::abs(cp.length() - 12.0) < 1e-9,
                  "V04a Square has 4 exact corner nodes and perimeter 8*R = 12.0 m");
            sp.startPhaseDeg = 45.0;
            const auto mpPhase = verbp::generateRawPathShape(sp);
            verbp::CompiledPath cpPhase; cpPhase.build(mpPhase);
            check(mpPhase.points.size() == 5 && std::abs(cpPhase.length() - 12.0) < 1e-9,
                  "V04a Square with mid-edge start phase preserves all 4 exact corners and perimeter");

            sp.kind = verbp::PathShapeKind::Rectangle;
            sp.radiusX = 2.0; sp.radiusY = 1.0; sp.startPhaseDeg = 0.0;
            const auto mpRect = verbp::generateRawPathShape(sp);
            verbp::CompiledPath cpRect; cpRect.build(mpRect);
            check(mpRect.points.size() == 4 && std::abs(cpRect.length() - 12.0) < 1e-9,
                  "V04a Rectangle has 4 exact corners and perimeter 4*(a+b) = 12.0 m");
        }

        // Triangle, Star (5-point), Spiral
        {
            verbp::PathShapeParams sp;
            sp.kind = verbp::PathShapeKind::Triangle;
            sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5; sp.radiusX = 1.8;
            const auto mpTri = verbp::generateRawPathShape(sp);
            verbp::CompiledPath cpTri; cpTri.build(mpTri);
            const double side = std::sqrt(3.0) * sp.radiusX;
            check(mpTri.points.size() == 3 && !mpTri.smooth && std::abs(cpTri.length() - 3.0 * side) < 1e-9,
                  "V04a Equilateral Triangle has 3 vertices at circumradius R");

            sp.kind = verbp::PathShapeKind::Star;
            sp.radiusX = 2.0; sp.radiusY = 0.8;
            const auto mpStar = verbp::generateRawPathShape(sp);
            bool radiiOk = mpStar.points.size() == 10 && !mpStar.smooth;
            for (size_t k = 0; k < mpStar.points.size(); ++k) {
                const double r = std::hypot(mpStar.points[k][0] - sp.cx, mpStar.points[k][1] - sp.cy);
                radiiOk &= std::abs(r - ((k % 2 == 0) ? sp.radiusX : sp.radiusY)) < 1e-9;
            }
            check(radiiOk, "V04a 5-point Star alternates outer and inner radii across 10 vertices");

            sp.kind = verbp::PathShapeKind::Spiral;
            sp.radiusX = 2.0; sp.radiusY = 0.5; sp.nodes = 24; sp.spiralTurns = 2; sp.spiralOutIn = true;
            const auto mpSpiral = verbp::generateRawPathShape(sp);
            verbp::CompiledPath cpSpiral; cpSpiral.build(mpSpiral);
            const auto pStart = cpSpiral.evaluateSingleLapArcFraction(0.0);
            const auto pMid = cpSpiral.evaluateSingleLapArcFraction(0.5);
            const double rStart = std::hypot(pStart[0] - sp.cx, pStart[1] - sp.cy);
            const double rMid = std::hypot(pMid[0] - sp.cx, pMid[1] - sp.cy);
            check(mpSpiral.closed && mpSpiral.smooth && mpSpiral.points.size() == 24
                  && std::abs(rStart - 0.5) < 1e-6 && std::abs(rMid - 2.0) < 0.05,
                  "V04a Closed Spiral expands from inner radius to outer radius and returns smoothly");
        }

        // Direction (CW vs CCW) and Rotation
        {
            verbp::PathShapeParams ccw;
            ccw.kind = verbp::PathShapeKind::Circle; ccw.cx = 3.0; ccw.cy = 4.0; ccw.z = 1.5; ccw.radiusX = 1.5;
            ccw.clockwise = false;
            verbp::PathShapeParams cw = ccw;
            cw.clockwise = true;
            verbp::CompiledPath cpCcw, cpCw;
            cpCcw.build(verbp::generateRawPathShape(ccw));
            cpCw.build(verbp::generateRawPathShape(cw));
            double yawCcw = 0.0, yawCw = 0.0;
            cpCcw.evaluateSingleLapArcFraction(0.0, &yawCcw);
            cpCw.evaluateSingleLapArcFraction(0.0, &yawCw);
            check(std::abs(std::abs(std::remainder(yawCcw - yawCw, 360.0)) - 180.0) < 2.0,
                  "V04a CW vs CCW reverses traversal heading by 180 deg");
        }

        // Room clamp test in Default Room (6 x 8 x 3 m)
        {
            VerbProcessor p;
            p.setPlayConfigDetails(2, 2, 48000, 128);
            p.prepareToPlay(48000, 128);
            check(waitReady(p, 90), "V04a room clamp processor ready");
            const auto sc = p.getDisplayScene();
            const verb::CeilingPlan plan(sc);

            verbp::PathShapeParams huge;
            huge.kind = verbp::PathShapeKind::Circle;
            huge.cx = 3.0; huge.cy = 4.0; huge.z = 25.0; // above ceiling
            huge.radiusX = 15.0; // outside 6x8 walls
            huge.nodes = 16;
            const auto rawFit = verbp::generatePathShapeForRoom(huge, &sc, false);
            const auto clampedFit = verbp::generatePathShapeForRoom(huge, &sc, true);
            check(rawFit.outsideRoom && !verbp::isCompiledPathInsideRoom(sc, plan, rawFit.path),
                  "V04a oversized shape is detected as outside room");
            check(clampedFit.outsideRoom && clampedFit.clamped
                  && verbp::isCompiledPathInsideRoom(sc, plan, clampedFit.path)
                  && clampedFit.fittedParams.z <= sc.height - 0.05 && clampedFit.fittedParams.z >= 0.05,
                  "V04a clamped shape fits strictly inside room walls and ceiling");
            // Verify clamped circle is still a true circle within 0.5 %
            verbp::CompiledPath cpClamped; cpClamped.build(clampedFit.path);
            double maxErr = 0.0;
            for (const auto& q : cpClamped.points) {
                const double r = std::hypot(q[0] - clampedFit.fittedParams.cx, q[1] - clampedFit.fittedParams.cy);
                maxErr = std::max(maxErr, std::abs(r - clampedFit.fittedParams.radiusX));
            }
            const double clampedRelPct = (maxErr / clampedFit.fittedParams.radiusX) * 100.0;
            std::printf("  Clamped Circle radius = %.4f m, radial deviation = %.4f %%\n",
                        clampedFit.fittedParams.radiusX, clampedRelPct);
            check(clampedRelPct < 0.1, "V04a room-clamped Circle remains a true circle (< 0.1 % deviation)");
        }
    }

    // 3. Endless loop: phase exactness after 1 h and 10 h, lap boundary continuity, offline render determinism, 0 allocations
    {
        verbp::PathShapeParams sp;
        sp.kind = verbp::PathShapeKind::Circle;
        sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5; sp.radiusX = 2.0; sp.nodes = 16;
        auto mp = verbp::generateRawPathShape(sp);
        mp.endless = true;
        mp.endlessPeriodS = 8.0;
        verbp::CompiledPath cp;
        cp.build(mp);

        const double sr = 48000.0;
        const int64_t sampleT1 = 59256; // t = 1.2345 s
        const int64_t periodSamples = (int64_t)(mp.endlessPeriodS * sr); // 384000
        const int64_t sample1h = sampleT1 + 450 * periodSamples;         // t + 450 * 8 s = 1 h + 1.2345 s
        const int64_t sample10h = sampleT1 + 4500 * periodSamples;       // t + 4500 * 8 s = 10 h + 1.2345 s

        const double ph1 = verbp::endlessTransportPhase(sampleT1, sr, mp.endlessPeriodS);
        const double ph1h = verbp::endlessTransportPhase(sample1h, sr, mp.endlessPeriodS);
        const double ph10h = verbp::endlessTransportPhase(sample10h, sr, mp.endlessPeriodS);
        const auto pos1 = cp.evaluateEndless(0.25, ph1);
        const auto pos1h = cp.evaluateEndless(0.25, ph1h);
        const auto pos10h = cp.evaluateEndless(0.25, ph10h);
        const double phaseDiff1h = std::abs(ph1 - ph1h);
        const double phaseDiff10h = std::abs(ph1 - ph10h);
        const double posDiff1h = verbp::pathDistance(pos1, pos1h);
        std::printf("  Endless 1 h phase drift (T=8 s): %.3e cycles, 10 h drift: %.3e cycles, 3D pose diff: %.3e m\n",
                    phaseDiff1h, phaseDiff10h, posDiff1h);
        check(phaseDiff1h == 0.0 && phaseDiff10h == 0.0 && posDiff1h == 0.0,
              "V04a Endless transport phase has zero drift after 1 hour and 10 hours");

        // Also test non-integer period T = 3.7 s over 1000 laps (3700 s > 1 h)
        const double nonIntT = 3.7;
        const int64_t sA = 12345;
        const int64_t sB = sA + (int64_t)(1000.0 * nonIntT * sr); // 1000 * 177600 = 177600000 samples
        const double phA = verbp::endlessTransportPhase(sA, sr, nonIntT);
        const double phB = verbp::endlessTransportPhase(sB, sr, nonIntT);
        std::printf("  Endless >1 h phase drift (T=3.7 s, 1000 laps): %.3e cycles\n", std::abs(phA - phB));
        check(std::abs(phA - phB) < 1e-15, "V04a Endless non-integer period T=3.7 s has < 1e-15 phase drift over 1000 laps");

        // Continuity across every lap boundary (5 laps, checking 128-sample control steps)
        const double stepU = 128.0 / (sr * mp.endlessPeriodS);
        const double expectedDist = stepU * cp.length();
        double maxBoundaryDistDiff = 0.0, maxBoundaryVelDiff = 0.0;
        auto prevP = cp.evaluateEndless(0.0, 0.0);
        auto prevP2 = cp.evaluateEndless(0.0, 1.0 - stepU);
        double prevStep = verbp::pathDistance(prevP2, prevP);
        const int totalSteps = (int)(5.0 * mp.endlessPeriodS * sr / 128.0);
        for (int k = 1; k <= totalSteps; ++k) {
            const double ph = verbp::endlessTransportPhase((int64_t)k * 128, sr, mp.endlessPeriodS);
            const auto curP = cp.evaluateEndless(0.0, ph);
            const double d = verbp::pathDistance(prevP, curP);
            maxBoundaryDistDiff = std::max(maxBoundaryDistDiff, std::abs(d - expectedDist));
            maxBoundaryVelDiff = std::max(maxBoundaryVelDiff, std::abs(d - prevStep));
            prevStep = d;
            prevP = curP;
        }
        std::printf("  Endless 5-lap step uniformity: expected %.6f m, max step deviation %.6e m, max step-to-step change %.6e m\n",
                    expectedDist, maxBoundaryDistDiff, maxBoundaryVelDiff);
        check(maxBoundaryDistDiff < 1e-4 && maxBoundaryVelDiff < 1e-4,
              "V04a Endless position and velocity are continuous across every lap boundary");

        // All 8 shapes through processor & engine pose at multiple transport positions + determinism of two offline renders
        auto renderOffline = [&](bool endlessOn, std::vector<float>& outL, std::vector<float>& outR,
                                 double* p99UsOut = nullptr, double* meanUsOut = nullptr,
                                 bool creativeMode = false, double dopplerStrength = 1.0, bool motionTail = false) {
            VerbProcessor p;
            p.setPlayConfigDetails(2, 2, 48000, 128);
            p.prepareToPlay(48000, 128);
            if (!waitReady(p, 90)) return false;
            if (motionTail) {
                p.setMotionTailEnabled(true);
                if (!waitReady(p, 90)) return false;
                for (int k = 0; k < 200 && !p.isWebTailCurrent(); ++k) p4test::pump(p, 2);
            }
            if (creativeMode) {
                setP(p, "mode", 1.0);
                setP(p, "doppler", dopplerStrength);
            }
            const auto sc = p.getDisplayScene();
            verbp::PathShapeParams cParam;
            cParam.kind = verbp::PathShapeKind::Circle;
            cParam.cx = 3.0; cParam.cy = 4.0; cParam.z = 1.5; cParam.radiusX = 1.8; cParam.nodes = 16;
            auto cPath = verbp::generatePathShapeForRoom(cParam, &sc, true).path;
            cPath.orient = true; // Along path
            cPath.endless = endlessOn;
            cPath.endlessPeriodS = 4.0;
            if (!p.setMovementPath(0, cPath)) return false;
            setP(p, "s1path", 0.25); // Start at 25 % phase

            E12PlayHead head;
            head.playing = true;
            head.samples = 0;
            p.setPlayHead(&head);

            juce::AudioBuffer<float> audio(2, 128);
            juce::MidiBuffer midi;
            const auto allocBefore = stage2AudioAllocations;
            std::vector<double> us;
            double sumUs = 0.0;
            for (int b = 0; b < 600; ++b) { // 1.6 s (> 0.4 lap, crosses lap boundary from 0.25)
                audio.clear();
                for (int i = 0; i < 128; ++i) {
                    const auto n = head.samples + i;
                    audio.setSample(0, i, 0.2f * (float)std::sin(n * (2.0 * verb::kPi * 220.0 / 48000.0)));
                }
                const auto t0 = std::chrono::steady_clock::now();
                stage2Process(p, audio, midi);
                const double elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                us.push_back(elapsed);
                sumUs += elapsed;
                for (int i = 0; i < 128; ++i) {
                    outL.push_back(audio.getSample(0, i));
                    outR.push_back(audio.getSample(1, i));
                }
                head.samples += 128;
            }
            const auto allocs = stage2AudioAllocations - allocBefore;
            if (meanUsOut && !us.empty()) *meanUsOut = sumUs / (double)us.size();
            if (p99UsOut && !us.empty()) {
                std::sort(us.begin(), us.end());
                *p99UsOut = us[(size_t)(us.size() * 99 / 100)];
            }
            p.setPlayHead(nullptr);
            return allocs == 0;
        };

        std::vector<float> rOffL, rOffR, r1L, r1R, r2L, r2R, rDop0L, rDop0R, rDop1L, rDop1R, rMotL, rMotR;
        double p99Off = 0.0, meanOff = 0.0, p99On = 0.0, meanOn = 0.0;
        const bool okOff = renderOffline(false, rOffL, rOffR, &p99Off, &meanOff);
        const bool ok1 = renderOffline(true, r1L, r1R, &p99On, &meanOn);
        const bool ok2 = renderOffline(true, r2L, r2R);
        const bool okDop0 = renderOffline(true, rDop0L, rDop0R, nullptr, nullptr, true, 0.0, false);
        const bool okDop1 = renderOffline(true, rDop1L, rDop1R, nullptr, nullptr, true, 1.0, false);
        const bool okMot = renderOffline(true, rMotL, rMotR, nullptr, nullptr, false, 1.0, true);
        check(okOff && ok1 && ok2 && okDop0 && okDop1 && okMot,
              "V04a Endless offline renders (Physical, Doppler=0/1, Motion) allocate 0 bytes on audio thread");
        float maxRenderDiff = 0.f, maxDopDiff = 0.f, maxMotAmp = 0.f;
        for (size_t i = 0; i < r1L.size() && i < r2L.size(); ++i) {
            maxRenderDiff = std::max(maxRenderDiff, std::abs(r1L[i] - r2L[i]));
            maxRenderDiff = std::max(maxRenderDiff, std::abs(r1R[i] - r2R[i]));
            maxDopDiff = std::max(maxDopDiff, std::abs(rDop0L[i] - rDop1L[i]));
            maxMotAmp = std::max({maxMotAmp, std::abs(rMotL[i]), std::abs(rMotR[i])});
        }
        std::printf("  Endless two offline renders (600 blocks): max diff = %.9g FS; CPU mean %.2f->%.2f us, p99 %.2f->%.2f us\n",
                    maxRenderDiff, meanOff, meanOn, p99Off, p99On);
        std::printf("  Endless Doppler=0 vs Doppler=1 max delta: %.6f FS; Motion+Endless peak: %.6f FS\n",
                    maxDopDiff, maxMotAmp);
        check(r1L.size() == 600 * 128 && maxRenderDiff == 0.f,
              "V04a two offline renders with Endless are bit-identical (0 FS difference)");
        check(maxDopDiff > 1e-4f && std::isfinite(maxMotAmp) && maxMotAmp > 1e-4f,
              "V04a Endless works with Doppler (0 vs 1 differs) and Motion mode (finite non-zero output)");

        // Test all 8 shapes through engine pose at transport t = 0, 2 s, 4 s, 6 s
        {
            VerbProcessor p;
            p.setPlayConfigDetails(2, 2, 48000, 128);
            p.prepareToPlay(48000, 128);
            check(waitReady(p, 90), "V04a shape engine pose processor ready");
            setP(p, "s1az", 0.0f);
            const auto sc = p.getDisplayScene();
            E12PlayHead head;
            head.playing = true;
            p.setPlayHead(&head);
            juce::AudioBuffer<float> audio(2, 128);
            juce::MidiBuffer midi;
            bool allShapesEngineOk = true;
            for (int sk = 0; sk < 8; ++sk) {
                verbp::PathShapeParams spk;
                spk.kind = (verbp::PathShapeKind)sk;
                spk.cx = 3.0; spk.cy = 4.0; spk.z = 1.5; spk.radiusX = 1.5; spk.radiusY = 0.9;
                auto mpShape = verbp::generatePathShapeForRoom(spk, &sc, true).path;
                mpShape.orient = true;
                mpShape.endless = true;
                mpShape.endlessPeriodS = 8.0;
                if (!p.setMovementPath(0, mpShape)) { allShapesEngineOk = false; break; }
                verbp::CompiledPath cpShape; cpShape.build(mpShape);
                for (int64_t sec : {0, 2, 4, 6}) {
                    head.samples = sec * 48000;
                    audio.clear();
                    stage2Process(p, audio, midi);
                    double expYaw = 0.0;
                    cpShape.evaluateEndless(0.0, verbp::endlessTransportPhase(head.samples, 48000.0, 8.0), &expYaw);
                    const double liveYaw = p.liveAzimuth(0);
                    const double yawErr = std::abs(std::remainder(liveYaw - expYaw, 360.0));
                    if (!std::isfinite(liveYaw) || yawErr > 1e-3) allShapesEngineOk = false;
                }
            }
            p.setPlayHead(nullptr);
            check(allShapesEngineOk, "V04a all 8 shapes evaluate exact Endless poses & tangents on the engine across transport positions");
        }
    }

    // 4. Endless interaction with Path LFO, Azimuth keys, Mod routes, and Bake
    {
        VerbProcessor p;
        p.setTailSettings({8000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        p.loadSceneText(p4test::kScene, "V04a corridor", true);
        check(waitReady(p, 90), "V04a interaction corridor loads");

        const auto sc = p.getDisplayScene();
        verbp::PathShapeParams sp;
        sp.kind = verbp::PathShapeKind::Circle;
        sp.cx = 2.0; sp.cy = 4.0; sp.z = 1.5; sp.radiusX = 1.2; sp.nodes = 16;
        auto mp = verbp::generatePathShapeForRoom(sp, &sc, true).path;
        mp.endless = false;
        mp.endlessPeriodS = 4.0;
        check(p.setMovementPath(0, mp), "V04a set closed circle path on source 0");

        // Bake first with Endless OFF
        if (p.startPathBake(0) && p4test::waitBaked(p, 120)) {
            const auto eng = E9bAccess::engine(p);
            const auto hash = eng->bakeStats().hash;
            check(hash != 0, "V04a initial closed circle path bakes");

            // Turn Endless ON -> new Bake blocked, existing Bake bypassed (0 lookups), BakeCache kept
            mp.endless = true;
            check(p.setMovementPath(0, mp), "V04a enable Endless on baked path");
            check(!p.startPathBake(0), "V04a startPathBake is blocked while Endless is active");
            const auto lookupsBefore = eng->bakeStats().lookups;
            E12PlayHead head; head.playing = true; head.samples = 0;
            p.setPlayHead(&head);
            juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
            for (int b = 0; b < 60; ++b) {
                audio.clear();
                stage2Process(p, audio, midi);
                head.samples += 128;
            }
            check(eng->bakeStats().lookups == lookupsBefore,
                  "V04a Endless bypasses baked table lookups on the audio thread");

            // Test Azimuth keys + Path LFO + Modulation route (pathPosition -> Mod Out 1) with Endless ON
            mp.azKeys = {{0.0, -90.0, verbp::AzimuthEasing::Linear},
                         {50.0, 90.0, verbp::AzimuthEasing::Linear},
                         {100.0, -90.0, verbp::AzimuthEasing::Linear}};
            mp.lfo.enabled = true;
            mp.lfo.shape = verbp::LfoShape::Sine;
            mp.lfo.rateHz = 2.0;
            mp.lfo.depth = 0.1;
            check(p.setMovementPath(0, mp), "V04a set Azimuth keys and Path LFO on Endless path");

            verbp::ModulationRoute route;
            route.enabled = true;
            route.object = 0;
            route.driver = verbp::ModDriver::PathPosition;
            route.target = verbp::ModTarget::None;
            route.targetObject = -1;
            route.hostOut = 1;
            route.attackMs = 0.0;
            route.releaseMs = 0.0;
            route.curve.inputMin = 0.0; route.curve.inputMax = 1.0;
            route.curve.outputMin = 0.0; route.curve.outputMax = 1.0;
            check(p.setModulationRoutes({route}), "V04a configure PathPosition -> Mod Out 1 route");
            setP(p, "mode", 1.0); // Creative mode

            float minAz = 180.f, maxAz = -180.f;
            float minMod = 1.f, maxMod = 0.f;
            const auto alloc0 = stage2AudioAllocations;
            for (int b = 0; b < 1500; ++b) { // 4.0 s = 1 full endless period
                audio.clear();
                stage2Process(p, audio, midi);
                minAz = std::min(minAz, p.liveAzimuth(0));
                maxAz = std::max(maxAz, p.liveAzimuth(0));
                const float mo = p.modOutParameter(0)->getValue();
                minMod = std::min(minMod, mo);
                maxMod = std::max(maxMod, mo);
                head.samples += 128;
            }
            check(stage2AudioAllocations - alloc0 == 0, "V04a Endless + LFO + AzKeys + ModRoute allocates 0 bytes");
            check(maxAz - minAz > 120.f && maxMod - minMod > 0.7f,
                  "V04a Endless drives Azimuth keys and PathPosition modulation across full range");
            p.setPlayHead(nullptr);

            // Turn Endless OFF -> baked table restored and lookups resume
            mp.endless = false;
            mp.azKeys.clear();
            mp.lfo = {};
            p.setModulationRoutes({});
            check(p.setMovementPath(0, mp), "V04a disable Endless");
            for (int i = 0; i < 60; ++i) p4test::pump(p, 1);
            const auto lookupsRestoredBefore = eng->bakeStats().lookups;
            h2test::pumpBlocks(p, 120, 128);
            check(eng->bakeStats().lookups > lookupsRestoredBefore,
                  "V04a turning Endless off restores stashed Bake table and resumes lookups");
        } else {
            check(false, "V04a corridor initial Bake succeeded");
        }
    }

    // 5. State round-trip, .verbroom export/import, legacy fixtures, impulse check after reload, and Classic editor
    {
        VerbProcessor p;
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "V04a round-trip processor ready");

        const auto sc = p.getDisplayScene();
        verbp::PathShapeParams sp;
        sp.kind = verbp::PathShapeKind::Figure8;
        sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5; sp.radiusX = 1.6; sp.radiusY = 0.8; sp.nodes = 16;
        auto mp = verbp::generatePathShapeForRoom(sp, &sc, true).path;
        mp.endless = false;
        check(p.setMovementPath(0, mp), "V04a set Figure-8 with Endless OFF");

        // Verify XML omits endless & endlessPeriodS when OFF and default 8.0 s
        juce::MemoryBlock stateOff;
        p.getStateInformation(stateOff);
        auto xmlOff = VerbProcessor::getXmlFromBinary(stateOff.getData(), (int)stateOff.getSize());
        auto* pathXmlOff = xmlOff ? xmlOff->getChildByName("MovementPaths")->getChildByName("Path") : nullptr;
        check(pathXmlOff && !pathXmlOff->hasAttribute("endless") && !pathXmlOff->hasAttribute("endlessPeriodS"),
              "V04a state omits endless and endlessPeriodS attributes when Endless is off");

        // Turn Endless ON with custom period 6.5 s
        mp.endless = true;
        mp.endlessPeriodS = 6.5;
        check(p.setMovementPath(0, mp), "V04a enable Endless with period 6.5 s");
        juce::MemoryBlock stateOn;
        p.getStateInformation(stateOn);
        const auto presetOn = p.getRoomPresetText();

        VerbProcessor q;
        q.setPlayConfigDetails(2, 2, 48000, 128);
        q.prepareToPlay(48000, 128);
        q.setStateInformation(stateOn.getData(), (int)stateOn.getSize());
        check(waitReady(q, 90), "V04a state with Endless ON reloads");
        const auto restoredPath = q.getMovementPath(0);
        check(restoredPath.closed && restoredPath.smooth && restoredPath.endless
              && std::abs(restoredPath.endlessPeriodS - 6.5) < 1e-9 && restoredPath.points == mp.points,
              "V04a project state round-trip preserves shape points, endless=true, and endlessPeriodS=6.5");

        // Impulse check after reload on Default Room
        const auto irDefault = h2test::captureImpulse(q, 2, 0, 120, 128);
        const double eDefault = h2test::channelEnergy(irDefault[0]) + h2test::channelEnergy(irDefault[1]);
        check(std::isfinite(eDefault) && eDefault > 1e-6, "V04a reloaded Default Room with Endless shape emits finite impulse response");

        // .verbroom export -> import (key present and key absent)
        juce::String err;
        VerbProcessor r;
        r.setPlayConfigDetails(2, 2, 48000, 128);
        r.prepareToPlay(48000, 128);
        check(waitReady(r, 90), "V04a preset import processor ready");
        check(r.loadRoomPresetText(presetOn, err) && waitReady(r, 90)
              && r.getMovementPath(0).endless && std::abs(r.getMovementPath(0).endlessPeriodS - 6.5) < 1e-9,
              "V04a .verbroom round-trip preserves endless and endlessPeriodS");

        auto xmlPresetNoKey = juce::parseXML(presetOn);
        if (auto* mpXml = xmlPresetNoKey->getChildByName("VerbState")->getChildByName("MovementPaths")) {
            if (auto* pXml = mpXml->getChildByName("Path")) {
                pXml->removeAttribute("endless");
                pXml->removeAttribute("endlessPeriodS");
            }
        }
        check(r.loadRoomPresetText(xmlPresetNoKey->toString(), err) && waitReady(r, 90)
              && !r.getMovementPath(0).endless && std::abs(r.getMovementPath(0).endlessPeriodS - 8.0) < 1e-9,
              "V04a .verbroom without endless attributes loads with endless=false and period=8.0");

        // Legacy fixtures (legacy.state + legacy.verbroom) and VSL preset round-trip + impulse check
        {
            const auto legacyStateFile = juce::File::getCurrentWorkingDirectory().getChildFile("plugin/fixtures/stage2/legacy.state");
            if (legacyStateFile.existsAsFile()) {
                juce::MemoryBlock legData;
                legacyStateFile.loadFileAsData(legData);
                VerbProcessor legProc;
                legProc.setPlayConfigDetails(2, 2, 48000, 128);
                legProc.prepareToPlay(48000, 128);
                legProc.setStateInformation(legData.getData(), (int)legData.getSize());
                check(waitReady(legProc, 90) && !legProc.getMovementPath(0).endless
                      && std::abs(legProc.getMovementPath(0).endlessPeriodS - 8.0) < 1e-9,
                      "V04a legacy.state loads cleanly with endless=false and default period");
            }
            const auto legacyPresetFile = juce::File::getCurrentWorkingDirectory().getChildFile("plugin/fixtures/stage2/legacy.verbroom");
            if (legacyPresetFile.existsAsFile()) {
                VerbProcessor legP;
                legP.setPlayConfigDetails(2, 2, 48000, 128);
                legP.prepareToPlay(48000, 128);
                juce::String legErr;
                check(legP.loadRoomPresetText(legacyPresetFile.loadFileAsString(), legErr) && waitReady(legP, 90)
                      && !legP.getMovementPath(0).endless,
                      "V04a legacy.verbroom loads cleanly with endless=false");
            }
            const auto vslFile = juce::File::getCurrentWorkingDirectory().getChildFile("VSL.verbroom");
            if (vslFile.existsAsFile()) {
                VerbProcessor vslP;
                vslP.setPlayConfigDetails(64, 2, 48000, 128);
                juce::AudioProcessor::BusesLayout vslLayout;
                vslLayout.inputBuses.add(juce::AudioChannelSet::discreteChannels(64));
                vslLayout.outputBuses.add(juce::AudioChannelSet::stereo());
                vslP.setBusesLayout(vslLayout);
                vslP.prepareToPlay(48000, 128);
                juce::String vslErr;
                check(vslP.loadRoomPresetText(vslFile.loadFileAsString(), vslErr) && waitReady(vslP, 120),
                      "V04a VSL preset loads");
                const auto vslSc = vslP.getDisplayScene();
                verbp::PathShapeParams vslSp;
                vslSp.kind = verbp::PathShapeKind::Circle;
                vslSp.cx = vslSc.listenerPos.x; vslSp.cy = vslSc.listenerPos.y; vslSp.z = 1.5;
                vslSp.radiusX = 2.0; vslSp.nodes = 16;
                auto vslPath = verbp::generatePathShapeForRoom(vslSp, &vslSc, true).path;
                vslPath.endless = true;
                vslPath.endlessPeriodS = 8.0;
                check(vslP.setMovementPath(0, vslPath), "V04a VSL preset accepts Circle + Endless path");
                juce::MemoryBlock vslSaved;
                vslP.getStateInformation(vslSaved);
                VerbProcessor vslReloaded;
                vslReloaded.setPlayConfigDetails(64, 2, 48000, 128);
                vslReloaded.setBusesLayout(vslLayout);
                vslReloaded.prepareToPlay(48000, 128);
                vslReloaded.setStateInformation(vslSaved.getData(), (int)vslSaved.getSize());
                check(waitReady(vslReloaded, 120) && vslReloaded.getMovementPath(0).endless
                      && vslReloaded.getMovementPath(0).points == vslPath.points,
                      "V04a VSL preset state round-trip preserves Circle + Endless");
                const auto irVsl = h2test::captureImpulse(vslReloaded, 64, 0, 120, 128);
                const double eVsl = h2test::channelEnergy(irVsl[0]) + h2test::channelEnergy(irVsl[1]);
                check(std::isfinite(eVsl) && eVsl > 1e-6,
                      "V04a reloaded VSL preset with Endless circle emits finite impulse response");
            }
        }

        // Classic editor (VerbEditor) shape menu + radius + Apply + Endless checkbox + Undo
        {
            auto editor = std::make_unique<VerbEditor>(r);
            editor->setSize(900, 600);
            editor->showPage(0, 0);
            juce::ComboBox* shapeBox = nullptr;
            juce::TextButton* applyBtn = nullptr;
            juce::ToggleButton* endlessBtn = nullptr;
            for (int i = 0; i < editor->getNumChildComponents(); ++i) {
                auto* ch = editor->getChildComponent(i);
                if (auto* cb = dynamic_cast<juce::ComboBox*>(ch)) {
                    if (cb->getItemText(0) == "Circle" && cb->getItemText(2) == "Figure-8") shapeBox = cb;
                } else if (auto* tb = dynamic_cast<juce::TextButton*>(ch)) {
                    if (tb->getButtonText() == "Apply" && tb->getTooltip().contains("shape")) applyBtn = tb;
                } else if (auto* tgb = dynamic_cast<juce::ToggleButton*>(ch)) {
                    if (tgb->getButtonText() == "Endless") endlessBtn = tgb;
                }
            }
            check(shapeBox && applyBtn && endlessBtn && shapeBox->isVisible() && applyBtn->isVisible() && endlessBtn->isVisible()
                  && editor->getLocalBounds().contains(shapeBox->getBounds())
                  && editor->getLocalBounds().contains(endlessBtn->getBounds()),
                  "V04a Classic editor shows Shape menu, Apply button, and Endless checkbox inside 900x600 window");
            if (shapeBox && applyBtn && endlessBtn) {
                endlessBtn->setToggleState(true, juce::sendNotificationSync);
                shapeBox->setSelectedId(4, juce::sendNotificationSync); // Square
                applyBtn->triggerClick();
                juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
                const auto sqPath = r.getMovementPath(0);
                check(sqPath.closed && !sqPath.smooth && sqPath.points.size() == 4 && sqPath.endless,
                      "V04a Classic editor Apply generates closed Square with Endless on");
                check(r.getEditorSession().undo().ok && r.getMovementPath(0).points.size() == 16,
                      "V04a Classic editor Apply is undoable in one EditorSession step");
            }
        }
    }

    // 6. V04a-2: Spiral start/end height Z, Figure-8 default centre offset (>= 1.5 m from listener),
    //            live shape parameter updates while audio is playing (0 allocations, RT-safe), and transport telemetry
    {
        // (a) Spiral start/end Z linear ramp + backward compatibility when zEnd is omitted
        verbp::PathShapeParams spFlat;
        spFlat.kind = verbp::PathShapeKind::Spiral;
        spFlat.cx = 3.0; spFlat.cy = 4.0; spFlat.z = 1.5;
        spFlat.radiusX = 2.0; spFlat.radiusY = 0.5; spFlat.nodes = 24; spFlat.spiralTurns = 2;
        const auto mpFlat = verbp::generateRawPathShape(spFlat);
        bool flatOk = mpFlat.points.size() == 24;
        for (const auto& pt : mpFlat.points) flatOk &= std::abs(pt[2] - 1.5) < 1e-12;
        check(flatOk, "V04a-2 Spiral with omitted zEnd keeps flat height z=1.5 across all points (backward compatible)");

        verbp::PathShapeParams spRamp = spFlat;
        spRamp.z = 0.6;
        spRamp.zEnd = 2.4;
        const auto mpRamp = verbp::generateRawPathShape(spRamp);
        bool rampLinearOk = mpRamp.points.size() == 24
            && std::abs(mpRamp.points.front()[2] - 0.6) < 1e-12
            && std::abs(mpRamp.points.back()[2] - 2.4) < 1e-12;
        for (size_t k = 0; k < mpRamp.points.size(); ++k) {
            const double expectedZ = 0.6 + (2.4 - 0.6) * ((double)k / 23.0);
            rampLinearOk &= std::abs(mpRamp.points[k][2] - expectedZ) < 1e-12;
            rampLinearOk &= std::abs(mpRamp.points[k][0] - mpFlat.points[k][0]) < 1e-12;
            rampLinearOk &= std::abs(mpRamp.points[k][1] - mpFlat.points[k][1]) < 1e-12;
        }
        check(rampLinearOk, "V04a-2 Spiral interpolates Z linearly from first point (zStart=0.6) to last point (zEnd=2.4) while preserving XY turns/radius");

        // Room clamp for Spiral start/end Z + Figure-8 default centre offset + state/.verbroom round-trip + impulse check
        VerbProcessor p;
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "V04a-2 processor ready");
        const auto sc = p.getDisplayScene();

        verbp::PathShapeParams spOverZ = spRamp;
        spOverZ.z = -2.0;
        spOverZ.zEnd = 25.0;
        const auto clampedSpiral = verbp::generatePathShapeForRoom(spOverZ, &sc, true);
        check(clampedSpiral.clamped
              && clampedSpiral.path.points.front()[2] >= 0.05 - 1e-9
              && clampedSpiral.path.points.back()[2] <= sc.height - 0.05 + 1e-9,
              "V04a-2 room clamp bounds both Spiral start Z and end Z inside room floor/ceiling");

        // Figure-8 default centre offset when initial centre coincides with listener
        const double lx = p.apvts.getRawParameterValue("lx")->load();
        const double ly = p.apvts.getRawParameterValue("ly")->load();
        verbp::PathShapeParams fig8OnLis;
        fig8OnLis.kind = verbp::PathShapeKind::Figure8;
        fig8OnLis.cx = lx;
        fig8OnLis.cy = ly;
        fig8OnLis.z = 1.5;
        fig8OnLis.radiusX = 1.8;
        fig8OnLis.radiusY = 0.9;
        fig8OnLis.nodes = 16;
        const auto fig8Offset = verbp::offsetFigure8DefaultCentre(fig8OnLis, lx, ly, &sc, 1.5);
        const auto fig8Path = verbp::generatePathShapeForRoom(fig8Offset, &sc, true).path;
        const double crossDist0 = std::hypot(fig8Path.points[0][0] - lx, fig8Path.points[0][1] - ly);
        const double crossDistHalf = std::hypot(fig8Path.points[8][0] - lx, fig8Path.points[8][1] - ly);
        const verb::CeilingPlan plan(sc);
        std::printf("  V04a-2 Figure-8 offset centre=(%.3f, %.3f), listener=(%.3f, %.3f), crossing dist=%.3f m\n",
                    fig8Offset.cx, fig8Offset.cy, lx, ly, crossDist0);
        check(std::hypot(fig8Offset.cx - lx, fig8Offset.cy - ly) >= 1.5 - 1e-4
              && crossDist0 >= 1.5 - 1e-4 && crossDistHalf >= 1.5 - 1e-4
              && verbp::isCompiledPathInsideRoom(sc, plan, fig8Path),
              "V04a-2 Figure-8 default centre is offset >= 1.5 m from listener and clamped inside room");

        // Save/load state and .verbroom round-trip for 3D rising Spiral + impulse check
        auto spiral3D = verbp::generatePathShapeForRoom(spRamp, &sc, true).path;
        spiral3D.endless = true;
        spiral3D.endlessPeriodS = 6.0;
        check(p.setMovementPath(0, spiral3D), "V04a-2 set 3D Spiral on source 0");
        juce::MemoryBlock spiralState;
        p.getStateInformation(spiralState);
        const auto spiralPreset = p.getRoomPresetText();

        VerbProcessor pReload;
        pReload.setPlayConfigDetails(2, 2, 48000, 128);
        pReload.prepareToPlay(48000, 128);
        pReload.setStateInformation(spiralState.getData(), (int)spiralState.getSize());
        check(waitReady(pReload, 90), "V04a-2 reloaded 3D Spiral processor ready");
        const auto loadedSpiral = pReload.getMovementPath(0);
        check(loadedSpiral.points == spiral3D.points
              && std::abs(loadedSpiral.points.front()[2] - 0.6) < 1e-9
              && std::abs(loadedSpiral.points.back()[2] - 2.4) < 1e-9,
              "V04a-2 project state round-trip preserves exact 3D Spiral start/end Z points");
        juce::String presetErr;
        VerbProcessor pFromPreset;
        pFromPreset.setPlayConfigDetails(2, 2, 48000, 128);
        pFromPreset.prepareToPlay(48000, 128);
        check(waitReady(pFromPreset, 90)
              && pFromPreset.loadRoomPresetText(spiralPreset, presetErr)
              && waitReady(pFromPreset, 90)
              && pFromPreset.getMovementPath(0).points == spiral3D.points,
              "V04a-2 .verbroom round-trip preserves exact 3D Spiral start/end Z points");
        const auto irSpiral = h2test::captureImpulse(pReload, 2, 0, 120, 128);
        const double eSpiral = h2test::channelEnergy(irSpiral[0]) + h2test::channelEnergy(irSpiral[1]);
        check(std::isfinite(eSpiral) && eSpiral > 1e-6,
              "V04a-2 reloaded Default Room with 3D rising Spiral emits finite impulse response");

        // (b) Live shape parameter drag (64 steps) while audio is playing: 0 audio allocations, no clicks/glitches, transport telemetry live
        E12PlayHead head;
        head.playing = true;
        head.samples = 24000;
        p.setPlayHead(&head);
        juce::AudioBuffer<float> audio(2, 128);
        juce::MidiBuffer midi;
        uint64_t audioAllocsDuringLiveDrag = 0;
        bool allFinite = true;
        float maxStepDelta = 0.f;
        float prevL = 0.f, prevR = 0.f;
        bool havePrevSample = false;
        for (int step = 0; step < 64; ++step) {
            verbp::PathShapeParams liveSp;
            liveSp.kind = verbp::PathShapeKind::Circle;
            liveSp.cx = 3.0; liveSp.cy = 4.0; liveSp.z = 1.5;
            liveSp.radiusX = 0.8 + (1.4 * (double)step / 63.0);
            liveSp.nodes = 16;
            auto livePath = verbp::generatePathShapeForRoom(liveSp, &sc, true).path;
            livePath.endless = true;
            livePath.endlessPeriodS = 8.0;
            p.setMovementPath(0, livePath);

            audio.clear();
            for (int i = 0; i < 128; ++i) {
                const auto n = head.samples + i;
                audio.setSample(0, i, 0.15f * (float)std::sin(n * (2.0 * verb::kPi * 220.0 / 48000.0)));
            }
            const auto a0 = stage2AudioAllocations;
            stage2Process(p, audio, midi);
            audioAllocsDuringLiveDrag += (stage2AudioAllocations - a0);
            for (int i = 0; i < 128; ++i) {
                const float sL = audio.getSample(0, i);
                const float sR = audio.getSample(1, i);
                if (!std::isfinite(sL) || !std::isfinite(sR)) allFinite = false;
                if (havePrevSample) {
                    maxStepDelta = std::max({maxStepDelta, std::abs(sL - prevL), std::abs(sR - prevR)});
                }
                prevL = sL; prevR = sR; havePrevSample = true;
            }
            head.samples += 128;
        }
        const auto trTel = p.webTransportTelemetry();
        p.setPlayHead(nullptr);
        std::printf("  V04a-2 live 64-step shape drag during audio: audio allocs=%llu, maxSampleStep=%.5f, transportPlaying=%d, samplePos=%lld\n",
                    (unsigned long long)audioAllocsDuringLiveDrag, maxStepDelta, (int)trTel.playing, (long long)trTel.samplePos);
        check(audioAllocsDuringLiveDrag == 0 && allFinite && maxStepDelta < 0.25f,
              "V04a-2 64-step live shape drag during audio playback has 0 audio-thread allocations and no clicks/glitches");
        check(p.webAnyEndlessPathActive() && trTel.playing && trTel.samplePos == 24000 + 63 * 128,
              "V04a-2 processor publishes lock-free Endless transport telemetry for WebView marker");
    }

    return fails == beforeFails ? 0 : 1;
}
