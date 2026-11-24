#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

#include "AutomationRange.h"
#include "EditorBridge.h"
#include "EditorSession.h"
#include "PluginProcessor.h"
#include "RoomLibrary.h"
#include "WebParameterBindings.h"

namespace g3test {

inline std::string boxSceneText(double w, double d, double h) {
    std::ostringstream o;
    o << "sr 48000\nlength 1.0\nheight " << h << "\norder 1\n"
      << "material Plaster 0.08 0.08 0.08 0.08 0.08 0.08 0.08 0.1 0.2\n"
      << "material Carpet 0.10 0.15 0.25 0.40 0.55 0.65 0.70 0.2 0.6\n"
      << "floor Carpet\nceiling Plaster\n"
      << "wall 0 0 " << w << " 0 Plaster\nwall " << w << " 0 " << w << ' ' << d << " Plaster\n"
      << "wall " << w << ' ' << d << " 0 " << d << " Plaster\nwall 0 " << d << " 0 0 Plaster\n"
      << "listener 7 4 1.5 0\n"
      << "source S1 3 4 1.5 0 0 0 0\n"
      << "receiver L 7 4 1.5 55 0.5\nreceiver R 7 4 1.5 -55 0.5\n";
    return o.str();
}

inline bool approx(double a, double b, double tol = 1e-4) {
    return std::abs(a - b) <= tol;
}

inline bool approxRange(const verbp::AutomationRange& a, const verbp::AutomationRange& b, double tol = 1e-4) {
    return approx(a.xMin, b.xMin, tol) && approx(a.xMax, b.xMax, tol)
        && approx(a.yMin, b.yMin, tol) && approx(a.yMax, b.yMax, tol)
        && approx(a.zMin, b.zMin, tol) && approx(a.zMax, b.zMax, tol);
}

inline juce::var parseReply0(const juce::var& res) {
    return (res.isArray() && !res.getArray()->isEmpty()) ? (*res.getArray())[0] : juce::var();
}

inline juce::var sendBridgeCmd(verbweb::EditorBridge& bridge, const juce::String& reqId, juce::var rev, const juce::var& payload) {
    auto* msg = new juce::DynamicObject();
    msg->setProperty("schemaVersion", 1);
    msg->setProperty("sessionId", "g3-test");
    msg->setProperty("kind", "command");
    msg->setProperty("requestId", reqId);
    msg->setProperty("baseRevision", rev);
    msg->setProperty("payload", payload);
    return bridge.handle(juce::var(msg));
}

}  // namespace g3test

static int testG3ThreadSafety() {
    const int before = fails;
    std::printf("G3 thread safety: concurrent range swaps + audio processBlock / VST3 param conversions\n");
    VerbProcessor p;
    p.setTailSettings({4000, false});
    p.setPlayConfigDetails(2, 2, 48000, 128);
    p.prepareToPlay(48000, 128);
    check(waitReady(p, 90), "G3 stress processor starts");

    auto* px = dynamic_cast<verbp::DynamicPositionParameter*>(p.apvts.getParameter("s1x"));
    auto* py = dynamic_cast<verbp::DynamicPositionParameter*>(p.apvts.getParameter("s1y"));
    auto* pz = dynamic_cast<verbp::DynamicPositionParameter*>(p.apvts.getParameter("s1z"));
    auto* lx = dynamic_cast<verbp::DynamicPositionParameter*>(p.apvts.getParameter("lx"));
    check(px && py && pz && lx, "G3 position parameters use thread-safe DynamicPositionParameter");

    std::atomic<bool> writerDone{false};
    std::atomic<bool> badRead{false};
    std::atomic<int> writerIters{0};

    std::thread writer([&] {
        for (int i = 0; i < 2500; ++i) {
            verbp::AutomationRange r;
            const double shift = (double)(i % 40) * 0.25;
            if (i % 7 == 0) {
                r = {};
            } else {
                r.xMin = -10.0 - shift;
                r.xMax = 12.0 + shift;
                r.yMin = -8.0 + (double)(i % 5);
                r.yMax = 14.0 + shift;
                r.zMin = (double)(i % 3) * 0.5;
                r.zMax = 5.0 + (double)(i % 6);
            }
            p.setAutomationRange(r, false, false);
            writerIters.fetch_add(1, std::memory_order_relaxed);
        }
        writerDone.store(true, std::memory_order_release);
    });

    juce::AudioBuffer<float> buf(2, 128);
    juce::MidiBuffer midi;
    int audioReads = 0;
    while (!writerDone.load(std::memory_order_acquire) || audioReads < 400) {
        for (int k = 0; k < 3; ++k) {
            auto* prm = k == 0 ? px : (k == 1 ? py : pz);
            const float norm = (float)((audioReads % 17) / 16.0);
            prm->setValue(norm);
            const float v = prm->getValue();
            const float dv = prm->getDefaultValue();
            const auto& nr = prm->getNormalisableRange();
            const float phys = prm->convertFrom0to1(norm);
            const float back = prm->convertTo0to1(phys);
            const auto txt = prm->getText(v, 32);
            const float parsedNorm = prm->getValueForText(txt);
            if (!std::isfinite(v) || !std::isfinite(dv) || !std::isfinite(phys) || !std::isfinite(back)
                || !std::isfinite(parsedNorm) || nr.end <= nr.start || v < -1e-4f || v > 1.0001f) {
                badRead.store(true, std::memory_order_relaxed);
            }
        }
        buf.clear();
        buf.setSample(0, 0, (audioReads % 16 == 0) ? 0.25f : 0.0f);
        p.processBlock(buf, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 128; ++i)
                if (!std::isfinite(buf.getSample(c, i)))
                    badRead.store(true, std::memory_order_relaxed);
        ++audioReads;
    }
    writer.join();
    check(!badRead.load() && writerIters.load() == 2500 && audioReads >= 400,
          "G3 2500 concurrent range swaps + audio thread conversions produce finite valid results without races");
    p.releaseResources();
    return fails == before ? 0 : 1;
}

static int testG3FitPersistenceAndUndo() {
    const int before = fails;
    std::printf("G3 Fit, bit-identical audio, state/.verbroom round-trip, legacy, Undo/Redo, BakeCache, Keep I/O, scale\n");

    // 1. 10x8x4 room with source, listener and outside path point -> Fit + bit-identical processBlock
    {
        VerbProcessor a, b;
        const auto roomTxt = g3test::boxSceneText(10.0, 8.0, 4.0);
        for (auto* pr : {&a, &b}) {
            pr->setTailSettings({4000, false});
            pr->loadSceneText(roomTxt, "10x8x4.txt", true);
            pr->setPlayConfigDetails(2, 2, 48000, 128);
            pr->prepareToPlay(48000, 128);
        }
        check(waitReady(a, 90) && waitReady(b, 90), "G3 10x8x4 processors start");
        verbp::MovementPath path;
        path.enabled = false;
        path.points = {{2.0, 2.0, 1.5}, {12.0, 4.0, 1.5}}; // outside east wall x=10
        a.setMovementPath(0, path);
        b.setMovementPath(0, path);

        // Wait for background tail worker to settle completely using stage2Impulse helper
        // Compare two fresh processors (a keeps default +-200 m range, b applies Fit) so FDN tail state starts clean on both.
        const auto sampBefore = stage2Impulse(a, 1.0);

        const auto fitRange = b.fitAutomationRangeToScene(true, true);
        // walls 0..10 x 0..8, path x=12 => rawX [0,12] span 12 -> margin 1.2 => x [-1.5, 13.5], y [-1.5, 9.5], z [0, 5.0]
        check(g3test::approxRange(fitRange, {-1.5, 13.5, -1.5, 9.5, 0.0, 5.0}),
              "G3 Fit computes expected 10x8x4 + outside path point box with 1.2 m margin rounded to 0.5 m");
        check(g3test::approx(b.apvts.getRawParameterValue("s1x")->load(), 3.0)
              && g3test::approx(b.apvts.getRawParameterValue("lx")->load(), 7.0),
              "G3 Fit preserves physical coordinates in metres");
        check(b.getParameters().size() == 283 + verbp::kMaxModHostOuts
              && b.apvts.getParameter("s1x")->getParameterIndex() == a.apvts.getParameter("s1x")->getParameterIndex()
              && b.apvts.getParameter("lpath")->getParameterIndex() == 282,
              "G3 keeps first 283 parameter indices unchanged + 16 appended Mod Out parameters");

        const auto sampAfter = stage2Impulse(b, 1.0);
        double maxDiff = 0.0;
        for (size_t idx = 0; idx < sampBefore.size(); ++idx)
            maxDiff = std::max(maxDiff, std::abs((double)sampBefore[idx] - (double)sampAfter[idx]));
        std::printf("    G3 bit-identical audio check: samples=%zu, memcmpAB=%d (maxDiff=%.9g)\n",
                    sampBefore.size(),
                    std::memcmp(sampBefore.data(), sampAfter.data(), sampBefore.size() * sizeof(float)), maxDiff);
        check(sampBefore.size() == sampAfter.size()
              && std::memcmp(sampBefore.data(), sampAfter.data(), sampBefore.size() * sizeof(float)) == 0,
              "G3 Fit without clamping yields bit-identical processBlock output");
        a.releaseResources(); b.releaseResources();
    }

    // 2. Non-symmetric range state + .verbroom round-trip, old state compat, .txt reset
    {
        VerbProcessor p;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "G3 persistence processor starts");

        const verbp::AutomationRange asym{-5.5, 18.0, 2.0, 14.5, 0.5, 6.5};
        p.setAutomationRange(asym, false, true);
        setP(p, "s1x", -2.25); setP(p, "s1y", 11.75); setP(p, "s1z", 2.5);
        setP(p, "lx", 15.0);   setP(p, "ly", 4.0);    setP(p, "lz", 1.5);
        const float expectedNorm = (float)((-2.25 - (-5.5)) / (18.0 - (-5.5)));
        check(g3test::approx(p.apvts.getParameter("s1x")->getValue(), expectedNorm, 1e-3),
              "G3 normalized value reflects asymmetric range (-5.5..18)");

        juce::MemoryBlock state;
        p.getStateInformation(state);
        VerbProcessor q;
        q.setTailSettings({4000, false});
        q.setPlayConfigDetails(2, 2, 48000, 128);
        q.prepareToPlay(48000, 128);
        check(waitReady(q, 60), "G3 receiver processor starts");
        q.setStateInformation(state.getData(), (int)state.getSize());
        check(waitReady(q, 90) && g3test::approxRange(q.getAutomationRange(), asym),
              "G3 asymmetric range survives getStateInformation/setStateInformation");
        check(g3test::approx(q.apvts.getRawParameterValue("s1x")->load(), -2.25, 0.002)
              && g3test::approx(q.apvts.getRawParameterValue("s1y")->load(), 11.75, 0.002)
              && g3test::approx(q.apvts.getRawParameterValue("s1z")->load(), 2.5, 0.002)
              && g3test::approx(q.apvts.getRawParameterValue("lx")->load(), 15.0, 0.002)
              && g3test::approx(q.apvts.getParameter("s1x")->getValue(), expectedNorm, 1e-3),
              "G3 physical and normalized coordinates match after state load (range applied before replaceState)");

        const auto preset = p.getRoomPresetText();
        auto presetXml = juce::parseXML(preset);
        check(presetXml != nullptr && presetXml->getChildByName("VerbState") != nullptr
              && presetXml->getChildByName("VerbState")->hasAttribute("autoXMin"),
              "G3 .verbroom carries autoXMin..autoZMax when non-default");

        VerbProcessor q2;
        q2.setTailSettings({4000, false});
        q2.setPlayConfigDetails(2, 2, 48000, 128);
        q2.prepareToPlay(48000, 128);
        juce::String err;
        check(waitReady(q2, 60) && q2.loadRoomPresetText(preset, err) && waitReady(q2, 90)
              && g3test::approxRange(q2.getAutomationRange(), asym)
              && g3test::approx(q2.apvts.getRawParameterValue("s1x")->load(), -2.25, 0.002),
              "G3 .verbroom round-trip preserves asymmetric range and positions");

        // Legacy 0.2.2/0.2.3 & stage2 compatibility when receiver currently has a tight range [-1, 4]
        q2.setAutomationRange({-1.0, 4.0, -1.0, 4.0, 0.0, 3.0}, false, false);
        const auto fixtures = repoFile("plugin/fixtures/stage2");
        juce::MemoryBlock legacyState;
        check(fixtures.getChildFile("legacy.state").loadFileAsData(legacyState), "G3 legacy.state fixture exists");
        q2.setStateInformation(legacyState.getData(), (int)legacyState.getSize());
        check(waitReady(q2, 90) && q2.getAutomationRange().isDefault()
              && g3test::approx(q2.apvts.getRawParameterValue("lx")->load(), 6.0, 0.01),
              "G3 legacy.state resets range to default before replaceState so lx=6 is not clipped to 4");

        // Strip auto* attributes to simulate 0.2.2/0.2.3 state loaded on top of a tight range
        q2.setAutomationRange({-1.0, 4.0, -1.0, 4.0, 0.0, 3.0}, false, false);
        auto oldXml = p.getXmlFromBinary(state.getData(), (int)state.getSize());
        if (oldXml) {
            for (const char* k : {"autoXMin", "autoXMax", "autoYMin", "autoYMax", "autoZMin", "autoZMax"})
                oldXml->removeAttribute(k);
            juce::MemoryBlock oldStateMb;
            p.copyXmlToBinary(*oldXml, oldStateMb);
            q2.setStateInformation(oldStateMb.getData(), (int)oldStateMb.getSize());
            check(waitReady(q2, 90) && q2.getAutomationRange().isDefault()
                  && g3test::approx(q2.apvts.getRawParameterValue("lx")->load(), 15.0, 0.01),
                  "G3 state without auto* (0.2.2/0.2.3) resets range to default before replaceState (lx=15 kept)");
        }

        q2.setAutomationRange({-1.0, 4.0, -1.0, 4.0, 0.0, 3.0}, false, false);
        check(q2.loadRoomPresetText(fixtures.getChildFile("legacy.verbroom").loadFileAsString(), err)
              && waitReady(q2, 90) && q2.getAutomationRange().isDefault()
              && g3test::approx(q2.apvts.getRawParameterValue("lx")->load(), 6.0, 0.01),
              "G3 legacy.verbroom loads with default range and unclipped positions");

        // Plain .txt / built-in room reset + optional v2 automationRange directive
        q2.setAutomationRange({-2.0, 12.0, -2.0, 10.0, 0.0, 5.0}, false, false);
        q2.loadSceneText(g3test::boxSceneText(8.0, 6.0, 3.0), "plain.txt", true);
        check(waitReady(q2, 90) && q2.getAutomationRange().isDefault(),
              "G3 plain .txt without v2 tag resets automationRange to default");
        q2.loadSceneText(g3test::boxSceneText(8.0, 6.0, 3.0) + "#@verbroom-editor-v2 automationRange -4 14 -3 11 0 5\n", "v2range.txt", true);
        check(waitReady(q2, 90) && g3test::approxRange(q2.getAutomationRange(), {-4.0, 14.0, -3.0, 11.0, 0.0, 5.0}),
              "G3 .txt with v2 automationRange directive loads that range");

        p.releaseResources(); q.releaseResources(); q2.releaseResources();
    }

    // 3. Bridge/Session commands: narrowing + clamp + 1-step Undo/Redo + BakeCache + Keep I/O + room.scale
    {
        VerbProcessor p;
        p.setTailSettings({4000, false});
        p.loadSceneText(g3test::boxSceneText(10.0, 8.0, 4.0), "G3Cmd.txt", true);
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "G3 command processor starts");

        std::vector<juce::var> emitted;
        verbweb::EditorBridge bridge(p, [&](const juce::String&, const juce::var& m) { emitted.push_back(m); });
        auto snap0 = g3test::parseReply0(bridge.handle(juce::JSON::parse(R"({"schemaVersion":1,"sessionId":"g3-test","kind":"hello","payload":{"supportedVersions":[1],"client":"test","ready":true}})")));
        check(snap0.isObject() && snap0.getProperty("kind", "") == "snapshot", "G3 bridge hello returns snapshot");

        // Bake a path first to verify Fit with 0 clamped objects does NOT invalidate BakeCache
        {
            verbp::MovementPath path;
            path.points = {{2.0, 2.0, 1.5}, {6.0, 2.0, 1.5}};
            path.enabled = true;
            p.setMovementPath(0, path);
            check(p.startPathBake(0), "G3 bake starts");
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            while (p.pathBakeStatus().getProperty("state", "").toString() != "baked"
                   && juce::Time::getMillisecondCounterHiRes() - t0 < 30000.0) {
                juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
            }
            check(p.pathBakeStatus().getProperty("state", "").toString() == "baked", "G3 bake finishes before Fit");
        }

        const auto rFit = g3test::sendBridgeCmd(bridge, "c-fit", juce::String(p.getRevision()),
                                                juce::JSON::parse(R"({"op":"room.fitAutomationRange"})"));
        check(g3test::parseReply0(rFit).getProperty("kind", "") == "ack", "G3 bridge accepts room.fitAutomationRange");
        check(p.pathBakeStatus().getProperty("state", "").toString() == "baked",
              "G3 Fit without clamping does NOT invalidate BakeCache");

        // Check snapshot descriptors and WebParameterBindings reflect fitted range
        auto snapFit = rFit.getArray()->getLast();
        const auto arJson = snapFit.getProperty("payload", {}).getProperty("document", {}).getProperty("scene", {}).getProperty("automationRange", {});
        check(arJson.isObject() && g3test::approx((double)arJson.getProperty("xM", {})[0], -1.0)
              && g3test::approx((double)arJson.getProperty("xM", {})[1], 11.0),
              "G3 snapshot carries fitted document.scene.automationRange");
        const auto descList = snapFit.getProperty("payload", {}).getProperty("parameters", {}).getProperty("descriptors", {}).getProperty("parameters", {});
        bool foundS1x = false;
        if (auto* arr = descList.getArray()) {
            for (const auto& d : *arr) if (d.getProperty("id", "") == "s1x") {
                foundS1x = g3test::approx((double)d.getProperty("min", 0.0), -1.0) && g3test::approx((double)d.getProperty("max", 0.0), 11.0);
            }
        }
        check(foundS1x, "G3 snapshot descriptors reflect fitted min/max for s1x");

        verbweb::WebParameterBindings bindings(p);
        const auto badApply = bindings.apply(juce::JSON::parse(R"([{"type":"begin","id":"s1x"},{"type":"change","id":"s1x","value":50},{"type":"end","id":"s1x"}])"));
        check(badApply.hasProperty("error"), "G3 WebParameterBindings rejects out-of-range value after Fit");

        // room.scale with non-default range (center (5,4), x: -1..11, y: -1..9, z: 0..5)
        const auto rScale = g3test::sendBridgeCmd(bridge, "c-scale", juce::String(p.getRevision()),
                                                  juce::JSON::parse(R"({"op":"room.scale","factor":2.0,"includeObjects":true})"));
        check(g3test::parseReply0(rScale).getProperty("kind", "") == "ack"
              && g3test::approxRange(p.getAutomationRange(), {-7.0, 17.0, -6.0, 14.0, 0.0, 10.0})
              && g3test::approx(p.apvts.getRawParameterValue("lx")->load(), 9.0, 0.01),
              "G3 room.scale scales non-default automationRange about room centre before clamping params");
        const auto rScalePlan = g3test::sendBridgeCmd(bridge, "c-scale2", juce::String(p.getRevision()),
                                                      juce::JSON::parse(R"({"op":"room.scale","factor":0.5,"includeObjects":false})"));
        check(g3test::parseReply0(rScalePlan).getProperty("kind", "") == "ack"
              && g3test::approx(p.getAutomationRange().zMax, 10.0)
              && g3test::approx(p.apvts.getRawParameterValue("lx")->load(), 9.0, 0.01),
              "G3 room.scale without includeObjects keeps Z range and object positions unmoved");

        // Narrowing + clamping + 1-step Undo / Redo
        setP(p, "s1x", 10.0);
        setP(p, "lx", 10.5);
        const auto rangeBeforeNarrow = p.getAutomationRange();
        const auto rNarrow = g3test::sendBridgeCmd(bridge, "c-narrow", juce::String(p.getRevision()),
                                                   juce::JSON::parse(R"({"op":"room.setAutomationRange","xM":[0,8],"yM":[0,8],"zM":[0,4]})"));
        check(g3test::parseReply0(rNarrow).getProperty("kind", "") == "ack"
              && g3test::approxRange(p.getAutomationRange(), {0.0, 8.0, 0.0, 8.0, 0.0, 4.0})
              && g3test::approx(p.apvts.getRawParameterValue("s1x")->load(), 8.0, 0.01)
              && g3test::approx(p.apvts.getRawParameterValue("lx")->load(), 8.0, 0.01),
              "G3 narrowing range clamps outside source and listener");

        const auto rUndo = g3test::sendBridgeCmd(bridge, "c-undo", juce::String(p.getRevision()),
                                                 juce::JSON::parse(R"({"op":"history.undo"})"));
        check(g3test::parseReply0(rUndo).getProperty("kind", "") == "ack"
              && g3test::approxRange(p.getAutomationRange(), rangeBeforeNarrow)
              && g3test::approx(p.apvts.getRawParameterValue("s1x")->load(), 10.0, 0.01)
              && g3test::approx(p.apvts.getRawParameterValue("lx")->load(), 10.5, 0.01),
              "G3 single Undo step restores both range and pre-clamp coordinates");

        const auto rRedo = g3test::sendBridgeCmd(bridge, "c-redo", juce::String(p.getRevision()),
                                                 juce::JSON::parse(R"({"op":"history.redo"})"));
        check(g3test::parseReply0(rRedo).getProperty("kind", "") == "ack"
              && g3test::approxRange(p.getAutomationRange(), {0.0, 8.0, 0.0, 8.0, 0.0, 4.0})
              && g3test::approx(p.apvts.getRawParameterValue("s1x")->load(), 8.0, 0.01),
              "G3 single Redo step re-applies narrowed range and clamping");

        // Keep I/O union rule on .verbroom import
        p.setAutomationRange({-1.0, 9.0, -1.0, 7.0, 0.0, 5.0}, true, false);
        const auto tightPreset = p.getRoomPresetText();
        VerbProcessor qKeep;
        qKeep.setTailSettings({4000, false});
        qKeep.setPlayConfigDetails(64, 2, 48000, 128);
        qKeep.prepareToPlay(48000, 128);
        check(waitReady(qKeep, 60), "G3 Keep I/O receiver starts");
        auto cfg = qKeep.getSources();
        cfg.resize(1);
        cfg[0].format = 1; cfg[0].sum = false; cfg[0].freePoints = true;
        cfg[0].ox = {25.f, 0.f}; cfg[0].oy = {0.f, 0.f}; cfg[0].oz = {0.f, 0.f};
        qKeep.setSources(cfg);
        check(waitReady(qKeep, 60), "G3 Keep I/O local setup ready");
        setP(qKeep, "s1x", 22.0);
        qKeep.setKeepRoutingEnabled(true);
        juce::String err;
        check(qKeep.loadRoomPresetText(tightPreset, err) && waitReady(qKeep, 90), "G3 Keep I/O loads tight preset");
        check(qKeep.getAutomationRange().xMax >= 25.0,
              "G3 Keep I/O unions preset automationRange with local offsets/positions so nothing is clipped");

        p.releaseResources(); qKeep.releaseResources();
    }

    return fails == before ? 0 : 1;
}

static int testG3AutomationRangeFast() {
    return testG3ThreadSafety() | testG3FitPersistenceAndUndo();
}
