#pragma once
// V04c: trigger zones -> modulation drivers (In zone / Zone index / Distance to zone) -> Mod Out.
#include <set>
#include "PathShapes.h"
#include "TriggerZones.h"

namespace v04ctest {
inline verbp::TriggerZone box(int id, double x0, double y0, double x1, double y1, double zMin = 0.0, double zMax = 3.0,
                              double edge = 0.0, double hyst = 0.05) {
    verbp::TriggerZone z;
    z.id = id; z.name = "Z" + std::to_string(id); z.polygon = false;
    z.points = {{x0, y0}, {x1, y1}};
    z.zMin = zMin; z.zMax = zMax; z.edge = edge; z.hysteresis = hyst;
    return z;
}
inline verbp::TriggerZone poly(int id, std::vector<std::pair<double, double>> pts, double edge = 0.0, double hyst = 0.05) {
    verbp::TriggerZone z;
    z.id = id; z.name = "P" + std::to_string(id); z.polygon = true; z.points = std::move(pts);
    z.zMin = 0.0; z.zMax = 3.0; z.edge = edge; z.hysteresis = hyst;
    return z;
}
inline verbp::TriggerZoneSet setOf(const std::vector<verbp::TriggerZone>& zs, uint64_t gen = 1) {
    verbp::TriggerZoneSet s;
    s.count = (int)zs.size();
    s.generation = gen;
    for (int i = 0; i < s.count; ++i) s.zones[(size_t)i] = verbp::makeTriggerZoneSnap(zs[(size_t)i]);
    return s;
}
inline verbp::ModulationRoute hostRoute(verbp::ModDriver d, int zoneId, int hostOut, double inMin, double inMax, int object = 0) {
    verbp::ModulationRoute r;
    r.enabled = true; r.object = object; r.driver = d; r.zoneId = zoneId;
    r.target = verbp::ModTarget::None; r.targetObject = -1; r.hostOut = hostOut;
    r.curve.inputMin = inMin; r.curve.inputMax = inMax; r.curve.outputMin = 0.0; r.curve.outputMax = 1.0;
    r.curve.nodeCount = 2; r.curve.nodes[0] = {0.0, 0.0, false}; r.curve.nodes[1] = {1.0, 1.0, false};
    r.attackMs = 0.0; r.releaseMs = 0.0;
    return r;
}
}  // namespace v04ctest

static int testV04cZones() {
    std::printf("=== V04c: trigger zones -> modulation drivers -> Mod Out ===\n");
    const int beforeFails = fails;
    using namespace v04ctest;

    // ---------------------------------------------------------------------------------------------- 1. geometry
    {
        const auto b = verbp::makeTriggerZoneSnap(box(1, 1.0, 1.0, 3.0, 2.0));
        const bool boxOk = verbp::triggerZoneSignedDistance(b, 2.0, 1.5, 1.0) < 0.0 && verbp::triggerZoneSignedDistance(b, 3.5, 1.5, 1.0) > 0.0
            && std::abs(verbp::triggerZoneSignedDistance(b, 2.0, 1.5, 1.0) + 0.5) < 1e-12
            && std::abs(verbp::triggerZoneSignedDistance(b, 3.5, 1.5, 1.0) - 0.5) < 1e-12
            && std::abs(verbp::triggerZoneSignedDistance(b, 3.0, 1.5, 1.0)) < 1e-12          // on an edge
            && std::abs(verbp::triggerZoneSignedDistance(b, 1.0, 1.0, 1.0)) < 1e-12          // on a vertex
            && std::abs(verbp::triggerZoneSignedDistance(b, 4.0, 3.0, 1.0) - std::sqrt(2.0)) < 1e-12  // corner region
            && std::abs(verbp::triggerZoneSignedDistance(b, 2.0, 1.5, 3.4) - 0.4) < 1e-12;   // above zMax
        // Box given by reversed corners is the same zone.
        const auto b2 = verbp::makeTriggerZoneSnap(box(1, 3.0, 2.0, 1.0, 1.0));
        const bool reversedOk = std::abs(verbp::triggerZoneSignedDistance(b2, 2.0, 1.5, 1.0) + 0.5) < 1e-12;
        // Concave U shape: the notch (2, 2.5) is OUTSIDE, the arms are inside.
        const auto u = verbp::makeTriggerZoneSnap(poly(2, {{0, 0}, {4, 0}, {4, 4}, {3, 4}, {3, 1}, {1, 1}, {1, 4}, {0, 4}}));
        const bool concaveOk = verbp::triggerZoneSignedDistance(u, 2.0, 2.5, 1.0) > 0.0
            && std::abs(verbp::triggerZoneSignedDistance(u, 2.0, 2.5, 1.0) - 1.0) < 1e-12
            && verbp::triggerZoneSignedDistance(u, 0.5, 3.0, 1.0) < 0.0
            && verbp::triggerZoneSignedDistance(u, 3.5, 3.0, 1.0) < 0.0
            && verbp::triggerZoneSignedDistance(u, 2.0, 0.5, 1.0) < 0.0;
        std::printf("  geometry: box d(centre)=%.3f d(out)=%.3f d(edge)=%.3g d(corner)=%.6f; U-notch d=%.3f\n",
                    verbp::triggerZoneSignedDistance(b, 2.0, 1.5, 1.0), verbp::triggerZoneSignedDistance(b, 3.5, 1.5, 1.0),
                    verbp::triggerZoneSignedDistance(b, 3.0, 1.5, 1.0), verbp::triggerZoneSignedDistance(b, 4.0, 3.0, 1.0),
                    verbp::triggerZoneSignedDistance(u, 2.0, 2.5, 1.0));
        check(boxOk && reversedOk, "V04c box zone: signed distance inside/outside/edge/vertex/corner/above is exact");
        check(concaveOk, "V04c concave polygon (U): notch is outside, arms inside, exact distances");
        // Validation: self-intersecting bow-tie rejected, simple accepted, degenerate rejected.
        check(!verbp::triggerZonePolygonSimple({{0, 0}, {2, 2}, {2, 0}, {0, 2}}) && verbp::triggerZonePolygonSimple({{0, 0}, {2, 0}, {2, 2}, {0, 2}})
              && !verbp::triggerZonePolygonSimple({{0, 0}, {1, 1}, {2, 2}}) && !verbp::validTriggerZone(poly(3, {{0, 0}, {2, 2}, {2, 0}, {0, 2}})),
              "V04c polygon validation rejects self-intersection and zero area, accepts simple polygons");
        // Soft edge profile.
        const double e = 0.8;
        const double v0 = verbp::triggerZoneSoftValue(0.0, e), vh = verbp::triggerZoneSoftValue(-e / 2, e), v1 = verbp::triggerZoneSoftValue(-e, e);
        const double vq = verbp::triggerZoneSoftValue(-e / 4, e), vout = verbp::triggerZoneSoftValue(0.3, e), vdeep = verbp::triggerZoneSoftValue(-3.0, e);
        std::printf("  soft edge (edge=%.2f m): v(0)=%.6f v(edge/4)=%.6f v(edge/2)=%.6f v(edge)=%.6f v(outside)=%.1f v(deep)=%.1f\n", e, v0, vq, vh, v1, vout, vdeep);
        check(v0 == 0.0 && std::abs(vh - 0.5) < 1e-12 && v1 == 1.0 && std::abs(vq - 0.15625) < 1e-12 && vout == 0.0 && vdeep == 1.0,
              "V04c soft edge: 0 at the border, 0.5 at edge/2, 1 at edge (smoothstep)");
        // Swept segment exactness.
        const auto thin = verbp::makeTriggerZoneSnap(box(4, 2.0, 0.0, 2.02, 4.0));
        const double a[3] = {1.99, 2.0, 1.0}, bb[3] = {2.03, 2.0, 1.0}, c[3] = {1.0, 2.0, 1.0}, dd[3] = {1.9, 2.0, 1.0};
        const double hi0[3] = {1.0, 2.0, 3.5}, hi1[3] = {3.0, 2.0, 3.5};
        check(verbp::triggerZoneSegmentHits(thin, a, bb) && !verbp::triggerZoneSegmentHits(thin, c, dd) && !verbp::triggerZoneSegmentHits(thin, hi0, hi1),
              "V04c swept segment test: crossing hits, short-of-zone and above-zMax miss");
    }

    // ---------------------------------------------------------------------------------------------- 2. tracker
    {
        // Hysteresis: object oscillating +-0.02 m around the border x = 2.0 (zone x in [0, 2]).
        auto countTransitions = [&](double hyst, double amp) {
            auto zs = setOf({box(1, 0.0, 0.0, 2.0, 4.0, 0.0, 3.0, 0.0, hyst)});
            verbp::TriggerZoneTracker t; t.reset();
            double poses[33][4] = {};
            int transitions = 0, prev = -1;
            for (int k = 0; k < 2000; ++k) {
                poses[0][0] = 2.0 + amp * std::sin(2.0 * verb::kPi * k / 37.0);
                poses[0][1] = 2.0; poses[0][2] = 1.0;
                t.update(zs, 1, poses, nullptr, k == 0);
                const int v = (int)t.inZone(0, 1);
                if (prev >= 0 && v != prev) ++transitions;
                prev = v;
            }
            return transitions;
        };
        const int tH = countTransitions(0.05, 0.02), t0 = countTransitions(0.0, 0.02), tBig = countTransitions(0.05, 0.08);
        std::printf("  hysteresis: +-0.02 m oscillation, 2000 periods: transitions=%d with hyst 0.05 m, %d with hyst 0 (chatter); +-0.08 m with hyst 0.05: %d\n", tH, t0, tBig);
        check(tH == 0 && t0 > 100 && tBig > 50, "V04c hysteresis 0.05 m: no chatter for +-0.02 m oscillation around the border (0 transitions after the first period)");

        // Swept test: crossing a 0.1 m wide zone at 10 m/s, 128-sample control period at 48 kHz (step 26.7 mm).
        auto crossing = [&](double width, double speed, double sr, int period, double phase, bool& pointOnlyHit) {
            auto zs = setOf({box(1, 3.0, 0.0, 3.0 + width, 8.0, 0.0, 3.0, 0.0, 0.05)});
            verbp::TriggerZoneTracker t; t.reset();
            double poses[33][4] = {};
            const double step = speed * period / sr;
            int hits = 0; pointOnlyHit = false;
            for (int k = 0; k < (int)(6.0 / step); ++k) {
                poses[0][0] = phase + k * step; poses[0][1] = 4.0; poses[0][2] = 1.2;
                t.update(zs, 1, poses, nullptr, k == 0);
                hits += t.inZone(0, 1) > 0.5;
                pointOnlyHit |= poses[0][0] >= 3.0 && poses[0][0] <= 3.0 + width;
            }
            return hits;
        };
        bool pointHit = false;
        const int h1 = crossing(0.10, 10.0, 48000.0, 128, 0.0, pointHit);
        bool pointHit2 = false;
        const int h2 = crossing(0.10, 100.0, 48000.0, 128, 0.013, pointHit2);  // 0.267 m per period: a point test would miss it
        bool pointHit3 = false;
        const int h3 = crossing(0.02, 10.0, 48000.0, 128, 0.0101, pointHit3);  // 2 cm zone, 26.7 mm per period
        std::printf("  swept: 0.10 m zone @ 10 m/s: %d periods in zone (point test %s); 0.10 m @ 100 m/s: %d (point test %s); 0.02 m @ 10 m/s: %d (point test %s)\n",
                    h1, pointHit ? "hits" : "MISSES", h2, pointHit2 ? "hits" : "MISSES", h3, pointHit3 ? "hits" : "MISSES");
        check(h1 >= 1 && h2 >= 1 && h3 >= 1 && !pointHit2, "V04c swept test: a fast object crossing a thin zone always triggers (also when no control period lands inside)");

        // Zone index with overlapping zones (first in list order wins).
        {
            auto zs = setOf({box(1, 0.0, 0.0, 2.0, 2.0), box(2, 1.0, 1.0, 3.0, 3.0), box(3, 4.0, 4.0, 5.0, 5.0)});
            verbp::TriggerZoneTracker t; t.reset();
            double poses[33][4] = {};
            auto idxAt = [&](double x, double y) { poses[0][0] = x; poses[0][1] = y; poses[0][2] = 1.0; t.update(zs, 1, poses, nullptr, true); return t.zoneIndex(0); };
            const int iOverlap = idxAt(1.5, 1.5), iOnly2 = idxAt(2.5, 2.5), iOnly1 = idxAt(0.5, 0.5), iThird = idxAt(4.5, 4.5), iNone = idxAt(3.5, 0.5);
            auto zsRev = setOf({box(2, 1.0, 1.0, 3.0, 3.0), box(1, 0.0, 0.0, 2.0, 2.0), box(3, 4.0, 4.0, 5.0, 5.0)}, 2);
            poses[0][0] = 1.5; poses[0][1] = 1.5; t.update(zsRev, 1, poses, nullptr, true);
            const int iRev = t.zoneIndex(0);
            std::printf("  zone index: overlap=%d only2=%d only1=%d third=%d none=%d; reordered overlap=%d\n", iOverlap, iOnly2, iOnly1, iThird, iNone, iRev);
            check(iOverlap == 1 && iOnly2 == 2 && iOnly1 == 1 && iThird == 3 && iNone == 0 && iRev == 1,
                  "V04c zone index: 0 outside, first zone in list order wins in overlaps, follows reordering");
        }
        // Seek / loop: no stuck state, re-evaluated from the pose (reset) even inside the hysteresis band rule.
        {
            auto zs = setOf({box(1, 0.0, 0.0, 2.0, 4.0, 0.0, 3.0, 0.0, 0.5)});
            verbp::TriggerZoneTracker t; t.reset();
            double poses[33][4] = {};
            poses[0][0] = 1.0; poses[0][1] = 2.0; poses[0][2] = 1.0;
            t.update(zs, 1, poses, nullptr, true);
            const double inBefore = t.inZone(0, 1);
            poses[0][0] = 2.2;  // inside the 0.5 m hysteresis band: continuous play would keep "in"
            t.update(zs, 1, poses, nullptr, false);
            const double bandContinuous = t.inZone(0, 1);
            poses[0][0] = 5.0;  // seek far away (no sweep across the zone from the stale pose)
            bool jumped[33] = {}; jumped[0] = true;
            t.update(zs, 1, poses, jumped, false);
            const double afterJump = t.inZone(0, 1);
            poses[0][0] = 2.2;  // loop back into the band: re-evaluated from the pose -> outside
            t.update(zs, 1, poses, nullptr, true);
            const double afterLoop = t.inZone(0, 1);
            check(inBefore == 1.0 && bandContinuous == 1.0 && afterJump == 0.0 && afterLoop == 0.0,
                  "V04c seek / loop re-evaluates from the pose: no stuck 'in zone', no sweep from the stale position");
        }
        // Listener as the object, disabled zone, missing zone id.
        {
            auto z1 = box(7, 0.0, 0.0, 2.0, 2.0); auto z2 = box(8, 0.0, 0.0, 2.0, 2.0); z2.enabled = false;
            auto zs = setOf({z1, z2});
            verbp::TriggerZoneTracker t; t.reset();
            double poses[33][4] = {};
            poses[32][0] = 1.0; poses[32][1] = 1.0; poses[32][2] = 1.5;
            poses[0][0] = 5.0; poses[0][1] = 5.0; poses[0][2] = 1.5;
            t.update(zs, 1, poses, nullptr, true);
            check(t.inZone(32, 7) == 1.0 && t.inZone(0, 7) == 0.0 && t.inZone(32, 8) == 0.0 && t.inZone(32, 99) == 0.0
                  && t.distanceTo(32, 99) == verbp::kTriggerZoneFar && std::abs(t.distanceTo(32, 7) + 1.0) < 1e-12 && t.zoneIndex(32) == 1
                  && t.occupancyMask(0, 1) == ((uint64_t)1 << 32),
                  "V04c listener as object; disabled zone and missing zone id read as outside");
        }
    }

    // ---------------------------------------------------------------------------------------------- 3. processor end-to-end
    // Default Room 6 x 8 x 3 m; source 1 on an Endless circle (centre 3,4  R 2 m, 4 s per lap) driven by the transport.
    const auto circlePath = [](const verb::Scene& sc, double periodS) {
        verbp::PathShapeParams sp;
        sp.kind = verbp::PathShapeKind::Circle;
        sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5; sp.radiusX = 2.0; sp.nodes = 32;
        auto mp = verbp::generatePathShapeForRoom(sp, &sc, true).path;
        mp.endless = true;
        mp.endlessPeriodS = periodS;
        return mp;
    };
    auto makeProc = [&](int numSources, std::unique_ptr<VerbProcessor>& out) -> bool {
        out = std::make_unique<VerbProcessor>();
        auto& p = *out;
        p.setPlayConfigDetails(numSources, 2, 48000, 128);
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add(numSources == 2 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::discreteChannels(numSources));
        layout.outputBuses.add(juce::AudioChannelSet::stereo());
        p.setBusesLayout(layout);
        p.prepareToPlay(48000, 128);
        if (!waitReady(p, 90)) return false;
        if (numSources > 2) {
            std::vector<verbp::SourceCfg> srcs((size_t)numSources);
            for (int s = 0; s < numSources; ++s) {
                srcs[(size_t)s].name = "Source " + std::to_string(s + 1);
                srcs[(size_t)s].format = 0; srcs[(size_t)s].channels = 1; srcs[(size_t)s].firstIn = s;
            }
            p.setSources(srcs);
            if (!waitReady(p, 90)) return false;
        }
        return true;
    };
    {
        std::unique_ptr<VerbProcessor> pp;
        check(makeProc(2, pp), "V04c end-to-end processor ready");
        auto& p = *pp;
        check(p.getParameters().size() == 299, "V04c host parameter list unchanged: 283 APVTS + Mod Out 1..16 (299)");
        setP(p, "mode", 1.0);
        const auto sc = p.getDisplayScene();
        const auto mp = circlePath(sc, 4.0);
        check(p.setMovementPath(0, mp), "V04c Endless circle on source 1");
        // Zone A box on the right side of the circle, B and C further round (zone index plateaus).
        const auto zA = box(1, 4.6, 3.0, 5.6, 5.0), zB = box(2, 2.0, 5.4, 4.0, 6.4), zC = box(3, 0.4, 3.0, 1.4, 5.0);
        check(p.setTriggerZones({zA, zB, zC}), "V04c three box zones accepted");
        check(p.setModulationRoutes({hostRoute(verbp::ModDriver::InZone, 1, 1, 0.0, 1.0),
                                      hostRoute(verbp::ModDriver::ZoneIndex, 0, 2, 0.0, 3.0)}),
              "V04c routes: In zone A -> Mod Out 1, Zone index -> Mod Out 2 (0..3)");
        verbp::CompiledPath cp; cp.build(p.getMovementPath(0));
        const auto snapA = verbp::makeTriggerZoneSnap(zA);
        const auto zsAll = setOf({zA, zB, zC});
        auto poseAt = [&](int64_t samp) {
            const double ph = verbp::endlessTransportPhase(samp, 48000.0, mp.endlessPeriodS);
            const double uEff = cp.endlessEffectiveU(0.0, ph);
            double yawDummy = 0.0;
            const auto q = cp.evaluateSingleLapArcFraction(uEff, &yawDummy);
            return std::array<double, 3>{q[0], q[1], std::clamp(q[2], 0.0, 3.0)};
        };
        E12PlayHead head; head.playing = true; head.samples = 0;
        p.setPlayHead(&head);
        juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
        int mismatchA = 0, mismatchIdx = 0, entries = 0, exits = 0, blocksIn = 0;
        std::set<int> plateaus; bool plateauOk = true;
        float prevA = -1.f;
        verbp::TriggerZoneTracker ref; ref.reset();
        std::vector<int> transitionsAt;
        const int blocks = (int)(8.0 * 48000 / 128);  // two laps
        for (int b = 0; b < blocks; ++b) {
            audio.clear();
            stage2Process(p, audio, midi);
            const auto q = poseAt(head.samples);
            double rp[33][4] = {}; rp[0][0] = q[0]; rp[0][1] = q[1]; rp[0][2] = q[2];
            ref.update(zsAll, 1, rp, nullptr, b == 0);
            const float mo1 = p.modOutParameter(0)->getValue(), mo2 = p.modOutParameter(1)->getValue();
            const float expA = (float)ref.inZone(0, 1);
            if (std::abs(mo1 - expA) > 1e-6f) ++mismatchA;
            const float expIdx = (float)ref.zoneIndex(0) / 3.f;
            if (std::abs(mo2 - expIdx) > 1e-5f) ++mismatchIdx;
            const int plateau = (int)std::lround(mo2 * 3.f);
            plateaus.insert(plateau);
            plateauOk &= std::abs(mo2 * 3.f - (float)plateau) < 1e-4f;
            if (prevA >= 0.f && mo1 != prevA) { (mo1 > prevA ? entries : exits)++; transitionsAt.push_back(b); }
            blocksIn += mo1 > 0.5f;
            prevA = mo1;
            head.samples += 128;
        }
        // Geometric expectation of the time inside zone A per lap: fraction of the circle with x >= 4.6.
        int geomIn = 0;
        for (int b = 0; b < blocks; ++b) { const auto q = poseAt((int64_t)b * 128); geomIn += verbp::triggerZoneSignedDistance(snapA, q[0], q[1], q[2]) <= 0.0; }
        std::printf("  end-to-end: Mod Out 1 (In zone A) mismatches vs reference tracker=%d / %d blocks, entries=%d exits=%d, blocks in=%d (geometry %d); Mod Out 2 (zone index) mismatches=%d, plateaus={",
                    mismatchA, blocks, entries, exits, blocksIn, geomIn, mismatchIdx);
        for (int v : plateaus) std::printf(" %d/3", v);
        std::printf(" }\n");
        if (!transitionsAt.empty()) {
            std::printf("  zone A transitions at blocks:");
            for (int t : transitionsAt) std::printf(" %d", t);
            std::printf("\n");
        }
        // Exits are delayed by the 0.05 m hysteresis: at 2*pi*2/4 = 3.14 m/s that is 0.05 / (3.14 * 128 / 48000) = 6 blocks per exit.
        const double stepPerBlock = 2.0 * verb::kPi * 2.0 / 4.0 * 128.0 / 48000.0;
        const int hystBlocks = (int)std::ceil(0.05 / stepPerBlock);
        std::printf("  hysteresis delay per exit: %d blocks (%.1f ms); blocks in - geometry = %d (allowed 2 x %d + 2)\n", hystBlocks, hystBlocks * 128.0 / 48.0, blocksIn - geomIn, hystBlocks);
        check(mismatchA == 0 && entries == 2 && exits == 2 && blocksIn - geomIn >= 0 && blocksIn - geomIn <= 2 * hystBlocks + 2,
              "V04c Mod Out 1 = In zone A: exact 0/1 per control period (0 mismatches vs reference, 2 entries + 2 exits in 2 laps, exit delayed only by the hysteresis)");
        check(mismatchIdx == 0 && plateauOk && plateaus == std::set<int>({0, 1, 2, 3}),
              "V04c Mod Out 2 = Zone index (curve 0..3): plateaus 0, 1/3, 2/3, 1 exactly as expected");

        // Smoothing: attack 40 ms / release 15 ms on the In zone route follows the exponential trajectory.
        {
            auto r = hostRoute(verbp::ModDriver::InZone, 1, 1, 0.0, 1.0);
            r.attackMs = 40.0; r.releaseMs = 15.0;
            check(p.setModulationRoutes({r}), "V04c In zone route with attack 40 ms / release 15 ms");
            double sim = p.modOutParameter(0)->getValue();
            double maxErr = 0.0; bool sawRise = false;
            const double dt = 128.0 / 48000.0;
            for (int b = 0; b < blocks; ++b) {
                audio.clear();
                stage2Process(p, audio, midi);
                const auto q = poseAt(head.samples);
                double rp[33][4] = {}; rp[0][0] = q[0]; rp[0][1] = q[1]; rp[0][2] = q[2];
                ref.update(zsAll, 1, rp, nullptr, false);
                const double goal = ref.inZone(0, 1);
                const double tau = goal > sim ? 0.040 : 0.015;
                sim += (goal - sim) * (1.0 - std::exp(-dt / tau));
                const double got = p.modOutParameter(0)->getValue();
                if (goal > 0.5 && got > 0.05 && got < 0.95) sawRise = true;
                maxErr = std::max(maxErr, std::abs(got - sim));
                head.samples += 128;
            }
            std::printf("  smoothing: max |Mod Out - exponential(attack 40 ms, release 15 ms)| = %.4f\n", maxErr);
            check(sawRise && maxErr < 1e-3, "V04c In zone smoothing follows the route attack/release exactly (exponential, max error < 1e-3)");
        }

        // Transport seek / loop: no stuck state.
        {
            check(p.setModulationRoutes({hostRoute(verbp::ModDriver::InZone, 1, 1, 0.0, 1.0)}), "V04c hard In zone route restored");
            // Play until inside zone A.
            int guard = 0;
            while (p.modOutParameter(0)->getValue() < 0.5f && guard++ < 4000) { audio.clear(); stage2Process(p, audio, midi); head.samples += 128; }
            const bool wasIn = p.modOutParameter(0)->getValue() > 0.5f;
            // Seek to the opposite side of the circle (half a lap later): no sweep across zone A from the stale pose.
            head.samples += (int64_t)(2.0 * 48000);
            audio.clear(); stage2Process(p, audio, midi);
            const auto q = poseAt(head.samples);
            const bool geomIn = verbp::triggerZoneSignedDistance(snapA, q[0], q[1], q[2]) <= 0.0;
            const float afterSeek = p.modOutParameter(0)->getValue();
            head.samples += 128;
            // Loop back to 0 (outside zone A: start point is at angle 0 -> x = 5, inside? use geometry).
            head.samples = 0;
            audio.clear(); stage2Process(p, audio, midi);
            const auto q0 = poseAt(0);
            const bool geomIn0 = verbp::triggerZoneSignedDistance(snapA, q0[0], q0[1], q0[2]) <= 0.0;
            const float afterLoop = p.modOutParameter(0)->getValue();
            std::printf("  seek: before=%d, after seek %.1f (geometry %d), after loop to 0 %.1f (geometry %d)\n", (int)wasIn, afterSeek, (int)geomIn, afterLoop, (int)geomIn0);
            check(wasIn && !geomIn && afterSeek == 0.f && ((afterLoop > 0.5f) == geomIn0),
                  "V04c transport seek / loop re-evaluates zone occupancy from the new pose (no stuck state)");
        }

        // Physical mode: Mod Out neutral 0.5.
        {
            setP(p, "mode", 0.0);
            for (int b = 0; b < 200; ++b) { audio.clear(); stage2Process(p, audio, midi); head.samples += 128; }
            const float mo = p.modOutParameter(0)->getValue();
            check(std::abs(mo - 0.5f) < 1e-6f, "V04c Physical mode: Mod Out stays neutral 0.5 (zones are Creative-only drivers)");
            setP(p, "mode", 1.0);
        }

        // Live occupancy telemetry (editor snapshot, read-only).
        {
            std::array<uint64_t, verbp::kMaxTriggerZones> masks{}; int cnt = 0;
            audio.clear(); stage2Process(p, audio, midi);
            const bool ok = p.webZoneTelemetry(masks, cnt);
            const auto q = poseAt(head.samples - 0);
            int expectedZone = 0;
            for (int zi = 0; zi < 3; ++zi) if (verbp::triggerZoneSignedDistance(zsAll.zones[(size_t)zi], q[0], q[1], q[2]) < -0.06) expectedZone = zi + 1;
            bool match = ok && cnt == 3;
            for (int zi = 0; zi < 3 && match && expectedZone; ++zi) match &= ((masks[(size_t)zi] & 1u) != 0) == (zi + 1 == expectedZone);
            std::printf("  live occupancy: published=%d zones=%d masks=%llx/%llx/%llx (pose zone %d)\n", (int)ok, cnt, (unsigned long long)masks[0], (unsigned long long)masks[1], (unsigned long long)masks[2], expectedZone);
            check(match, "V04c live zone occupancy telemetry published for the editor (no APVTS parameter)");
        }
        p.setPlayHead(nullptr);
    }

    // ---------------------------------------------------------------------------------------------- 4. determinism
    // Endless + Wander through a zone: two offline renders identical; a seek equals the continuous render.
    {
        auto render = [&](int64_t startSample, int blocks, std::vector<float>& mo, std::vector<float>& audioOut) -> bool {
            std::unique_ptr<VerbProcessor> pp;
            if (!makeProc(2, pp)) return false;
            auto& p = *pp;
            setP(p, "mode", 1.0);
            auto mp = circlePath(p.getDisplayScene(), 4.0);
            mp.wander.enabled = true; mp.wander.ampHorizM = 0.4; mp.wander.ampVertM = 0.2; mp.wander.rateHz = 0.9; mp.wander.seed = 4242;
            if (!p.setMovementPath(0, mp)) return false;
            auto zs = box(1, 4.4, 3.0, 5.6, 5.0); zs.hysteresis = 0.0;
            if (!p.setTriggerZones({zs})) return false;
            auto r = hostRoute(verbp::ModDriver::InZone, 1, 1, 0.0, 1.0);
            auto rg = hostRoute(verbp::ModDriver::InZone, 1, 0, 0.0, 1.0);
            rg.target = verbp::ModTarget::SourceGain; rg.curve.outputMin = 0.0; rg.curve.outputMax = -12.0; rg.targetObject = -1; rg.attackMs = 10; rg.releaseMs = 30;
            if (!p.setModulationRoutes({r, rg})) return false;
            E12PlayHead head; head.playing = true; head.samples = startSample;
            p.setPlayHead(&head);
            juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < 128; ++i) audio.setSample(0, i, 0.1f * (float)std::sin((head.samples + i) * 2.0 * verb::kPi * 220.0 / 48000.0));
                audio.clear(1, 0, 128);
                stage2Process(p, audio, midi);
                mo.push_back(p.modOutParameter(0)->getValue());
                for (int i = 0; i < 128; ++i) audioOut.push_back(audio.getSample(0, i));
                head.samples += 128;
            }
            p.setPlayHead(nullptr);
            return true;
        };
        std::vector<float> moA, moB, moSeek, auA, auB, auSeek;
        const int blocks = (int)(8.0 * 48000 / 128);
        const int seekBlock = 777;
        check(render(0, blocks, moA, auA) && render(0, blocks, moB, auB) && render((int64_t)seekBlock * 128, blocks - seekBlock, moSeek, auSeek),
              "V04c Endless + Wander renders complete");
        int sameMo = 0, sameAudio = (int)(auA.size() == auB.size() && std::memcmp(auA.data(), auB.data(), auA.size() * sizeof(float)) == 0);
        for (size_t i = 0; i < moA.size() && i < moB.size(); ++i) sameMo += moA[i] == moB[i];
        int seekMismatch = 0, ones = 0;
        for (size_t i = 0; i < moSeek.size(); ++i) { seekMismatch += moSeek[i] != moA[(size_t)seekBlock + i]; ones += moSeek[i] > 0.5f; }
        std::printf("  determinism: Mod Out equal %d / %zu blocks, audio bit-identical %d; seek@block %d vs continuous: %d mismatching blocks of %zu (%d blocks in zone)\n",
                    sameMo, moA.size(), sameAudio, seekBlock, seekMismatch, moSeek.size(), ones);
        check(sameMo == (int)moA.size() && sameAudio == 1, "V04c Endless + Wander through a zone: two offline renders bit-identical (Mod Out and audio)");
        check(seekMismatch == 0 && ones > 0, "V04c seek equals continuous (Mod Out per block, hysteresis 0)");
    }

    // ---------------------------------------------------------------------------------------------- 5. bit-identity
    // Zones present but no route using them => audio bit-identical (Creative, a moving source, a distance->gain route).
    {
        auto renderAudio = [&](bool withZones, bool creative, std::vector<float>& out) -> bool {
            std::unique_ptr<VerbProcessor> pp;
            if (!makeProc(2, pp)) return false;
            auto& p = *pp;
            setP(p, "mode", creative ? 1.0 : 0.0);
            if (!p.setMovementPath(0, circlePath(p.getDisplayScene(), 4.0))) return false;
            verbp::ModulationRoute r;
            r.object = 0; r.driver = verbp::ModDriver::DistanceToListener; r.target = verbp::ModTarget::SourceLowPass;
            r.curve.inputMin = 1.0; r.curve.inputMax = 4.0; r.curve.outputMin = 20000.0; r.curve.outputMax = 1500.0;
            if (!p.setModulationRoutes({r})) return false;
            if (withZones) {
                std::vector<verbp::TriggerZone> zs;
                for (int i = 0; i < 16; ++i) zs.push_back(box(i + 1, 0.2 + 0.35 * i, 0.5, 0.5 + 0.35 * i, 7.5));
                if (!p.setTriggerZones(zs)) return false;
            }
            E12PlayHead head; head.playing = true; head.samples = 0;
            p.setPlayHead(&head);
            juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
            for (int b = 0; b < 600; ++b) {
                for (int i = 0; i < 128; ++i) audio.setSample(0, i, 0.2f * (float)std::sin((head.samples + i) * 2.0 * verb::kPi * 330.0 / 48000.0));
                audio.clear(1, 0, 128);
                stage2Process(p, audio, midi);
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i) out.push_back(audio.getSample(c, i));
                head.samples += 128;
            }
            p.setPlayHead(nullptr);
            return true;
        };
        for (bool creative : {true, false}) {
            std::vector<float> a, b;
            const bool ok = renderAudio(false, creative, a) && renderAudio(true, creative, b);
            float maxD = 0.f; int diff = 0;
            for (size_t i = 0; i < a.size() && i < b.size(); ++i) { maxD = std::max(maxD, std::abs(a[i] - b[i])); diff += a[i] != b[i]; }
            std::printf("  16 zones without zone routes (%s): max diff %.9g FS, %d differing samples of %zu\n", creative ? "Creative" : "Physical", maxD, diff, a.size());
            check(ok && a.size() == b.size() && diff == 0, creative ? "V04c 16 zones present, no route uses them: Creative audio bit-identical (0 FS)"
                                                                    : "V04c 16 zones present, no route uses them: Physical audio bit-identical (0 FS)");
        }
    }

    // ---------------------------------------------------------------------------------------------- 6. CPU / allocations
    {
        auto bench = [&](bool zones, double& meanUs, double& p99Us, int64_t& allocs) -> bool {
            std::unique_ptr<VerbProcessor> pp;
            if (!makeProc(8, pp)) return false;
            auto& p = *pp;
            setP(p, "mode", 1.0);
            const auto sc = p.getDisplayScene();
            for (int s = 0; s < 8; ++s) {
                verbp::PathShapeParams sp;
                sp.kind = verbp::PathShapeKind::Circle; sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5; sp.radiusX = 2.4; sp.nodes = 16;
                sp.startPhaseDeg = 45.0 * s;
                auto mp = verbp::generatePathShapeForRoom(sp, &sc, true).path;
                mp.endless = true; mp.endlessPeriodS = 3.0 + 0.3 * s;
                if (!p.setMovementPath(s, mp)) return false;
            }
            std::vector<verbp::ModulationRoute> routes;
            if (zones) {
                std::vector<verbp::TriggerZone> zs;
                for (int i = 0; i < 16; ++i) {
                    if (i % 2) {
                        const double cx = 1.0 + (i % 4) * 1.3, cy = 1.0 + (i / 4) * 1.8;
                        std::vector<std::pair<double, double>> pts;
                        for (int k = 0; k < 12; ++k) pts.emplace_back(cx + 0.6 * std::cos(k * verb::kPi / 6), cy + (k % 2 ? 0.35 : 0.7) * std::sin(k * verb::kPi / 6));
                        zs.push_back(poly(i + 1, pts, i % 3 ? 0.0 : 0.3));
                    } else zs.push_back(box(i + 1, 0.3 + 0.35 * i, 0.5, 0.6 + 0.35 * i, 7.5));
                }
                if (!p.setTriggerZones(zs)) return false;
            }
            for (int s = 0; s < 8; ++s) {
                const verbp::ModDriver d = !zones ? verbp::ModDriver::DistanceToListener
                    : (s % 3 == 0 ? verbp::ModDriver::InZone : s % 3 == 1 ? verbp::ModDriver::ZoneIndex : verbp::ModDriver::DistanceToZone);
                auto r = hostRoute(d, d == verbp::ModDriver::ZoneIndex || !zones ? 0 : 1 + 2 * s, s + 1, 0.0, d == verbp::ModDriver::ZoneIndex ? 16.0 : 3.0, s);
                r.attackMs = 15; r.releaseMs = 80;
                routes.push_back(r);
            }
            if (!p.setModulationRoutes(routes)) return false;
            E12PlayHead head; head.playing = true; head.samples = 0;
            p.setPlayHead(&head);
            juce::AudioBuffer<float> audio(8, 128); juce::MidiBuffer midi;
            for (int w = 0; w < 50; ++w) { audio.clear(); stage2Process(p, audio, midi); head.samples += 128; }
            std::vector<double> us; us.reserve(3000);
            const auto a0 = stage2AudioAllocations;
            for (int b = 0; b < 3000; ++b) {
                for (int s = 0; s < 8; ++s) for (int i = 0; i < 128; ++i) audio.setSample(s, i, 0.1f * (float)std::sin((head.samples + i) * 0.03 * (s + 1)));
                const auto t0 = std::chrono::steady_clock::now();
                stage2Process(p, audio, midi);
                us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
                head.samples += 128;
            }
            allocs = stage2AudioAllocations - a0;
            double sum = 0.0; for (double v : us) sum += v;
            meanUs = sum / (double)us.size();
            std::sort(us.begin(), us.end());
            p99Us = us[us.size() * 99 / 100];
            p.setPlayHead(nullptr);
            return true;
        };
        double m0 = 0, q0 = 0, m1 = 0, q1 = 0; int64_t a0 = 0, a1 = 0;
        // Interleave twice to reduce drift; keep the better (lower) mean of each.
        double mm0 = 1e9, qq0 = 1e9, mm1 = 1e9, qq1 = 1e9; int64_t aa0 = 0, aa1 = 0; bool ok = true;
        for (int rep = 0; rep < 2; ++rep) {
            ok &= bench(false, m0, q0, a0); ok &= bench(true, m1, q1, a1);
            mm0 = std::min(mm0, m0); qq0 = std::min(qq0, q0); mm1 = std::min(mm1, m1); qq1 = std::min(qq1, q1); aa0 += a0; aa1 += a1;
        }
        std::printf("  CPU (8 sources Endless, 8 routes -> Mod Out, block 128 @ 48 kHz, budget 2666.7 us): no zones mean %.2f us p99 %.2f us; 16 zones (8 box + 8 polygon x 12 vertices, zone drivers) mean %.2f us p99 %.2f us; delta mean %+.2f us (%+.2f %% of the block budget); allocations %lld / %lld\n",
                    mm0, qq0, mm1, qq1, mm1 - mm0, 100.0 * (mm1 - mm0) / 2666.7, (long long)aa0, (long long)aa1);
        check(ok && aa1 == 0 && aa0 == 0, "V04c 16 zones x 8 routes x 8 sources: 0 allocations on the audio thread");
        check(ok && (mm1 - mm0) < 0.05 * 2666.7, "V04c zone tracking costs < 5 % of the 128-sample block budget");
    }

    // ---------------------------------------------------------------------------------------------- 7. state / preset / impulse
    {
        std::unique_ptr<VerbProcessor> pp;
        check(makeProc(2, pp), "V04c state processor ready");
        auto& p = *pp;
        juce::MemoryBlock none;
        p.getStateInformation(none);
        auto xmlNone = VerbProcessor::getXmlFromBinary(none.getData(), (int)none.getSize());
        check(xmlNone && xmlNone->getChildByName("TriggerZones") == nullptr, "V04c project state without zones has no <TriggerZones> child (old layout)");
        auto zA = box(5, 1.0, 1.0, 2.5, 3.0, 0.2, 2.4, 0.3, 0.1); zA.name = "Parquet"; zA.colour = "#22c55e";
        auto zB = poly(9, {{3.0, 1.0}, {5.0, 1.0}, {5.0, 3.0}, {4.0, 2.0}, {3.0, 3.0}}); zB.name = "Tile"; zB.enabled = false; zB.visible = false;
        check(p.setTriggerZones({zA, zB}), "V04c set two zones (box + concave polygon)");
        auto r = hostRoute(verbp::ModDriver::InZone, 5, 3, 0.0, 1.0);
        check(p.setModulationRoutes({r}), "V04c route referencing zone 5");
        juce::MemoryBlock st;
        p.getStateInformation(st);
        const auto preset = p.getRoomPresetText();
        std::unique_ptr<VerbProcessor> qq;
        check(makeProc(2, qq), "V04c reload processor ready");
        auto& q = *qq;
        q.setStateInformation(st.getData(), (int)st.getSize());
        check(waitReady(q, 90) && q.getTriggerZones() == std::vector<verbp::TriggerZone>({zA, zB}) && q.getModulationRoutes() == std::vector<verbp::ModulationRoute>({r}),
              "V04c project state round-trip preserves zones (all fields) and the route zoneId");
        const auto ir = h2test::captureImpulse(q, 2, 0, 120, 128);
        const double eIr = h2test::channelEnergy(ir[0]) + h2test::channelEnergy(ir[1]);
        check(std::isfinite(eIr) && eIr > 1e-6, "V04c impulse after reloading a project with zones (Default Room)");
        juce::String err;
        std::unique_ptr<VerbProcessor> rr;
        check(makeProc(2, rr), "V04c preset processor ready");
        check(rr->loadRoomPresetText(preset, err) && waitReady(*rr, 90) && rr->getTriggerZones() == std::vector<verbp::TriggerZone>({zA, zB}),
              "V04c .verbroom export -> import preserves zones");
        auto xmlPreset = juce::parseXML(preset);
        if (auto* vs = xmlPreset ? xmlPreset->getChildByName("VerbState") : nullptr)
            if (auto* tz = vs->getChildByName("TriggerZones")) vs->removeChildElement(tz, true);
        check(xmlPreset && rr->loadRoomPresetText(xmlPreset->toString(), err) && waitReady(*rr, 90) && rr->getTriggerZones().empty(),
              "V04c .verbroom without <TriggerZones> loads with no zones (old behaviour)");
        // A corrupt zone (self-intersecting) in a project is dropped, the rest loads.
        auto xmlBad = VerbProcessor::getXmlFromBinary(st.getData(), (int)st.getSize());
        if (auto* tz = xmlBad ? xmlBad->getChildByName("TriggerZones") : nullptr) {
            auto* bad = tz->createNewChildElement("Zone");
            bad->setAttribute("id", 77); bad->setAttribute("kind", "polygon"); bad->setAttribute("zMin", "0"); bad->setAttribute("zMax", "3");
            for (auto xy : {std::pair<double, double>{0, 0}, {2, 2}, {2, 0}, {0, 2}}) { auto* pt = bad->createNewChildElement("P"); pt->setAttribute("x", xy.first); pt->setAttribute("y", xy.second); }
        }
        juce::MemoryBlock badState;
        if (xmlBad) VerbProcessor::copyXmlToBinary(*xmlBad, badState);
        std::unique_ptr<VerbProcessor> bb;
        check(makeProc(2, bb), "V04c corrupt-zone processor ready");
        bb->setStateInformation(badState.getData(), (int)badState.getSize());
        check(waitReady(*bb, 90) && bb->getTriggerZones().size() == 2, "V04c a self-intersecting zone in a project is dropped on load, valid zones kept");
        // Legacy fixture loads with no zones.
        const auto legacyStateFile = juce::File::getCurrentWorkingDirectory().getChildFile("plugin/fixtures/stage2/legacy.state");
        if (legacyStateFile.existsAsFile()) {
            juce::MemoryBlock legData; legacyStateFile.loadFileAsData(legData);
            std::unique_ptr<VerbProcessor> lp;
            check(makeProc(2, lp), "V04c legacy processor ready");
            lp->setStateInformation(legData.getData(), (int)legData.getSize());
            check(waitReady(*lp, 90) && lp->getTriggerZones().empty(), "V04c legacy.state loads unchanged (no zones)");
        }
        // VSL preset + zones: save, reload, impulse.
        const auto vslFile = juce::File::getCurrentWorkingDirectory().getChildFile("VSL.verbroom");
        if (vslFile.existsAsFile()) {
            auto vp = std::make_unique<VerbProcessor>();
            vp->setPlayConfigDetails(64, 2, 48000, 128);
            juce::AudioProcessor::BusesLayout vl;
            vl.inputBuses.add(juce::AudioChannelSet::discreteChannels(64));
            vl.outputBuses.add(juce::AudioChannelSet::stereo());
            vp->setBusesLayout(vl);
            vp->prepareToPlay(48000, 128);
            juce::String vErr;
            check(vp->loadRoomPresetText(vslFile.loadFileAsString(), vErr) && waitReady(*vp, 120), "V04c VSL preset loads");
            const auto vsc = vp->getDisplayScene();
            const auto lx = vsc.listenerPos.x, ly = vsc.listenerPos.y;
            check(vp->setTriggerZones({box(1, lx - 3.0, ly - 2.0, lx - 1.0, ly + 2.0), box(2, lx + 1.0, ly - 2.0, lx + 3.0, ly + 2.0)})
                  && vp->setModulationRoutes({hostRoute(verbp::ModDriver::ZoneIndex, 0, 1, 0.0, 2.0)}), "V04c VSL preset accepts zones and a zone route");
            juce::MemoryBlock vs; vp->getStateInformation(vs);
            auto vr = std::make_unique<VerbProcessor>();
            vr->setPlayConfigDetails(64, 2, 48000, 128);
            vr->setBusesLayout(vl);
            vr->prepareToPlay(48000, 128);
            vr->setStateInformation(vs.getData(), (int)vs.getSize());
            check(waitReady(*vr, 120) && vr->getTriggerZones().size() == 2, "V04c VSL project with zones reloads");
            const auto irV = h2test::captureImpulse(*vr, 64, 0, 120, 128);
            const double eV = h2test::channelEnergy(irV[0]) + h2test::channelEnergy(irV[1]);
            std::printf("  impulse energy after reload: Default Room %.6g, VSL %.6g\n", eIr, eV);
            check(std::isfinite(eV) && eV > 1e-6, "V04c impulse after reloading the VSL preset project with zones");
        }
        // Classic editor: loads a project with zones and opens without crashing.
        {
            auto editor = std::make_unique<VerbEditor>(q);
            editor->setSize(900, 600);
            editor->showPage(0, 0);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
            check(q.getTriggerZones().size() == 2, "V04c classic editor opens a project with zones (no crash, zones kept)");
        }
    }

    return fails == beforeFails ? 0 : 1;
}
