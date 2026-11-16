#pragma once
// E21: "Keep processing during silence" (infinite reported tail) + opt-in latency measurement of a moving pose.
//   VERB_ONLY_E21=1     functional checks (tail reporting, state round trip, old state, bit identity) + idle CPU cost
//   VERB_E21_MEASURE=1  Hypothesis B table: lag of direct / early / late components behind a ramp automation (blocks 256 and 1024)
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>

namespace e21 {
static std::string room() {
    std::ostringstream o;
    o << "sr 48000\nlength 1.0\nheight 3\norder 1\n"
      << "material Plaster 0.02 0.03 0.04 0.05 0.06 0.07 0.08 0.1 0.2\n"
      << "material Carpet 0.05 0.1 0.25 0.45 0.65 0.7 0.7 0.2 0.6\n"
      << "floor Carpet\nceiling Plaster\n"
      << "wall 0 0 9 0 Plaster\nwall 9 0 9 7 Plaster\nwall 9 7 0 7 Plaster\nwall 0 7 0 0 Plaster\n"
      << "listener 2 3 1.5 0\nsource A 3 3 1.5 0 0 0 0\nreceiver L 2 3 1.5 55 0.5\nreceiver R 2 3 1.5 -55 0.5\n";
    return o.str();
}
// Renders `blocks` blocks of `blk` samples; the input is silence except an optional unit click on ch0 at the first sample of block `clickBlock`.
struct Rig {
    VerbProcessor p; int blk;
    explicit Rig(int b, int tailRays = 12000) : blk(b) {
        p.setTailSettings({tailRays, false}); p.setPlayConfigDetails(2, 2, 48000, b); p.prepareToPlay(48000, b);
        waitReady(p, 90); p.loadSceneText(juce::String(room()), "E21 box", true); waitReady(p, 90);
    }
    void silence(int blocks) { juce::AudioBuffer<float> buf(2, blk); juce::MidiBuffer midi; for (int i = 0; i < blocks; ++i) { buf.clear(); p.processBlock(buf, midi); } }
    // click at the first sample of the first block; returns L/R for `total` samples
    void click(int total, std::vector<float>& L, std::vector<float>& R, const std::function<void(int)>& perBlock = {}) {
        juce::AudioBuffer<float> buf(2, blk); juce::MidiBuffer midi; L.clear(); R.clear();
        for (int done = 0, b = 0; done < total; done += blk, ++b) {
            if (perBlock) perBlock(b);
            buf.clear(); if (b == 0) buf.setSample(0, 0, 1.f);
            p.processBlock(buf, midi);
            for (int i = 0; i < blk; ++i) { L.push_back(buf.getSample(0, i)); R.push_back(buf.getSample(1, i)); }
        }
    }
};
struct Feat { double arrival = -1, early = -200, late = -200, ildD = 0, ildE = 0, ildL = 0; };
static double dbE(const std::vector<float>& L, const std::vector<float>& R, int a, int b) {
    double e = 0; a = std::max(a, 0); b = std::min<int>(b, (int)L.size());
    for (int i = a; i < b; ++i) e += (double)L[(size_t)i] * L[(size_t)i] + (double)R[(size_t)i] * R[(size_t)i];
    return 10 * std::log10(e + 1e-30);
}
static double dbCh(const std::vector<float>& X, int a, int b) { double e = 0; a = std::max(a, 0); b = std::min<int>(b, (int)X.size()); for (int i = a; i < b; ++i) e += (double)X[(size_t)i] * X[(size_t)i]; return 10 * std::log10(e + 1e-30); }
static Feat features(const std::vector<float>& L, const std::vector<float>& R) {
    Feat f; const int n = (int)L.size(); double pk = 0; const int search = std::min(n, 48000 * 6 / 100);
    for (int i = 0; i < search; ++i) pk = std::max(pk, (double)std::fabs(L[(size_t)i]) + std::fabs(R[(size_t)i]));
    if (pk <= 0) return f;
    for (int i = 1; i < search; ++i) {
        const double a = std::fabs(L[(size_t)i - 1]) + std::fabs(R[(size_t)i - 1]), b = std::fabs(L[(size_t)i]) + std::fabs(R[(size_t)i]);
        if (b >= 0.15 * pk) { f.arrival = i - 1 + (a >= 0.15 * pk ? 0.0 : (0.15 * pk - a) / std::max(b - a, 1e-30)); break; }
    }
    const int a0 = (int)std::floor(f.arrival), ms = 48;
    f.early = dbE(L, R, a0 + 2 * ms, a0 + 50 * ms); f.late = dbE(L, R, a0 + 120 * ms, a0 + 360 * ms);
    f.ildD = dbCh(L, a0 - 10, a0 + 60) - dbCh(R, a0 - 10, a0 + 60);
    f.ildE = dbCh(L, a0 + 2 * ms, a0 + 50 * ms) - dbCh(R, a0 + 2 * ms, a0 + 50 * ms);
    f.ildL = dbCh(L, a0 + 120 * ms, a0 + 360 * ms) - dbCh(R, a0 + 120 * ms, a0 + 360 * ms);
    return f;
}
// time (relative to ramp start) at which the static curve y(x_j) takes the measured value, nearest crossing to the current ramp position
static bool invert(const std::vector<double>& xs, const std::vector<double>& ys, double meas, double xNow, double& xHat) {
    bool found = false; double best = 1e30;
    for (size_t j = 0; j + 1 < xs.size(); ++j) {
        const double y0 = ys[j], y1 = ys[j + 1];
        if ((meas - y0) * (meas - y1) <= 0 && y0 != y1) {
            const double x = xs[j] + (meas - y0) / (y1 - y0) * (xs[j + 1] - xs[j]);
            if (std::fabs(x - xNow) < best) { best = std::fabs(x - xNow); xHat = x; found = true; }
        }
    }
    return found;
}
static void measureCase(const char* name, int blk, bool xCase) {
    const double x0 = xCase ? 3.0 : 0.0, x1 = xCase ? 6.0 : 60.0, T = 3.0, step = xCase ? 0.1 : 2.0, speed = (x1 - x0) / T;
    const char* id = xCase ? "s1x" : "laz"; const int total = 48000 / 2;
    std::vector<double> xs; std::vector<Feat> table;
    {   // static curves
        Rig r(blk);
        for (double x = x0 - step; x <= x1 + step + 1e-9; x += step) {
            setP(r.p, id, x); r.silence(2 * 48000 / blk + 2); std::vector<float> L, R; r.click(total, L, R);
            xs.push_back(x); table.push_back(features(L, R));
        }
    }
    const auto col = [&](double Feat::*m) { std::vector<double> v; for (const auto& t : table) v.push_back(t.*m); return v; };
    Rig r(blk); setP(r.p, id, x0); r.silence(2 * 48000 / blk + 2);
    const int spacing = 48000 * 4 / 10;   // 0.4 s between clicks
    const int clickBlocks = std::max(1, spacing / blk);
    std::printf("  [E21 lag] %s, block %d (ramp %g -> %g over %g s)\n", name, blk, x0, x1, T);
    std::printf("  [E21 lag]  t_click   x_now    %s\n", xCase ? "direct_lag_ms  early_lag_ms  late_lag_ms" : "dirILD_lag_ms  earlyILD_lag_ms  lateILD_lag_ms");
    double t = -0.8; const double ramp0 = 0.0;
    // the ramp starts at t = 0: before it the pose is x0
    int blockIndex = 0; double timeOfBlock0 = -0.8;
    for (int k = 0; k < 12; ++k, t += 0.4) {
        std::vector<float> L, R;
        const double tc = timeOfBlock0 + (double)blockIndex * blk / 48000.0;
        r.click(spacing, L, R, [&](int b) { const double tb = tc + (double)b * blk / 48000.0; setP(r.p, id, x0 + std::clamp((tb - ramp0) / T, 0.0, 1.0) * (x1 - x0)); });
        blockIndex += (spacing + blk - 1) / blk;
        const double xNow = x0 + std::clamp((tc - ramp0) / T, 0.0, 1.0) * (x1 - x0), v = speed;
        const Feat f = features(L, R);
        const auto lagOf = [&](double Feat::*m, double meas, bool& ok) { double xh = 0; ok = invert(xs, col(m), meas, xNow, xh); return ok ? (xNow - xh) / v * 1000.0 : 0.0; };
        bool ok1, ok2, ok3;
        const double l1 = lagOf(xCase ? &Feat::arrival : &Feat::ildD, xCase ? f.arrival : f.ildD, ok1);
        const double l2 = lagOf(xCase ? &Feat::early : &Feat::ildE, xCase ? f.early : f.ildE, ok2);
        const double l3 = lagOf(xCase ? &Feat::late : &Feat::ildL, xCase ? f.late : f.ildL, ok3);
        char a[24], b[24], c[24]; std::snprintf(a, 24, ok1 ? "%7.0f" : "    n/a", l1); std::snprintf(b, 24, ok2 ? "%7.0f" : "    n/a", l2); std::snprintf(c, 24, ok3 ? "%7.0f" : "    n/a", l3);
        std::printf("  [E21 lag]  %6.2f  %7.2f   %s %s %s\n", tc, xNow, a, b, c);
    }
    // settled comparison: last click vs the static value at x1 (table index of x1)
    size_t j1 = 0; for (size_t j = 0; j < xs.size(); ++j) if (std::fabs(xs[j] - x1) < 1e-6) j1 = j;
    std::vector<float> L, R; r.click(total, L, R); const Feat fe = features(L, R), fs = table[j1];
    std::printf("  [E21 lag]  settled vs static @%g: arrival %.1f vs %.1f smp, early %.2f vs %.2f dB, late %.2f vs %.2f dB, ILD d/e/l %.2f/%.2f/%.2f vs %.2f/%.2f/%.2f dB\n", x1,
        fe.arrival, fs.arrival, fe.early, fs.early, fe.late, fs.late, fe.ildD, fe.ildE, fe.ildL, fs.ildD, fs.ildE, fs.ildL);
}
} // namespace e21

static int testE21Measure() {
    const int before = fails;
    for (const int blk : {256, 1024}) { e21::measureCase("X ramp", blk, true); e21::measureCase("listener azimuth ramp", blk, false); }
    return fails - before;
}

static int testE21Plugin() {
    const int before = fails;
    const auto tree = [](VerbProcessor& p) { juce::MemoryBlock m; p.getStateInformation(m); auto xml = juce::AudioProcessor::getXmlFromBinary(m.getData(), (int)m.getSize()); return xml ? juce::ValueTree::fromXml(*xml) : juce::ValueTree(); };
    VerbProcessor p; p.setTailSettings({4000, false}); p.setPlayConfigDetails(2, 2, 48000, 256); p.prepareToPlay(48000, 256);
    check(waitReady(p, 90), "E21 processor ready");
    check(!p.keepProcessingEnabled() && p.getTailLengthSeconds() == 20.25, "E21 default OFF: reported tail stays 20.25 s");
    juce::MemoryBlock offState; p.getStateInformation(offState);
    check(tree(p).isValid() && !tree(p).hasProperty("keepProcessing"), "E21 OFF: project state has no keepProcessing field (old layout)");
    p.setKeepProcessingEnabled(true);
    check(p.keepProcessingEnabled() && std::isinf(p.getTailLengthSeconds()) && p.getTailLengthSeconds() > 0, "E21 ON: reported tail is +infinity (JUCE VST3 maps it to kInfiniteTail)");
    juce::MemoryBlock onState; p.getStateInformation(onState);
    check(tree(p).hasProperty("keepProcessing") && (bool)tree(p).getProperty("keepProcessing"), "E21 ON: project state carries keepProcessing=true");
    {
        VerbProcessor q; q.setTailSettings({4000, false}); q.setPlayConfigDetails(2, 2, 48000, 256); q.prepareToPlay(48000, 256);
        q.setStateInformation(onState.getData(), (int)onState.getSize()); waitReady(q, 90);
        check(q.keepProcessingEnabled() && std::isinf(q.getTailLengthSeconds()), "E21 state with the field restores ON");
        q.setStateInformation(offState.getData(), (int)offState.getSize()); waitReady(q, 90);
        check(!q.keepProcessingEnabled() && q.getTailLengthSeconds() == 20.25, "E21 a state without the field (old project) restores OFF");
        q.setKeepProcessingEnabled(true);
        juce::String why; const auto preset = p.getRoomPresetText();
        check(q.loadRoomPresetText(preset, why) && waitReady(q, 90) && q.keepProcessingEnabled(), "E21 importing a preset keeps the local ON setting");
        q.setKeepProcessingEnabled(false);
        const bool okLoad = q.loadRoomPresetText(preset, why), okReady = waitReady(q, 90);
        check(okLoad && okReady && !q.keepProcessingEnabled(), "E21 importing a preset does not switch the option ON");
    }
    // bit identity of the audio path: ON vs OFF
    auto render = [](bool on) {
        e21::Rig r(256, 12000); r.p.setKeepProcessingEnabled(on); setP(r.p, "s1x", 4); setP(r.p, "s1y", 3);
        r.silence(40); std::vector<float> L, R; r.click(48000 / 2, L, R); L.insert(L.end(), R.begin(), R.end()); return L;
    };
    const auto a = render(false), b = render(true), c = render(false);
    check(a.size() == b.size() && std::memcmp(a.data(), c.data(), a.size() * sizeof(float)) == 0, "E21 control: two OFF renders are bit-identical");
    check(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0, "E21 impulse render with the option ON is bit-identical to OFF");
    // idle CPU cost of processing silence (what the host starts paying when the option is ON)
    for (const int blk : {256, 512, 1024}) {
        e21::Rig r(blk, 48000); r.silence(2 * 48000 / blk);
        juce::AudioBuffer<float> buf(2, blk); juce::MidiBuffer midi; const int n = 6 * 48000 / blk; std::vector<double> ms;
        for (int i = 0; i < n; ++i) { buf.clear(); const auto t0 = juce::Time::getHighResolutionTicks(); r.p.processBlock(buf, midi); ms.push_back(juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0) * 1000.0); }
        double sum = 0, worst = 0; for (double v : ms) { sum += v; worst = std::max(worst, v); }
        const double mean = sum / n, budget = blk * 1000.0 / 48000.0;
        std::printf("  [E21 cost] silence, block %4d, 48 kHz, balanced tail 48000 rays: mean %.4f ms (%.3f %% of one core), worst %.3f ms (budget %.2f ms)\n", blk, mean, 100.0 * mean / budget, worst, budget);
        check(std::isfinite(mean), "E21 idle cost measured");
    }
    return fails - before;
}
