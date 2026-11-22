#pragma once
#include <cmath>
#include <cstring>
#include "MovementPath.h"

// E19: orientation modes. S1 = data model, pure orientation function, persistence.
static int testE19Plugin() {
    const int before = fails;
    using namespace verbp;
    // --- pure geometry: listener->source, source->listener, source->source, ->point (all reduce to lookAtYaw on plan positions)
    double yaw = 0;
    check(lookAtYaw(0, 0, 1, 1, yaw) && std::abs(yaw - 45.0) < 1e-12, "E19 look at (1,1) from origin = 45 deg");
    check(lookAtYaw(3, 2, 3, 6, yaw) && std::abs(yaw - 90.0) < 1e-12, "E19 look straight +y = 90 deg");
    check(lookAtYaw(3, 2, -1, 2, yaw) && std::abs(std::abs(yaw) - 180.0) < 1e-12, "E19 look straight -x = +-180 deg");
    check(lookAtYaw(0, 0, 0, -5, yaw) && std::abs(yaw + 90.0) < 1e-12, "E19 look -y = -90 deg");
    check(lookAtYaw(-4, 7, -4, 7, yaw) == false, "E19 coincident target is not valid");
    check(lookAtYaw(0, 0, std::nan(""), 1, yaw) == false, "E19 NaN target is not valid");
    // --- orientedAzimuth
    check(std::abs(orientedAzimuth(OrientMode::LookAt, 0, 0, true, 135, 90) - (-135.0)) < 1e-12, "E19 look at 135 + offset 90 wraps to -135");
    check(std::abs(orientedAzimuth(OrientMode::LookAt, 17, 0, true, 135, 0) - 152.0) < 1e-12, "E19 look at adds the manual azimuth (acts as the offset)");
    check(std::abs(orientedAzimuth(OrientMode::LookAt, 0, 0, true, -170, -30) - 160.0) < 1e-12, "E19 look at -170 - 30 wraps to +160");
    check(orientedAzimuth(OrientMode::LookAt, 17, 0, false, 99, 90) == 17.0, "E19 invalid look target holds the manual value");
    check(orientedAzimuth(OrientMode::Manual, 17, 45, true, 99, 90) == 17.0, "E19 manual mode ignores everything");
    check(std::abs(orientedAzimuth(OrientMode::AlongPath, 10, 45, false, 0, 90) - 145.0) < 1e-12, "E19 along path adds the offset to manual + tangent");
    bool bitSame = true;
    for (int i = 0; i < 2000; ++i) {
        const double m = std::fmod(i * 37.3125, 720.0) - 360.0, t = std::fmod(i * 11.77, 360.0) - 180.0;
        const double a = orientedAzimuth(OrientMode::AlongPath, m, t, false, 0, 0.0), b = std::remainder(m + t, 360.0);
        bitSame &= std::memcmp(&a, &b, sizeof a) == 0;
    }
    check(bitSame, "E19 along path with offset 0 is bit-identical to the pre-E19 expression");
    MovementPath legacy; legacy.orient = true;
    check(orientModeOf(legacy) == OrientMode::AlongPath && orientModeOf(MovementPath{}) == OrientMode::Manual, "E19 orient=true maps to Along path");
    MovementPath bad; bad.orientOffset = 400; check(!validPath(bad), "E19 offset outside +-360 rejected");
    bad = {}; bad.lookTarget = 33; check(!validPath(bad), "E19 target index above listener rejected");
    bad = {}; bad.lookPos = {0, 0, 99}; check(!validPath(bad), "E19 look point outside the room bounds rejected");
    bad = {}; bad.orientOffset = std::nan(""); check(!validPath(bad), "E19 NaN offset rejected");

    // --- persistence: new fields round-trip; defaults are not written
    VerbProcessor p; p.setPlayConfigDetails(2, 2, 48000, 128); p.prepareToPlay(48000, 128);
    check(waitReady(p, 90), "E19 processor ready");
    MovementPath lisPath;  // listener without a path looks at source 0
    lisPath.lookAt = true; lisPath.lookTarget = 0; lisPath.orientOffset = 30.5;
    check(validPath(lisPath) && p.setMovementPath(kListener, lisPath), "E19 orientation-only slot (no points) accepted");
    MovementPath srcPath; srcPath.points = {PathPoint{2, 3, 1.5}, PathPoint{4, 3, 1.5}}; srcPath.lookAt = true; srcPath.lookPoint = true; srcPath.lookPos = {7.25, 1.5, 1.0};
    check(p.setMovementPath(0, srcPath), "E19 source path with a look-at point accepted");
    MovementPath oldPath; oldPath.points = {PathPoint{1, 1, 1.5}, PathPoint{3, 1, 1.5}}; oldPath.orient = true;
    check(p.setMovementPath(1, oldPath), "E19 legacy orient path accepted");
    juce::MemoryBlock state; p.getStateInformation(state);
    auto xml = p.getXmlFromBinary(state.getData(), (int)state.getSize());
    check(xml != nullptr, "E19 state parses");
    if (xml) {
        auto* paths = xml->getChildByName("MovementPaths");
        check(paths != nullptr && paths->getNumChildElements() == 3, "E19 three path nodes written (listener slot without points included)");
        bool legacyClean = false;
        if (paths) for (auto* e : paths->getChildIterator()) if (e->getIntAttribute("object") == 1) legacyClean = !e->hasAttribute("lookAt") && !e->hasAttribute("orientOffset") && !e->hasAttribute("lookTarget") && !e->hasAttribute("lookPoint") && e->getBoolAttribute("orient");
        check(legacyClean, "E19 legacy orient path is written without any E19 attribute");
    }
    p.setStateInformation(state.getData(), (int)state.getSize());
    check(waitReady(p, 90), "E19 reload ready");
    check(p.getMovementPath(kListener) == lisPath, "E19 listener look-at target + offset survive reload");
    check(p.getMovementPath(0) == srcPath, "E19 source look-at point survives reload");
    check(p.getMovementPath(1) == oldPath && p.getMovementPath(1).orient && !p.getMovementPath(1).lookAt, "E19 legacy orient=true reloads as Along path");
    p.releaseResources();
    return fails == before ? 0 : 1;
}

// S2: orientation applied by the audio thread.
static int testE19Audio() {
    const int before = fails;
    using namespace verbp;
    VerbProcessor p; p.setPlayConfigDetails(4, 2, 48000, 128); p.prepareToPlay(48000, 128);
    check(waitReady(p, 90), "E19a processor ready");
    {
        auto cfg = p.getSources(); cfg.resize(2);
        cfg[0].name = "A"; cfg[0].firstIn = 0; cfg[0].format = 0;
        cfg[1].name = "B"; cfg[1].firstIn = 1; cfg[1].format = 0;
        p.setSources(cfg);
        check(waitReady(p, 90), "E19a two sources");
    }
    juce::AudioBuffer<float> buf(4, 128); juce::MidiBuffer midi;
    auto block = [&]() { buf.clear(); p.processBlock(buf, midi); };
    auto az = [&](int o) { return (double)p.liveAzimuth(o); };
    auto closeTo = [](double a, double b, double tol = 1e-3) { return std::abs(std::remainder(a - b, 360.0)) < tol; };
    setP(p, "lx", 4); setP(p, "ly", 4); setP(p, "laz", 10);
    setP(p, "s1x", 7); setP(p, "s1y", 8); setP(p, "s1az", 33);
    setP(p, "s2x", 1); setP(p, "s2y", 4);
    block(); block();
    check(closeTo(az(kListener), 10) && closeTo(az(0), 33), "E19a manual azimuths are untouched");
    MovementPath m; m.lookAt = true; m.lookTarget = 0;  // listener -> source 1: dx=3 dy=4
    check(p.setMovementPath(kListener, m), "E19a listener look at source accepted");
    block(); block();
    check(closeTo(az(kListener), std::atan2(4.0, 3.0) * 180 / 3.14159265358979323846 + 10), "E19a listener -> source azimuth (+ manual 10 acts as the offset)");
    m.orientOffset = 90; p.setMovementPath(kListener, m); block(); block();
    check(closeTo(az(kListener), std::atan2(4.0, 3.0) * 180 / 3.14159265358979323846 + 100), "E19a listener -> source with offset 90 (+ manual 10)");
    m.lookTarget = 1; m.orientOffset = 0; p.setMovementPath(kListener, m); block(); block();
    check(closeTo(az(kListener), -170.0), "E19a listener -> source 2 straight -x = 180, + manual 10 wraps to -170");
    MovementPath s; s.lookAt = true; s.lookTarget = kListener;  // source 1 -> listener: from (7,8) to (4,4)
    p.setMovementPath(0, s); block(); block();
    check(closeTo(az(0), std::remainder(std::atan2(-4.0, -3.0) * 180 / 3.14159265358979323846 + 33, 360.0)), "E19a source -> listener azimuth (+ manual 33)");
    s.lookTarget = 1; p.setMovementPath(0, s); block(); block();
    check(closeTo(az(0), std::remainder(std::atan2(-4.0, -6.0) * 180 / 3.14159265358979323846 + 33, 360.0)), "E19a source -> source azimuth (+ manual 33)");
    s.lookTarget = 0; p.setMovementPath(0, s); block(); block();
    check(closeTo(az(0), 33), "E19a source cannot look at itself: manual value held");
    s.lookTarget = 31; p.setMovementPath(0, s); block(); block();
    check(closeTo(az(0), 33), "E19a missing target: manual value held");
    s.lookTarget = -1; s.lookPoint = true; s.lookPos = {7, 20, 1}; p.setMovementPath(0, s); block(); block();
    check(closeTo(az(0), 123), "E19a source -> fixed point azimuth (+ manual 33)");
    s.lookPos = {7, 8, 3}; p.setMovementPath(0, s); block(); block();
    check(closeTo(az(0), 33) && std::isfinite(az(0)), "E19a coincident point (plan) holds manual, no NaN");
    // Look at a moving target: source 1 moves along y; listener (static) follows it
    MovementPath mv; mv.points = {PathPoint{7, 2, 1.5}, PathPoint{7, 12, 1.5}};
    p.setMovementPath(0, mv);
    m.lookTarget = 0; m.orientOffset = 0; p.setMovementPath(kListener, m);
    bool tracks = true;
    for (double u : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        setP(p, VerbProcessor::pathParamId(0), u); block(); block();
        tracks &= closeTo(az(kListener), std::atan2(2.0 + 10.0 * u - 4.0, 3.0) * 180 / 3.14159265358979323846 + 10);
    }
    check(tracks, "E19a static listener follows a moving source");
    // Along path + offset 90 on a moving listener
    MovementPath along; along.points = {PathPoint{2, 2, 1.5}, PathPoint{8, 2, 1.5}}; along.orient = true; along.orientOffset = 90;
    p.setMovementPath(kListener, along); setP(p, VerbProcessor::pathParamId(kListener), .5); block(); block();
    check(closeTo(az(kListener), 10 + 0 + 90), "E19a along path (+x tangent) + offset 90 on top of manual 10");
    p.releaseResources();
    return fails == before ? 0 : 1;
}

// S3: bake validity. Table vs full reference trace at exact look-at / along-path orientations (gate: 1 dB).
struct E19Play final : juce::AudioPlayHead {
    bool playing = false;
    juce::Optional<PositionInfo> getPosition() const override { PositionInfo q; q.setIsPlaying(playing); return q; }
};
static double e19MaxErrDb(const std::vector<double>& pred, const std::vector<double>& ref, int S, int R) {
    double worst = 0.0;
    for (int r = 0; r < R; ++r)
        for (int b = 0; b < verb::NB; ++b) {
            double peak = 0, sp = 0, sr = 0;
            for (int s = 0; s < S; ++s) peak = std::max(peak, ref[((size_t)s * R + r) * verb::NB + b]);
            for (int s = 0; s < S; ++s) {
                const size_t k = ((size_t)s * R + r) * verb::NB + b;
                sp += pred[k]; sr += ref[k];
                if (ref[k] >= peak * 1e-4 && peak > 0) worst = std::max(worst, std::abs(10 * std::log10(std::max(1e-30, pred[k]) / std::max(1e-30, ref[k]))));
            }
            if (sr > 0) worst = std::max(worst, std::abs(10 * std::log10(std::max(1e-30, sp) / sr)));
        }
    return worst;
}
template <class Configure>
static void e19Accuracy(const char* label, Configure configure) {
    using namespace verbp;
    VerbProcessor p; p.setTailSettings({12000, true}); p.setPlayConfigDetails(4, 2, 48000, 128); p.prepareToPlay(48000, 128);
    check(waitReady(p, 90), (std::string(label) + ": ready").c_str());
    auto cfg = p.getSources(); cfg.resize(2);
    cfg[0].name = "A"; cfg[0].firstIn = 0; cfg[0].format = 0; cfg[1].name = "B"; cfg[1].firstIn = 1; cfg[1].format = 0;
    p.setSources(cfg); check(waitReady(p, 90), (std::string(label) + ": sources").c_str());
    setP(p, "lx", 3.5); setP(p, "ly", 2.0); setP(p, "lz", 1.5); setP(p, "laz", 0);
    setP(p, "s1x", 2.0); setP(p, "s1y", 5.0); setP(p, "s1z", 1.5); setP(p, "s2x", 5.0); setP(p, "s2y", 5.5); setP(p, "s2z", 1.5);
    juce::AudioBuffer<float> buf(4, 128); juce::MidiBuffer midi;
    auto block = [&]() { buf.clear(); p.processBlock(buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(1); };
    configure(p);
    block(); block();
    check(p.startPathBake(-1), (std::string(label) + ": bake accepted").c_str());
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 150000;
    while (p.pathBakePercent() < 100 && juce::Time::getMillisecondCounterHiRes() < deadline) { block(); juce::Thread::sleep(2); }
    check(p.pathBakePercent() == 100, (std::string(label) + ": bake completes").c_str());
    auto engine = E9bAccess::engine(p);
    const auto tables = engine->bakedTablesSnapshot();
    const auto scSnap = engine->sceneSnapshot();
    check(!tables.empty() && scSnap != nullptr, (std::string(label) + ": tables available").c_str());
    if (tables.empty() || !scSnap) return;
    const verb::Scene& sc = *scSnap;
    const int S = engine->numSources(), R = engine->numReceivers();
    double worst = 0;
    for (double u : {0.1, 0.35, 0.6, 0.9}) {
        for (int o = 0; o <= kMaxSources; ++o) if (p.getMovementPath(o).enabled && p.getMovementPath(o).points.size() >= 2) setP(p, VerbProcessor::pathParamId(o), u);
        for (int i = 0; i < 3; ++i) block();
        std::array<double, 4> lst{}; std::vector<std::array<double, 4>> src((size_t)S);
        auto pose = [&](int o) {
            std::array<double, 4> v{};
            const auto mp = p.getMovementPath(o);
            if (mp.enabled && mp.points.size() >= 2) { CompiledPath c; c.build(mp); const auto q = c.evaluate(u); v = {q[0], q[1], q[2], 0}; }
            else { const auto id = [&](int k) { return paramId(o, k); }; for (int k = 0; k < 3; ++k) v[(size_t)k] = p.apvts.getRawParameterValue(id(k))->load(); }
            v[3] = p.liveAzimuth(o);
            return v;
        };
        lst = pose(kListener); for (int s = 0; s < S; ++s) src[(size_t)s] = pose(s);
        verb::Vec3 origin = sc.hasListener ? sc.listenerPos : verb::Vec3{};
        auto rc = sc.receivers; const double dyaw = lst[3] - sc.listenerYaw, cy = std::cos(dyaw * verb::kPi / 180), sy = std::sin(dyaw * verb::kPi / 180);
        for (auto& r : rc) { const verb::Vec3 d = r.pos - origin; r.pos = {lst[0] + d.x * cy - d.y * sy, lst[1] + d.x * sy + d.y * cy, lst[2] + d.z}; r.azimuthDeg += dyaw; }
        std::vector<double> pred((size_t)S * R * verb::NB, 0.0), ref(pred.size(), 0.0);
        std::vector<uint8_t> covered((size_t)S, 0);
        for (const auto& t : tables) {
            std::vector<double> scratch(t->stride());
            if (!t->lookup(u, scratch.data())) continue;
            for (size_t si = 0; si < t->groupSources.size(); ++si) {
                const int s = t->groupSources[si];
                if (s < 0 || s >= S) continue;
                for (int r = 0; r < R; ++r) for (int b = 0; b < verb::NB; ++b) pred[((size_t)s * R + r) * verb::NB + b] = scratch[(si * (size_t)R + r) * verb::NB + b];
                covered[(size_t)s] = 1;
            }
        }
        for (int s = 0; s < S; ++s) {
            verb::Transducer t = sc.sources[(size_t)s];
            t.pos = {src[(size_t)s][0], src[(size_t)s][1], src[(size_t)s][2]}; t.azimuthDeg = src[(size_t)s][3];
            const auto e = verb::traceTailEnergy(sc, t, rc, 1, 16000, sc.seed + 131 * s);
            for (int r = 0; r < R; ++r) for (int b = 0; b < verb::NB; ++b) {
                ref[((size_t)s * R + r) * verb::NB + b] = e[(size_t)r][(size_t)b];
                if (!covered[(size_t)s]) pred[((size_t)s * R + r) * verb::NB + b] = e[(size_t)r][(size_t)b];
            }
        }
        worst = std::max(worst, e19MaxErrDb(pred, ref, S, R));
    }
    std::printf("  [E19] %-40s tables=%zu traceErr=%.3f dB\n", label, tables.size(), worst);
    check(worst <= 1.0, (std::string(label) + ": table accuracy <= 1 dB").c_str());
}

static int testE19Bake() {
    const int before = fails;
    using namespace verbp;
    e19Accuracy("A listener along path + offset 90", [](VerbProcessor& p) {
        MovementPath m; m.points = {PathPoint{3, 2, 1.5}, PathPoint{5.5, 3.5, 1.5}}; m.orient = true; m.orientOffset = 90; p.setMovementPath(kListener, m); });
    e19Accuracy("B listener moves, looks at static source", [](VerbProcessor& p) {
        MovementPath m; m.points = {PathPoint{3, 2, 1.5}, PathPoint{5.5, 3.5, 1.5}}; m.lookAt = true; m.lookTarget = 0; p.setMovementPath(kListener, m); });
    e19Accuracy("C coupled group, listener looks at member", [](VerbProcessor& p) {
        MovementPath l; l.points = {PathPoint{3, 2, 1.5}, PathPoint{5, 3, 1.5}}; l.group = 0; l.lookAt = true; l.lookTarget = 0; p.setMovementPath(kListener, l);
        MovementPath s; s.points = {PathPoint{2, 5, 1.5}, PathPoint{5, 6, 1.5}}; s.group = 0; p.setMovementPath(0, s); });
    e19Accuracy("D static listener looks at moving source", [](VerbProcessor& p) {
        MovementPath s; s.points = {PathPoint{2, 5, 1.5}, PathPoint{5, 6, 1.5}}; p.setMovementPath(0, s);
        MovementPath l; l.lookAt = true; l.lookTarget = 0; p.setMovementPath(kListener, l); });
    return fails == before ? 0 : 1;
}

// S3b: Blocked hint for independent movers, recorded pass keeps the orientation.
static int testE19Blocked() {
    const int before = fails;
    using namespace verbp;
    auto ascii = [](const juce::String& t) { for (unsigned char c : t.toStdString()) if (c < 32 || c > 126) return false; return true; };
    {
        VerbProcessor p; p.setTailSettings({4000, false}); p.setPlayConfigDetails(4, 2, 48000, 128); p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "E19b ready");
        auto cfg = p.getSources(); cfg.resize(2);
        cfg[0].name = "A"; cfg[0].firstIn = 0; cfg[0].format = 0; cfg[1].name = "B"; cfg[1].firstIn = 1; cfg[1].format = 0;
        p.setSources(cfg); check(waitReady(p, 90), "E19b sources");
        MovementPath a; a.points = {PathPoint{2, 5, 1.5}, PathPoint{5, 6, 1.5}}; p.setMovementPath(0, a);
        MovementPath b; b.points = {PathPoint{5, 3, 1.5}, PathPoint{2, 3.5, 1.5}}; p.setMovementPath(1, b);
        MovementPath l; l.lookAt = true; l.lookTarget = 0; p.setMovementPath(kListener, l);
        juce::AudioBuffer<float> buf(4, 128); juce::MidiBuffer midi;
        p.startPathBake(-1);
        juce::String reason, state;
        const auto until = juce::Time::getMillisecondCounterHiRes() + 8000;
        while (juce::Time::getMillisecondCounterHiRes() < until) {
            buf.clear(); p.processBlock(buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
            const auto st = p.pathBakeStatus(); state = st["state"].toString(); reason = st["reason"].toString();
            if (state == "blocked") break;
        }
        check(state == "blocked", "E19b static listener looking at one of two independent movers is Blocked");
        check(reason.contains("Look at") && ascii(reason), "E19b blocked reason carries the ASCII Look at hint");
        check(reason.contains("Look at target moves independently: Listener looks at A;"), "E21C static listener -> moving source: reason names Listener and source A");
        std::printf("  [E19] blocked reason: %s\n", reason.toRawUTF8());
        // Two moving objects that look at each other without a group: conflict hint too
        MovementPath c = a; c.lookAt = true; c.lookTarget = 1; p.setMovementPath(0, c);
        p.setMovementPath(kListener, MovementPath{});
        const auto until2 = juce::Time::getMillisecondCounterHiRes() + 8000; state.clear(); reason.clear();
        while (juce::Time::getMillisecondCounterHiRes() < until2) {
            buf.clear(); p.processBlock(buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
            const auto st = p.pathBakeStatus(); state = st["state"].toString(); reason = st["reason"].toString();
            if (state == "blocked") break;
        }
        check(state == "blocked" && reason.contains("Look at") && ascii(reason), "E19b moving source looking at an uncoupled moving source is Blocked with the hint");
        {   // the status may still show the previous scenario's reason for a moment: wait for the new pair
            const auto untilPair = juce::Time::getMillisecondCounterHiRes() + 8000;
            while (!reason.contains("A looks at B;") && juce::Time::getMillisecondCounterHiRes() < untilPair) {
                buf.clear(); p.processBlock(buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
                reason = p.pathBakeStatus()["reason"].toString();
            }
            std::printf("  [E21C] A->B reason: %s\n", reason.toRawUTF8());
        }
        check(reason.contains("Look at target moves independently: A looks at B;"), "E21C moving source A -> moving source B: reason names the pair");
        c.group = 0; MovementPath b2 = b; b2.group = 0; p.setMovementPath(0, c); p.setMovementPath(1, b2);
        const auto until3 = juce::Time::getMillisecondCounterHiRes() + 8000; state.clear();
        while (juce::Time::getMillisecondCounterHiRes() < until3) {
            buf.clear(); p.processBlock(buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
            state = p.pathBakeStatus()["state"].toString();
            if (state != "blocked" && state.isNotEmpty()) break;
        }
        check(state != "blocked", "E19b the same pair inside one Group is no longer blocked");
        // Different point counts (the raw per-point time vectors differ in length, the time maps do not): still one Group with the same timing
        MovementPath c3 = c; c3.points = {PathPoint{2, 5, 1.5}, PathPoint{3.5, 5.6, 1.5}, PathPoint{5, 6, 1.5}}; c3.times = {std::nullopt, std::nullopt, std::nullopt};
        p.setMovementPath(0, c3);
        const auto until4 = juce::Time::getMillisecondCounterHiRes() + 8000; state.clear();
        while (juce::Time::getMillisecondCounterHiRes() < until4) {
            buf.clear(); p.processBlock(buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
            state = p.pathBakeStatus()["state"].toString();
            if (state != "blocked" && state.isNotEmpty()) break;
        }
        check(state != "blocked", "E19b Group members with different point counts and plain timing are not blocked (Look at inside one Group)");
        // E28 REQUIREMENT CHANGE (replaces the E19b/E27 expectation "different Start % in one Group is blocked"): the owner is now
        // baked indexed by the group Position, so a different time map in the same Group is no longer blocked.
        c3.start = 25.0; p.setMovementPath(0, c3);
        const auto until5 = juce::Time::getMillisecondCounterHiRes() + 8000; state.clear();
        while (juce::Time::getMillisecondCounterHiRes() < until5) {
            buf.clear(); p.processBlock(buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
            state = p.pathBakeStatus()["state"].toString();
            if (state != "blocked" && state.isNotEmpty()) break;
        }
        check(state != "blocked", "E28 (replaces E19b) Group members with different Start % are NOT blocked for Look at (u-indexed owner)");
        // Closed owner with several laps looking at an open one-lap target in the same Group (plain timing): baked unrolled, not blocked
        MovementPath c4 = c; c4.closed = true; c4.laps = 3; c4.group = 0; c4.start = 0.0;
        c4.points = {PathPoint{2, 5, 1.5}, PathPoint{3.5, 6.5, 1.5}, PathPoint{5, 5, 1.5}, PathPoint{3.5, 3.5, 1.5}};
        p.setMovementPath(0, c4); p.startPathBake(-1);
        const auto until6 = juce::Time::getMillisecondCounterHiRes() + 60000; state.clear(); reason.clear();
        while (juce::Time::getMillisecondCounterHiRes() < until6) {
            buf.clear(); p.processBlock(buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
            const auto st = p.pathBakeStatus(); state = st["state"].toString(); reason = st["reason"].toString();
            if (state == "blocked" || state == "baked" || state == "approximate" || state == "failed") break;
        }
        check(state == "baked" || state == "approximate", "E19b closed multi-lap owner looking at an open target in one Group (plain timing) bakes unrolled instead of being blocked");
        std::printf("  [E19] closed-laps owner state: %s %s\n", state.toRawUTF8(), reason.toRawUTF8());
        p.releaseResources();
    }
    {   // recorded pass with a Look at listener: orientation is part of the recorded state
        auto ptr = std::make_unique<VerbProcessor>(); auto& p = *ptr;
        p.setTailSettings({4000, false}); p.setPlayConfigDetails(4, 2, 48000, 128); p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "E19c ready");
        auto cfg = p.getSources(); cfg.resize(2);
        cfg[0].name = "A"; cfg[0].firstIn = 0; cfg[0].format = 0; cfg[1].name = "B"; cfg[1].firstIn = 1; cfg[1].format = 0;
        p.setSources(cfg); check(waitReady(p, 90), "E19c sources");
        MovementPath l; l.lookAt = true; l.lookTarget = 0; l.orientOffset = 20; p.setMovementPath(kListener, l); setP(p, "laz", 0);
        E19Play ph; p.setPlayHead(&ph);
        juce::AudioBuffer<float> b(4, 128); juce::MidiBuffer midi;
        auto tick = [&] { b.clear(); stage2Process(p, b, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(5); };
        auto engine = E9bAccess::engine(p);
        check(p.getEditorSession().run(juce::JSON::parse(R"({"op":"path.recordPass"})")).ok, "E19c record command accepted");
        const auto untilArm = juce::Time::getMillisecondCounterHiRes() + 3000.0;
        while (engine->recordedPassStats().state != 1 && juce::Time::getMillisecondCounterHiRes() < untilArm) tick();
        const auto allocBefore = stage2AudioAllocations;
        ph.playing = true;
        for (int i = 0; i < 90; ++i) {
            const double t = (double)i / 89.0;
            setP(p, "s1x", 2.0 + 3.0 * t); setP(p, "s1y", 5.0 + 1.0 * t);   // host-automated independent movers
            setP(p, "s2x", 5.0 - 2.0 * t); setP(p, "s2y", 3.0 + 0.5 * t);
            tick();
        }
        ph.playing = false; tick();
        const auto stats = engine->recordedPassStats();
        check(stats.state == 3 && stats.samples > 10, "E19c pass finished");
        check(stage2AudioAllocations == allocBefore, "E19c zero audio-thread allocations with Look at active");
        const auto data = engine->exportRecordedPass();
        double worstYaw = 0; size_t checked = 0;
        for (int k = 0; k < data.samples; ++k) {
            const double* o = &data.poses[(size_t)k * data.objects * 4];
            // object 0 = listener, object 1 = source 0 (engine point 0)
            double look = 0;
            if (lookAtYaw(o[0], o[1], o[4], o[5], look)) { const double dev = std::abs(std::remainder(o[3] - (look + 20.0), 360.0)); if (k < 6 || dev > 1.0) std::printf("    sample %d dev %.3f deg (stored %.2f look %.2f)\n", k, dev, o[3], look); worstYaw = std::max(worstYaw, dev); ++checked; }
        }
        std::printf("  [E19] recorded pass samples=%d listener yaw vs look-at+offset worst dev=%.4f deg\n", data.samples, worstYaw);
        check(checked > 10 && worstYaw < 1.0, "E19c recorded pass stores the computed Look at orientation (<1 deg)");
        check(p.startPathBake(-1), "E19c recorded pass bake accepted");
        const auto bakeDeadline = juce::Time::getMillisecondCounterHiRes() + 120000.0;
        while (p.pathBakePercent() < 100 && juce::Time::getMillisecondCounterHiRes() < bakeDeadline) { tick(); juce::Thread::sleep(2); }
        check(p.pathBakePercent() == 100, "E19c recorded pass bakes to 100%");
        check(p.pathBakeStatus()["name"].toString() == "Recorded pass", "E19c status names the recorded pass");
        p.releaseResources();
    }
    {   // E21 C: the blocked reason names the offending pair; non-ASCII names fall back to "Source N"
        VerbProcessor p; p.setTailSettings({4000, false}); p.setPlayConfigDetails(4, 2, 48000, 128); p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "E21C ready");
        auto cfg = p.getSources(); cfg.resize(2);
        cfg[0].name = "Guitar"; cfg[0].firstIn = 0; cfg[0].format = 0;
        cfg[1].name = std::string({(char)0xD0, (char)0x93, (char)0xD1, (char)0x96, (char)0xD1, (char)0x82}); cfg[1].firstIn = 1; cfg[1].format = 0;   // UTF-8 Cyrillic
        p.setSources(cfg); check(waitReady(p, 90), "E21C sources");
        MovementPath a; a.points = {PathPoint{2, 5, 1.5}, PathPoint{5, 6, 1.5}}; a.lookAt = true; a.lookTarget = 1; p.setMovementPath(0, a);
        MovementPath b; b.points = {PathPoint{5, 3, 1.5}, PathPoint{2, 3.5, 1.5}}; p.setMovementPath(1, b);
        juce::AudioBuffer<float> buf(4, 128); juce::MidiBuffer midi;
        p.startPathBake(-1);
        juce::String reason, state;
        const auto until = juce::Time::getMillisecondCounterHiRes() + 8000;
        while (juce::Time::getMillisecondCounterHiRes() < until) {
            buf.clear(); p.processBlock(buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
            const auto st = p.pathBakeStatus(); state = st["state"].toString(); reason = st["reason"].toString();
            if (state == "blocked") break;
        }
        std::printf("  [E21C] blocked reason: %s\n", reason.toRawUTF8());
        check(state == "blocked" && ascii(reason), "E21C blocked reason is ASCII even with a non-ASCII source name");
        check(reason.contains("Look at target moves independently: Guitar looks at Source 2;") && reason.contains("put both paths in one Group or record a pass"),
              "E21C reason names the pair: owner by name, non-ASCII target falls back to Source 2");
    }
    return fails == before ? 0 : 1;
}

// S4: commands, undo/redo, validation.
static int testE19Commands() {
    const int before = fails;
    using namespace verbp;
    VerbProcessor p; p.setPlayConfigDetails(4, 2, 48000, 128); p.prepareToPlay(48000, 128);
    check(waitReady(p, 90), "E19d ready");
    auto cfg = p.getSources(); cfg.resize(2); cfg[0].firstIn = 0; cfg[1].firstIn = 1; p.setSources(cfg); check(waitReady(p, 90), "E19d sources");
    auto& session = p.getEditorSession();
    auto run = [&](const char* json) { return session.run(juce::JSON::parse(json)); };
    const auto depth0 = session.undoDepth();
    check(run(R"({"op":"path.setFlags","object":32,"orientMode":"lookAt","lookTarget":1,"orientOffset":45})").ok, "E19d set Look at + target + offset in one command");
    auto m = p.getMovementPath(kListener);
    check(m.lookAt && !m.orient && !m.lookPoint && m.lookTarget == 1 && m.orientOffset == 45.0, "E19d command applied to the processor");
    check(session.undoDepth() == depth0 + 1, "E19d one undo step");
    check(!run(R"({"op":"path.setFlags","object":32,"lookTarget":32})").ok, "E19d an object cannot look at itself");
    check(!run(R"({"op":"path.setFlags","object":0,"lookTarget":9})").ok, "E19d missing source target rejected");
    check(!run(R"({"op":"path.setFlags","object":0,"orientOffset":500})").ok, "E19d offset out of range rejected");
    check(run(R"({"op":"path.setFlags","object":0,"orientMode":"lookAt","lookPos":[3,4,1.5]})").ok, "E19d source looks at a fixed point");
    check(p.getMovementPath(0).lookPoint && p.getMovementPath(0).lookPos[1] == 4.0, "E19d fixed point stored");
    check(run(R"({"op":"path.setFlags","object":0,"orient":true})").ok && p.getMovementPath(0).orient && !p.getMovementPath(0).lookAt, "E19d orient flag switches Look at off");
    check(session.undo().ok, "E19d undo"); check(p.getMovementPath(0).lookAt && !p.getMovementPath(0).orient, "E19d undo restores Look at");
    check(session.redo().ok && p.getMovementPath(0).orient, "E19d redo");
    check(run(R"({"op":"source.remove","index":0})").ok, "E19d remove source 1");
    check(p.getMovementPath(kListener).lookAt && p.getMovementPath(kListener).lookTarget == 0, "E19d Look at target follows the source list (source 2 became source 1)");
    check(run(R"({"op":"path.setFlags","object":32,"orientMode":"manual","orientOffset":0})").ok, "E19d back to manual");
    check(!p.getMovementPath(kListener).lookAt && p.getMovementPath(kListener).orientOffset == 0.0, "E19d manual mode clears Look at");
    p.releaseResources();
    return fails == before ? 0 : 1;
}
