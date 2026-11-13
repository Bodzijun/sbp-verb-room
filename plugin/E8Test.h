#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

struct E8MockPlayHead : public juce::AudioPlayHead {
    bool playing = true;
    int64_t timeSamples = 0;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo info;
        info.setIsPlaying(playing);
        info.setTimeInSamples(timeSamples);
        info.setTimeInSeconds((double)timeSamples / 48000.0);
        return info;
    }
};

static inline std::string e8CorridorSceneText(double lx = 0.05, double ly = 12.5, double sx = 2.7, double sy = 12.5) {
    verb::Scene s;
    s.sr = 48000.0;
    s.length = 2.0;
    s.height = 2.7;
    s.order = 2;
    s.diffraction = true;
    s.mats = verb::surfacePresets();
    s.floorMat = verb::presetIndexByName("Ceramic_tiles");
    s.ceilMat = verb::presetIndexByName("Plaster");
    const int mw = verb::presetIndexByName("Plaster");
    s.walls = {{0, 0, 0.85, 0, mw}, {2.85, 0, 3.7, 0, mw}, {3.7, 0, 3.7, 14, mw}, {3.7, 14, 0, 14, mw}, {0, 14, 0, 0, mw}};
    verb::Transducer src;
    src.name = "Source";
    src.pos = {sx, sy, 1.5};
    src.azimuthDeg = 0;
    src.pattern = 0;
    s.sources.push_back(src);
    s.receivers = verb::makeReceiverArray(verb::Layout::StereoORTF, {lx, ly, 1.5}, 0.1, 2);
    s.hasListener = true;
    s.listenerPos = {lx, ly, 1.5};
    s.listenerYaw = 0;
    return verb::serializeScene(s);
}

static inline int testE8Plugin() {
    std::printf("Test E8 (Plugin Step 1): jump discontinuity, pause, playhead loop, 20 m/s smooth\n");

    const double sr = 48000.0;
    const int blk = 512;
    const double S0x = 2.7, S0y = 12.5, L0x = 0.05, L0y = 12.5;
    const double S1x = 1.35, S1y = -0.5, L1x = 2.35, L1y = -0.5;
    const double yaw0 = std::atan2(S0y - L0y, S0x - L0x) * 180.0 / verb::kPi;
    const double yaw1 = std::atan2(S1y - L1y, S1x - L1x) * 180.0 / verb::kPi;

    // Periodic broadband signal (8192 samples)
    std::mt19937 rng(8802);
    std::uniform_real_distribution<double> phDist(0.0, 2.0 * verb::kPi);
    std::vector<verb::cd> cpx(8192);
    for (int k = 1; k < 4096; ++k) {
        const double f = k * sr / 8192.0;
        if (f >= 88.0 && f <= 11314.0) { cpx[(size_t)k] = std::polar(1.0, phDist(rng)); cpx[(size_t)(8192 - k)] = std::conj(cpx[(size_t)k]); }
    }
    verb::fft(cpx, true);
    double ss = 0.0;
    for (const auto& v : cpx) ss += v.real() * v.real();
    const double scale = 0.02 / std::sqrt(ss / 8192.0);
    std::vector<float> signal(8192);
    for (int j = 0; j < 8192; ++j) signal[(size_t)j] = (float)(cpx[(size_t)j].real() * scale);

    auto runLoopMode = [&](bool withPauseAndPlayhead, int soloWhich, double startX = 0.05) {
        const double yaw0 = std::atan2(S0y - L0y, S0x - startX) * 180.0 / verb::kPi;
        VerbProcessor p, ref;
        E8MockPlayHead phP, phRef;
        p.setPlayHead(&phP);
        ref.setPlayHead(&phRef);
        p.setPlayConfigDetails(2, 2, sr, blk);
        ref.setPlayConfigDetails(2, 2, sr, blk);
        p.prepareToPlay(sr, blk);
        ref.prepareToPlay(sr, blk);
        const std::string roomText = e8CorridorSceneText(startX, L0y, S0x, S0y);
        p.loadSceneText(juce::String(roomText), "corridor.txt", true);
        ref.loadSceneText(juce::String(roomText), "corridor.txt", true);
        const bool okP = waitReady(p, 60), okR = waitReady(ref, 60);
        if (!okP || !okR) std::printf("  [diag] p status=%s ref status=%s\n", p.getStatus().toRawUTF8(), ref.getStatus().toRawUTF8());
        check(okP && okR, "corridor scene loads in plugin");
        p.setTailSettings({12000, false});
        ref.setTailSettings({12000, false});

        auto setupProc = [&](VerbProcessor& vp) {
            setP(vp, "mode", 0.0);
            setP(vp, "mix", 100.0);
            setP(vp, "spatialDry", 1.0);
            setP(vp, "doppler", 1.0);
            setP(vp, "soloEarly", soloWhich == 2 ? 1.0 : 0.0);
            setP(vp, "soloTail", soloWhich == 4 ? 1.0 : 0.0);
            setP(vp, "s1x", S0x); setP(vp, "s1y", S0y); setP(vp, "s1z", 1.5);
            setP(vp, "lx", startX); setP(vp, "ly", L0y); setP(vp, "lz", 1.5); setP(vp, "laz", yaw0);
        };
        setupProc(p);
        setupProc(ref);

        juce::AudioBuffer<float> bp(2, blk), br(2, blk);
        juce::MidiBuffer midi;
        uint64_t fidx = 0;
        auto pumpBlock = [&](double* outDbDiff = nullptr, double* outPreLvlDb = nullptr, double* outP = nullptr, double* outR = nullptr) {
            bp.clear(); br.clear();
            for (int i = 0; i < blk; ++i) {
                const float x = signal[(fidx + (uint64_t)i) % 8192];
                bp.setSample(0, i, x); br.setSample(0, i, x);
            }
            p.processBlock(bp, midi);
            ref.processBlock(br, midi);
            phP.timeSamples += blk;
            phRef.timeSamples += blk;
            fidx += (uint64_t)blk;
            if (outDbDiff || outPreLvlDb || outP || outR) {
                double eP = 0.0, eR = 0.0;
                for (int c = 0; c < 2; ++c) {
                    const float* dp = bp.getReadPointer(c);
                    const float* dr = br.getReadPointer(c);
                    for (int i = 0; i < blk; ++i) { eP += (double)dp[i] * dp[i]; eR += (double)dr[i] * dr[i]; }
                }
                const double lP = 10.0 * std::log10(eP / (2.0 * blk) + 1e-30);
                const double lR = 10.0 * std::log10(eR / (2.0 * blk) + 1e-30);
                if (outPreLvlDb) *outPreLvlDb = lP;
                if (outP) *outP = lP;
                if (outR) *outR = lR;
                if (outDbDiff) *outDbDiff = lP - lR;
            }
        };

        // Wait until tail quality (12k, non-adaptive) & initial pose take effect on both p and ref
        for (int b = 0; b < 300; ++b) {
            pumpBlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto dp = p.getTailDiagnostics(), dr = ref.getTailDiagnostics();
            if (b >= 40 && dp && dr && dp->baseRays == 12000 && !dp->adaptive && dr->baseRays == 12000 && !dr->adaptive
                && p.isTailCurrent() && ref.isTailCurrent())
                break;
        }
        double worstBurstDb = -1e9, maxAfterErrDb = 0.0, maxRecoverMs = 0.0;
        for (int rep = 0; rep < 5; ++rep) {
            // Walk smoothly down corridor out the opening to (L1, S1) over ~0.8 s (75 blocks)
            const int moveBlocks = 75;
            for (int b = 0; b < moveBlocks; ++b) {
                const double u = (double)(b + 1) / moveBlocks;
                setP(p, "s1x", S0x + (S1x - S0x) * u);
                setP(p, "s1y", S0y + (S1y - S0y) * u);
                setP(p, "lx", startX + (L1x - startX) * u);
                setP(p, "ly", L0y + (L1y - L0y) * u);
                setP(p, "laz", yaw0 + std::remainder(yaw1 - yaw0, 360.0) * u);
                pumpBlock();
            }
            // Wait outside so worker settles and builds "outside" tail, measuring pre-jump outside level
            double preJumpLvl = -100.0;
            for (int b = 0; b < 120; ++b) {
                double lv = -100.0;
                pumpBlock(nullptr, &lv);
                if (b >= 110) preJumpLvl = (b == 110) ? lv : std::max(preJumpLvl, lv);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (withPauseAndPlayhead) {
                // Simulate a 0.35 s audio callback gap + host loop restart to sample 0
                std::this_thread::sleep_for(std::chrono::milliseconds(350));
                phP.timeSamples = 0;
            }
            // Jump back to start pose
            setP(p, "s1x", S0x); setP(p, "s1y", S0y);
            setP(p, "lx", startX); setP(p, "ly", L0y); setP(p, "laz", yaw0);

            const auto tJump = std::chrono::steady_clock::now();
            double recoverMs = -1.0;
            double tailEndErrSum = 0.0;
            int tailEndCnt = 0;
            // Measure for ~1.4 s (130 blocks), pacing real time ~10 ms/block so worker completes rebuild (~700 ms)
            for (int b = 0; b < 130; ++b) {
                double diff = 0.0, lP = 0.0, lR = 0.0;
                pumpBlock(&diff, nullptr, &lP, &lR);
                if (b < 25) {
                    const double over = lP - std::max(preJumpLvl, lR);
                    worstBurstDb = std::max(worstBurstDb, over);
                }
                if (b >= 3 && std::fabs(diff) <= 1.0 && recoverMs < 0.0 && (soloWhich == 2 || p.isTailCurrent()))
                    recoverMs = 1000.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - tJump).count();
                if (b >= 115) { tailEndErrSum += diff; ++tailEndCnt; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const double repEndDiff = tailEndErrSum / std::max(1, tailEndCnt);
            if (recoverMs < 0.0) recoverMs = 1300.0;
            maxRecoverMs = std::max(maxRecoverMs, recoverMs);
            maxAfterErrDb = std::max(maxAfterErrDb, std::fabs(repEndDiff));
        }
        std::printf("  plugin %s (x=%.3f, solo=%d, 5 reps): worst burst %+.2f dB vs max(pre,ref), steady err %.2f dB, max recover %.1f ms\n",
            withPauseAndPlayhead ? "pause+playhead" : "continuous", startX, soloWhich, worstBurstDb, maxAfterErrDb, maxRecoverMs);
        check(worstBurstDb <= 1.0 && maxAfterErrDb <= 1.0,
            withPauseAndPlayhead ? "plugin pause+playhead 5-rep loop: no burst > +1 dB and <= 1 dB recovery"
                                 : "plugin continuous 5-rep loop: no burst > +1 dB and <= 1 dB recovery");
    };

    runLoopMode(false, 2, 0.05);
    runLoopMode(true, 2, 0.05);
    runLoopMode(false, 4, 0.05);
    runLoopMode(true, 4, 0.05);
    runLoopMode(false, 2, -0.005);
    runLoopMode(true, 4, -0.005);

    // Plugin wall-line yaw check (x = 0.0, ORTF): both channels keep reflections & tail across orientations
    {
        VerbProcessor p, ref;
        p.setPlayConfigDetails(2, 2, sr, blk);
        ref.setPlayConfigDetails(2, 2, sr, blk);
        p.prepareToPlay(sr, blk);
        ref.prepareToPlay(sr, blk);
        p.loadSceneText(juce::String(e8CorridorSceneText(0.0, L0y, S0x, S0y)), "corridor.txt", true);
        ref.loadSceneText(juce::String(e8CorridorSceneText(0.50, L0y, S0x, S0y)), "corridor.txt", true);
        check(waitReady(p, 60) && waitReady(ref, 60), "plugin wall yaw check loads");
        setP(p, "mode", 0.0); setP(p, "mix", 100.0); setP(p, "spatialDry", 1.0); setP(p, "soloEarly", 1.0);
        setP(ref, "mode", 0.0); setP(ref, "mix", 100.0); setP(ref, "spatialDry", 1.0); setP(ref, "soloEarly", 1.0);
        juce::AudioBuffer<float> bp(2, blk), br(2, blk);
        juce::MidiBuffer midi;
        double worstDrop = 0.0;
        for (double yaw : {0.0, 90.0, 180.0, -90.0}) {
            setP(p, "laz", yaw); setP(ref, "laz", yaw);
            double eP[2]{}, eR[2]{};
            for (int b = 0; b < 30; ++b) {
                bp.clear(); br.clear();
                for (int i = 0; i < blk; ++i) {
                    const float x = signal[((size_t)b * blk + (size_t)i) % 8192];
                    bp.setSample(0, i, x); br.setSample(0, i, x);
                }
                p.processBlock(bp, midi); ref.processBlock(br, midi);
                if (b >= 15) {
                    for (int c = 0; c < 2; ++c) {
                        for (int i = 0; i < blk; ++i) {
                            eP[c] += (double)bp.getSample(c, i) * bp.getSample(c, i);
                            eR[c] += (double)br.getSample(c, i) * br.getSample(c, i);
                        }
                    }
                }
            }
            for (int c = 0; c < 2; ++c) {
                const double d = 10.0 * std::log10((eR[c] + 1e-30) / (eP[c] + 1e-30));
                worstDrop = std::max(worstDrop, d);
            }
        }
        std::printf("  plugin x=0 ORTF yaw sweep (0/90/180/-90): worst channel early drop vs x=0.50m = %.2f dB\n", worstDrop);
        check(worstDrop < 4.0, "plugin x=0 ORTF retains early reflections in both channels at all headings");
    }

    // 20 m/s smooth motion in VerbProcessor must behave as smooth Doppler without jump reset
    {
        static const char* freeField = R"(sr 48000
length 1.0
height 30
material absorb 1 1 1 1 1 1 1 0 0
floor absorb
ceiling open
source S 0 0 1.5
receiver L 0 0 1.5 0 0
receiver R 0 0 1.5 0 0
)";
        VerbProcessor p;
        p.setPlayConfigDetails(2, 2, 48000, 512);
        p.prepareToPlay(48000, 512);
        p.loadSceneText(freeField, "free.txt", true);
        waitReady(p, 60);
        setP(p, "doppler", 1.0);
        setP(p, "lx", 0.0); setP(p, "ly", 0.0); setP(p, "lz", 1.5);
        setP(p, "s1y", 3.0); setP(p, "s1z", 1.5); setP(p, "s1x", -20.0);
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        for (int i = 0; i < 40; ++i) { buf.clear(); p.processBlock(buf, midi); }
        const double speed = 20.0, f0 = 4000.0;
        const int total = (int)(2.0 * 48000);
        std::vector<float> out;
        for (int bs = 0; bs < total; bs += 512) {
            setP(p, "s1x", -20.0 + speed * (double)bs / 48000.0);
            for (int i = 0; i < 512; ++i) {
                const double t = (double)(bs + i) / 48000.0;
                buf.setSample(0, i, (float)(0.5 * std::sin(2.0 * verb::kPi * f0 * t)));
                buf.setSample(1, i, 0.f);
            }
            p.processBlock(buf, midi);
            for (int i = 0; i < 512; ++i) out.push_back(buf.getSample(0, i));
        }
        double amp = 0.0, jump = 0.0;
        for (size_t i = 12000; i + 1 < out.size() - 4800; ++i) {
            amp = std::max<double>(amp, std::fabs(out[i]));
            jump = std::max<double>(jump, std::fabs(out[i + 1] - out[i]));
        }
        const double natural = amp * 2.0 * std::sin(verb::kPi * f0 * 1.2 / 48000.0);
        const double clickRatio = jump / std::max(1e-12, natural);
        std::printf("  plugin 20 m/s smooth pass: click ratio %.2f (expect < 1.15)\n", clickRatio);
        check(clickRatio < 1.15, "plugin 20 m/s smooth movement stays click-free (not treated as jump)");
    }

    return fails ? 1 : 0;
}
