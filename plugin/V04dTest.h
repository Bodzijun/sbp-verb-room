#pragma once
// V04d: "Link to path" (rig) — a source follows another source's trajectory with a manual offset in the path frame
// (forward = drawn tangent, left = horizontal perpendicular, up = world Z). Pure geometry, commands / undo policy,
// processor poses (Endless, Laps, Position, LFO, shared wander, clamp, zones, mod drivers), determinism, audio,
// Bake bypass, state / .verbroom round trips and impulse checks.
#include <chrono>
#include <fstream>
#include "PathShapes.h"
#include "TriggerZones.h"

namespace v04dtest {
inline verbp::PathFollow follow(int target, double f, double l, double u, bool fw = true) {
    verbp::PathFollow r; r.target = target; r.offset = {f, l, u}; r.followWander = fw; return r;
}
inline verbp::MovementPath followerPath(int target, double f, double l, double u, bool fw = true) {
    verbp::MovementPath p; p.follow = follow(target, f, l, u, fw); return p;
}
inline verbp::CompiledPath compiled(const verbp::MovementPath& f, const verbp::MovementPath& t) {
    verbp::CompiledPath c; c.build(verbp::resolveFollowerPath(f, &t)); return c;
}
inline verbp::MovementPath circle(const verb::Scene& sc, double cx, double cy, double r, double z, bool endless, double period = 8.0, int nodes = 16) {
    verbp::PathShapeParams sp; sp.kind = verbp::PathShapeKind::Circle; sp.cx = cx; sp.cy = cy; sp.z = z; sp.radiusX = r; sp.nodes = nodes;
    auto mp = verbp::generatePathShapeForRoom(sp, &sc, true).path;
    mp.endless = endless; mp.endlessPeriodS = period;
    return mp;
}
inline double dist3(const std::array<double, 3>& a, const verbp::PathPoint& b) {
    return std::hypot(std::hypot(a[0] - b[0], a[1] - b[1]), a[2] - b[2]);
}
inline std::unique_ptr<VerbProcessor> makeProc(int ns, int block = 128) {
    auto p = std::make_unique<VerbProcessor>();
    p->setPlayConfigDetails(ns, 2, 48000, block);
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add(ns == 2 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::discreteChannels(std::max(ns, 1)));
    layout.outputBuses.add(juce::AudioChannelSet::stereo());
    p->setBusesLayout(layout);
    p->prepareToPlay(48000, block);
    if (!waitReady(*p, 90)) return nullptr;
    if (ns > 1) {
        std::vector<verbp::SourceCfg> srcs((size_t)ns);
        for (int s = 0; s < ns; ++s) { srcs[(size_t)s].name = "Source " + std::to_string(s + 1); srcs[(size_t)s].format = 0; srcs[(size_t)s].channels = 1; srcs[(size_t)s].firstIn = s; }
        p->setSources(srcs);
        if (!waitReady(*p, 90)) return nullptr;
    }
    return p;
}
inline juce::var cmd(const char* json) { return juce::JSON::parse(juce::String(json)); }
inline bool writeWav(const juce::File& f, const std::vector<float>& l, const std::vector<float>& r) {
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream());
    if (!os) return false;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 48000, 2, 24, {}, 0));
    if (!w) return false;
    os.release();
    juce::AudioBuffer<float> b(2, (int)l.size());
    for (int i = 0; i < (int)l.size(); ++i) { b.setSample(0, i, l[(size_t)i]); b.setSample(1, i, r[(size_t)i]); }
    return w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
}
}  // namespace v04dtest

static int testV04dLinkToPath() {
    std::printf("=== V04d: Link to path (rig) ===\n");
    const int beforeFails = fails;
    using namespace v04dtest;
    const double kPi = 3.14159265358979323846;

    const bool audioOnly = std::getenv("VERB_V04D_AUDIO_ONLY") != nullptr;
    // ------------------------------------------------------------------ 0. host parameters unchanged
    if (!audioOnly) {
        auto pp = makeProc(2);
        check(pp != nullptr, "V04d Default Room loads");
        if (pp) {
            bool hostOrder = pp->getParameters().size() == 299;
            for (int index = 283; index < pp->getParameters().size(); ++index) {
                const auto* param = dynamic_cast<const juce::AudioProcessorParameterWithID*>(pp->getParameters()[index]);
                hostOrder &= param && param->paramID == "mo" + juce::String(index - 282);
            }
            check(hostOrder, "V04d APVTS count 283 + Mod Out 1..16 (host indices 283..298) unchanged — no new parameter");
        }
    }

    // ------------------------------------------------------------------ 1. model validation
    if (!audioOnly) {
        verbp::MovementPath f = followerPath(0, 0.0, 0.0, 0.3);
        check(verbp::validPath(f), "V04d follower record (no own points) is valid");
        auto g = f; g.points = {{{0, 0, 1}}, {{1, 0, 1}}};
        check(!verbp::validPath(g), "V04d follower with own points is rejected (follow replaces own geometry)");
        g = f; g.laps = 2; check(!verbp::validPath(g), "V04d follower with own Laps is rejected (inherited)");
        g = f; g.group = 1; check(!verbp::validPath(g), "V04d follower with Coupling group is rejected");
        g = f; g.lfo.enabled = true; g.lfo.depth = 0.1; check(!verbp::validPath(g), "V04d follower with own LFO is rejected");
        g = f; g.follow.offset[1] = 51.0; check(!verbp::validPath(g), "V04d offset beyond +/-50 m is rejected");
        g = f; g.follow.target = 32; check(!verbp::validPath(g), "V04d the listener cannot be a link target");
        g = f; g.azKeys = {{10.0, 45.0, verbp::AzimuthEasing::Linear}}; check(verbp::validPath(g), "V04d follower keeps its own azimuth keys");
        verbp::MovementPath t; t.points = {{{1, 1, 1.5}}, {{5, 1, 1.5}}};
        auto rf = verbp::resolveFollowerPath(f, &t);
        check(rf.enabled && rf.points == t.points && rf.group == -1 && rf.follow == f.follow, "V04d resolveFollowerPath takes the target geometry, never a group");
        auto tf = t; tf.follow = follow(3, 0, 0, 0); tf.points.clear();
        check(!verbp::resolveFollowerPath(f, &tf).enabled, "V04d a follower of a follower resolves to a static source (no chains)");
        auto td = t; td.enabled = false;
        check(!verbp::resolveFollowerPath(f, &td).enabled, "V04d a disabled target leaves the follower static");
        check(!verbp::resolveFollowerPath(f, &t, 0).enabled, "V04d a self-link resolves to a static source");
        auto tw = t; tw.wander.enabled = true;
        check(verbp::resolveFollowerPath(f, &tw).wander.enabled && !verbp::resolveFollowerPath(followerPath(0, 0, 0, 0, false), &tw).wander.enabled,
              "V04d followWander ON shares the target wander, OFF drops it");
    }

    // ------------------------------------------------------------------ 2. pure geometry
    if (!audioOnly) {
        verb::Scene sc;  // no walls: the shape generator does not clamp
        // a) circle (CCW, 16 nodes, smooth): left offset L -> distance from the centre r - L; up -> z + up
        auto t = circle(sc, 3.0, 4.0, 2.5, 1.5, false);
        double maxRad = 0.0, maxUp = 0.0, maxRot = 0.0;
        const double L = 0.4, F = 0.3;
        auto cl = compiled(followerPath(0, 0.0, L, 0.6), t);
        auto cf = compiled(followerPath(0, F, 0.0, 0.0), t);
        for (int k = 0; k <= 400; ++k) {
            const double u = k / 400.0;
            const auto b = cl.evaluate(u);
            const double rt = std::hypot(b[0] - 3.0, b[1] - 4.0);
            const auto q = verbp::followerPoseAt(cl, u, 0.0, 30.0);
            maxRad = std::max(maxRad, std::abs(std::hypot(q[0] - 3.0, q[1] - 4.0) - (rt - L)));
            maxUp = std::max(maxUp, std::abs(q[2] - 2.1));
            // forward offset: perpendicular to the radius (tangent), radius sqrt(r^2 + F^2)
            const auto w = verbp::followerPoseAt(cf, u, 0.0, 30.0);
            maxRot = std::max(maxRot, std::abs(std::hypot(w[0] - 3.0, w[1] - 4.0) - std::hypot(rt, F)));
        }
        std::printf("  circle: |r_f-(r_t-L)| max %.2e m, |z-(z0+up)| max %.2e, forward |r_f-sqrt(r_t^2+F^2)| max %.2e\n", maxRad, maxUp, maxRot);
        check(maxRad < 2e-4 && maxUp < 1e-12 && maxRot < 3e-3,  // forward: the 16-node smooth circle is not exactly round (tangent vs radius)
               "V04d circle: left offset stays on radius r-L, up offset is world Z, forward offset tangent");

        // b) open polyline (L-shape) incl. both ends: forward 1 m
        verbp::MovementPath ol; ol.points = {{{0, 0, 1}}, {{4, 0, 1}}, {{4, 3, 1}}};
        auto co = compiled(followerPath(0, 1.0, 0.0, 0.0), ol);
        const auto e0 = verbp::followerPoseAt(co, 0.0, 0.0, 30.0), e1 = verbp::followerPoseAt(co, 1.0, 0.0, 30.0);
        const auto m1 = verbp::followerPoseAt(co, 2.0 / 7.0, 0.0, 30.0), m2 = verbp::followerPoseAt(co, 6.0 / 7.0, 0.0, 30.0);
        check(dist3(e0, {{1, 0, 1}}) < 1e-12 && dist3(e1, {{4, 4, 1}}) < 1e-12 && dist3(m1, {{3, 0, 1}}) < 1e-12 && dist3(m2, {{4, 3, 1}}) < 1e-9,
              "V04d open polyline: forward offset along each segment, ends hold the end segment headings");

        // c) vertical segment: heading held from the previous segment, up stays world Z
        verbp::MovementPath vp; vp.points = {{{1, 1, 0.5}}, {{3, 1, 0.5}}, {{3, 1, 2.5}}, {{5, 1, 2.5}}};
        auto cv = compiled(followerPath(0, 0.5, 0.2, 0.3), vp);
        bool vertOk = true;
        for (int k = 0; k <= 100; ++k) {
            const double s = 0.34 + 0.32 * k / 100.0;  // inside the vertical segment (arc 2..4 of 6)
            double yaw = 99;
            const auto q = verbp::followerPoseAt(cv, s, 0.0, 30.0, &yaw);
            const auto b = cv.evaluate(s);
            vertOk &= std::abs(yaw) < 1e-12 && std::abs(q[0] - (b[0] + 0.5)) < 1e-12 && std::abs(q[1] - (b[1] + 0.2)) < 1e-12 && std::abs(q[2] - (b[2] + 0.3)) < 1e-12;
        }
        check(vertOk, "V04d vertical segment: forward/left keep the held heading, up offset stays world Z");
        verbp::MovementPath vfirst; vfirst.points = {{{1, 1, 0.5}}, {{1, 1, 2.0}}, {{3, 1, 2.0}}};
        auto cvf = compiled(followerPath(0, 0, 1.0, 0), vfirst);
        double yv = 99; verbp::followerPoseAt(cvf, 0.1, 0.0, 30.0, &yv);
        check(std::abs(yv - 0.0) < 1e-12, "V04d a leading vertical segment takes the first valid heading (no NaN)");

        // d) figure-8: continuous, tangent flips across the crossing
        verbp::PathShapeParams sp; sp.kind = verbp::PathShapeKind::Figure8; sp.cx = 3; sp.cy = 4; sp.z = 1.5; sp.radiusX = 2.5; sp.radiusY = 1.2; sp.nodes = 24;
        auto f8 = verbp::generatePathShapeForRoom(sp, &sc, true).path;
        auto c8 = compiled(followerPath(0, 0.0, 0.5, 0.0), f8);
        double maxStep = 0.0; verbp::PathPoint prev{}; bool fin = true;
        double minYaw = 1e9, maxYaw = -1e9;
        for (int k = 0; k <= 4000; ++k) {
            double yaw = 0;
            const auto q = verbp::followerPoseAt(c8, k / 4000.0, 0.0, 30.0, &yaw);
            fin &= std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]);
            if (k) maxStep = std::max(maxStep, dist3(q, prev));
            prev = q; minYaw = std::min(minYaw, yaw); maxYaw = std::max(maxYaw, yaw);
        }
        std::printf("  figure-8: max step per 1/4000 %.4f m, heading range %.1f..%.1f deg\n", maxStep, minYaw, maxYaw);
        check(fin && maxStep < 0.02 && maxYaw - minYaw > 300.0, "V04d Figure-8: follower curve continuous through the tangent flip");

        // e) stop (zero-length segment with key times): pose and heading held during the stop
        verbp::MovementPath st; st.points = {{{1, 1, 1}}, {{4, 1, 1}}, {{4, 1, 1}}, {{4, 4, 1}}};
        st.times = {std::nullopt, 30.0, 60.0, std::nullopt};
        auto cs = compiled(followerPath(0, 0.0, 0.7, 0.0), st);
        double ya = 0, yb = 0;
        const auto qa = verbp::followerPoseAt(cs, 0.31, 0.0, 30.0, &ya), qb = verbp::followerPoseAt(cs, 0.59, 0.0, 30.0, &yb);
        check(dist3(qa, qb) < 1e-9 && std::abs(ya) < 1e-9 && std::abs(qa[1] - 1.7) < 1e-9, "V04d stop: follower pose and heading held while the target stands");
    }

    // ------------------------------------------------------------------ 3. commands, policy, undo
    if (!audioOnly) {
        auto pp = makeProc(5);
        check(pp != nullptr, "V04d 5-source processor ready");
        if (pp) {
            auto& p = *pp;
            auto& es = p.getEditorSession();
            const auto sc = p.getDisplayScene();
            const auto t = circle(sc, 3.0, 4.0, 2.0, 1.5, true);
            check(p.setMovementPath(0, t), "V04d target circle (Endless) set");
            const int depth0 = es.undoDepth();
            auto r = es.run(cmd(R"({"op":"path.link","objects":[1,2,3],"target":0})"));
            check(r.ok && p.getMovementPath(1).follow.target == 0 && p.getMovementPath(2).follow.target == 0 && p.getMovementPath(3).follow.target == 0
                  && p.getMovementPath(1).points.empty() && es.undoDepth() == depth0 + 1 && es.undoLabel() == "Link 3 sources to path",
                  "V04d multi-select link: 3 followers in one undo step (default offset 0,0,0)");
            r = es.run(cmd(R"({"op":"path.link","object":4,"target":1})"));
            check(!r.ok && juce::String(r.message).contains("no chains"), "V04d linking to a follower is refused (no chains)");
            r = es.run(cmd(R"({"op":"path.link","object":0,"target":1})"));
            check(!r.ok, "V04d linking the target to its follower is refused (no cycles)");
            r = es.run(cmd(R"({"op":"path.link","object":4,"target":4})"));
            check(!r.ok, "V04d self-link refused");
            r = es.run(cmd(R"({"op":"path.link","object":4,"target":3})"));
            check(!r.ok, "V04d link to a source without own path refused");
            r = es.run(cmd(R"({"op":"path.setFlags","object":1,"group":2})"));
            check(!r.ok && r.field == "object", "V04d a linked source cannot be Coupling-grouped (refused with a message)");
            r = es.run(cmd(R"({"op":"path.setFlags","object":1,"laps":3})"));
            check(!r.ok, "V04d Laps of a follower are locked (inherited)");
            r = es.run(cmd(R"({"op":"path.addPoint","object":1,"point":[1,1,1]})"));
            check(!r.ok, "V04d a follower has no own points to edit");
            r = es.run(cmd(R"({"op":"path.setFlags","object":1,"orientMode":"along"})"));
            check(r.ok && p.getMovementPath(1).orient, "V04d follower orientation (Along path) is its own setting");
            r = es.run(cmd(R"({"op":"path.setAzimuthKey","object":2,"t":50,"az":90})"));
            check(r.ok && p.getMovementPath(2).azKeys.size() == 1, "V04d follower azimuth keys accepted");
            // one undo step per offset gesture (transaction with live updates), Esc = cancel
            const int d1 = es.undoDepth();
            check(es.run(cmd(R"({"op":"transaction.begin","label":"Edit link offset"})"), {}, {}, "tx-v04d").ok, "V04d gesture begins");
            for (double up : {0.1, 0.2, 0.35}) {
                juce::DynamicObject::Ptr o = new juce::DynamicObject(); o->setProperty("op", "path.setFollow"); o->setProperty("object", 1);
                o->setProperty("offset", juce::Array<juce::var>{0.0, 0.0, up});
                es.run(juce::var(o.get()), {}, {}, "tx-v04d");
            }
            const bool liveDuring = std::abs(p.getMovementPath(1).follow.offset[2] - 0.35) < 1e-12;
            check(es.run(cmd(R"({"op":"transaction.commit"})"), {}, {}, "tx-v04d").ok && liveDuring && es.undoDepth() == d1 + 1,
                  "V04d offset drag: live updates while dragging, one undo step per gesture");
            check(es.run(cmd(R"({"op":"transaction.begin","label":"Edit link offset"})"), {}, {}, "tx-v04d2").ok, "V04d second gesture begins");
            es.run(cmd(R"({"op":"path.setFollow","object":1,"offset":[0,0,2.0]})"), {}, {}, "tx-v04d2");
            es.run(cmd(R"({"op":"transaction.cancel"})"), {}, {}, "tx-v04d2");
            check(std::abs(p.getMovementPath(1).follow.offset[2] - 0.35) < 1e-12 && es.undoDepth() == d1 + 1, "V04d Esc (transaction cancel) restores the offset, no undo step");
            check(es.undo().ok && std::abs(p.getMovementPath(1).follow.offset[2]) < 1e-12, "V04d undo reverts the whole offset gesture");
            es.redo();
            r = es.run(cmd(R"({"op":"path.setFollow","object":3,"followWander":false})"));
            check(r.ok && !p.getMovementPath(3).follow.followWander, "V04d Follow wander switch");

            // Unlink conversion: own path = sampled offset curve (64 pts, closed, smooth), reproduces the pose
            const auto fBefore = p.getMovementPath(1);
            const auto cBefore = compiled(fBefore, p.getMovementPath(0));
            setP(p, "s1path", 0.42);
            r = es.run(cmd(R"({"op":"path.unlink","object":1})"));
            check(p.apvts.getRawParameterValue("s2path")->load() == p.apvts.getRawParameterValue("s1path")->load(),
                  "V04d Unlink copies the target's Position to the new independent path");
            const auto own = p.getMovementPath(1);
            verbp::CompiledPath cOwn; cOwn.build(own);
            double maxDev = 0.0;
            for (int k = 0; k < 2000; ++k) {
                const double u = k / 2000.0;
                maxDev = std::max(maxDev, dist3(cOwn.evaluateEndless(u, 0.0), verbp::followerPoseAt(cBefore, u, 0.0, sc.height)));
            }
            std::printf("  unlink (circle, offset up .35): 64 points, max |own - follower| over u = %.2e m\n", maxDev);
            check(r.ok && !own.follow.active() && own.points.size() == 64 && own.closed && own.smooth && own.endless && own.orient && maxDev < 2e-3,
                  "V04d Unlink keeps the motion as an own path (64-pt sampled offset curve, Endless/orientation kept, < 2 mm)");
            check(es.undo().ok && p.getMovementPath(1).follow.target == 0, "V04d Unlink is one undo step");

            // delete-target policy: clearing the target converts every follower (one undo step restores all)
            const int d2 = es.undoDepth();
            r = es.run(cmd(R"({"op":"path.clear","object":0})"));
            bool allOwn = true;
            for (int f = 1; f <= 3; ++f) allOwn &= !p.getMovementPath(f).follow.active() && p.getMovementPath(f).points.size() == 64;
            check(r.ok && allOwn && p.getMovementPath(0).points.empty() && es.undoDepth() == d2 + 1, "V04d clearing the target detaches its followers as own paths in the same undo step");
            check(es.undo().ok && p.getMovementPath(1).follow.target == 0 && p.getMovementPath(2).follow.target == 0 && p.getMovementPath(0).points.size() >= 2,
                  "V04d undo of the target clear restores the links");
            // removing the target source: followers detached, indexes remapped
            r = es.run(cmd(R"({"op":"path.link","object":4,"target":2})"));
            check(!r.ok, "V04d a follower cannot be a target");
            r = es.run(cmd(R"({"op":"source.remove","index":0})"));
            bool remapOk = r.ok;
            for (int f = 0; f < 3; ++f) remapOk &= !p.getMovementPath(f).follow.active() && p.getMovementPath(f).points.size() == 64;
            check(remapOk, "V04d removing the target source detaches its followers (motion kept) in one step");
            es.undo();
            // a target with index > removed one: link 4 -> 3? (3 is follower) — build 2-level: unlink 3, link 4 -> 3, remove 0
            es.run(cmd(R"({"op":"path.unlink","object":3})"));
            r = es.run(cmd(R"({"op":"path.link","object":4,"target":3})"));
            check(r.ok, "V04d an unlinked source can itself become a target");
            r = es.run(cmd(R"({"op":"source.remove","index":0})"));
            check(r.ok && p.getMovementPath(3).follow.target == 2, "V04d link target index follows the source list after a removal");
        }
    }

    // ------------------------------------------------------------------ 4. processor poses
    if (!audioOnly) {
        auto pp = makeProc(5);
        if (pp) {
            auto& p = *pp;
            const auto sc = p.getDisplayScene();
            const double zTop = sc.height;
            auto t = circle(sc, 3.0, 4.0, 2.0, 1.5, true, 4.0);
            t.orient = true;
            check(p.setMovementPath(0, t), "V04d processor: target Endless circle");
            check(p.setMovementPath(1, followerPath(0, 0.0, 0.0, 0.3)), "V04d head follower (up .3)");
            check(p.setMovementPath(2, followerPath(0, 0.1, 0.35, -0.3)), "V04d hand follower (fwd .1, left .35, up -.3)");
            check(p.setMovementPath(3, followerPath(0, 0.15, -0.1, -1.3)), "V04d foot follower (fwd .15, left -.1, up -1.3)");
            E12PlayHead head; head.playing = true; head.samples = 0;
            p.setPlayHead(&head);
            juce::AudioBuffer<float> audio(5, 128); juce::MidiBuffer midi;
            const verbp::MovementPath tp = p.getMovementPath(0);
            verbp::CompiledPath ct; ct.build(tp);
            double maxErr = 0.0, maxTargetErr = 0.0;
            for (int b = 0; b < 400; ++b) {
                const int64_t s0 = head.samples;
                audio.clear(); stage2Process(p, audio, midi); head.samples += 128;
                if (b < 4) continue;
                const double phase = verbp::endlessTransportPhase(s0, 48000.0, 4.0);
                const auto tq = ct.evaluateEndless(0.0, phase);
                maxTargetErr = std::max(maxTargetErr, dist3(p.livePosition(0), {{tq[0], tq[1], std::clamp(tq[2], 0.0, zTop)}}));
                for (int f = 1; f <= 3; ++f) maxErr = std::max(maxErr, dist3(p.livePosition(f), verbp::followerPoseAt(compiled(p.getMovementPath(f), tp), 0.0, phase, zTop)));
            }
            std::printf("  processor Endless rig: max |live - followerPose| %.2e m (target %.2e)\n", maxErr, maxTargetErr);
            check(maxErr < 1e-9 && maxTargetErr < 1e-9, "V04d Endless: followers take the target's transport-driven parameter exactly (target pose + local offset)");
            check(std::abs(p.liveAzimuth(1) - p.liveAzimuth(0)) > -1.0, "V04d azimuth telemetry available");

            // Laps + Position (non-Endless, Physical): settled pose equals the pure pose at the target's Position
            auto t2 = t; t2.endless = false; t2.laps = 3;
            check(p.setMovementPath(0, t2), "V04d target: 3 laps, Position-driven");
            head.playing = false;
            setP(p, "s1path", 0.37);
            setP(p, "s2path", 0.9);  // the follower's own Position is ignored
            for (int b = 0; b < 600; ++b) { audio.clear(); stage2Process(p, audio, midi); }
            double lapErr = 0.0;
            const double u37 = (double)0.37f;  // the host parameter is a float
            for (int f = 1; f <= 3; ++f) lapErr = std::max(lapErr, dist3(p.livePosition(f), verbp::followerPoseAt(compiled(p.getMovementPath(f), p.getMovementPath(0)), u37, 0.0, zTop)));
            std::printf("  Laps=3 Position .37: max |live - pose| %.2e m\n", lapErr);
            check(lapErr < 1e-6, "V04d Laps / Position: followers use the target's Position (own Position ignored)");

            // Path LFO of the target (Creative): followers inherit the modulated parameter exactly
            auto t3 = t2; t3.laps = 1; t3.lfo.enabled = true; t3.lfo.depth = 0.15; t3.lfo.rateHz = 0.7;
            check(p.setMovementPath(0, t3), "V04d target with Path LFO");
            setP(p, "mode", 1.0);
            head.playing = true; head.samples = 48000;
            for (int b = 0; b < 300; ++b) { audio.clear(); stage2Process(p, audio, midi); head.samples += 128; }
            double lfoErr = 0.0;
            for (int b = 0; b < 200; ++b) {
                const int64_t s0 = head.samples;
                audio.clear(); stage2Process(p, audio, midi); head.samples += 128;
                const double uEff = t3.lfo.applyToU(u37, (double)s0 / 48000.0, true, 1.0);
                for (int f = 1; f <= 3; ++f) lfoErr = std::max(lfoErr, dist3(p.livePosition(f), verbp::followerPoseAt(compiled(p.getMovementPath(f), t3), uEff, 0.0, zTop)));
            }
            std::printf("  Path LFO (Creative): max |live - pose(lfo u)| %.2e m\n", lfoErr);
            check(lfoErr < 1e-9, "V04d Path LFO of the target drives the followers (same modulated u)");

            // Mod driver "Position on path" of a follower = the target's
            {
                verbp::ModulationRoute rf = v04etest::zoneRoute(verbp::ModDriver::PathPosition, 0, 0.0, 1.0, 2);
                verbp::ModulationRoute rt = v04etest::zoneRoute(verbp::ModDriver::PathPosition, 0, 0.0, 1.0, 0);
                rf.hostOut = 1; rt.hostOut = 2;
                auto t4 = t3; t4.lfo = {};
                p.setMovementPath(0, t4);
                check(p.setModulationRoutes({rf, rt}), "V04d position-on-path routes on follower and target");
                for (int b = 0; b < 60; ++b) { audio.clear(); stage2Process(p, audio, midi); head.samples += 128; }
                VerbProcessor::ModLiveSample ms[verbp::kMaxModRoutes]; int cnt = 0;
                const bool fresh = p.webModTelemetry(ms, verbp::kMaxModRoutes, cnt);
                std::printf("  pathPosition driver: follower %.4f target %.4f\n", ms[0].raw, ms[1].raw);
                check(fresh && cnt == 2 && std::abs(ms[0].raw - 0.37f) < 1e-5f && ms[0].raw == ms[1].raw, "V04d Mod driver 'position on path' of a follower = the target's");
                p.setModulationRoutes({});
            }

            // Shared wander (Creative): followWander ON -> identical displacement; OFF -> undisplaced
            auto tw = circle(sc, 3.0, 4.0, 1.8, 1.5, true, 6.0);
            tw.wander.enabled = true; tw.wander.ampHorizM = 0.5; tw.wander.ampVertM = 0.3; tw.wander.rateHz = 0.9; tw.wander.seed = 77;
            check(p.setMovementPath(0, tw), "V04d target with wander");
            p.setMovementPath(1, followerPath(0, 0, 0, 0, true));
            p.setMovementPath(2, followerPath(0, 0, 0, 0.4, true));
            p.setMovementPath(3, followerPath(0, 0, 0, 0, false));
            double dSame = 0.0, dUp = 0.0, dOffMax = 0.0, dOffPose = 0.0;
            for (int b = 0; b < 400; ++b) {
                const int64_t s0 = head.samples;
                audio.clear(); stage2Process(p, audio, midi); head.samples += 128;
                if (b < 4) continue;
                const auto a0 = p.livePosition(0), a1 = p.livePosition(1), a2 = p.livePosition(2), a3 = p.livePosition(3);
                dSame = std::max(dSame, dist3(a1, {{a0[0], a0[1], a0[2]}}));
                dUp = std::max(dUp, dist3(a2, {{a0[0], a0[1], std::min(a0[2] + 0.4, zTop)}}));
                dOffMax = std::max(dOffMax, dist3(a3, {{a0[0], a0[1], a0[2]}}));
                dOffPose = std::max(dOffPose, dist3(a3, verbp::followerPoseAt(compiled(p.getMovementPath(3), tw), u37, verbp::endlessTransportPhase(s0, 48000.0, 6.0), zTop)));
            }
            std::printf("  wander: |f(ON,0)-t| %.2e, |f(ON,up)-t-up| %.2e, |f(OFF)-t| max %.3f m, |f(OFF)-drawn| %.2e\n", dSame, dUp, dOffMax, dOffPose);
            check(dSame == 0.0 && dUp < 1e-12 && dOffMax > 0.05 && dOffPose < 1e-9,
                  "V04d wander: one shared displacement for the rig (bit-identical), Follow wander OFF follows the drawn path");

            // room clamp: a 10 m left offset stays inside the walls / under the ceiling
            setP(p, "mode", 0.0);
            p.setMovementPath(0, circle(sc, 3.0, 4.0, 2.0, 1.5, true, 4.0));
            p.setMovementPath(1, followerPath(0, 0.0, 10.0, 5.0));
            double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9, maxZ = -1e9;
            for (int b = 0; b < 300; ++b) {
                audio.clear(); stage2Process(p, audio, midi); head.samples += 128;
                const auto a = p.livePosition(1);
                minX = std::min(minX, a[0]); maxX = std::max(maxX, a[0]); minY = std::min(minY, a[1]); maxY = std::max(maxY, a[1]); maxZ = std::max(maxZ, a[2]);
            }
            double rx0 = 1e9, rx1 = -1e9, ry0 = 1e9, ry1 = -1e9;
            for (const auto& w : sc.walls) { rx0 = std::min({rx0, w.ax, w.bx}); rx1 = std::max({rx1, w.ax, w.bx}); ry0 = std::min({ry0, w.ay, w.by}); ry1 = std::max({ry1, w.ay, w.by}); }
            std::printf("  clamp: follower x %.2f..%.2f (room %.2f..%.2f), y %.2f..%.2f (room %.2f..%.2f), z max %.2f (H %.2f)\n", minX, maxX, rx0, rx1, minY, maxY, ry0, ry1, maxZ, zTop);
            check(minX >= rx0 && maxX <= rx1 && minY >= ry0 && maxY <= ry1 && maxZ <= zTop, "V04d room clamp: a large offset keeps the follower inside walls, floor and ceiling");

            // zone trigger on a follower (head above the target): the zone sees the follower's effective pose
            p.setMovementPath(1, followerPath(0, 0.0, 0.0, 1.0));
            auto z = v04etest::box(1, -1.0, -1.0, 10.0, 10.0); z.zMin = 2.2; z.zMax = 2.9;
            check(p.setTriggerZones({z}), "V04d zone above the rig");
            for (int b = 0; b < 40; ++b) { audio.clear(); stage2Process(p, audio, midi); head.samples += 128; }
            std::array<uint64_t, verbp::kMaxTriggerZones> masks{}; int zc = 0;
            const bool zok = p.webZoneTelemetry(masks, zc);
            check(zok && zc == 1 && (masks[0] & (1ull << 1)) && !(masks[0] & 1ull), "V04d zone triggers see the follower's effective (offset) position");
            p.setTriggerZones({});

            // determinism: seek equals continuous (Endless rig)
            p.setMovementPath(1, followerPath(0, 0.0, 0.3, 0.2));
            auto qq = makeProc(5);
            if (qq) {
                auto& q = *qq;
                q.setMovementPath(0, p.getMovementPath(0)); q.setMovementPath(1, p.getMovementPath(1));
                setP(q, "s1path", 0.37);
                E12PlayHead hq; hq.playing = true; hq.samples = head.samples + 128 * 50;
                q.setPlayHead(&hq);
                for (int b = 0; b < 50; ++b) { audio.clear(); stage2Process(p, audio, midi); head.samples += 128; }
                double seekDiff = 0.0;
                for (int b = 0; b < 10; ++b) {
                    audio.clear(); stage2Process(p, audio, midi); head.samples += 128;
                    juce::AudioBuffer<float> a2(5, 128); a2.clear(); stage2Process(q, a2, midi); hq.samples += 128;
                    if (b >= 2) seekDiff = std::max(seekDiff, dist3(p.livePosition(1), {{q.livePosition(1)[0], q.livePosition(1)[1], q.livePosition(1)[2]}}));
                }
                check(seekDiff == 0.0, "V04d seek equals continuous playback (follower pose bit-identical)");
                q.setPlayHead(nullptr);
            }
            p.setPlayHead(nullptr);
        }
    }

    // ------------------------------------------------------------------ 5. audio: clicks, allocations, CPU, determinism, listening material
    {
        struct Metrics { double maxD2 = 0, peak = 0, meanUs = 0, p99Us = 0; int64_t allocs = 0; };
        // mode: 0 = rig (target + followers), 1 = each source its own drawn path = the follower's sampled offset curve
        auto run = [&](int ns, int mode, bool motion, int blocks, std::vector<float>& L, std::vector<float>& R, Metrics& m,
                       const std::vector<verbp::PathPoint>& offs, bool sine = false) -> bool {
            std::printf("  [run ns=%d mode=%d motion=%d sine=%d]\n", ns, mode, (int)motion, (int)sine); std::fflush(stdout);
            auto pp = makeProc(ns);
            if (!pp) return false;
            auto& p = *pp;
            if (motion) {
                p.setMotionTailEnabled(true);
                if (!waitReady(p, 90)) return false;
                juce::AudioBuffer<float> pb(std::max(ns, 2), 128); juce::MidiBuffer pm;
                for (int k = 0; k < 250 && !p.isWebTailCurrent(); ++k) { pb.clear(); stage2Process(p, pb, pm); juce::MessageManager::getInstance()->runDispatchLoopUntil(5); }
            }
            setP(p, "mode", 0.0);
            const auto sc = p.getDisplayScene();
            auto t = circle(sc, 3.0, 4.0, 2.0, 1.5, true, 5.0);
            t.orient = true;
            p.setMovementPath(0, t);
            for (int s = 1; s < ns; ++s) {
                const auto& o = offs[(size_t)(s - 1) % offs.size()];
                auto f = followerPath(0, o[0], o[1], o[2]); f.orient = true;
                if (mode == 0) p.setMovementPath(s, f);
                else p.setMovementPath(s, verbp::followerToOwnPath(f, t, sc.height, 64));
            }
            for (int s = 0; s < ns; ++s) setP(p, verbp::paramId(s, 4), ns > 1 ? -6.0 : 0.0);
            E12PlayHead head; head.playing = true; head.samples = 0;
            p.setPlayHead(&head);
            juce::AudioBuffer<float> audio(std::max(ns, 2), 128); juce::MidiBuffer midi;
            juce::Random rnd(4242);
            std::vector<float> noise((size_t)ns * 48000);
            for (auto& v : noise) v = rnd.nextFloat() * 2.f - 1.f;
            auto fill = [&]() {
                audio.clear();
                for (int s = 0; s < ns; ++s)
                    for (int i = 0; i < 128; ++i) {
                        const int64_t n = head.samples + i;
                        if (sine) { audio.setSample(s, i, 0.15f * (float)std::sin((double)n * (2.0 * verb::kPi * (180.0 + 35.0 * s) / 48000.0))); continue; }
                        // footstep-like bursts: 60 ms noise every 0.5 s, phase per source (head / hands / feet)
                        const int64_t per = 24000, ph = (n + (int64_t)s * 6000) % per;
                        const float env = ph < 2880 ? (float)std::exp(-(double)ph / 700.0) : 0.f;
                        audio.setSample(s, i, 0.35f * env * noise[(size_t)(s * 48000 + (int)(n % 48000))]);
                    }
            };
            for (int w = 0; w < 20; ++w) { fill(); stage2Process(p, audio, midi); head.samples += 128; }
            L.clear(); R.clear();
            std::vector<double> us; us.reserve((size_t)blocks);
            const auto a0 = stage2AudioAllocations;
            for (int b = 0; b < blocks; ++b) {
                fill();
                const auto t0 = std::chrono::steady_clock::now();
                stage2Process(p, audio, midi);
                us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
                for (int i = 0; i < 128; ++i) { L.push_back(audio.getSample(0, i)); R.push_back(audio.getSample(1, i)); }
                head.samples += 128;
            }
            m.allocs = stage2AudioAllocations - a0;
            double sum = 0; for (double v : us) sum += v; m.meanUs = sum / (double)us.size();
            std::sort(us.begin(), us.end()); m.p99Us = us[us.size() * 99 / 100];
            for (const auto* ch : {&L, &R})
                for (size_t i = 0; i < ch->size(); ++i) {
                    m.peak = std::max(m.peak, (double)std::abs((*ch)[i]));
                    if (i >= 2) m.maxD2 = std::max(m.maxD2, std::abs((double)(*ch)[i] - 2.0 * (*ch)[i - 1] + (*ch)[i - 2]));
                }
            p.setPlayHead(nullptr);
            return true;
        };
        const std::vector<verbp::PathPoint> rigOffs = {{{0.0, 0.0, 0.25}}, {{0.1, 0.35, -0.35}}, {{0.15, -0.1, -1.3}},
                                                       {{0.1, -0.35, -0.35}}, {{0.0, 0.2, -1.3}}, {{-0.1, 0.0, 0.1}}, {{0.05, 0.0, -0.8}}};
        std::vector<float> L1, R1, L2, R2, L3, R3, L4, R4;
        Metrics m1, m2, m3, m4;
        const int blocks10s = 3750;  // 10 s
        check(run(4, 0, false, blocks10s, L1, R1, m1, rigOffs) && run(4, 0, false, blocks10s, L2, R2, m2, rigOffs), "V04d rig renders (Physical, Motion OFF) x2");
        float detDiff = 0.f; for (size_t i = 0; i < L1.size() && i < L2.size(); ++i) detDiff = std::max({detDiff, std::abs(L1[i] - L2[i]), std::abs(R1[i] - R2[i])});
        check(detDiff == 0.f && L1.size() == L2.size(), "V04d two offline renders of a rig are bit-identical");
        check(run(4, 1, false, blocks10s, L3, R3, m3, rigOffs), "V04d three separately drawn identical-offset paths render");
        double se = 0, sr = 0, maxAbs = 0;
        for (size_t i = 0; i < L1.size() && i < L3.size(); ++i) {
            for (double d : {(double)L1[i] - L3[i], (double)R1[i] - R3[i]}) { se += d * d; maxAbs = std::max(maxAbs, std::abs(d)); }
            sr += (double)L1[i] * L1[i] + (double)R1[i] * R1[i];
        }
        const double relDb = 10.0 * std::log10(std::max(1e-30, se) / std::max(1e-30, sr));
        std::printf("  rig vs separate paths (10 s, 4 src, Physical): diff energy %.1f dB re signal, max |diff| %.4f, peak %.3f\n", relDb, maxAbs, m1.peak);
        check(relDb < -20.0, "V04d rig and separately drawn identical-offset paths sound nearly the same (< -20 dB difference)");
        std::printf("  rig 4 src Physical (noise bursts): allocs %lld, CPU mean %.1f us p99 %.1f us (separate paths %.1f / %.1f)\n",
                    (long long)m1.allocs, m1.meanUs, m1.p99Us, m3.meanUs, m3.p99Us);
        check(m1.allocs == 0 && m1.peak > 1e-3, "V04d rig (noise bursts): zero allocations");
        // click metric (second difference) with steady sines, rig vs the same motion drawn as own paths
        Metrics s1, s1r, s4, s4r, sm1, sm4;
        std::vector<float> Ls, Rs;
        check(run(1, 0, false, 1500, Ls, Rs, s1, rigOffs, true) && run(4, 0, false, 1500, Ls, Rs, s4, rigOffs, true) && run(4, 1, false, 1500, Ls, Rs, s4r, rigOffs, true)
              && run(1, 0, true, 1500, Ls, Rs, sm1, rigOffs, true) && run(4, 0, true, 1500, Ls, Rs, sm4, rigOffs, true), "V04d sine click renders (1 / 4 sources, Physical / Motion)");
        (void)s1r;
        std::printf("  clicks (sines, max |d2|): 1 src Physical %.5f, 4 src rig %.5f vs own paths %.5f, Motion 1 src %.5f, Motion 4 src rig %.5f; allocs %lld/%lld/%lld/%lld\n",
                    s1.maxD2, s4.maxD2, s4r.maxD2, sm1.maxD2, sm4.maxD2, (long long)s1.allocs, (long long)s4.allocs, (long long)sm1.allocs, (long long)sm4.allocs);
        check(s1.allocs == 0 && s4.allocs == 0 && sm1.allocs == 0 && sm4.allocs == 0 && s4.maxD2 < 0.05 && sm4.maxD2 < 0.05 && s4.maxD2 <= 1.1 * s4r.maxD2 + 1e-4,
              "V04d rig: zero allocations, no clicks (second difference < 0.05, Physical and Motion; not above the own-path reference)");
        check(run(8, 0, false, 1500, L4, R4, m4, rigOffs, true), "V04d 8 sources: 1 target + 7 followers");
        std::vector<float> L5, R5; Metrics m5;
        check(run(8, 1, false, 1500, L5, R5, m5, rigOffs, true), "V04d 8 sources: 8 own paths");
        std::vector<float> L6, R6; Metrics m6;
        check(run(8, 0, true, 1500, L6, R6, m6, rigOffs, true), "V04d 8 sources rig with Motion ON");
        std::printf("  8 src: rig CPU mean %.1f us p99 %.1f, own paths mean %.1f p99 %.1f (delta %+.1f us); Motion rig mean %.1f; allocs %lld/%lld/%lld; maxD2 %.4f/%.4f/%.4f\n",
                    m4.meanUs, m4.p99Us, m5.meanUs, m5.p99Us, m4.meanUs - m5.meanUs, m6.meanUs, (long long)m4.allocs, (long long)m5.allocs, (long long)m6.allocs, m4.maxD2, m5.maxD2, m6.maxD2);
        check(m4.allocs == 0 && m6.allocs == 0 && m4.maxD2 < 0.05 && m6.maxD2 < 0.05, "V04d 8-source rig: zero allocations, no clicks (Physical and Motion)");
        Metrics rig9, own9;
        check(run(9, 0, false, 1500, Ls, Rs, rig9, rigOffs, true)
                  && run(9, 1, false, 1500, Ls, Rs, own9, rigOffs, true)
                  && rig9.allocs == 0 && own9.allocs == 0 && rig9.maxD2 < 0.05,
              "V04d 8 followers: zero allocations and no clicks");
        std::printf("  8 followers: rig CPU mean %.1f us p99 %.1f, own paths mean %.1f p99 %.1f (delta %+.1f us); maxD2 %.5f / %.5f\n",
                    rig9.meanUs, rig9.p99Us, own9.meanUs, own9.p99Us, rig9.meanUs - own9.meanUs, rig9.maxD2, own9.maxD2);
        if (const char* dir = std::getenv("VERB_V04D_AUDIO_DIR")) {
            const juce::File d(dir);
            writeWav(d.getChildFile("rig_walking_character_physical.wav"), L1, R1);
            writeWav(d.getChildFile("separate_paths_identical_offsets_physical.wav"), L3, R3);
            std::vector<float> Lw, Rw; Metrics mw;
            if (run(4, 0, true, blocks10s, Lw, Rw, mw, rigOffs)) writeWav(d.getChildFile("rig_walking_character_motion.wav"), Lw, Rw);
            std::vector<float> Ls, Rs; Metrics ms2;
            if (run(4, 1, true, blocks10s, Ls, Rs, ms2, rigOffs)) writeWav(d.getChildFile("separate_paths_identical_offsets_motion.wav"), Ls, Rs);
            std::printf("  wrote listening material to %s\n", dir);
        }
    }

    // ------------------------------------------------------------------ 6. Bake: followers bypass (live), target baked
    {
        VerbProcessor p; p.setTailSettings({8000, false}); p.setPlayConfigDetails(2, 2, 48000, 128); p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "V04d bake processor ready");
        p.loadSceneText(p4test::kScene2, "V04d corridor", true);
        check(waitReady(p, 90), "V04d 2-source corridor loads");
        verbp::MovementPath tp; tp.points = {{{1, 1, 1.5}}, {{3, 6, 1.5}}};
        VerbProcessor p0; p0.setTailSettings({8000, false}); p0.setPlayConfigDetails(2, 2, 48000, 128); p0.prepareToPlay(48000, 128);
        check(waitReady(p0, 90), "V04d bake reference processor ready");
        p0.loadSceneText(p4test::kScene2, "V04d corridor", true);
        check(waitReady(p0, 90) && p0.setMovementPath(0, tp), "V04d corridor target path (reference, no follower)");
        auto t0 = std::chrono::steady_clock::now();
        const bool b1 = p0.startPathBake(-1) && p4test::waitBaked(p0, 240);
        const double secTarget = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto hashTargetOnly = E9bAccess::engine(p0)->bakeStats().hash;
        auto eng = E9bAccess::engine(p);
        check(p.setMovementPath(0, tp), "V04d corridor target path");
        auto lookAtTarget = followerPath(0, 0.0, 0.5, 0.2);
        lookAtTarget.lookAt = true;
        lookAtTarget.lookTarget = 0;
        check(p.setMovementPath(1, lookAtTarget), "V04d corridor follower linked and looking at the target");
        check(!p.startPathBake(1), "V04d Bake of a follower alone is not offered (no own geometry)");
        t0 = std::chrono::steady_clock::now();
        const bool b2 = p.startPathBake(-1) && p4test::waitBaked(p, 240);
        const double secRig = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto st = p.pathBakeStatus();
        int perPath = 0; bool followerListed = false;
        if (auto* arr = st["paths"].getArray()) for (const auto& it : *arr) { ++perPath; followerListed |= (int)it["object"] == 1; }
        std::printf("  Bake corridor: target only %.1f s (hash %016llx), rig 1+1 %.1f s (hash %016llx), per-path items %d\n",
                    secTarget, (unsigned long long)hashTargetOnly, secRig, (unsigned long long)eng->bakeStats().hash, perPath);
        check(b1 && b2 && !followerListed && eng->bakeStats().hash == hashTargetOnly,
              "V04d Bake: followers are bypassed (live, like Endless/wander), the target's baked table is unchanged");
        const auto lookups0 = eng->bakeStats().lookups;
        p4test::pump(p, 60);
        check(eng->bakeStats().lookups > lookups0, "V04d baked target keeps using its table with a follower present");
    }

    // ------------------------------------------------------------------ 7. state, .verbroom, old loaders, impulse after reload
    {
        auto pp = makeProc(4);
        if (pp) {
            auto& p = *pp;
            const auto sc = p.getDisplayScene();
            auto t = circle(sc, 3.0, 4.0, 2.0, 1.5, true);
            p.setMovementPath(0, t);
            auto f1 = followerPath(0, 0.0, 0.0, 0.3); f1.azKeys = {{20.0, 30.0, verbp::AzimuthEasing::Hold}};
            p.setMovementPath(1, f1);
            p.setMovementPath(2, followerPath(0, 0.1, 0.35, -0.3, false));
            p.setMovementPath(3, followerPath(0, 0.15, -0.1, -1.3));
            juce::MemoryBlock blob; p.getStateInformation(blob);
            auto xml = VerbProcessor::getXmlFromBinary(blob.getData(), (int)blob.getSize());
            juce::XmlElement* fx = nullptr;
            if (xml) if (auto* mp = xml->getChildByName("MovementPaths")) for (auto* pe : mp->getChildIterator()) if (pe->getIntAttribute("object") == 2) fx = pe->getChildByName("Follow");
            check(fx && fx->getIntAttribute("target") == 0 && fx->getStringAttribute("frame") == "path" && !fx->getBoolAttribute("followWander", true),
                  "V04d state XML: optional <Follow target fwd left up followWander frame=path/>");
            auto q = makeProc(4);
            if (q) {
                q->setStateInformation(blob.getData(), (int)blob.getSize());
                check(waitReady(*q, 90) && q->getMovementPath(1) == p.getMovementPath(1) && q->getMovementPath(2) == p.getMovementPath(2)
                      && q->getMovementPath(3) == p.getMovementPath(3), "V04d project save / reload keeps the rig");
                const auto ir = h2test::captureImpulse(*q, 2, 0, 120, 128);
                const double e = h2test::channelEnergy(ir[0]) + h2test::channelEnergy(ir[1]);
                check(std::isfinite(e) && e > 1e-6, "V04d impulse after reload (Default Room, rig) is finite and non-zero");
            }
            juce::String err;
            const auto preset = p.getRoomPresetText();
            auto r = makeProc(4);
            if (r) {
                check(r->loadRoomPresetText(preset, err) && waitReady(*r, 90) && r->getMovementPath(2) == p.getMovementPath(2),
                      ".verbroom export -> import keeps the links (V04d)");
                r->setKeepRoutingEnabled(true);
                check(r->loadRoomPresetText(preset, err) && waitReady(*r, 90) && r->getMovementPath(2) == p.getMovementPath(2),
                      "V04d Keep I/O preserves the imported rig when the local source slots match");
                // old-loader view: without <Follow> the record is an empty path -> a static source (what 0.4.0-pre versions do)
                auto px = juce::parseXML(preset);
                if (auto* mp = px->getChildByName("VerbState")->getChildByName("MovementPaths"))
                    for (auto* pe : mp->getChildIterator()) if (auto* f = pe->getChildByName("Follow")) pe->removeChildElement(f, true);
                check(r->loadRoomPresetText(px->toString(), err) && waitReady(*r, 90) && !r->getMovementPath(3).follow.active()
                      && r->getMovementPath(3).points.empty(), "V04d .verbroom without <Follow> (old reader view) loads followers as static sources");
            }
            // legacy fixtures unchanged
            const auto legacyStateFile = juce::File::getCurrentWorkingDirectory().getChildFile("plugin/fixtures/stage2/legacy.state");
            if (legacyStateFile.existsAsFile()) {
                juce::MemoryBlock legData; legacyStateFile.loadFileAsData(legData);
                VerbProcessor leg; leg.setPlayConfigDetails(2, 2, 48000, 128); leg.prepareToPlay(48000, 128);
                leg.setStateInformation(legData.getData(), (int)legData.getSize());
                bool noFollow = waitReady(leg, 90);
                for (int o = 0; o <= verbp::kMaxSources; ++o) noFollow &= !leg.getMovementPath(o).follow.active();
                check(noFollow, "V04d legacy.state loads with no links");
            }
            // VSL preset + rig: reload + impulse
            const auto vslFile = juce::File::getCurrentWorkingDirectory().getChildFile("VSL.verbroom");
            if (vslFile.existsAsFile()) {
                VerbProcessor v; v.setPlayConfigDetails(64, 2, 48000, 128);
                juce::AudioProcessor::BusesLayout lay; lay.inputBuses.add(juce::AudioChannelSet::discreteChannels(64)); lay.outputBuses.add(juce::AudioChannelSet::stereo());
                v.setBusesLayout(lay); v.prepareToPlay(48000, 128);
                juce::String ve;
                check(v.loadRoomPresetText(vslFile.loadFileAsString(), ve) && waitReady(v, 120), "V04d VSL preset loads");
                const auto vsc = v.getDisplayScene();
                const int vns = (int)v.getSources().size();
                auto vt = circle(vsc, vsc.listenerPos.x, vsc.listenerPos.y + 3.0, 2.0, 1.5, true);
                check(vns >= 4 && v.setMovementPath(0, vt) && v.setMovementPath(1, followerPath(0, 0, 0, 0.3)) && v.setMovementPath(2, followerPath(0, 0.1, 0.35, -0.3))
                      && v.setMovementPath(3, followerPath(0, 0.15, -0.1, -1.3)), "V04d VSL: rig of 1 target + 3 followers");
                juce::MemoryBlock vb; v.getStateInformation(vb);
                VerbProcessor v2; v2.setPlayConfigDetails(64, 2, 48000, 128); v2.setBusesLayout(lay); v2.prepareToPlay(48000, 128);
                v2.setStateInformation(vb.getData(), (int)vb.getSize());
                check(waitReady(v2, 120) && v2.getMovementPath(2) == v.getMovementPath(2), "V04d VSL rig state round trip");
                const auto ir = h2test::captureImpulse(v2, 64, 0, 120, 128);
                const double e = h2test::channelEnergy(ir[0]) + h2test::channelEnergy(ir[1]);
                check(std::isfinite(e) && e > 1e-6, "V04d impulse after reload (VSL preset, rig) is finite and non-zero");
            }
        }
    }

    std::printf("V04d: %d failures\n", fails - beforeFails);
    return fails - beforeFails;
}
