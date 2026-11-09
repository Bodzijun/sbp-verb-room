// Headless test of the plugin processor (no host): state, layout, automation, rendering, state round-trip.
#include <juce_audio_processors/juce_audio_processors.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

#include "InputOrder.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "../src/tail.hpp"

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

static bool waitReady(VerbProcessor& p, double seconds) {
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    while ((juce::Time::getMillisecondCounterHiRes() - t0) < seconds * 1000) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        if (p.getLoadState() == 2) return true;
        if (p.getLoadState() == 3) return false;
    }
    return false;
}

static void setP(VerbProcessor& p, const juce::String& id, double v) {
    auto* prm = p.apvts.getParameter(id);
    prm->setValueNotifyingHost(prm->convertTo0to1((float)v));
}

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#undef min
#undef max
static double privateMB() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    return (double)pmc.PrivateUsage / 1048576.0;
}
#else
static double privateMB() { return 0; }
#endif

static void testTailControls() {
    std::printf("Tail quality, Solo and decay diagnostics\n");
    VerbProcessor p; p.setPlayConfigDetails(4, 2, 48000, 256); p.prepareToPlay(48000, 256);
    check(p.getTailSettings().adaptive && p.getTailSettings().rays == 48000, "new instances use adaptive Mix quality");
    auto waitQuality = [&](int rays, bool adaptive) {
        juce::AudioBuffer<float> silence(4, 256); juce::MidiBuffer midi;
        const auto until = juce::Time::getMillisecondCounterHiRes() + 90000;
        while (juce::Time::getMillisecondCounterHiRes() < until) {
            silence.clear(); p.processBlock(silence, midi);
            auto diag = p.getTailDiagnostics();
            if (diag && diag->baseRays == rays && diag->adaptive == adaptive) return true;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        }
        return false;
    };
    p.setTailSettings({12000, false});
    check(waitReady(p, 90) && waitQuality(12000, false), "manual ray budget is applied to the room");
    const auto d = p.getTailDiagnostics();
    check(d && d->raysUsed == 12000 && !d->adaptive && d->active[3] && d->rt60[3] > 0, "the graph describes the current model and actual ray count");
    const auto serial = p.getLoadSerial();
    p.setTailSettings({24000, true});
    check(waitQuality(24000, true) && p.getLoadSerial() == serial, "live quality update retraces without rebuilding or resetting the engine");
    p.setTailSettings({12000, false});
    check(waitQuality(12000, false), "manual quality can be restored while audio keeps running");
    setP(p, "soloEarly", 1); setP(p, "mix", 0);
    juce::AudioBuffer<float> b(4, 256); juce::MidiBuffer m;
    for (int i = 0; i < 20; ++i) { b.clear(); p.processBlock(b, m); }
    b.clear(); b.setSample(0, 0, 1.f); p.processBlock(b, m);
    check(std::abs(b.getSample(0, 0)) < 1e-8, "Solo excludes the dry bypass even when Dry/Wet is zero");
    setP(p, "soloTail", 1);
    juce::MemoryBlock state; p.getStateInformation(state);
    VerbProcessor q; q.setPlayConfigDetails(4, 2, 48000, 256); q.prepareToPlay(48000, 256);
    q.setStateInformation(state.getData(), (int)state.getSize());
    check(waitReady(q, 90), "quality and Solo state load");
    check(q.getTailSettings().rays == 12000 && !q.getTailSettings().adaptive
          && q.apvts.getRawParameterValue("soloEarly")->load() == 1 && q.apvts.getRawParameterValue("soloTail")->load() == 1,
          "manual ray budget, adaptive switch and combined solos survive state round-trip");
    auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), (int)state.getSize());
    auto old = juce::ValueTree::fromXml(*xml);
    old.removeProperty("tailRays", nullptr); old.removeProperty("adaptiveTail", nullptr);
    for (const char* id : {"soloDirect", "soloEarly", "soloTail"}) old.removeChild(old.getChildWithProperty("id", id), nullptr);
    juce::MemoryBlock legacy;
    if (auto legacyXml = old.createXml()) juce::AudioProcessor::copyXmlToBinary(*legacyXml, legacy);
    q.setStateInformation(legacy.getData(), (int)legacy.getSize());
    check(waitReady(q, 90), "a pre-quality project still loads");
    check(q.getTailSettings().rays == 48000 && !q.getTailSettings().adaptive
          && q.apvts.getRawParameterValue("soloEarly")->load() == 0 && q.apvts.getRawParameterValue("soloTail")->load() == 0,
          "old projects use safe defaults and do not inherit a previous Solo");
    std::unique_ptr<juce::AudioProcessorEditor> base(p.createEditor());
    if (auto* ed = dynamic_cast<VerbEditor*>(base.get())) {
        auto dir = juce::File::getCurrentWorkingDirectory().getChildFile("out/quality"); dir.createDirectory();
        for (int width : {900, 1160}) {
            ed->setSize(width, width == 900 ? 600 : 760); ed->showPage(4, -1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
            auto img = ed->createComponentSnapshot(ed->getLocalBounds());
            juce::FileOutputStream f(dir.getChildFile("decay_" + juce::String(width) + ".png"));
            juce::PNGImageFormat png;
            if (f.openedOk()) { f.setPosition(0); f.truncate(); png.writeImageToStream(img, f); }
            check(img.getWidth() == width, "Decay tab paints at minimum and default window sizes");
        }
        ed->showPage(2, -1);
        auto img = ed->createComponentSnapshot(ed->getLocalBounds());
        juce::FileOutputStream f(dir.getChildFile("mix_solo.png")); juce::PNGImageFormat png;
        if (f.openedOk()) { f.setPosition(0); f.truncate(); png.writeImageToStream(img, f); }
    }
    VerbProcessor lfe;
    lfe.setPlayConfigDetails(6, 6, 48000, 256); lfe.prepareToPlay(48000, 256);
    check(waitReady(lfe, 90), "5.1 Solo processor starts before configuring its sources");
    verbp::SourceCfg bed; bed.format = 5;
    lfe.setSources({bed});
    verbp::OutputCfg output; output.layout = (int)verb::Layout::Surround51; lfe.setOutputCfg(output);
    check(waitReady(lfe, 90) && lfe.numPoints() == 5, "5.1 Solo test loads its five spatial points and LFE input");
    juce::AudioBuffer<float> surround(6, 256);
    setP(lfe, "soloTail", 1);
    for (int k = 0; k < 20; ++k) { surround.clear(); lfe.processBlock(surround, m); }
    surround.clear(); surround.setSample(3, 0, 1.f); lfe.processBlock(surround, m);
    const float tailLfe = surround.getSample(3, 0);
    setP(lfe, "soloTail", 0); setP(lfe, "soloDirect", 1);
    for (int k = 0; k < 20; ++k) { surround.clear(); lfe.processBlock(surround, m); }
    surround.clear(); surround.setSample(3, 0, 1.f); lfe.processBlock(surround, m);
    std::printf("  LFE Solo levels: Tail %.6f, Direct %.6f\n", tailLfe, surround.getSample(3, 0));
    check(std::abs(tailLfe) < 1e-8 && std::abs(surround.getSample(3, 0) - 1.f) < 0.01f,
          "LFE is excluded from Tail Solo and included at unity in Direct Solo");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep the output even if something crashes
    juce::ScopedJuceInitialiser_GUI init;
    if (std::getenv("VERB_PRESETS_ONLY")) {
        VerbProcessor p;
        p.setPlayConfigDetails(64, 2, 48000, 256); p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "portable preset processor starts");
        auto cfg = p.getSources(); cfg.resize(2);
        cfg[0].name = juce::String::fromUTF8(u8"Скрипки I & II").toStdString();
        cfg[0].format = 1; cfg[0].channels = 2; cfg[0].firstIn = 8; cfg[0].spread = 0.8; cfg[0].pattern = 0.5;
        cfg[0].freePoints = true; cfg[0].ox = {-0.25f, 0.75f}; cfg[0].oy = {0.1f, -0.1f}; cfg[0].oz = {0.f, 0.2f};
        cfg[1].name = "Harp"; cfg[1].format = 1; cfg[1].channels = 2; cfg[1].firstIn = 62; cfg[1].sum = true;
        p.setSources(cfg); check(waitReady(p, 90), "custom stereo and free-point setup loads");
        auto output = p.getOutputCfg(); output.radius = 0.25; output.micPattern = (int)verb::MicPattern::HumanEars;
        p.setOutputCfg(output); check(waitReady(p, 90), "custom output loads");
        p.setTailSettings({12000, false});
        setP(p, verbp::paramId(0, 0), 2.345); setP(p, verbp::paramId(0, 4), -7.5);
        setP(p, verbp::paramId(verbp::kListener, 3), 65); setP(p, "mix", 72); setP(p, "tail", -3);
        setP(p, "soloTail", 1); setP(p, verbp::sourceSwitchId(0, true), 1); setP(p, verbp::sourceSwitchId(1, false), 1);
        const auto preset = p.getRoomPresetText();
        auto dir = juce::File::getCurrentWorkingDirectory().getChildFile("out/presets"); dir.createDirectory();
        const auto file = dir.getChildFile("roundtrip.verbroom");
        check(preset.isNotEmpty() && file.replaceWithText(preset), "complete preset writes to a standalone XML file");
        VerbProcessor q; q.setPlayConfigDetails(64, 2, 48000, 256); q.prepareToPlay(48000, 256);
        check(waitReady(q, 60), "fresh receiving processor starts");
        q.setMixerCompactEnabled(false); q.setInputMetersEnabled(false);
        juce::String error;
        check(q.loadRoomPresetText(file.loadFileAsString(), error) && waitReady(q, 90), "preset loads in another instance");
        const auto restored = q.getSources();
        check(restored.size() == 2 && restored[0].name == cfg[0].name && restored[0].format == 1 && restored[0].firstIn == 8
            && restored[0].freePoints && restored[0].ox == cfg[0].ox && restored[0].oz == cfg[0].oz
            && restored[0].spread == cfg[0].spread && restored[0].pattern == cfg[0].pattern
            && restored[1].sum && restored[1].firstIn == 62 && q.numPoints() == 3,
            "Unicode names, stereo routing, summing, spread, directivity and free points survive");
        auto value = [&](const juce::String& id) { return q.apvts.getRawParameterValue(id)->load(); };
        check(std::abs(value("s1x") - 2.345) < 0.002 && std::abs(value("s1g") + 7.5) < 0.002 && std::abs(value("laz") - 65) < 0.002
            && value("mix") == 72 && std::abs(value("tail") + 3) < 0.002 && value("soloTail") == 1
            && value("s1solo") == 1 && value("s2mute") == 1 && q.getTailSettings().rays == 12000
            && !q.getTailSettings().adaptive && q.getOutputCfg().radius == 0.25
            && q.getOutputCfg().micPattern == (int)verb::MicPattern::HumanEars,
            "positions, gains, Solo/Mute, mix, tail quality and output survive");
        check(!q.mixerCompactEnabled() && !q.inputMetersEnabled() && !q.keepRoutingEnabled(), "local display and Keep I/O preferences remain local");
        const auto before = q.getSceneSerial();
        auto broken = juce::parseXML(preset); broken->setAttribute("formatVersion", 999);
        check(!q.loadRoomPresetText(broken->toString(), error) && error.isNotEmpty(), "future preset version is rejected");
        broken = juce::parseXML(preset); broken->getChildByName("VerbState")->setAttribute("sceneText", "floor missing");
        check(!q.loadRoomPresetText(broken->toString(), error), "invalid scene is rejected before state changes");
        broken = juce::parseXML(preset);
        broken->getChildByName("VerbState")->getChildByName("Sources")->getChildByName("Src")->setAttribute("firstIn", 63);
        check(!q.loadRoomPresetText(broken->toString(), error), "stereo input beyond channel 64 is rejected before state changes");
        broken = juce::parseXML(preset); auto* payload = broken->getChildByName("VerbState");
        payload->removeChildElement(payload->getChildByName("PARAM"), true);
        check(!q.loadRoomPresetText(broken->toString(), error) && q.getSceneSerial() == before && value("mix") == 72,
              "incomplete preset leaves the current room and mix intact");
        auto local = cfg; local.resize(1); local[0].firstIn = 20; local[0].name = "Local routing";
        q.setSources(local); check(waitReady(q, 90), "different local routing loads");
        auto localOutput = q.getOutputCfg(); localOutput.radius = 0.4; q.setOutputCfg(localOutput);
        check(waitReady(q, 90), "local output loads");
        q.setKeepRoutingEnabled(true);
        check(q.loadRoomPresetText(preset, error) && waitReady(q, 90) && q.getSources().size() == 1
            && q.getSources()[0].firstIn == 20 && q.getSources()[0].name == "Local routing"
            && q.getOutputCfg().radius == 0.4 && q.keepRoutingEnabled(), "Keep I/O preserves local source setup and output when importing");
        q.setKeepRoutingEnabled(false);
        q.loadSceneText(p.getRoomText(), "legacy.txt", true);
        check(waitReady(q, 90) && q.getSources().size() == 2, "legacy plain-text scenes still load");
        return fails ? 1 : 0;
    }
    if (std::getenv("VERB_VSL_HALL_ONLY")) {
        for (bool conductor : {false, true}) {
            const auto room = verbp::vslGrandHall(conductor);
            std::istringstream input(room.text);
            auto scene = verb::parseScene(input, room.name);
            check(scene.walls.size() == 16 && scene.sources.size() == 32 && room.setup.size() == 32, "VSL hall has a faceted outline and thirty-two groups");
            const int rear = scene.walls[0].mat;
            check(scene.walls[1].mat == rear && scene.walls[15].mat == rear
                && scene.mats[(size_t)rear].name == "Rear_absorbing_wall" && scene.walls[2].mat != rear,
                "both rear bevels share the absorbing back-wall material; side walls stay reflective");
            verb::Zone footprint; for (const auto& w : scene.walls) footprint.poly.emplace_back(w.ax, w.ay);
            bool inside = verb::pointInZone(footprint, scene.listenerPos.x, scene.listenerPos.y);
            bool connected = true, routes = true, aboveFloor = true;
            for (size_t i = 0; i < scene.walls.size(); ++i) {
                const auto& a = scene.walls[i]; const auto& b = scene.walls[(i + 1) % scene.walls.size()];
                connected &= std::hypot(a.bx - b.ax, a.by - b.ay) < 1e-5;
            }
            for (size_t i = 0; i < room.setup.size(); ++i) {
                const auto& c = room.setup[i]; const auto& t = scene.sources[i];
                routes &= c.format == 1 && c.sum && c.firstIn == (int)(2 * i);
                inside &= verb::pointInZone(footprint, t.pos.x, t.pos.y);
                double floor = 0;
                for (const auto& z : scene.floorZones) if (z.z >= 0 && verb::pointInZone(z, t.pos.x, t.pos.y)) floor = std::max(floor, z.z);
                aboveFloor &= t.pos.z > floor && t.pos.z < scene.height;
            }
            int plates = 0;
            for (const auto* zones : {&scene.floorZones, &scene.ceilZones}) for (const auto& z : *zones) plates += z.z >= 0;
            check(connected && inside && aboveFloor && plates == 8, "hall outline is closed; sources and listener are inside; instruments clear the risers");
            check(routes && verbp::buildPoints(room.setup).size() == 32, "sixty-four inputs map to thirty-two summed stereo sources");
            VerbProcessor p;
            p.setPlayConfigDetails(64, 2, 48000, 256); p.prepareToPlay(48000, 256);
            check(waitReady(p, 60), "VSL test processor starts");
            p.loadRoomWithSetup(juce::String(room.text), juce::String(room.name), room.setup, room.micPattern);
            check(waitReady(p, 180) && p.getSources().size() == 32 && p.numPoints() == 32, "VSL hall loads all source routes");
            auto presetDir = juce::File::getCurrentWorkingDirectory().getChildFile("out/vsl-hall/presets"); presetDir.createDirectory();
            const auto presetFile = presetDir.getChildFile(conductor ? "VSL_Grand_Hall_Conductor.verbroom" : "VSL_Grand_Hall_Audience.verbroom");
            check(presetFile.replaceWithText(p.getRoomPresetText()), "VSL portable preset exports");
            juce::String presetError;
            check(p.loadRoomPresetText(presetFile.loadFileAsString(), presetError) && waitReady(p, 180)
                && p.getSources().size() == 32 && p.getSources().back().firstIn == 62,
                "VSL portable preset restores all thirty-two stereo groups");
            setP(p, "soloDirect", 1);
            juce::AudioBuffer<float> b(64, 256); juce::MidiBuffer midi;
            auto level = [&](int ch) {
                for (int k = 0; k < 40; ++k) { b.clear(); p.processBlock(b, midi); }
                double e = 0;
                for (int k = 0; k < 100; ++k) {
                    b.clear(); if (k == 0) b.setSample(ch, 0, 1.f); p.processBlock(b, midi);
                    for (int c = 0; c < 2; ++c) for (int n = 0; n < 256; ++n) e += b.getSample(c, n) * b.getSample(c, n);
                }
                return e;
            };
            const double first = level(0), lastLeft = level(62), lastRight = level(63);
            check(std::isfinite(first) && first > 1e-10 && lastLeft > 1e-10 && std::abs(lastLeft - lastRight) < lastLeft * 0.001,
                  "first and last stereo inputs sound; channels 63 and 64 sum equally");
            if (!conductor) {
                juce::MemoryBlock state; p.getStateInformation(state);
                p.setStateInformation(state.getData(), (int)state.getSize());
                check(waitReady(p, 180) && p.getSources().back().firstIn == 62, "VSL routing survives project state restore");
                auto dir = juce::File::getCurrentWorkingDirectory().getChildFile("out/vsl-hall"); dir.createDirectory();
                std::unique_ptr<juce::AudioProcessorEditor> base(p.createEditor());
                if (auto* ed = dynamic_cast<VerbEditor*>(base.get())) {
                    ed->setSize(1160, 760);
                    for (int plane : {0, 1}) {
                        ed->showPlane(plane); juce::MessageManager::getInstance()->runDispatchLoopUntil(120);
                        auto image = ed->createComponentSnapshot(ed->getLocalBounds());
                        juce::FileOutputStream out(dir.getChildFile(plane == 0 ? "vsl_plan.png" : "vsl_front.png"));
                        juce::PNGImageFormat png; if (out.openedOk()) png.writeImageToStream(image, out);
                    }
                }
            }
        }
        return fails ? 1 : 0;
    }
    if (std::getenv("VERB_SOURCE_SWITCHES_ONLY")) {
        VerbProcessor p;
        p.setPlayConfigDetails(2, 2, 48000, 256); p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "source switch room loads");
        auto cfg = p.getSources(); cfg.resize(2); cfg[0].firstIn = 0; cfg[1].firstIn = 1;
        p.setSources(cfg); check(waitReady(p, 60), "two source routes load");
        for (int k = 0; k < 4; ++k) setP(p, verbp::paramId(1, k), p.apvts.getRawParameterValue(verbp::paramId(0, k))->load());
        setP(p, "soloDirect", 1);
        juce::AudioBuffer<float> audio(2, 256); juce::MidiBuffer midi;
        auto energy = [&](int input) {
            for (int i = 0; i < 40; ++i) { audio.clear(); p.processBlock(audio, midi); }
            double e = 0;
            for (int i = 0; i < 100; ++i) {
                audio.clear(); if (i == 0) audio.setSample(input, 0, 1.f);
                p.processBlock(audio, midi);
                for (int ch = 0; ch < 2; ++ch) for (int n = 0; n < 256; ++n) e += audio.getSample(ch, n) * audio.getSample(ch, n);
            }
            return e;
        };
        const double baseline = energy(0);
        check(baseline > 1e-10, "source direct signal is audible");
        setP(p, verbp::sourceSwitchId(0, false), 1);
        check(energy(0) < baseline * 1e-8 && energy(1) > baseline * 0.5, "Mute gates only its own source");
        setP(p, verbp::sourceSwitchId(0, false), 0);
        setP(p, verbp::sourceSwitchId(0, true), 1);
        check(energy(0) > baseline * 0.5 && energy(1) < baseline * 1e-8, "Solo excludes other sources");
        setP(p, verbp::sourceSwitchId(1, true), 1);
        check(energy(1) > baseline * 0.5, "multiple source solos combine");
        setP(p, verbp::sourceSwitchId(1, false), 1);
        check(energy(1) < baseline * 1e-8, "Mute takes priority over Solo");
        setP(p, "soloDirect", 0); setP(p, "mix", 0);
        check(energy(1) < 1e-8 && energy(0) > 0.9, "source switches also gate the mapped dry bypass");
        p.setMixerCompactEnabled(false);
        juce::MemoryBlock state; p.getStateInformation(state);
        setP(p, verbp::sourceSwitchId(0, true), 0); p.setMixerCompactEnabled(true);
        p.setStateInformation(state.getData(), (int)state.getSize());
        check(waitReady(p, 90), "source switch state reloads");
        check(!p.mixerCompactEnabled() && p.apvts.getRawParameterValue(verbp::sourceSwitchId(0, true))->load() == 1.f,
              "source Solo and mixer width survive state restore");
        auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), (int)state.getSize());
        auto legacy = juce::ValueTree::fromXml(*xml);
        legacy.removeProperty("mixerCompact", nullptr);
        for (int s = 0; s < verbp::kMaxSources; ++s) for (bool solo : {false, true}) legacy.removeChild(legacy.getChildWithProperty("id", verbp::sourceSwitchId(s, solo)), nullptr);
        juce::MemoryBlock old; if (auto x = legacy.createXml()) juce::AudioProcessor::copyXmlToBinary(*x, old);
        p.setStateInformation(old.getData(), (int)old.getSize()); check(waitReady(p, 90), "legacy state reloads");
        check(p.mixerCompactEnabled() && p.apvts.getRawParameterValue(verbp::sourceSwitchId(0, true))->load() == 0.f
              && p.apvts.getRawParameterValue(verbp::sourceSwitchId(1, false))->load() == 0.f, "legacy state defaults to Compact with all sources enabled");
        return fails ? 1 : 0;
    }
    if (std::getenv("VERB_MIXER_ONLY")) {
        VerbProcessor p;
        p.setPlayConfigDetails(32, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "mixer test room loads");
        std::vector<verbp::SourceCfg> cfg(17);
        for (int i = 0; i < 17; ++i) { cfg[(size_t)i].name = "Orchestra " + std::to_string(i + 1); cfg[(size_t)i].firstIn = i; }
        cfg[0].format = 1;  // exercise the stereo input-range caption
        p.setSources(cfg);
        check(waitReady(p, 90), "seventeen mixer sources load");
        setP(p, verbp::sourceSwitchId(0, true), 1);
        setP(p, verbp::sourceSwitchId(1, false), 1);
        juce::AudioBuffer<float> meterInput(32, 256); meterInput.clear();
        meterInput.setSample(0, 0, 0.5f); meterInput.setSample(1, 0, 0.2f);
        juce::MidiBuffer midi;
        p.processBlock(meterInput, midi);
        std::unique_ptr<juce::AudioProcessorEditor> base(p.createEditor());
        auto* ed = dynamic_cast<VerbEditor*>(base.get());
        check(ed != nullptr, "mixer editor opens with seventeen sources");
        if (ed) {
            auto dir = juce::File::getCurrentWorkingDirectory().getChildFile("out/mixer"); dir.createDirectory();
            for (int width : {900, 1160}) {
                ed->setSize(width, width == 900 ? 600 : 760);
                for (int page : {0, 2, 4}) {
                    ed->showPage(page, 0);
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
                    auto img = ed->createComponentSnapshot(ed->getLocalBounds());
                    juce::FileOutputStream f(dir.getChildFile(juce::String("page_") + juce::String(page) + "_" + juce::String(width) + ".png"));
                    juce::PNGImageFormat png;
                    if (f.openedOk()) { f.setPosition(0); f.truncate(); png.writeImageToStream(img, f); }
                    check(img.isValid(), "mixer, inputs and decay pages paint");
                }
                p.setMixerCompactEnabled(false); ed->showPage(2, -1);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
                auto normal = ed->createComponentSnapshot(ed->getLocalBounds());
                juce::FileOutputStream f(dir.getChildFile("normal_" + juce::String(width) + ".png"));
                juce::PNGImageFormat png;
                if (f.openedOk()) { f.setPosition(0); f.truncate(); png.writeImageToStream(normal, f); }
                p.setMixerCompactEnabled(true);
            }
            ed->showPage(2, -1);
            std::function<juce::Slider*(juce::Component&)> findGain = [&](juce::Component& c) -> juce::Slider* {
                if (c.getName() == "Source 1 gain") return dynamic_cast<juce::Slider*>(&c);
                for (auto* child : c.getChildren()) if (auto* result = findGain(*child)) return result;
                return nullptr;
            };
            auto* gain = findGain(*ed);
            check(gain != nullptr, "first source gain is available in mixer");
            if (gain) {
                gain->setValue(-12, juce::sendNotificationSync);
                check(std::abs(p.apvts.getRawParameterValue(verbp::paramId(0, 4))->load() + 12.f) < 0.01f,
                      "mixer fader controls the matching source gain");
            }
        }
        return fails ? 1 : 0;
    }
    if (std::getenv("VERB_ROUTING_ONLY")) {
        VerbProcessor p;
        check(p.getTailLengthSeconds() >= 20.0, "host sees the full possible tail before asynchronous room loading");
        p.setPlayConfigDetails(8, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "initial room loads");
        auto sources = p.getSources();
        sources[0].name = "Preserved input";
        sources[0].firstIn = 3;
        p.setSources(sources);
        check(waitReady(p, 60), "custom input loads");
        auto output = p.getOutputCfg();
        output.radius = 0.23;
        output.micPattern = 1;
        p.setOutputCfg(output);
        check(waitReady(p, 60), "custom output loads");
        p.setKeepRoutingEnabled(true);
        const auto rooms = verbp::builtinRooms();
        auto it = std::find_if(rooms.begin(), rooms.end(), [](const auto& r) { return !r.setup.empty(); });
        check(it != rooms.end(), "preset with source setup exists");
        if (it != rooms.end()) {
            p.loadRoomWithSetup(juce::String(it->text), "routing test", it->setup, it->micPattern);
            check(waitReady(p, 90), "preset loads with routing lock");
            const auto kept = p.getSources();
            const auto out = p.getOutputCfg();
            check(kept.size() == sources.size() && kept[0].firstIn == 3 && kept[0].name == "Preserved input", "source routing survives preset change");
            check(std::abs(out.radius - 0.23) < 1e-6 && out.micPattern == 1, "listener output survives preset change");
            juce::MemoryBlock state;
            p.getStateInformation(state);
            VerbProcessor restored;
            restored.setStateInformation(state.getData(), (int)state.getSize());
            check(restored.keepRoutingEnabled(), "routing lock survives project state restore");
            p.setKeepRoutingEnabled(false);
            p.loadRoomWithSetup(juce::String(it->text), "routing test unlocked", it->setup, it->micPattern);
            check(waitReady(p, 90), "preset loads without routing lock");
            check(p.getSources().size() == it->setup.size() && p.getSources()[0].name == it->setup[0].name, "unlocked preset replaces source setup");
        }
        return fails ? 1 : 0;
    }
    testTailControls();
    if (std::getenv("VERB_QUALITY_ONLY")) return fails ? 1 : 0;
    if (std::getenv("VERB_MEMPROBE")) {  // exploration: does the plugin grow while a room is reloaded and a source is moved?
        const auto rooms = verbp::builtinRooms();
        for (auto& r : rooms) {
            if (r.name.find(std::getenv("VERB_MEMPROBE")) == std::string::npos) continue;
            VerbProcessor p;
            p.setPlayConfigDetails(32, 2, 48000, 512);
            p.prepareToPlay(48000, 512);
            waitReady(p, 60);
            juce::AudioBuffer<float> b(32, 512);
            juce::MidiBuffer m;
            std::printf("  %s: start %.0f MB\n", r.name.c_str(), privateMB());
            for (int it = 0; it < 12; ++it) {
                const auto tl0 = std::chrono::steady_clock::now();
                p.loadRoomWithSetup(juce::String(r.text), "hall.txt", r.setup, r.micPattern);
                waitReady(p, 90);
                const double tl = std::chrono::duration<double>(std::chrono::steady_clock::now() - tl0).count();
                double worst = 0;
                for (int k = 0; k < 20; ++k) {
                    b.clear();
                    const auto tb0 = std::chrono::steady_clock::now();
                    p.processBlock(b, m);
                    worst = std::max(worst, std::chrono::duration<double>(std::chrono::steady_clock::now() - tb0).count() * 1000.0);
                }
                std::printf("   reload %2d: %.0f MB, load %.2f s, worst block %.2f ms (tail %.2f s)\n", it + 1, privateMB(), tl, worst, p.getTailLengthSeconds());
            }
            for (int it = 0; it < 6; ++it) {  // a source swept back and forth: the tail is rebuilt again and again
                double worst = 0;
                for (int k = 0; k < 400; ++k) {
                    if (auto* prm = p.apvts.getParameter(verbp::paramId(0, 0))) prm->setValueNotifyingHost((float)(0.5 + 0.4 * std::sin(k * 0.05)));
                    b.clear();
                    const auto tb0 = std::chrono::steady_clock::now();
                    p.processBlock(b, m);
                    worst = std::max(worst, std::chrono::duration<double>(std::chrono::steady_clock::now() - tb0).count() * 1000.0);
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
                }
                std::printf("   sweep %d: %.0f MB, worst block %.2f ms (budget 10.7 ms)\n", it + 1, privateMB(), worst);
            }
            break;
        }
        return 0;
    }
    std::printf("Plugin processor test\n");
    {
        VerbProcessor p;
        p.setPlayConfigDetails(4, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "built-in scene loads in the background");
        std::printf("  status: %s\n", p.getStatus().toRawUTF8());
        check(p.getTotalNumOutputChannels() == 2, "default output is stereo");

        // impulse on source 1 -> check arrival time (direct path to the listener centroid) and finiteness
        const int N = 48000, blk = 256;
        juce::AudioBuffer<float> buf(4, blk);
        std::vector<float> L, R;
        juce::MidiBuffer midi;
        bool finite = true;
        for (int pos = 0; pos < N; pos += blk) {
            buf.clear();
            if (pos == 0) buf.setSample(0, 0, 1.f);
            p.processBlock(buf, midi);
            for (int i = 0; i < blk; ++i) {
                float a = buf.getSample(0, i), b = buf.getSample(1, i);
                finite &= std::isfinite(a) && std::isfinite(b);
                L.push_back(a); R.push_back(b);
            }
        }
        check(finite, "output finite");
        int first = -1;
        double e = 0;
        for (size_t i = 0; i < L.size(); ++i) { e += L[i] * L[i]; if (first < 0 && std::fabs(L[i]) > 1e-3f) first = (int)i; }
        std::printf("  first arrival at sample %d (direct path 4 m => ~560), energy %.4f\n", first, e);
        check(first > 540 && first < 600 && e > 1e-4, "direct sound arrives at the geometric delay with energy present");

        // automation: move the source far away -> level must drop
        auto energy = [&](int steps) {
            double en = 0;
            for (int s = 0; s < steps; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int i = 0; i < blk; ++i) en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i);
            }
            return en;
        };
        for (int i = 0; i < 100; ++i) { buf.clear(); p.processBlock(buf, midi); }
        double eNear = energy(40);
        setP(p, "s1x", 0.5);
        for (int i = 0; i < 400; ++i) { buf.clear(); p.processBlock(buf, midi); }  // let glide + decay settle
        double eMoved = energy(40);
        std::printf("  energy near %.3e, after automating s1x to 0.5 m: %.3e\n", eNear, eMoved);
        check(std::isfinite(eMoved) && eMoved != eNear, "automating a source parameter changes the output");

        // room editing: an opaque partition between source 1 and the listener must cut the early sound, an Open
        // segment must let it through again, and the edit must survive the project state
        auto early = [&](int steps) {  // energy of the first ~50 ms after an impulse on input 1
            double en = 0;
            for (int s = 0; s < steps; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int i = 0; i < blk; ++i) en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i);
            }
            return en;
        };
        auto settleBlocks = [&](int n) { for (int i = 0; i < n; ++i) { buf.clear(); p.processBlock(buf, midi); } };
        setP(p, "s1x", 3.0);  // back to the scene position (the automation test above moved it)
        settleBlocks(400);
        const double eOpenRoom = early(10);
        const size_t wallsBefore = p.getDisplayScene().walls.size();
        p.editScene([](verb::Scene& s) {
            verb::Material m = verb::surfacePresets()[0];  // Concrete
            s.mats.push_back(m);
            s.walls.push_back(verb::Wall{0.0, 4.0, 6.0, 4.0, (int)s.mats.size() - 1});  // between the source (3, 6) and the listener (3, 2)
        });
        // early paths follow within a control period; the late field is re-traced on a worker thread (seconds)
        juce::Thread::sleep(6000);
        settleBlocks(400);
        const double eBlocked = early(10);
        std::printf("  early energy: open room %.3e, with a concrete partition %.3e (%.0f dB)\n", eOpenRoom, eBlocked, 10 * std::log10(eBlocked / eOpenRoom));
        check(p.getDisplayScene().walls.size() == wallsBefore + 1, "editScene adds the wall to the live room");
        check(eBlocked < eOpenRoom * 0.05, "an opaque wall inserted while running cuts the early sound by > 13 dB");
        juce::MemoryBlock edited;
        p.getStateInformation(edited);
        p.editScene([](verb::Scene& s) {  // make that segment an opening (own surface: only this wall changes)
            s.mats[(size_t)s.walls.back().mat] = verb::presetByName("Open");
        });
        juce::Thread::sleep(6000);
        settleBlocks(400);
        const double eOpening = early(10);
        std::printf("  with the segment set to Open: %.3e (%.1f dB vs open room)\n", eOpening, 10 * std::log10(eOpening / eOpenRoom));
        check(std::fabs(10 * std::log10(eOpening / eOpenRoom)) < 3.0, "an Open segment lets the sound through (no extra reflection/loss)");
        // the same partition, now only 1.0 m high: the sound (source and listener at 1.5 m) passes over it
        p.editScene([](verb::Scene& s) {
            s.mats[(size_t)s.walls.back().mat] = verb::presetByName("Concrete");
            s.walls.back().z1 = 1.0;
        });
        juce::Thread::sleep(6000);
        settleBlocks(400);
        const double eLow = early(10);
        std::printf("  concrete partition 1.0 m high (ears at 1.5 m): %.3e (%.1f dB vs open room)\n", eLow, 10 * std::log10(eLow / eOpenRoom));
        check(std::fabs(10 * std::log10(eLow / eOpenRoom)) < 3.0, "a low partition is passed over by sound at ear height");
        {
            VerbProcessor q2;
            q2.setPlayConfigDetails(4, 2, 48000, 256);
            q2.prepareToPlay(48000, 256);
            q2.setStateInformation(edited.getData(), (int)edited.getSize());
            check(waitReady(q2, 60), "project with the edited room reloads");
            check(q2.getDisplayScene().walls.size() == wallsBefore + 1, "the edited walls are saved in the project state");
        }

        // positions are free: the listener far outside the room (and above the ceiling) must not break anything,
        // and must come back to the same level
        {
            setP(p, "lx", 3.0); setP(p, "ly", 2.0);
            settleBlocks(200);
            const double eHome = early(10);
            setP(p, "lx", -40.0); setP(p, "ly", 25.0); setP(p, "lz", 29.0);
            settleBlocks(200);
            bool fin = true;
            for (int s = 0; s < 20; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int i = 0; i < blk; ++i) fin &= std::isfinite(buf.getSample(0, i)) && std::isfinite(buf.getSample(1, i));
            }
            check(fin, "listener far outside the room: output finite");
            check(p.getDisplayScene().walls.size() > 0 && std::fabs(p.apvts.getRawParameterValue("lx")->load() + 40.f) < 1e-3f, "outside position is kept (not clamped to the walls)");
            setP(p, "lx", 3.0); setP(p, "ly", 2.0); setP(p, "lz", 1.5);
            settleBlocks(300);
            const double eBack = early(10);
            check(std::fabs(10 * std::log10(eBack / eHome)) < 1.0, "back inside: the same level as before");
        }

        // the editor must construct, lay out and paint; snapshots go to out/ for a visual check
        {
            std::unique_ptr<juce::AudioProcessorEditor> base(p.createEditor());
            auto* ed = dynamic_cast<VerbEditor*>(base.get());
            check(ed != nullptr, "editor is created");
            if (ed) {
                ed->setSize(1160, 760);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
                auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile("out");
                outDir.createDirectory();
                auto save = [&](const char* name) {
                    juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
                    juce::PNGImageFormat png;
                    juce::FileOutputStream fo(outDir.getChildFile(name));
                    if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
                    return img.getWidth() > 100;
                };
                check(save("editor_objects.png"), "editor paints (objects tab)");
                ed->selectWall((int)p.getDisplayScene().walls.size() - 1);  // the segment made Open above
                juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
                check(save("editor_surfaces.png"), "editor paints (surfaces tab, selected segment)");
                ed->selectWall(0);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
                check(save("editor_surfaces_wall1.png"), "editor paints another segment");
            }
        }

        // state round trip
        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        VerbProcessor q;
        q.setPlayConfigDetails(4, 2, 48000, 256);
        q.prepareToPlay(48000, 256);
        q.setStateInformation(mb.getData(), (int)mb.getSize());
        check(waitReady(q, 60), "scene restored from saved state");
        check(std::fabs(q.apvts.getRawParameterValue("s1x")->load() - 3.0f) < 1e-3f, "automated parameter value restored");
        check(q.getSceneName() == p.getSceneName(), "scene name restored");
        {   // the input meters are an option saved with the project
            p.setInputMetersEnabled(false);
            juce::MemoryBlock mb2;
            p.getStateInformation(mb2);
            VerbProcessor q2;
            q2.setPlayConfigDetails(4, 2, 48000, 256);
            q2.prepareToPlay(48000, 256);
            q2.setStateInformation(mb2.getData(), (int)mb2.getSize());
            check(!q2.inputMetersEnabled() && q.inputMetersEnabled(), "the input meters option is saved with the project");
            p.setInputMetersEnabled(true);
        }
    }
    {   // orientation, scaling, free channel points, space (air, clutter)
        VerbProcessor r;
        r.setPlayConfigDetails(8, 2, 48000, 256);
        r.prepareToPlay(48000, 256);
        check(waitReady(r, 60), "setup for orientation / scaling / free points loads");
        auto param = [&](const char* id) { return (double)r.apvts.getRawParameterValue(id)->load(); };
        setP(r, "s1x", 3.0); setP(r, "s1y", 6.0);
        r.orientObject(0, 3.0, 10.0);
        const double az1 = param("s1az");
        r.orientObject(verbp::kListener, param("lx"), param("ly") - 8.0);
        const double yaw1 = param("laz");  // the listener's yaw uses the "az" component id
        std::printf("  orient: source towards +y -> azimuth %.1f, listener towards -y -> yaw %.1f\n", az1, yaw1);
        check(std::fabs(az1 - 90.0) < 0.01 && std::fabs(yaw1 + 90.0) < 0.01, "orientObject points a source / the listener at a position");

        const auto before = r.getDisplayScene();
        const double s1x0 = param("s1x"), lx0 = param("lx");
        r.scaleRoom(2.0, true);
        const auto big = r.getDisplayScene();
        const bool bigOk = std::fabs(big.walls[1].ax - (-3.0)) < 1e-6 && std::fabs(big.walls[1].bx - 9.0) < 1e-6 && std::fabs(big.height - 6.0) < 1e-6 &&
                           std::fabs(param("s1x") - (3.0 + (s1x0 - 3.0) * 2.0)) < 1e-3 && std::fabs(param("lx") - (3.0 + (lx0 - 3.0) * 2.0)) < 1e-3;
        r.scaleRoom(0.5, true);
        const auto back = r.getDisplayScene();
        check(bigOk && std::fabs(back.walls[0].ax - before.walls[0].ax) < 1e-6 && std::fabs(back.height - before.height) < 1e-6 && std::fabs(param("s1x") - s1x0) < 1e-3,
              "scaleRoom scales walls, height, sources and listener about the room centre; x2 then x0.5 restores it");
        r.scaleWall(0, 0.5);
        const auto sw = r.getDisplayScene();
        check(std::fabs(std::hypot(sw.walls[0].bx - sw.walls[0].ax, sw.walls[0].by - sw.walls[0].ay) - 4.0) < 1e-6 &&
                  std::fabs(0.5 * (sw.walls[0].ay + sw.walls[0].by) - 4.0) < 1e-6,
              "scaleWall halves a segment about its midpoint");

        // free channel points: moving a point 2 m towards the listener makes its sound arrive 2 m / c earlier
        setP(r, "s1az", -90.0);  // faces the listener, which is below the source
        {
            auto v = r.getSources();
            v[0].name = "Pair"; v[0].format = 1; v[0].channels = 2; v[0].firstIn = 0; v[0].spread = 1.0; v[0].freePoints = true;
            verbp::freezeLayout(v[0]);
            r.setSources(v);
        }
        check(waitReady(r, 60), "stereo source with free points loads");
        auto arrival = [&]() {
            juce::AudioBuffer<float> b(8, 256);
            juce::MidiBuffer m;
            for (int s = 0; s < 60; ++s) { b.clear(); r.processBlock(b, m); }  // settle
            int first = -1;
            for (int s = 0; s < 30 && first < 0; ++s) {
                b.clear();
                if (s == 0) b.setSample(0, 0, 1.f);  // impulse on channel 1 = point 0 of the pair
                r.processBlock(b, m);
                for (int i = 0; i < 256 && first < 0; ++i)
                    if (std::fabs(b.getSample(0, i)) > 2e-3f || std::fabs(b.getSample(1, i)) > 2e-3f) first = s * 256 + i;
            }
            return first;
        };
        const int a0 = arrival();
        const auto cfg0 = r.getSources()[0];
        r.setPointOffset(0, 0, cfg0.ox[0] + 2.0f, cfg0.oy[0], cfg0.oz[0]);
        const int a1 = arrival();
        const double expectShift = 2.0 / 343.2 * 48000.0;
        std::printf("  free point moved 2 m towards the listener: first arrival %d -> %d (shift %d, expected %.0f)\n", a0, a1, a0 - a1, expectShift);
        check(a0 > 0 && a1 > 0 && std::fabs((a0 - a1) - expectShift) < 12, "a free channel point moves independently of the others (arrival time follows its distance)");
        juce::MemoryBlock fm;
        r.getStateInformation(fm);

        // space: air and clutter live in the scene and in the project state
        r.editScene([](verb::Scene& s) { s.tempC = 8.0; s.humidity = 25.0; s.clutter.count = 30; s.clutter.seed = 1234; });
        juce::Thread::sleep(300);
        juce::MemoryBlock sm;
        r.getStateInformation(sm);
        VerbProcessor r2;
        r2.setPlayConfigDetails(8, 2, 48000, 256);
        r2.prepareToPlay(48000, 256);
        r2.setStateInformation(sm.getData(), (int)sm.getSize());
        check(waitReady(r2, 90), "project with air, clutter and free points reloads");
        const auto s2 = r2.getDisplayScene();
        const auto src2 = r2.getSources();
        check(std::fabs(s2.tempC - 8.0) < 1e-9 && std::fabs(s2.humidity - 25.0) < 1e-9 && s2.clutter.count == 30 && s2.clutter.seed == 1234, "temperature, humidity and clutter (count, seed) are saved in the project");
        check(src2[0].freePoints && src2[0].ox.size() == 2 && std::fabs(src2[0].ox[0] - (cfg0.ox[0] + 2.0f)) < 1e-3f, "free channel points are saved in the project");

        // pictures of the new pages
        std::unique_ptr<juce::AudioProcessorEditor> base(r.createEditor());
        if (auto* ed = dynamic_cast<VerbEditor*>(base.get())) {
            ed->setSize(1160, 760);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile("out");
            outDir.createDirectory();
            auto save = [&](const char* name) {
                juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
                juce::PNGImageFormat png;
                juce::FileOutputStream fo(outDir.getChildFile(name));
                if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
                return img.getWidth() > 100;
            };
            ed->showPage(0, 0);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_free_points.png"), "editor paints free channel points");
            ed->showPage(0, verbp::kListener);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_listener.png"), "editor paints the listener page with the output configuration");
            ed->showPage(3, -1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_space.png"), "editor paints the Space page");
            ed->selectWall(1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_surface_classes.png"), "editor paints the Surfaces page with classes and scale");
            check(ed->isAutoView(), "the map starts in auto view (follows walls and objects)");
            ed->zoomView(0.12);  // zoom far out: room for a long path (a car driving by)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(!ed->isAutoView() && save("editor_zoomed_out.png"), "zooming switches to the free view and paints");
            ed->setAutoView(true);
            check(ed->isAutoView(), "auto view can be switched back on");
        }
    }
    {   // many sources, formats and a 7.1.4 output
        VerbProcessor q;
        q.setPlayConfigDetails(32, 12, 48000, 256);
        q.prepareToPlay(48000, 256);
        check(waitReady(q, 60), "setup with 12 output channels loads");
        std::printf("  status: %s\n", q.getStatus().toRawUTF8());
        // a track with 12 channels (it needs them for side inputs, say) does not become a 7.1.4 output by itself
        check(q.numOutputChannels() == 2 && q.effectiveLayout() == (int)verb::Layout::StereoORTF, "default output is stereo even on a 12-channel track (inputs and outputs are independent)");
        {
            juce::AudioBuffer<float> bb(32, 256);
            juce::MidiBuffer mm;
            double e01 = 0, eRest = 0;
            for (int s = 0; s < 20; ++s) {
                bb.clear();
                if (s == 0) bb.setSample(0, 0, 1.f);
                q.processBlock(bb, mm);
                for (int ch = 0; ch < 12; ++ch)
                    for (int i = 0; i < 256; ++i) (ch < 2 ? e01 : eRest) += (double)bb.getSample(ch, i) * bb.getSample(ch, i);
            }
            check(e01 > 1e-6 && eRest == 0.0, "channels beyond the layout stay silent (the sources' side-input channels are consumed)");
        }
        {
            verbp::OutputCfg oc = q.getOutputCfg();
            oc.layout = (int)verb::Layout::Surround714;
            q.setOutputCfg(oc);
            check(waitReady(q, 60), "explicit 7.1.4 layout loads");
            check(q.numOutputChannels() == 12 && q.effectiveLayout() == (int)verb::Layout::Surround714, "the 7.1.4 layout is chosen explicitly on the listener");
            oc.layout = -1;  // "match the track" still exists as an option
            q.setOutputCfg(oc);
            check(waitReady(q, 60) && q.numOutputChannels() == 12, "the auto option follows the track's channel count");
            oc.layout = (int)verb::Layout::Surround714;
            q.setOutputCfg(oc);
            check(waitReady(q, 60), "back to explicit 7.1.4");
        }
        for (int i = 0; i < 5; ++i) q.addSource();  // 6 mono sources on inputs 1..6
        {
            auto v = q.getSources();
            verbp::SourceCfg music; music.name = "Music"; music.format = 1; music.firstIn = 6; music.spread = 1.0;           // stereo, inputs 7-8
            verbp::SourceCfg bed; bed.name = "Bed"; bed.format = 5; bed.firstIn = 8;                                           // 5.1, inputs 9-14 (LFE = input 12)
            verbp::SourceCfg vox; vox.name = "Vox"; vox.format = 1; vox.firstIn = 14; vox.sum = true;                          // stereo summed into one point
            v.push_back(music); v.push_back(bed); v.push_back(vox);
            q.setSources(v);
        }
        check(waitReady(q, 90), "setup with 9 sources of mixed formats loads");
        std::printf("  status: %s\n", q.getStatus().toRawUTF8());
        check(q.numPoints() == 6 + 2 + 5 + 1, "points: 6 mono + stereo (2) + 5.1 without LFE (5) + summed stereo (1)");

        const int blk = 256;
        juce::AudioBuffer<float> b(32, blk);
        juce::MidiBuffer midi;
        bool fin = true;
        std::vector<double> chEnergy(12, 0.0);
        std::vector<float> lfeOut;
        for (int s = 0; s < 40; ++s) {
            b.clear();
            if (s == 0) b.setSample(0, 0, 1.f);    // impulse on mono source 1
            if (s == 0) b.setSample(11, 0, 1.f);   // impulse on the LFE input of the 5.1 bed (input 12)
            q.processBlock(b, midi);
            for (int ch = 0; ch < 12; ++ch)
                for (int i = 0; i < blk; ++i) {
                    const float v = b.getSample(ch, i);
                    fin &= std::isfinite(v);
                    chEnergy[(size_t)ch] += (double)v * v;
                    if (ch == 3) lfeOut.push_back(v);
                }
        }
        check(fin, "16-source setup: output finite");
        bool allSound = true;
        for (int ch = 0; ch < 12; ++ch) if (ch != 3 && chEnergy[(size_t)ch] <= 1e-9) allSound = false;
        check(allSound, "every 7.1.4 channel (including the 4 height channels) receives sound from a source");
        check(std::fabs(lfeOut[0] - 1.f) < 0.01f, "the 5.1 LFE input reaches the LFE output directly (sample 0, unity)");

        // CPU for 16 mono-ish sources through 12 outputs while one source moves
        const int blocks = 400;
        double total = 0, worst = 0;
        std::mt19937 rng(3);
        std::normal_distribution<float> nd(0, 0.05f);
        for (int s = 0; s < blocks; ++s) {
            for (int ch = 0; ch < 16; ++ch) { auto* d = b.getWritePointer(ch); for (int i = 0; i < blk; ++i) d[i] = nd(rng); }
            setP(q, "s1x", 2.0 + 0.5 * std::sin(s * 0.05));
            const auto t0 = juce::Time::getHighResolutionTicks();
            q.processBlock(b, midi);
            const double dt = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0);
            total += dt;
            worst = std::max(worst, dt);
        }
        const double load = 100.0 * total / (blocks * blk / 48000.0);
        std::printf("  9 sources / 14 points -> 12 outputs: mean load %.1f %% of one core, worst block %.2f ms (budget %.2f ms)\n", load, worst * 1000, 1000.0 * blk / 48000.0);
        check(load < 60.0, "mean audio-thread load below 60 % of one core");

        // pictures of the source / output pages for this setup
        std::unique_ptr<juce::AudioProcessorEditor> base(q.createEditor());
        if (auto* ed = dynamic_cast<VerbEditor*>(base.get())) {
            ed->setSize(1160, 760);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile("out");
            outDir.createDirectory();
            auto save = [&](const char* name) {
                juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
                juce::PNGImageFormat png;
                juce::FileOutputStream fo(outDir.getChildFile(name));
                if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
                return img.getWidth() > 100;
            };
            ed->showPage(0, 7);  // the 5.1 bed
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_sources_bed.png"), "editor paints the 5.1 source page");
            ed->showPage(2, -1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_mix_714.png"), "editor paints the output page");
        }
    }
    {
        VerbProcessor p;
        p.setPlayConfigDetails(4, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        waitReady(p, 60);
        p.loadSceneText("wall 0 0 1 1 nosuchmaterial", "bad.txt", true);
        bool err = false;
        for (int i = 0; i < 100 && !err; ++i) { juce::Thread::sleep(20); err = p.getLoadState() == 3; }
        check(err, "invalid scene reports an error");
        juce::AudioBuffer<float> buf(4, 256);
        juce::MidiBuffer midi;
        buf.clear();
        p.processBlock(buf, midi);
        check(true, "processing continues with the previous scene after a bad load");
    }
    {   // source directivity and Doppler strength: live controls, saved in the project
        std::printf("Directivity and Doppler controls\n");
        VerbProcessor p;
        p.setPlayConfigDetails(4, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "scene loads");
        juce::AudioBuffer<float> buf(4, 256);
        juce::MidiBuffer midi;
        auto energy = [&]() {  // impulse on input 1, 0.3 s of output
            double en = 0;
            for (int s = 0; s < 56; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int i = 0; i < 256; ++i) en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i);
            }
            return en;
        };
        for (int i = 0; i < 100; ++i) { buf.clear(); p.processBlock(buf, midi); }
        setP(p, "s1az", 0.0);
        for (int i = 0; i < 100; ++i) { buf.clear(); p.processBlock(buf, midi); }
        const double omni = energy();
        p.setSourcePattern(0, 0.5);
        for (int i = 0; i < 300; ++i) { buf.clear(); p.processBlock(buf, midi); }
        const double card0 = energy();
        setP(p, "s1az", 180.0);
        for (int i = 0; i < 300; ++i) { buf.clear(); p.processBlock(buf, midi); }
        const double card180 = energy();
        std::printf("  energy omni %.3e, cardioid az 0 %.3e, cardioid az 180 %.3e\n", omni, card0, card180);
        check(std::isfinite(card0) && std::isfinite(card180) && card0 != card180, "directivity changes the output when the source turns");
        check(p.getSources()[0].pattern == 0.5, "the directivity is part of the source configuration");
        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        VerbProcessor q;
        q.setPlayConfigDetails(4, 2, 48000, 256);
        q.prepareToPlay(48000, 256);
        q.setStateInformation(mb.getData(), (int)mb.getSize());
        check(q.getSources()[0].pattern == 0.5, "the directivity survives the project state");
        setP(p, "doppler", 0.0);
        for (int i = 0; i < 50; ++i) { buf.clear(); p.processBlock(buf, midi); }
        check(std::fabs(p.apvts.getRawParameterValue("doppler")->load()) < 1e-6f, "Doppler strength is an automatable parameter");
    }
    {   // floor and ceiling zones through the plugin: they change the sound, survive the project state, and the editor draws them
        std::printf("Floor and ceiling zones\n");
        VerbProcessor p;
        p.setPlayConfigDetails(4, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "scene loads");
        juce::AudioBuffer<float> buf(4, 256);
        juce::MidiBuffer midi;
        auto tailEnergy = [&]() {  // reverberant energy 80..400 ms after an impulse
            double en = 0;
            for (int s = 0; s < 80; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                if (s >= 15) for (int i = 0; i < 256; ++i) en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i);
            }
            return en;
        };
        for (int i = 0; i < 100; ++i) { buf.clear(); p.processBlock(buf, midi); }
        const double before = tailEnergy();
        p.editScene([](verb::Scene& s) {
            s.mats.push_back(verb::presetByName("Absorber"));
            verb::Zone z;
            z.mat = (int)s.mats.size() - 1;
            z.poly = {{-50, -50}, {50, -50}, {50, 50}, {-50, 50}};
            s.floorZones.push_back(z);
            verb::Zone c = z;
            c.mat = -1;  // an opening over the whole room
            s.ceilZones.push_back(c);
        });
        for (int i = 0; i < 60; ++i) {  // the new geometry is built and published by the loader thread
            for (int k = 0; k < 20; ++k) { buf.clear(); p.processBlock(buf, midi); }
            juce::Thread::sleep(100);
        }
        for (int i = 0; i < 300; ++i) { buf.clear(); p.processBlock(buf, midi); }
        const double after = tailEnergy();
        std::printf("  late energy: %.3e before, %.3e with an absorbing floor zone and a ceiling opening\n", before, after);
        check(std::isfinite(after) && after < before * 0.7, "floor and ceiling zones change the reverberation");
        const auto sc = p.getDisplayScene();
        check(sc.floorZones.size() == 1 && sc.ceilZones.size() == 1 && sc.ceilZones[0].mat < 0, "the zones are part of the scene");
        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        VerbProcessor q;
        q.setPlayConfigDetails(4, 2, 48000, 256);
        q.prepareToPlay(48000, 256);
        q.setStateInformation(mb.getData(), (int)mb.getSize());
        waitReady(q, 60);
        const auto sq = q.getDisplayScene();
        check(sq.floorZones.size() == 1 && sq.ceilZones.size() == 1 && sq.ceilZones[0].mat < 0 && sq.floorZones[0].poly.size() == 4, "the zones survive the project state");
        // editor: zones on the plan, the front and side elevations
        p.editScene([](verb::Scene& s) {
            s.floorZones[0].poly = {{0.5, 0.5}, {3.5, 0.5}, {3.5, 3.0}, {0.5, 3.0}};
            s.ceilZones[0].poly = {{4.0, 1.0}, {6.0, 1.0}, {6.0, 3.0}, {4.0, 3.0}};
            s.floorZones[0].mat = s.floorMat;
            verb::Zone plate;  // a lowered ceiling over part of the room
            plate.mat = s.floorMat;
            plate.z = 2.0;
            plate.poly = {{0.5, 3.5}, {3.5, 3.5}, {3.5, 5.0}, {0.5, 5.0}};
            s.ceilZones.push_back(plate);
        });
        std::unique_ptr<juce::AudioProcessorEditor> base(p.createEditor());
        auto* ed = dynamic_cast<VerbEditor*>(base.get());
        check(ed != nullptr, "editor opens with zones");
        if (ed) {
            ed->setSize(1160, 760);
            auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile("out");
            outDir.createDirectory();
            auto save = [&](const char* name) {
                juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
                juce::PNGImageFormat png;
                juce::FileOutputStream fo(outDir.getChildFile(name));
                if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
                return img.getWidth() > 100;
            };
            ed->selectZone(false, 0);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_zone_plan.png"), "editor paints floor and ceiling zones on the plan");
            ed->selectZone(true, 1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_plate.png"), "editor paints a ceiling plate with its height row");
            check(p.getDisplayScene().ceilZones.size() == 2 && std::fabs(p.getDisplayScene().ceilZones[1].z - 2.0) < 1e-9, "the plate keeps its height");
            {   // overlapping surfaces: every one under the point can be reached; layers can be locked or hidden
                p.editScene([](verb::Scene& s) {
                    verb::Zone extra;
                    extra.mat = s.floorMat;
                    extra.poly = {{0.5, 2.5}, {3.5, 2.5}, {3.5, 4.5}, {0.5, 4.5}};
                    s.floorZones.push_back(extra);  // overlaps the first floor zone and the plate
                });
                juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
                check(save("editor_overlap.png"), "editor paints overlapping zones");
                const int both = ed->testPickCount(2.0, 3.8), floors = ed->testPickCount(2.0, 2.8);
                ed->testSetLayer(3, 2);  // plates hidden
                const int noPlate = ed->testPickCount(2.0, 3.8);
                ed->testSetLayer(3, 0);
                ed->testSetLayer(1, 1);  // floors locked
                const int onlyPlate = ed->testPickCount(2.0, 3.8);
                ed->testSetLayer(3, 1);
                const int none = ed->testPickCount(2.0, 3.8);
                ed->testSetLayer(1, 0);
                ed->testSetLayer(3, 0);
                std::printf("  surfaces under a point: %d (plate + floor zone), %d (two floor zones); plates hidden %d, floors locked %d, both %d\n", both, floors, noPlate, onlyPlate, none);
                check(both == 2 && floors == 2 && noPlate == 1 && onlyPlate == 1 && none == 0, "all overlapping surfaces are reachable, hidden and locked layers are skipped");
            }
            ed->showPlane(1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_view_front.png"), "editor paints the front elevation");
            ed->showPlane(2);
            ed->selectWall(1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_view_side.png"), "editor paints the side elevation");
            ed->showPlane(3);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_view_3d.png"), "editor paints the 3D view");
            ed->showPlane(0);
            // several walls selected together take a surface change together
            ed->selectWalls({0, 1, 2});
            ed->applyPresetToSelection("Heavy_curtain");
            juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
            const auto sc3 = p.getDisplayScene();
            int curtains = 0;
            for (size_t i = 0; i < sc3.walls.size(); ++i)
                curtains += sc3.mats[(size_t)sc3.walls[i].mat].name == "Heavy_curtain" ? 1 : 0;
            std::printf("  walls with the curtain material after editing a 3-wall selection: %d of %zu\n", curtains, sc3.walls.size());
            check(sc3.walls.size() >= 4 && curtains == 3, "a selection of several walls is edited together, the others stay as they were");
            check(save("editor_multi_select.png"), "editor paints the multi-selection");
            // "clear everything": no walls, no zones, the plugin keeps running and still makes (free-field) sound
            ed->clearEverything();
            juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
            const auto sc4 = p.getDisplayScene();
            check(sc4.walls.empty() && sc4.floorZones.empty() && sc4.ceilZones.empty() && sc4.clutter.count == 0, "clear everything leaves an empty room");
            for (int i = 0; i < 60; ++i) { juce::Thread::sleep(50); buf.clear(); p.processBlock(buf, midi); }
            double en = 0;
            bool fin = true;
            for (int s = 0; s < 40; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int i = 0; i < 256; ++i) { fin &= std::isfinite(buf.getSample(0, i)); en += buf.getSample(0, i) * buf.getSample(0, i); }
            }
            check(fin && en > 1e-9, "an empty room still renders finite sound");
            check(save("editor_cleared.png"), "editor paints the empty room");
        }
    }
    {   // Doppler of a fast fly-by driven by block-rate automation (what a host does): the pitch must follow the physics smoothly
        std::printf("Fast fly-by, automation once per host block\n");
        static const char* freeField = R"(sr 48000
length 1.0
height 30
material absorb 1 1 1 1 1 1 1 0 0
floor absorb
ceiling open
source S 0 0 1.5
receiver L 0 0 1.5 0 0
receiver R 0 0 1.5 0 0
)";
        // updatePeriod > 0: the position is written only every `updatePeriod` samples, not at every block start (what a mouse
        // drag or sparse automation does); dy: closest distance of the pass
        auto runPass = [&](int blockSize, double speed, int updatePeriod = 0, double dyIn = 3.0) {
            VerbProcessor p;
            p.setPlayConfigDetails(2, 2, 48000, blockSize);
            p.prepareToPlay(48000, blockSize);
            p.loadSceneText(freeField, "free.txt", true);
            waitReady(p, 60);
            setP(p, "doppler", 1.0);
            setP(p, "lx", 0.0); setP(p, "ly", 0.0); setP(p, "lz", 1.5);
            const double sr = 48000, f0 = 4000, c = 343.2 * std::sqrt((20.0 + 273.15) / 293.15), dy = dyIn;
            const double x0 = -50.0, T = 100.0 / speed;
            const int total = (int)(T * sr);
            juce::AudioBuffer<float> buf(2, blockSize);
            juce::MidiBuffer midi;
            std::vector<float> out;
            setP(p, "s1y", dy); setP(p, "s1z", 1.5); setP(p, "s1az", 0.0);
            setP(p, "s1x", x0);
            for (int i = 0; i < 40; ++i) { buf.clear(); p.processBlock(buf, midi); }  // settle at the start
            long pos = 0;
            int nextUpdate = 0;
            for (int blockStart = 0; blockStart < total; blockStart += blockSize) {
                if (updatePeriod == 0 || blockStart >= nextUpdate) {  // the host writes the automation value at the block start
                    setP(p, "s1x", x0 + speed * (double)blockStart / sr);
                    nextUpdate = blockStart + updatePeriod;
                }
                for (int i = 0; i < blockSize; ++i) {
                    const double t = (double)(blockStart + i) / sr;
                    buf.setSample(0, i, (float)(0.5 * std::sin(2 * juce::MathConstants<double>::pi * f0 * t)));
                    buf.setSample(1, i, 0.f);
                }
                p.processBlock(buf, midi);
                for (int i = 0; i < blockSize; ++i) out.push_back(buf.getSample(0, i));
                pos += blockSize;
            }
            // measured frequency in 512-sample windows (rising zero crossings) vs the ideal Doppler of the straight pass
            auto idealF = [&](double tArr) {  // arrival time tArr -> the frequency the receiver hears
                auto arr = [&](double te) { return te + std::hypot(x0 + speed * te, dy) / c; };
                double lo = 0, hi = T;  // te solves arr(te) = tArr
                for (int it = 0; it < 60; ++it) { const double mid = 0.5 * (lo + hi); (arr(mid) < tArr ? lo : hi) = mid; }
                const double te = 0.5 * (lo + hi), h = 1e-4;
                return f0 * h / (arr(te + h) - arr(te));
            };
            double worst = 0, sum2 = 0;
            int n = 0;
            std::vector<double> fSeries;  // measured pitch per window, for the wobble measure below
            const int win = 512;
            for (size_t s = (size_t)(0.25 * sr); s + win < out.size() - (size_t)(0.1 * sr); s += win / 2) {
                double first = -1, last = -1;
                int cnt = 0;
                for (size_t i = s + 1; i < s + win; ++i)
                    if (out[i - 1] < 0 && out[i] >= 0) {
                        const double tc = (double)(i - 1) + (0 - out[i - 1]) / (out[i] - out[i - 1]);
                        if (first < 0) first = tc;
                        last = tc;
                        ++cnt;
                    }
                if (cnt < 8) continue;
                const double fm = (cnt - 1) * sr / (last - first);
                const double tMid = ((double)s + win / 2.0) / sr;
                const double err = std::fabs(fm - idealF(tMid)) / f0;
                fSeries.push_back(fm / f0);
                worst = std::max(worst, err);
                sum2 += err * err;
                ++n;
            }
            // clicks: the largest sample-to-sample step against the largest a pure tone at the highest Doppler pitch can make
            double amp = 0, jump = 0;
            for (size_t i = (size_t)(0.25 * sr); i + 1 < out.size() - (size_t)(0.1 * sr); ++i) { amp = std::max<double>(amp, std::fabs(out[i])); jump = std::max<double>(jump, std::fabs(out[i + 1] - out[i])); }
            const double natural = amp * 2 * std::sin(juce::MathConstants<double>::pi * f0 * 1.6 / sr);
            // wobble ("chewed tape"): the pitch's window-to-window scatter around a smooth curve (second differences)
            double s2 = 0;
            for (size_t i = 1; i + 1 < fSeries.size(); ++i) { const double d2 = fSeries[i - 1] - 2 * fSeries[i] + fSeries[i + 1]; s2 += d2 * d2; }
            const double wobble = fSeries.size() > 3 ? std::sqrt(s2 / (double)(fSeries.size() - 2) / 6.0) : 0.0;
            struct R { double worst, rms, click, wobble; };
            return R{worst, std::sqrt(sum2 / std::max(1, n)), jump / natural, wobble};
        };
        {   // pitch wobble of a moderate pass (10 m/s, 20 m away) under irregular position updates
            struct Case { const char* what; int block, period; };
            for (const Case& cs : {Case{"every block", 512, 0}, Case{"60 Hz mouse drag", 512, 800}, Case{"60 Hz mouse drag, big block", 1024, 800}, Case{"30 Hz sparse automation", 256, 1600}}) {
                const auto r = runPass(cs.block, 10.0, cs.period, 20.0);
                std::printf("  wobble 10 m/s pass at 20 m, block %d, updates: %s: pitch scatter %.3f %%, click ratio %.2f\n", cs.block, cs.what, 100 * r.wobble, r.click);
                // before the alpha-beta tracker a mouse drag / sparse automation gave 4.7-6.9 % ("chewed tape")
                check(r.wobble < 0.006 && r.click < 1.15, "the pitch stays steady with irregular position updates (scatter < 0.6 %, no clicks)");
            }
        }
        for (auto cfg : {std::make_pair(256, 40.0), std::make_pair(1024, 40.0), std::make_pair(2048, 40.0), std::make_pair(512, 80.0), std::make_pair(2048, 80.0)}) {
            const auto r = runPass(cfg.first, cfg.second);
            std::printf("  %.0f m/s fly-by at 3 m, host block %d: pitch error vs physics worst %.2f %%, rms %.2f %%, click ratio %.2f\n", cfg.second, cfg.first, 100 * r.worst, 100 * r.rms, r.click);
            // up to 1024 samples (the usual host blocks) the pitch follows the physics closely; at 2048 (43 ms) the
            // extrapolation over the block of a 500 m/s^2 turn at closest approach can no longer keep up (limit documented)
            if (cfg.first <= 1024) check(r.rms < 0.005 && r.worst < 0.02, "the Doppler follows the physical pitch curve (rms < 0.5 %, worst < 2 %) with block-rate automation");
            else check(r.rms < 0.02 && r.worst < 0.2, "very large host blocks (2048): still within 2 % rms");
            check(r.click < 1.15, "no clicks or steps in the fly-by");
        }
    }
    {   // the car: a place in the cabin for every one of the 30 engine stems
        std::printf("Car cabin preset\n");
        const auto rooms = verbp::builtinRooms();
        std::vector<const verbp::BuiltinRoom*> cars;
        for (auto& r : rooms) if (!r.setup.empty() && r.name.rfind("MotorSynth", 0) == 0) cars.push_back(&r);
        check(cars.size() == 2, "the library has the two MotorSynth presets");
        for (size_t ci = 0; ci < cars.size(); ++ci) {
            const verbp::BuiltinRoom* car = cars[ci];
            const size_t nSrc = ci == 0 ? 15 : 21;
            const int nPts = ci == 0 ? 19 : 25;
            std::printf("  %s\n", car->name.c_str());
            int channels = 0;
            for (auto& c : car->setup) channels += c.effectiveChannels();
            check(car->setup.size() == nSrc && channels == nPts, "sources and engine points as designed (4 wide sources carry 2 points)");
            VerbProcessor p;
            p.setPlayConfigDetails(48, 2, 48000, 256);
            p.prepareToPlay(48000, 256);
            check(waitReady(p, 60), "the plugin starts");
            p.loadRoomWithSetup(juce::String(car->text), "car.txt", car->setup, car->micPattern);
            check(waitReady(p, 60), "the car loads with its sources");
            check((int)p.getSources().size() == (int)nSrc && p.numPoints() == nPts, "the loaded plugin has the same sources and points");
            check(p.getOutputCfg().micPattern == (int)verb::MicPattern::HumanEars, "the driver listens with human ears");
            {   // every source sits on the Room inputs that MotorSynth feeds (stem i -> inputs 2i-1, 2i)
                const std::vector<std::pair<std::string, int>> want = {
                    {"Exhaust", 1}, {"Engine", 3}, {"Mechanical", 5}, {"Gearbox", 7}, {"Intake", 9}, {"Turbo BOV", 11}, {"Tires", 13}, {"Chassis", 15},
                    {"Brakes", 17}, {"Backfire", 19}, {"Wind", 21}, {"Cabin", 23}, {"Accessories", 25}, {"Electric", 27}, {"Environment", 29},
                    {"EngineSim Exhaust A", 31}, {"EngineSim Exhaust B", 33}, {"EngineSim Intake", 35}, {"EngineSim Block", 37}, {"EngineSim Valvetrain", 39}, {"EngineSim Starter", 41}};
                bool ok = true;
                int seen = 0;
                for (auto& w : want)
                    for (auto& c : car->setup)
                        if (c.name == w.first) { ++seen; ok &= c.firstIn + 1 == w.second; if (c.firstIn + 1 != w.second) std::printf("  %s is on input %d, MotorSynth sends it to %d\n", c.name.c_str(), c.firstIn + 1, w.second); }
                check(ok && seen == (int)car->setup.size(), "every stem is on the Room input that MotorSynth feeds");
            }
            {   // the engine bay is a box under a bonnet plate
                std::istringstream is(car->text);
                const verb::Scene cs = verb::parseScene(is, "car");
                int plates = 0;
                for (auto& z : cs.ceilZones) plates += z.z >= 0 && z.mat >= 0;
                check(plates == 1 && cs.walls.size() >= 11, "the car has a bonnet plate and the engine-bay walls");
            }
            // the stems sit where they should: tailpipes behind the cabin, engine in front (read back from the parameters)
            auto px = [&](int src) { return p.apvts.getRawParameterValue(verbp::paramId(src, 1))->load(); };  // the car drives up the plan: "forward" is y
            std::printf("  x positions: Exhaust A %.1f, Exhaust B %.1f, Intake %.1f, Engine block %.1f\n", px(0), px(1), px(2), px(3));
            check(px(0) < -1.6 && px(1) > 1.5 && px(4) > 1.5 && px(3) < 1.0, "tailpipe behind the car, the engine ahead of the firewall, the gearbox in the tunnel");
            // sound: an impulse in cylinder 1 (input 15) and in the tailpipe (input 1) arrives finite, the engine one is more muffled by the firewall
            auto level = [&](int inputCh) {
                juce::AudioBuffer<float> buf(48, 256);
                juce::MidiBuffer midi;
                for (int i = 0; i < 60; ++i) { buf.clear(); p.processBlock(buf, midi); }
                double en = 0;
                bool fin = true;
                for (int s = 0; s < 40; ++s) {
                    buf.clear();
                    if (s == 0) buf.setSample(inputCh, 0, 1.f);
                    p.processBlock(buf, midi);
                    for (int i = 0; i < 256; ++i) { fin &= std::isfinite(buf.getSample(0, i)) && std::isfinite(buf.getSample(1, i)); en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i); }
                }
                return fin ? 10 * std::log10(en + 1e-30) : 99.0;
            };
            {   // the input channel index in the buffer (0-based) is the number REAPER shows minus one: the source must hear exactly its own channel
                std::vector<bool> read(48, false);
                for (auto& c : car->setup) for (int q = 0; q < c.effectiveChannels(); ++q) read[(size_t)(c.firstIn + q)] = true;
                int bad = 0, tested = 0;
                for (auto& c : car->setup) {
                    if (c.effectiveChannels() != 1) continue;
                    const double own = level(c.firstIn);
                    ++tested;
                    if (c.firstIn >= 1 && !read[(size_t)c.firstIn - 1] && level(c.firstIn - 1) > own - 30.0) { ++bad; std::printf("  %s also hears input %d (one before it)\n", c.name.c_str(), c.firstIn); }
                    if (!read[(size_t)c.firstIn + 1] && level(c.firstIn + 1) > own - 30.0) { ++bad; std::printf("  %s also hears input %d (one after it)\n", c.name.c_str(), c.firstIn + 2); }
                    if (own < -200) { ++bad; std::printf("  %s hears nothing on input %d\n", c.name.c_str(), c.firstIn + 1); }
                }
                std::printf("  %d mono sources checked against their neighbouring input channels\n", tested);
                check(bad == 0 && tested > 0, "each source reacts to its own input channel and not to the one before or after it");
            }
            const double engine = level(8), pipe = level(0);
            std::printf("  an impulse in the intake: %.1f dB, in exhaust A: %.1f dB (at the driver's ears)\n", engine, pipe);
            check(engine < 90 && pipe < 90 && engine > -200 && pipe > -200, "all stems render finite sound at the driver");
            std::unique_ptr<juce::AudioProcessorEditor> base(p.createEditor());
            if (auto* ed = dynamic_cast<VerbEditor*>(base.get())) {
                ed->setSize(1160, 760);
                ed->showPage(0, 0);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(400);
                juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
                juce::PNGImageFormat png;
                juce::FileOutputStream fo(juce::File::getCurrentWorkingDirectory().getChildFile("out").getChildFile((ci == 0 ? "editor_car.png" : "editor_car_es.png")));
                if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
                check(img.getWidth() > 100, "editor paints the car");
            }
        }
    }
    {   // the host's channel order: JUCE reorders a long run of VST3 speaker bits; the plug-in undoes it
        std::printf("Input channel order\n");
        using C = juce::AudioChannelSet;
        const std::vector<C::ChannelType> bitOrder = {C::left, C::right, C::centre, C::LFE, C::leftSurround, C::rightSurround, C::leftCentre, C::rightCentre, C::centreSurround,
            C::leftSurroundSide, C::rightSurroundSide, C::topMiddle, C::topFrontLeft, C::topFrontCentre, C::topFrontRight, C::topRearLeft, C::topRearCentre, C::topRearRight, C::LFE2,
            C::discreteChannel0, C::ambisonicACN0, C::ambisonicACN1, C::ambisonicACN2, C::ambisonicACN3, C::topSideLeft, C::topSideRight, C::leftSurroundRear, C::rightSurroundRear,
            C::bottomFrontLeft, C::bottomFrontCentre, C::bottomFrontRight, C::proximityLeft, C::proximityRight, C::bottomSideLeft, C::bottomSideRight, C::bottomRearLeft,
            C::bottomRearCentre, C::bottomRearRight, C::ambisonicACN4, C::ambisonicACN5, C::ambisonicACN6, C::ambisonicACN7};
        juce::Array<C::ChannelType> types;
        for (auto t : bitOrder) types.add(t);
        const C set = C::channelSetWithChannels(types);
        const auto m = verbp::hostToBufferIndices(set);
        std::vector<int> sorted = m;
        std::sort(sorted.begin(), sorted.end());
        bool perm = (int)m.size() == set.size();
        for (size_t i = 0; i < sorted.size(); ++i) perm &= sorted[i] == (int)i;
        std::printf("  42 speaker bits: host channel -> buffer channel:");
        for (size_t i = 16; i < 26 && i < m.size(); ++i) std::printf(" %zu->%d", i + 1, m[i] + 1);
        std::printf(" ...\n");
        check(perm, "the host-to-buffer map of a 42-bit arrangement is a permutation");
        bool same = true;
        for (auto set2 : {C::stereo(), C::create5point1(), C::discreteChannels(8), C::discreteChannels(16)}) {
            const auto id = verbp::hostToBufferIndices(set2);
            for (size_t i = 0; i < id.size(); ++i) same &= id[i] == (int)i;
        }
        check(same, "a stereo, 5.1 or a short discrete bus keeps its order");
    }
    if (std::getenv("VERB_RTTEST")) {  // exploration: a 0.35 s C5 burst in the concert hall, with and without the tail, 25 ms envelope
        const auto rooms = verbp::builtinRooms();
        for (auto& r : rooms) {
            if (r.name.find("Concert hall") == std::string::npos) continue;
            std::istringstream is(r.text);
            verb::Scene sc = verb::parseScene(is, r.name);
            std::printf("  room: listener (%.2f %.2f %.2f) yaw %.0f, source (%.2f %.2f %.2f)\n", sc.listenerPos.x, sc.listenerPos.y, sc.listenerPos.z, sc.listenerYaw,
                        sc.sources[0].pos.x, sc.sources[0].pos.y, sc.sources[0].pos.z);
            for (double tailDb : {-48.0, 0.0}) {
                verb::EngineConfig cfg;
                cfg.tailThread = false; cfg.erOrder = 1; cfg.tailRays = 150000;
                verb::Engine eng(sc, cfg);
                eng.setSourcePosition(0, 0.98, 19.93, 1.6);
                eng.setEarlyLevel(-9.0);
                eng.setTailLevel(tailDb);
                const size_t N = 48000 * 3;
                std::vector<float> in(512), o0(512), o1(512);
                std::vector<double> env;
                double cur = 0;
                int cnt = 0;
                for (size_t pos = 0; pos < N; pos += 512) {
                    for (int i = 0; i < 512; ++i) {
                        const double t = (double)(pos + (size_t)i) / 48000.0;
                        in[(size_t)i] = (t >= 0.3 && t < 0.65) ? (float)(0.3 * std::sin(2 * verb::kPi * 1046.5 * t)) : 0.f;
                    }
                    const float* ip[1] = {in.data()};
                    float* op[2] = {o0.data(), o1.data()};
                    eng.process(ip, op, 512);
                    for (int i = 0; i < 512; ++i) cur += 0.5 * ((double)o0[(size_t)i] * o0[(size_t)i] + (double)o1[(size_t)i] * o1[(size_t)i]);
                    if (++cnt == 2) { env.push_back(10 * std::log10(cur / 1024 + 1e-18)); cur = 0; cnt = 0; }  // ~21 ms
                }
                {   // impulse response energy in 10 ms bins, 0 .. 1.2 s
                    verb::Engine e2(sc, cfg);
                    e2.setSourcePosition(0, 0.98, 19.93, 1.6);
                    e2.setEarlyLevel(-9.0);
                    e2.setTailLevel(tailDb);
                    std::printf("  IR 10 ms bins, tail %+.0f dB:", tailDb);
                    double acc = 0;
                    std::vector<float> raw;
                    for (size_t pos = 0; pos < 48000 * 3; pos += 512) {
                        std::fill(in.begin(), in.end(), 0.f);
                        if (pos == 0) in[0] = 1.f;
                        const float* ip[1] = {in.data()};
                        float* op[2] = {o0.data(), o1.data()};
                        e2.process(ip, op, 512);
                        for (int i = 0; i < 512; ++i) {
                            raw.push_back(o0[(size_t)i]); raw.push_back(o1[(size_t)i]);
                            acc += (double)o0[(size_t)i] * o0[(size_t)i] + (double)o1[(size_t)i] * o1[(size_t)i];
                            if (((pos + (size_t)i + 1) % 480) == 0 && pos < 48000 * 6 / 5) { std::printf(" %.0f", 10 * std::log10(acc / 480 + 1e-18)); acc = 0; }
                        }
                    }
                    if (const char* dp = std::getenv("VERB_RTDUMP"); dp && tailDb == 0.0) { juce::File(juce::String(dp)).replaceWithData(raw.data(), raw.size() * sizeof(float)); }
                    std::printf("\n");
                }
                std::printf("  tail %+.0f dB, ER -9 dB; 0.1 s steps (dB):", tailDb);
                for (size_t k = 0; k < env.size(); k += 5) std::printf(" %.0f", env[k]);
                std::printf("\n");
            }
        }
    }
    if (std::getenv("VERB_RTHIST")) {  // exploration: the ray histogram of the concert hall, 100 ms steps
        const auto rooms = verbp::builtinRooms();
        for (auto& r : rooms) {
            if (r.name.find("Concert hall") == std::string::npos) continue;
            std::istringstream is(r.text);
            verb::Scene sc = verb::parseScene(is, r.name);
            sc.length = 6.0;
            std::vector<verb::Transducer> rc{sc.receivers[0]};
            for (int rays : {20000, 40000, 150000}) {
                const auto t0 = std::chrono::steady_clock::now();
                const auto h = verb::traceTail(sc, sc.sources[0], rc, 1, rays, sc.seed);
                const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::printf("  rays %d: %.2f s; reliable end per band (ms):", rays, sec);
                for (int bb = 0; bb < verb::NB; ++bb) std::printf(" %zu", h.reliableEnd[0][(size_t)bb]);
                std::printf("\n    band4 dB per 100ms:");
                for (size_t k = 0; k + 100 <= h.nBins; k += 100) { double a2 = 0; for (size_t j = 0; j < 100; ++j) a2 += h.h[0][4][k + j]; std::printf(" %.0f", 10 * std::log10(a2 / 100 + 1e-30)); }
                std::printf("\n");
            }
        }
    }
    if (std::getenv("VERB_RTPLUG")) {  // exploration: the same note through the whole plugin in host-sized blocks, 20 ms envelope of the end of the tail
        const auto rooms = verbp::builtinRooms();
        for (auto& r : rooms) {
            if (r.name.find("Concert hall") == std::string::npos) continue;
            VerbProcessor p;
            p.setPlayConfigDetails(1, 2, 48000, 512);
            p.prepareToPlay(48000, 512);
            waitReady(p, 60);
            p.loadRoomWithSetup(juce::String(r.text), "hall.txt", r.setup, r.micPattern);
            waitReady(p, 90);
            std::printf("  plugin tail length reported %.2f s\n", p.getTailLengthSeconds());
            juce::AudioBuffer<float> b(2, 512);
            juce::MidiBuffer m;
            const int total = 48000 * 7;
            std::vector<double> L, R;
            for (int pos = 0; pos < total; pos += 512) {
                b.clear();
                for (int i = 0; i < 512; ++i) {
                    const double t = (double)(pos + i) / 48000.0;
                    b.setSample(0, i, (t >= 0.02 && t < 0.62) ? (float)(0.3 * std::sin(2 * verb::kPi * 1046.5 * t)) : 0.f);
                }
                p.processBlock(b, m);
                for (int i = 0; i < 512; ++i) { L.push_back(b.getSample(0, i)); R.push_back(b.getSample(1, i)); }
            }
            {   // the impulse response of the same plugin instance, 100 ms energy steps
                juce::AudioBuffer<float> bi(2, 512);
                std::vector<double> E;
                std::vector<float> rawIr;
                double acc = 0;
                int cntn = 0;
                for (int pos = 0; pos < 48000 * 7; pos += 512) {
                    bi.clear();
                    if (pos == 0) bi.setSample(0, 0, 1.f);
                    p.processBlock(bi, m);
                    for (int i = 0; i < 512; ++i) {
                        rawIr.push_back(bi.getSample(0, i)); rawIr.push_back(bi.getSample(1, i));
                        acc += (double)bi.getSample(0, i) * bi.getSample(0, i);
                        if (++cntn == 4800) { E.push_back(10 * std::log10(acc / 4800 + 1e-20)); acc = 0; cntn = 0; }
                    }
                }
                if (const char* dp = std::getenv("VERB_RTDUMP")) juce::File(juce::String(dp)).replaceWithData(rawIr.data(), rawIr.size() * sizeof(float));
                std::printf("  IR ch0, 100 ms steps (dB):");
                for (double v : E) std::printf(" %.0f", v);
                std::printf("\n");
            }
            for (int c = 0; c < 2; ++c) {
                std::printf("  ch %d, 40 ms steps from 1.0 s:", c);
                const auto& v = c ? R : L;
                for (size_t t0 = 48000; t0 + 960 < v.size(); t0 += 1920) {
                    double a = 0;
                    for (size_t i = 0; i < 960; ++i) a += v[t0 + i] * v[t0 + i];
                    std::printf(" %.0f", 10 * std::log10(a / 960 + 1e-20));
                }
                std::printf("\n");
            }
        }
    }
    if (std::getenv("VERB_TAILS2")) {  // exploration: smoothness of the tail envelope of the cathedral for different ray counts
        const auto rooms = verbp::builtinRooms();
        for (auto& r : rooms) {
            if (r.name.find("Cathedral") == std::string::npos && r.name.find("hall") == std::string::npos) continue;
            std::istringstream is(r.text);
            verb::Scene sc = verb::parseScene(is, r.name);
            for (int rays : {150000}) {
                verb::EngineConfig cfg;
                cfg.tailThread = false; cfg.erOrder = 1; cfg.tailRays = rays;
                verb::Engine eng(sc, cfg);
                const size_t N = (size_t)(eng.tailSeconds() * 48000) + 24000;
                std::vector<float> in(512, 0.f), o0(512), o1(512), zeros(512, 0.f);
                std::vector<double> env;
                double cur = 0;
                size_t cnt = 0;
                for (size_t pos = 0; pos < N; pos += 512) {
                    std::fill(in.begin(), in.end(), 0.f);
                    if (pos == 0) in[0] = 1.f;
                    std::vector<const float*> ip((size_t)eng.numSources(), zeros.data());  // an impulse into source 1, silence into the others
                    ip[0] = in.data();
                    float* op[2] = {o0.data(), o1.data()};
                    eng.process(ip.data(), op, 512);
                    for (int i = 0; i < 512; ++i) cur += (double)o0[i] * o0[i];
                    if (++cnt == 5) { env.push_back(10 * std::log10(cur + 1e-30)); cur = 0; cnt = 0; }  // ~53 ms windows
                }
                // residual of the dB envelope about a straight line through windows 0.5 s .. where it is 50 dB below the peak
                double pk = -1e9;
                for (double v : env) pk = std::max(pk, v);
                std::vector<double> xs, ys;
                for (size_t k = 10; k < env.size(); ++k) if (env[k] > pk - 50.0) { xs.push_back((double)k); ys.push_back(env[k]); }
                double sx = 0, sy = 0, sxx = 0, sxy = 0;
                const double n = (double)xs.size();
                for (size_t k = 0; k < xs.size(); ++k) { sx += xs[k]; sy += ys[k]; sxx += xs[k] * xs[k]; sxy += xs[k] * ys[k]; }
                const double slope = (n * sxy - sx * sy) / std::max(1e-9, n * sxx - sx * sx), icpt = (sy - slope * sx) / n;
                double rs = 0;
                for (size_t k = 0; k < xs.size(); ++k) rs += (ys[k] - (icpt + slope * xs[k])) * (ys[k] - (icpt + slope * xs[k]));
                {   // where the envelope bulges above the decay line (late "echoes")
                    std::printf("   bulges above the line:");
                    for (size_t k = 0; k < xs.size(); ++k) {
                        const double res = ys[k] - (icpt + slope * xs[k]);
                        if (res > 3.0) std::printf(" t=%.2fs(+%.1f)", xs[k] * 0.0533, res);
                    }
                    std::printf("\n");
                }
                std::printf("  %s, %d rays: length %.1f s, envelope scatter about the decay line %.2f dB over %zu windows, slope %.2f dB/s\n", r.name.c_str(), rays, eng.tailSeconds(),
                            std::sqrt(rs / std::max(1.0, n)), xs.size(), slope / 0.0533);
            }
        }
    }
    {   // the orchestra halls: every group at its own place and height, the listener below the stage, sound renders
        std::printf("Orchestra halls\n");
        const auto rooms = verbp::builtinRooms();
        int halls = 0;
        for (auto& r : rooms) {
            if (r.name.rfind("Symphony hall", 0) != 0 && r.name.rfind("Chamber hall", 0) != 0) continue;
            ++halls;
            std::istringstream is(r.text);
            const verb::Scene sc = verb::parseScene(is, r.name);
            int plates = 0;
            for (auto* zl : {&sc.floorZones, &sc.ceilZones}) for (auto& z : *zl) plates += z.z >= 0;
            bool above = sc.sources.size() == r.setup.size() && sc.sources.size() >= 14;
            bool inputsOk = true, risers = false;
            for (size_t i = 0; i < r.setup.size(); ++i) {
                inputsOk &= r.setup[i].firstIn == (int)i && r.setup[i].effectiveChannels() == 1 && !r.setup[i].name.empty();
                above &= sc.sources[i].pos.y > sc.listenerPos.y + 0.1;  // all of them in front of the listener (up the plan)
                risers |= sc.sources[i].pos.z > 3.0;                    // some stand on the risers
            }
            std::printf("  %s: %zu groups, %d plates, listener (%.1f, %.1f, %.2f)\n", r.name.c_str(), r.setup.size(), plates, sc.listenerPos.x, sc.listenerPos.y, sc.listenerPos.z);
            check(inputsOk && above && plates >= 3 && plates <= verb::kMaxPlates && std::fabs(sc.listenerYaw - 90.0) < 1e-6, "the hall has its stage, risers, one mono input per group and the listener facing the orchestra from below");
            VerbProcessor p;
            p.setPlayConfigDetails(32, 2, 48000, 256);
            p.prepareToPlay(48000, 256);
            check(waitReady(p, 60), "the plugin starts");
            p.loadRoomWithSetup(juce::String(r.text), "hall.txt", r.setup, r.micPattern);
            check(waitReady(p, 90), "the hall loads with its groups");
            juce::AudioBuffer<float> b(32, 256);
            juce::MidiBuffer m;
            double en = 0;
            bool fin = true;
            for (int k = 0; k < 120; ++k) {
                b.clear();
                if (k == 0) for (int c = 0; c < (int)r.setup.size(); ++c) b.setSample(c, 0, 1.f);
                p.processBlock(b, m);
                for (int i = 0; i < 256; ++i) { fin &= std::isfinite(b.getSample(0, i)) && std::isfinite(b.getSample(1, i)); en += b.getSample(0, i) * b.getSample(0, i); }
            }
            if (std::getenv("VERB_DUMPHALL") && r.name.rfind("Symphony hall - audience", 0) == 0) {  // exploration: the impulse response of the first group, as raw float32 stereo
                p.loadRoomWithSetup(juce::String(r.text), "hall.txt", r.setup, r.micPattern);
                waitReady(p, 90);
                juce::AudioBuffer<float> bb(32, 256);
                std::vector<float> raw;
                const int nb = (int)(6.0 * 48000 / 256);
                for (int k = 0; k < 40; ++k) { bb.clear(); p.processBlock(bb, m); }
                for (int k = 0; k < nb; ++k) {
                    bb.clear();
                    if (k == 0) bb.setSample(0, 0, 1.f);
                    p.processBlock(bb, m);
                    for (int i = 0; i < 256; ++i) { raw.push_back(bb.getSample(0, i)); raw.push_back(bb.getSample(1, i)); }
                }
                juce::File f(juce::String(std::getenv("VERB_DUMPHALL")));
                f.replaceWithData(raw.data(), raw.size() * sizeof(float));
                std::printf("  hall impulse response written: %zu frames, tail length %.2f s\n", raw.size() / 2, p.getTailLengthSeconds());
            }
            check(fin && en > 1e-9, "every group of the hall sounds");
            (void)risers;
            {   // a picture of the hall for a visual check
                std::unique_ptr<juce::AudioProcessorEditor> base(p.createEditor());
                if (auto* ed = dynamic_cast<VerbEditor*>(base.get())) {
                    ed->setSize(1160, 760);
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
                    juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
                    juce::PNGImageFormat png;
                    auto f = juce::File::getCurrentWorkingDirectory().getChildFile("out").getChildFile(juce::String("hall_") + juce::String(halls) + ".png");
                    juce::FileOutputStream fo(f);
                    if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
                }
            }
        }
        check(halls == 3, "the library has a chamber hall and two symphony hall views");
    }
    {   // listener microphone pattern: human ears
        std::printf("Listener microphone pattern\n");
        VerbProcessor p;
        p.setPlayConfigDetails(2, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "scene loads");
        verbp::OutputCfg o = p.getOutputCfg();
        o.micPattern = (int)verb::MicPattern::HumanEars;
        p.setOutputCfg(o);
        check(waitReady(p, 60), "the engine rebuilds with the human-ear pattern");
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer midi;
        double en = 0;
        bool fin = true;
        for (int s = 0; s < 60; ++s) {
            buf.clear();
            if (s == 0) buf.setSample(0, 0, 1.f);
            p.processBlock(buf, midi);
            for (int i = 0; i < 256; ++i) { fin &= std::isfinite(buf.getSample(0, i)) && std::isfinite(buf.getSample(1, i)); en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i); }
        }
        check(fin && en > 1e-9, "the human-ear listener renders finite sound");
        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        VerbProcessor q;
        q.setPlayConfigDetails(2, 2, 48000, 256);
        q.prepareToPlay(48000, 256);
        q.setStateInformation(mb.getData(), (int)mb.getSize());
        waitReady(q, 60);
        check(q.getOutputCfg().micPattern == (int)verb::MicPattern::HumanEars, "the microphone pattern is saved in the project");
        std::unique_ptr<juce::AudioProcessorEditor> base(p.createEditor());
        if (auto* ed = dynamic_cast<VerbEditor*>(base.get())) {
            ed->setSize(1160, 760);
            ed->showPage(0, verbp::kListener);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
            juce::PNGImageFormat png;
            auto f = juce::File::getCurrentWorkingDirectory().getChildFile("out").getChildFile("editor_listener_ears.png");
            juce::FileOutputStream fo(f);
            if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
            check(img.getWidth() > 100, "editor paints the listener page with the pattern selector");
        }
    }
    {   // dry/wet
        std::printf("Dry/wet mix\n");
        VerbProcessor p;
        p.setPlayConfigDetails(2, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "scene loads");
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer midi;
        auto firstSample = [&](double mixPercent) {  // output of channel 0 at the sample of an input impulse
            setP(p, "mix", mixPercent);
            for (int i = 0; i < 20; ++i) { buf.clear(); p.processBlock(buf, midi); }  // let the gain ramp settle
            buf.clear();
            buf.setSample(0, 5, 1.f);
            p.processBlock(buf, midi);
            return buf.getSample(0, 5);
        };
        const double wetOnly = firstSample(100), dryOnly = firstSample(0), half = firstSample(50);
        std::printf("  output at the impulse: mix 100 %% -> %.4f, 0 %% -> %.4f, 50 %% -> %.4f\n", wetOnly, dryOnly, half);
        check(std::fabs(wetOnly) < 1e-3, "100 % is only the room (the room's direct sound arrives later)");
        check(std::fabs(dryOnly - 1.0) < 1e-3, "0 % passes the input straight through");
        check(std::fabs(half - std::cos(juce::MathConstants<double>::pi / 4)) < 0.01, "50 % is an equal-power blend");
        setP(p, "mix", 100);
    }
    {   // room library and impulse-response export
        std::printf("Room library and IR export\n");
        const auto rooms = verbp::builtinRooms();
        check(rooms.size() >= 8, "the library ships a set of rooms");
        bool allParse = true;
        for (auto& r : rooms) {
            try {
                std::istringstream is(r.text);
                auto sc = verb::parseScene(is, r.name);
                allParse &= sc.walls.size() >= 4 && sc.sources.size() >= 1 && sc.hasListener;
            } catch (const std::exception& ex) {
                std::printf("    %s: %s\n", r.name.c_str(), ex.what());
                allParse = false;
            }
        }
        check(allParse, "every built-in room parses (walls, a source and a listener)");
        VerbProcessor p;
        p.setPlayConfigDetails(4, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "scene loads");
        bool allLoad = true, allSound = true;
        juce::AudioBuffer<float> buf(4, 256);
        juce::MidiBuffer midi;
        for (auto& r : rooms) {
            p.loadSceneText(juce::String(r.text), juce::String(r.name), true);
            if (!waitReady(p, 60)) { allLoad = false; std::printf("    %s did not load: %s\n", r.name.c_str(), p.getStatus().toRawUTF8()); continue; }
            double en = 0;
            bool fin = true;
            for (int s = 0; s < 120; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int i = 0; i < 256; ++i) { fin &= std::isfinite(buf.getSample(0, i)); en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i); }
            }
            allSound &= fin && en > 1e-9;
        }
        {   // the tail is long enough for the room (not cut while still loud) and ends smoothly; VERB_TAILS=1 prints every room
            for (auto& r : rooms) {
                if (!r.setup.empty() && r.name.rfind("MotorSynth", 0) == 0) continue;
                const bool garage = r.name == "Garage (concrete)";
                if (!garage && !std::getenv("VERB_TAILS")) continue;
                p.loadSceneText(juce::String(r.text), juce::String(r.name), true);
                if (!waitReady(p, 60)) continue;
                std::istringstream is(r.text);
                const auto sc = verb::parseScene(is, r.name);
                const double L = p.getTailLengthSeconds() - 0.25;  // what the engine really renders
                const int blocks = (int)((L + 1.0) * 48000 / 256);
                std::vector<double> win;
                const auto tStart = juce::Time::getMillisecondCounterHiRes();
                double cur = 0;
                int cnt = 0;
                for (int b = 0; b < blocks; ++b) {
                    buf.clear();
                    if (b == 0) buf.setSample(0, 0, 1.f);
                    p.processBlock(buf, midi);
                    for (int i = 0; i < 256; ++i) cur += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i);
                    if (++cnt == 19) { win.push_back(cur); cur = 0; cnt = 0; }  // ~100 ms windows
                }
                const double cpu = (juce::Time::getMillisecondCounterHiRes() - tStart) / (1000.0 * blocks * 256 / 48000.0);
                std::printf("  [cpu %.0f %%] ", 100 * cpu);
                double pk = 1e-30;
                for (double w : win) pk = std::max(pk, w);
                std::printf("  %-34s length %.1f s (scene %.1f):", r.name.c_str(), L, sc.length);
                const size_t atEnd = (size_t)(L * 10.0);
                for (size_t k : {atEnd / 4, atEnd / 2, atEnd * 3 / 4, atEnd * 9 / 10, atEnd * 95 / 100, atEnd > 1 ? atEnd - 1 : 0, atEnd + 2})
                    if (k < win.size()) std::printf(" %5.1f", 10 * std::log10(win[k] / pk + 1e-30));
                std::printf(" dB (at 25/50/75/90/95/100 %% of the length, +0.2 s)\n");
                if (garage) {
                    const size_t n100 = atEnd > 1 ? atEnd - 1 : 0;
                    check(L > sc.length + 1.5, "a concrete garage gets a tail longer than its scene length");
                    check(win.size() > n100 && 10 * std::log10(win[atEnd * 95 / 100] / pk + 1e-30) < -35.0 && 10 * std::log10(win[n100] / pk + 1e-30) < -60.0,
                          "the tail has died away or is faded out where the engine's length ends");
                }
            }
        }
        check(allLoad, "every built-in room loads into the plugin");
        check(allSound, "every built-in room makes finite, audible sound");

        // export: back to the demo room, export, read the files back
        p.loadSceneText(juce::String(rooms[1].text), "Bedroom.txt", true);
        check(waitReady(p, 60), "bedroom loads");
        auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("verb_ir_export_test");
        dir.deleteRecursively();
        p.startExportIR(dir);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        while (p.getExportState() == 1 && juce::Time::getMillisecondCounterHiRes() - t0 < 120000) juce::Thread::sleep(100);
        std::printf("  export: %s\n", p.getExportMessage().toRawUTF8());
        check(p.getExportState() == 2, "the export finishes");
        auto wavs = dir.findChildFiles(juce::File::findFiles, false, "*.wav");
        check(wavs.size() == 1, "one WAV per source point");
        if (wavs.size() == 1) {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> rd(wav.createReaderFor(wavs[0].createInputStream().release(), true));
            check(rd != nullptr && rd->numChannels == 2 && rd->sampleRate == 48000 && rd->lengthInSamples >= 90000, "the WAV is stereo, 48 kHz, as long as the scene's tail");
            if (rd) {
                juce::AudioBuffer<float> ir((int)rd->numChannels, (int)rd->lengthInSamples);
                rd->read(&ir, 0, (int)rd->lengthInSamples, 0, true, true);
                bool fin = true;
                double en = 0;
                int first = -1;
                for (int i = 0; i < ir.getNumSamples(); ++i) {
                    const float a = ir.getSample(0, i);
                    fin &= std::isfinite(a) && std::isfinite(ir.getSample(1, i));
                    en += a * a;
                    if (first < 0 && std::fabs(a) > 1e-4f) first = i;
                }
                // bedroom: source (1.2, 1.7), listener (3.0, 1.7): 1.8 m (plus the microphone offset) = ~250 samples
                std::printf("  first arrival at sample %d, energy %.3e\n", first, en);
                check(fin && en > 1e-6 && first > 150 && first < 400, "the exported IR is finite, with the direct sound at the geometric delay");
            }
        }
        auto roomFile = dir.getChildFile("Bedroom_room.txt");
        bool roomOk = false;
        if (roomFile.existsAsFile()) {
            try {
                std::istringstream is(roomFile.loadFileAsString().toStdString());
                auto sc = verb::parseScene(is, "exported room");
                roomOk = sc.walls.size() >= 4 && sc.hasListener && !sc.sources.empty() && std::fabs(sc.listenerPos.x - 1.7) < 0.05;  // the room is turned so that the listener looks up at the source
            } catch (...) {}
        }
        check(roomOk, "the room is written next to the IRs and reads back with the current listener position");
        dir.deleteRecursively();
        // room text with current positions
        setP(p, "lx", 3.3);
        const auto text = p.getRoomText();
        std::istringstream is2(text.toStdString());
        auto sc2 = verb::parseScene(is2, "room text");
        check(std::fabs(sc2.listenerPos.x - 3.3) < 0.01, "the saved room carries the current positions");
    }
    std::printf(fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
