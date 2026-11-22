#pragma once
// P5: Baked late-tail doorway dip on Flate path — numerical reproduction & regression test.
// Run alone via VERB_ONLY_P5=1.
#include "E9bTest.h"
#include "../src/baked_path.hpp"
#include "../src/tail.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace verb_p5_test {

struct BiquadBP {
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    void design(double fc, double sr, double bwOct = 1.0) {
        const double w0 = 2.0 * 3.14159265358979323846 * fc / sr;
        const double sn = std::sin(w0), cs = std::cos(w0);
        const double alpha = sn * std::sinh(std::log(2.0) / 2.0 * bwOct * w0 / sn);
        const double a0 = 1.0 + alpha;
        b0 = alpha / a0;
        b1 = 0.0;
        b2 = -alpha / a0;
        a1 = (-2.0 * cs) / a0;
        a2 = (1.0 - alpha) / a0;
        z1 = z2 = 0.0;
    }
    double step(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct WindowEnergies {
    double earlyDb = -200, late1Db = -200, late2Db = -200, lateTotalDb = -200;
    double late1Db500 = -200, late2Db500 = -200, lateTotalDb500 = -200;
    double late1Db2k = -200, late2Db2k = -200, lateTotalDb2k = -200;
    double peak = 0, rmsDb = -200;
    uint64_t lookupsDelta = 0, transitionsDelta = 0;
};

inline WindowEnergies measureWindows(const std::vector<float>& L, const std::vector<float>& R, double sr) {
    WindowEnergies out;
    const size_t n = std::min(L.size(), R.size());
    const size_t i80 = std::min(n, (size_t)std::llround(0.080 * sr));
    const size_t i150 = std::min(n, (size_t)std::llround(0.150 * sr));
    const size_t i400 = std::min(n, (size_t)std::llround(0.400 * sr));
    const size_t i1000 = std::min(n, (size_t)std::llround(1.000 * sr));

    BiquadBP bp500L, bp500R, bp2kL, bp2kR;
    bp500L.design(500.0, sr); bp500R.design(500.0, sr);
    bp2kL.design(2000.0, sr); bp2kR.design(2000.0, sr);

    double eEarly = 0, eAll = 0;
    double eL1 = 0, eL2 = 0, eLT = 0;
    double e500_1 = 0, e500_2 = 0, e500_T = 0;
    double e2k_1 = 0, e2k_2 = 0, e2k_T = 0;

    for (size_t i = 0; i < i1000; ++i) {
        const double l = L[i], r = R[i];
        out.peak = std::max({out.peak, std::fabs(l), std::fabs(r)});
        const double l500 = bp500L.step(l), r500 = bp500R.step(r);
        const double l2k = bp2kL.step(l), r2k = bp2kR.step(r);
        const double p = 0.5 * (l * l + r * r);
        const double p500 = 0.5 * (l500 * l500 + r500 * r500);
        const double p2k = 0.5 * (l2k * l2k + r2k * r2k);
        eAll += p;
        if (i < i80) eEarly += p;
        if (i >= i150 && i < i400) { eL1 += p; e500_1 += p500; e2k_1 += p2k; }
        if (i >= i400 && i < i1000) { eL2 += p; e500_2 += p500; e2k_2 += p2k; }
        if (i >= i150 && i < i1000) { eLT += p; e500_T += p500; e2k_T += p2k; }
    }
    auto toDb = [](double sumSq, size_t count) {
        if (count == 0) return -200.0;
        return 10.0 * std::log10(std::max(sumSq / (double)count, 1e-20));
    };
    out.earlyDb = toDb(eEarly, i80);
    out.late1Db = toDb(eL1, i400 - i150);
    out.late2Db = toDb(eL2, i1000 - i400);
    out.lateTotalDb = toDb(eLT, i1000 - i150);
    out.late1Db500 = toDb(e500_1, i400 - i150);
    out.late2Db500 = toDb(e500_2, i1000 - i400);
    out.lateTotalDb500 = toDb(e500_T, i1000 - i150);
    out.late1Db2k = toDb(e2k_1, i400 - i150);
    out.late2Db2k = toDb(e2k_2, i1000 - i400);
    out.lateTotalDb2k = toDb(e2k_T, i1000 - i150);
    out.rmsDb = toDb(eAll, i1000);
    return out;
}

inline int run() {
    std::printf("=== P5: Baked Late-Tail Doorway Dip Diagnostic & Regression Test ===\n");
    std::fflush(stdout);

    namespace fs = std::filesystem;
    const juce::File presetFile = repoFile("out/P4/impulse/Flate-with-bake.verbroom");
    if (!presetFile.existsAsFile()) {
        std::printf("FAIL P5: missing %s\n", presetFile.getFullPathName().toRawUTF8());
        return 1;
    }
    const char* tagEnv = std::getenv("VERB_P5_TAG");
    const std::string tag = (tagEnv && *tagEnv) ? std::string("-") + tagEnv : std::string();
    const char* outEnv = std::getenv("VERB_P5_OUT");
    const fs::path outDir(outEnv && *outEnv ? outEnv : "E:/Claude/verb/out/P5");
    fs::create_directories(outDir);

    const double sr = 48000.0;
    const int bs = 128;
    const size_t irN = (size_t)(1.05 * sr);

    VerbProcessor proc;
    proc.setPlayConfigDetails(2, 2, sr, bs);
    proc.prepareToPlay(sr, bs);
    if (!waitReady(proc, 90)) return 1;
    auto xml = juce::parseXML(presetFile.loadFileAsString());
    if (!xml) return 1;
    juce::String err;
    if (!proc.loadRoomPresetText(xml->toString(), err) || !waitReady(proc, 120)) return 1;

    auto engine = E9bAccess::engine(proc);
    juce::AudioBuffer<float> buf(2, bs);
    juce::MidiBuffer midi;
    for (int i = 0; i < 40; ++i) {
        buf.clear();
        stage2Process(proc, buf, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    }

    auto tbl = engine->bakedTableSnapshot();
    if (!tbl || tbl->u.size() < 2) {
        proc.startPathBake(verbp::kListener);
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 180.0) {
            buf.clear();
            stage2Process(proc, buf, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
            if (proc.pathBakePercent() == 100) break;
            juce::Thread::sleep(1);
        }
        tbl = engine->bakedTableSnapshot();
    }
    if (!tbl || tbl->u.size() < 2) return 1;

    const auto stInitial = engine->bakeStats();
    std::printf("Baked table ready: %zu stations, selectedRays=%d, estimator=%d, hash=%016llx\n",
                tbl->u.size(), tbl->selectedRays, tbl->estimator, (unsigned long long)stInitial.hash);

    const double l3Val = proc.apvts.getRawParameterValue(verbp::paramId(verbp::kListener, 3))->load();
    const auto rawPath = proc.getMovementPath(verbp::kListener);
    verbp::CompiledPath cpath;
    cpath.build(rawPath);
    const double totalLen = cpath.length();
    const int steps = rawPath.smooth ? 128 : 1;
    std::vector<double> knotU;
    for (size_t k = 0; k < rawPath.points.size(); ++k) {
        const size_t idx = std::min(k * (size_t)steps, cpath.lengths.size() - 1);
        const double uKnot = totalLen > 0 ? cpath.lengths[idx] / totalLen : 0.0;
        knotU.push_back(uKnot);
        const auto q = cpath.evaluate(uKnot);
        std::printf("  P%zu: u=%.5f dist=%.3fm pos=(%.4f, %.4f, %.4f) liveYaw=%.2f\n",
                    k + 1, uKnot, uKnot * totalLen, q[0], q[1], q[2],
                    verbp::orientedAzimuth(verbp::orientModeOf(rawPath), l3Val, q[3], false, 0.0, rawPath.orientOffset));
    }
    double uDoorway = 0.9025;
    for (int i = 0; i <= 20000; ++i) {
        const double u = (double)i / 20000.0;
        const auto q = cpath.evaluate(u);
        if (q[0] >= 7.0) { uDoorway = u; break; }
    }
    {
        const auto q = cpath.evaluate(uDoorway);
        std::printf("  Doorway (x=7.0, y in [-4,-2]): u=%.5f dist=%.3fm pos=(%.4f, %.4f, %.4f) liveYaw=%.2f\n",
                    uDoorway, uDoorway * totalLen, q[0], q[1], q[2],
                    verbp::orientedAzimuth(verbp::orientModeOf(rawPath), l3Val, q[3], false, 0.0, rawPath.orientOffset));
    }

    // Build dense u grid (~82 values: 0.015 step in 0..0.42, 0.010 step in 0.42..1.00, plus knots, doorway, and stations)
    std::vector<double> uGrid;
    for (double u = 0.0; u <= 0.420001; u += 0.015) uGrid.push_back(std::min(1.0, u));
    for (double u = 0.43; u <= 1.000001; u += 0.010) uGrid.push_back(std::min(1.0, u));
    for (double uk : knotU) uGrid.push_back(uk);
    uGrid.push_back(uDoorway);
    for (double us : tbl->u) uGrid.push_back(us);
    std::sort(uGrid.begin(), uGrid.end());
    std::vector<double> uList;
    for (double u : uGrid) {
        if (uList.empty() || u - uList.back() > 0.0025) uList.push_back(u);
    }
    if (std::fabs(uList.back() - 1.0) > 1e-6) uList.push_back(1.0);
    std::printf("Sweep uList has %zu points.\n", uList.size());
    std::fflush(stdout);

    // Helper 1: settled render at a subset of u points (every 3rd point + knots + doorway = ~34 points, so it runs in ~45s)
    // Wait properly for a real worker rebuild at u by triggering notifyJump() after setting lpath=u and waiting for tailRefreshCount() to advance!
    auto renderSettledAt = [&](double u) -> WindowEnergies {
        setP(proc, "lpath", u);
        // Pump 12 blocks so mpx/mpy smoother reaches u
        for (int i = 0; i < 12; ++i) { buf.clear(); stage2Process(proc, buf, midi); }
        const uint64_t refBefore = engine->tailRefreshCount();
        engine->notifyJump();
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 10.0) {
            buf.clear();
            stage2Process(proc, buf, midi);
            juce::Thread::sleep(2);
            if (engine->tailRefreshCount() > refBefore && engine->tailIsCurrent()) break;
        }
        // Pump 0.35s of silence so FDL & smoothers are completely clean
        for (int i = 0; i < (int)(0.35 * sr / bs); ++i) { buf.clear(); stage2Process(proc, buf, midi); }
        const auto bs0 = engine->bakeStats();
        const auto ms0 = engine->motionStats();
        std::vector<float> L(irN, 0.f), R(irN, 0.f);
        size_t pos = 0;
        bool fired = false;
        while (pos < irN) {
            const int m = (int)std::min<size_t>(bs, irN - pos);
            buf.clear();
            if (!fired) { buf.setSample(0, 0, 1.0f); buf.setSample(1, 0, 1.0f); fired = true; }
            stage2Process(proc, buf, midi);
            for (int i = 0; i < m; ++i) { L[pos + (size_t)i] = buf.getSample(0, i); R[pos + (size_t)i] = buf.getSample(1, i); }
            pos += (size_t)m;
        }
        const auto bs1 = engine->bakeStats();
        const auto ms1 = engine->motionStats();
        auto res = measureWindows(L, R, sr);
        res.lookupsDelta = bs1.lookups - bs0.lookups;
        res.transitionsDelta = ms1.transitionsStarted - ms0.transitionsStarted;
        return res;
    };

    // Helper 2: moving render along the path from u=0 -> 1.
    // First settle at u=0.0 (reference IR at P1). Then for each u_k in uList, advance smoothly from u_{k-1} to u_k over 12 blocks
    // (~32 ms, real walking speed ~3.5 m/s), fire the impulse at u_k, and capture the 1.05s response while advancing u by a tiny
    // drift (or keeping the P1 reference slot active without waiting on the worker thread) so we measure the exact moving playback response at u_k!
    auto runMovingSweep = [&]() -> std::vector<WindowEnergies> {
        std::vector<WindowEnergies> out(uList.size());
        // Settle cleanly at u=0.0
        (void)renderSettledAt(0.0);
        double uPrev = 0.0;
        for (size_t idx = 0; idx < uList.size(); ++idx) {
            const double uTarget = uList[idx];
            // Clear FDL from previous impulse (1.1s of silence while holding uPrev)
            for (int i = 0; i < (int)(1.1 * sr / bs); ++i) {
                setP(proc, "lpath", uPrev);
                buf.clear();
                stage2Process(proc, buf, midi);
            }
            // Now walk smoothly from uPrev to uTarget at realistic speed (~3.0 m/s, 8 mm per 128-sample block)
            const double dist = std::fabs(uTarget - uPrev) * totalLen;
            const int walkBlocks = std::max(8, (int)std::ceil(dist / (3.0 * bs / sr)));
            for (int b = 1; b <= walkBlocks; ++b) {
                const double uStep = uPrev + (uTarget - uPrev) * ((double)b / (double)walkBlocks);
                setP(proc, "lpath", uStep);
                buf.clear();
                stage2Process(proc, buf, midi);
            }
            const auto bs0 = engine->bakeStats();
            const auto ms0 = engine->motionStats();
            std::vector<float> L(irN, 0.f), R(irN, 0.f);
            size_t pos = 0;
            bool fired = false;
            while (pos < irN) {
                const int m = (int)std::min<size_t>(bs, irN - pos);
                setP(proc, "lpath", uTarget);
                buf.clear();
                if (!fired) { buf.setSample(0, 0, 1.0f); buf.setSample(1, 0, 1.0f); fired = true; }
                stage2Process(proc, buf, midi);
                for (int i = 0; i < m; ++i) { L[pos + (size_t)i] = buf.getSample(0, i); R[pos + (size_t)i] = buf.getSample(1, i); }
                pos += (size_t)m;
            }
            const auto bs1 = engine->bakeStats();
            const auto ms1 = engine->motionStats();
            out[idx] = measureWindows(L, R, sr);
            out[idx].lookupsDelta = bs1.lookups - bs0.lookups;
            out[idx].transitionsDelta = ms1.transitionsStarted - ms0.transitionsStarted;
            uPrev = uTarget;
        }
        return out;
    };

    // Helper 3: continuous 5.0s walk along u=0..1 + 2.0s hold at P5 with soloTail=1 (measures real-time moving tail envelope every 25 ms)
    auto runContinuousTailWalk = [&](double walkSec, const std::string& csvName) -> std::vector<std::tuple<double, double, int>> {
        setP(proc, "soloTail", 1.f);
        setP(proc, "soloDirect", 0.f);
        setP(proc, "soloEarly", 0.f);
        (void)renderSettledAt(0.0);

        std::ofstream wf((outDir / csvName).string());
        wf << "time_s,u,dist_m,x,y,tail_rms_db,lookups_delta,trans_started,trans_done\n";
        std::vector<std::tuple<double, double, int>> trace;

        const size_t walkSamples = (size_t)(walkSec * sr);
        const size_t holdSamples = (size_t)(2.0 * sr);
        const size_t totalSamples = walkSamples + holdSamples;
        const size_t winSamples = (size_t)(0.025 * sr);
        size_t pos = 0, sqCount = 0;
        double sqSum = 0;
        uint32_t rng = 123456789u;
        auto bsPrev = engine->bakeStats();
        auto msPrev = engine->motionStats();

        while (pos + (size_t)bs <= totalSamples) {
            const double u = std::clamp((double)pos / (double)walkSamples, 0.0, 1.0);
            setP(proc, "lpath", u);
            buf.clear();
            const bool feedInput = pos < walkSamples + (size_t)(0.8 * sr);
            for (int i = 0; i < bs; ++i) {
                rng = rng * 1664525u + 1013904223u;
                const float wn = feedInput ? (((float)(int32_t)rng / 2147483648.f) * 0.1f) : 0.f;
                buf.setSample(0, i, wn);
                buf.setSample(1, i, wn);
            }
            stage2Process(proc, buf, midi);
            for (int i = 0; i < bs; ++i) {
                const double l = buf.getSample(0, i), r = buf.getSample(1, i);
                sqSum += 0.5 * (l * l + r * r);
                if (++sqCount >= winSamples) {
                    const double tSec = (double)(pos + (size_t)i) / sr;
                    const double uNow = std::clamp((double)(pos + (size_t)i) / (double)walkSamples, 0.0, 1.0);
                    const auto q = cpath.evaluate(uNow);
                    const double db = 10.0 * std::log10(std::max(sqSum / (double)sqCount, 1e-20));
                    const auto bsCur = engine->bakeStats();
                    const auto msCur = engine->motionStats();
                    const int lk = (int)(bsCur.lookups - bsPrev.lookups);
                    wf << std::fixed << std::setprecision(5)
                       << tSec << "," << uNow << "," << (uNow * totalLen) << "," << q[0] << "," << q[1] << "," << db << ","
                       << lk << ","
                       << (msCur.transitionsStarted - msPrev.transitionsStarted) << ","
                       << (msCur.transitionsDone - msPrev.transitionsDone) << "\n";
                    if (pos + (size_t)i <= walkSamples) trace.emplace_back(uNow, db, lk);
                    bsPrev = bsCur;
                    msPrev = msCur;
                    sqSum = 0;
                    sqCount = 0;
                }
            }
            pos += (size_t)bs;
        }
        setP(proc, "soloTail", 0.f);
        return trace;
    };

    // Select settled checkpoints (~25 points across uList, denser in P3..P5, interpolated cleanly for intermediate u points if needed, or exact at every point)
    // Wait: how fast is renderSettledAt if we run it at 28 points? ~28 * 0.45s = 12.6 seconds! Let's run settled at all 82 points if fast, or every 2nd point!
    // Wait: 82 * 0.45s is only 36 seconds, and we only need to run renderSettledAt ONCE because settled IR is identical for nobake settled and baked settled (atRef == true)!
    // Wait: let's verify! In baked settled, atRef is true at the settled pose, so bakeEqualize sets target = 1.0!
    // Let's run settled at every 3rd point in 0..0.42 and every 2nd point in 0.42..1.0 (plus knots & doorway = 42 points) for BOTH baked settled and nobake settled (or all points if fast)!
    std::vector<WindowEnergies> bakedSettled(uList.size()), nobakeSettled(uList.size());
    std::vector<bool> isSettledSample(uList.size(), false);
    for (size_t i = 0; i < uList.size(); ++i) {
        bool pick = (i % 2 == 0) || (i + 1 == uList.size()) || std::fabs(uList[i] - uDoorway) < 1e-4;
        for (double uk : knotU) if (std::fabs(uList[i] - uk) < 1e-4) pick = true;
        isSettledSample[i] = pick;
    }
    auto interpSettled = [&](std::vector<WindowEnergies>& vec) {
        size_t lastIdx = 0;
        for (size_t i = 1; i < uList.size(); ++i) {
            if (isSettledSample[i]) {
                for (size_t j = lastIdx + 1; j < i; ++j) {
                    const double f = (uList[j] - uList[lastIdx]) / std::max(1e-9, uList[i] - uList[lastIdx]);
                    auto lerp = [&](double a, double b) { return a + f * (b - a); };
                    vec[j].earlyDb = lerp(vec[lastIdx].earlyDb, vec[i].earlyDb);
                    vec[j].late1Db = lerp(vec[lastIdx].late1Db, vec[i].late1Db);
                    vec[j].late2Db = lerp(vec[lastIdx].late2Db, vec[i].late2Db);
                    vec[j].lateTotalDb = lerp(vec[lastIdx].lateTotalDb, vec[i].lateTotalDb);
                    vec[j].late1Db500 = lerp(vec[lastIdx].late1Db500, vec[i].late1Db500);
                    vec[j].late2Db500 = lerp(vec[lastIdx].late2Db500, vec[i].late2Db500);
                    vec[j].lateTotalDb500 = lerp(vec[lastIdx].lateTotalDb500, vec[i].lateTotalDb500);
                    vec[j].late1Db2k = lerp(vec[lastIdx].late1Db2k, vec[i].late1Db2k);
                    vec[j].late2Db2k = lerp(vec[lastIdx].late2Db2k, vec[i].late2Db2k);
                    vec[j].lateTotalDb2k = lerp(vec[lastIdx].lateTotalDb2k, vec[i].lateTotalDb2k);
                    vec[j].rmsDb = lerp(vec[lastIdx].rmsDb, vec[i].rmsDb);
                    vec[j].lookupsDelta = vec[i].lookupsDelta;
                }
                lastIdx = i;
            }
        }
    };

    std::printf("Running (b) baked settled & (d) baked moving sweeps...\n");
    std::fflush(stdout);
    for (size_t i = 0; i < uList.size(); ++i) {
        if (isSettledSample[i]) bakedSettled[i] = renderSettledAt(uList[i]);
    }
    interpSettled(bakedSettled);
    auto bakedMoving = runMovingSweep();
    auto bakedWalk5s = runContinuousTailWalk(5.0, "tail-walk-baked-5s" + tag + ".csv");

    // Measure dynamic lookup success rate across 4 walk speeds WITH bake
    int totalWalkBlocks = 0, failedLookupBlocks = 0, failedP3 = 0, failedP4 = 0;
    {
        const double speedsSec[4] = {15.0, 8.0, 5.0, 3.0};
        for (double dur : speedsSec) {
            setP(proc, "lpath", 0.0);
            engine->notifyJump();
            for (int k = 0; k < 20; ++k) { buf.clear(); stage2Process(proc, buf, midi); }
            const int nBlocks = (int)(dur * sr / bs);
            int fTot = 0, f3 = 0, f4 = 0;
            for (int b = 0; b <= nBlocks; ++b) {
                const double uCmd = std::clamp((double)b / (double)nBlocks, 0.0, 1.0);
                setP(proc, "lpath", uCmd);
                const auto l0 = engine->bakeStats().lookups;
                buf.clear();
                stage2Process(proc, buf, midi);
                const auto l1 = engine->bakeStats().lookups;
                ++totalWalkBlocks;
                if (l1 == l0) {
                    ++fTot; ++failedLookupBlocks;
                    if (uCmd >= 0.40 && uCmd <= 0.50) { ++f3; ++failedP3; }
                    if (uCmd >= 0.78 && uCmd <= 0.87) { ++f4; ++failedP4; }
                }
            }
            std::printf("  [Dynamic Projection %.1fs (%.2f m/s)] missed lookups: %d / %d (P3:%d, P4:%d)\n",
                        dur, totalLen / dur, fTot, nBlocks + 1, f3, f4);
        }
    }

    // Clear bake for (a) nobake settled and (c) nobake moving
    proc.clearPathBake();
    {
        const auto until = juce::Time::getMillisecondCounterHiRes() + 15000;
        while (juce::Time::getMillisecondCounterHiRes() < until && proc.pathBakeStatus()["state"].toString() != "notBaked") {
            buf.clear(); stage2Process(proc, buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(2); juce::Thread::sleep(1);
        }
    }
    std::printf("Running (a) nobake settled & (c) nobake moving sweeps...\n");
    std::fflush(stdout);
    for (size_t i = 0; i < uList.size(); ++i) {
        if (isSettledSample[i]) nobakeSettled[i] = renderSettledAt(uList[i]);
    }
    interpSettled(nobakeSettled);
    auto nobakeMoving = runMovingSweep();
    auto nobakeWalk5s = runContinuousTailWalk(5.0, "tail-walk-nobake-5s" + tag + ".csv");

    // Write sweep CSV
    const std::string sweepCsvName = "p5-sweep" + tag + ".csv";
    {
        std::ofstream f((outDir / sweepCsvName).string());
        f << "u,dist_m,x,y,z,yaw,"
          << "nb_set_early,nb_set_late1,nb_set_late2,nb_set_lateTot,nb_set_500,nb_set_2k,"
          << "bk_set_early,bk_set_late1,bk_set_late2,bk_set_lateTot,bk_set_500,bk_set_2k,"
          << "nb_mov_early,nb_mov_late1,nb_mov_late2,nb_mov_lateTot,nb_mov_500,nb_mov_2k,"
          << "bk_mov_early,bk_mov_late1,bk_mov_late2,bk_mov_lateTot,bk_mov_500,bk_mov_2k,bk_mov_lookups\n";
        for (size_t i = 0; i < uList.size(); ++i) {
            const double u = uList[i];
            const auto q = cpath.evaluate(u);
            const double yaw = verbp::orientedAzimuth(verbp::orientModeOf(rawPath), l3Val, q[3], false, 0.0, rawPath.orientOffset);
            const auto& ns = nobakeSettled[i];
            const auto& bs2 = bakedSettled[i];
            const auto& nm = nobakeMoving[i];
            const auto& bm = bakedMoving[i];
            f << std::fixed << std::setprecision(5)
              << u << "," << (u * totalLen) << "," << q[0] << "," << q[1] << "," << q[2] << "," << yaw << ","
              << ns.earlyDb << "," << ns.late1Db << "," << ns.late2Db << "," << ns.lateTotalDb << "," << ns.lateTotalDb500 << "," << ns.lateTotalDb2k << ","
              << bs2.earlyDb << "," << bs2.late1Db << "," << bs2.late2Db << "," << bs2.lateTotalDb << "," << bs2.lateTotalDb500 << "," << bs2.lateTotalDb2k << ","
              << nm.earlyDb << "," << nm.late1Db << "," << nm.late2Db << "," << nm.lateTotalDb << "," << nm.lateTotalDb500 << "," << nm.lateTotalDb2k << ","
              << bm.earlyDb << "," << bm.late1Db << "," << bm.late2Db << "," << bm.lateTotalDb << "," << bm.lateTotalDb500 << "," << bm.lateTotalDb2k << ","
              << bm.lookupsDelta << "\n";
        }
    }

    // Compute numerical smoothness metrics between P3 (u=0.440) and P5 (u=1.000):
    // 1. Max downward dip (gate) in baked moving impulse late tail (150-1000 ms) over any <= 0.5 m window in P3..P5
    // 2. Max downward dip (gate) in continuous 5s walk tail envelope over any <= 0.6 m window in P3..P5
    // 3. Max overshoot of baked moving late tail (150-1000 ms) above baked settled late tail (150-1000 ms) in P3..P5
    double maxImpulseDipPerHalfM = 0.0, dipImpulseU = 0.0;
    double maxImpulseStepPerHalfM = 0.0, stepImpulseU = 0.0;
    double maxMovVsSetOvershootDb = 0.0, overshootU = 0.0;
    for (size_t i = 0; i < uList.size(); ++i) {
        if (uList[i] < 0.43) continue;
        const double ov = bakedMoving[i].lateTotalDb - bakedSettled[i].lateTotalDb;
        if (ov > maxMovVsSetOvershootDb) { maxMovVsSetOvershootDb = ov; overshootU = uList[i]; }
        for (size_t j = i + 1; j < uList.size(); ++j) {
            const double dM = (uList[j] - uList[i]) * totalLen;
            if (dM > 0.55) break;
            const double delta = bakedMoving[j].lateTotalDb - bakedMoving[i].lateTotalDb;
            if (-delta > maxImpulseDipPerHalfM) { maxImpulseDipPerHalfM = -delta; dipImpulseU = uList[j]; }
            if (std::fabs(delta) > maxImpulseStepPerHalfM) { maxImpulseStepPerHalfM = std::fabs(delta); stepImpulseU = uList[j]; }
        }
    }

    // For continuous walk, smooth over 3 adjacent 25ms windows (75ms running average) to separate steady-noise variance (~1.2 dB) from macroscopic gating
    double maxWalkGateDb = 0.0, walkGateU = 0.0;
    if (bakedWalk5s.size() >= 5) {
        std::vector<double> smU, smDb;
        for (size_t i = 1; i + 1 < bakedWalk5s.size(); ++i) {
            smU.push_back(std::get<0>(bakedWalk5s[i]));
            smDb.push_back((std::get<1>(bakedWalk5s[i - 1]) + std::get<1>(bakedWalk5s[i]) + std::get<1>(bakedWalk5s[i + 1])) / 3.0);
        }
        for (size_t i = 0; i < smU.size(); ++i) {
            if (smU[i] < 0.42) continue;
            for (size_t j = i + 1; j < smU.size(); ++j) {
                const double dM = (smU[j] - smU[i]) * totalLen;
                if (dM > 0.80) break;
                const double drop = smDb[i] - smDb[j]; // positive = downward dip as u increases
                if (drop > maxWalkGateDb) { maxWalkGateDb = drop; walkGateU = smU[j]; }
            }
        }
    }

    std::printf("=== P5 Numerical Summary (%s) ===\n", sweepCsvName.c_str());
    std::printf("  1. Dynamic projection missed lookups across 4 walk speeds: %d / %d blocks (P3:%d, P4:%d)\n",
                failedLookupBlocks, totalWalkBlocks, failedP3, failedP4);
    std::printf("  2. Max downward dip in baked moving late-tail (150-1000ms) over <=0.5m in P3..P5: %.2f dB at u=%.4f\n",
                maxImpulseDipPerHalfM, dipImpulseU);
    std::printf("  3. Max level step |dL| in baked moving late-tail (150-1000ms) over <=0.5m in P3..P5: %.2f dB at u=%.4f\n",
                maxImpulseStepPerHalfM, stepImpulseU);
    std::printf("  4. Max 5s-walk tail envelope gate (drop over <=0.8m) in P3..P5: %.2f dB at u=%.4f\n",
                maxWalkGateDb, walkGateU);
    std::printf("  5. Max baked moving vs settled late-tail (150-1000ms) overshoot in P3..P5: +%.2f dB at u=%.4f\n",
                maxMovVsSetOvershootDb, overshootU);
    std::fflush(stdout);

    proc.releaseResources();

    // Regression gate (unless VERB_P5_DIAG_ONLY=1 is set for pre-fix reproduction):
    if (!std::getenv("VERB_P5_DIAG_ONLY")) {
        check(failedLookupBlocks == 0, "P5: zero missed bake table lookups during dynamic path walks across all speeds");
        check(maxImpulseDipPerHalfM <= 1.0, "P5: no downward dip > 1.0 dB per 0.5 m in baked moving late tail (150-1000 ms) between P3 and P5");
        check(maxImpulseStepPerHalfM <= 2.5, "P5: no level step > 2.5 dB per 0.5 m in baked moving late tail (150-1000 ms) between P3 and P5");
        check(maxWalkGateDb <= 2.5, "P5: no doorway/turn gating > 2.5 dB in continuous walk late-tail envelope between P3 and P5");
    }
    return fails ? 1 : 0;
}

} // namespace verb_p5_test

static int testP5() {
    return verb_p5_test::run();
}
