#pragma once
#include <cmath>
#include <sstream>
#include "MovementPath.h"
#include "../src/layouts.hpp"

static int testE16Plugin() {
    const int before = fails;
    auto isPureAscii = [](const juce::String& s) {
        const std::string raw = s.toStdString();
        for (unsigned char c : raw)
            if (c < 32 || c > 126) return false;
        return true;
    };

    // Part 1 + Part 2 processor tests: multi-source bake, selective invalidation, coupled listener, persistence, blocked rules.
    {
        auto pPtr = std::make_unique<VerbProcessor>();
        auto& p = *pPtr;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(4, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        check(waitReady(p, 90), "E16 processor starts");

        auto cfg = p.getSources();
        cfg.resize(2);
        cfg[0].name = "Lead"; cfg[0].firstIn = 0; cfg[0].format = 0;
        cfg[1].name = "Pads"; cfg[1].firstIn = 1; cfg[1].format = 0;
        p.setSources(cfg);
        check(waitReady(p, 90), "E16 two-source config loads");

        verbp::MovementPath path0;
        path0.enabled = true;
        path0.smooth = true;
        path0.points = {{{1.5, 4.5, 1.5}}, {{3.0, 5.2, 1.5}}, {{4.5, 4.5, 1.5}}};
        verbp::MovementPath path1;
        path1.enabled = true;
        path1.smooth = true;
        path1.points = {{{4.5, 3.0, 1.5}}, {{3.0, 3.5, 1.5}}, {{1.5, 3.0, 1.5}}};

        check(p.setMovementPath(0, path0) && p.setMovementPath(1, path1), "E16 two independent source paths accepted");
        check(p.pathBakePercent() == 0, "E16 setting paths does not auto-bake");

        check(p.startPathBake(-1), "E16 multi-source bake starts");
        auto engine = E9bAccess::engine(p);
        juce::AudioBuffer<float> buf(4, 128);
        juce::MidiBuffer midi;
        const auto allocBefore = stage2AudioAllocations;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        int blk = 0;
        while (p.pathBakePercent() < 100 && std::chrono::steady_clock::now() < deadline) {
            buf.clear();
            stage2Process(p, buf, midi);
            if (++blk % 16 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
            juce::Thread::sleep(1);
        }
        check(p.pathBakePercent() == 100, "E16 multi-source bake reaches 100%");
        check(stage2AudioAllocations == allocBefore, "E16 multi-source bake has zero audio allocations");

        // Pump diagnostics until baked status is published.
        auto waitStatus = [&](auto pred) {
            const auto until = juce::Time::getMillisecondCounterHiRes() + 5000.0;
            while (juce::Time::getMillisecondCounterHiRes() < until) {
                buf.clear();
                stage2Process(p, buf, midi);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
                if (pred(p.pathBakeStatus())) return true;
                juce::Thread::sleep(2);
            }
            return pred(p.pathBakeStatus());
        };

        auto findPerPath = [](const juce::var& st, int obj) -> juce::var {
            if (auto* arr = st["paths"].getArray())
                for (const auto& item : *arr)
                    if ((int)item["object"] == obj) return item;
            return {};
        };

        const bool bothBaked = waitStatus([&](const juce::var& st) {
            return st["state"].toString() == "baked"
                && (int)st["object"] == -1
                && findPerPath(st, 0)["state"].toString() == "baked"
                && findPerPath(st, 1)["state"].toString() == "baked";
        });
        check(bothBaked, "E16 pathBakeStatus reports per-path baked states for both sources");

        // Render audio with uncorrelated s1path / s2path sweeps.
        setP(p, "s1path", 0.25);
        setP(p, "s2path", 0.85);
        double en0 = 0, en1 = 0;
        bool fin = true;
        for (int i = 0; i < 40; ++i) {
            buf.clear();
            if (i == 0) { buf.setSample(0, 0, 1.f); buf.setSample(1, 0, 1.f); }
            stage2Process(p, buf, midi);
            for (int n = 0; n < 128; ++n) {
                const float l = buf.getSample(0, n), r = buf.getSample(1, n);
                fin &= std::isfinite(l) && std::isfinite(r);
                en0 += l * l; en1 += r * r;
            }
        }
        check(fin && en0 > 1e-8 && en1 > 1e-8, "E16 multi-source baked playback produces finite non-zero audio");

        // Selective invalidation: edit Source 0 only -> Source 0 outdated, Source 1 stays baked!
        auto edited0 = path0;
        edited0.points[1][1] = 5.6;
        check(p.setMovementPath(0, edited0), "E16 selective edit on source 0 applied");
        const bool selOk = waitStatus([&](const juce::var& st) {
            return st["state"].toString() == "outdated"
                && findPerPath(st, 0)["state"].toString() == "outdated"
                && findPerPath(st, 1)["state"].toString() == "baked"
                && isPureAscii(st["reason"].toString());
        });
        check(selOk, "E16 editing path 0 invalidates only path 0 while path 1 remains baked");
        const auto tablesAfterEdit = engine->bakedTablesSnapshot();
        check(tablesAfterEdit.size() == 1 && tablesAfterEdit[0]->path.object == 1, "E16 untouched source 1 table remains active in engine");

        // Part 2: Coupled group including listener (object 32 + object 0 in group 0; disable object 1).
        path1.enabled = false;
        p.setMovementPath(1, path1);
        verbp::MovementPath cSrc0;
        cSrc0.enabled = true;
        cSrc0.group = 0;
        cSrc0.points = {{{2.0, 2.0, 1.5}}, {{2.0, 5.5, 1.5}}};
        verbp::MovementPath cLst;
        cLst.enabled = true;
        cLst.group = 0;
        cLst.points = {{{3.0, 2.0, 1.5}}, {{5.0, 2.0, 1.5}}};
        check(p.setMovementPath(0, cSrc0) && p.setMovementPath(verbp::kListener, cLst), "E16 coupled listener+source paths accepted");

        // Verify XML serialization omits "group" for uncoupled path1 and emits group="0" for coupled paths.
        const auto preset = p.getRoomPresetText();
        auto presetXml = juce::parseXML(preset);
        check(presetXml != nullptr, "E16 preset XML valid");
        if (presetXml) {
            auto* pathsNode = presetXml->getChildByName("VerbState")->getChildByName("MovementPaths");
            bool foundC0 = false, foundCL = false, foundUncoupledNoAttr = false;
            if (pathsNode) {
                for (auto* ch = pathsNode->getFirstChildElement(); ch; ch = ch->getNextElement()) {
                    const int obj = ch->getIntAttribute("object", -99);
                    if (obj == 0) foundC0 = ch->hasAttribute("group") && ch->getIntAttribute("group") == 0;
                    if (obj == verbp::kListener) foundCL = ch->hasAttribute("group") && ch->getIntAttribute("group") == 0;
                    if (obj == 1) foundUncoupledNoAttr = !ch->hasAttribute("group");
                }
            }
            check(foundC0 && foundCL && foundUncoupledNoAttr, "E16 XML saves group=\"0\" on coupled paths and omits group on uncoupled paths");
        }

        // Verify project state + .verbroom reload preserves group and resets bake.
        auto qPtr = std::make_unique<VerbProcessor>();
        auto& q = *qPtr;
        q.setPlayConfigDetails(4, 2, 48000, 128);
        q.prepareToPlay(48000, 128);
        juce::String err;
        check(q.loadRoomPresetText(preset, err) && waitReady(q, 90), "E16 coupled preset reloads into fresh processor");
        check(q.getMovementPath(0).group == 0 && q.getMovementPath(verbp::kListener).group == 0 && q.getMovementPath(1).group == -1,
              "E16 coupled group attribute survives .verbroom roundtrip");
        check(q.pathBakePercent() == 0, "E16 reloaded coupled preset does not auto-bake");

        // Bake coupled group on p.
        check(p.startPathBake(-1), "E16 coupled group bake starts");
        const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        blk = 0;
        while (p.pathBakePercent() < 100 && std::chrono::steady_clock::now() < deadline2) {
            buf.clear();
            stage2Process(p, buf, midi);
            if (++blk % 16 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
            juce::Thread::sleep(1);
        }
        check(p.pathBakePercent() == 100, "E16 coupled {Listener, Source 0} bake reaches 100%");
        const bool coupledMetaOk = waitStatus([&](const juce::var& st) {
            auto p0 = findPerPath(st, 0), pL = findPerPath(st, verbp::kListener);
            return st["state"].toString() == "baked"
                && (int)p0["group"] == 0 && (int)p0["drivenBy"] == 0
                && (int)pL["group"] == 0 && (int)pL["drivenBy"] == 0;
        });
        check(coupledMetaOk, "E16 coupled status reports drivenBy=0 for both Source 0 and Listener");

        // Driver rule in audio: lpath changes must NOT move coupled listener; s1path moves both.
        setP(p, "soloDirect", 1);
        setP(p, "s1path", 0.0);
        setP(p, "lpath", 0.0);
        for (int k = 0; k < 240; ++k) { buf.clear(); stage2Process(p, buf, midi); }
        auto captureDirect = [&](double s1u, double lu) {
            setP(p, "s1path", s1u);
            setP(p, "lpath", lu);
            for (int k = 0; k < 25; ++k) { buf.clear(); stage2Process(p, buf, midi); }
            std::vector<float> out;
            for (int k = 0; k < 20; ++k) {
                buf.clear();
                if (k == 0) buf.setSample(0, 0, 1.f);
                stage2Process(p, buf, midi);
                out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 128);
            }
            return out;
        };
        const auto w_u0_l0 = captureDirect(0.0, 0.0);
        const auto w_u0_l1 = captureDirect(0.0, 1.0);
        const auto w_u1_l0 = captureDirect(1.0, 0.0);
        auto maxDiff = [](const std::vector<float>& a, const std::vector<float>& b) {
            double d = 0;
            for (size_t i = 0; i < a.size(); ++i) d = std::max(d, (double)std::abs(a[i] - b[i]));
            return d;
        };
        const double diffL = maxDiff(w_u0_l0, w_u0_l1);
        const double diffD = maxDiff(w_u0_l0, w_u1_l0);
        if (!(diffL < 1e-6)) {
            auto stats = [](const std::vector<float>& v) {
                size_t pkI = 0; float pk = 0.f;
                for (size_t i = 0; i < v.size(); ++i) if (std::abs(v[i]) > std::abs(pk)) { pk = v[i]; pkI = i; }
                return std::pair<size_t, float>{pkI, pk};
            };
            auto s0 = stats(w_u0_l0), s1 = stats(w_u0_l1), s2 = stats(w_u1_l0);
            std::printf("    [DIAG] diffL=%.9g diffD=%.9g pk0=(%zu,%.6f) pk1=(%zu,%.6f) pk2=(%zu,%.6f)\n",
                        diffL, diffD, s0.first, s0.second, s1.first, s1.second, s2.first, s2.second);
        }
        check(diffL < 1e-6, "E16 moving lpath alone does not move coupled listener (s1path is driver)");
        check(diffD > 1e-3, "E16 moving s1path (driver) shifts coupled listener and source");
        check(std::abs(p.getEnergyScene(0).sources[0].pos.y - 5.5) < 0.05, "E16 getEnergyScene evaluates coupled source at driver position");

        // Blocked uncoupled rule: enable Source 1 uncoupled while Listener is moving.
        path1.enabled = true;
        path1.group = -1;
        p.setMovementPath(1, path1);
        const bool blockedOk = waitStatus([&](const juce::var& st) {
            return st["state"].toString() == "blocked"
                && st["reason"].toString().isNotEmpty()
                && isPureAscii(st["reason"].toString());
        });
        check(blockedOk, "E16 mixing moving listener with uncoupled source reports blocked with pure-ASCII actionable reason");

        p.releaseResources();
        q.releaseResources();
    }

    // Reviewer addition regression check: host output channel count changes must trigger engine rebuild for EXPLICIT layouts too.
    {
        auto ambPtr = std::make_unique<VerbProcessor>();
        auto& amb = *ambPtr;
        amb.setTailSettings({4000, false});
        amb.setPlayConfigDetails(2, 2, 48000, 256);
        amb.prepareToPlay(48000, 256);
        check(waitReady(amb, 90), "Reviewer fix: AmbiX processor starts at 2 host output channels");
        setP(amb, "s1x", 4.5); setP(amb, "s1y", 4.8); setP(amb, "s1z", 2.2);
        auto outCfg = amb.getOutputCfg();
        outCfg.layout = (int)verb::Layout::AmbiX1;
        amb.setOutputCfg(outCfg);
        check(waitReady(amb, 90), "Reviewer fix: explicit AmbiX1 ready at 2 host channels");
        check(amb.numOutputChannels() == 2 && amb.hostOutputChannels() == 2 && amb.effectiveLayout() == (int)verb::Layout::AmbiX1,
              "Reviewer fix: AmbiX1 initially truncated to 2 host channels");

        const uint64_t serialAt2 = amb.getLoadSerial();
        amb.setPlayConfigDetails(2, 4, 48000, 256);
        amb.prepareToPlay(48000, 256);
        check(waitReady(amb, 90), "Reviewer fix: AmbiX1 rebuilds when host bus grows 2 -> 4 channels");
        check(amb.getLoadSerial() > serialAt2 && amb.numOutputChannels() == 4 && amb.hostOutputChannels() == 4
              && amb.effectiveLayout() == (int)verb::Layout::AmbiX1,
              "Reviewer fix: AmbiX1 rendered channels grow from 2 to 4");

        // Verify channels 3 and 4 (0-indexed 2 and 3) carry finite non-zero audio.
        juce::AudioBuffer<float> buf4(4, 256);
        juce::MidiBuffer midi;
        std::array<double, 4> chEnergy{};
        bool fin4 = true;
        for (int b = 0; b < 32; ++b) {
            buf4.clear();
            if (b == 0) buf4.setSample(0, 0, 1.f);
            amb.processBlock(buf4, midi);
            for (int c = 0; c < 4; ++c)
                for (int i = 0; i < 256; ++i) {
                    const float v = buf4.getSample(c, i);
                    fin4 &= std::isfinite(v);
                    chEnergy[(size_t)c] += (double)v * v;
                }
        }
        check(fin4 && chEnergy[2] > 1e-10 && chEnergy[3] > 1e-10,
              "Reviewer fix: widened AmbiX1 renders non-zero finite audio on channels 3 and 4");

        // Repeated prepareToPlay with same 4 host channels must NOT trigger another rebuild.
        const uint64_t serialAt4 = amb.getLoadSerial();
        amb.prepareToPlay(48000, 256);
        check(amb.getLoadSerial() == serialAt4, "Reviewer fix: unchanged host output channel count does not trigger rebuild");

        // Shrinking back to 2 channels must rebuild & truncate rendered channels back to 2.
        amb.setPlayConfigDetails(2, 2, 48000, 256);
        amb.prepareToPlay(48000, 256);
        check(waitReady(amb, 90) && amb.getLoadSerial() > serialAt4 && amb.numOutputChannels() == 2 && amb.hostOutputChannels() == 2,
              "Reviewer fix: shrinking AmbiX1 host bus 4 -> 2 rebuilds and truncates back to 2 channels");
        amb.releaseResources();
    }
    {
        auto surPtr = std::make_unique<VerbProcessor>();
        auto& sur = *surPtr;
        sur.setTailSettings({4000, false});
        sur.setPlayConfigDetails(2, 2, 48000, 256);
        sur.prepareToPlay(48000, 256);
        check(waitReady(sur, 90), "Reviewer fix: Surround51 processor starts at 2 host channels");
        auto outCfg = sur.getOutputCfg();
        outCfg.layout = (int)verb::Layout::Surround51;
        sur.setOutputCfg(outCfg);
        check(waitReady(sur, 90) && sur.numOutputChannels() == 2 && sur.effectiveLayout() == (int)verb::Layout::Surround51,
              "Reviewer fix: explicit 5.1 starts truncated to 2 host channels");

        const uint64_t serialAt2 = sur.getLoadSerial();
        sur.setPlayConfigDetails(2, 6, 48000, 256);
        sur.prepareToPlay(48000, 256);
        check(waitReady(sur, 90) && sur.getLoadSerial() > serialAt2 && sur.numOutputChannels() == 6 && sur.hostOutputChannels() == 6,
              "Reviewer fix: explicit 5.1 grows to 6 rendered channels when host bus widens to 6");

        juce::AudioBuffer<float> buf6(6, 256);
        juce::MidiBuffer midi;
        std::array<double, 6> chEnergy{};
        bool fin6 = true;
        for (int b = 0; b < 32; ++b) {
            buf6.clear();
            if (b == 0) buf6.setSample(0, 0, 1.f);
            sur.processBlock(buf6, midi);
            for (int c = 0; c < 6; ++c)
                for (int i = 0; i < 256; ++i) {
                    const float v = buf6.getSample(c, i);
                    fin6 &= std::isfinite(v);
                    chEnergy[(size_t)c] += (double)v * v;
                }
        }
        check(fin6 && chEnergy[4] > 1e-10 && chEnergy[5] > 1e-10,
              "Reviewer fix: widened 5.1 renders non-zero finite audio on surround channels Ls and Rs");

        const uint64_t serialAt6 = sur.getLoadSerial();
        sur.setPlayConfigDetails(2, 2, 48000, 256);
        sur.prepareToPlay(48000, 256);
        check(waitReady(sur, 90) && sur.getLoadSerial() > serialAt6 && sur.numOutputChannels() == 2,
              "Reviewer fix: shrinking 5.1 host bus 6 -> 2 rebuilds and truncates back to 2 channels");
        sur.releaseResources();
    }

    return fails == before ? 0 : 1;
}
