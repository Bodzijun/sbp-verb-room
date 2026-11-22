#pragma once
// E23: AmbiX 2nd (9 ch) and 3rd order (16 ch) at the processor level: host-channel rebuild 2 -> N -> 2, audio on every
// channel, zero audio-thread allocations, meters, state/.verbroom round trip, Match track never picks AmbiX, IR export.
#include <cmath>
#include "../src/layouts.hpp"

static int testE23Plugin() {
    const int before = fails;
    auto ck = [](bool ok, const juce::String& msg) { check(ok, msg.toRawUTF8()); };
    struct Case { verb::Layout lay; int ch; const char* name; };
    const Case cases[] = {{verb::Layout::AmbiX2, 9, "AmbiX2"}, {verb::Layout::AmbiX3, 16, "AmbiX3"}};

    for (const auto& cs : cases) {
        auto ptr = std::make_unique<VerbProcessor>();
        auto& p = *ptr;
        const juce::String tag = juce::String("E23 ") + cs.name + ": ";
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(2, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        ck(waitReady(p, 90), tag + "processor starts at 2 host channels");
        setP(p, "s1x", 4.5); setP(p, "s1y", 4.8); setP(p, "s1z", 2.2);
        auto cfg = p.getOutputCfg();
        cfg.layout = (int)cs.lay;
        p.setOutputCfg(cfg);
        ck(waitReady(p, 90), tag + "explicit layout ready on a stereo track");
        ck(p.numOutputChannels() == 2 && p.hostOutputChannels() == 2 && p.effectiveLayout() == (int)cs.lay, tag + "truncated to the 2 host channels");

        for (int round = 0; round < 2; ++round) {  // 2 -> N -> 2, twice
            const uint64_t serialLow = p.getLoadSerial();
            p.setPlayConfigDetails(2, cs.ch, 48000, 256);
            p.prepareToPlay(48000, 256);
            ck(waitReady(p, 90), tag + "rebuilds when the host bus grows 2 -> N (round " + juce::String(round) + ")");
            ck(p.getLoadSerial() > serialLow && p.numOutputChannels() == cs.ch && p.hostOutputChannels() == cs.ch && p.effectiveLayout() == (int)cs.lay,
                  tag + "all channels rendered after growing (round " + juce::String(round) + ")");

            juce::AudioBuffer<float> buf(cs.ch, 256);
            juce::MidiBuffer midi;
            std::vector<double> energy((size_t)cs.ch, 0.0);
            bool finite = true;
            for (int b = 0; b < 48; ++b) {  // warm: let the graph settle, then count allocations
                buf.clear();
                if (b == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
            }
            const auto allocBefore = stage2AudioAllocations;
            for (int b = 0; b < 64; ++b) {
                buf.clear();
                if (b % 8 == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int c = 0; c < cs.ch; ++c)
                    for (int i = 0; i < 256; ++i) {
                        const float v = buf.getSample(c, i);
                        finite &= std::isfinite(v);
                        energy[(size_t)c] += (double)v * v;
                    }
            }
            ck(stage2AudioAllocations == allocBefore, tag + "zero audio-thread allocations (round " + juce::String(round) + ")");
            int silent = 0;
            for (int c = 0; c < cs.ch; ++c) if (!(energy[(size_t)c] > 1e-12)) ++silent;
            ck(finite && silent == 0, tag + "every channel carries finite non-zero audio (silent channels: " + juce::String(silent) + ")");
            int meterBad = 0;
            for (int c = 0; c < cs.ch; ++c) if (!(p.outputRms(c) > 0.f)) ++meterBad;
            ck(meterBad == 0, tag + "meters report all channels (silent meters: " + juce::String(meterBad) + ")");

            const uint64_t serialHigh = p.getLoadSerial();
            p.prepareToPlay(48000, 256);
            ck(p.getLoadSerial() == serialHigh, tag + "unchanged channel count does not rebuild");

            p.setPlayConfigDetails(2, 2, 48000, 256);
            p.prepareToPlay(48000, 256);
            ck(waitReady(p, 90) && p.getLoadSerial() > serialHigh && p.numOutputChannels() == 2 && p.hostOutputChannels() == 2,
                  tag + "shrinking N -> 2 rebuilds and truncates (round " + juce::String(round) + ")");
        }

        // state round trip: the layout index is stored as today, a project saved with it loads again
        juce::MemoryBlock state;
        p.getStateInformation(state);
        {
            auto qptr = std::make_unique<VerbProcessor>();
            auto& q = *qptr;
            q.setTailSettings({4000, false});
            q.setPlayConfigDetails(2, cs.ch, 48000, 256);
            q.prepareToPlay(48000, 256);
            q.setStateInformation(state.getData(), (int)state.getSize());
            ck(waitReady(q, 90), tag + "state loads on an N-channel host");
            ck(q.getOutputCfg().layout == (int)cs.lay && q.numOutputChannels() == cs.ch && q.effectiveLayout() == (int)cs.lay,
                  tag + "layout index and channel count survive the state round trip");
            q.releaseResources();
        }
        p.releaseResources();
    }

    // Match track on 9 / 16 channel tracks never selects an Ambisonic layout (Ring), and 4 channels stay Quad
    for (int ch : {4, 9, 16}) {
        auto ptr = std::make_unique<VerbProcessor>();
        auto& p = *ptr;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(2, ch, 48000, 256);
        p.prepareToPlay(48000, 256);
        ck(waitReady(p, 90), "E23 Match track processor starts at " + juce::String(ch) + " channels");
        const int expect = ch == 4 ? (int)verb::Layout::Quad : (int)verb::Layout::Ring;
        {auto cfg = p.getOutputCfg(); cfg.layout = -1; p.setOutputCfg(cfg);}
        ck(waitReady(p, 90), "E23 Match track rebuilt at " + juce::String(ch) + " channels");
        ck(p.getOutputCfg().layout == -1 && p.effectiveLayout() == expect && p.numOutputChannels() == ch,
              "E23 Match track at " + juce::String(ch) + " channels does not choose AmbiX");
        p.releaseResources();
    }

    // REAPER tracks have an even channel count: a 10-channel track carries AmbiX 2nd order (9 channels), the 10th stays silent
    {
        auto ptr = std::make_unique<VerbProcessor>();
        auto& p = *ptr;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(2, 10, 48000, 256);
        p.prepareToPlay(48000, 256);
        ck(waitReady(p, 90), "E23 10-channel track processor starts");
        auto cfg = p.getOutputCfg();
        cfg.layout = (int)verb::Layout::AmbiX2;
        p.setOutputCfg(cfg);
        ck(waitReady(p, 90) && p.numOutputChannels() == 9 && p.hostOutputChannels() == 10 && p.effectiveLayout() == (int)verb::Layout::AmbiX2,
           "E23 AmbiX2 on a 10-channel track renders 9 channels");
        setP(p, "s1x", 4.5); setP(p, "s1y", 4.8); setP(p, "s1z", 2.2);
        juce::AudioBuffer<float> buf(10, 256);
        juce::MidiBuffer midi;
        std::vector<double> energy(10, 0.0);
        bool finite = true;
        for (int b = 0; b < 96; ++b) {
            buf.clear();
            if (b % 8 == 0) buf.setSample(0, 0, 1.f);
            p.processBlock(buf, midi);
            if (b < 32) continue;
            for (int c = 0; c < 10; ++c)
                for (int i = 0; i < 256; ++i) { const float v = buf.getSample(c, i); finite &= std::isfinite(v); energy[(size_t)c] += (double)v * v; }
        }
        int silent = 0;
        for (int c = 0; c < 9; ++c) if (!(energy[(size_t)c] > 1e-12)) ++silent;
        ck(finite && silent == 0 && energy[9] == 0.0, "E23 9 channels carry audio and the 10th channel of the track is silent");
        p.releaseResources();
    }

    // an old project (layout Quad) is untouched; an out-of-range layout id is clamped to the last format
    {
        auto ptr = std::make_unique<VerbProcessor>();
        auto& p = *ptr;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(2, 4, 48000, 256);
        p.prepareToPlay(48000, 256);
        ck(waitReady(p, 90), "E23 old-project processor starts");
        auto cfg = p.getOutputCfg();
        cfg.layout = (int)verb::Layout::Quad;
        p.setOutputCfg(cfg);
        ck(waitReady(p, 90) && p.effectiveLayout() == (int)verb::Layout::Quad && p.numOutputChannels() == 4, "E23 Quad project is unchanged");
        p.releaseResources();
    }

    // IR export with 16 channels
    {
        auto ptr = std::make_unique<VerbProcessor>();
        auto& p = *ptr;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(1, 16, 48000, 256);
        p.prepareToPlay(48000, 256);
        ck(waitReady(p, 90), "E23 export processor starts at 16 channels");
        auto cfg = p.getOutputCfg();
        cfg.layout = (int)verb::Layout::AmbiX3;
        p.setOutputCfg(cfg);
        ck(waitReady(p, 90) && p.numOutputChannels() == 16, "E23 export: AmbiX3 ready with 16 channels");
        const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("verb-e23-export");
        dir.deleteRecursively();
        p.startExportIR(dir);
        const auto until = juce::Time::getMillisecondCounterHiRes() + 180000;
        while (p.getExportState() == 1 && juce::Time::getMillisecondCounterHiRes() < until)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        const auto files = dir.findChildFiles(juce::File::findFiles, false, "*.wav");
        ck(p.getExportState() == 2 && files.size() == 1, "E23 IR export finishes with one file");
        if (files.size() == 1) {
            juce::WavAudioFormat format;
            std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(files[0].createInputStream().release(), true));
            ck(reader != nullptr && reader->numChannels == 16 && reader->lengthInSamples > 4800, "E23 exported WAV has 16 channels");
            if (reader) {
                juce::AudioBuffer<float> ab(16, (int)std::min<juce::int64>(reader->lengthInSamples, 96000));
                reader->read(&ab, 0, ab.getNumSamples(), 0, true, true);
                int silent = 0;
                for (int c = 0; c < 16; ++c) if (!(ab.getMagnitude(c, 0, ab.getNumSamples()) > 1e-9f)) ++silent;
                std::printf("  E23 exported 16-channel IR: %d channels, %lld samples, silent channels %d\n", (int)reader->numChannels, (long long)reader->lengthInSamples, silent);
                ck(silent == 0, "E23 every exported channel carries signal");
            }
        }
        dir.deleteRecursively();
        p.releaseResources();
    }
    return fails == before ? 0 : 1;
}
