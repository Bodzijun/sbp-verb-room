#pragma once
// E28: Look at a moving member of the same Group with ANY Start/End/keys/easing/laps. The owner is baked indexed by the group Position u.
// S3 tests: station/halfway equality with the live evaluation, processor-level bake never blocked, different-group stays blocked,
// Bake bag scene (+ variant), accuracy gate (table vs full reference trace) for cases a-e.
#include <cmath>
#include "MovementPath.h"

namespace e28 {
using namespace verbp;
struct Lcg { uint64_t s; double next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return (double)(s >> 11) / 9007199254740992.0; } };

// Random time map on a 4-point path (keys on the two interior points, any easing), optional closed + laps.
static MovementPath randomPath(Lcg& r, bool allowClosed, int group, PathPoint base) {
    MovementPath m; m.group = group;
    m.points = {PathPoint{base[0], base[1], 1.5}, PathPoint{base[0] + 1.6, base[1] + 1.2, 1.5}, PathPoint{base[0] + 3.0, base[1] - 0.2, 1.5}, PathPoint{base[0] + 1.2, base[1] - 1.6, 1.5}};
    if (allowClosed && r.next() < .5) { m.closed = true; m.laps = 1 + (int)(r.next() * 5.0); }
    m.start = std::floor(r.next() * 30.0); m.end = 100.0 - std::floor(r.next() * 30.0);
    const int nk = 2 + (int)(r.next() * 3.0);   // 2..4 timing keys (counting segment easings)
    m.times.assign(m.points.size(), std::nullopt); m.easings.assign(m.points.size(), PathEasing::Linear);
    if (nk >= 2) m.times[1] = 25.0 + std::floor(r.next() * 15.0);
    if (nk >= 3) m.times[2] = 60.0 + std::floor(r.next() * 20.0);
    for (auto& e : m.easings) e = (PathEasing)((int)(r.next() * 4.0) & 3);
    return m;
}

static juce::String terminalState(VerbProcessor& p, int timeoutMs, juce::String* reasonOut = nullptr) {
    juce::AudioBuffer<float> buf(4, 128); juce::MidiBuffer midi;
    const auto until = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    juce::String state, reason; double blockedSince = -1;
    while (juce::Time::getMillisecondCounterHiRes() < until) {
        buf.clear(); p.processBlock(buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        const auto st = p.pathBakeStatus(); state = st["state"].toString(); reason = st["reason"].toString();
        if (state == "baked" || state == "approximate" || state == "failed") break;
        // a stale/transient "blocked" can be shown right after a path edit: it counts only when it persists
        if (state == "blocked") { if (blockedSince < 0) blockedSince = juce::Time::getMillisecondCounterHiRes(); else if (juce::Time::getMillisecondCounterHiRes() - blockedSince > 2500) break; }
        else blockedSince = -1;
    }
    if (reasonOut) *reasonOut = reason;
    return state;
}

struct Rig {
    std::unique_ptr<VerbProcessor> pp = std::make_unique<VerbProcessor>();
    VerbProcessor& p = *pp;
    explicit Rig(int sources = 2, int rays = 4000) {
        p.setTailSettings({rays, false}); p.setPlayConfigDetails(4, 2, 48000, 128); p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "E28 rig ready");
        auto cfg = p.getSources(); cfg.resize((size_t)sources);
        for (int i = 0; i < sources; ++i) { cfg[(size_t)i].name = std::string(1, (char)('A' + i)); cfg[(size_t)i].firstIn = i; cfg[(size_t)i].format = 0; }
        p.setSources(cfg); check(waitReady(p, 90), "E28 rig sources");
    }
    ~Rig() { p.releaseResources(); }
};
} // namespace e28

// Pose of the owner at table coordinate u = live evaluate(u) + orientedAzimuth(LookAt) towards the target at the same u.
static void e28LiveOwner(const verbp::MovementPath& own, const verbp::MovementPath& tgt, double u, double scale, double manualYaw, double out[4]) {
    verbp::CompiledPath co, ct; co.build(own); ct.build(tgt);
    const auto q = co.evaluate(u), tp = ct.evaluate(u); double look = 0;
    const bool ok = verbp::lookAtYaw(q[0], q[1], tp[0], tp[1], look);
    out[0] = q[0] * scale; out[1] = q[1] * scale; out[2] = q[2] * scale;
    out[3] = verbp::orientedAzimuth(verbp::OrientMode::LookAt, manualYaw, 0.0, ok, look, own.orientOffset);
}

// Unit: random time maps. Bake, then compare the uIndexed description with the live evaluation at stations and halfway.
static int testE28Stations() {
    const int before = fails;
    using namespace verbp;
    e28::Lcg rng{0xE28ULL * 7919ULL};
    for (int c = 0; c < 6; ++c) {
        const bool listenerOwner = (c % 3 == 2);
        e28::Rig rig;
        auto& p = rig.p;
        MovementPath tgt = e28::randomPath(rng, true, 0, PathPoint{2, 4, 1.5});
        MovementPath own = e28::randomPath(rng, true, 0, PathPoint{3, 2, 1.5});
        own.lookAt = true; own.orientOffset = (c == 1) ? 25.0 : 0.0;
        int ownerObj;
        if (listenerOwner) { own.lookTarget = 0; ownerObj = kListener; p.setMovementPath(0, tgt); p.setMovementPath(kListener, own); }
        else { own.lookTarget = 1; ownerObj = 0; p.setMovementPath(0, own); p.setMovementPath(1, tgt); }
        const auto label = std::string("E28 stations case ") + std::to_string(c) + (listenerOwner ? " (listener owner)" : " (source owner)");
        check(p.startPathBake(-1), (label + ": bake accepted").c_str());
        juce::String reason; const auto state = e28::terminalState(p, 90000, &reason);
        check(state == "baked" || state == "approximate", (label + ": reaches baked/approximate, never blocked (" + state.toStdString() + " " + reason.toStdString() + ")").c_str());
        auto engine = E9bAccess::engine(p);
        const auto tables = engine->bakedTablesSnapshot();
        const double scale = p.webAppliedSize() * .01;
        const double manualYaw = p.apvts.getRawParameterValue(ownerObj == kListener ? juce::String("laz") : juce::String(paramId(ownerObj, 3)))->load();
        size_t found = 0; double worstPos = 0, worstYaw = 0, worstMidPos = 0, worstMidYaw = 0;
        const auto& ownRef = listenerOwner ? p.getMovementPath(kListener) : p.getMovementPath(0);
        const auto& tgtRef = listenerOwner ? p.getMovementPath(0) : p.getMovementPath(1);
        for (const auto& t : tables) for (const auto& mp : t->movingPaths) {
            if (!mp.uIndexed) continue;
            ++found;
            check(!mp.closed && mp.startU == 0.0 && mp.endU == 1.0 && mp.laps == 1 && mp.timingKeys.empty(), (label + ": identity mapping fields").c_str());
            const double D = mp.distance.back();
            for (size_t i = 0; i < mp.poses.size(); ++i) {
                const double u = mp.distance[i] / D; double ex[4];
                e28LiveOwner(ownRef, tgtRef, u, scale, manualYaw, ex);
                for (int k = 0; k < 3; ++k) worstPos = std::max(worstPos, std::abs(mp.poses[i][(size_t)k] - ex[k]));
                worstYaw = std::max(worstYaw, std::abs(std::remainder(mp.poses[i][3] - ex[3], 360.)));
                if (i + 1 < mp.poses.size()) {
                    const double um = .5 * (u + mp.distance[i + 1] / D); double em[4]; e28LiveOwner(ownRef, tgtRef, um, scale, manualYaw, em);
                    const auto a = mp.at(um);
                    for (int k = 0; k < 3; ++k) worstMidPos = std::max(worstMidPos, std::abs(a[(size_t)k] - em[k]));
                    worstMidYaw = std::max(worstMidYaw, std::abs(std::remainder(a[3] - em[3], 360.)));
                }
            }
            std::printf("  [E28] %s: stations=%zu D=%.2f pos<=%.2e m yaw<=%.4f deg | halfway pos<=%.4f m yaw<=%.3f deg\n", label.c_str(), mp.poses.size(), D, worstPos, worstYaw, worstMidPos, worstMidYaw);
        }
        check(found == 1, (label + ": exactly one u-indexed owner description").c_str());
        check(worstPos <= 1e-3 && worstYaw <= .05, (label + ": stations equal live evaluation (1 mm / 0.05 deg)").c_str());
        check(worstMidPos <= .01 && worstMidYaw <= .5, (label + ": halfway between stations within 1 cm / 0.5 deg").c_str());
    }
    return fails == before ? 0 : 1;
}

// Processor-level: states for the brief's cases, different-group stays blocked, E19b replaced expectation, Bake bag.
static int testE28Bake() {
    const int before = fails;
    using namespace verbp;
    auto bakeCase = [&](const char* label, auto configure, int sources = 2) {
        e28::Rig rig(sources);
        configure(rig.p);
        check(rig.p.startPathBake(-1), (std::string(label) + ": bake accepted").c_str());
        juce::String reason; const auto st = e28::terminalState(rig.p, 90000, &reason);
        std::printf("  [E28] %s: %s %s\n", label, st.toRawUTF8(), reason.toRawUTF8());
        return st;
    };
    auto plainB = [](int g) { MovementPath b; b.points = {PathPoint{5, 3, 1.5}, PathPoint{2, 3.5, 1.5}}; b.group = g; return b; };
    {   // E19b REPLACED expectation (requirement change E28): different Start % in one Group now bakes
        const auto st = bakeCase("E19b-replaced different Start in one Group", [&](VerbProcessor& p) {
            MovementPath a; a.points = {PathPoint{2, 5, 1.5}, PathPoint{3.5, 5.6, 1.5}, PathPoint{5, 6, 1.5}}; a.group = 0; a.lookAt = true; a.lookTarget = 1; a.start = 25.0;
            p.setMovementPath(0, a); p.setMovementPath(1, plainB(0)); });
        check(st == "baked" || st == "approximate", "E28 (replaces E19b 'different Start is blocked') different Start % in one Group bakes");
    }
    {   const auto st = bakeCase("different groups stay blocked", [&](VerbProcessor& p) {
            MovementPath a; a.points = {PathPoint{2, 5, 1.5}, PathPoint{5, 6, 1.5}}; a.group = 0; a.lookAt = true; a.lookTarget = 1;
            p.setMovementPath(0, a); p.setMovementPath(1, plainB(1)); });
        check(st == "blocked", "E28 different Groups: Look at stays Blocked");
    }
    {   const auto st = bakeCase("(a) Start20/End80 EaseInOut looks at plain", [&](VerbProcessor& p) {
            MovementPath a; a.points = {PathPoint{2, 5, 1.5}, PathPoint{3.5, 6.2, 1.5}, PathPoint{5, 5.5, 1.5}}; a.group = 0; a.lookAt = true; a.lookTarget = 1; a.start = 20; a.end = 80;
            a.times = {std::nullopt, 40.0, std::nullopt}; a.easings = {PathEasing::EaseInOut, PathEasing::EaseInOut, PathEasing::Linear};
            p.setMovementPath(0, a); p.setMovementPath(1, plainB(0)); });
        check(st == "baked" || st == "approximate", "E28 case (a) bakes");
    }
    {   const auto st = bakeCase("(c) closed laps 4 + keys looks at open Start 10", [&](VerbProcessor& p) {
            MovementPath a; a.points = {PathPoint{2, 5, 1.5}, PathPoint{3.5, 6.5, 1.5}, PathPoint{5, 5, 1.5}, PathPoint{3.5, 3.5, 1.5}}; a.closed = true; a.laps = 4; a.group = 0; a.lookAt = true; a.lookTarget = 1;
            a.times = {std::nullopt, 30.0, std::nullopt, std::nullopt}; a.easings = {PathEasing::EaseOut, PathEasing::Linear, PathEasing::Linear, PathEasing::Linear};
            auto b = plainB(0); b.start = 10; p.setMovementPath(0, a); p.setMovementPath(1, b); });
        check(st == "baked" || st == "approximate", "E28 case (c) bakes");
    }
    {   // Bake bag as the owner saved it, then a variant with Start 30 % and an EaseOut key on source 2
        for (int variant = 0; variant < 2; ++variant) {
            e28::Rig rig;
            auto& p = rig.p;
            const auto f = repoFile("test/Bake bag.verbroom");
            juce::String why;
            const bool loaded = f.existsAsFile() && p.loadRoomPresetText(f.loadFileAsString(), why) && waitReady(p, 90);
            check(loaded, "E28 Bake bag.verbroom loads");
            if (!loaded) continue;
            p.setTailSettings({4000, false});
            if (variant == 1) { auto b = p.getMovementPath(1); b.start = 30.0; b.easings = {PathEasing::EaseOut, PathEasing::Linear}; check(p.setMovementPath(1, b), "E28 Bake bag variant accepted"); }
            {   // let the edit settle (the UI does too: the user presses Bake after editing)
                juce::AudioBuffer<float> sb(4, 128); juce::MidiBuffer sm; const auto t0 = juce::Time::getMillisecondCounterHiRes();
                while (juce::Time::getMillisecondCounterHiRes() - t0 < 1500) { sb.clear(); p.processBlock(sb, sm); juce::MessageManager::getInstance()->runDispatchLoopUntil(5); }
            }
            check(p.startPathBake(-1), "E28 Bake bag bake accepted");
            juce::String reason; const auto st = e28::terminalState(p, 240000, &reason);
            std::printf("  [E28] Bake bag variant %d: %s %s\n", variant, st.toRawUTF8(), reason.toRawUTF8());
            check(st == "baked" || st == "approximate", variant ? "E28 Bake bag with Start 30 + EaseOut on source 2 bakes" : "E28 Bake bag still bakes");
        }
    }
    return fails == before ? 0 : 1;
}

// Accuracy gate: table vs full reference trace (E19 harness, 12000 rays, 4 phases), <= 1 dB.
static int testE28Gate() {
    const int before = fails;
    using namespace verbp;
    auto plainB = [](int g) { MovementPath b; b.points = {PathPoint{5, 3, 1.5}, PathPoint{2, 3.5, 1.5}}; b.group = g; return b; };
    e19Accuracy("E28a Start20/End80 EaseInOut keys -> plain, one Group", [&](VerbProcessor& p) {
        MovementPath a; a.points = {PathPoint{2, 5, 1.5}, PathPoint{3.5, 6.2, 1.5}, PathPoint{5, 5.5, 1.5}}; a.group = 0; a.lookAt = true; a.lookTarget = 1; a.start = 20; a.end = 80;
        a.times = {std::nullopt, 40.0, std::nullopt}; a.easings = {PathEasing::EaseInOut, PathEasing::EaseInOut, PathEasing::Linear};
        p.setMovementPath(0, a); p.setMovementPath(1, plainB(0)); });
    e19Accuracy("E28b different keys and easings on both", [&](VerbProcessor& p) {
        MovementPath a; a.points = {PathPoint{2, 5, 1.5}, PathPoint{3.5, 6.2, 1.5}, PathPoint{5, 5.5, 1.5}}; a.group = 0; a.lookAt = true; a.lookTarget = 1;
        a.times = {std::nullopt, 35.0, std::nullopt}; a.easings = {PathEasing::EaseIn, PathEasing::EaseOut, PathEasing::Linear};
        MovementPath b; b.points = {PathPoint{5, 3, 1.5}, PathPoint{3.5, 2.2, 1.5}, PathPoint{2, 3.5, 1.5}}; b.group = 0; b.start = 10;
        b.times = {std::nullopt, 60.0, std::nullopt}; b.easings = {PathEasing::EaseInOut, PathEasing::Linear, PathEasing::Linear};
        p.setMovementPath(0, a); p.setMovementPath(1, b); });
    e19Accuracy("E28c closed laps 4 + keys -> open Start 10", [&](VerbProcessor& p) {
        MovementPath a; a.points = {PathPoint{2, 5, 1.5}, PathPoint{3.5, 6.5, 1.5}, PathPoint{5, 5, 1.5}, PathPoint{3.5, 3.5, 1.5}}; a.closed = true; a.laps = 4; a.group = 0; a.lookAt = true; a.lookTarget = 1;
        a.times = {std::nullopt, 30.0, std::nullopt, std::nullopt}; a.easings = {PathEasing::EaseOut, PathEasing::Linear, PathEasing::Linear, PathEasing::Linear};
        auto b = plainB(0); b.start = 10; p.setMovementPath(0, a); p.setMovementPath(1, b); });
    e19Accuracy("E28d listener owner -> source, different timing", [&](VerbProcessor& p) {
        MovementPath l; l.points = {PathPoint{3, 2, 1.5}, PathPoint{5, 3, 1.5}}; l.group = 0; l.lookAt = true; l.lookTarget = 0; l.end = 70;
        MovementPath s; s.points = {PathPoint{2, 5, 1.5}, PathPoint{5, 6, 1.5}}; s.group = 0; s.start = 25; s.easings = {PathEasing::EaseInOut, PathEasing::Linear};
        p.setMovementPath(kListener, l); p.setMovementPath(0, s); });
    e19Accuracy("E28e owner table with a third unrelated Group member", [&](VerbProcessor& p) {
        auto cfg = p.getSources(); cfg.resize(3); cfg[2].name = "C"; cfg[2].firstIn = 2; cfg[2].format = 0; p.setSources(cfg); waitReady(p, 90);
        MovementPath a; a.points = {PathPoint{2, 5, 1.5}, PathPoint{3.5, 6.2, 1.5}, PathPoint{5, 5.5, 1.5}}; a.group = 0; a.lookAt = true; a.lookTarget = 1; a.start = 20; a.end = 85;
        MovementPath c; c.points = {PathPoint{1.5, 1.5, 1.5}, PathPoint{2.5, 3, 1.5}}; c.group = 0; c.start = 5; c.easings = {PathEasing::EaseOut, PathEasing::Linear};
        p.setMovementPath(0, a); p.setMovementPath(1, plainB(0)); p.setMovementPath(2, c); });
    return fails == before ? 0 : 1;
}
