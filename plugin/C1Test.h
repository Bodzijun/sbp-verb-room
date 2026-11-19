#pragma once
// C1: User-defined source colours (model, state/preset round-trip, Keep I/O, compatibility, invalid ignore, bit-identical audio).
// Run alone via VERB_ONLY_C1=1.
#include "E9bTest.h"
#include "../src/scene.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace c1test {

static juce::String stateXmlOf(VerbProcessor& p) {
    juce::MemoryBlock data;
    p.getStateInformation(data);
    auto xml = p.getXmlFromBinary(data.getData(), (int)data.getSize());
    return xml ? xml->toString() : juce::String();
}

static bool loadStateXml(VerbProcessor& p, const juce::String& xmlText) {
    auto xml = juce::parseXML(xmlText);
    if (!xml) return false;
    juce::MemoryBlock data;
    juce::AudioProcessor::copyXmlToBinary(*xml, data);
    p.setStateInformation(data.getData(), (int)data.getSize());
    return waitReady(p, 90);
}

static std::vector<float> captureImpulse(VerbProcessor& p) {
    juce::AudioBuffer<float> buf(4, 128);
    juce::MidiBuffer midi;
    const auto until = juce::Time::getMillisecondCounterHiRes() + 30000;
    int warmed = 0;
    while (juce::Time::getMillisecondCounterHiRes() < until) {
        buf.clear();
        p.processBlock(buf, midi);
        ++warmed;
        if (p.isTailCurrent()) break;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    }
    constexpr int kFixedWarmupBlocks = 512;  // > 1.3 s at 48 kHz and divisible by tail FFT partition (4 * 128)
    while (warmed < kFixedWarmupBlocks || (warmed % 4) != 0) {
        buf.clear();
        p.processBlock(buf, midi);
        ++warmed;
    }
    std::vector<float> ir;
    ir.reserve(2 * 128 * 200);
    bool fired = false;
    for (int b = 0; b < 200; ++b) {
        buf.clear();
        if (!fired) {
            buf.setSample(0, 0, 1.f);
            buf.setSample(1, 0, 1.f);
            fired = true;
        }
        p.processBlock(buf, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 128; ++i)
                ir.push_back(buf.getSample(c, i));
    }
    return ir;
}

}  // namespace c1test

static int testC1() {
    std::printf("C1: source colours (model, persistence, compatibility, Keep I/O, DSP bit-identity)\n");

    // 1. Hex validation & normalization
    check(verbp::isValidSourceColorHex("#ff00aa"), "valid lowercase #rrggbb");
    check(verbp::isValidSourceColorHex("#AABBCC"), "valid uppercase #RRGGBB");
    check(verbp::normalizeSourceColor("#A1b2C3") == "#a1b2c3", "normalizeSourceColor lowercases valid hex");
    check(!verbp::isValidSourceColorHex(""), "empty string is not valid hex");
    check(!verbp::isValidSourceColorHex("#123"), "3-digit hex rejected");
    check(!verbp::isValidSourceColorHex("#12345678"), "8-digit hex rejected");
    check(!verbp::isValidSourceColorHex("123456"), "missing # rejected");
    check(!verbp::isValidSourceColorHex("red"), "named colour rejected");
    check(!verbp::isValidSourceColorHex("#12345g"), "non-hex digit rejected");
    check(verbp::normalizeSourceColor("invalid").empty(), "normalizeSourceColor maps invalid to empty");

    // 2. Old scenes / fixtures without colour omit `color` property when saved -> identical XML
    const char* twoSrcScene =
        "sr 48000\nlength 1.0\nheight 3\n"
        "material concrete 0.05 0.05 0.05 0.05 0.05 0.05 0.05 0.1 0.3\n"
        "floor concrete\nceiling concrete\n"
        "wall 0 4 0 0 concrete\nwall 4 0 4 4 concrete\nwall 4 4 0 4 concrete\nwall 0 4 0 0 concrete\n"
        "listener 2 2 1.5 0\nsource Strings 1 1 1.5 0\nsource Flutes 3 1 1.5 0\n"
        "receiver L 2 2 1.5 55 0.5\nreceiver R 2 2 1.5 -55 0.5\n";

    VerbProcessor p;
    p.setPlayConfigDetails(4, 2, 48000, 128);
    p.prepareToPlay(48000, 128);
    p.setTailSettings({8000, false});
    p.loadSceneText(twoSrcScene, "C1Room", true);
    check(waitReady(p, 90), "C1 test room loads");

    const juce::String baselineXml = c1test::stateXmlOf(p);
    check(!baselineXml.contains("color="), "state without source colours omits `color` attribute completely");

    // 3. Set colours live via setSourceColor (no engine rebuild!) and round-trip state & .verbroom
    const uint64_t loadSer0 = p.getLoadSerial();
    const uint64_t rev0 = p.getRevision();
    p.setSourceColor(0, "#E04040");
    p.setSourceColor(1, "#3aa76d");
    check(p.getLoadSerial() == loadSer0, "setSourceColor does NOT rebuild the audio engine");
    check(p.getRevision() > rev0, "setSourceColor bumps structural revision");
    auto srcs = p.getSources();
    check(srcs.size() == 2 && srcs[0].color == "#e04040" && srcs[1].color == "#3aa76d",
          "setSourceColor normalizes and stores colours on SourceCfg");

    const juce::String colouredXml = c1test::stateXmlOf(p);
    check(colouredXml.contains("color=\"#e04040\"") && colouredXml.contains("color=\"#3aa76d\""),
          "saved state XML persists source colours");

    VerbProcessor restored;
    restored.setPlayConfigDetails(4, 2, 48000, 128);
    restored.prepareToPlay(48000, 128);
    restored.setTailSettings({8000, false});
    check(c1test::loadStateXml(restored, colouredXml), "restored processor loads coloured state");
    const auto rSrcs = restored.getSources();
    check(rSrcs.size() == 2 && rSrcs[0].color == "#e04040" && rSrcs[1].color == "#3aa76d",
          "source colours survive state save/load round-trip");

    // Resetting back to auto emits XML identical to baseline (when no colours set)
    restored.setSourceColor(0, "");
    restored.setSourceColor(1, "");
    const juce::String clearedXml = c1test::stateXmlOf(restored);
    check(!clearedXml.contains("color="), "clearing colours omits `color` attribute again");

    // 4. .verbroom export/import & Keep I/O semantics (like source name)
    const juce::String presetColoured = p.getRoomPresetText();
    check(presetColoured.contains("formatVersion=\"2\"") && presetColoured.contains("color=\"#e04040\""),
          ".verbroom preset keeps formatVersion=2 and persists source colour");

    // Keep I/O OFF: preset colours override local colours
    VerbProcessor receiverOff;
    receiverOff.setPlayConfigDetails(4, 2, 48000, 128);
    receiverOff.prepareToPlay(48000, 128);
    receiverOff.setTailSettings({8000, false});
    receiverOff.loadSceneText(twoSrcScene, "Local", true);
    check(waitReady(receiverOff, 90), "receiverOff loads");
    receiverOff.setSourceColor(0, "#111111");
    receiverOff.setKeepRoutingEnabled(false);
    juce::String err;
    check(receiverOff.loadRoomPresetText(presetColoured, err) && waitReady(receiverOff, 90),
          "loadRoomPresetText with Keep I/O OFF succeeds");
    check(receiverOff.getSources()[0].color == "#e04040" && receiverOff.getSources()[1].color == "#3aa76d",
          "Keep I/O OFF imports preset source colours");

    // Keep I/O ON: local source colours are preserved (same as source name)
    receiverOff.setSourceColor(0, "#123456");
    receiverOff.setSourceColor(1, "");
    receiverOff.setKeepRoutingEnabled(true);
    check(receiverOff.loadRoomPresetText(presetColoured, err) && waitReady(receiverOff, 90),
          "loadRoomPresetText with Keep I/O ON succeeds");
    check(receiverOff.getSources()[0].color == "#123456" && receiverOff.getSources()[1].color.empty(),
          "Keep I/O ON preserves local source colours");

    // 5. Invalid colour attributes in XML / .verbroom are ignored -> automatic, never block loading
    juce::String corruptPreset = presetColoured.replace("color=\"#e04040\"", "color=\"not-a-color\"")
                                               .replace("color=\"#3aa76d\"", "color=\"#12345G\"");
    receiverOff.setKeepRoutingEnabled(false);
    check(receiverOff.loadRoomPresetText(corruptPreset, err) && waitReady(receiverOff, 90),
          "preset with invalid source colours still loads cleanly");
    check(receiverOff.getSources()[0].color.empty() && receiverOff.getSources()[1].color.empty(),
          "invalid source colours in preset fall back to automatic (empty)");

    // 6. Real fixture check (`test/Flate.verbroom` and `scenes/demo_box.txt` if present)
    if (const auto flate = repoFile("test/Flate.verbroom"); flate.existsAsFile()) {
        VerbProcessor fx;
        fx.setPlayConfigDetails(4, 2, 48000, 128);
        fx.prepareToPlay(48000, 128);
        juce::String fxErr;
        check(fx.loadRoomPresetText(flate.loadFileAsString(), fxErr) && waitReady(fx, 90),
              "legacy fixture test/Flate.verbroom loads cleanly");
        const juce::String fxXml = c1test::stateXmlOf(fx);
        check(!fxXml.contains("color="), "legacy fixture without colours saves without `color` attribute");
    }
    if (const auto demo = repoFile("scenes/demo_box.txt"); demo.existsAsFile()) {
        VerbProcessor dx;
        dx.setPlayConfigDetails(4, 2, 48000, 128);
        dx.prepareToPlay(48000, 128);
        dx.loadSceneText(demo.loadFileAsString(), "demo_box", true);
        check(waitReady(dx, 90), "scenes/demo_box.txt loads cleanly");
        check(!c1test::stateXmlOf(dx).contains("color="), "demo_box.txt produces no `color` attribute");
    }

    // 7. removeSource keeps colours attached to the remaining sources
    {
        VerbProcessor rm;
        rm.setPlayConfigDetails(4, 2, 48000, 128);
        rm.prepareToPlay(48000, 128);
        rm.setTailSettings({8000, false});
        check(waitReady(rm, 90), "rm processor starts");
        std::vector<verbp::SourceCfg> three(3);
        three[0].name = "Strings"; three[0].color = "#e04040"; three[0].firstIn = 0;
        three[1].name = "Winds"; three[1].color = "#3aa76d"; three[1].firstIn = 1;
        three[2].name = "Brass"; three[2].color = "#d9a426"; three[2].firstIn = 2;
        rm.setSources(three);
        check(waitReady(rm, 90), "3 coloured sources ready");
        rm.removeSource(1);
        check(waitReady(rm, 90), "removeSource completes");
        const auto after = rm.getSources();
        check(after.size() == 2 && after[0].name == "Strings" && after[0].color == "#e04040"
              && after[1].name == "Brass" && after[1].color == "#d9a426",
              "removeSource preserves colours on their respective sources");
    }

    // 8. Bit-identical impulse response before vs after setting colours & reloading preset (AGENTS.md)
    {
        VerbProcessor srcP;
        srcP.setPlayConfigDetails(4, 2, 48000, 128);
        srcP.prepareToPlay(48000, 128);
        srcP.setTailSettings({8000, false});
        srcP.loadSceneText(twoSrcScene, "BitIdenticalRoom", true);
        check(waitReady(srcP, 90), "srcP loads");
        const juce::String plainPreset = srcP.getRoomPresetText();

        VerbProcessor plainReloaded;
        plainReloaded.setPlayConfigDetails(4, 2, 48000, 128);
        plainReloaded.prepareToPlay(48000, 128);
        plainReloaded.setTailSettings({8000, false});
        juce::String pErr;
        check(plainReloaded.loadRoomPresetText(plainPreset, pErr) && waitReady(plainReloaded, 90), "plainReloaded loads baseline preset");
        const auto irBefore = c1test::captureImpulse(plainReloaded);

        VerbProcessor colP;
        colP.setPlayConfigDetails(4, 2, 48000, 128);
        colP.prepareToPlay(48000, 128);
        colP.setTailSettings({8000, false});
        check(colP.loadRoomPresetText(plainPreset, pErr) && waitReady(colP, 90), "colP loads baseline preset");
        colP.setSourceColor(0, "#ff2244");
        colP.setSourceColor(1, "#2288ff");
        const auto irLiveColoured = c1test::captureImpulse(colP);
        const juce::String colouredPreset = colP.getRoomPresetText();

        VerbProcessor colReloaded;
        colReloaded.setPlayConfigDetails(4, 2, 48000, 128);
        colReloaded.prepareToPlay(48000, 128);
        colReloaded.setTailSettings({8000, false});
        check(colReloaded.loadRoomPresetText(colouredPreset, pErr) && waitReady(colReloaded, 90),
              "colReloaded reloads coloured preset");
        const auto irAfter = c1test::captureImpulse(colReloaded);
        const bool liveExact = irBefore.size() == irLiveColoured.size() && !irBefore.empty()
            && std::memcmp(irBefore.data(), irLiveColoured.data(), irBefore.size() * sizeof(float)) == 0;
        const bool reloadExact = irBefore.size() == irAfter.size() && !irBefore.empty()
            && std::memcmp(irBefore.data(), irAfter.data(), irBefore.size() * sizeof(float)) == 0;
        check(liveExact, "impulse response is bit-identical after setting source colours live");
        check(reloadExact, "impulse response is bit-identical after saving and reloading coloured preset");
    }

    return fails ? 1 : 0;
}
