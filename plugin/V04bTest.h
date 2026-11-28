#pragma once
#include "PathShapes.h"

static int testV04bPathWander() {
    std::printf("=== V04b: Path Wander ('Fly Mode') Tests ===\n");
    const int beforeFails = fails;

    // 1. Verify APVTS parameter count (283) + Mod Out 1..16 (indices 283..298) unchanged
    {
        auto pPtr = std::make_unique<VerbProcessor>();
        auto& p = *pPtr;
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "V04b Default Room loads");
        bool hostOrder = p.getParameters().size() == 299;
        for (int index = 283; index < p.getParameters().size(); ++index) {
            const auto* param = dynamic_cast<const juce::AudioProcessorParameterWithID*>(p.getParameters()[index]);
            hostOrder &= param && param->paramID == "mo" + juce::String(index - 282);
        }
        check(hostOrder, "V04b APVTS count (283) + Mod Out 1..16 (indices 283..298) unchanged");
    }

    // 2. Statistics of the offset: mean ≈ 0, RMS ≈ amplitude (per axis), correlation time,
    //    continuity at lap closure (max jump), pin-ends tapering, horizOnly, room clamp
    {
        const double targetH = 0.50;
        const double targetV = 0.30;
        for (double smooth : {0.0, 0.5, 0.8, 1.0}) {
            double sumH = 0.0, sumV = 0.0, sumH2 = 0.0, sumV2 = 0.0, sumHV = 0.0;
            const int N = 100000;
            const double dt = 0.037; // incommensurate step across 3700 node intervals
            verbp::PathWander w;
            w.enabled = true;
            w.ampHorizM = targetH;
            w.ampVertM = targetV;
            w.rateHz = 1.0;
            w.smoothness = smooth;
            w.pinEnds = false;
            w.horizOnly = false;
            w.seed = 42;
            for (int i = 0; i < N; ++i) {
                double h = 0.0, v = 0.0;
                w.evaluateLocalOffsets(i * dt, 0.0, 0.5, true, h, v);
                sumH += h;
                sumV += v;
                sumH2 += h * h;
                sumV2 += v * v;
                sumHV += h * v;
            }
            const double meanH = sumH / N;
            const double meanV = sumV / N;
            const double rmsH = std::sqrt(sumH2 / N);
            const double rmsV = std::sqrt(sumV2 / N);
            const double corrHV = (sumHV / N) / std::max(1e-12, rmsH * rmsV);
            std::printf("  Wander stats (smooth=%.1f, N=%d): meanH=%+.5f m, meanV=%+.5f m, rmsH=%.4f m (target %.2f), rmsV=%.4f m (target %.2f), crossCorr=%.4f\n",
                        smooth, N, meanH, meanV, rmsH, targetH, rmsV, targetV, corrHV);
            check(std::abs(meanH) < 0.015 * targetH && std::abs(meanV) < 0.015 * targetV,
                  "V04b Wander offset mean is within 1.5 % of zero on both axes");
            check(std::abs(rmsH - targetH) < 0.025 * targetH && std::abs(rmsV - targetV) < 0.025 * targetV,
                  "V04b Wander offset RMS matches target amplitude within 2.5 % across all smoothness settings");
            check(std::abs(corrHV) < 0.03,
                  "V04b Wander horizontal and vertical axes are statistically uncorrelated");
        }

        // Correlation time scaling with rateHz (0.25, 0.5, 1.0, 2.0, 4.0 Hz)
        {
            bool corrOk = true;
            double prevTauHalf = 1e9;
            for (double rate : {0.25, 0.5, 1.0, 2.0, 4.0}) {
                verbp::PathWander w;
                w.enabled = true;
                w.ampHorizM = 1.0;
                w.rateHz = rate;
                w.smoothness = 0.8;
                w.seed = 777;
                const double expectedPeriod = 1.0 / rate;
                // Measure autocorrelation R(tau) at tau = 0.5/rate, 1.0/rate, 2.0/rate and find half-correlation time R(tau_half) = 0.5
                auto autocorrAt = [&](double tauSec) {
                    const int M = 20000;
                    const double step = expectedPeriod * 0.073;
                    double s00 = 0.0, s01 = 0.0;
                    for (int i = 0; i < M; ++i) {
                        const double t = i * step;
                        double h0 = 0.0, v0 = 0.0, h1 = 0.0, v1 = 0.0;
                        w.evaluateLocalOffsets(t, 0.0, 0.5, true, h0, v0);
                        w.evaluateLocalOffsets(t + tauSec, 0.0, 0.5, true, h1, v1);
                        s00 += h0 * h0;
                        s01 += h0 * h1;
                    }
                    return s01 / std::max(1e-12, s00);
                };
                const double rHalfNode = autocorrAt(0.5 * expectedPeriod);
                const double rOneNode  = autocorrAt(1.0 * expectedPeriod);
                const double rTwoNodes = autocorrAt(2.0 * expectedPeriod);
                // Binary search tau where R(tau) == 0.5
                double lo = 0.0, hi = 1.5 * expectedPeriod;
                for (int it = 0; it < 20; ++it) {
                    const double mid = 0.5 * (lo + hi);
                    if (autocorrAt(mid) > 0.5) lo = mid; else hi = mid;
                }
                const double tauHalf = 0.5 * (lo + hi);
                std::printf("  Wander correlation (rate=%.2f Hz, 1/rate=%.2f s): R(0.5T)=%.3f, R(1.0T)=%.3f, R(2.0T)=%.3f, tau_0.5=%.3f s (%.3f * T)\n",
                            rate, expectedPeriod, rHalfNode, rOneNode, rTwoNodes, tauHalf, tauHalf * rate);
                corrOk &= (rHalfNode > 0.65 && rOneNode > 0.30 && rOneNode < 0.50 && std::abs(rTwoNodes) < 0.06);
                corrOk &= (std::abs(tauHalf * rate - 0.886) < 0.04) && (tauHalf < prevTauHalf);
                prevTauHalf = tauHalf;
            }
            check(corrOk, "V04b Wander correlation time scales as 1/rateHz and decorrelates within 2 node periods");
        }

        // Continuity at lap closure (max jump across 5 laps on a closed Circle, Endless and non-Endless)
        {
            verbp::PathShapeParams sp;
            sp.kind = verbp::PathShapeKind::Circle;
            sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5; sp.radiusX = 2.0; sp.nodes = 16;
            auto mp = verbp::generateRawPathShape(sp);
            mp.endless = true;
            mp.endlessPeriodS = 8.0;
            mp.wander.enabled = true;
            mp.wander.ampHorizM = 0.50;
            mp.wander.ampVertM = 0.30;
            mp.wander.rateHz = 0.73; // non-integer multiple of lap frequency -> every lap is different
            mp.wander.smoothness = 0.8;
            mp.wander.seed = 12345;
            verbp::CompiledPath cp;
            cp.build(mp);

            const double sr = 48000.0;
            const int totalSteps = (int)(5.0 * mp.endlessPeriodS * sr / 128.0); // 15000 steps (5 laps)
            auto evalDisplacedAtStep = [&](int k) -> verbp::PathPoint {
                const int64_t samp = (int64_t)k * 128;
                const double tSec = (double)samp / sr;
                const double ph = verbp::endlessTransportPhase(samp, sr, mp.endlessPeriodS);
                const double uEff = cp.endlessEffectiveU(0.0, ph);
                double yaw = 0.0;
                const auto q = cp.evaluateSingleLapArcFraction(uEff, &yaw);
                double dx = 0.0, dy = 0.0, dz = 0.0;
                cp.wander.evaluateWorldDelta(tSec, 0.0, uEff, true, yaw, dx, dy, dz);
                return {q[0] + dx, q[1] + dy, q[2] + dz};
            };
            double maxStepOverall = 0.0, maxSeamStep = 0.0, maxAccelStep = 0.0;
            auto pPrev = evalDisplacedAtStep(0);
            double prevDist = 0.0;
            const int stepsPerLap = (int)(mp.endlessPeriodS * sr / 128.0); // 3000
            for (int k = 1; k <= totalSteps; ++k) {
                const auto pCur = evalDisplacedAtStep(k);
                const double d = verbp::pathDistance(pPrev, pCur);
                maxStepOverall = std::max(maxStepOverall, d);
                if (k % stepsPerLap == 0 || k % stepsPerLap == 1)
                    maxSeamStep = std::max(maxSeamStep, d);
                if (k > 1)
                    maxAccelStep = std::max(maxAccelStep, std::abs(d - prevDist));
                prevDist = d;
                pPrev = pCur;
            }
            // Verify each lap has a distinct trajectory at u = 0 (seam crossings k = 0, 3000, 6000, 9000, 12000)
            const auto lap0Pose = evalDisplacedAtStep(0);
            const auto lap1Pose = evalDisplacedAtStep(stepsPerLap);
            const auto lap2Pose = evalDisplacedAtStep(2 * stepsPerLap);
            const double lap01Diff = verbp::pathDistance(lap0Pose, lap1Pose);
            const double lap12Diff = verbp::pathDistance(lap1Pose, lap2Pose);
            std::printf("  Closed Circle + Wander 5-lap continuity: max step overall = %.6f m, max seam step = %.6f m, max step-to-step delta = %.6e m\n",
                        maxStepOverall, maxSeamStep, maxAccelStep);
            std::printf("  Lap-to-lap variation at u=0: lap0->lap1 = %.4f m, lap1->lap2 = %.4f m\n",
                        lap01Diff, lap12Diff);
            check(maxSeamStep <= maxStepOverall + 1e-6 && maxSeamStep < 0.015 && maxStepOverall < 0.015 && maxAccelStep < 5e-3,
                  "V04b Closed path with Wander has continuous interpolation across every lap closure seam (no jump)");
            check(lap01Diff > 0.05 && lap12Diff > 0.05,
                  "V04b Each lap on a closed path with Wander follows a distinct trajectory");
        }

        // Open path Pin ends tapering & horizOnly
        {
            verbp::PathWander w;
            w.enabled = true;
            w.ampHorizM = 0.8;
            w.ampVertM = 0.4;
            w.rateHz = 1.0;
            w.smoothness = 0.5;
            w.pinEnds = true;
            w.horizOnly = false;
            w.seed = 99;
            double h0 = 1.0, v0 = 1.0, h1 = 1.0, v1 = 1.0, hMid = 0.0, vMid = 0.0;
            w.evaluateLocalOffsets(1.23, 0.0, 0.0, false, h0, v0);
            w.evaluateLocalOffsets(1.23, 0.0, 1.0, false, h1, v1);
            w.evaluateLocalOffsets(1.23, 0.0, 0.5, false, hMid, vMid);
            check(h0 == 0.0 && v0 == 0.0 && h1 == 0.0 && v1 == 0.0 && (std::abs(hMid) > 1e-3 || std::abs(vMid) > 1e-3),
                  "V04b Pin ends tapers open-path offset to exact zero at u=0 and u=1 while active in interior");
            check(w.endEnvelope(0.0, false) == 0.0 && w.endEnvelope(1.0, false) == 0.0
                  && std::abs(w.endEnvelope(0.075, false) - 0.5) < 1e-9
                  && w.endEnvelope(0.15, false) == 1.0 && w.endEnvelope(0.85, false) == 1.0,
                  "V04b Pin ends smoothstep envelope ramps over 15 % at both ends");
            w.pinEnds = false;
            w.evaluateLocalOffsets(1.23, 0.0, 0.0, false, h0, v0);
            check(std::abs(h0) > 1e-3 || std::abs(v0) > 1e-3,
                  "V04b Disabling Pin ends allows non-zero offset at open-path endpoints");
            w.horizOnly = true;
            bool allZZero = true;
            for (int i = 0; i < 100; ++i) {
                double h = 0.0, v = 0.0;
                w.evaluateLocalOffsets(i * 0.1, 0.0, 0.5, true, h, v);
                allZZero &= (v == 0.0);
            }
            check(allZZero && w.ampVertM == 0.4,
                  "V04b horizOnly zeroes vertical Z displacement while preserving ampVertM");
        }

        // Room clamp test (walls, floor, ceiling, and interior partition wall)
        {
            verbp::WallSnapshot ws;
            // 6 x 8 x 3 m room + interior partition wall at y = 4.0 from x = 0.0 to x = 4.5
            ws.zTop = 3.0;
            ws.count = 5;
            ws.segs[0] = {0.0, 0.0, 6.0, 0.0, 0.0, 3.0};
            ws.segs[1] = {6.0, 0.0, 6.0, 8.0, 0.0, 3.0};
            ws.segs[2] = {6.0, 8.0, 0.0, 8.0, 0.0, 3.0};
            ws.segs[3] = {0.0, 8.0, 0.0, 0.0, 0.0, 3.0};
            ws.segs[4] = {0.0, 4.0, 4.5, 4.0, 0.0, 3.0}; // interior partition
            ws.planPolyCount = 1;
            ws.planPolys[0].vertCount = 4;
            ws.planPolys[0].vx = {0.0, 6.0, 6.0, 0.0};
            ws.planPolys[0].vy = {0.0, 0.0, 8.0, 8.0};

            verbp::PathWander w;
            w.enabled = true;
            w.ampHorizM = 5.0; // huge amplitude that would easily cross walls and ceiling
            w.ampVertM = 4.0;
            w.rateHz = 2.0;
            w.smoothness = 0.5;
            w.seed = 31415;

            bool allClampedInside = true;
            int clampedCount = 0;
            const double bx = 2.0, by = 3.5, bz = 1.5; // 0.5 m below partition wall at y=4.0
            for (int i = 0; i < 500; ++i) {
                double dx = 0.0, dy = 0.0, dz = 0.0;
                w.evaluateWorldDelta(i * 0.05, 0.0, 0.5, true, 0.0, dx, dy, dz);
                double wx = bx + dx, wy = by + dy, wz = bz + dz;
                const bool rawOutside = !verbp::isPointInsideWallSnapshot(ws, wx, wy, wz)
                                     || verbp::segmentCrossesWallSnapshot(ws, bx, by, wx, wy, 0.5 * (bz + wz));
                verbp::clampWanderPoseToRoom(ws, bx, by, bz, wx, wy, wz);
                if (rawOutside) ++clampedCount;
                const bool okIn = verbp::isPointInsideWallSnapshot(ws, wx, wy, wz);
                const bool crossesPartition = verbp::segmentCrossesWallSnapshot(ws, bx, by, wx, wy, 0.5 * (bz + wz));
                if (!okIn || crossesPartition || wy >= 4.0 - 0.079 || wz < 0.05 - 1e-9 || wz > 2.95 + 1e-9)
                    allClampedInside = false;
            }
            std::printf("  Room clamp test: %d/500 oversized wander offsets clamped strictly inside walls/partition/floor/ceiling\n",
                        clampedCount);
            check(clampedCount > 200 && allClampedInside,
                  "V04b clampWanderPoseToRoom keeps all displaced poses inside walls, ceiling, floor, and prevents wall crossing");
        }
    }

    // 3. Determinism, Seek invariance, Click metric, Zero allocations, CPU delta, Motion ON/OFF, 1 & 8 sources
    {
        // Seek invariance: continuous render vs seek to random sample positions
        {
            auto setupSeekProc = [&](VerbProcessor& p, bool endlessMode) {
                p.setPlayConfigDetails(2, 2, 48000, 128);
                p.prepareToPlay(48000, 128);
                if (!waitReady(p, 90)) return false;
                setP(p, "mode", 1.0); // Creative
                const auto sc = p.getDisplayScene();
                verbp::PathShapeParams sp;
                sp.kind = verbp::PathShapeKind::Circle;
                sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5; sp.radiusX = 2.0; sp.nodes = 16;
                auto mp = verbp::generatePathShapeForRoom(sp, &sc, true).path;
                mp.orient = false;
                mp.lookAt = true;
                mp.lookTarget = verbp::kListener; // LookAt listener -> liveAzimuth reflects exact displaced XY pose!
                mp.endless = endlessMode;
                mp.endlessPeriodS = 8.0;
                mp.wander.enabled = true;
                mp.wander.ampHorizM = 0.5;
                mp.wander.ampVertM = 0.3;
                mp.wander.rateHz = 1.25;
                mp.wander.smoothness = 0.8;
                mp.wander.seed = 2026;
                if (!p.setMovementPath(0, mp)) return false;
                setP(p, "s1path", 0.35f);
                // Also route X, Y, Z to Mod Out 1, 2, 3 with 0 ms attack/release to read exact displaced 3D pose on audio thread
                std::vector<verbp::ModulationRoute> routes(3);
                const verbp::ModDriver drivers[3] = {verbp::ModDriver::X, verbp::ModDriver::Y, verbp::ModDriver::Z};
                for (int k = 0; k < 3; ++k) {
                    routes[(size_t)k].enabled = true;
                    routes[(size_t)k].object = 0;
                    routes[(size_t)k].driver = drivers[k];
                    routes[(size_t)k].target = verbp::ModTarget::None;
                    routes[(size_t)k].targetObject = -1;
                    routes[(size_t)k].hostOut = k + 1;
                    routes[(size_t)k].attackMs = 0.0;
                    routes[(size_t)k].releaseMs = 0.0;
                    routes[(size_t)k].curve.inputMin = 0.0;
                    routes[(size_t)k].curve.inputMax = 10.0;
                    routes[(size_t)k].curve.outputMin = 0.0;
                    routes[(size_t)k].curve.outputMax = 1.0;
                }
                return p.setModulationRoutes(routes);
            };

            bool seekAllOk = true;
            double maxSeekPoseDiff = 0.0;
            for (bool endlessMode : {true, false}) {
                auto pContPtr = std::make_unique<VerbProcessor>();
                auto pSeekPtr = std::make_unique<VerbProcessor>();
                auto& pCont = *pContPtr;
                auto& pSeek = *pSeekPtr;
                check(setupSeekProc(pCont, endlessMode) && setupSeekProc(pSeek, endlessMode),
                      endlessMode ? "V04b seek test processors ready (Endless)" : "V04b seek test processors ready (Position)");
                E12PlayHead hCont, hSeek;
                hCont.playing = true; hCont.samples = 0;
                hSeek.playing = true; hSeek.samples = 0;
                pCont.setPlayHead(&hCont);
                pSeek.setPlayHead(&hSeek);
                juce::AudioBuffer<float> bCont(2, 128), bSeek(2, 128);
                juce::MidiBuffer midi;
                // Warm up 1 block at sample 0 so creativeBlend = 1.0
                bCont.clear(); stage2Process(pCont, bCont, midi);
                bSeek.clear(); stage2Process(pSeek, bSeek, midi);

                const int targetBlocks[] = {75, 290, 613, 1140};
                int curBlock = 1;
                for (int tb : targetBlocks) {
                    while (curBlock <= tb) {
                        hCont.samples = (int64_t)curBlock * 128;
                        if (!endlessMode) setP(pCont, "s1path", (float)std::fmod(0.15 + curBlock * 0.0023, 1.0));
                        bCont.clear();
                        stage2Process(pCont, bCont, midi);
                        ++curBlock;
                    }
                    // Now seek pSeek directly to tb * 128 from wherever it was
                    hSeek.samples = (int64_t)tb * 128;
                    if (!endlessMode) setP(pSeek, "s1path", (float)std::fmod(0.15 + tb * 0.0023, 1.0));
                    bSeek.clear();
                    stage2Process(pSeek, bSeek, midi);

                    VerbProcessor::ModLiveSample sampCont[3] = {}, sampSeek[3] = {};
                    int nCont = 0, nSeek = 0;
                    pCont.webModTelemetry(sampCont, 3, nCont);
                    pSeek.webModTelemetry(sampSeek, 3, nSeek);
                    for (int k = 0; k < 3; ++k) {
                        maxSeekPoseDiff = std::max(maxSeekPoseDiff, (double)std::abs(sampCont[k].raw - sampSeek[k].raw));
                    }
                    const double azDiff = std::abs(pCont.liveAzimuth(0) - pSeek.liveAzimuth(0));
                    maxSeekPoseDiff = std::max(maxSeekPoseDiff, azDiff);
                    if (maxSeekPoseDiff > 1e-7) seekAllOk = false;
                }
                pCont.setPlayHead(nullptr);
                pSeek.setPlayHead(nullptr);
            }
            std::printf("  Seek invariance (continuous vs random seek across Endless & non-Endless): max pose/azimuth diff = %.3e\n",
                        maxSeekPoseDiff);
            check(seekAllOk && maxSeekPoseDiff == 0.0,
                  "V04b Seek to random transport position produces exact same displaced pose as continuous render (0.0 diff)");
        }

        // Audio rendering: Circle (3 m clamped to Default Room, Endless, 8 s/lap), Wander H=0.5 m, V=0.3 m
        // Measure:
        //  - Physical mode Wander ON vs Wander OFF bit-identical (0.0 FS)
        //  - Seed reproducibility: two Creative renders with seed=101 bit-identical (0.0 FS); seed=202 differs
        //  - Click metric (max |d2y| and d2Peak/d2Rms) for 1 source & 8 sources, Motion OFF & Motion ON
        //  - Zero allocations on audio thread, CPU mean & p99, no 'Rebuilding...' in Motion ON
        struct RenderMetrics {
            double maxD2 = 0.0;
            double d2Crest = 0.0;
            double peak = 0.0;
            double meanUs = 0.0;
            double p99Us = 0.0;
            int64_t allocs = 0;
            bool motionStayedReady = true;
        };

        auto runAudioScenario = [&](int numSources, bool creativeMode, bool wanderOn, uint32_t seed,
                                    bool motionOn, std::vector<float>& outL, std::vector<float>& outR,
                                    RenderMetrics& mOut, int numBlocks = 750) -> bool {
            auto pPtr = std::make_unique<VerbProcessor>();
            auto& p = *pPtr;
            p.setPlayConfigDetails(numSources, 2, 48000, 128);
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add(numSources == 2 ? juce::AudioChannelSet::stereo()
                                                  : juce::AudioChannelSet::discreteChannels(numSources));
            layout.outputBuses.add(juce::AudioChannelSet::stereo());
            p.setBusesLayout(layout);
            p.prepareToPlay(48000, 128);
            if (!waitReady(p, 90)) return false;

            if (numSources > 1) {
                std::vector<verbp::SourceCfg> srcs((size_t)numSources);
                for (int s = 0; s < numSources; ++s) {
                    srcs[(size_t)s].name = "Source " + std::to_string(s + 1);
                    srcs[(size_t)s].format = 0; // Mono
                    srcs[(size_t)s].channels = 1;
                    srcs[(size_t)s].firstIn = s;
                }
                p.setSources(srcs);
                if (!waitReady(p, 90)) return false;
            }
            if (motionOn) {
                p.setMotionTailEnabled(true);
                if (!waitReady(p, 90)) return false;
                juce::AudioBuffer<float> pumpBuf(std::max(numSources, 2), 128);
                juce::MidiBuffer pumpMidi;
                for (int k = 0; k < 250 && !p.isWebTailCurrent(); ++k) {
                    pumpBuf.clear();
                    stage2Process(p, pumpBuf, pumpMidi);
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
                }
            }
            setP(p, "mode", creativeMode ? 1.0f : 0.0f);

            const auto sc = p.getDisplayScene();
            for (int s = 0; s < numSources; ++s) {
                verbp::PathShapeParams sp;
                sp.kind = verbp::PathShapeKind::Circle;
                sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5;
                sp.radiusX = 3.0; // Clamped to room (2.92 m in Default Room 6x8x3)
                sp.nodes = 16;
                sp.startPhaseDeg = (360.0 * s) / std::max(1, numSources);
                auto mp = verbp::generatePathShapeForRoom(sp, &sc, true).path;
                mp.orient = true;
                mp.endless = true;
                mp.endlessPeriodS = 8.0;
                mp.wander.enabled = wanderOn;
                mp.wander.ampHorizM = 0.50;
                mp.wander.ampVertM = 0.30;
                mp.wander.rateHz = 0.75;
                mp.wander.smoothness = 0.8;
                mp.wander.seed = seed + (uint32_t)(s * 17);
                if (!p.setMovementPath(s, mp)) return false;
                setP(p, verbp::paramId(s, 4), numSources > 1 ? -6.0f : 0.0f);
            }

            E12PlayHead head;
            head.playing = true;
            head.samples = 0;
            p.setPlayHead(&head);

            juce::AudioBuffer<float> audio(std::max(numSources, 2), 128);
            juce::MidiBuffer midi;
            // Warm up 20 blocks so filters/gains settle before measuring
            for (int w = 0; w < 20; ++w) {
                audio.clear();
                for (int s = 0; s < numSources; ++s) {
                    const double freq = 180.0 + 35.0 * s;
                    for (int i = 0; i < 128; ++i) {
                        const auto n = head.samples + i;
                        audio.setSample(s, i, 0.15f * (float)std::sin(n * (2.0 * verb::kPi * freq / 48000.0)));
                    }
                }
                stage2Process(p, audio, midi);
                head.samples += 128;
            }

            outL.clear(); outR.clear();
            outL.reserve((size_t)numBlocks * 128);
            outR.reserve((size_t)numBlocks * 128);
            std::vector<double> us;
            us.reserve((size_t)numBlocks);
            double sumUs = 0.0;
            mOut = {};
            const auto alloc0 = stage2AudioAllocations;
            for (int b = 0; b < numBlocks; ++b) {
                audio.clear();
                for (int s = 0; s < numSources; ++s) {
                    const double freq = 180.0 + 35.0 * s;
                    for (int i = 0; i < 128; ++i) {
                        const auto n = head.samples + i;
                        audio.setSample(s, i, 0.15f * (float)std::sin(n * (2.0 * verb::kPi * freq / 48000.0)));
                    }
                }
                const auto t0 = std::chrono::steady_clock::now();
                stage2Process(p, audio, midi);
                const double el = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                us.push_back(el);
                sumUs += el;
                if (motionOn && p.getLoadState() != 2) mOut.motionStayedReady = false;
                for (int i = 0; i < 128; ++i) {
                    outL.push_back(audio.getSample(0, i));
                    outR.push_back(audio.getSample(1, i));
                }
                head.samples += 128;
            }
            mOut.allocs = stage2AudioAllocations - alloc0;
            mOut.meanUs = sumUs / std::max<size_t>(1, us.size());
            std::sort(us.begin(), us.end());
            mOut.p99Us = us[(size_t)(us.size() * 99 / 100)];

            double sumD2Sq = 0.0;
            size_t countD2 = 0;
            for (const auto* ch : {&outL, &outR}) {
                for (size_t i = 0; i < ch->size(); ++i) {
                    mOut.peak = std::max(mOut.peak, (double)std::abs((*ch)[i]));
                    if (i >= 2) {
                        const double d2 = std::abs((double)(*ch)[i] - 2.0 * (double)(*ch)[i - 1] + (double)(*ch)[i - 2]);
                        mOut.maxD2 = std::max(mOut.maxD2, d2);
                        sumD2Sq += d2 * d2;
                        ++countD2;
                    }
                }
            }
            const double rmsD2 = std::sqrt(sumD2Sq / std::max<size_t>(1, countD2));
            mOut.d2Crest = rmsD2 > 1e-12 ? (mOut.maxD2 / rmsD2) : 0.0;
            p.setPlayHead(nullptr);
            return true;
        };

        // 3a. Physical mode bit-identity (Wander OFF vs Wander ON in Physical mode)
        std::vector<float> physOffL, physOffR, physOnL, physOnR;
        RenderMetrics mPhysOff, mPhysOn;
        check(runAudioScenario(1, false, false, 101, false, physOffL, physOffR, mPhysOff, 400)
              && runAudioScenario(1, false, true, 101, false, physOnL, physOnR, mPhysOn, 400),
              "V04b Physical mode renders complete");
        float maxPhysDiff = 0.f;
        for (size_t i = 0; i < physOffL.size(); ++i) {
            maxPhysDiff = std::max(maxPhysDiff, std::abs(physOffL[i] - physOnL[i]));
            maxPhysDiff = std::max(maxPhysDiff, std::abs(physOffR[i] - physOnR[i]));
        }
        std::printf("  Physical mode Wander OFF vs ON max diff: %.9g FS (allocs=%lld)\n",
                    maxPhysDiff, (long long)mPhysOn.allocs);
        check(maxPhysDiff == 0.f && mPhysOn.allocs == 0,
              "V04b Wander has zero effect in Physical mode (bit-identical 0.0 FS)");

        // 3b. Creative mode 1 source (Motion OFF): Wander OFF vs Wander ON (seed 101 x2, seed 202)
        std::vector<float> crOffL, crOffR, crOn1L, crOn1R, crOn2L, crOn2R, crSeedBL, crSeedBR;
        RenderMetrics mCrOff, mCrOn1, mCrOn2, mCrSeedB;
        check(runAudioScenario(1, true, false, 101, false, crOffL, crOffR, mCrOff, 600)
              && runAudioScenario(1, true, true, 101, false, crOn1L, crOn1R, mCrOn1, 600)
              && runAudioScenario(1, true, true, 101, false, crOn2L, crOn2R, mCrOn2, 600)
              && runAudioScenario(1, true, true, 202, false, crSeedBL, crSeedBR, mCrSeedB, 600),
              "V04b Creative 1-source renders complete");
        float maxSeedSameDiff = 0.f, maxWanderEffectDiff = 0.f, maxSeedDiff = 0.f;
        for (size_t i = 0; i < crOn1L.size(); ++i) {
            maxSeedSameDiff = std::max({maxSeedSameDiff, std::abs(crOn1L[i] - crOn2L[i]), std::abs(crOn1R[i] - crOn2R[i])});
            maxWanderEffectDiff = std::max({maxWanderEffectDiff, std::abs(crOn1L[i] - crOffL[i]), std::abs(crOn1R[i] - crOffR[i])});
            maxSeedDiff = std::max({maxSeedDiff, std::abs(crOn1L[i] - crSeedBL[i]), std::abs(crOn1R[i] - crSeedBR[i])});
        }
        std::printf("  Creative 1-src (Motion OFF, 600 blocks): two renders same seed diff = %.9g FS; OFF vs ON delta = %.6f FS; seed 101 vs 202 delta = %.6f FS\n",
                    maxSeedSameDiff, maxWanderEffectDiff, maxSeedDiff);
        std::printf("  Creative 1-src (Motion OFF): CPU mean %.2f->%.2f us (p99 %.2f->%.2f us), max|d2y|=%.6f FS (crest %.2f), allocs=%lld\n",
                    mCrOff.meanUs, mCrOn1.meanUs, mCrOff.p99Us, mCrOn1.p99Us, mCrOn1.maxD2, mCrOn1.d2Crest, (long long)mCrOn1.allocs);
        check(maxSeedSameDiff == 0.f, "V04b Two offline renders with Wander ON and same seed are bit-identical (0 FS)");
        check(maxWanderEffectDiff > 1e-3f && maxSeedDiff > 1e-3f,
              "V04b Wander ON modifies Creative audio and Reseed produces a distinct render");
        check(mCrOn1.allocs == 0 && mCrOn1.maxD2 < 0.05 && mCrOn1.peak > 1e-3,
              "V04b Creative 1-src Wander ON has 0 allocations and no clicks");

        // 3c. Creative mode 1 source (Motion ON): Wander OFF vs Wander ON
        std::vector<float> mot1OffL, mot1OffR, mot1OnL, mot1OnR;
        RenderMetrics mMot1Off, mMot1On;
        check(runAudioScenario(1, true, false, 101, true, mot1OffL, mot1OffR, mMot1Off, 600)
              && runAudioScenario(1, true, true, 101, true, mot1OnL, mot1OnR, mMot1On, 600),
              "V04b Creative 1-source Motion ON renders complete");
        std::printf("  Creative 1-src (Motion ON):  CPU mean %.2f->%.2f us (p99 %.2f->%.2f us), max|d2y|=%.6f FS (crest %.2f), allocs=%lld, motionReady=%d\n",
                    mMot1Off.meanUs, mMot1On.meanUs, mMot1Off.p99Us, mMot1On.p99Us, mMot1On.maxD2, mMot1On.d2Crest,
                    (long long)mMot1On.allocs, (int)mMot1On.motionStayedReady);
        check(mMot1On.allocs == 0 && mMot1On.motionStayedReady && mMot1On.maxD2 < 0.05 && mMot1On.peak > 1e-3,
              "V04b Motion ON + Wander (1 source) runs with 0 allocations, no Rebuilding, and no clicks");

        // 3d. Creative mode 8 sources (Motion OFF and Motion ON): Wander OFF vs Wander ON
        std::vector<float> cr8OffL, cr8OffR, cr8OnL, cr8OnR, mot8OffL, mot8OffR, mot8OnL, mot8OnR;
        RenderMetrics mCr8Off, mCr8On, mMot8Off, mMot8On;
        check(runAudioScenario(8, true, false, 303, false, cr8OffL, cr8OffR, mCr8Off, 400)
              && runAudioScenario(8, true, true, 303, false, cr8OnL, cr8OnR, mCr8On, 400)
              && runAudioScenario(8, true, false, 303, true, mot8OffL, mot8OffR, mMot8Off, 400)
              && runAudioScenario(8, true, true, 303, true, mot8OnL, mot8OnR, mMot8On, 400),
              "V04b Creative 8-source (Motion OFF & ON) renders complete");
        std::printf("  Creative 8-src (Motion OFF): CPU mean %.2f->%.2f us (p99 %.2f->%.2f us), max|d2y|=%.6f FS (crest %.2f), allocs=%lld\n",
                    mCr8Off.meanUs, mCr8On.meanUs, mCr8Off.p99Us, mCr8On.p99Us, mCr8On.maxD2, mCr8On.d2Crest, (long long)mCr8On.allocs);
        std::printf("  Creative 8-src (Motion ON):  CPU mean %.2f->%.2f us (p99 %.2f->%.2f us), max|d2y|=%.6f FS (crest %.2f), allocs=%lld, motionReady=%d\n",
                    mMot8Off.meanUs, mMot8On.meanUs, mMot8Off.p99Us, mMot8On.p99Us, mMot8On.maxD2, mMot8On.d2Crest,
                    (long long)mMot8On.allocs, (int)mMot8On.motionStayedReady);
        check(mCr8On.allocs == 0 && mMot8On.allocs == 0 && mMot8On.motionStayedReady
              && mCr8On.maxD2 < 0.08 && mMot8On.maxD2 < 0.08 && mMot8On.peak > 1e-3,
              "V04b Motion ON/OFF + Wander (8 sources) runs with 0 allocations, no Rebuilding, and no clicks");
    }

    // 4. Orientation (AlongPath no-flip vs LookAt live displaced pose), Doppler, Mod drivers, and Bake bypass/restore
    {
        auto pPtr = std::make_unique<VerbProcessor>();
        auto& p = *pPtr;
        p.setTailSettings({8000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        p.loadSceneText(p4test::kScene, "V04b corridor", true);
        check(waitReady(p, 90), "V04b interaction corridor loads");

        // 4a. Straight path with AlongPath orientation + high-amplitude Wander:
        // Verify orientation uses undisplaced path tangent (90 deg along +Y) with zero flipping even when s1path is stationary!
        verbp::MovementPath linePath;
        linePath.points = {{{2.0, 1.5, 1.5}}, {{2.0, 6.5, 1.5}}}; // heading = +90 deg
        linePath.enabled = true;
        linePath.closed = false;
        linePath.orient = true; // Along path
        linePath.wander.enabled = true;
        linePath.wander.ampHorizM = 0.8;
        linePath.wander.ampVertM = 0.3;
        linePath.wander.rateHz = 4.0;
        linePath.wander.pinEnds = true;
        linePath.wander.seed = 555;
        setP(p, "s1az", 0.0f);
        setP(p, "s1path", 0.5f);
        setP(p, "mode", 1.0f);
        check(p.setMovementPath(0, linePath), "V04b set straight AlongPath with Wander");

        E12PlayHead head; head.playing = true; head.samples = 0;
        p.setPlayHead(&head);
        juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
        double maxTangentFlip = 0.0;
        for (int b = 0; b < 120; ++b) {
            audio.clear();
            stage2Process(p, audio, midi);
            const double az = p.liveAzimuth(0);
            maxTangentFlip = std::max(maxTangentFlip, std::abs(std::remainder(az - 90.0, 360.0)));
            head.samples += 128;
        }
        check(maxTangentFlip < 1e-4, "V04b AlongPath orientation follows undisplaced path tangent with zero flipping under transverse Wander");

        // 4b. Switch to LookAt listener + Modulation drivers (DistanceToListener -> Mod Out 1, Speed -> Mod Out 2):
        // With s1path stationary at 0.5, all variation in LookAt azimuth, DistanceToListener, Speed, and Doppler comes from Wander!
        linePath.orient = false;
        linePath.lookAt = true;
        linePath.lookTarget = verbp::kListener;
        check(p.setMovementPath(0, linePath), "V04b switch path to LookAt listener with Wander");

        verbp::ModulationRoute rDist, rSpeed;
        rDist.enabled = true; rDist.object = 0; rDist.driver = verbp::ModDriver::DistanceToListener;
        rDist.target = verbp::ModTarget::None; rDist.targetObject = -1; rDist.hostOut = 1;
        rDist.attackMs = 0.0; rDist.releaseMs = 0.0;
        rDist.curve.inputMin = 0.0; rDist.curve.inputMax = 6.0;
        rDist.curve.outputMin = 0.0; rDist.curve.outputMax = 1.0;

        rSpeed.enabled = true; rSpeed.object = 0; rSpeed.driver = verbp::ModDriver::Speed;
        rSpeed.target = verbp::ModTarget::None; rSpeed.targetObject = -1; rSpeed.hostOut = 2;
        rSpeed.attackMs = 0.0; rSpeed.releaseMs = 0.0;
        rSpeed.curve.inputMin = 0.0; rSpeed.curve.inputMax = 5.0;
        rSpeed.curve.outputMin = 0.0; rSpeed.curve.outputMax = 1.0;
        check(p.setModulationRoutes({rDist, rSpeed}), "V04b configure DistanceToListener and Speed routes");

        float minLookAz = 180.f, maxLookAz = -180.f;
        float minDistOut = 1.f, maxDistOut = 0.f, maxSpeedOut = 0.f;
        for (int b = 0; b < 300; ++b) {
            audio.clear();
            stage2Process(p, audio, midi);
            const float az = p.liveAzimuth(0);
            minLookAz = std::min(minLookAz, az);
            maxLookAz = std::max(maxLookAz, az);
            const float dVal = p.modOutParameter(0)->getValue();
            const float sVal = p.modOutParameter(1)->getValue();
            minDistOut = std::min(minDistOut, dVal);
            maxDistOut = std::max(maxDistOut, dVal);
            maxSpeedOut = std::max(maxSpeedOut, sVal);
            head.samples += 128;
        }
        std::printf("  Wander at fixed Position=50%%: LookAt azimuth span = %.2f deg, Distance ModOut span = %.4f, peak Speed ModOut = %.4f\n",
                    maxLookAz - minLookAz, maxDistOut - minDistOut, maxSpeedOut);
        check(maxLookAz - minLookAz > 5.0f && maxDistOut - minDistOut > 0.03f && maxSpeedOut > 0.05f,
              "V04b LookAt orientation and Distance/Speed modulation drivers track the effective Wander-displaced pose");

        // 4c. Doppler sees Wander velocity even when Position is fixed at 50 %
        auto renderDopplerAtFixedU = [&](float dopplerVal) {
            head.samples = 0;
            setP(p, "doppler", dopplerVal);
            std::vector<float> out;
            for (int b = 0; b < 200; ++b) {
                audio.clear();
                for (int i = 0; i < 128; ++i)
                    audio.setSample(0, i, 0.2f * (float)std::sin((head.samples + i) * (2.0 * verb::kPi * 440.0 / 48000.0)));
                stage2Process(p, audio, midi);
                for (int i = 0; i < 128; ++i) out.push_back(audio.getSample(0, i));
                head.samples += 128;
            }
            return out;
        };
        const auto dop0 = renderDopplerAtFixedU(0.0f);
        const auto dop1 = renderDopplerAtFixedU(1.0f);
        float maxDopDelta = 0.f;
        for (size_t i = 0; i < dop0.size(); ++i) maxDopDelta = std::max(maxDopDelta, std::abs(dop0[i] - dop1[i]));
        std::printf("  Wander at fixed Position=50%% Doppler=0 vs Doppler=1 max delta: %.6f FS\n", maxDopDelta);
        check(maxDopDelta > 1e-4f, "V04b Doppler pitch shift responds to transverse Wander velocity");
        p.setPlayHead(nullptr);
        p.setModulationRoutes({});

        // 4d. Bake interaction: Bake with Wander OFF -> enable Wander in Creative (bypassed, startPathBake blocked, reason reported)
        //     -> switch to Physical or turn Wander OFF (stashed Bake restored, lookups resume)
        linePath.lookAt = false;
        linePath.orient = true;
        linePath.wander.enabled = false;
        check(p.setMovementPath(0, linePath), "V04b disable Wander before baking path");
        if (p.startPathBake(0) && p4test::waitBaked(p, 120)) {
            const auto eng = E9bAccess::engine(p);
            const auto bakedHash = eng->bakeStats().hash;
            check(bakedHash != 0, "V04b path bakes cleanly when Wander is OFF");

            // Enable Wander in Creative mode
            linePath.wander.enabled = true;
            check(p.setMovementPath(0, linePath), "V04b enable Wander on baked path in Creative mode");
            for (int i = 0; i < 40; ++i) p4test::pump(p, 1);
            check(!p.startPathBake(0), "V04b startPathBake is blocked while Wander is active in Creative mode");
            const auto stJson = juce::JSON::toString(p.pathBakeStatus(), true);
            check(stJson.contains("Path wander bypasses baked tables"),
                  "V04b pathBakeStatus reports 'Path wander bypasses baked tables'");
            const auto lookupsBefore = eng->bakeStats().lookups;
            h2test::pumpBlocks(p, 60, 128);
            check(eng->bakeStats().lookups == lookupsBefore,
                  "V04b active Wander in Creative mode bypasses baked table lookups on the audio thread");

            // Switch to Physical mode (Wander still enabled in state, but inactive in Physical!) -> Bake table restores!
            setP(p, "mode", 0.0f);
            const bool restoredPhys = p4test::waitBaked(p, 15);
            const auto lookupsPhysBefore = eng->bakeStats().lookups;
            h2test::pumpBlocks(p, 120, 128);
            check(restoredPhys && eng->bakeStats().hash == bakedHash && eng->bakeStats().lookups > lookupsPhysBefore,
                  "V04b switching to Physical mode restores stashed Bake table and resumes lookups");

            // Switch back to Creative -> bypassed again; then turn Wander OFF in Creative -> restored again!
            setP(p, "mode", 1.0f);
            p4test::waitState(p, "notBaked", nullptr, 15);
            linePath.wander.enabled = false;
            check(p.setMovementPath(0, linePath), "V04b turn Wander OFF in Creative mode");
            const bool restoredCr = p4test::waitBaked(p, 15);
            const auto lookupsCrBefore = eng->bakeStats().lookups;
            h2test::pumpBlocks(p, 120, 128);
            check(restoredCr && eng->bakeStats().hash == bakedHash && eng->bakeStats().lookups > lookupsCrBefore,
                  "V04b turning Wander OFF in Creative mode restores stashed Bake table and resumes lookups");
        } else {
            check(false, "V04b corridor Bake succeeded");
        }
    }

    // 5. State round-trip, .verbroom export/import (key present & absent), unknown keys, legacy fixtures,
    //    impulse check after reload (Default Room & VSL), and Classic editor controls
    {
        auto pPtr = std::make_unique<VerbProcessor>();
        auto& p = *pPtr;
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "V04b round-trip processor ready");

        const auto sc = p.getDisplayScene();
        verbp::PathShapeParams sp;
        sp.kind = verbp::PathShapeKind::Circle;
        sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5; sp.radiusX = 1.8; sp.nodes = 16;
        auto mp = verbp::generatePathShapeForRoom(sp, &sc, true).path;
        mp.endless = true;
        mp.endlessPeriodS = 8.0;
        check(p.setMovementPath(0, mp), "V04b set Circle with default Wander OFF");

        // Verify XML omits <Wander> element when Wander is default
        juce::MemoryBlock stateDefault;
        p.getStateInformation(stateDefault);
        auto xmlDefault = VerbProcessor::getXmlFromBinary(stateDefault.getData(), (int)stateDefault.getSize());
        auto* pathXmlDefault = xmlDefault ? xmlDefault->getChildByName("MovementPaths")->getChildByName("Path") : nullptr;
        check(pathXmlDefault && pathXmlDefault->getChildByName("Wander") == nullptr,
              "V04b state XML omits <Wander> child when Wander is at defaults");

        // Configure custom Wander parameters
        mp.wander.enabled = true;
        mp.wander.ampHorizM = 0.65;
        mp.wander.ampVertM = 0.35;
        mp.wander.rateHz = 1.45;
        mp.wander.smoothness = 0.72;
        mp.wander.pinEnds = false;
        mp.wander.horizOnly = true;
        mp.wander.seed = 424242;
        check(p.setMovementPath(0, mp), "V04b enable custom Wander on path 0");

        juce::MemoryBlock stateOn;
        p.getStateInformation(stateOn);
        const auto presetOn = p.getRoomPresetText();

        auto qPtr = std::make_unique<VerbProcessor>();
        auto& q = *qPtr;
        q.setPlayConfigDetails(2, 2, 48000, 128);
        q.prepareToPlay(48000, 128);
        q.setStateInformation(stateOn.getData(), (int)stateOn.getSize());
        check(waitReady(q, 90), "V04b state with Wander ON reloads");
        const auto rw = q.getMovementPath(0).wander;
        check(rw.enabled && std::abs(rw.ampHorizM - 0.65) < 1e-9 && std::abs(rw.ampVertM - 0.35) < 1e-9
              && std::abs(rw.rateHz - 1.45) < 1e-9 && std::abs(rw.smoothness - 0.72) < 1e-9
              && !rw.pinEnds && rw.horizOnly && rw.seed == 424242,
              "V04b project state round-trip preserves all 8 PathWander fields");

        // Impulse check after reload on Default Room
        const auto irDefault = h2test::captureImpulse(q, 2, 0, 120, 128);
        const double eDefault = h2test::channelEnergy(irDefault[0]) + h2test::channelEnergy(irDefault[1]);
        check(std::isfinite(eDefault) && eDefault > 1e-6,
              "V04b reloaded Default Room with Wander path emits finite impulse response");

        // .verbroom export -> import (key present, key absent, and unknown future child/attribute)
        juce::String err;
        auto rPtr = std::make_unique<VerbProcessor>();
        auto& r = *rPtr;
        r.setPlayConfigDetails(2, 2, 48000, 128);
        r.prepareToPlay(48000, 128);
        check(waitReady(r, 90), "V04b preset import processor ready");
        check(r.loadRoomPresetText(presetOn, err) && waitReady(r, 90)
              && r.getMovementPath(0).wander == mp.wander,
              "V04b .verbroom round-trip preserves PathWander identically");

        auto xmlPresetNoWander = juce::parseXML(presetOn);
        if (auto* mpXml = xmlPresetNoWander->getChildByName("VerbState")->getChildByName("MovementPaths")) {
            if (auto* pXml = mpXml->getChildByName("Path")) {
                if (auto* wXml = pXml->getChildByName("Wander")) pXml->removeChildElement(wXml, true);
                pXml->setAttribute("futureUnknownAttr", "123");
                pXml->createNewChildElement("FutureUnknownChild")->setAttribute("foo", "bar");
            }
        }
        check(r.loadRoomPresetText(xmlPresetNoWander->toString(), err) && waitReady(r, 90)
              && r.getMovementPath(0).wander.isDefault() && r.getMovementPath(0).points == mp.points,
              "V04b .verbroom without <Wander> and with unknown keys loads cleanly with default Wander");

        // Legacy fixtures and VSL preset round-trip + impulse check
        {
            const auto legacyStateFile = juce::File::getCurrentWorkingDirectory().getChildFile("plugin/fixtures/stage2/legacy.state");
            if (legacyStateFile.existsAsFile()) {
                juce::MemoryBlock legData;
                legacyStateFile.loadFileAsData(legData);
                auto legProcPtr = std::make_unique<VerbProcessor>();
                auto& legProc = *legProcPtr;
                legProc.setPlayConfigDetails(2, 2, 48000, 128);
                legProc.prepareToPlay(48000, 128);
                legProc.setStateInformation(legData.getData(), (int)legData.getSize());
                check(waitReady(legProc, 90) && legProc.getMovementPath(0).wander.isDefault(),
                      "V04b legacy.state loads cleanly with default Wander");
            }
            const auto vslFile = juce::File::getCurrentWorkingDirectory().getChildFile("VSL.verbroom");
            if (vslFile.existsAsFile()) {
                auto vslPPtr = std::make_unique<VerbProcessor>();
                auto& vslP = *vslPPtr;
                vslP.setPlayConfigDetails(64, 2, 48000, 128);
                juce::AudioProcessor::BusesLayout vslLayout;
                vslLayout.inputBuses.add(juce::AudioChannelSet::discreteChannels(64));
                vslLayout.outputBuses.add(juce::AudioChannelSet::stereo());
                vslP.setBusesLayout(vslLayout);
                vslP.prepareToPlay(48000, 128);
                juce::String vslErr;
                check(vslP.loadRoomPresetText(vslFile.loadFileAsString(), vslErr) && waitReady(vslP, 120),
                      "V04b VSL preset loads");
                const auto vslSc = vslP.getDisplayScene();
                verbp::PathShapeParams vslSp;
                vslSp.kind = verbp::PathShapeKind::Circle;
                vslSp.cx = vslSc.listenerPos.x; vslSp.cy = vslSc.listenerPos.y; vslSp.z = 1.5;
                vslSp.radiusX = 2.0; vslSp.nodes = 16;
                auto vslPath = verbp::generatePathShapeForRoom(vslSp, &vslSc, true).path;
                vslPath.endless = true;
                vslPath.endlessPeriodS = 8.0;
                vslPath.wander.enabled = true;
                vslPath.wander.ampHorizM = 0.5;
                vslPath.wander.ampVertM = 0.3;
                vslPath.wander.rateHz = 0.6;
                vslPath.wander.seed = 777;
                check(vslP.setMovementPath(0, vslPath), "V04b VSL preset accepts Circle + Endless + Wander path");
                juce::MemoryBlock vslSaved;
                vslP.getStateInformation(vslSaved);
                auto vslReloadedPtr = std::make_unique<VerbProcessor>();
                auto& vslReloaded = *vslReloadedPtr;
                vslReloaded.setPlayConfigDetails(64, 2, 48000, 128);
                vslReloaded.setBusesLayout(vslLayout);
                vslReloaded.prepareToPlay(48000, 128);
                vslReloaded.setStateInformation(vslSaved.getData(), (int)vslSaved.getSize());
                check(waitReady(vslReloaded, 120) && vslReloaded.getMovementPath(0).wander == vslPath.wander,
                      "V04b VSL preset state round-trip preserves Circle + Endless + Wander");
                const auto irVsl = h2test::captureImpulse(vslReloaded, 64, 0, 120, 128);
                const double eVsl = h2test::channelEnergy(irVsl[0]) + h2test::channelEnergy(irVsl[1]);
                check(std::isfinite(eVsl) && eVsl > 1e-6,
                      "V04b reloaded VSL preset with Wander circle emits finite impulse response");
            }
        }

        // Classic editor (VerbEditor) Wander toggle, amplitude slider, Reseed button, and Undo/Redo
        {
            auto editor = std::make_unique<VerbEditor>(r);
            editor->setSize(900, 600);
            editor->showPage(0, 0);
            juce::ToggleButton* wanderBtn = nullptr;
            juce::Slider* wanderAmpSlider = nullptr;
            juce::TextButton* reseedBtn = nullptr;
            for (int i = 0; i < editor->getNumChildComponents(); ++i) {
                auto* ch = editor->getChildComponent(i);
                if (auto* tgb = dynamic_cast<juce::ToggleButton*>(ch)) {
                    if (tgb->getButtonText() == "Wander") wanderBtn = tgb;
                } else if (auto* sl = dynamic_cast<juce::Slider*>(ch)) {
                    if (sl->getTooltip().contains("wander amplitude")) wanderAmpSlider = sl;
                } else if (auto* tb = dynamic_cast<juce::TextButton*>(ch)) {
                    if (tb->getButtonText() == "Reseed") reseedBtn = tb;
                }
            }
            check(wanderBtn && wanderAmpSlider && reseedBtn
                  && wanderBtn->isVisible() && wanderAmpSlider->isVisible() && reseedBtn->isVisible()
                  && editor->getLocalBounds().contains(wanderBtn->getBounds())
                  && editor->getLocalBounds().contains(wanderAmpSlider->getBounds())
                  && editor->getLocalBounds().contains(reseedBtn->getBounds()),
                  "V04b Classic editor shows Wander toggle, amplitude slider, and Reseed button inside 900x600 window");
            if (wanderBtn && wanderAmpSlider && reseedBtn) {
                const uint32_t seedBefore = r.getMovementPath(0).wander.seed;
                wanderBtn->setToggleState(true, juce::sendNotificationSync);
                wanderAmpSlider->setValue(1.2, juce::sendNotificationSync);
                reseedBtn->triggerClick();
                juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
                const auto wAfter = r.getMovementPath(0).wander;
                check(wAfter.enabled && std::abs(wAfter.ampHorizM - 1.2) < 1e-6
                      && std::abs(wAfter.ampVertM - 0.6) < 1e-6 && wAfter.seed != seedBefore,
                      "V04b Classic editor controls enable Wander, set amplitude, and reseed via EditorSession");
                check(r.getEditorSession().undo().ok && r.getMovementPath(0).wander.seed == seedBefore,
                      "V04b Classic editor Wander Reseed is undoable in one EditorSession step");
            }
        }
    }

    return fails == beforeFails ? 0 : 1;
}
