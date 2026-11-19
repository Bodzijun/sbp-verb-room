#pragma once
// P1 (bake slowdown) diagnostic harness. Not a pass/fail suite: VERB_ONLY_BAKE_PERF=1 [VERB_BAKE_PERF_VARIANT=asis|box|half]
// [VERB_BAKE_PERF_FILE=test/Flate.verbroom] [VERB_BAKE_PERF_OUT=out/P1/bake-perf]. Loads the preset through the processor,
// runs the listener path bake and writes per-station / per-decision CSV from Engine::bakeTrace() (worker-side timing only).
#include "E9bTest.h"
#include <fstream>
#include <algorithm>
static int testBakePerf() {
    const char* variantEnv = std::getenv("VERB_BAKE_PERF_VARIANT");
    const std::string variant = variantEnv ? variantEnv : "asis";
    const char* fileEnv = std::getenv("VERB_BAKE_PERF_FILE");
    const char* outEnv = std::getenv("VERB_BAKE_PERF_OUT");
    const juce::File file = repoFile(fileEnv ? fileEnv : "test/Flate.verbroom");
    const juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile(outEnv ? outEnv : "out/P1/bake-perf");
    outDir.createDirectory();
    auto xml = juce::parseXML(file.loadFileAsString());
    if (!xml) { std::printf("BAKEPERF: cannot parse %s\n", file.getFullPathName().toRawUTF8()); return 1; }
    if (auto* st = xml->getChildByName("VerbState")) {
        juce::String text = st->getStringAttribute("sceneText");
        if (variant == "box") {
            juce::StringArray keep;
            int wallIdx = 0;
            for (auto line : juce::StringArray::fromLines(text)) {
                if (line.startsWith("wall ")) { if (wallIdx++ >= 4) continue; }
                else if (line.startsWith("#@verbroom-editor-v1 wall ")) {
                    const int idx = line.fromFirstOccurrenceOf("wall ", false, false).getIntValue();
                    if (idx >= 4) continue;
                }
                keep.add(line);
            }
            text = keep.joinIntoString("\n") + "\n";
        } else if (variant == "half") {
            text = text.replace("rays 50000", "rays 25000");
            st->setAttribute("tailRays", st->getIntAttribute("tailRays") / 2);
        }
        st->setAttribute("sceneText", text);
    }
    VerbProcessor p; p.setPlayConfigDetails(2, 2, 48000, 128); p.prepareToPlay(48000, 128);
    if (!waitReady(p, 90)) { std::printf("BAKEPERF: processor not ready\n"); return 1; }
    juce::String error;
    if (!p.loadRoomPresetText(xml->toString(), error) || !waitReady(p, 120)) { std::printf("BAKEPERF: load failed %s\n", error.toRawUTF8()); return 1; }
    if (!p.startPathBake(verbp::kListener)) { std::printf("BAKEPERF: startPathBake refused\n"); return 1; }
    juce::AudioBuffer<float> buffer(2, 128); juce::MidiBuffer midi;
    auto engine = E9bAccess::engine(p);
    const auto start = std::chrono::steady_clock::now();
    int last = -1; uint64_t blocks = 0; const double limitSec = 1500;
    std::vector<std::pair<double, int>> percentTimeline;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < limitSec) {
        buffer.clear(); stage2Process(p, buffer, midi);
        if (++blocks % 16 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        juce::Thread::sleep(1);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const int second = (int)(sec * 2);
        if (second != last) {
            last = second; auto s = engine->bakeStats();
            std::printf("BAKEPERF t%.1f web%d engine%d done%d/%d stations%llu wait%d state%d\n", sec, p.pathBakePercent(), s.percent, s.done, s.total, (unsigned long long)s.stations, s.waitReason, (int)s.state);
            percentTimeline.push_back({sec, p.pathBakePercent()});
        }
        if (p.pathBakePercent() == 100) break;
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    auto stats = engine->bakeStats(); auto trace = engine->bakeTrace(); auto table = engine->bakedTableSnapshot();
    const juce::String tag = juce::String(variant);
    // P3: progress stall analysis on the sampled timeline (monotonicity + longest time on one integer value).
    {
        double maxStall = 0, stallStart = 0; int stallPct = -1, lastPct = -1; bool backwards = false;
        for (auto& pt : percentTimeline) {
            if (pt.second < lastPct) backwards = true; lastPct = pt.second;
            if (pt.second != stallPct) { stallPct = pt.second; stallStart = pt.first; }
            maxStall = std::max(maxStall, pt.first - stallStart);
        }
        std::printf("BAKEPERF_PROGRESS maxSecondsOnOneValue=%.1f wentBackwards=%d samples=%zu\n", maxStall, (int)backwards, percentTimeline.size());
    }
    // P3: dump every published table (u nodes + raw band energies) for offline level/jump analysis against the D1 reference.
    {
        auto tables = engine->bakedTablesSnapshot();
        std::ofstream tb((outDir.getChildFile("table-" + tag + ".csv")).getFullPathName().toStdString());
        tb << "table,object,group,u,selectedRays,discontinuities,si,r,b,energy\n";
        for (size_t ti = 0; ti < tables.size(); ++ti) {
            const auto& t = *tables[ti]; const size_t stride = t.stride();
            for (size_t j = 0; j < t.u.size(); ++j)
                for (size_t si = 0; si < t.groupSources.size(); ++si)
                    for (int r = 0; r < t.receivers; ++r)
                        for (int b = 0; b < verb::NB; ++b)
                            tb << ti << "," << t.path.object << "," << t.path.group << "," << t.u[j] << "," << t.selectedRays << "," << t.discontinuities << "," << si << "," << r << "," << b << "," << t.energy[j * stride + (si * (size_t)t.receivers + (size_t)r) * verb::NB + (size_t)b] << "\n";
        }
    }
    {
        std::ofstream f((outDir.getChildFile("stations-" + tag + ".csv")).getFullPathName().toStdString());
        f << "group,u,kind,rays,queuedMs,doneMs,durMs,activeAtQueue,retry\n";
        for (auto& s : trace.stations) f << s.group << "," << s.u << "," << (s.kind ? "validation" : "initial") << "," << s.rays << "," << s.queuedMs << "," << s.doneMs << "," << (s.doneMs - s.queuedMs) << "," << s.activeAtQueue << "," << (s.retry ? 1 : 0) << "\n";
        std::ofstream d((outDir.getChildFile("decisions-" + tag + ".csv")).getFullPathName().toStdString());
        d << "group,a,b,error,decision,nodesAtDecision,ms\n";
        for (auto& x : trace.decisions) d << x.group << "," << x.a << "," << x.b << "," << x.error << "," << (x.decision == 0 ? "accept" : x.decision == 1 ? "split" : "approx") << "," << x.nodes << "," << x.ms << "\n";
        std::ofstream t((outDir.getChildFile("percent-" + tag + ".csv")).getFullPathName().toStdString());
        t << "sec,percent\n"; for (auto& pt : percentTimeline) t << pt.first << "," << pt.second << "\n";
    }
    // Summary
    double initEnd = 0, initSum = 0, valSum = 0, maxDur = 0, firstVal = 1e30, lastDone = 0; int nInit = 0, nVal = 0, nRetry = 0, maxActive = 0;
    for (auto& s : trace.stations) {
        const double dur = s.doneMs - s.queuedMs; maxDur = std::max(maxDur, dur); lastDone = std::max(lastDone, s.doneMs); maxActive = std::max(maxActive, s.activeAtQueue + 1);
        if (s.kind == 0) { initSum += dur; ++nInit; initEnd = std::max(initEnd, s.doneMs); } else { valSum += dur; ++nVal; firstVal = std::min(firstVal, s.queuedMs); }
        if (s.rays > 0 && s.retry) ++nRetry;
    }
    // concurrency profile: sample every 50 ms
    double lowTimeMs = 0, sampleMs = 50; int samples = 0, lowSamples = 0; double avgConc = 0;
    for (double t = 0; t < lastDone; t += sampleMs) {
        int c = 0; for (auto& s : trace.stations) if (s.queuedMs <= t && s.doneMs > t) ++c;
        ++samples; avgConc += c; if (c <= 2) { ++lowSamples; lowTimeMs += sampleMs; }
    }
    int splits = 0, accepts = 0, approx = 0; for (auto& x : trace.decisions) { if (x.decision == 1) ++splits; else if (x.decision == 0) ++accepts; else ++approx; }
    const double lastDec = trace.decisions.empty() ? 0 : trace.decisions.back().ms;
    std::printf("BAKEPERF_SUMMARY variant=%s totalWallSec=%.2f engineMs=%.0f workers=%d activeLimit=%d nodes=%zu discontinuities=%d percent=%d hash=%016llx selectedRays=%d\n", variant.c_str(), total, stats.milliseconds, trace.workers, trace.activeLimit, table ? table->u.size() : 0, stats.discontinuities, p.pathBakePercent(), (unsigned long long)stats.hash, table ? table->selectedRays : 0);
    std::printf("BAKEPERF_SUMMARY stationsInitial=%d stationsValidation=%d initialPhaseEndMs=%.0f initialSumMs=%.0f validationSumMs=%.0f maxStationMs=%.0f lastDoneMs=%.0f\n", nInit, nVal, initEnd, initSum, valSum, maxDur, lastDone);
    std::printf("BAKEPERF_SUMMARY decisions accept=%d split=%d approx=%d lastDecisionMs=%.0f avgConcurrency=%.2f samplesWithLE2active=%d/%d lowConcurrencyMs=%.0f maxStepGapMs=%.1f stepCalls=%llu waitSteps=%llu\n", accepts, splits, approx, lastDec, samples ? avgConc / samples : 0., lowSamples, samples, lowTimeMs, trace.maxStepGapMs, (unsigned long long)trace.stepCalls, (unsigned long long)trace.waitSteps);
    p.releaseResources();
    return stats.percent == 100 || p.pathBakePercent() == 100 ? 0 : 1;
}
