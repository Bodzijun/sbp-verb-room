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

        // Move off recorded pass -> status reports Off recorded pass
        setP(p, "lx", 8.5);
        setP(p, "ly", 8.5);
        const bool offOk = waitStatus([&](const juce::var& st) {
            return st["reason"].toString().contains("Off recorded pass")
                && isPureAscii(st["reason"].toString());
        });
        check(offOk, "E17 pathBakeStatus reports Off recorded pass warning in pure ASCII");

        // Verify persisted state XML preserves RecordedPass when loaded AFTER baking too, with no auto-bake
        p.getStateInformation(persistedState);
        p.setPlayHead(nullptr);
        p.releaseResources();
    }

    // Reload persisted project state into fresh processor: pass preserved, NO auto-bake, manually bakeable
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
        check(q.pathBakePercent() == 0 && engQ->bakeStats().state == verb::Engine::BakeState::NotBaked && !engQ->bakedTableSnapshot(),
              "E17 reloaded project does NOT save table or auto-bake");

        check(q.startPathBake(-1), "E17 reloaded project allows explicit Bake without re-recording");
        juce::AudioBuffer<float> b(2, 128);
        juce::MidiBuffer midi;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (q.pathBakePercent() < 100 && std::chrono::steady_clock::now() < deadline) {
            b.clear();
            stage2Process(q, b, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
            juce::Thread::sleep(2);
        }
        check(q.pathBakePercent() == 100, "E17 reloaded RecordedPass bakes cleanly to 100%");
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
        q.releaseResources();
    }

    // Pre-E17 project XML (without RecordedPass) loads cleanly unchanged
    {
        auto legacyXml = juce::AudioProcessor::getXmlFromBinary(persistedState.getData(), (int)persistedState.getSize());
        check(legacyXml != nullptr, "E17 cloned state XML valid");
        if (legacyXml) {
            while (auto* ch = legacyXml->getChildByName("RecordedPass")) legacyXml->removeChildElement(ch, true);
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
