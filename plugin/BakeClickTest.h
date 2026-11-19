#pragma once
// P2 (baked playback crackle) diagnostic harness. Not a pass/fail suite unless VERB_BAKE_CLICK_GATE=1.
//   VERB_ONLY_BAKE_CLICK=1 [VERB_BAKE_CLICK_VARIANT=half|asis] [VERB_BAKE_CLICK_FILE=test/Flate.verbroom]
//   [VERB_BAKE_CLICK_SECONDS=20] [VERB_BAKE_CLICK_BLOCKS=64,256,512] [VERB_BAKE_CLICK_OUT=out/P2/baked-crackle]
// Loads the preset through the processor, renders the listener path (lpath ramp 0..1, host-automation style) with
// bass / kick / pink inputs, first WITHOUT a bake, then after the bake has finished, and counts clicks as outliers of the
// second difference against a robust local sigma. Positions are mapped to path phase u and to the 512/128-sample grid.
#include "E9bTest.h"
#include <fstream>
#include <map>
#include <algorithm>
namespace bakeclick {
struct Click { size_t pos = 0; int ch = 0; double ratio = 0, abs2 = 0; };
struct Result { int count = 0; double peakRatio = 0, peakAbs = 0; std::vector<Click> clicks; double rmsOut = 0; double peakOut = 0; };
inline std::vector<float> makeInput(int kind, size_t n, double sr) {
    std::vector<float> x(n, 0.f);
    const double twoPi = 6.283185307179586;
    if (kind == 0) { // 40 -> 80 Hz sweep (log), amplitude 0.5
        double ph = 0; const double f0 = 40, f1 = 80;
        for (size_t i = 0; i < n; ++i) { const double f = f0 * std::pow(f1 / f0, (double)i / (double)n); ph += twoPi * f / sr; x[i] = (float)(0.5 * std::sin(ph)); }
    } else if (kind == 1) { // 60 Hz kick bursts at 2 Hz, 5 ms attack, 90 ms decay
        const size_t period = (size_t)(sr / 2);
        for (size_t i = 0; i < n; ++i) {
            const double t = (double)(i % period) / sr;
            const double env = std::min(1.0, t / 0.005) * std::exp(-t / 0.09);
            x[i] = (float)(0.7 * env * std::sin(twoPi * (60 + 40 * std::exp(-t / 0.03)) * t));
        }
    } else { // pink noise (Kellet), rms ~0.15
        uint32_t s = 12345; double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (size_t i = 0; i < n; ++i) {
            s = s * 1664525u + 1013904223u; const double w = ((double)(s >> 8) / 8388608.0) - 1.0;
            b0 = .99886 * b0 + w * .0555179; b1 = .99332 * b1 + w * .0750759; b2 = .969 * b2 + w * .153852;
            b3 = .8665 * b3 + w * .3104856; b4 = .55 * b4 + w * .5329522; b5 = -.7616 * b5 - w * .016898;
            const double pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * .5362; b6 = w * .115926;
            x[i] = (float)(pink * 0.15 * 0.5);
        }
    }
    return x;
}
// second-difference outliers against the robust local sigma (median |d2| / 0.6745 over +-1024 samples)
inline Result detect(const std::vector<float>& y, int ch, double minRatio = 15.0, double floorAbs = 2e-6) {
    Result r; const size_t n = y.size(); if (n < 4096) return r;
    std::vector<float> d2(n, 0.f);
    for (size_t i = 2; i < n; ++i) d2[i] = y[i] - 2.f * y[i - 1] + y[i - 2];
    const size_t hop = 256, half = 1024;
    std::vector<float> sigma((n + hop - 1) / hop + 1, 0.f), tmp;
    for (size_t h = 0; h < sigma.size(); ++h) {
        const size_t c = h * hop; const size_t lo = c > half ? c - half : 0, hi = std::min(n, c + half + hop);
        tmp.clear(); for (size_t i = lo; i < hi; ++i) tmp.push_back(std::fabs(d2[i]));
        if (tmp.empty()) continue; std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        sigma[h] = tmp[tmp.size() / 2] / 0.6745f;
    }
    double e = 0; for (size_t i = 0; i < n; ++i) { e += (double)y[i] * y[i]; r.peakOut = std::max(r.peakOut, (double)std::fabs(y[i])); }
    r.rmsOut = std::sqrt(e / (double)n);
    for (size_t i = 2; i < n; ++i) {
        const float sg = std::max(sigma[i / hop], 1e-9f); const double a = std::fabs(d2[i]);
        if (a < floorAbs || a / sg < minRatio) continue;
        if (!r.clicks.empty() && r.clicks.back().ch == ch && i - r.clicks.back().pos < 16) {
            if (a / sg > r.clicks.back().ratio) { r.clicks.back().ratio = a / sg; r.clicks.back().abs2 = a; r.clicks.back().pos = i; }
        } else r.clicks.push_back({i, ch, a / sg, a});
    }
    for (auto& c : r.clicks) { r.peakRatio = std::max(r.peakRatio, c.ratio); r.peakAbs = std::max(r.peakAbs, c.abs2); }
    r.count = (int)r.clicks.size(); return r;
}
}
static int testBakeClick() {
    using namespace bakeclick;
    const char* variantEnv = std::getenv("VERB_BAKE_CLICK_VARIANT"); const std::string variant = variantEnv ? variantEnv : "half";
    const char* fileEnv = std::getenv("VERB_BAKE_CLICK_FILE"); const char* outEnv = std::getenv("VERB_BAKE_CLICK_OUT");
    const char* secEnv = std::getenv("VERB_BAKE_CLICK_SECONDS"); const char* blkEnv = std::getenv("VERB_BAKE_CLICK_BLOCKS");
    const char* tagEnv = std::getenv("VERB_BAKE_CLICK_TAG");
    const double rampSec = secEnv ? std::atof(secEnv) : 20.0;
    const bool gate = std::getenv("VERB_BAKE_CLICK_GATE") != nullptr;
    const double holdU = std::getenv("VERB_BAKE_CLICK_HOLD") ? std::atof(std::getenv("VERB_BAKE_CLICK_HOLD")) : -1.0;  // >=0: park the listener at this u instead of the ramp
    const double skipSec = std::getenv("VERB_BAKE_CLICK_SKIP") ? std::atof(std::getenv("VERB_BAKE_CLICK_SKIP")) : 0.5;
    std::vector<int> blocks; { juce::StringArray sa; sa.addTokens(blkEnv ? blkEnv : "64,256,512", ",", ""); for (auto& s : sa) if (s.getIntValue() > 0) blocks.push_back(s.getIntValue()); }
    const juce::File file = repoFile(fileEnv ? fileEnv : "test/Flate.verbroom");
    const juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile(outEnv ? outEnv : "out/P2/baked-crackle");
    outDir.createDirectory();
    const juce::String tag = juce::String(variant) + (tagEnv ? juce::String("-") + tagEnv : juce::String());
    auto xml = juce::parseXML(file.loadFileAsString());
    if (!xml) { std::printf("BAKECLICK: cannot parse\n"); return 1; }
    if (auto* st = xml->getChildByName("VerbState")) {
        if (variant == "half") {
            juce::String text = st->getStringAttribute("sceneText").replace("rays 50000", "rays 25000");
            st->setAttribute("sceneText", text); st->setAttribute("tailRays", st->getIntAttribute("tailRays") / 2);
        }
    }
    const int maxBlock = *std::max_element(blocks.begin(), blocks.end());
    VerbProcessor p; p.setPlayConfigDetails(2, 2, 48000, maxBlock); p.prepareToPlay(48000, maxBlock);
    if (!waitReady(p, 90)) { std::printf("BAKECLICK: not ready\n"); return 1; }
    juce::String error;
    if (!p.loadRoomPresetText(xml->toString(), error) || !waitReady(p, 120)) { std::printf("BAKECLICK: load failed %s\n", error.toRawUTF8()); return 1; }
    auto engine = E9bAccess::engine(p);
    { auto td = engine->tailDiagnostics(); std::printf("BAKECLICK status: %s | sharedTail=%d\n", p.getStatus().toRawUTF8(), td ? (int)td->shared : -1); }
    juce::MidiBuffer midi; const double sr = 48000;
    const size_t rampN = (size_t)(rampSec * sr);
    const char* kindName[3] = {"bass-sweep", "kick-train", "pink-noise"};
    std::ofstream csv((outDir.getChildFile("clicks-" + tag + ".csv")).getFullPathName().toStdString());
    csv << "phase,input,block,count,peakRatio,peakD2,rms,peak,ptsPer512Grid\n";
    std::ofstream detail((outDir.getChildFile("click-positions-" + tag + ".csv")).getFullPathName().toStdString());
    detail << "phase,input,block,ch,sample,sec,u,mod512,mod128,ratio,d2\n";
    int fails0 = 0;
    std::map<std::string, int> counts;
    auto renderOne = [&](const char* phase, int kind, int bs) {
        juce::AudioBuffer<float> buf(2, bs);
        auto setU = [&](double u) { setP(p, "lpath", u); };
        setU(0.0); { juce::AudioBuffer<float> z(2, bs); const int settle = (int)(2.0 * sr / bs); for (int i = 0; i < settle; ++i) { z.clear(); stage2Process(p, z, midi); } }
        // pre-roll 1 s at u=0 with signal so the tail is filled, then the ramp
        const size_t preN = (size_t)sr, total = preN + rampN + (size_t)(2 * sr);
        auto x = makeInput(kind, total, sr);
        std::vector<float> yl(total), yr(total);
        size_t pos = 0; size_t tick = 0; std::vector<double> blockUs;
        while (pos + (size_t)bs <= total) {
            double u = pos < preN ? 0.0 : (holdU >= 0 ? holdU : std::min(1.0, (double)(pos - preN + bs / 2) / (double)rampN));
            setU(u);
            for (int i = 0; i < bs; ++i) { buf.setSample(0, i, x[pos + i]); buf.setSample(1, i, x[pos + i]); }
            const auto c0 = std::chrono::steady_clock::now();
            stage2Process(p, buf, midi);
            if (pos >= preN && pos < preN + rampN) blockUs.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - c0).count());
            for (int i = 0; i < bs; ++i) { yl[pos + i] = buf.getSample(0, i); yr[pos + i] = buf.getSample(1, i); }
            pos += (size_t)bs;
            if (++tick % 64 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil(0);
        }
        yl.resize(pos); yr.resize(pos);
        double cpuMean = 0, cpuP99 = 0, cpuMax = 0;
        if (!blockUs.empty()) { for (double v : blockUs) { cpuMean += v; cpuMax = std::max(cpuMax, v); } cpuMean /= (double)blockUs.size(); std::sort(blockUs.begin(), blockUs.end()); cpuP99 = blockUs[(size_t)((double)blockUs.size() * 0.99)]; }
        const double budgetUs = 1e6 * bs / sr;
        std::printf("BAKECLICK_CPU %-6s %-10s block%-4d mean %.1f us p99 %.1f us max %.1f us budget %.0f us mean/budget %.4f\n", phase, kindName[kind], bs, cpuMean, cpuP99, cpuMax, budgetUs, cpuMean / budgetUs);
        auto L = detect(yl, 0), R = detect(yr, 1);
        // skip the first 0.5 s of the pre-roll (tail filling)
        auto keep = [&](Result& r) { std::vector<Click> k; for (auto& c : r.clicks) if (c.pos > (size_t)(skipSec * sr)) k.push_back(c); r.clicks = k; r.count = (int)k.size(); r.peakRatio = r.peakAbs = 0; for (auto& c : k) { r.peakRatio = std::max(r.peakRatio, c.ratio); r.peakAbs = std::max(r.peakAbs, c.abs2); } };
        keep(L); keep(R);
        const int count = L.count + R.count; const double pr = std::max(L.peakRatio, R.peakRatio), pa = std::max(L.peakAbs, R.peakAbs);
        int grid[16] = {};
        auto emit = [&](const Result& r) {
            for (auto& c : r.clicks) {
                const double u = c.pos < preN ? 0.0 : std::min(1.0, (double)(c.pos - preN) / (double)rampN);
                detail << phase << "," << kindName[kind] << "," << bs << "," << c.ch << "," << c.pos << "," << (double)c.pos / sr << "," << u << "," << (c.pos % 512) << "," << (c.pos % 128) << "," << c.ratio << "," << c.abs2 << "\n";
                ++grid[(c.pos % 512) / 32];
            }
        };
        emit(L); emit(R);
        std::string g; for (int i = 0; i < 16; ++i) g += std::to_string(grid[i]) + (i < 15 ? "/" : "");
        csv << phase << "," << kindName[kind] << "," << bs << "," << count << "," << pr << "," << pa << "," << std::max(L.rmsOut, R.rmsOut) << "," << std::max(L.peakOut, R.peakOut) << "," << g << "\n"; csv.flush();
        std::printf("BAKECLICK %-6s %-10s block%-4d clicks%5d peakRatio%9.1f peakD2 %.3e rms %.4f peak %.4f grid512(32-sample bins) %s\n", phase, kindName[kind], bs, count, pr, pa, std::max(L.rmsOut, R.rmsOut), std::max(L.peakOut, R.peakOut), g.c_str());
        // first ten strongest positions
        std::vector<Click> all = L.clicks; all.insert(all.end(), R.clicks.begin(), R.clicks.end());
        std::sort(all.begin(), all.end(), [](const Click& a, const Click& b) { return a.ratio > b.ratio; });
        for (size_t i = 0; i < std::min<size_t>(5, all.size()); ++i) { const double u = all[i].pos < preN ? 0.0 : std::min(1.0, (double)(all[i].pos - preN) / (double)rampN); std::printf("    click ch%d t%.3fs u%.4f mod512=%zu mod128=%zu ratio %.1f d2 %.3e\n", all[i].ch, (double)all[i].pos / sr, u, all[i].pos % 512, all[i].pos % 128, all[i].ratio, all[i].abs2); }
        counts[std::string(phase) + "/" + kindName[kind] + "/" + std::to_string(bs)] = count;
        if (std::getenv("VERB_BAKE_CLICK_WAV") && bs == blocks.back()) {
            juce::WavAudioFormat wf; juce::File f = outDir.getChildFile(juce::String(phase) + "-" + kindName[kind] + "-" + tag + ".wav"); f.deleteFile();
            std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream()); if (os) { std::unique_ptr<juce::AudioFormatWriter> w(wf.createWriterFor(os.get(), sr, 2, 24, {}, 0)); if (w) { os.release(); juce::AudioBuffer<float> o(2, (int)yl.size()); o.copyFrom(0, 0, yl.data(), (int)yl.size()); o.copyFrom(1, 0, yr.data(), (int)yr.size()); w->writeFromAudioSampleBuffer(o, 0, o.getNumSamples()); } }
        }
    };
    auto allRenders = [&](const char* phase) { for (int kind = 0; kind < 3; ++kind) for (int bs : blocks) renderOne(phase, kind, bs); };
    p.clearPathBake();
    std::printf("BAKECLICK variant=%s ramp=%.1fs engine bake percent=%d\n", variant.c_str(), rampSec, p.pathBakePercent());
    allRenders("nobake");
    if (!p.startPathBake(verbp::kListener)) { std::printf("BAKECLICK: startPathBake refused\n"); return 1; }
    {
        juce::AudioBuffer<float> buffer(2, 128); const auto start = std::chrono::steady_clock::now(); uint64_t nb = 0; int lastSec = -1;
        std::vector<std::pair<double, int>> timeline; int lastPct = -1; double stallStart = 0, maxStall = 0; int stallPct = -1; bool backwards = false; uint64_t lastStations = 0; double lastStationT = 0, maxStationGap = 0;
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 1500) {
            buffer.clear(); stage2Process(p, buffer, midi); if (++nb % 16 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
            juce::Thread::sleep(1);
            const int sec = (int)std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (sec != lastSec && sec % 10 == 0) { lastSec = sec; std::printf("BAKECLICK bake t%d percent%d\n", sec, p.pathBakePercent()); }
            { const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); const int pc = p.pathBakePercent();
              if (pc != lastPct) { if (pc < lastPct) backwards = true; timeline.push_back({now, pc}); lastPct = pc; }
              { const auto bs2 = engine->bakeStats(); if (bs2.stations != lastStations) { maxStationGap = std::max(maxStationGap, now - lastStationT); lastStationT = now; lastStations = bs2.stations; } }
              if (pc != stallPct) { stallPct = pc; stallStart = now; } maxStall = std::max(maxStall, now - stallStart); }
            if (p.pathBakePercent() == 100) break;
        }
        { std::ofstream tf((outDir.getChildFile("bake-progress-" + tag + ".csv")).getFullPathName().toStdString()); tf << "sec,percent\n"; for (auto& pt : timeline) tf << pt.first << "," << pt.second << "\n"; }
        std::printf("BAKECLICK_PROGRESS steps=%zu maxSecondsOnOneValue=%.1f wentBackwards=%d final=%d maxGapBetweenCompletedStations=%.1f\n", timeline.size(), maxStall, (int)backwards, p.pathBakePercent(), maxStationGap);
    }
    auto stats = engine->bakeStats(); auto table = engine->bakedTableSnapshot();
    std::printf("BAKECLICK bake done percent=%d state=%d nodes=%zu discontinuities=%d hash=%016llx ms=%.0f\n", p.pathBakePercent(), (int)stats.state, table ? table->u.size() : 0, stats.discontinuities, (unsigned long long)stats.hash, stats.milliseconds);
    if (p.pathBakePercent() != 100) { std::printf("BAKECLICK bake did not finish\n"); return 1; }
    // target-gain trajectory from the baked table (receiver 0, source 0): max dB step per band between neighbouring u samples
    if (table && table->u.size() >= 2) {
        const size_t n = table->stride(); std::vector<double> a(n), b(n); const int NBd = 7;
        const int N = 2000; std::vector<std::array<double, 7>> prev(1); double maxStep[7] = {}, atU[7] = {}; double lastDb[7] = {}; bool first = true;
        std::ofstream tr((outDir.getChildFile("table-gain-" + tag + ".csv")).getFullPathName().toStdString());
        tr << "u,b0,b1,b2,b3,b4,b5,b6\n";
        for (int i = 0; i <= N; ++i) {
            const double u = (double)i / N; if (!table->lookup(u, a.data())) continue; tr << u;
            for (int bnd = 0; bnd < NBd; ++bnd) { const double db = 10 * std::log10(std::max(1e-30, a[(size_t)bnd])); tr << "," << db; if (!first && std::fabs(db - lastDb[bnd]) > maxStep[bnd]) { maxStep[bnd] = std::fabs(db - lastDb[bnd]); atU[bnd] = u; } lastDb[bnd] = db; }
            tr << "\n"; first = false;
        }
        std::printf("BAKECLICK table-gain max dB step per 1/%d of path (source0, receiver0), bands 125Hz..8k: ", N);
        for (int bnd = 0; bnd < NBd; ++bnd) std::printf("b%d %.3f@u%.3f  ", bnd, maxStep[bnd], atU[bnd]);
        std::printf("\n");
    }
    allRenders("baked");
    // verdict table
    std::printf("BAKECLICK_SUMMARY variant=%s\n", variant.c_str());
    for (auto& kv : counts) std::printf("BAKECLICK_SUMMARY %s clicks=%d\n", kv.first.c_str(), kv.second);
    p.releaseResources();
    if (gate) { // after the fix: baked bass must not exceed the no-bake count
        for (int bs : blocks) for (int k = 0; k < 2; ++k) { const int nb = counts[std::string("nobake/") + kindName[k] + "/" + std::to_string(bs)], bk = counts[std::string("baked/") + kindName[k] + "/" + std::to_string(bs)]; if (bk > nb + 2) { std::printf("BAKECLICK GATE FAIL %s block %d baked %d > nobake %d\n", kindName[k], bs, bk, nb); ++fails0; } }
    }
    return fails0;
}
