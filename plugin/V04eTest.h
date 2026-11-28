#pragma once
// V04e: MIDI out of modulation routes (notes / CC) — generator unit tests + processor end-to-end, stuck-note safety,
// determinism, CPU/allocations, state compatibility.
#include <map>
#include <set>
#include "ModMidiOut.h"
#include "PathShapes.h"
#include "TriggerZones.h"

namespace v04etest {
struct Ev { int64_t t; int status, d1, d2; bool operator==(const Ev& o) const { return t == o.t && status == o.status && d1 == o.d1 && d2 == o.d2; } };
using Gen = verbp::ModMidiGenerator<verbp::kMaxModRoutes>;

inline verbp::ModMidiOut midiCfg(verbp::ModMidiMode mode, int ch, int num) {
    verbp::ModMidiOut m; m.mode = mode; m.channel = ch; m.number = num; return m;
}
// Feeds one route value per 128-sample period; returns events with absolute sample times.
struct Driver {
    Gen g;
    std::vector<Ev> ev;
    int64_t t0 = 0;
    void period(const std::vector<verbp::ModMidiRouteInput>& in, int len = 128) {
        g.beginBlock();
        g.process(in.data(), (int)in.size(), 0, len, 48000.0, [&](const verbp::ModMidiEvent& e) { ev.push_back({t0 + e.sample, e.status, e.d1, e.d2}); });
        t0 += len;
    }
    void releaseAll() { g.releaseAll([&](const verbp::ModMidiEvent& e) { ev.push_back({t0 + e.sample, e.status, e.d1, e.d2}); }, 0); }
};
inline int countKind(const std::vector<Ev>& ev, int kind, int d1 = -1) {
    int n = 0; for (const auto& e : ev) if ((e.status & 0xF0) == kind && (d1 < 0 || e.d1 == d1)) ++n; return n;
}
// Note-ons and note-offs balance and never double-on per (channel, note).
inline bool balanced(const std::vector<Ev>& ev, bool requireAllOff = true) {
    std::map<int, int> on;
    for (const auto& e : ev) {
        const int k = (e.status & 0x0F) * 128 + e.d1;
        if ((e.status & 0xF0) == 0x90) { if (on[k]) return false; on[k] = 1; }
        else if ((e.status & 0xF0) == 0x80) { if (!on[k]) return false; on[k] = 0; }
    }
    if (requireAllOff) for (auto& kv : on) if (kv.second) return false;
    return true;
}
inline std::set<int> sounding(const std::vector<Ev>& ev, int64_t upTo) {
    std::set<int> s;
    for (const auto& e : ev) {
        if (e.t > upTo) break;
        const int k = (e.status & 0x0F) * 128 + e.d1;
        if ((e.status & 0xF0) == 0x90) s.insert(k); else if ((e.status & 0xF0) == 0x80) s.erase(k);
    }
    return s;
}
inline void collect(const juce::MidiBuffer& midi, int64_t blockStart, std::vector<Ev>& out) {
    for (const auto meta : midi) {
        const auto* d = meta.data;
        out.push_back({blockStart + meta.samplePosition, d[0], meta.numBytes > 1 ? d[1] : 0, meta.numBytes > 2 ? d[2] : 0});
    }
}
inline verbp::ModulationRoute zoneRoute(verbp::ModDriver d, int zoneId, double inMin, double inMax, int object = 0) {
    verbp::ModulationRoute r;
    r.enabled = true; r.object = object; r.driver = d; r.zoneId = verbp::modDriverUsesZone(d) ? zoneId : 0;
    r.target = verbp::ModTarget::None; r.targetObject = -1; r.hostOut = 0;
    r.curve.inputMin = inMin; r.curve.inputMax = inMax; r.curve.outputMin = 0.0; r.curve.outputMax = 1.0;
    r.curve.nodeCount = 2; r.curve.nodes[0] = {0.0, 0.0, false}; r.curve.nodes[1] = {1.0, 1.0, false};
    r.attackMs = 0.0; r.releaseMs = 0.0;
    return r;
}
inline verbp::TriggerZone box(int id, double x0, double y0, double x1, double y1) {
    verbp::TriggerZone z; z.id = id; z.name = "Z" + std::to_string(id); z.polygon = false;
    z.points = {{x0, y0}, {x1, y1}}; z.zMin = 0.0; z.zMax = 3.0; z.edge = 0.0; z.hysteresis = 0.05;
    return z;
}
}  // namespace v04etest

static int testV04eMidiOut() {
    std::printf("=== V04e: MIDI out of modulation routes ===\n");
    const int beforeFails = fails;
    using namespace v04etest;
    using verbp::ModMidiMode;

    // ------------------------------------------------------------------------------------------- 1. generator units
    {   // Gate with hysteresis: oscillation +-0.02 around the threshold produces exactly one note, no chatter.
        auto cfg = midiCfg(ModMidiMode::Gate, 1, 60); cfg.threshold = 0.5; cfg.hysteresis = 0.05;
        auto cfg0 = cfg; cfg0.hysteresis = 0.0;
        Driver d, d0;
        for (int k = 0; k < 2000; ++k) {
            const double v = k < 10 ? 0.0 : 0.5 + 0.02 * std::sin(k * 0.7);
            d.period({{&cfg, true, v, 0.0, 1}});
            d0.period({{&cfg0, true, v, 0.0, 1}});
        }
        d.period({{&cfg, true, 0.0, 0.0, 1}});
        d0.period({{&cfg0, true, 0.0, 0.0, 1}});
        std::printf("  gate hysteresis 0.05, oscillation +-0.02 for 2000 periods: note-on %d note-off %d (hysteresis 0: on %d off %d)\n",
                    countKind(d.ev, 0x90), countKind(d.ev, 0x80), countKind(d0.ev, 0x90), countKind(d0.ev, 0x80));
        check(countKind(d.ev, 0x90) == 1 && countKind(d.ev, 0x80) == 1 && balanced(d.ev) && countKind(d0.ev, 0x90) > 100,
              "V04e gate: hysteresis removes chatter (1 note for an oscillating driver; without hysteresis > 100)");
        check(d.ev.size() == 2 && d.ev[0].t == 10 * 128 + 127 && d.ev[0].status == 0x90 && d.ev[0].d1 == 60 && d.ev[0].d2 == 100,
              "V04e gate: note-on stamped at the last sample of the crossing period, channel 1 note 60 velocity 100");
    }
    {   // Trigger length exact across periods and host blocks; retrigger is off+on, never two note-ons.
        auto cfg = midiCfg(ModMidiMode::Trigger, 3, 38); cfg.lengthMs = 100.0;  // 4800 samples
        Driver d;
        d.period({{&cfg, true, 0.0, 0.0, 1}});         // primes the level
        d.period({{&cfg, true, 1.0, 0.0, 1}});         // crossing -> on at 255
        for (int k = 0; k < 60; ++k) d.period({{&cfg, true, k < 5 ? 1.0 : 0.0, 0.0, 1}}, k % 3 == 0 ? 100 : 128);
        const bool lenOk = d.ev.size() == 2 && d.ev[1].t - d.ev[0].t == 4800 && d.ev[0].status == 0x92 && d.ev[1].status == 0x82;
        std::printf("  trigger 100 ms @ 48 kHz: on t=%lld off t=%lld length %lld samples (irregular 100/128 periods)\n",
                    d.ev.size() > 0 ? (long long)d.ev[0].t : -1LL, d.ev.size() > 1 ? (long long)d.ev[1].t : -1LL,
                    d.ev.size() > 1 ? (long long)(d.ev[1].t - d.ev[0].t) : -1LL);
        check(lenOk, "V04e trigger: note length exactly lengthMs (4800 samples), channel 3");
        Driver r;   // fast re-crossings while the one-shot is still sounding
        r.period({{&cfg, true, 0.0, 0.0, 1}});
        for (int k = 0; k < 20; ++k) r.period({{&cfg, true, (k % 4 < 2) ? 1.0 : 0.0, 0.0, 1}});
        r.releaseAll();
        check(balanced(r.ev) && countKind(r.ev, 0x90) == 5, "V04e trigger retrigger while sounding: note-off then note-on (no double note-on), 5 one-shots");
        Driver s;   // already above the threshold at start: a trigger does not fire for 'already inside'
        for (int k = 0; k < 10; ++k) s.period({{&cfg, true, 1.0, 0.0, 1}});
        check(s.ev.empty(), "V04e trigger: no one-shot when the value is already above the threshold at start/seek (fires on observed crossings only)");
    }
    {   // CC: change-only, at most one message per period, range lo..hi.
        auto cfg = midiCfg(ModMidiMode::CC, 2, 11); cfg.ccLo = 20; cfg.ccHi = 100;
        Driver d; int maxPerPeriod = 0; size_t prev = 0;
        for (int k = 0; k <= 4000; ++k) {
            const double v = k < 1000 ? k / 1000.0 : (k < 2000 ? 1.0 : (k < 3000 ? 0.5 + 0.0004 * std::sin(k * 0.3) : 0.0));
            d.period({{&cfg, true, v, 0.0, 1}});
            maxPerPeriod = std::max(maxPerPeriod, (int)(d.ev.size() - prev)); prev = d.ev.size();
        }
        std::set<int> values; bool monotonic = true; int last = -1;
        for (size_t i = 0; i < d.ev.size(); ++i) {
            if (d.ev[i].t > 1000 * 128) break;
            values.insert(d.ev[i].d2); monotonic &= d.ev[i].d2 > last; last = d.ev[i].d2;
        }
        std::printf("  CC 11 ch 2 range 20..100: %zu messages over 4001 periods (ramp gives %zu distinct values), max %d per period\n",
                    d.ev.size(), values.size(), maxPerPeriod);
        check(maxPerPeriod <= 1 && monotonic && values.size() == 81 && *values.begin() == 20 && *values.rbegin() == 100 && d.ev.back().d2 == 20,
              "V04e CC: sent only when the integer value changes, <= 1 message per control period, mapped lo..hi");
    }
    {   // Keyswitch / zone index with 3 zones (+ program change).
        auto cfg = midiCfg(ModMidiMode::Index, 4, 36); cfg.program = true;
        Driver d;
        for (double raw : {0.0, 1.0, 1.0, 2.0, 3.0, 3.0, 0.0, 2.0, 0.0}) d.period({{&cfg, true, 0.0, raw, 1}});
        std::vector<std::pair<int, int>> got; for (const auto& e : d.ev) got.push_back({e.status, e.d1});
        const std::vector<std::pair<int, int>> want = {{0x93, 36}, {0xC3, 1}, {0x83, 36}, {0x93, 37}, {0xC3, 2}, {0x83, 37}, {0x93, 38}, {0xC3, 3},
                                                       {0x83, 38}, {0xC3, 0}, {0x93, 37}, {0xC3, 2}, {0x83, 37}, {0xC3, 0}};
        check(got == want && balanced(d.ev), "V04e keyswitch (3 zones): index i holds note base+i-1, previous released first, program change = index");
    }
    {   // Two routes on the same note: merged (one note-on, note-off when the last holder releases). Channels isolated.
        auto a = midiCfg(ModMidiMode::Gate, 1, 64), b = midiCfg(ModMidiMode::Gate, 1, 64), c = midiCfg(ModMidiMode::Gate, 2, 64);
        Driver d;
        auto step = [&](double va, double vb, double vc) { d.period({{&a, true, va, 0, 1}, {&b, true, vb, 0, 2}, {&c, true, vc, 0, 3}}); };
        step(1, 0, 0); step(1, 1, 1); step(0, 1, 1); step(0, 0, 1); step(0, 0, 0);
        std::vector<std::pair<int, int64_t>> got; for (const auto& e : d.ev) got.push_back({e.status, e.t / 128});
        const std::vector<std::pair<int, int64_t>> want = {{0x90, 0}, {0x91, 1}, {0x80, 3}, {0x81, 4}};
        check(got == want && balanced(d.ev), "V04e same note from two routes is merged (no double note-on; off after the last holder); channel 1/2 isolated");
    }
    {   // Route deleted / identity changed / disabled while holding: released.
        auto a = midiCfg(ModMidiMode::Gate, 1, 60);
        Driver d;
        d.period({{&a, true, 1, 0, 1}});
        d.period({});                                  // route deleted
        d.period({{&a, true, 1, 0, 1}});
        d.period({{&a, true, 1, 0, 7}});               // identity (object/driver/zone) changed
        d.period({{&a, false, 1, 0, 7}});              // disabled / inactive
        auto moved = a; moved.number = 61;
        d.period({{&a, true, 1, 0, 7}});
        d.period({{&moved, true, 1, 0, 7}});           // note number changed while holding
        d.releaseAll();
        check(balanced(d.ev) && countKind(d.ev, 0x90) == 5 && countKind(d.ev, 0x80) == 5,
              "V04e route delete / identity change / disable / note change release the held note first");
    }

    // ------------------------------------------------------------------------------------------- 2. processor
    const auto circlePath = [](const verb::Scene& sc, double periodS) {
        verbp::PathShapeParams sp;
        sp.kind = verbp::PathShapeKind::Circle; sp.cx = 3.0; sp.cy = 4.0; sp.z = 1.5; sp.radiusX = 2.0; sp.nodes = 16;
        auto mp = verbp::generatePathShapeForRoom(sp, &sc, true).path;
        mp.endless = true; mp.endlessPeriodS = periodS;
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
    const auto zA = box(1, 4.6, 3.0, 5.6, 5.0), zB = box(2, 2.0, 5.4, 4.0, 6.4), zC = box(3, 0.4, 3.0, 1.4, 5.0);
    auto gateRoute = [&] { auto r = zoneRoute(verbp::ModDriver::InZone, 1, 0.0, 1.0); r.hostOut = 1; r.midi = midiCfg(ModMidiMode::Gate, 1, 60); return r; };
    auto indexRoute = [&] { auto r = zoneRoute(verbp::ModDriver::ZoneIndex, 0, 0.0, 3.0); r.midi = midiCfg(ModMidiMode::Index, 2, 36); return r; };
    auto trigRoute = [&] { auto r = zoneRoute(verbp::ModDriver::InZone, 2, 0.0, 1.0); r.midi = midiCfg(ModMidiMode::Trigger, 10, 42); r.midi.lengthMs = 50.0; return r; };
    auto ccRoute = [&] { auto r = zoneRoute(verbp::ModDriver::DistanceToListener, 0, 0.0, 6.0); r.midi = midiCfg(ModMidiMode::CC, 3, 1); return r; };
    // Set up a processor in Creative with the circle path and three zones; routes as given.
    auto setup = [&](std::unique_ptr<VerbProcessor>& pp, const std::vector<verbp::ModulationRoute>& routes) -> bool {
        if (!makeProc(2, pp)) return false;
        auto& p = *pp;
        setP(p, "mode", 1.0);
        const auto sc = p.getDisplayScene();
        return p.setMovementPath(0, circlePath(sc, 4.0)) && p.setTriggerZones({zA, zB, zC}) && p.setModulationRoutes(routes);
    };
    // Render `blocks` blocks from transport position `start`; returns events (absolute samples) and Mod Out 1 per block.
    auto render = [&](VerbProcessor& p, E12PlayHead& head, int blocks, std::vector<Ev>& ev, std::vector<float>* mo1 = nullptr, int blockSize = 128) {
        juce::AudioBuffer<float> audio(2, blockSize); juce::MidiBuffer midi; midi.ensureSize(4096);
        for (int b = 0; b < blocks; ++b) {
            audio.clear();
            stage2Process(p, audio, midi);
            collect(midi, head.samples, ev);
            if (mo1) mo1->push_back(p.modOutParameter(0)->getValue());
            if (head.playing) head.samples += blockSize;
        }
    };
    {
        std::unique_ptr<VerbProcessor> pp;
        check(setup(pp, {gateRoute(), indexRoute(), trigRoute(), ccRoute()}), "V04e processor with 4 MIDI routes (gate, index, trigger, CC) ready");
        auto& p = *pp;
        check(p.producesMidi() && !p.acceptsMidi() && !p.isMidiEffect() && p.getParameters().size() == 299
              && p.getBusCount(true) == 1 && p.getBusCount(false) == 1 && p.getTotalNumInputChannels() == 2 && p.getTotalNumOutputChannels() == 2,
              "V04e producesMidi=true, acceptsMidi=false, not a MIDI effect; 299 host parameters; audio buses unchanged");
        E12PlayHead head; head.playing = true; head.samples = 0;
        p.setPlayHead(&head);
        std::vector<Ev> ev; std::vector<float> mo1;
        const int blocks = (int)(8.0 * 48000 / 128);  // two laps of 4 s
        render(p, head, blocks, ev, &mo1);
        // Gate note 60 follows Mod Out 1 (In zone A) to the block.
        std::vector<int64_t> moRise, moFall, onBlk, offBlk;
        for (int b = 1; b < blocks; ++b) { if (mo1[(size_t)b] > 0.5f && mo1[(size_t)b - 1] <= 0.5f) moRise.push_back(b); if (mo1[(size_t)b] <= 0.5f && mo1[(size_t)b - 1] > 0.5f) moFall.push_back(b); }
        if (mo1[0] > 0.5f) moRise.insert(moRise.begin(), 0);
        for (const auto& e : ev) if ((e.status & 0x0F) == 0 && e.d1 == 60) ((e.status & 0xF0) == 0x90 ? onBlk : offBlk).push_back(e.t / 128);
        std::vector<Ev> chEv[16]; for (const auto& e : ev) chEv[e.status & 0x0F].push_back(e);
        std::set<int> keys; for (const auto& e : chEv[1]) if ((e.status & 0xF0) == 0x90) keys.insert(e.d1);
        int ccMsgs = 0, ccMaxPerBlock = 0; std::map<int64_t, int> ccPerBlock;
        for (const auto& e : chEv[2]) { ++ccMsgs; ccMaxPerBlock = std::max(ccMaxPerBlock, ++ccPerBlock[e.t / 128]); }
        int trigOn = countKind(chEv[9], 0x90), trigOff = countKind(chEv[9], 0x80); bool trigLen = true;
        for (size_t i = 0; i + 1 < chEv[9].size(); i += 2) trigLen &= chEv[9][i + 1].t - chEv[9][i].t == 2400;
        bool inBlock = true; for (const auto& e : ev) inBlock &= e.t >= 0;
        std::printf("  2 laps: gate 60 on blocks {"); for (auto b : onBlk) std::printf(" %lld", (long long)b);
        std::printf(" } Mod Out 1 rises {"); for (auto b : moRise) std::printf(" %lld", (long long)b);
        std::printf(" }; off {"); for (auto b : offBlk) std::printf(" %lld", (long long)b);
        std::printf(" } falls {"); for (auto b : moFall) std::printf(" %lld", (long long)b);
        std::printf(" }; keyswitch notes {"); for (int k : keys) std::printf(" %d", k);
        std::printf(" }; trigger on %d off %d; CC msgs %d (max %d per block); total events %zu\n", trigOn, trigOff, ccMsgs, ccMaxPerBlock, ev.size());
        check(onBlk == moRise && offBlk == moFall && onBlk.size() >= 2, "V04e end-to-end gate (In zone A -> note 60 ch 1): note-on/off in the same block as Mod Out 1 rises/falls (2 laps)");
        check(keys == std::set<int>({36, 37, 38}) && balanced(chEv[1], false), "V04e end-to-end keyswitch (Zone index, 3 zones) plays notes 36/37/38 on ch 2, balanced");
        check(trigOn >= 1 && trigOn == trigOff && trigLen, "V04e end-to-end trigger (zone B -> note 42 ch 10, 50 ms): each one-shot exactly 2400 samples");
        check(ccMsgs > 10 && ccMaxPerBlock <= 1, "V04e end-to-end CC (distance -> CC 1 ch 3): change-only, <= 1 message per 128-sample period");

        // Stuck-note safety: get the gate note held, then each event must release it.
        auto holdGate = [&]() -> bool {
            std::vector<Ev> e2;
            for (int i = 0; i < 2000; ++i) {
                render(p, head, 1, e2);
                if (sounding(e2, INT64_MAX).count(60)) return true;
            }
            return false;
        };
        auto releasedNow = [&](std::vector<Ev>& e2) { return countKind(e2, 0x80, 60) >= 1 && !sounding(e2, INT64_MAX).count(60); };
        std::map<std::string, bool> safety;
        {   // transport stop: note-off; nothing while stopped
            holdGate();
            head.playing = false;
            std::vector<Ev> e2; render(p, head, 50, e2);
            safety["stop"] = releasedNow(e2) && e2.size() == (size_t)countKind(e2, 0x80) && e2.front().t == head.samples;
            head.playing = true;
        }
        {   // seek (jump of 1.3 s): release at sample 0, then re-evaluated from the new pose
            holdGate();
            head.samples += (int64_t)(1.3 * 48000);
            std::vector<Ev> e2; render(p, head, 1, e2);
            for (const auto& e : e2) std::printf("    seek block event t=%lld (block start %lld) %02x %d %d\n", (long long)e.t, (long long)(head.samples - 128), e.status, e.d1, e.d2);
            safety["seek"] = !e2.empty() && e2.front().status == 0x80 && e2.front().d1 == 60 && e2.front().t == head.samples - 128 && countKind(e2, 0x90, 60) <= 1;
        }
        {   // switch to Physical
            holdGate();
            setP(p, "mode", 0.0);
            std::vector<Ev> e2; render(p, head, 100, e2);
            safety["physical"] = releasedNow(e2) && countKind(e2, 0x90) == 0;
            setP(p, "mode", 1.0);
        }
        {   // route deleted
            holdGate();
            auto routes = p.getModulationRoutes(); routes.erase(routes.begin());
            p.setModulationRoutes(routes);
            std::vector<Ev> e2; render(p, head, 2, e2);
            safety["delete"] = releasedNow(e2);
            p.setModulationRoutes({gateRoute(), indexRoute(), trigRoute(), ccRoute()});
        }
        {   // route disabled
            holdGate();
            auto routes = p.getModulationRoutes(); routes[0].enabled = false;
            p.setModulationRoutes(routes);
            std::vector<Ev> e2; render(p, head, 2, e2);
            safety["disable"] = releasedNow(e2) && countKind(e2, 0x90, 60) == 0;
            p.setModulationRoutes({gateRoute(), indexRoute(), trigRoute(), ccRoute()});
        }
        {   // state load (same project): released at sample 0 of the next block
            holdGate();
            juce::MemoryBlock st; p.getStateInformation(st);
            p.setStateInformation(st.getData(), (int)st.getSize());
            waitReady(p, 90);
            std::vector<Ev> e2; render(p, head, 1, e2);
            safety["stateLoad"] = !e2.empty() && e2.front().status == 0x80 && e2.front().d1 == 60;
        }
        {   // prepareToPlay / releaseResources
            holdGate();
            p.releaseResources(); p.prepareToPlay(48000, 128); waitReady(p, 90);
            std::vector<Ev> e2; render(p, head, 1, e2);
            safety["prepare"] = !e2.empty() && e2.front().status == 0x80 && e2.front().d1 == 60;
        }
        {   // host bypass (processBlockBypassed)
            holdGate();
            juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi; midi.ensureSize(4096);
            p.processBlockBypassed(audio, midi);
            std::vector<Ev> e2; collect(midi, head.samples, e2);
            juce::MidiBuffer midi2; p.processBlockBypassed(audio, midi2);
            safety["bypass"] = releasedNow(e2) && midi2.isEmpty();
        }
        std::printf("  stuck-note safety:");
        bool allSafe = true; for (auto& kv : safety) { std::printf(" %s=%s", kv.first.c_str(), kv.second ? "ok" : "FAIL"); allSafe &= kv.second; }
        std::printf("\n");
        check(allSafe && safety.size() == 8, "V04e stuck-note safety: stop / seek / Physical / route delete / disable / state load / prepare / bypass release held notes");
        p.setPlayHead(nullptr);
    }
    {   // Determinism: two fresh renders identical; seek == continuous after the seek block.
        auto run = [&](int64_t seekAt, std::vector<Ev>& ev, int blockSize) -> bool {
            std::unique_ptr<VerbProcessor> pp;
            if (!setup(pp, {gateRoute(), indexRoute(), trigRoute(), ccRoute()})) return false;
            auto& p = *pp;
            E12PlayHead head; head.playing = true; head.samples = 0;
            p.setPlayHead(&head);
            const int64_t total = (int64_t)(8.0 * 48000);
            if (seekAt > 0) {
                std::vector<Ev> pre; render(p, head, 200, pre, nullptr, blockSize);  // play elsewhere first
                head.samples = seekAt;
            }
            while (head.samples < total) render(p, head, 1, ev, nullptr, blockSize);
            p.setPlayHead(nullptr);
            return true;
        };
        std::vector<Ev> a, b, c, s;
        const int64_t seekAt = 777 * 128;
        const bool ok = run(0, a, 128) && run(0, b, 128) && run(0, c, 512) && run(seekAt, s, 128);
        std::vector<Ev> aAfter, sAfter;
        for (const auto& e : a) if (e.t >= seekAt + 128) aAfter.push_back(e);
        for (const auto& e : s) if (e.t >= seekAt + 128) sAfter.push_back(e);
        // Notes sounding at the end of the seek block must equal the continuous render (CC values may legitimately re-send).
        auto notesOnly = [](std::set<int> x) { return x; };
        const bool soundEq = notesOnly(sounding(a, seekAt + 127)) == notesOnly(sounding(s, seekAt + 127));
        std::vector<Ev> aN, sN; for (auto& e : aAfter) if ((e.status & 0xF0) != 0xB0) aN.push_back(e); for (auto& e : sAfter) if ((e.status & 0xF0) != 0xB0) sN.push_back(e);
        std::printf("  determinism: render A %zu events, render B %zu (identical: %s), 512-sample blocks %zu (identical: %s); seek at %lld: notes sounding equal %s, note events after the seek block %zu vs %zu (identical: %s)\n",
                    a.size(), b.size(), a == b ? "yes" : "no", c.size(), a == c ? "yes" : "no", (long long)seekAt, soundEq ? "yes" : "no", sN.size(), aN.size(), sN == aN ? "yes" : "no");
        check(ok && a == b && !a.empty(), "V04e offline determinism: two renders produce identical event lists");
        check(ok && a == c, "V04e host block size 128 vs 512: identical event list (control-period resolution, not block resolution)");
        check(ok && soundEq && sN == aN, "V04e seek == continuous: same sounding notes after the seek block and identical note events afterwards");
    }
    {   // No MIDI routes => buffer empty, also with Mod Out routes, and in Physical.
        std::unique_ptr<VerbProcessor> pp;
        auto plain = zoneRoute(verbp::ModDriver::InZone, 1, 0.0, 1.0); plain.hostOut = 1;
        check(setup(pp, {plain, zoneRoute(verbp::ModDriver::ZoneIndex, 0, 0.0, 3.0)}), "V04e processor without MIDI routes ready");
        auto& p = *pp;
        E12PlayHead head; head.playing = true; head.samples = 0;
        p.setPlayHead(&head);
        std::vector<Ev> ev; render(p, head, 1500, ev);
        setP(p, "mode", 0.0); render(p, head, 300, ev);
        // Even a junk incoming buffer comes back empty (no MIDI input / pass-through).
        juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer junk; junk.addEvent(juce::MidiMessage::noteOn(1, 70, (juce::uint8)90), 3);
        stage2Process(p, audio, junk);
        check(ev.empty() && junk.isEmpty(), "V04e routes without MIDI out: MIDI buffer empty every block (Creative and Physical; input not passed through)");
        auto withMidi = gateRoute(); setP(p, "mode", 0.0);
        p.setModulationRoutes({withMidi});
        std::vector<Ev> phys; render(p, head, 1500, phys);
        check(phys.empty(), "V04e Physical mode: a MIDI route sends nothing");
        p.setPlayHead(nullptr);
    }
    // ------------------------------------------------------------------------------------------- 3. CPU / allocations
    {
        auto bench = [&](bool midiOn, double& meanUs, double& p99Us, int64_t& allocs, int& grew, uint64_t& events) -> bool {
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
            std::vector<verbp::TriggerZone> zs;
            for (int i = 0; i < 8; ++i) zs.push_back(box(i + 1, 0.3 + 0.7 * i, 0.5, 0.8 + 0.7 * i, 7.5));
            if (!p.setTriggerZones(zs)) return false;
            std::vector<verbp::ModulationRoute> routes;
            for (int i = 0; i < 16; ++i) {
                const int s = i % 8;
                verbp::ModulationRoute r = i < 8 ? zoneRoute(verbp::ModDriver::InZone, 1 + i, 0.0, 1.0, s)
                                                 : (i < 12 ? zoneRoute(verbp::ModDriver::ZoneIndex, 0, 0.0, 8.0, s) : zoneRoute(verbp::ModDriver::DistanceToListener, 0, 0.0, 6.0, s));
                r.hostOut = i + 1; r.attackMs = 5; r.releaseMs = 20;
                if (midiOn) r.midi = i < 4 ? midiCfg(ModMidiMode::Gate, 1 + i, 60) : i < 8 ? midiCfg(ModMidiMode::Trigger, 1 + i, 40) : i < 12 ? midiCfg(ModMidiMode::Index, 1 + i, 36) : midiCfg(ModMidiMode::CC, 1 + i, 1);
                routes.push_back(r);
            }
            if (!p.setModulationRoutes(routes)) return false;
            E12PlayHead head; head.playing = true; head.samples = 0;
            p.setPlayHead(&head);
            juce::AudioBuffer<float> audio(8, 128); juce::MidiBuffer midi; midi.ensureSize(2048);
            const auto* base = midi.data.begin(); int maxBytes = 0; int moved = 0;
            for (int w = 0; w < 50; ++w) { audio.clear(); stage2Process(p, audio, midi); head.samples += 128; }
            std::vector<double> us; us.reserve(3000);
            const auto a0 = stage2AudioAllocations; const auto e0 = p.midiEventsTotal();
            for (int b = 0; b < 3000; ++b) {
                for (int s = 0; s < 8; ++s) for (int i = 0; i < 128; ++i) audio.setSample(s, i, 0.1f * (float)std::sin((head.samples + i) * 0.03 * (s + 1)));
                const auto t0 = std::chrono::steady_clock::now();
                stage2Process(p, audio, midi);
                us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
                maxBytes = std::max(maxBytes, midi.data.size()); moved += midi.data.begin() != base && midi.data.size() > 0;
                head.samples += 128;
            }
            allocs = stage2AudioAllocations - a0;
            grew = (maxBytes > 2048 ? maxBytes : 0) + moved;
            std::printf("    (max MidiBuffer bytes per block %d of 2048 reserved)\n", maxBytes);
            events = p.midiEventsTotal() - e0;
            double sum = 0.0; for (double v : us) sum += v;
            meanUs = sum / (double)us.size();
            std::sort(us.begin(), us.end());
            p99Us = us[us.size() * 99 / 100];
            p.setPlayHead(nullptr);
            return true;
        };
        double mm0 = 1e9, qq0 = 1e9, mm1 = 1e9, qq1 = 1e9; int64_t aa0 = 0, aa1 = 0; int grew0 = 0, grew1 = 0; uint64_t ev0 = 0, ev1 = 0; bool ok = true;
        for (int rep = 0; rep < 2; ++rep) {
            double m0, q0, m1, q1; int64_t a0, a1; int g0, g1; uint64_t e0, e1;
            ok &= bench(false, m0, q0, a0, g0, e0); ok &= bench(true, m1, q1, a1, g1, e1);
            mm0 = std::min(mm0, m0); qq0 = std::min(qq0, q0); mm1 = std::min(mm1, m1); qq1 = std::min(qq1, q1);
            aa0 += a0; aa1 += a1; grew0 = std::max(grew0, g0); grew1 = std::max(grew1, g1); ev0 += e0; ev1 += e1;
        }
        std::printf("  CPU (8 sources Endless, 8 zones, 16 routes, block 128 @ 48 kHz, budget 2666.7 us): no MIDI mean %.2f us p99 %.2f us; 16 MIDI routes (4 gate, 4 trigger, 4 index, 4 CC) mean %.2f us p99 %.2f us; delta %+.2f us/block (%+.3f %% of budget); MIDI events %llu / %llu; allocations %lld / %lld; MidiBuffer reallocations %d / %d\n",
                    mm0, qq0, mm1, qq1, mm1 - mm0, 100.0 * (mm1 - mm0) / 2666.7, (unsigned long long)ev0, (unsigned long long)ev1, (long long)aa0, (long long)aa1, grew0, grew1);
        check(ok && aa0 == 0 && aa1 == 0 && grew1 == 0 && ev0 == 0 && ev1 > 100, "V04e 16 MIDI routes: 0 allocations, MidiBuffer never grows (host-sized 2048 B), events produced");
        check(ok && (mm1 - mm0) < 0.02 * 2666.7, "V04e MIDI generation costs < 2 % of the 128-sample block budget");
    }
    // ------------------------------------------------------------------------------------------- 4. state / preset / impulse
    {
        std::unique_ptr<VerbProcessor> pp;
        check(makeProc(2, pp), "V04e state processor ready");
        auto& p = *pp;
        p.setTriggerZones({zA, zB, zC});
        auto plain = zoneRoute(verbp::ModDriver::InZone, 1, 0.0, 1.0); plain.hostOut = 2;
        juce::MemoryBlock plainState;
        p.setModulationRoutes({plain});
        p.getStateInformation(plainState);
        auto xmlPlain = VerbProcessor::getXmlFromBinary(plainState.getData(), (int)plainState.getSize());
        bool noMidiChild = xmlPlain != nullptr;
        if (auto* mod = xmlPlain ? xmlPlain->getChildByName("Modulation") : nullptr)
            for (auto* rt : mod->getChildIterator()) noMidiChild &= rt->getChildByName("Midi") == nullptr;
        check(noMidiChild, "V04e route without MIDI out writes no <Midi> child (state layout identical to V04c)");
        auto g = gateRoute(); g.midi.velocityFromValue = true; g.midi.threshold = 0.4; g.midi.hysteresis = 0.1;
        auto ix = indexRoute(); ix.midi.program = true;
        auto tr = trigRoute(); auto cc = ccRoute(); cc.midi.ccLo = 10; cc.midi.ccHi = 90;
        const std::vector<verbp::ModulationRoute> routes = {g, ix, tr, cc, plain};
        check(p.setModulationRoutes(routes), "V04e five routes (4 with MIDI out) accepted");
        juce::MemoryBlock st; p.getStateInformation(st);
        const auto preset = p.getRoomPresetText();
        std::unique_ptr<VerbProcessor> qq;
        check(makeProc(2, qq), "V04e reload processor ready");
        auto& q = *qq;
        q.setStateInformation(st.getData(), (int)st.getSize());
        check(waitReady(q, 90) && q.getModulationRoutes() == routes, "V04e project state round-trip preserves every MIDI field");
        const auto ir = h2test::captureImpulse(q, 2, 0, 120, 128);
        const double eIr = h2test::channelEnergy(ir[0]) + h2test::channelEnergy(ir[1]);
        check(std::isfinite(eIr) && eIr > 1e-6, "V04e impulse after reloading a project with MIDI routes (Default Room)");
        juce::String err;
        std::unique_ptr<VerbProcessor> rr;
        check(makeProc(2, rr), "V04e preset processor ready");
        check(rr->loadRoomPresetText(preset, err) && waitReady(*rr, 90) && rr->getModulationRoutes() == routes, "V04e .verbroom export -> import preserves MIDI routes");
        // Damaged MIDI block: only the MIDI out is dropped, the route stays.
        auto xmlBad = VerbProcessor::getXmlFromBinary(st.getData(), (int)st.getSize());
        if (auto* mod = xmlBad ? xmlBad->getChildByName("Modulation") : nullptr)
            if (auto* rt = mod->getChildElement(0))
                if (auto* mi = rt->getChildByName("Midi")) mi->setAttribute("channel", 23);
        juce::MemoryBlock badState; if (xmlBad) VerbProcessor::copyXmlToBinary(*xmlBad, badState);
        std::unique_ptr<VerbProcessor> bb;
        check(makeProc(2, bb), "V04e damaged-MIDI processor ready");
        bb->setStateInformation(badState.getData(), (int)badState.getSize());
        const auto bad = bb->getModulationRoutes();
        check(waitReady(*bb, 90) && bad.size() == routes.size() && !bad[0].midi.enabled() && bad[1] == routes[1],
              "V04e invalid MIDI block (channel 23) drops only that MIDI out; the route and others load");
        // Legacy fixture loads unchanged.
        const auto legacyStateFile = juce::File::getCurrentWorkingDirectory().getChildFile("plugin/fixtures/stage2/legacy.state");
        if (legacyStateFile.existsAsFile()) {
            juce::MemoryBlock legData; legacyStateFile.loadFileAsData(legData);
            std::unique_ptr<VerbProcessor> lp;
            check(makeProc(2, lp), "V04e legacy processor ready");
            lp->setStateInformation(legData.getData(), (int)legData.getSize());
            bool none = true; for (const auto& r : lp->getModulationRoutes()) none &= !r.midi.enabled();
            check(waitReady(*lp, 90) && none, "V04e legacy.state loads unchanged (no MIDI routes)");
        }
        // Save the MIDI project for the old-build check (VerbWebVst3Smoke old-state mode).
        juce::File::getCurrentWorkingDirectory().getChildFile("V04e-midi-routes.state").replaceWithData(st.getData(), st.getSize());
        // VSL preset + MIDI routes: save, reload, impulse.
        const auto vslFile = juce::File::getCurrentWorkingDirectory().getChildFile("VSL.verbroom");
        if (vslFile.existsAsFile()) {
            juce::AudioProcessor::BusesLayout vl;
            vl.inputBuses.add(juce::AudioChannelSet::discreteChannels(64));
            vl.outputBuses.add(juce::AudioChannelSet::stereo());
            auto vp = std::make_unique<VerbProcessor>();
            vp->setPlayConfigDetails(64, 2, 48000, 128); vp->setBusesLayout(vl); vp->prepareToPlay(48000, 128);
            juce::String vErr;
            check(vp->loadRoomPresetText(vslFile.loadFileAsString(), vErr) && waitReady(*vp, 120), "V04e VSL preset loads");
            const auto vsc = vp->getDisplayScene();
            const auto lx = vsc.listenerPos.x, ly = vsc.listenerPos.y;
            auto vr1 = zoneRoute(verbp::ModDriver::ZoneIndex, 0, 0.0, 2.0); vr1.midi = midiCfg(ModMidiMode::Index, 1, 36);
            check(vp->setTriggerZones({box(1, lx - 3.0, ly - 2.0, lx - 1.0, ly + 2.0), box(2, lx + 1.0, ly - 2.0, lx + 3.0, ly + 2.0)})
                  && vp->setModulationRoutes({vr1}), "V04e VSL preset accepts zones and a MIDI keyswitch route");
            juce::MemoryBlock vs; vp->getStateInformation(vs);
            auto vr = std::make_unique<VerbProcessor>();
            vr->setPlayConfigDetails(64, 2, 48000, 128); vr->setBusesLayout(vl); vr->prepareToPlay(48000, 128);
            vr->setStateInformation(vs.getData(), (int)vs.getSize());
            check(waitReady(*vr, 120) && vr->getModulationRoutes().size() == 1 && vr->getModulationRoutes()[0].midi == vr1.midi, "V04e VSL project with a MIDI route reloads");
            const auto irV = h2test::captureImpulse(*vr, 64, 0, 120, 128);
            const double eV = h2test::channelEnergy(irV[0]) + h2test::channelEnergy(irV[1]);
            std::printf("  impulse energy after reload with MIDI routes: Default Room %.6g, VSL %.6g\n", eIr, eV);
            check(std::isfinite(eV) && eV > 1e-6, "V04e impulse after reloading the VSL project with a MIDI route");
        }
        {   // Classic editor opens a project with MIDI routes.
            auto editor = std::make_unique<VerbEditor>(q);
            editor->setSize(900, 600);
            editor->showPage(0, 0);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
            check(q.getModulationRoutes() == routes, "V04e classic editor opens a project with MIDI routes (no crash, routes kept)");
        }
        {   // C: classic editor minimal zone list (enable / visible / delete through the processor).
            auto zq = std::make_unique<VerbProcessor>();
            zq->setTriggerZones({box(1, 0.5, 0.5, 2.0, 2.0), box(2, 3.0, 0.5, 4.0, 2.0)});
            auto editor = std::make_unique<VerbEditor>(*zq);
            editor->setSize(1160, 760);
            editor->showPage(3, -1);
            auto& zl = editor->testZoneList();
            zl.refresh();
            std::printf("  classic zone list: rows %d, visible %d, bounds %s\n", zl.numRows(), (int)zl.isVisible(), zl.getBounds().toString().toRawUTF8());
            check(zl.numRows() == 2 && zl.isVisible() && zl.getHeight() >= 60, "V04e classic Space page lists the 2 zones");
            zl.testToggle(0, false);
            zl.testToggle(1, true);
            auto z = zq->getTriggerZones();
            check(z.size() == 2 && !z[0].enabled && z[0].visible && z[1].enabled && !z[1].visible, "V04e classic zone list toggles Enabled / Visible");
            zl.testDelete(0);
            z = zq->getTriggerZones();
            zl.refresh();
            check(z.size() == 1 && z[0].id == 2 && zl.numRows() == 1, "V04e classic zone list deletes a zone");
            editor.reset();
        }
    }
    return fails == beforeFails ? 0 : 1;
}
