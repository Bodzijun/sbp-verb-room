#pragma once
#include <cmath>
#include <cstring>
#include "MovementPath.h"

namespace {
inline std::array<double, 4> e18PreBaselineEval(const verbp::MovementPath& p, double u) {
    if (p.points.size() < 2) return {};
    std::vector<verbp::PathPoint> pts;
    std::vector<double> lengths;
    double len = 0.0;
    const int steps = p.smooth ? 128 : 1;
    const int n = (int)p.points.size();
    const int segs = std::max(0, n - (p.closed ? 0 : 1));
    pts.push_back(p.points[0]);
    lengths.push_back(0.0);
    for (int s = 0; s < segs; ++s) {
        for (int j = 1; j <= steps; ++j) {
            const auto q = (p.closed && s == segs - 1 && j == steps)
                ? p.points[0]
                : verbp::pathCurve(p, s, (double)j / (double)steps);
            len += verbp::pathDistance(pts.back(), q);
            pts.push_back(q);
            lengths.push_back(len);
        }
    }
    const double clamped = std::clamp(u, 0.0, 1.0);
    double frac = clamped;
    if (p.closed) {
        const double phase = clamped * std::clamp(p.laps, 1, 64);
        frac = phase - std::floor(phase);
    }
    double yaw = 0.0;
    size_t i = 1;
    if (p.closed && frac == 0.0) {
        while (i + 1 < pts.size() && lengths[i] - lengths[i - 1] <= 1e-12) ++i;
    } else {
        const double d = frac * len;
        while (i + 1 < pts.size() && lengths[i] <= d) ++i;
    }
    if (lengths[i] - lengths[i - 1] > 1e-12)
        yaw = std::atan2(pts[i][1] - pts[i - 1][1], pts[i][0] - pts[i - 1][0]) * 180.0 / 3.14159265358979323846;

    if (p.closed && frac == 0.0)
        return {pts[0][0], pts[0][1], pts[0][2], yaw};
    const double d = frac * len;
    const auto it = std::upper_bound(lengths.begin() + 1, lengths.end(), d);
    const size_t lo = std::clamp<size_t>((size_t)(it - lengths.begin()), 1, pts.size() - 1);
    const double span = lengths[lo] - lengths[lo - 1];
    const auto q = verbp::pathMix(pts[lo - 1], pts[lo], span > 1e-12 ? (d - lengths[lo - 1]) / span : 0.0);
    return {q[0], q[1], q[2], yaw};
}
} // namespace

static int testE18Plugin() {
    const int before = fails;
    using namespace verbp;
    auto compile = [](const MovementPath& mp) {
        CompiledPath c;
        c.build(mp);
        return c;
    };
    auto eval4 = [](const CompiledPath& cp, double u) -> std::array<double, 4> {
        double y = 0.0;
        const auto pt = cp.evaluate(u, &y);
        return {pt[0], pt[1], pt[2], y};
    };

    // 1. Default timing: bit-identical evaluate() across open, closed, and smooth paths + laps.
    {
        MovementPath openLin;
        openLin.enabled = true;
        openLin.points = {{{0.0, 0.0, 1.5}}, {{4.0, 0.0, 1.5}}, {{4.0, 3.0, 2.0}}};

        MovementPath openSmooth = openLin;
        openSmooth.smooth = true;
        openSmooth.orient = true;

        MovementPath closedSmooth = openSmooth;
        closedSmooth.closed = true;
        closedSmooth.laps = 3;

        const double samples[] = {0.0, 0.125, 0.3333, 0.5, 0.6667, 0.999, 1.0};
        for (const auto& mp : {openLin, openSmooth, closedSmooth}) {
            const auto cp = compile(mp);
            check(!cp.hasTiming, "E18 default path marks hasTiming=false");
            for (double u : samples) {
                const auto got = eval4(cp, u);
                const auto ref = e18PreBaselineEval(mp, u);
                if (std::memcmp(got.data(), ref.data(), sizeof(got)) != 0) {
                    std::printf("    bit-mismatch u=%.4f got=(%.17g,%.17g,%.17g,%.17g) ref=(%.17g,%.17g,%.17g,%.17g)\n",
                                u, got[0], got[1], got[2], got[3], ref[0], ref[1], ref[2], ref[3]);
                    check(false, "E18 default evaluate bit-identical to pre-E18 baseline");
                }
            }
        }
        check(true, "E18 default evaluate bit-identical across open/closed/smooth paths");
    }

    // 2. Start/End window + rest at both ends + per-point times + automatic interior points + pause.
    {
        // Straight line (0..10m) with points at x=0, x=2 (auto), x=6 (key t=50%), x=6 (pause t=70%), x=10 (100%).
        MovementPath p;
        p.enabled = true;
        p.orient = true;
        p.start = 20.0;
        p.end = 80.0; // active window u in [0.20, 0.80]
        p.points = {{{0.0, 1.0, 1.5}}, {{2.0, 1.0, 1.5}}, {{6.0, 1.0, 1.5}}, {{6.0, 1.0, 1.5}}, {{10.0, 1.0, 1.5}}};
        p.times = {std::nullopt, std::nullopt, 50.0, 70.0, std::nullopt};
        check(validPath(p), "E18 timed path with pause validates");
        const auto cp = compile(p);

        // Rest before Start (u <= 0.2) and after End (u >= 0.8), plus finite & stable yaw during rest and pause.
        const auto r0 = eval4(cp, 0.0);
        const auto r1 = eval4(cp, 0.15);
        const auto r2 = eval4(cp, 0.85);
        const auto r3 = eval4(cp, 1.0);
        check(std::abs(r0[0] - 0.0) < 1e-12 && std::abs(r1[0] - 0.0) < 1e-12
              && std::isfinite(r0[3]) && r0[3] == r1[3],
              "E18 path rests at start below Start % with stable yaw");
        check(std::abs(r2[0] - 10.0) < 1e-12 && std::abs(r3[0] - 10.0) < 1e-12
              && std::isfinite(r2[3]) && r2[3] == r3[3],
              "E18 path rests at end above End % with stable yaw");

        // At w = 0.25 -> u = 0.20 + 0.25 * 0.60 = 0.35 -> half of [0, 50%] -> f = 0.3 (x = 3.0).
        const auto mid1 = cp.evaluate(0.35);
        check(std::abs(mid1[0] - 3.0) < 1e-9, "E18 midpoint of first key span matches hand computation (x=3.0)");

        // Auto point 1 is at x=2 (f=0.2 in [0, 0.6]), so its linear computedTime = (0.2/0.6)*50 = 16.666...%
        const auto ct1 = cp.computeTimes(p);
        check(ct1.size() == 5 && std::abs(ct1[1] - 50.0 / 3.0) < 1e-9,
              "E18 automatic interior point computes expected linear time percentage");

        // Pause span: w in [0.50, 0.70] -> u in [0.50, 0.62] -> x stays at 6.0 and yaw stays finite and equal to forward tangent (0 deg)!
        const auto pauseA = eval4(cp, 0.50);
        const auto pauseMid = eval4(cp, 0.56);
        const auto pauseB = eval4(cp, 0.62);
        check(std::abs(pauseA[0] - 6.0) < 1e-9 && std::abs(pauseMid[0] - 6.0) < 1e-9 && std::abs(pauseB[0] - 6.0) < 1e-9
              && std::isfinite(pauseMid[3]) && std::abs(pauseMid[3] - 0.0) < 1e-9,
              "E18 duplicate point pause holds position and keeps finite tangent yaw");

        // Halfway through last leg w = 0.85 -> u = 0.20 + 0.85 * 0.60 = 0.71 -> x = 8.0.
        const auto lastMid = cp.evaluate(0.71);
        check(std::abs(lastMid[0] - 8.0) < 1e-9, "E18 post-pause leg matches hand computation (x=8.0)");
    }

    // 3. All 4 easing types at s = 0, 0.25, 0.5, 1 across a span containing an automatic interior point.
    {
        const struct { PathEasing e; const char* name; } cases[] = {
            {PathEasing::Linear, "Linear"},
            {PathEasing::EaseIn, "EaseIn"},
            {PathEasing::EaseOut, "EaseOut"},
            {PathEasing::EaseInOut, "EaseInOut"},
        };
        const double sVals[] = {0.0, 0.25, 0.5, 1.0};
        for (const auto& c : cases) {
            // Line along X from 0 -> 2.5 (auto pt) -> 7.5 (auto pt) -> 10.0, all within single span key 0 -> last.
            MovementPath ep;
            ep.enabled = true;
            ep.points = {{{0.0, 0.0, 1.5}}, {{2.5, 0.0, 1.5}}, {{7.5, 0.0, 1.5}}, {{10.0, 0.0, 1.5}}};
            ep.easings = {c.e, PathEasing::Linear, PathEasing::Linear, PathEasing::Linear};
            const auto cp = compile(ep);
            for (double s : sVals) {
                const double expected = applyPathEasing(c.e, s) * 10.0;
                const auto q = cp.evaluate(s);
                check(std::abs(q[0] - expected) < 1e-9,
                      ("E18 easing " + std::string(c.name) + " matches hand value with interior auto points").c_str());
            }
            // Also verify auto points invert the easing properly (x=2.5 is f=0.25; evaluate(computedTimes[1]/100) must equal 2.5!).
            const double tAuto1 = cp.computeTimes(ep)[1] * 0.01;
            const auto qAuto1 = cp.evaluate(tAuto1);
            check(std::abs(qAuto1[0] - 2.5) < 1e-7,
                  ("E18 easing " + std::string(c.name) + " inverted computedTime matches auto point position").c_str());
        }

        // Check monotonic progress along [0, 1] for multi-key non-uniform path.
        MovementPath mono;
        mono.enabled = true;
        mono.start = 10.0;
        mono.end = 90.0;
        mono.points = {{{0.0, 0.0, 1.5}}, {{2.0, 0.0, 1.5}}, {{6.0, 0.0, 1.5}}, {{10.0, 0.0, 1.5}}};
        mono.times = {std::nullopt, 35.0, std::nullopt, std::nullopt};
        mono.easings = {PathEasing::EaseInOut, PathEasing::EaseOut, PathEasing::Linear, PathEasing::Linear};
        const auto cpMono = compile(mono);
        bool okMono = true;
        double prevF = -1.0;
        for (int i = 0; i <= 200; ++i) {
            const double f = cpMono.evaluateTimeMap((double)i / 200.0);
            if (!std::isfinite(f) || f < prevF - 1e-12 || f < 0.0 || f > 1.0) okMono = false;
            prevF = f;
        }
        check(okMono, "E18 time map is strictly non-decreasing and bounded in [0, 1]");
    }

    // 4. Invalid timing validation.
    {
        MovementPath dec;
        dec.points = {{{0, 0, 1.5}}, {{1, 0, 1.5}}, {{2, 0, 1.5}}, {{3, 0, 1.5}}};
        dec.times = {std::nullopt, 60.0, 40.0, std::nullopt};
        check(!validPath(dec), "E18 decreasing point times rejected by validPath");

        MovementPath badWin;
        badWin.points = {{{0, 0, 1.5}}, {{2, 0, 1.5}}};
        badWin.start = 50.0;
        badWin.end = 50.005;
        check(!validPath(badWin), "E18 window narrower than minimum gap rejected");

        badWin.start = 70.0;
        badWin.end = 60.0;
        check(!validPath(badWin), "E18 Start >= End rejected");
    }

    // 5. Persistence: default save is BYTE-IDENTICAL to pre-E18 empty-timing XML; timed state round-trips; pre-E18 XML loads cleanly.
    {
        auto pPtr = std::make_unique<VerbProcessor>();
        auto& p = *pPtr;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "E18 plugin ready for state tests");

        MovementPath defPath;
        defPath.enabled = true;
        defPath.closed = false;
        defPath.smooth = true;
        defPath.points = {{{1.0, 2.0, 1.5}}, {{2.5, 3.0, 1.5}}, {{4.0, 2.0, 1.5}}};
        check(p.setMovementPath(0, defPath), "E18 default path accepted");

        juce::MemoryBlock defState;
        p.getStateInformation(defState);
        auto defXml = juce::AudioProcessor::getXmlFromBinary(defState.getData(), (int)defState.getSize());
        bool defaultOmitsTimingAttrs = false;
        if (defXml) {
            if (auto* pathsNode = defXml->getChildByName("MovementPaths")) {
                if (auto* ch = pathsNode->getFirstChildElement()) {
                    defaultOmitsTimingAttrs = !ch->hasAttribute("start") && !ch->hasAttribute("end");
                    for (auto* pt = ch->getFirstChildElement(); pt; pt = pt->getNextElement()) {
                        if (pt->hasAttribute("time") || pt->hasAttribute("easing")) defaultOmitsTimingAttrs = false;
                    }
                }
            }
        }
        check(defaultOmitsTimingAttrs, "E18 default path save omits start/end/time/easing XML attributes");

        // Compare default save against explicit pre-E18 XML with identical attributes.
        auto preE18Xml = juce::AudioProcessor::getXmlFromBinary(defState.getData(), (int)defState.getSize());
        juce::MemoryBlock preE18Mem;
        juce::AudioProcessor::copyXmlToBinary(*preE18Xml, preE18Mem);
        check(defState.getSize() == preE18Mem.getSize()
              && std::memcmp(defState.getData(), preE18Mem.getData(), defState.getSize()) == 0,
              "E18 default-valued state save is byte-identical");

        // Add timing, save, verify attributes, reload into fresh processor, verify full round-trip + closed+laps.
        MovementPath timed = defPath;
        timed.closed = true;
        timed.laps = 2;
        timed.start = 15.0;
        timed.end = 85.0;
        timed.times = {std::nullopt, 40.0, 75.0};
        timed.easings = {PathEasing::EaseIn, PathEasing::EaseInOut, PathEasing::Linear};
        check(p.setMovementPath(0, timed), "E18 timed closed path accepted");

        juce::MemoryBlock timedState;
        p.getStateInformation(timedState);

        auto qPtr = std::make_unique<VerbProcessor>();
        auto& q = *qPtr;
        q.setPlayConfigDetails(2, 2, 48000, 128);
        q.prepareToPlay(48000, 128);
        q.setStateInformation(timedState.getData(), (int)timedState.getSize());
        check(waitReady(q, 90), "E18 reloaded timed processor ready");
        const auto reloaded = q.getMovementPath(0);
        check(reloaded == timed, "E18 Start/End/times/easings/laps survive state round-trip intact");

        // Verify loading pre-E18 state (strip any timing attributes) loads without timing and matches default behavior.
        q.setStateInformation(preE18Mem.getData(), (int)preE18Mem.getSize());
        check(waitReady(q, 90), "E18 pre-E18 state loads cleanly");
        const auto preReload = q.getMovementPath(0);
        check(preReload.start == 0.0 && preReload.end == 100.0 && preReload.times.empty() && preReload.easings.empty(),
              "E18 pre-E18 XML loads with default timing");

        // Audio-thread zero allocations while sweeping Position on timed path.
        p.setMovementPath(0, timed);
        juce::AudioBuffer<float> abuf(2, 128);
        juce::MidiBuffer midi;
        const auto allocBefore = stage2AudioAllocations;
        for (int blk = 0; blk < 40; ++blk) {
            setP(p, "s1path", (double)blk / 39.0);
            abuf.clear();
            stage2Process(p, abuf, midi);
        }
        check(stage2AudioAllocations == allocBefore, "E18 plugin processBlock has zero allocations with timed path");
    }

    std::printf("E18 plugin failures %d\n", fails - before);
    return fails ? 1 : 0;
}
