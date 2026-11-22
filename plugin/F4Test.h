#pragma once

namespace f4test {
constexpr int sr = 48000, cycle = 8192;

struct Probe {
    std::unique_ptr<VerbProcessor> processor = std::make_unique<VerbProcessor>();
    VerbProcessor& p = *processor;
    E19Play host;
    juce::AudioBuffer<float> audio{2, 1024};
    juce::MidiBuffer midi;
    std::array<float, cycle> signal{};
    std::shared_ptr<verb::Engine> engine;
    int blockSize, sample = 0;
    bool feed = false;

    explicit Probe(int size) : blockSize(size) {
        uint32_t rng = 1;
        for (auto& v : signal) {
            rng = 1664525u * rng + 1013904223u;
            v = ((rng >> 8) / 8388608.f - 1.f) * .125f;
        }
        p.setPlayConfigDetails(2, 2, sr, blockSize);
        p.prepareToPlay(sr, blockSize);
        p.setKeepRoutingEnabled(false);
        p.setPlayHead(&host);
    }

    ~Probe() { p.setPlayHead(nullptr); }

    void block(std::ostream* save = nullptr, bool impulse = false) {
        audio.setSize(2, blockSize, false, false, true);
        audio.clear();
        if (feed) for (int i = 0; i < blockSize; ++i) audio.setSample(0, i, signal[(sample + i) % cycle]);
        if (impulse) audio.setSample(0, 0, 1.f);
        p.processBlock(audio, midi);
        sample += blockSize;
        if (save) for (int i = 0; i < blockSize; ++i) {
            const float stereo[] = {audio.getSample(0, i), audio.getSample(1, i)};
            save->write(reinterpret_cast<const char*>(stereo), sizeof(stereo));
        }
    }

    void pump(int samples) {
        for (int i = 0; i < samples; i += blockSize) block();
    }

    void alignSignal() {
        pump((cycle - sample % cycle) % cycle);
    }

    bool wait(const std::function<bool()>& predicate, int seconds = 25) {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 1000. * seconds;
        while (juce::Time::getMillisecondCounterHiRes() < deadline) {
            block();
            if (predicate()) return true;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
            juce::Thread::sleep(1);
        }
        return false;
    }

    void pose(double u, double az, bool grouped) {
        setP(p, "s1path", u);
        if (!grouped) setP(p, "lpath", u);
        setP(p, "s1az", std::remainder(az, 360.));
    }

    void solo(const char* id) {
        for (const char* name : {"soloDirect", "soloEarly", "soloTail"})
            setP(p, name, std::strcmp(name, id) == 0 ? 1. : 0.);
    }

    void capture(const std::string& tag, bool impulse = false, int length = cycle,
                 const std::function<void()>& afterBlock = {}) {
        std::ofstream out("out/F4-audio/" + tag + ".f32", std::ios::binary);
        for (int n = 0; n < length; n += blockSize) {
            block(&out, impulse && n == 0);
            if (afterBlock) afterBlock();
        }
        check(out.good(), "F4 stereo capture written");
    }
};

static bool open(Probe& q, const juce::String& preset) {
    juce::String error;
    if (!q.p.loadRoomPresetText(preset, error) || !waitReady(q.p, 60)) {
        std::printf("F4 load error: %s\n", error.toRawUTF8());
        return false;
    }
    q.engine = E9bAccess::engine(q.p);
    return q.engine != nullptr;
}

static void inspect(const juce::String& preset) {
    Probe probe(128);
    if (!open(probe, preset)) { check(false, "F4 owner preset opens"); return; }
    auto xml = juce::parseXML(preset);
    auto* state = p4test::verbStateOf(xml.get());
    auto* cache = state->getChildByName("BakeCache");
    juce::MemoryBlock raw;
    std::vector<verb::BakedTable> saved;
    const bool decoded = cache && raw.fromBase64Encoding(cache->getStringAttribute("data")) &&
        verb::bakeDeserializeTables(static_cast<const uint8_t*>(raw.getData()), raw.getSize(), saved);
    for (const auto& table : saved)
        std::printf("F4 stored table recorded=%d group=%d moves_listener=%d stations=%zu u=[%.8f,%.8f] free_yaw=%d\n",
                    table.path.recorded, table.path.group, table.movesListener, table.u.size(),
                    table.u.front(), table.u.back(), table.path.yawMatch.free);
    const auto source = probe.p.getMovementPath(0), listener = probe.p.getMovementPath(verbp::kListener);
    const auto config = probe.p.getSources().front();
    const auto quality = probe.p.getTailSettings();
    probe.wait([&]{ return probe.engine->bakeStats().restored > 0; }, 2);
    std::printf("F4 owner preset: source points=%zu group=%d orient=%d lookAt=%d; listener points=%zu group=%d orient=%d lookAt=%d; rays=%d adaptive=%d voice=%s amount=%.2f profile=%d restored=%d decoded=%d RecordedPass=%d\n",
                source.points.size(), source.group, source.orient, source.lookAt,
                listener.points.size(), listener.group, listener.orient, listener.lookAt,
                quality.rays, quality.adaptive, config.instrument.c_str(), config.instrumentAmount,
                config.directivity != nullptr, probe.engine->bakeStats().restored, decoded,
                state->getChildByName("RecordedPass") != nullptr);
    check(decoded && config.directivity != nullptr && quality.adaptive && quality.rays == 48000,
          "F4 owner preset, voice and VRBK decoded");
}

static double bearing(double x1, double y1, double x2, double y2) {
    return std::atan2(y2 - y1, x2 - x1) * 180. / verb::kPi;
}

static const char* reason(const verb::Engine::P7ProfileStats& now, const verb::Engine::P7ProfileStats& prev) {
    if (now.workerJumpTriggers > prev.workerJumpTriggers) return "jump";
    if (now.workerSettleTriggers > prev.workerSettleTriggers) return "settle";
    if (now.workerLegacyTriggers > prev.workerLegacyTriggers) return "legacy";
    return "none";
}

static void measureMotion(Probe& q, const std::string& tag, double face, bool grouped, bool step) {
    auto& e = *q.engine;
    const auto table = e.bakedTableSnapshot();
    const double back = face - 180.;
    q.feed = false;
    q.pose(0., back, grouped);
    const auto rewound = juce::Time::getMillisecondCounterHiRes();
    check(q.wait([&] {
        return juce::Time::getMillisecondCounterHiRes() - rewound >= 700. &&
            e.tailIsCurrent() && e.motionStats().transState == 0;
    }), "F4 start pose ready after rewind");
    q.solo("soloTail");
    q.feed = true;
    q.pump(2 * sr);
    q.alignSignal();
    const auto start = e.p7ProfileStats();
    const auto transitions = e.motionStats().transitionsDone;
    const auto motionStart = e.bakeStats().lookups;
    const auto legacyStart = e.motionStats().legacySwaps;
    uint64_t lastHits = 0;
    uint64_t lastSpanHits = 0;
    int off = 0;
    q.host.playing = true;
    constexpr int frames = 98304;
    for (int n = q.blockSize; n <= frames; n += q.blockSize) {
        const double u = double(n) / frames;
        const double az = back + (step ? (u < .5 ? 0. : 180.) : 180. * u);
        q.pose(u, az, grouped);
        const auto before = e.bakeStats().lookups;
        q.block();
        lastHits = e.bakeStats().lookups - before;
        if (n > frames - cycle) lastSpanHits += lastHits;
        if (table && lastHits != uint64_t(q.blockSize / 128))
            std::printf("F4_MISS %s phase=%.8f lookups=%llu/%d off=%d\n",
                        tag.c_str(), u, (unsigned long long)lastHits, q.blockSize / 128, e.recordedPassStats().offPass);
        off += e.recordedPassStats().offPass;
        if (n % 1024 == 0) juce::Thread::sleep(8);
    }
    q.host.playing = false;
    const auto atEnd = e.p7ProfileStats();
    const auto atEndHits = e.bakeStats().lookups;
    const auto atEndSwaps = e.motionStats().legacySwaps;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    double startMs = -1.;
    int afterFrames = 0;
    int startFrames = -1;
    auto observe = [&] {
        if (startFrames < 0 && std::strcmp(reason(e.p7ProfileStats(), atEnd), "none") != 0) {
            startFrames = afterFrames;
            startMs = juce::Time::getMillisecondCounterHiRes() - t0;
        }
    };

    for (int n = 0; n < cycle; n += q.blockSize) {
        q.block();
        afterFrames += q.blockSize;
        observe();
    }
    q.capture(tag + "-before", false, cycle, [&] {
        afterFrames += q.blockSize;
        observe();
    });
    const bool cleanBefore = startFrames < 0;
    check(cleanBefore, "F4 pre-rebuild audio captured before worker starts");
    const bool settled = q.wait([&] {
        afterFrames += q.blockSize;
        observe();
        const auto stats = e.motionStats();
        const bool updated = stats.transitionsDone > transitions ||
            (step && stats.legacySwaps > atEndSwaps && afterFrames >= 2 * sr);
        return e.tailIsCurrent() && updated && stats.transState == 0;
    });
    q.pump(4 * sr);
    q.alignSignal();
    q.capture(tag + "-after");

    double projected = 0.;
    bool endpoint = false, strict = false;
    std::array<double, 8> live{};
    if (table) {
        for (const auto& path : table->movingPaths) {
            auto last = path.tracePose(1.);
            std::copy(last.begin(), last.end(), live.begin() + (path.object < 0 ? 0 : 4));
        }
        live[7] = q.p.liveAzimuth(0);
        endpoint = table->path.recorded ? table->projectJoint(live.data(), projected) : table->projectDeclared(live.data(), projected);
        verb::BakedTable legacy = *table;
        for (auto& path : legacy.movingPaths) path.yawMatch = {};
        legacy.path.yawMatch = {};
        if (legacy.path.recorded) legacy.buildJointCurve();
        double legacyU = 0.;
        strict = table->path.recorded ? legacy.projectJoint(live.data(), legacyU) : legacy.projectDeclared(live.data(), legacyU);
        int legacyStations = 0;
        for (int i = 0; i <= 128; ++i) {
            const double u = i / 128.;
            for (const auto& path : table->movingPaths) {
                auto pose = path.tracePose(u);
                std::copy(pose.begin(), pose.end(), live.begin() + (path.object < 0 ? 0 : 4));
            }
            if (step || !table->path.recorded)
                live[7] = std::remainder(back + (step ? (u < .5 ? 0. : 180.) : 180. * u), 360.);
            legacyStations += table->path.recorded ? legacy.projectJoint(live.data(), legacyU) :
                                                   legacy.projectDeclared(live.data(), legacyU);
        }
        std::printf("F4_TABLE %s stations=%zu phase=%.9f matches=%d old_yaw_gate=%d old_gate_poses=%d/129 yaw_first=%.3f yaw_last=%.3f actual=%.3f off=%d hash=%llx\n",
            tag.c_str(), table->u.size(), projected, endpoint, strict,
            legacyStations, table->movingPaths.back().tracePose(0.)[3],
            table->movingPaths.back().tracePose(1.)[3], q.p.liveAzimuth(0),
            off, (unsigned long long)e.bakeStats().hash);
        check(endpoint && projected > .999 && lastSpanHits == cycle / 128 && !e.recordedPassStats().offPass,
              "F4 table covers endpoint and last 8192 samples");
        check(atEndHits != e.bakeStats().lookups, "F4 real lookups continue at rest");
    }
    const auto done = e.p7ProfileStats();
    std::printf("F4_MOTION %s hits=%llu/%d last=%llu last_span=%llu/64 during[settle=%llu jump=%llu legacy=%llu] post_reason=%s post[settle=%llu jump=%llu legacy=%llu] bake_wait_code=%d start_samples=%d start_wall_ms=%.2f transitions=%llu short_swaps=%llu retrace_ms=%.2f before_clean=%d\n",
        tag.c_str(), (unsigned long long)(atEndHits - motionStart), frames / 128, (unsigned long long)lastHits,
        (unsigned long long)lastSpanHits,
        (unsigned long long)(atEnd.workerSettleTriggers - start.workerSettleTriggers),
        (unsigned long long)(atEnd.workerJumpTriggers - start.workerJumpTriggers),
        (unsigned long long)(atEnd.workerLegacyTriggers - start.workerLegacyTriggers),
        reason(done, atEnd), (unsigned long long)(done.workerSettleTriggers - atEnd.workerSettleTriggers),
        (unsigned long long)(done.workerJumpTriggers - atEnd.workerJumpTriggers),
        (unsigned long long)(done.workerLegacyTriggers - atEnd.workerLegacyTriggers),
        e.bakeStats().waitReason, startFrames, startMs,
        e.motionStats().transitionsDone - transitions, e.motionStats().legacySwaps - legacyStart,
        e.motionStats().lastRetraceMs, cleanBefore);
    const auto completed = e.motionStats();
    const bool settleTransition = completed.transitionsDone == transitions + 1 &&
        done.workerSettleTriggers > atEnd.workerSettleTriggers;
    const bool endpointJump = step && completed.transitionsDone == transitions &&
        completed.legacySwaps > atEndSwaps && done.workerJumpTriggers > atEnd.workerJumpTriggers;
    check(settled && (settleTransition || endpointJump), "F4 exact tail reaches the endpoint");
    if (!step) check(done.workerJumpTriggers == atEnd.workerJumpTriggers, "F4 linear ramp does not trigger jump");
}

static void measureOrientation(Probe& q, const std::string& tag, double face, bool grouped) {
    auto& e = *q.engine;
    q.pose(1., face, grouped);
    q.feed = false;
    q.pump(2 * sr);
    check(q.wait([&]{ return e.tailIsCurrent() && e.motionStats().transState == 0; }), "F4 orientation starts settled");
    for (const char* solo : {"soloDirect", "soloEarly", "soloTail"}) {
        const bool tail = std::strcmp(solo, "soloTail") == 0;
        q.solo(solo);
        const auto prior = e.p7ProfileStats();
        q.pose(1., face - 180., grouped);
        q.feed = false;
        q.pump(2 * sr);
        check(q.wait([&]{ return e.p7ProfileStats().workerJumpTriggers > prior.workerJumpTriggers &&
                  e.tailIsCurrent() && e.motionStats().transState == 0; }), "F4 rear-facing reference settled");
        q.feed = tail;
        if (tail) {
            q.pump(4 * sr);
            q.alignSignal();
        }
        const auto initial = e.p7ProfileStats();
        for (int relative = 180; relative >= 0; relative -= 45) {
            // Add intermediate automation values instead of tripping the jump tracker.
            if (relative != 180) for (int angle = relative + 40; angle >= relative; angle -= 5) {
                q.pose(1., face - angle, grouped);
                q.block();
            }
            if (tail) q.alignSignal();
            else q.pump(2048);
            q.capture(tag + "-" + solo + "-" + std::to_string(relative), !tail);
        }
        const auto turned = e.p7ProfileStats();
        std::printf("F4_YAW %s %s bearing=%.4f actual=%.4f before_rebuild=%d\n", tag.c_str(), solo, face,
                    q.p.liveAzimuth(0), std::strcmp(reason(turned, initial), "none") == 0);
        check(std::strcmp(reason(turned, initial), "none") == 0, "F4 yaw sweep measures live DSP before a rebuild");
        check(q.wait([&]{ return e.p7ProfileStats().workerSettleTriggers > turned.workerSettleTriggers &&
                  e.tailIsCurrent() && e.motionStats().transState == 0; }), "F4 yaw-only movement schedules settle with XYZ fixed");
        if (tail) {
            q.pump(4 * sr);
            q.alignSignal();
            q.capture(tag + "-soloTail-0-settled");
        }
    }
}

static void compareReload(Probe& q, const std::string& tag, bool recorded) {
    auto reloaded = std::make_unique<Probe>(q.blockSize);
    bool ready = recorded ? p4test::loadStateXml(reloaded->p, p4test::stateXml(q.p)) :
                            open(*reloaded, q.p.getRoomPresetText());
    if (ready && !reloaded->engine) reloaded->engine = E9bAccess::engine(reloaded->p);
    check(ready && p4test::waitBaked(reloaded->p, 20) &&
          q.engine->bakeStats().hash == reloaded->engine->bakeStats().hash, "F4 table hash survives save/reload");
    if (!ready) return;
    auto& r = *reloaded;
    q.feed = false;
    check(r.wait([&] {
        return r.engine->motionStats().transitionsDone > 0 && r.engine->tailIsCurrent() &&
            r.engine->motionStats().transState == 0;
    }), "F4 restored endpoint reaches exact tail before impulse");
    for (Probe* x : {&q, &r}) {
        x->solo("soloTail");
        x->wait([&] { return x->engine->tailIsCurrent() && x->engine->motionStats().transState == 0; });
        x->pump(4 * sr);
    }
    std::printf("F4_RELOAD %s late hash=%llx reload=%llx yaw=%.4f reload=%.4f\n", tag.c_str(),
                (unsigned long long)q.engine->lateStateHash(), (unsigned long long)r.engine->lateStateHash(),
                q.p.liveAzimuth(0), r.p.liveAzimuth(0));
    q.capture(tag + "-save", true, 4 * cycle);
    r.capture(tag + "-reload", true, 4 * cycle);
}
} // namespace f4test

static int testF4() {
    using namespace f4test;
    const int before = fails;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    juce::File::getCurrentWorkingDirectory().getChildFile("out/F4-audio").createDirectory();
    const auto original = repoFile("test/test azimut.verbroom").loadFileAsString();
    check(original.isNotEmpty(), "F4 owner preset found");
    if (original.isEmpty()) return 1;
    inspect(original);

    for (int size : {128, 1024}) for (bool omni : {false, true}) for (int mode = 0; mode < 3; ++mode) {
        if (const char* buffer = std::getenv("VERB_F4_BUFFER"))
            if (std::atoi(buffer) != size) continue;
        if (const char* filter = std::getenv("VERB_F4_TAG")) {
            const std::string name = std::string(mode == 0 ? "none" : mode == 1 ? "group" : "record") +
                (omni ? "-omni-" : "-voice-") + std::to_string(size);
            if (name != filter) continue;
        }
        const bool grouped = mode != 2;
        auto xml = juce::parseXML(original);
        auto* state = p4test::verbStateOf(xml.get());
        state->deleteAllChildElementsWithTagName("BakeCache");
        if (omni) state->getChildByName("Sources")->getChildElement(0)->setAttribute("instrumentAmount", 0.);
        Probe q(size);
        if (!open(q, xml->toString())) { check(false, "F4 working preset opens"); continue; }
        auto listener = q.p.getMovementPath(verbp::kListener);
        auto source = q.p.getMovementPath(0);
        listener.group = grouped ? 0 : -1;
        source.group = listener.group;
        const auto& s = source.points.back();
        const auto& l = listener.points.back();
        const double face = bearing(s[0], s[1], l[0], l[1]);
        check(q.p.setMovementPath(0, source) && q.p.setMovementPath(verbp::kListener, listener), "F4 owner paths configured");
        q.pose(0., face - 180., grouped);
        q.pump(4096);
        if (mode == 2) {
            check(q.p.getEditorSession().run(juce::JSON::parse(R"({"op":"path.recordPass"})")).ok,
                  "F4 arm record command");
            check(q.wait([&] { return q.engine->recordedPassStats().state == 1; }), "F4 pass armed");
            q.host.playing = true;
            q.block();
            for (int n = size; n <= 98304; n += size) {
                const double u = double(n) / 98304.;
                q.pose(u, face - 180. + 180. * u, grouped);
                q.block();
            }
            q.pump(4096); // Include the true held endpoint, after interpolation across the last host block.
            q.host.playing = false;
            q.block();
            check(q.engine->exportRecordedPass().samples > 2, "F4 pass includes real motion");
            q.pose(0., face - 180., grouped);
            q.pump(4096);
        }
        if (mode != 0) {
            check(q.p.startPathBake(-1) && p4test::waitBaked(q.p, 180), "F4 path bake completes");
            if (!q.engine->bakedTableSnapshot()) continue;
        }
        const std::string tag = std::string(mode == 0 ? "none" : mode == 1 ? "group" : "record") +
            (omni ? "-omni-" : "-voice-") + std::to_string(size);
        measureMotion(q, tag + "-ramp", face, grouped, false);
        if (mode != 0) measureMotion(q, tag + "-turn", face, grouped, true);
        measureOrientation(q, tag, face, grouped);
        if (!omni && size == 128 && mode != 0) compareReload(q, tag, mode == 2);
    }
    return fails == before ? 0 : 1;
}
