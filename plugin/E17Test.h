#pragma once
#include <cmath>

struct E17PlayHead final : juce::AudioPlayHead {
    bool playing = false;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo p;
        p.setIsPlaying(playing);
        return p;
    }
};

static int testE17Plugin() {
    const int before = fails;
    auto isPureAscii = [](const juce::String& s) {
        const std::string raw = s.toStdString();
        for (unsigned char c : raw)
            if (c < 32 || c > 126) return false;
        return true;
    };

    juce::MemoryBlock persistedState;
    juce::MemoryBlock clearedState;
    int savedSamples = 0;

    {
        auto ptr = std::make_unique<VerbProcessor>();
        auto& p = *ptr;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "E17 plugin ready");

        E17PlayHead ph;
        p.setPlayHead(&ph);
        juce::AudioBuffer<float> b(2, 128);
        juce::MidiBuffer midi;
        auto tick = [&] {
            b.clear();
            stage2Process(p, b, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        };
        auto waitStatus = [&](auto pred) {
            const auto until = juce::Time::getMillisecondCounterHiRes() + 6000.0;
            while (juce::Time::getMillisecondCounterHiRes() < until) {
                tick();
                if (pred(p.pathBakeStatus())) return true;
                juce::Thread::sleep(2);
            }
            return pred(p.pathBakeStatus());
        };

        auto engine = E9bAccess::engine(p);
        const auto revision = p.getRevision();
        const auto undo = p.getEditorSession().undoDepth();
        auto clear = [&] {
            return p.getEditorSession().run(juce::JSON::parse(R"({"op":"path.recordClear"})")).ok
                && waitStatus([](const juce::var& st) {
                    return (int)st["passState"] == 0 && (int)st["passSamples"] == 0
                        && st["bakeSource"].toString() == "none";
                })
                && engine->exportRecordedPass().samples == 0;
        };
        auto arm = [&] {
            return p.getEditorSession().run(juce::JSON::parse(R"({"op":"path.recordPass"})")).ok
                && waitStatus([](const juce::var& st) { return (int)st["passState"] == 1; });
        };
        check(clear(), "F9 clear while idle is a no-op");
        check(arm() && clear(), "F9 clear while armed cancels before playback");
        check(arm(), "F9 arm for running-clear regression");
        ph.playing = true;
        tick();
        setP(p, "s1x", 1.5);
        tick();
        check(engine->recordedPassStats().state == 2 && clear(), "F9 clear stops and discards while playing");
        tick();
        check(engine->recordedPassStats().state == 0, "F9 playback cannot restart cleared capture");
        ph.playing = false;
        check(arm(), "F9 arm for unbaked recording-clear regression");
        ph.playing = true;
        for (int i = 0; i < 8; ++i) {
            setP(p, "s1x", 1.5 + 0.1 * i);
            tick();
        }
        check(p.getEditorSession().run(juce::JSON::parse(R"({"op":"path.recordStop"})")).ok
              && waitStatus([](const juce::var& st) {
                  return (int)st["passState"] == 3 && st["bakeSource"].toString() == "recorded";
              }), "F9 explicit Stop leaves a recording ready before Bake");
        ph.playing = false;
        check(p.pathBakePercent() == 0 && clear(), "F9 clear discards a finished recording before Bake");

        // DAW automation only: NO MovementPaths defined on any object!
        check(p.getEditorSession().run(juce::JSON::parse(R"({"op":"path.recordPass"})")).ok, "E17 record command accepted");
        const auto untilArm = juce::Time::getMillisecondCounterHiRes() + 3000.0;
        while (engine->recordedPassStats().state != 1 && juce::Time::getMillisecondCounterHiRes() < untilArm) tick();
        check(engine->recordedPassStats().state == 1, "E17 plugin armed waits for host");

        const auto alloc = stage2AudioAllocations;
        ph.playing = true;
        for (int i = 0; i < 90; ++i) {
            const double t = (double)i / 89.0;
            setP(p, "s1x", 2.0 + 1.5 * t);
            setP(p, "s1y", 2.0 + 0.8 * t);
            setP(p, "lx", 3.0 + 0.7 * t);
            setP(p, "ly", 2.8 + 0.4 * t);
            tick();
        }
        ph.playing = false;
        tick();
        const auto recStats = engine->recordedPassStats();
        savedSamples = recStats.samples;
        check(recStats.state == 3 && recStats.samples > 10, "E17 host stop finishes recorded pass with no MovementPaths");
        check(stage2AudioAllocations == alloc, "E17 zero audio allocations while recording");
        check(p.getRevision() == revision && p.getEditorSession().undoDepth() == undo, "E17 record action does not edit document/undo");
        check(p.pathBakePercent() == 0 && engine->bakeStats().state == verb::Engine::BakeState::NotBaked, "E17 recording does not auto-bake");
        check(waitStatus([](const juce::var& st) {
            return st["bakeSource"].toString() == "recorded" && st["recordingNote"].isVoid();
        }), "F9 recorded source available before Bake");

        // Persistence checks: state XML includes <RecordedPass>, .verbroom preset does NOT
        p.getStateInformation(persistedState);
        if (auto stateXml = juce::AudioProcessor::getXmlFromBinary(persistedState.getData(), (int)persistedState.getSize())) {
            check(stateXml->getChildByName("RecordedPass") != nullptr, "E17 state XML contains RecordedPass");
        } else {
            check(false, "E17 state XML parses");
        }
        const juce::String preset = p.getRoomPresetText();
        auto presetXml = juce::parseXML(preset);
        check(presetXml != nullptr && presetXml->getChildByName("VerbState") != nullptr
              && presetXml->getChildByName("VerbState")->getChildByName("RecordedPass") == nullptr,
              "E17 .verbroom preset excludes RecordedPass");

        // Bake recorded pass via plugin API with no MovementPaths (DAW automation)
        check(p.startPathBake(0) || p.startPathBake(-1), "E17 startPathBake accepts recorded pass without MovementPaths");
        check(p.getEditorSession().run(juce::JSON::parse(R"({"op":"path.bake","object":"all"})")).ok, "E17 EditorSession path.bake accepted for recorded pass");
        const auto allocBake = stage2AudioAllocations;
        const auto bakeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (p.pathBakePercent() < 100 && std::chrono::steady_clock::now() < bakeDeadline) {
            tick();
            juce::Thread::sleep(2);
        }
        check(p.pathBakePercent() == 100, "E17 plugin DAW-automation recorded pass bakes to 100%");
        check(stage2AudioAllocations == allocBake, "E17 zero audio allocations during bake and lookup");

        const bool diagOk = waitStatus([&](const juce::var& st) {
            return st["state"].toString() == "baked"
                && st["name"].toString() == "Recorded pass"
                && (int)st["passState"] == 3
                && (int)st["passSamples"] == savedSamples
                && isPureAscii(st["reason"].toString());
        });
        check(diagOk, "E17 pathBakeStatus reports Recorded pass baked cleanly in pure ASCII");

        // Move outside the table's joint pose coverage.
        setP(p, "lx", 8.5);
        setP(p, "ly", 8.5);
        const bool offOk = waitStatus([&](const juce::var& st) {
            return st["reason"].toString().contains("Live motion is outside the baked recording - estimating the tail")
                && isPureAscii(st["reason"].toString());
        });
        check(offOk, "F9 live motion mismatch warning is pure ASCII");

        setP(p, "lx", 3.7);
        setP(p, "ly", 3.2);
        check(waitStatus([](const juce::var& st) {
            return st["state"].toString() == "baked" && st["reason"].toString().isEmpty();
        }), "F9 recorded playback re-enters table coverage");
        p.getStateInformation(persistedState);
        auto bakedXml = juce::AudioProcessor::getXmlFromBinary(persistedState.getData(), (int)persistedState.getSize());
        check(bakedXml && bakedXml->getChildByName("RecordedPass") && bakedXml->getChildByName("BakeCache"),
              "F9 state persists both recorded poses and baked table");
        p.setPlayHead(nullptr);
        p.releaseResources();
    }

    // Reload the actual saved table and render an impulse through it.
    {
        auto qPtr = std::make_unique<VerbProcessor>();
        auto& q = *qPtr;
        q.setTailSettings({4000, false});
        q.setPlayConfigDetails(2, 2, 48000, 128);
        q.prepareToPlay(48000, 128);
        q.setStateInformation(persistedState.getData(), (int)persistedState.getSize());
        check(waitReady(q, 90), "E17 project with RecordedPass reloads cleanly");

        auto engQ = E9bAccess::engine(q);
        const auto ps = engQ->recordedPassStats();
        check(ps.state == 3 && ps.samples == savedSamples, "E17 reloaded project restores RecordedPass samples");
        juce::AudioBuffer<float> b(2, 128);
        juce::MidiBuffer midi;
        auto pump = [&] {
            b.clear();
            stage2Process(q, b, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        };
        auto await = [&](auto pred, int seconds = 6) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            while (!pred() && std::chrono::steady_clock::now() < deadline) pump();
            return pred();
        };
        check(await([&] { return q.pathBakePercent() == 100; }) && engQ->bakeStats().restored == 1
              && q.pathBakeStatus()["bakeSource"].toString() == "recorded",
              "F9 reloaded project restores recorded table and bake source");
        // Verify on-pass pose audio and faster-than-real-time non-realtime bounce in plugin
        setP(q, "s1x", 2.75);
        setP(q, "s1y", 2.40);
        setP(q, "lx", 3.35);
        setP(q, "ly", 3.00);
        q.setNonRealtime(true);
        double energySum = 0.0;
        bool allFinite = true;
        for (int k = 0; k < 40; ++k) {
            b.clear();
            if (k == 0) { b.setSample(0, 0, 0.5f); b.setSample(1, 0, 0.5f); }
            stage2Process(q, b, midi);
            for (int i = 0; i < 128; ++i) {
                const float l = b.getSample(0, i), r = b.getSample(1, i);
                allFinite &= std::isfinite(l) && std::isfinite(r);
                energySum += (double)l * l + (double)r * r;
            }
        }
        if (!(allFinite && energySum > 1e-8)) {
            std::printf("  [diag] E17 reload audio: loadState=%d finite=%d energySum=%.6e lx=%.2f s1x=%.2f reason=%s\n",
                        q.getLoadState(), (int)allFinite, energySum,
                        (double)q.apvts.getRawParameterValue("lx")->load(),
                        (double)q.apvts.getRawParameterValue("s1x")->load(),
                        q.pathBakeStatus()["reason"].toString().toRawUTF8());
        }
        check(allFinite && energySum > 1e-8, "E17 baked table produces finite audio in faster-than-real-time non-realtime mode after reload");
        q.setNonRealtime(false);
        std::printf("F9 restored recorded impulse energy %.9g\n", energySum);

        const auto revision = q.getRevision();
        const auto undo = q.getEditorSession().undoDepth();
        const auto clearRecording = [&] {
            return q.getEditorSession().run(juce::JSON::parse(R"({"op":"path.recordClear"})")).ok;
        };
        const auto pass = engQ->exportRecordedPass();
        juce::MemoryBlock beforeClear;
        q.getStateInformation(beforeClear);
        check(clearRecording(), "F9 clear a recording with baked tables");
        q.getStateInformation(clearedState); // Also covers save immediately after command acknowledgement.
        check(await([&] { return q.pathBakeStatus()["bakeSource"].toString() == "none"; })
              && !engQ->bakedTableSnapshot() && engQ->recordedPassStats().samples == 0,
              "F9 clears recorded tables and no longer offers a recording source");
        check(q.getRevision() == revision && q.getEditorSession().undoDepth() == undo,
              "F9 clear is revision- and undo-neutral");
        auto beforeXml = juce::AudioProcessor::getXmlFromBinary(beforeClear.getData(), (int)beforeClear.getSize());
        auto afterXml = juce::AudioProcessor::getXmlFromBinary(clearedState.getData(), (int)clearedState.getSize());
        check(afterXml && !afterXml->getChildByName("RecordedPass") && !afterXml->getChildByName("BakeCache"),
              "F9 immediate save omits cleared poses and table");
        if (beforeXml && afterXml) {
            beforeXml->deleteAllChildElementsWithTagName("RecordedPass");
            beforeXml->deleteAllChildElementsWithTagName("BakeCache");
            check(beforeXml->isEquivalentTo(afterXml.get(), true), "F9 unrelated saved scene and parameters remain identical");
        }

        check(engQ->importRecordedPass(pass), "F9 test restores a kept recording before editing drawn paths");
        verbp::MovementPath drawn;
        drawn.enabled = true;
        drawn.group = 0;
        drawn.points = {{{2.75, 2.4, 1.5}}, {{3.2, 2.4, 1.5}}};
        check(q.setMovementPath(0, drawn), "F9 test draws a grouped path");
        check(await([&] {
            const auto status = q.pathBakeStatus();
            const auto note = status["recordingNote"].toString();
            return status["bakeSource"].toString() == "drawn" && (int)status["bakeSourceGroup"] == 1
                && note == "Recording kept, not used: drawn paths are baked instead. Press Discard if you do not need it."
                && isPureAscii(note);
        }), "F9 grouped path wins over the kept recording with a distinct ASCII message");
        check(q.startPathBake(0) && await([&] { return q.pathBakePercent() == 100; }, 90), "F9 drawn path bakes with a recording kept");
        const auto drawnTable = engQ->bakedTableSnapshot();
        check(clearRecording() && drawnTable && !drawnTable->path.recorded
              && await([&] { return (int)q.pathBakeStatus()["passState"] == 0; })
              && engQ->bakedTableSnapshot() && engQ->bakedTableSnapshot()->hash() == drawnTable->hash(),
              "F9 clear unused recording preserves the drawn table");
        q.releaseResources();
    }

    {
        auto processor = std::make_unique<VerbProcessor>();
        auto& q = *processor;
        q.setPlayConfigDetails(2, 2, 48000, 128);
        q.prepareToPlay(48000, 128);
        q.setStateInformation(clearedState.getData(), (int)clearedState.getSize());
        check(waitReady(q, 90), "F9 project reloads after Discard");
        auto engine = E9bAccess::engine(q);
        check(engine->recordedPassStats().state == 0 && !engine->bakedTableSnapshot()
              && q.pathBakeStatus()["bakeSource"].toString() == "none",
              "F9 second reload leaves recording and its table cleared");
        q.releaseResources();
    }

    // Pre-E17 project XML (without RecordedPass) loads cleanly unchanged
    {
        auto legacyXml = juce::AudioProcessor::getXmlFromBinary(persistedState.getData(), (int)persistedState.getSize());
        check(legacyXml != nullptr, "E17 cloned state XML valid");
        if (legacyXml) {
            while (auto* ch = legacyXml->getChildByName("RecordedPass")) legacyXml->removeChildElement(ch, true);
            legacyXml->deleteAllChildElementsWithTagName("BakeCache");
            juce::MemoryBlock legacyBin;
            juce::AudioProcessor::copyXmlToBinary(*legacyXml, legacyBin);
            auto rPtr = std::make_unique<VerbProcessor>();
            auto& r = *rPtr;
            r.setTailSettings({4000, false});
            r.setPlayConfigDetails(2, 2, 48000, 128);
            r.prepareToPlay(48000, 128);
            r.setStateInformation(legacyBin.getData(), (int)legacyBin.getSize());
            check(waitReady(r, 90), "E17 pre-E17 project without RecordedPass loads cleanly");
            auto engR = E9bAccess::engine(r);
            check(engR->recordedPassStats().state == 0 && engR->recordedPassStats().samples == 0 && r.pathBakePercent() == 0,
                  "E17 pre-E17 project has empty recordedPassStats and no auto-bake");
            r.releaseResources();
        }
    }

    std::printf("E17 plugin failures %d\n", fails - before);
    return fails == before ? 0 : 1;
}
