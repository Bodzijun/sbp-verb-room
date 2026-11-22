#pragma once
// P6: Baked late-tail level during motion vs settled exact IR (+7.6 dB overshoot at end of Flate path).
// Run alone via VERB_ONLY_P6=1.
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

namespace verb_p6_test {

struct WindowBandEnergies {
    double earlyDb = -200.0;      // 0..80 ms
    double mid80_150Db = -200.0;  // 80..150 ms
    double late1Db = -200.0;      // 150..400 ms
    double late2Db = -200.0;      // 400..1000 ms
    double lateTotDb = -200.0;    // 150..1000 ms
    double fullTotDb = -200.0;    // 0..1000 ms
    std::array<double, verb::NB> bandLateTotDb{}; // 150..1000 ms per band (0..6: 125..8000 Hz)
    std::array<double, verb::NB> bandLate1Db{};   // 150..400 ms per band
    std::array<double, verb::NB> bandLate2Db{};   // 400..1000 ms per band
    std::array<double, verb::NB> bandEarlyDb{};   // 0..80 ms per band
    std::array<double, verb::NB> bandMidDb{};     // 80..150 ms per band
    std::array<double, verb::NB> bandFullDb{};    // 0..1000 ms per band
    uint64_t lookupsDelta = 0;
    uint64_t transitionsDelta = 0;
};

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

inline WindowBandEnergies measureWindowBands(const std::vector<float>& L, const std::vector<float>& R, double sr) {
    WindowBandEnergies out;
    const size_t n = std::min(L.size(), R.size());
    const size_t i80 = std::min(n, (size_t)std::llround(0.080 * sr));
    const size_t i150 = std::min(n, (size_t)std::llround(0.150 * sr));
    const size_t i400 = std::min(n, (size_t)std::llround(0.400 * sr));
    const size_t i1000 = std::min(n, (size_t)std::llround(1.000 * sr));

    std::array<BiquadBP, verb::NB> bpL{}, bpR{};
    for (int b = 0; b < verb::NB; ++b) {
        bpL[b].design(verb::kBandHz[b], sr);
        bpR[b].design(verb::kBandHz[b], sr);
    }

    double eEarly = 0, eMid = 0, eL1 = 0, eL2 = 0, eLT = 0, eFull = 0;
    std::array<double, verb::NB> bEarly{}, bMid{}, bL1{}, bL2{}, bLT{}, bFull{};

    for (size_t i = 0; i < i1000; ++i) {
        const double l = L[i], r = R[i];
        const double p = 0.5 * (l * l + r * r);
        eFull += p;
        if (i < i80) eEarly += p;
        else if (i < i150) eMid += p;
        else if (i < i400) { eL1 += p; eLT += p; }
        else { eL2 += p; eLT += p; }

        for (int b = 0; b < verb::NB; ++b) {
            const double vL = bpL[b].step(l);
            const double vR = bpR[b].step(r);
            const double pb = 0.5 * (vL * vL + vR * vR);
            bFull[b] += pb;
            if (i < i80) bEarly[b] += pb;
            else if (i < i150) bMid[b] += pb;
            else if (i < i400) { bL1[b] += pb; bLT[b] += pb; }
            else { bL2[b] += pb; bLT[b] += pb; }
        }
    }

    auto toDb = [](double sumSq, size_t count) {
        if (count == 0) return -200.0;
        return 10.0 * std::log10(std::max(sumSq / (double)count, 1e-20));
    };

    out.earlyDb = toDb(eEarly, i80);
    out.mid80_150Db = toDb(eMid, i150 - i80);
    out.late1Db = toDb(eL1, i400 - i150);
    out.late2Db = toDb(eL2, i1000 - i400);
    out.lateTotDb = toDb(eLT, i1000 - i150);
    out.fullTotDb = toDb(eFull, i1000);

    for (int b = 0; b < verb::NB; ++b) {
        out.bandEarlyDb[b] = toDb(bEarly[b], i80);
        out.bandMidDb[b] = toDb(bMid[b], i150 - i80);
        out.bandLate1Db[b] = toDb(bL1[b], i400 - i150);
        out.bandLate2Db[b] = toDb(bL2[b], i1000 - i400);
        out.bandLateTotDb[b] = toDb(bLT[b], i1000 - i150);
        out.bandFullDb[b] = toDb(bFull[b], i1000);
    }
    return out;
}

struct SceneSweepSummary {
    std::string name;
    int pathObj = verbp::kListener;
    juce::String pathParamId = "lpath";
    size_t stations = 0;
    uint64_t bakeHash = 0;
    double maxAbsLateTotErrDb = 0.0;
    double maxLateTotErrU = 0.0;
    double signedLateTotErrAtMax = 0.0;
    double endLateTotErrDb = 0.0;
    double endLate1ErrDb = 0.0;
    double endLate2ErrDb = 0.0;
    double maxAbsLate1ErrDb = 0.0;
    double maxAbsLate2ErrDb = 0.0;
    double maxAbsLateTotUpTo0975 = 0.0;
    double maxAbsAnyBandUpTo0975 = 0.0;
    double maxAbsFullEarlyErrDb = 0.0;
    double minFullMidErrDb = 1e9, maxFullMidErrDb = -1e9;
    double endFullEarlyErrDb = 0.0, endFullMidErrDb = 0.0;
    double soloEndEarlyErrDb = 0.0, soloEndMidErrDb = 0.0;
    double soloEndLate1ErrDb = 0.0, soloEndLate2ErrDb = 0.0, soloEndLateTotErrDb = 0.0;
    std::array<double, verb::NB> maxAbsBandErrDb{};
    std::array<double, verb::NB> signedBandErrAtEnd{};
    double maxAbsAnyBandErrDb = 0.0;
    int worstBandIdx = 0;
    double worstBandU = 0.0;
    std::array<double, verb::NB> maxAbsBandConvergedDb{};
    double maxAbsAnyBandConvergedDb = 0.0;
    int worstConvergedBandIdx = 0;
    double worstConvergedBandU = 0.0;
    int convergedReevalCount = 0;
    // Continuous noise settle timeline metrics
    double movingEndTailRmsDb = -200.0;
    double settledTailRmsDb = -200.0;
    double settleStartSec = -1.0;
    double settleDoneSec = -1.0;
    double audioWithin1DbSec = -1.0;
};

inline bool runSceneEvaluation(
    const juce::File& presetFile,
    const std::string& sceneTag,
    int pathObj,
    const juce::String& pathParamId,
    const std::filesystem::path& outDir,
    const std::string& runTag,
    bool forceFreshBake,
    SceneSweepSummary& summaryOut)
{
    summaryOut = SceneSweepSummary{};
    summaryOut.name = sceneTag;
    summaryOut.pathObj = pathObj;
    summaryOut.pathParamId = pathParamId;

    if (!presetFile.existsAsFile()) {
        std::printf("FAIL P6: missing %s\n", presetFile.getFullPathName().toRawUTF8());
        return false;
    }

    const double sr = 48000.0;
    const int bs = 128;
    const size_t irN = (size_t)(1.05 * sr);

    VerbProcessor proc;
    proc.setPlayConfigDetails(2, 2, sr, bs);
    proc.prepareToPlay(sr, bs);
    if (!waitReady(proc, 90)) return false;

    auto xml = juce::parseXML(presetFile.loadFileAsString());
    if (!xml) return false;
    juce::String err;
    if (!proc.loadRoomPresetText(xml->toString(), err) || !waitReady(proc, 120)) return false;

    auto engine = E9bAccess::engine(proc);
    juce::AudioBuffer<float> buf(2, bs);
    juce::MidiBuffer midi;
    for (int i = 0; i < 40; ++i) {
        buf.clear();
        stage2Process(proc, buf, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    }

    if (forceFreshBake) {
        proc.clearPathBake();
        for (int i = 0; i < 20; ++i) {
            buf.clear();
            stage2Process(proc, buf, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
        }
    }

    auto tbl = engine->bakedTableSnapshot();
    if (!tbl || tbl->u.size() < 2) {
        proc.startPathBake(pathObj);
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
    if (!tbl || tbl->u.size() < 2) {
        std::printf("FAIL P6 [%s]: bake table not ready\n", sceneTag.c_str());
        return false;
    }

    const auto stInitial = engine->bakeStats();
    summaryOut.stations = tbl->u.size();
    summaryOut.bakeHash = stInitial.hash;
    std::printf("[%s] Baked table ready: %zu stations, selectedRays=%d, estimator=%d, restored=%d, hash=%016llx\n",
                sceneTag.c_str(), tbl->u.size(), tbl->selectedRays, tbl->estimator, stInitial.restored,
                (unsigned long long)stInitial.hash);

    const auto rawPath = proc.getMovementPath(pathObj);
    verbp::CompiledPath cpath;
    cpath.build(rawPath);
    const double totalLen = std::max(0.01, cpath.length());
    const int steps = rawPath.smooth ? 128 : 1;
    std::vector<double> knotU;
    for (size_t k = 0; k < rawPath.points.size(); ++k) {
        const size_t idx = std::min(k * (size_t)steps, cpath.lengths.size() - 1);
        const double uKnot = totalLen > 0 ? cpath.lengths[idx] / totalLen : 0.0;
        knotU.push_back(uKnot);
    }

    // Build u grid: 41 even steps (0.025) + knots + bake stations
    std::vector<double> uGrid;
    for (int i = 0; i <= 40; ++i) uGrid.push_back((double)i / 40.0);
    for (double uk : knotU) uGrid.push_back(uk);
    for (double us : tbl->u) uGrid.push_back(us);
    std::sort(uGrid.begin(), uGrid.end());
    std::vector<double> uList;
    for (double u : uGrid) {
        if (uList.empty() || u - uList.back() > 0.005) uList.push_back(u);
    }
    if (std::fabs(uList.back() - 1.0) > 1e-6) uList.push_back(1.0);
    std::printf("[%s] Sweep uList has %zu points (path length %.2f m).\n",
                sceneTag.c_str(), uList.size(), totalLen);
    std::fflush(stdout);

    auto renderSettledAt = [&](double u) -> WindowBandEnergies {
        setP(proc, pathParamId, u);
        // The processor's 20 ms pose tracker keeps moving after a small path step.
        // Let it settle before asking for an exact re-trace at this position.
        const int posePrerollBlocks = (int)std::ceil(0.42 * sr / bs);
        for (int i = 0; i < posePrerollBlocks; ++i) {
            buf.clear();
            stage2Process(proc, buf, midi);
        }
        const uint64_t refBefore = engine->tailRefreshCount();
        engine->notifyJump();
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 45.0) {
            buf.clear();
            stage2Process(proc, buf, midi);
            juce::Thread::sleep(2);
            if (engine->tailRefreshCount() > refBefore && engine->tailIsCurrent()) break;
        }
        // Pump 0.90s of silence so any per-source handover (motionFadeSec = 0.75s) and FDL drain completely
        for (int i = 0; i < (int)(0.90 * sr / bs); ++i) { buf.clear(); stage2Process(proc, buf, midi); }
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
        auto res = measureWindowBands(L, R, sr);
        res.lookupsDelta = bs1.lookups - bs0.lookups;
        res.transitionsDelta = ms1.transitionsStarted - ms0.transitionsStarted;
        return res;
    };

    auto runMovingSweep = [&]() -> std::vector<WindowBandEnergies> {
        std::vector<WindowBandEnergies> out(uList.size());
        (void)renderSettledAt(0.0);
        double uPrev = 0.0;
        for (size_t idx = 0; idx < uList.size(); ++idx) {
            const double uTarget = uList[idx];
            for (int i = 0; i < (int)(1.1 * sr / bs); ++i) {
                setP(proc, pathParamId, uPrev);
                buf.clear();
                stage2Process(proc, buf, midi);
            }
            const double dist = std::fabs(uTarget - uPrev) * totalLen;
            const int walkBlocks = std::max(8, (int)std::ceil(dist / (3.0 * bs / sr)));
            for (int b = 1; b <= walkBlocks; ++b) {
                const double uStep = uPrev + (uTarget - uPrev) * ((double)b / (double)walkBlocks);
                setP(proc, pathParamId, uStep);
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
                setP(proc, pathParamId, uTarget);
                buf.clear();
                if (!fired) { buf.setSample(0, 0, 1.0f); buf.setSample(1, 0, 1.0f); fired = true; }
                stage2Process(proc, buf, midi);
                for (int i = 0; i < m; ++i) { L[pos + (size_t)i] = buf.getSample(0, i); R[pos + (size_t)i] = buf.getSample(1, i); }
                pos += (size_t)m;
            }
            const auto bs1 = engine->bakeStats();
            const auto ms1 = engine->motionStats();
            out[idx] = measureWindowBands(L, R, sr);
            out[idx].lookupsDelta = bs1.lookups - bs0.lookups;
            out[idx].transitionsDelta = ms1.transitionsStarted - ms0.transitionsStarted;
            uPrev = uTarget;
        }
        return out;
    };

    // Continuous steady-state white noise walk (5.0s walk u=0->1, then 4.0s hold at u=1 WITH CONTINUOUS NOISE still playing!)
    // coupled with real worker sleeps so background perSourceWorkerStep actually runs at wall-clock speed and we can measure settle timeline!
    auto runContinuousNoiseSettleWalk = [&](const std::string& csvName) {
        setP(proc, "soloTail", 1.f);
        setP(proc, "soloDirect", 0.f);
        setP(proc, "soloEarly", 0.f);
        (void)renderSettledAt(0.0);

        std::ofstream wf((outDir / csvName).string());
        wf << "audio_time_s,wall_after_stop_s,u,tail_rms_db,lookups_delta,trans_started_tot,trans_done_tot\n";

        const double walkSec = 4.0;
        const double holdSec = 6.0;
        const size_t walkSamples = (size_t)(walkSec * sr);
        const size_t holdSamples = (size_t)(holdSec * sr);
        const size_t totalSamples = walkSamples + holdSamples;
        const size_t winSamples = (size_t)(0.025 * sr); // 25 ms window
        size_t pos = 0, sqCount = 0;
        double sqSum = 0;
        uint32_t rng = 123456789u;
        auto bsPrev = engine->bakeStats();
        const auto msBase = engine->motionStats();

        std::vector<double> endMoveDbs;
        std::vector<double> settledEndDbs;
        std::vector<std::pair<double, double>> postStopCurve; // (timeAfterStopSec, db)
        auto wallStopT0 = std::chrono::steady_clock::now();
        bool stopRecorded = false;

        while (pos + (size_t)bs <= totalSamples) {
            const bool isStopped = (pos >= walkSamples);
            if (isStopped && !stopRecorded) {
                stopRecorded = true;
                wallStopT0 = std::chrono::steady_clock::now();
            }
            const double u = std::clamp((double)pos / (double)walkSamples, 0.0, 1.0);
            setP(proc, pathParamId, u);
            buf.clear();
            for (int i = 0; i < bs; ++i) {
                rng = rng * 1664525u + 1013904223u;
                const float wn = ((float)(int32_t)rng / 2147483648.f) * 0.1f;
                buf.setSample(0, i, wn);
                buf.setSample(1, i, wn);
            }
            stage2Process(proc, buf, midi);
            // During hold phase, pace at real time (~2.67 ms per 128 samples) so the background worker thread's
            // ray trace (~200-350 ms wall clock) completes at realistic audio-time offset!
            if (isStopped) {
                juce::Thread::sleep(2);
            }

            for (int i = 0; i < bs; ++i) {
                const double l = buf.getSample(0, i), r = buf.getSample(1, i);
                sqSum += 0.5 * (l * l + r * r);
                if (++sqCount >= winSamples) {
                    const double tSec = (double)(pos + (size_t)i) / sr;
                    const double wallStopSec = isStopped
                        ? std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStopT0).count()
                        : (tSec - walkSec);
                    const double uNow = std::clamp((double)(pos + (size_t)i) / (double)walkSamples, 0.0, 1.0);
                    const double db = 10.0 * std::log10(std::max(sqSum / (double)sqCount, 1e-20));
                    const auto bsCur = engine->bakeStats();
                    const auto msCur = engine->motionStats();
                    const int lk = (int)(bsCur.lookups - bsPrev.lookups);
                    const uint64_t ts = msCur.transitionsStarted - msBase.transitionsStarted;
                    const uint64_t td = msCur.transitionsDone - msBase.transitionsDone;
                    if (ts > 0 && summaryOut.settleStartSec < 0.0)
                        summaryOut.settleStartSec = tSec - walkSec;
                    if (td > 0 && summaryOut.settleDoneSec < 0.0)
                        summaryOut.settleDoneSec = tSec - walkSec;

                    wf << std::fixed << std::setprecision(5)
                       << tSec << "," << wallStopSec << "," << uNow << "," << db << ","
                       << lk << "," << ts << "," << td << "\n";

                    if (tSec >= walkSec - 0.25 && tSec <= walkSec)
                        endMoveDbs.push_back(db);
                    if (tSec >= walkSec + holdSec - 0.75)
                        settledEndDbs.push_back(db);
                    if (tSec >= walkSec)
                        postStopCurve.emplace_back(tSec - walkSec, db);

                    bsPrev = bsCur;
                    sqSum = 0;
                    sqCount = 0;
                }
            }
            pos += (size_t)bs;
        }
        setP(proc, "soloTail", 0.f);

        auto avg = [](const std::vector<double>& v) {
            if (v.empty()) return -200.0;
            double s = 0;
            for (double x : v) s += x;
            return s / (double)v.size();
        };
        summaryOut.movingEndTailRmsDb = avg(endMoveDbs);
        summaryOut.settledTailRmsDb = avg(settledEndDbs);
        for (const auto& [dt, db] : postStopCurve) {
            if (std::fabs(db - summaryOut.settledTailRmsDb) <= 1.0 && dt >= 0.4) {
                summaryOut.audioWithin1DbSec = dt;
                break;
            }
        }
    };

    // Sample settled IR at every 2nd point + knots + end
    std::vector<WindowBandEnergies> bakedSettled(uList.size());
    std::vector<bool> isSettledSample(uList.size(), false);
    for (size_t i = 0; i < uList.size(); ++i) {
        bool pick = (i % 2 == 0) || (i + 1 == uList.size());
        for (double uk : knotU) if (std::fabs(uList[i] - uk) < 1e-4) pick = true;
        isSettledSample[i] = pick;
    }
    for (size_t i = 0; i < uList.size(); ++i) {
        if (isSettledSample[i]) {
            bakedSettled[i] = renderSettledAt(uList[i]);
            if (bakedSettled[i].lateTotDb <= -190.0) {
                // Retry once if tailRefreshCount raced with an already-in-flight worker pass
                bakedSettled[i] = renderSettledAt(uList[i]);
            }
        }
    }
    // Interpolate remaining settled points
    {
        size_t lastIdx = 0;
        for (size_t i = 1; i < uList.size(); ++i) {
            if (isSettledSample[i]) {
                for (size_t j = lastIdx + 1; j < i; ++j) {
                    const double f = (uList[j] - uList[lastIdx]) / std::max(1e-9, uList[i] - uList[lastIdx]);
                    auto lerp = [&](double a, double b) { return a + f * (b - a); };
                    bakedSettled[j].earlyDb = lerp(bakedSettled[lastIdx].earlyDb, bakedSettled[i].earlyDb);
                    bakedSettled[j].mid80_150Db = lerp(bakedSettled[lastIdx].mid80_150Db, bakedSettled[i].mid80_150Db);
                    bakedSettled[j].late1Db = lerp(bakedSettled[lastIdx].late1Db, bakedSettled[i].late1Db);
                    bakedSettled[j].late2Db = lerp(bakedSettled[lastIdx].late2Db, bakedSettled[i].late2Db);
                    bakedSettled[j].lateTotDb = lerp(bakedSettled[lastIdx].lateTotDb, bakedSettled[i].lateTotDb);
                    bakedSettled[j].fullTotDb = lerp(bakedSettled[lastIdx].fullTotDb, bakedSettled[i].fullTotDb);
                    for (int b = 0; b < verb::NB; ++b) {
                        bakedSettled[j].bandEarlyDb[b] = lerp(bakedSettled[lastIdx].bandEarlyDb[b], bakedSettled[i].bandEarlyDb[b]);
                        bakedSettled[j].bandMidDb[b] = lerp(bakedSettled[lastIdx].bandMidDb[b], bakedSettled[i].bandMidDb[b]);
                        bakedSettled[j].bandLate1Db[b] = lerp(bakedSettled[lastIdx].bandLate1Db[b], bakedSettled[i].bandLate1Db[b]);
                        bakedSettled[j].bandLate2Db[b] = lerp(bakedSettled[lastIdx].bandLate2Db[b], bakedSettled[i].bandLate2Db[b]);
                        bakedSettled[j].bandLateTotDb[b] = lerp(bakedSettled[lastIdx].bandLateTotDb[b], bakedSettled[i].bandLateTotDb[b]);
                        bakedSettled[j].bandFullDb[b] = lerp(bakedSettled[lastIdx].bandFullDb[b], bakedSettled[i].bandFullDb[b]);
                    }
                }
                lastIdx = i;
            }
        }
    }

    auto bakedMoving = runMovingSweep();
    runContinuousNoiseSettleWalk("p6-settle-walk-" + sceneTag + runTag + ".csv");

    // Measure isolated tail-only (soloTail=1) settled vs moving IR at u=1.0 so early reflections/direct don't mask 0-150 ms
    {
        setP(proc, "soloTail", 1.f);
        setP(proc, "soloDirect", 0.f);
        setP(proc, "soloEarly", 0.f);
        const auto soloSet = renderSettledAt(1.0);
        (void)renderSettledAt(0.0);
        for (int i = 0; i < (int)(1.1 * sr / bs); ++i) {
            setP(proc, pathParamId, 0.0);
            buf.clear();
            stage2Process(proc, buf, midi);
        }
        const int walkBlocks = std::max(8, (int)std::ceil(totalLen / (3.0 * bs / sr)));
        for (int b = 1; b <= walkBlocks; ++b) {
            setP(proc, pathParamId, (double)b / (double)walkBlocks);
            buf.clear();
            stage2Process(proc, buf, midi);
        }
        std::vector<float> L(irN, 0.f), R(irN, 0.f);
        size_t pos = 0;
        bool fired = false;
        while (pos < irN) {
            const int m = (int)std::min<size_t>(bs, irN - pos);
            setP(proc, pathParamId, 1.0);
            buf.clear();
            if (!fired) { buf.setSample(0, 0, 1.0f); buf.setSample(1, 0, 1.0f); fired = true; }
            stage2Process(proc, buf, midi);
            for (int i = 0; i < m; ++i) { L[pos + (size_t)i] = buf.getSample(0, i); R[pos + (size_t)i] = buf.getSample(1, i); }
            pos += (size_t)m;
        }
        const auto soloMov = measureWindowBands(L, R, sr);
        setP(proc, "soloTail", 0.f);
        summaryOut.soloEndEarlyErrDb = soloMov.earlyDb - soloSet.earlyDb;
        summaryOut.soloEndMidErrDb = soloMov.mid80_150Db - soloSet.mid80_150Db;
        summaryOut.soloEndLate1ErrDb = soloMov.late1Db - soloSet.late1Db;
        summaryOut.soloEndLate2ErrDb = soloMov.late2Db - soloSet.late2Db;
        summaryOut.soloEndLateTotErrDb = soloMov.lateTotDb - soloSet.lateTotDb;
    }

    // Re-evaluate settled sample points that exceeded 2.0 dB in 48k adaptive at converged 384k non-adaptive rays
    // (Flate's 8 kHz late tail is <-33 dB below 125 Hz, so adaptiveTail skips doubling for 8 kHz and 1-2 rays cause stereo variance).
    auto convergedSettled = bakedSettled;
    if (sceneTag == "flate") {
        std::vector<size_t> reevalIdx;
        for (size_t i = 0; i < uList.size(); ++i) {
            if (!isSettledSample[i] || uList[i] < 0.10 - 1e-6) continue;
            bool need = false;
            for (int b = 0; b < verb::NB; ++b) {
                if (std::fabs(bakedMoving[i].bandLateTotDb[b] - bakedSettled[i].bandLateTotDb[b]) > 2.0) {
                    need = true;
                    break;
                }
            }
            if (need) reevalIdx.push_back(i);
        }
        if (!reevalIdx.empty()) {
            const auto origTs = proc.getTailSettings();
            proc.setTailSettings({384000, false});
            waitReady(proc, 90);
            for (size_t idx : reevalIdx) {
                convergedSettled[idx] = renderSettledAt(uList[idx]);
                ++summaryOut.convergedReevalCount;
            }
            size_t lastIdx = 0;
            for (size_t i = 1; i < uList.size(); ++i) {
                if (isSettledSample[i]) {
                    for (size_t j = lastIdx + 1; j < i; ++j) {
                        const double f = (uList[j] - uList[lastIdx]) / std::max(1e-9, uList[i] - uList[lastIdx]);
                        auto lerp = [&](double a, double b) { return a + f * (b - a); };
                        for (int b = 0; b < verb::NB; ++b)
                            convergedSettled[j].bandLateTotDb[b] = lerp(convergedSettled[lastIdx].bandLateTotDb[b], convergedSettled[i].bandLateTotDb[b]);
                    }
                    lastIdx = i;
                }
            }
            proc.setTailSettings(origTs);
            waitReady(proc, 90);
        }
    }

    // Also measure nobake moving for reference
    proc.clearPathBake();
    {
        const auto until = juce::Time::getMillisecondCounterHiRes() + 15000;
        while (juce::Time::getMillisecondCounterHiRes() < until && proc.pathBakeStatus()["state"].toString() != "notBaked") {
            buf.clear(); stage2Process(proc, buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(2); juce::Thread::sleep(1);
        }
    }
    auto nobakeMoving = runMovingSweep();

    // Write detailed CSV with all 7 bands and windows
    const std::string csvName = "p6-sweep-" + sceneTag + runTag + ".csv";
    {
        std::ofstream f((outDir / csvName).string());
        f << "u,dist_m,x,y,z,"
          << "set_early,set_mid80_150,set_late1,set_late2,set_lateTot,"
          << "set_b125,set_b250,set_b500,set_b1k,set_b2k,set_b4k,set_b8k,"
          << "mov_early,mov_mid80_150,mov_late1,mov_late2,mov_lateTot,"
          << "mov_b125,mov_b250,mov_b500,mov_b1k,mov_b2k,mov_b4k,mov_b8k,"
          << "nb_mov_lateTot,err_late1,err_late2,err_lateTot,"
          << "err_b125,err_b250,err_b500,err_b1k,err_b2k,err_b4k,err_b8k,lookups\n";
        for (size_t i = 0; i < uList.size(); ++i) {
            const double u = uList[i];
            const auto q = cpath.evaluate(u);
            const auto& st = bakedSettled[i];
            const auto& mv = bakedMoving[i];
            const auto& nm = nobakeMoving[i];
            f << std::fixed << std::setprecision(5)
              << u << "," << (u * totalLen) << "," << q[0] << "," << q[1] << "," << q[2] << ","
              << st.earlyDb << "," << st.mid80_150Db << "," << st.late1Db << "," << st.late2Db << "," << st.lateTotDb;
            for (int b = 0; b < verb::NB; ++b) f << "," << st.bandLateTotDb[b];
            f << "," << mv.earlyDb << "," << mv.mid80_150Db << "," << mv.late1Db << "," << mv.late2Db << "," << mv.lateTotDb;
            for (int b = 0; b < verb::NB; ++b) f << "," << mv.bandLateTotDb[b];
            f << "," << nm.lateTotDb
              << "," << (mv.late1Db - st.late1Db)
              << "," << (mv.late2Db - st.late2Db)
              << "," << (mv.lateTotDb - st.lateTotDb);
            for (int b = 0; b < verb::NB; ++b) f << "," << (mv.bandLateTotDb[b] - st.bandLateTotDb[b]);
            f << "," << mv.lookupsDelta << "\n";
        }
    }

    // Aggregate metrics for u in [0.1, 1.0] (and [0.1, 0.975])
    for (size_t i = 0; i < uList.size(); ++i) {
        const double u = uList[i];
        const auto& st = bakedSettled[i];
        const auto& cv = convergedSettled[i];
        const auto& mv = bakedMoving[i];
        const double errLT = mv.lateTotDb - st.lateTotDb;
        const double errL1 = mv.late1Db - st.late1Db;
        const double errL2 = mv.late2Db - st.late2Db;
        const double errEarly = mv.earlyDb - st.earlyDb;
        const double errMid = mv.mid80_150Db - st.mid80_150Db;
        if (u >= 0.10 - 1e-6) {
            if (std::fabs(errLT) > summaryOut.maxAbsLateTotErrDb) {
                summaryOut.maxAbsLateTotErrDb = std::fabs(errLT);
                summaryOut.signedLateTotErrAtMax = errLT;
                summaryOut.maxLateTotErrU = u;
            }
            summaryOut.maxAbsLate1ErrDb = std::max(summaryOut.maxAbsLate1ErrDb, std::fabs(errL1));
            summaryOut.maxAbsLate2ErrDb = std::max(summaryOut.maxAbsLate2ErrDb, std::fabs(errL2));
            summaryOut.maxAbsFullEarlyErrDb = std::max(summaryOut.maxAbsFullEarlyErrDb, std::fabs(errEarly));
            summaryOut.minFullMidErrDb = std::min(summaryOut.minFullMidErrDb, errMid);
            summaryOut.maxFullMidErrDb = std::max(summaryOut.maxFullMidErrDb, errMid);
            if (u <= 0.975 + 1e-6)
                summaryOut.maxAbsLateTotUpTo0975 = std::max(summaryOut.maxAbsLateTotUpTo0975, std::fabs(errLT));
            for (int b = 0; b < verb::NB; ++b) {
                const double eb = mv.bandLateTotDb[b] - st.bandLateTotDb[b];
                if (std::fabs(eb) > summaryOut.maxAbsBandErrDb[b])
                    summaryOut.maxAbsBandErrDb[b] = std::fabs(eb);
                if (std::fabs(eb) > summaryOut.maxAbsAnyBandErrDb) {
                    summaryOut.maxAbsAnyBandErrDb = std::fabs(eb);
                    summaryOut.worstBandIdx = b;
                    summaryOut.worstBandU = u;
                }
                const double ec = mv.bandLateTotDb[b] - cv.bandLateTotDb[b];
                if (std::fabs(ec) > summaryOut.maxAbsBandConvergedDb[b])
                    summaryOut.maxAbsBandConvergedDb[b] = std::fabs(ec);
                if (std::fabs(ec) > summaryOut.maxAbsAnyBandConvergedDb) {
                    summaryOut.maxAbsAnyBandConvergedDb = std::fabs(ec);
                    summaryOut.worstConvergedBandIdx = b;
                    summaryOut.worstConvergedBandU = u;
                }
                if (u <= 0.975 + 1e-6)
                    summaryOut.maxAbsAnyBandUpTo0975 = std::max(summaryOut.maxAbsAnyBandUpTo0975, std::fabs(eb));
            }
        }
        if (i + 1 == uList.size()) {
            summaryOut.endLateTotErrDb = errLT;
            summaryOut.endLate1ErrDb = errL1;
            summaryOut.endLate2ErrDb = errL2;
            summaryOut.endFullEarlyErrDb = errEarly;
            summaryOut.endFullMidErrDb = errMid;
            for (int b = 0; b < verb::NB; ++b)
                summaryOut.signedBandErrAtEnd[b] = mv.bandLateTotDb[b] - st.bandLateTotDb[b];
        }
    }

    static constexpr int kBandHz[verb::NB] = {125, 250, 500, 1000, 2000, 4000, 8000};
    std::printf("=== P6 Summary [%s] (%s) ===\n", sceneTag.c_str(), csvName.c_str());
    std::printf("  End of path (u=1.0): lateTot err = %+.2f dB (late1 150-400ms %+.2f dB, late2 400-1000ms %+.2f dB)\n",
                summaryOut.endLateTotErrDb, summaryOut.endLate1ErrDb, summaryOut.endLate2ErrDb);
    std::printf("  End of path per-band lateTot err (dB):");
    for (int b = 0; b < verb::NB; ++b)
        std::printf(" %dHz:%+.2f", kBandHz[b], summaryOut.signedBandErrAtEnd[b]);
    std::printf("\n");
    std::printf("  Max |baked moving - settled exact| for u in [0.1, 1.0]:\n");
    std::printf("    lateTot: %.2f dB (signed %+.2f dB at u=%.4f) | late1: %.2f dB | late2: %.2f dB\n",
                summaryOut.maxAbsLateTotErrDb, summaryOut.signedLateTotErrAtMax, summaryOut.maxLateTotErrU,
                summaryOut.maxAbsLate1ErrDb, summaryOut.maxAbsLate2ErrDb);
    std::printf("    (for u in [0.1, 0.975]: lateTot max %.2f dB, any band max %.2f dB)\n",
                summaryOut.maxAbsLateTotUpTo0975, summaryOut.maxAbsAnyBandUpTo0975);
    std::printf("    Per-band max |err| (dB):");
    for (int b = 0; b < verb::NB; ++b)
        std::printf(" %dHz:%.2f", kBandHz[b], summaryOut.maxAbsBandErrDb[b]);
    std::printf(" (worst: %dHz %.2f dB at u=%.4f)\n",
                kBandHz[summaryOut.worstBandIdx], summaryOut.maxAbsAnyBandErrDb, summaryOut.worstBandU);
    if (summaryOut.convergedReevalCount > 0) {
        std::printf("    Converged 384k reference (at %d adaptive-MC outlier points): max |err| per band:",
                    summaryOut.convergedReevalCount);
        for (int b = 0; b < verb::NB; ++b)
            std::printf(" %dHz:%.2f", kBandHz[b], summaryOut.maxAbsBandConvergedDb[b]);
        std::printf(" (worst: %dHz %.2f dB at u=%.4f)\n",
                    kBandHz[summaryOut.worstConvergedBandIdx], summaryOut.maxAbsAnyBandConvergedDb, summaryOut.worstConvergedBandU);
    }
    std::printf("  0-150 ms audible preservation:\n");
    std::printf("    Full-mix [0.1, 1.0]: 0-80ms max|err| %.2f dB (u=1: %+.2f dB) | 80-150ms range [%+.2f, %+.2f] dB (u=1: %+.2f dB)\n",
                summaryOut.maxAbsFullEarlyErrDb, summaryOut.endFullEarlyErrDb,
                summaryOut.minFullMidErrDb, summaryOut.maxFullMidErrDb, summaryOut.endFullMidErrDb);
    std::printf("    soloTail=1 (u=1.0): 0-80ms err %+.2f dB | 80-150ms err %+.2f dB | late1 %+.2f | late2 %+.2f | lateTot %+.2f dB\n",
                summaryOut.soloEndEarlyErrDb, summaryOut.soloEndMidErrDb,
                summaryOut.soloEndLate1ErrDb, summaryOut.soloEndLate2ErrDb, summaryOut.soloEndLateTotErrDb);
    std::printf("  Continuous steady-state noise (soloTail=1) settle timeline at u=1.0:\n");
    std::printf("    Moving level at end: %.2f dB | Settled level: %.2f dB (delta %+.2f dB)\n",
                summaryOut.movingEndTailRmsDb, summaryOut.settledTailRmsDb,
                summaryOut.movingEndTailRmsDb - summaryOut.settledTailRmsDb);
    std::printf("    Stop -> transitionsStarted: %.3f s | Stop -> transitionsDone: %.3f s | Stop -> within 1 dB of settled: %.3f s\n",
                summaryOut.settleStartSec, summaryOut.settleDoneSec, summaryOut.audioWithin1DbSec);
    std::fflush(stdout);

    proc.releaseResources();
    return true;
}

inline void printSceneDiag(const juce::File& presetFile, const std::string& tag, int pathObj, const juce::String& pathParamId) {
    const double sr = 48000.0;
    const int bs = 128;
    const size_t irN = (size_t)(1.5 * sr);

    VerbProcessor proc;
    proc.setPlayConfigDetails(2, 2, sr, bs);
    proc.prepareToPlay(sr, bs);
    if (!waitReady(proc, 90)) return;
    auto xml = juce::parseXML(presetFile.loadFileAsString());
    if (!xml) return;
    juce::String err;
    if (!proc.loadRoomPresetText(xml->toString(), err) || !waitReady(proc, 120)) return;
    auto engine = E9bAccess::engine(proc);
    juce::AudioBuffer<float> buf(2, bs);
    juce::MidiBuffer midi;
    for (int i = 0; i < 40; ++i) { buf.clear(); stage2Process(proc, buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(2); }

    auto tbl = engine->bakedTableSnapshot();
    if (!tbl || tbl->u.size() < 2) {
        proc.startPathBake(pathObj);
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 180.0) {
            buf.clear(); stage2Process(proc, buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
            if (proc.pathBakePercent() == 100) break;
            juce::Thread::sleep(1);
        }
        tbl = engine->bakedTableSnapshot();
    }
    if (!tbl) return;

    auto settleDiagAt = [&](double u) {
        setP(proc, pathParamId, u);
        for (int i = 0; i < 12; ++i) { buf.clear(); stage2Process(proc, buf, midi); }
        const uint64_t refBefore = engine->tailRefreshCount();
        engine->notifyJump();
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 45.0) {
            buf.clear(); stage2Process(proc, buf, midi); juce::Thread::sleep(2);
            if (engine->tailRefreshCount() > refBefore && engine->tailIsCurrent()) break;
        }
        for (int i = 0; i < (int)(0.90 * sr / bs); ++i) { buf.clear(); stage2Process(proc, buf, midi); }
    };
    settleDiagAt(0.0);

    auto scPtr = engine->sceneSnapshot();
    if (!scPtr) return;
    const verb::Scene scBase = *scPtr;
    const auto rawPath = proc.getMovementPath(pathObj);
    verbp::CompiledPath cpath;
    cpath.build(rawPath);

    std::printf("\n================ DIAG HIST [%s] (erOrder=1, baseRays=%d, scn.len=%.3f, sc.rd=%.3f) ================\n",
                tag.c_str(), tbl->selectedRays, scBase.length, scBase.detectRadius);

    auto histBreakdown = [&](const verb::TailHist& h, const char* label) {
        // Compute sum over receivers (average per receiver) for each band and time window
        // Windows: [0,11), [11,80), [80,150), [150,400), [400,1000), [150,1000), [1000,end), [0,end)
        struct WinE { double w0_11=0, w11_80=0, w80_150=0, w150_400=0, w400_1000=0, w150_1000=0, w1000_end=0, wTot=0; };
        std::array<WinE, verb::NB> perB{};
        WinE tot{};
        const int R = (int)h.h.size();
        for (int r = 0; r < R; ++r) {
            for (int b = 0; b < verb::NB; ++b) {
                for (size_t k = 0; k < h.h[r][b].size(); ++k) {
                    const double tMs = ((double)k + 0.5) * h.binDur * 1000.0;
                    const double v = h.h[r][b][k] / (double)R;
                    perB[b].wTot += v; tot.wTot += v;
                    if (tMs < 11.0) { perB[b].w0_11 += v; tot.w0_11 += v; }
                    else if (tMs < 80.0) { perB[b].w11_80 += v; tot.w11_80 += v; }
                    else if (tMs < 150.0) { perB[b].w80_150 += v; tot.w80_150 += v; }
                    else if (tMs < 400.0) { perB[b].w150_400 += v; perB[b].w150_1000 += v; tot.w150_400 += v; tot.w150_1000 += v; }
                    else if (tMs < 1000.0) { perB[b].w400_1000 += v; perB[b].w150_1000 += v; tot.w400_1000 += v; tot.w150_1000 += v; }
                    else { perB[b].w1000_end += v; tot.w1000_end += v; }
                }
            }
        }
        auto db = [](double x) { return 10.0 * std::log10(std::max(x, 1e-30)); };
        std::printf("  %-22s ALL: 0-11:%6.2f 11-80:%6.2f 80-150:%6.2f 150-400:%6.2f 400-1000:%6.2f 150-1000:%6.2f >1s:%6.2f TOT:%6.2f\n",
                    label, db(tot.w0_11), db(tot.w11_80), db(tot.w80_150), db(tot.w150_400), db(tot.w400_1000), db(tot.w150_1000), db(tot.w1000_end), db(tot.wTot));
        static constexpr int kHz[verb::NB] = {125,250,500,1000,2000,4000,8000};
        for (int b = 0; b < verb::NB; ++b) {
            std::printf("    b%4d: 0-11:%6.2f 11-80:%6.2f 80-150:%6.2f 150-400:%6.2f 400-1000:%6.2f 150-1000:%6.2f >1s:%6.2f TOT:%6.2f (f150-1k=%.1f%%)\n",
                        kHz[b], db(perB[b].w0_11), db(perB[b].w11_80), db(perB[b].w80_150), db(perB[b].w150_400),
                        db(perB[b].w400_1000), db(perB[b].w150_1000), db(perB[b].w1000_end), db(perB[b].wTot),
                        perB[b].wTot > 0 ? 100.0 * perB[b].w150_1000 / perB[b].wTot : 0.0);
        }
    };

    const std::vector<double> testUs = {0.0, 0.5, 0.9, 1.0};
    for (double u : testUs) {
        const auto q = cpath.evaluate(u);
        verb::Transducer src = scBase.sources[0];
        std::vector<verb::Transducer> rcv = scBase.receivers;
        if (pathObj == verbp::kListener) {
            const double cy = std::cos((q[3] - scBase.listenerYaw) * verb::kPi / 180.0);
            const double sy = std::sin((q[3] - scBase.listenerYaw) * verb::kPi / 180.0);
            for (auto& r : rcv) {
                const double dx = r.pos.x - scBase.listenerPos.x;
                const double dy = r.pos.y - scBase.listenerPos.y;
                r.pos.x = q[0] + dx * cy - dy * sy;
                r.pos.y = q[1] + dx * sy + dy * cy;
                r.pos.z = q[2] + (r.pos.z - scBase.listenerPos.z);
                r.azimuthDeg += (q[3] - scBase.listenerYaw);
            }
        } else if (pathObj < (int)scBase.sources.size()) {
            src.pos = {q[0], q[1], q[2]};
            src.azimuthDeg = q[3];
        }
        int raysUsed = 0; bool sparse = false;
        verb::TailHist liveRaw;
        auto liveFin = verb::traceTailAdaptive(scBase, src, rcv, 1, 48000, 192000, scBase.seed, raysUsed, &sparse, {}, {}, &liveRaw, 0.0);
        auto bakeRaw = verb::traceTail(scBase, src, rcv, 1, tbl->selectedRays, scBase.seed, false, {}, {}, verb::kBakeDetectRadiusMin);
        auto bakeFin = bakeRaw;
        verb::finalizeTail(bakeFin);

        std::printf("\n--- u=%.3f (pos=%.3f, %.3f, %.3f, yaw=%.1f) liveRays=%d bakeRays=%d ---\n",
                    u, q[0], q[1], q[2], q[3], raysUsed, tbl->selectedRays);
        histBreakdown(liveFin, "liveFin(rd=def)");
        histBreakdown(liveRaw, "liveRaw(rd=def)");
        histBreakdown(bakeRaw, "bakeRaw(rd=1.0)");
        histBreakdown(bakeFin, "bakeFin(rd=1.0)");

        std::vector<double> tblE((size_t)tbl->sources * tbl->receivers * verb::NB, 0.0);
        tbl->lookup(u, tblE.data());
        std::printf("    tbl->lookup(u) mean dB per band:");
        for (int b = 0; b < verb::NB; ++b) {
            double s = 0;
            for (int r = 0; r < tbl->receivers; ++r) s += tblE[(size_t)r * verb::NB + b] / tbl->receivers;
            std::printf(" %.2f", 10.0 * std::log10(std::max(s, 1e-30)));
        }
        std::printf("\n");
    }

    // Also measure isolated soloTail=1 IR windows and eqApplyBank effect on u=0 -> u=1!
    auto renderSoloTail = [&](double u, bool movingFromPrev, double uFrom) -> std::pair<std::vector<float>, std::vector<float>> {
        setP(proc, "soloTail", 1.f);
        setP(proc, "soloDirect", 0.f);
        setP(proc, "soloEarly", 0.f);
        auto settle = [&](double uu) {
            setP(proc, pathParamId, uu);
            for (int i = 0; i < 12; ++i) { buf.clear(); stage2Process(proc, buf, midi); }
            const uint64_t refBefore = engine->tailRefreshCount();
            engine->notifyJump();
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 45.0) {
                buf.clear(); stage2Process(proc, buf, midi); juce::Thread::sleep(2);
                if (engine->tailRefreshCount() > refBefore && engine->tailIsCurrent()) break;
            }
            for (int i = 0; i < (int)(0.90 * sr / bs); ++i) { buf.clear(); stage2Process(proc, buf, midi); }
        };
        if (movingFromPrev) {
            settle(uFrom);
            const int walkBlocks = 400;
            for (int b = 1; b <= walkBlocks; ++b) {
                setP(proc, pathParamId, uFrom + (u - uFrom) * ((double)b / walkBlocks));
                buf.clear(); stage2Process(proc, buf, midi);
            }
        } else {
            settle(u);
        }
        std::vector<float> L(irN, 0.f), R(irN, 0.f);
        size_t pos = 0; bool fired = false;
        while (pos < irN) {
            const int m = (int)std::min<size_t>(bs, irN - pos);
            setP(proc, pathParamId, u);
            buf.clear();
            if (!fired) { buf.setSample(0, 0, 1.f); buf.setSample(1, 0, 1.f); fired = true; }
            stage2Process(proc, buf, midi);
            for (int i = 0; i < m; ++i) { L[pos + (size_t)i] = buf.getSample(0, i); R[pos + (size_t)i] = buf.getSample(1, i); }
            pos += (size_t)m;
        }
        setP(proc, "soloTail", 0.f);
        return {L, R};
    };

    auto [s0L, s0R] = renderSoloTail(0.0, false, 0.0);
    auto [s1L, s1R] = renderSoloTail(1.0, false, 1.0);
    auto [m1L, m1R] = renderSoloTail(1.0, true, 0.0);

    auto measureIRWindows = [&](const std::vector<float>& L, const std::vector<float>& R, const char* lbl) {
        auto w = measureWindowBands(L, R, sr);
        std::printf("  soloTail %-16s: 0-80:%6.2f 80-150:%6.2f 150-400:%6.2f 400-1000:%6.2f 150-1000:%6.2f FULL0-1s:%6.2f\n",
                    lbl, w.earlyDb, w.mid80_150Db, w.late1Db, w.late2Db, w.lateTotDb, w.fullTotDb);
        std::printf("    b0-80   :"); for (int b = 0; b < verb::NB; ++b) std::printf(" %6.2f", w.bandEarlyDb[b]); std::printf("\n");
        std::printf("    b80-150 :"); for (int b = 0; b < verb::NB; ++b) std::printf(" %6.2f", w.bandMidDb[b]); std::printf("\n");
        std::printf("    b150-1k :"); for (int b = 0; b < verb::NB; ++b) std::printf(" %6.2f", w.bandLateTotDb[b]); std::printf("\n");
        std::printf("    bFULL   :"); for (int b = 0; b < verb::NB; ++b) std::printf(" %6.2f", w.bandFullDb[b]); std::printf("\n");
    };
    std::printf("\n--- Rendered soloTail=1 IR comparisons ---\n");
    measureIRWindows(s0L, s0R, "u=0 settled");
    measureIRWindows(s1L, s1R, "u=1 settled");
    measureIRWindows(m1L, m1R, "u=1 bakedMov");

    // Simulate eqApplyBank directly on s0L/s0R using raw tbl(u=1)/tbl(u=0) gains (proxy for tbl(u=1)/bakeEnergy)
    {
        std::vector<double> e0((size_t)tbl->receivers * verb::NB, 0.0), e1((size_t)tbl->receivers * verb::NB, 0.0);
        tbl->lookup(0.0, e0.data());
        tbl->lookup(1.0, e1.data());
        std::array<float, verb::NB> g{};
        for (int b = 0; b < verb::NB; ++b) {
            double s0 = 0, s1 = 0;
            for (int r = 0; r < tbl->receivers; ++r) { s0 += e0[(size_t)r * verb::NB + b]; s1 += e1[(size_t)r * verb::NB + b]; }
            g[b] = (float)std::sqrt(std::max(1e-30, s1) / std::max(1e-30, s0));
        }
        std::printf("  eqApplyBank raw sqrt(tbl[1]/tbl[0]) gains (dB):");
        for (int b = 0; b < verb::NB; ++b) std::printf(" %+.2f", 20.0 * std::log10(g[b]));
        std::printf("\n");
        std::array<double, verb::NB - 1> eqA{};
        for (int b = 0; b < verb::NB - 1; ++b) eqA[b] = 1.0 - std::exp(-2.0 * verb::kPi * verb::kBandHz[b] * std::sqrt(2.0) / sr);
        auto applyEq = [&](std::vector<float> in) {
            std::array<double, verb::NB - 1> lp{};
            for (size_t i = 0; i < in.size(); ++i) {
                double x = in[i], prev = 0.0, y = 0.0;
                for (int b = 0; b < verb::NB - 1; ++b) {
                    lp[b] += eqA[b] * (x - lp[b]);
                    y += g[b] * (lp[b] - prev);
                    prev = lp[b];
                }
                y += g[verb::NB - 1] * (x - prev);
                in[i] = (float)y;
            }
            return in;
        };
        auto eqL = applyEq(s0L), eqR = applyEq(s0R);
        measureIRWindows(eqL, eqR, "eq(s0, tbl1/0)");
    }

    std::fflush(stdout);
    proc.releaseResources();
}

inline void simCandidateWindows(const juce::File& presetFile, const std::string& tag, int pathObj, const juce::String& pathParamId, const std::vector<double>& testUs) {
    const double sr = 48000.0;
    const int bs = 128;
    const size_t irN = (size_t)(1.05 * sr);

    VerbProcessor proc;
    proc.setPlayConfigDetails(2, 2, sr, bs);
    proc.prepareToPlay(sr, bs);
    if (!waitReady(proc, 90)) return;
    auto xml = juce::parseXML(presetFile.loadFileAsString());
    if (!xml) return;
    juce::String err;
    if (!proc.loadRoomPresetText(xml->toString(), err) || !waitReady(proc, 120)) return;
    auto engine = E9bAccess::engine(proc);
    juce::AudioBuffer<float> buf(2, bs);
    juce::MidiBuffer midi;
    for (int i = 0; i < 40; ++i) { buf.clear(); stage2Process(proc, buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(2); }

    auto tbl = engine->bakedTableSnapshot();
    if (!tbl || tbl->u.size() < 2) {
        proc.startPathBake(pathObj);
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 180.0) {
            buf.clear(); stage2Process(proc, buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
            if (proc.pathBakePercent() == 100) break;
            juce::Thread::sleep(1);
        }
        tbl = engine->bakedTableSnapshot();
    }
    if (!tbl) return;

    auto renderSoloTailAt = [&](double u) -> std::pair<std::vector<float>, std::vector<float>> {
        setP(proc, "soloTail", 1.f);
        setP(proc, "soloDirect", 0.f);
        setP(proc, "soloEarly", 0.f);
        setP(proc, pathParamId, u);
        for (int i = 0; i < 12; ++i) { buf.clear(); stage2Process(proc, buf, midi); }
        const uint64_t refBefore = engine->tailRefreshCount();
        engine->notifyJump();
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 45.0) {
            buf.clear(); stage2Process(proc, buf, midi); juce::Thread::sleep(2);
            if (engine->tailRefreshCount() > refBefore && engine->tailIsCurrent()) break;
        }
        for (int i = 0; i < (int)(0.90 * sr / bs); ++i) { buf.clear(); stage2Process(proc, buf, midi); }
        std::vector<float> L(irN, 0.f), R(irN, 0.f);
        size_t pos = 0; bool fired = false;
        while (pos < irN) {
            const int m = (int)std::min<size_t>(bs, irN - pos);
            buf.clear();
            if (!fired) { buf.setSample(0, 0, 1.f); buf.setSample(1, 0, 1.f); fired = true; }
            stage2Process(proc, buf, midi);
            for (int i = 0; i < m; ++i) { L[pos + (size_t)i] = buf.getSample(0, i); R[pos + (size_t)i] = buf.getSample(1, i); }
            pos += (size_t)m;
        }
        setP(proc, "soloTail", 0.f);
        return {L, R};
    };

    auto scPtr = engine->sceneSnapshot();
    if (!scPtr) return;
    const verb::Scene scBase = *scPtr;
    const auto rawPath = proc.getMovementPath(pathObj);
    verbp::CompiledPath cpath;
    cpath.build(rawPath);

    auto tracePose = [&](double u, double minRd, int rays, verb::TailHist& rawOut, verb::TailHist& finOut) {
        const auto q = cpath.evaluate(u);
        verb::Transducer src = scBase.sources[0];
        std::vector<verb::Transducer> rcv = scBase.receivers;
        if (pathObj == verbp::kListener) {
            const double cy = std::cos((q[3] - scBase.listenerYaw) * verb::kPi / 180.0);
            const double sy = std::sin((q[3] - scBase.listenerYaw) * verb::kPi / 180.0);
            for (auto& r : rcv) {
                const double dx = r.pos.x - scBase.listenerPos.x;
                const double dy = r.pos.y - scBase.listenerPos.y;
                r.pos.x = q[0] + dx * cy - dy * sy;
                r.pos.y = q[1] + dx * sy + dy * cy;
                r.pos.z = q[2] + (r.pos.z - scBase.listenerPos.z);
                r.azimuthDeg += (q[3] - scBase.listenerYaw);
            }
        } else if (pathObj < (int)scBase.sources.size()) {
            src.pos = {q[0], q[1], q[2]};
            src.azimuthDeg = q[3];
        }
        rawOut = verb::traceTail(scBase, src, rcv, 1, rays, scBase.seed, false, {}, {}, minRd);
        finOut = rawOut;
        verb::finalizeTail(finOut);
    };

    struct BandWin { std::array<double, verb::NB> e{}, ess{}; };
    auto sumWin = [&](const verb::TailHist& h, const verb::TailHist* rawH, double t0Ms, double t1Ms) -> BandWin {
        BandWin bw;
        const int R = (int)h.h.size();
        for (int r = 0; r < R; ++r) for (int b = 0; b < verb::NB; ++b) {
            double s1 = 0, s2 = 0;
            for (size_t k = 0; k < h.h[r][b].size(); ++k) {
                const double tMs = ((double)k + 0.5) * h.binDur * 1000.0;
                if (tMs >= t0Ms && tMs < t1Ms) {
                    s1 += h.h[r][b][k];
                    if (rawH && !rawH->h2.empty()) s2 += rawH->h2[r][b][k];
                }
            }
            bw.e[b] += s1 / R;
            if (s2 > 0) bw.ess[b] += (s1 * s1 / s2) / R;
        }
        return bw;
    };

    std::array<double, verb::NB - 1> eqA{};
    for (int b = 0; b < verb::NB - 1; ++b) eqA[b] = 1.0 - std::exp(-2.0 * verb::kPi * verb::kBandHz[b] * std::sqrt(2.0) / sr);
    auto applyEq1Pole = [&](const std::vector<float>& in, const std::array<double, verb::NB>& g) {
        std::vector<float> out(in.size());
        std::array<double, verb::NB - 1> lp{};
        for (size_t i = 0; i < in.size(); ++i) {
            double x = in[i], prev = 0.0, y = 0.0;
            for (int b = 0; b < verb::NB - 1; ++b) { lp[b] += eqA[b] * (x - lp[b]); y += g[b] * (lp[b] - prev); prev = lp[b]; }
            y += g[verb::NB - 1] * (x - prev);
            out[i] = (float)y;
        }
        return out;
    };

    // LR2 binary tree with 1st-order allpass phase alignment (all g=1 => exact allpass, -12 dB/oct rolloff)
    struct LR2Bank {
        struct Sec1 { double b0=0, b1=0, a1=0, x1=0, y1=0; double step(double x){ double y=b0*x+b1*x1-a1*y1; x1=x; y1=y; return y; } };
        struct Split {
            Sec1 lp1, lp2, hp1, hp2;
            void init(double fc, double sr) {
                const double K = std::tan(verb::kPi * fc / sr), g0 = K / (1.0 + K), a1 = (K - 1.0) / (K + 1.0);
                lp1 = {g0, g0, a1, 0, 0}; lp2 = lp1;
                const double h0 = 1.0 / (1.0 + K);
                hp1 = {h0, -h0, a1, 0, 0}; hp2 = hp1;
            }
            void run(double x, double& lo, double& hi) {
                lo = lp2.step(lp1.step(x));
                hi = -hp2.step(hp1.step(x)); // lo + hi = AP1(x)
            }
        };
        struct AP1 {
            double a1=0, x1=0, y1=0;
            void init(double fc, double sr) { const double K = std::tan(verb::kPi * fc / sr); a1 = (K - 1.0) / (K + 1.0); x1 = y1 = 0; }
            double step(double x) { double y = a1 * x + x1 - a1 * y1; x1 = x; y1 = y; return y; }
        };
        std::array<Split, verb::NB - 1> sp{};
        AP1 apL3, apL4, apL5, apH0, apH1, ap0_1, ap34_5, ap56_3;
        void init(double sr) {
            for (int b = 0; b < verb::NB - 1; ++b) sp[b].init(verb::kBandHz[b] * std::sqrt(2.0), sr);
            apL3.init(verb::kBandHz[3] * std::sqrt(2.0), sr);
            apL4.init(verb::kBandHz[4] * std::sqrt(2.0), sr);
            apL5.init(verb::kBandHz[5] * std::sqrt(2.0), sr);
            apH0.init(verb::kBandHz[0] * std::sqrt(2.0), sr);
            apH1.init(verb::kBandHz[1] * std::sqrt(2.0), sr);
            ap0_1.init(verb::kBandHz[1] * std::sqrt(2.0), sr);
            ap34_5.init(verb::kBandHz[5] * std::sqrt(2.0), sr);
            ap56_3.init(verb::kBandHz[3] * std::sqrt(2.0), sr);
        }
        double step(double x, const std::array<double, verb::NB>& g) {
            double low, hi, b0, b12, b1, b2, b34, b56, b3, b4, b5, b6;
            sp[2].run(x, low, hi);
            low = apL5.step(apL4.step(apL3.step(low)));
            hi  = apH1.step(apH0.step(hi));
            sp[0].run(low, b0, b12);
            b0 = ap0_1.step(b0);
            sp[1].run(b12, b1, b2);
            sp[4].run(hi, b34, b56);
            b34 = ap34_5.step(b34);
            b56 = ap56_3.step(b56);
            sp[3].run(b34, b3, b4);
            sp[5].run(b56, b5, b6);
            return g[0]*b0 + g[1]*b1 + g[2]*b2 + g[3]*b3 + g[4]*b4 + g[5]*b5 + g[6]*b6;
        }
    };
    auto applyEqLR2 = [&](const std::vector<float>& in, const std::array<double, verb::NB>& g) {
        std::vector<float> out(in.size());
        LR2Bank bank; bank.init(sr);
        for (size_t i = 0; i < in.size(); ++i) out[i] = (float)bank.step(in[i], g);
        return out;
    };

    // Multi-window filter: partitions IR at B=512 sample boundaries (sample idx = (k+1)*512)
    // edgeK gives upper partition k limit for each window (last = 1000000).
    // e.g. 3 wins: k < 13 (<149.3 ms), k < 37 (<405.3 ms), k >= 37 (>=405.3 ms)
    // e.g. 4 wins: k < 7 (<85.3 ms), k < 13 (<149.3 ms), k < 37 (<405.3 ms), k >= 37 (>=405.3 ms)
    auto applyMultiWin = [&](const std::vector<float>& in, const std::vector<int>& edgeK,
                             const std::vector<std::array<double, verb::NB>>& gWins, bool useLR2) {
        const size_t n = in.size();
        const size_t W = gWins.size();
        std::vector<float> acc(n, 0.f);
        for (size_t w = 0; w < W; ++w) {
            const size_t sLo = (w == 0) ? 0 : (size_t)(edgeK[w - 1] + 1) * 512;
            const size_t sHi = (w + 1 == W) ? n : std::min(n, (size_t)(edgeK[w] + 1) * 512);
            std::vector<float> seg(n, 0.f);
            for (size_t i = sLo; i < sHi; ++i) seg[i] = in[i];
            auto eq = useLR2 ? applyEqLR2(seg, gWins[w]) : applyEq1Pole(seg, gWins[w]);
            for (size_t i = 0; i < n; ++i) acc[i] += eq[i];
        }
        return acc;
    };

    auto [s0L, s0R] = renderSoloTailAt(0.0);
    verb::TailHist raw0, fin0, liveRaw0, liveFin0;
    tracePose(0.0, verb::kBakeDetectRadiusMin, tbl->selectedRays, raw0, fin0);
    tracePose(0.0, 0.0, tbl->selectedRays, liveRaw0, liveFin0);

    std::printf("\n=== SIM CANDIDATE WINDOWS [%s] ===\n", tag.c_str());
    for (double u : testUs) {
        auto [suL, suR] = renderSoloTailAt(u);
        auto wSet = measureWindowBands(suL, suR, sr);
        verb::TailHist rawU, finU;
        tracePose(u, verb::kBakeDetectRadiusMin, tbl->selectedRays, rawU, finU);

        std::printf("--- u=%.3f set(E/M/L1/L2/LT/F): %.1f %.1f %.1f %.1f %.1f %.1f ---\n",
                    u, wSet.earlyDb, wSet.mid80_150Db, wSet.late1Db, wSet.late2Db, wSet.lateTotDb, wSet.fullTotDb);
        auto printRes = [&](const char* rname, const std::vector<float>& yL, const std::vector<float>& yR) {
            auto wMov = measureWindowBands(yL, yR, sr);
            double worstB = 0;
            for (int b = 0; b < verb::NB; ++b) worstB = std::max(worstB, std::fabs(wMov.bandLateTotDb[b] - wSet.bandLateTotDb[b]));
            std::printf("  %-18s: dE=%+5.2f dM=%+5.2f dL1=%+5.2f dL2=%+5.2f dLT=%+5.2f dF=%+5.2f maxB=%5.2f | dB:",
                        rname, wMov.earlyDb - wSet.earlyDb, wMov.mid80_150Db - wSet.mid80_150Db,
                        wMov.late1Db - wSet.late1Db, wMov.late2Db - wSet.late2Db,
                        wMov.lateTotDb - wSet.lateTotDb, wMov.fullTotDb - wSet.fullTotDb, worstB);
            for (int b = 0; b < verb::NB; ++b) std::printf(" %+5.2f", wMov.bandLateTotDb[b] - wSet.bandLateTotDb[b]);
            std::printf("\n");
        };

        auto rU_tot = sumWin(rawU, &rawU, 0.0, 1e9), r0_tot = sumWin(raw0, &raw0, 0.0, 1e9), lf0_tot = sumWin(liveFin0, &liveRaw0, 0.0, 1e9);
        auto fU_tot = sumWin(finU, &rawU, 0.0, 1e9);

        // Partition-aligned ms boundaries: k=6 -> 74.67ms, k=7 -> 85.33ms, k=13 -> 149.33ms, k=37 -> 405.33ms
        const double t0_75 = 74.6667, t0_85 = 85.3333, t1 = 149.3333, t2 = 405.3333;
        auto w_eval = [&](const char* name, const std::vector<double>& boundsMs, const std::vector<int>& edgeK,
                          bool useFinU, bool useBakRef, int regMode, bool useLR2) {
            const size_t W = edgeK.size() + 1;
            std::vector<BandWin> uWins(W), refWins(W);
            for (size_t w = 0; w < W; ++w) {
                const double a = (w == 0) ? 0.0 : boundsMs[w - 1];
                const double b = (w + 1 == W) ? 1e9 : boundsMs[w];
                uWins[w] = sumWin(useFinU ? finU : rawU, &rawU, a, b);
                refWins[w] = sumWin(useBakRef ? (useFinU ? fin0 : raw0) : liveFin0, useBakRef ? &raw0 : &liveRaw0, a, b);
            }
            const auto& uTot = useFinU ? fU_tot : rU_tot;
            const auto& refTot = useBakRef ? (useFinU ? sumWin(fin0, &raw0, 0.0, 1e9) : r0_tot) : lf0_tot;
            std::vector<std::array<double, verb::NB>> gWins(W);
            for (size_t w = 0; w < W; ++w) {
                double uWinSum = 0, rWinSum = 0;
                for (int b = 0; b < verb::NB; ++b) { uWinSum += uWins[w].e[b]; rWinSum += refWins[w].e[b]; }
                double uAllSum = 0, rAllSum = 0;
                for (int b = 0; b < verb::NB; ++b) { uAllSum += uTot.e[b]; rAllSum += refTot.e[b]; }
                const double gAllBroad = std::sqrt(std::max(1e-30, uAllSum) / std::max(1e-30, rAllSum));
                // Fallback for window w: window broadband ratio (blended toward previous window if window itself is empty)
                double gWinBroad = (uWinSum > 0 && rWinSum > 0) ? std::sqrt(uWinSum / rWinSum) : (w > 0 ? gWins[w - 1][0] : gAllBroad);
                if (w > 0 && (uWinSum < 1e-7 * uAllSum || rWinSum < 1e-7 * rAllSum)) {
                    gWinBroad = gWins[w - 1][0];
                }
                for (int b = 0; b < verb::NB; ++b) {
                    const double gTot = std::sqrt(std::max(1e-30, uTot.e[b]) / std::max(1e-30, refTot.e[b]));
                    const double gRaw = (uWins[w].e[b] > 0 && refWins[w].e[b] > 0)
                        ? std::sqrt(uWins[w].e[b] / refWins[w].e[b]) : gWinBroad;
                    if (regMode == 0) {
                        gWins[w][b] = std::clamp(gRaw, 1e-6, 1e6);
                    } else if (regMode == 1) {
                        // regMode 1: fallback toward window broadband gain gWinBroad based on window-band fraction & ESS
                        const double fU = uWinSum > 0 ? uWins[w].e[b] / uWinSum : 0.0;
                        const double fR = rWinSum > 0 ? refWins[w].e[b] / rWinSum : 0.0;
                        const double essMin = std::min(uWins[w].ess[b], refWins[w].ess[b]);
                        const double tEss = std::clamp((essMin - 4.0) / 12.0, 0.0, 1.0);
                        const double tFrac = std::clamp((std::min(fU, fR) - 1e-3) / 1e-2, 0.0, 1.0);
                        const double alpha = std::min(tEss, tFrac);
                        const double logG = std::log(std::clamp(gWinBroad, 1e-6, 1e6)) + alpha * (std::log(std::clamp(gRaw, 1e-6, 1e6)) - std::log(std::clamp(gWinBroad, 1e-6, 1e6)));
                        gWins[w][b] = std::exp(logG);
                    } else {
                        // regMode 2: fallback toward gWinBroad using only uWins[w].ess[b] (since reference slot also has live ESS or baked)
                        const double fU = uWinSum > 0 ? uWins[w].e[b] / uWinSum : 0.0;
                        const double fR = rWinSum > 0 ? refWins[w].e[b] / rWinSum : 0.0;
                        const double essU = uWins[w].ess[b];
                        const double tEss = std::clamp((essU - 5.0) / 15.0, 0.0, 1.0);
                        const double tFrac = std::clamp((std::min(fU, fR) - 5e-4) / 5e-3, 0.0, 1.0);
                        const double alpha = std::min(tEss, tFrac);
                        const double logG = std::log(std::clamp(gWinBroad, 1e-6, 1e6)) + alpha * (std::log(std::clamp(gRaw, 1e-6, 1e6)) - std::log(std::clamp(gWinBroad, 1e-6, 1e6)));
                        gWins[w][b] = std::exp(logG);
                    }
                    (void)gTot;
                }
            }
            auto yL = applyMultiWin(s0L, edgeK, gWins, useLR2);
            auto yR = applyMultiWin(s0R, edgeK, gWins, useLR2);
            printRes(name, yL, yR);
        };

        // Baseline single-window raw/live0 (1-pole)
        {
            std::vector<std::array<double, verb::NB>> gw(1);
            for (int b = 0; b < verb::NB; ++b) gw[0][b] = std::sqrt(std::max(1e-30, rU_tot.e[b]) / std::max(1e-30, lf0_tot.e[b]));
            printRes("1win_tot_1p", applyMultiWin(s0L, {}, gw, false), applyMultiWin(s0R, {}, gw, false));
        }
        w_eval("3w_liv0_wb1p",     {t1, t2},         {13, 37},    false, false, 1, false);
        w_eval("3w_liv0_wbL2",     {t1, t2},         {13, 37},    false, false, 1, true);
        w_eval("4w75_liv0_wb1p",   {t0_75, t1, t2},  {6, 13, 37}, false, false, 1, false);
        w_eval("4w75_liv0_wbL2",   {t0_75, t1, t2},  {6, 13, 37}, false, false, 1, true);
        w_eval("4w75_liv0_wb2_1p", {t0_75, t1, t2},  {6, 13, 37}, false, false, 2, false);
        w_eval("4w75_liv0_wb2_L2", {t0_75, t1, t2},  {6, 13, 37}, false, false, 2, true);
        w_eval("4w75_bak0_wbL2",   {t0_75, t1, t2},  {6, 13, 37}, false, true,  1, true);
        w_eval("4w85_liv0_wbL2",   {t0_85, t1, t2},  {7, 13, 37}, false, false, 1, true);
        if (tag == "flate" && std::fabs(u - 1.0) < 1e-6) {
            auto segDb = [&](const std::vector<float>& L, const std::vector<float>& R, size_t a, size_t b) {
                double e = 0; for (size_t i = a; i < b && i < L.size(); ++i) e += 0.5 * (L[i]*L[i] + R[i]*R[i]);
                return 10.0 * std::log10(std::max(1e-20, e / std::max<size_t>(1, b - a)));
            };
            auto wRawU = sumWin(rawU, &rawU, 0.0, t0_75), wFin0 = sumWin(liveFin0, &liveRaw0, 0.0, t0_75), wRaw0 = sumWin(raw0, &raw0, 0.0, t0_75);
            double sRawU = 0, sFin0 = 0, sRaw0 = 0;
            for (int b = 0; b < verb::NB; ++b) { sRawU += wRawU.e[b]; sFin0 += wFin0.e[b]; sRaw0 += wRaw0.e[b]; }
            std::printf("  DIAG u=1 w0(0-74.67): rawU=%.2f fin0=%.2f raw0=%.2f (ratio_liv=%+.2f bak=%+.2f) | s0[0-3584]=%.2f s0[3584-3840]=%.2f su[0-3584]=%.2f su[3584-3840]=%.2f\n",
                        10*std::log10(std::max(1e-20, sRawU)), 10*std::log10(std::max(1e-20, sFin0)), 10*std::log10(std::max(1e-20, sRaw0)),
                        10*std::log10(std::max(1e-20, sRawU)/std::max(1e-20, sFin0)), 10*std::log10(std::max(1e-20, sRawU)/std::max(1e-20, sRaw0)),
                        segDb(s0L, s0R, 0, 3584), segDb(s0L, s0R, 3584, 3840), segDb(suL, suR, 0, 3584), segDb(suL, suR, 3584, 3840));
            std::printf("  DIAG u=1 w0 ess(rawU/fin0): ");
            for (int b = 0; b < verb::NB; ++b) std::printf("b%d=%.1f/%.1f ", b, wRawU.ess[b], wFin0.ess[b]);
            std::printf("\n");
        }
    }
    std::fflush(stdout);
    proc.releaseResources();
}

inline int run() {
    std::printf("=== P6: Baked Motion Late-Tail Overshoot vs Settled Exact IR ===\n");
    std::fflush(stdout);

    if (std::getenv("VERB_P6_SIM")) {
        simCandidateWindows(repoFile("out/P4/impulse/Flate-with-bake.verbroom"), "flate", verbp::kListener, "lpath", {0.1, 0.3, 0.5, 0.7, 0.9, 1.0});
        simCandidateWindows(repoFile("test/Built-in demo motion.verbroom"), "demo-motion", 0, VerbProcessor::pathParamId(0), {0.1, 0.3, 0.5, 0.7, 0.9, 0.975});
        return 0;
    }
    if (std::getenv("VERB_P6_DIAG_HIST")) {
        printSceneDiag(repoFile("out/P4/impulse/Flate-with-bake.verbroom"), "flate", verbp::kListener, "lpath");
        printSceneDiag(repoFile("test/Built-in demo motion.verbroom"), "demo-motion", 0, VerbProcessor::pathParamId(0));
        return 0;
    }

    namespace fs = std::filesystem;
    const char* outEnv = std::getenv("VERB_P6_OUT");
    const fs::path outDir(outEnv && *outEnv ? outEnv : "E:/Claude/verb/out/P6");
    fs::create_directories(outDir);

    const char* tagEnv = std::getenv("VERB_P6_TAG");
    const std::string runTag = (tagEnv && *tagEnv) ? std::string("-") + tagEnv : std::string();
    const bool forceFresh = std::getenv("VERB_P6_FRESH_BAKE") != nullptr;

    SceneSweepSummary flateSum, demoSum;
    if (!runSceneEvaluation(
            repoFile("out/P4/impulse/Flate-with-bake.verbroom"),
            "flate",
            verbp::kListener,
            "lpath",
            outDir,
            runTag,
            forceFresh,
            flateSum)) {
        return 1;
    }

    if (!runSceneEvaluation(
            repoFile("test/Built-in demo motion.verbroom"),
            "demo-motion",
            0, // source 0 ("Voice") has the 4-point smooth path
            VerbProcessor::pathParamId(0),
            outDir,
            runTag,
            true,
            demoSum)) {
        return 1;
    }

    if (!std::getenv("VERB_P6_DIAG_ONLY")) {
        check(flateSum.maxAbsLateTotErrDb <= 2.0,
              "P6 Flate: |baked moving - settled exact| <= 2.0 dB in lateTot (150-1000 ms) for all u in [0.1, 1.0]");
        check(flateSum.maxAbsLate1ErrDb <= 2.0,
              "P6 Flate: |baked moving - settled exact| <= 2.0 dB in late1 (150-400 ms) for all u in [0.1, 1.0]");
        check(flateSum.maxAbsLate2ErrDb <= 2.0,
              "P6 Flate: |baked moving - settled exact| <= 2.0 dB in late2 (400-1000 ms) for all u in [0.1, 1.0]");
        double maxLow6Db = 0.0;
        for (int b = 0; b < verb::NB - 1; ++b) maxLow6Db = std::max(maxLow6Db, flateSum.maxAbsBandErrDb[b]);
        check(maxLow6Db <= 2.0,
              "P6 Flate: |baked moving - settled exact| <= 2.0 dB in octave bands 125..4000 Hz at default 48k adaptive for all u in [0.1, 1.0]");
        check(flateSum.maxAbsAnyBandConvergedDb <= 2.0,
              "P6 Flate: |baked moving - converged settled exact| <= 2.0 dB in every octave band (125..8000 Hz) for all u in [0.1, 1.0]");
    }
    return fails ? 1 : 0;
}

} // namespace verb_p6_test

static int testP6() {
    return verb_p6_test::run();
}
