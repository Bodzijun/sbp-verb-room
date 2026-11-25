#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace h2test {

inline void pumpBlocks(VerbProcessor& p, int blocks, int blockSize = 256, int nBufCh = 64) {
    const int ch = std::max({2, nBufCh, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()});
    juce::AudioBuffer<float> buf(ch, blockSize);
    juce::MidiBuffer midi;
    for (int i = 0; i < blocks; ++i) {
        buf.clear();
        p.processBlock(buf, midi);
    }
}

inline void settleMotion(VerbProcessor& p, int nBufCh = 64) {
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    for (int iter = 0; iter < 120 && juce::Time::getMillisecondCounterHiRes() - t0 < 10000.0; ++iter) {
        pumpBlocks(p, 4, 256, nBufCh);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        auto eng = E9bAccess::engine(p);
        if (p.isTailCurrent() && eng && eng->motionStats().transState == 0 && iter >= 16)
            break;
    }
    pumpBlocks(p, 12, 256, nBufCh);
}

inline std::vector<std::vector<float>> captureImpulse(VerbProcessor& p, int nOutCheck, int inputCh = 0, int blocks = 24, int blockSize = 256, bool* outAllFinite = nullptr) {
    const int bufCh = std::max({2, nOutCheck, inputCh + 1, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()});
    juce::AudioBuffer<float> buf(bufCh, blockSize);
    juce::MidiBuffer midi;
    std::vector<std::vector<float>> out((size_t)nOutCheck);
    for (int c = 0; c < nOutCheck; ++c) out[(size_t)c].reserve((size_t)blocks * (size_t)blockSize);
    bool finite = true;
    for (int b = 0; b < blocks; ++b) {
        buf.clear();
        if (b == 0) buf.setSample(inputCh, 0, 1.0f);
        p.processBlock(buf, midi);
        for (int c = 0; c < nOutCheck; ++c) {
            const float* rp = buf.getReadPointer(c);
            for (int i = 0; i < blockSize; ++i) {
                const float v = rp[i];
                finite &= std::isfinite(v);
                out[(size_t)c].push_back(v);
            }
        }
    }
    if (outAllFinite) *outAllFinite = finite;
    return out;
}

inline double channelEnergy(const std::vector<float>& v) {
    double e = 0.0;
    for (float s : v) e += (double)s * s;
    return e;
}

inline void testBusAcceptanceMatrix() {
    std::printf("  [H2] bus layout acceptance matrix...\n");
    VerbProcessor p;
    auto mk = [](int inCh, int outCh) {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add(inCh > 0 ? juce::AudioChannelSet::discreteChannels(inCh) : juce::AudioChannelSet::disabled());
        l.outputBuses.add(outCh > 0 ? juce::AudioChannelSet::discreteChannels(outCh) : juce::AudioChannelSet::disabled());
        return l;
    };
    for (const auto& [inCh, outCh] : std::vector<std::pair<int, int>>{
             {0, 1}, {1, 1}, {2, 2}, {64, 2}, {32, 16}, {17, 17}, {32, 32}, {48, 48}, {64, 64}, {2, 64}}) {
        check(p.isBusesLayoutSupported(mk(inCh, outCh)),
              ("H2 layout accepts " + std::to_string(inCh) + " in / " + std::to_string(outCh) + " out").c_str());
    }
    for (const auto& [inCh, outCh] : std::vector<std::pair<int, int>>{
             {65, 2}, {2, 65}, {65, 64}, {64, 65}, {2, 0}, {0, 0}}) {
        check(!p.isBusesLayoutSupported(mk(inCh, outCh)),
              ("H2 layout refuses " + std::to_string(inCh) + " in / " + std::to_string(outCh) + " out").c_str());
    }
    {
        juce::AudioProcessor::BusesLayout ambi64;
        ambi64.inputBuses.add(juce::AudioChannelSet::ambisonic(7));
        ambi64.outputBuses.add(juce::AudioChannelSet::ambisonic(7));
        check(p.isBusesLayoutSupported(ambi64), "H2 accepts Ambisonic 7th-order 64 in / 64 out");
    }
}

inline void testWideStereoBitIdenticalAndSilence() {
    std::printf("  [H2] 64/64 stereo bit-identical to 64/2, channels 3..64 silent, mute-all & raw dry...\n");
    auto refPtr = std::make_unique<VerbProcessor>();
    auto widePtr = std::make_unique<VerbProcessor>();
    auto& ref = *refPtr;
    auto& wide = *widePtr;
    ref.setTailSettings({4000, false});
    wide.setTailSettings({4000, false});
    ref.setPlayConfigDetails(64, 2, 48000, 256);
    wide.setPlayConfigDetails(64, 64, 48000, 256);
    ref.prepareToPlay(48000, 256);
    wide.prepareToPlay(48000, 256);
    check(waitReady(ref, 90) && waitReady(wide, 90), "H2 64/2 and 64/64 stereo instances become ready");
    check(wide.hostInputChannels() == 64 && wide.hostOutputChannels() == 64 && wide.numOutputChannels() == 2
          && wide.effectiveLayout() == (int)verb::Layout::StereoORTF,
          "H2 64/64 defaults to Stereo ORTF: rendered=2, host=64");

    settleMotion(ref, 64);
    settleMotion(wide, 64);

    bool finRef = false, finWide = false;
    const auto capRef = captureImpulse(ref, 2, 0, 24, 256, &finRef);
    const auto allocBefore = stage2AudioAllocations;
    const auto capWide = captureImpulse(wide, 64, 0, 24, 256, &finWide);
    check(stage2AudioAllocations == allocBefore, "H2 64/64 stereo processBlock makes zero audio-thread allocations");
    check(finRef && finWide, "H2 64/64 stereo output is finite on all 64 channels");

    bool bitExact = true;
    for (int c = 0; c < 2 && bitExact; ++c) {
        if (capRef[(size_t)c].size() != capWide[(size_t)c].size()
            || std::memcmp(capRef[(size_t)c].data(), capWide[(size_t)c].data(), capRef[(size_t)c].size() * sizeof(float)) != 0)
            bitExact = false;
    }
    double restMax = 0.0;
    for (int c = 2; c < 64; ++c)
        for (float v : capWide[(size_t)c]) restMax = std::max(restMax, (double)std::abs(v));
    check(channelEnergy(capWide[0]) > 1e-6 && channelEnergy(capWide[1]) > 1e-6, "H2 64/64 stereo ch 1/2 carry signal");
    check(bitExact, "H2 64/64 stereo outputs 1/2 match 64/2 bit-identically");
    check(restMax == 0.0, "H2 64/64 stereo outputs 3..64 are strictly silent with Positioned dry on");

    // Mute-all check on 64/64 (Positioned dry ON and Positioned dry OFF / Mix 0 with 32 stereo sources)
    std::vector<verbp::SourceCfg> sources;
    sources.reserve(verbp::kMaxSources);
    for (int s = 0; s < verbp::kMaxSources; ++s) {
        verbp::SourceCfg sc;
        sc.name = "S" + std::to_string(s + 1);
        sc.format = 1;
        sc.channels = 2;
        sc.firstIn = 2 * s;
        sc.sum = true;
        sources.push_back(std::move(sc));
    }
    wide.setSources(sources);
    check(waitReady(wide, 90), "H2 64/64 32 stereo sources ready");
    settleMotion(wide, 64);
    for (int s = 0; s < verbp::kMaxSources; ++s) setP(wide, ("s" + std::to_string(s + 1) + "mute").c_str(), 1.0);
    setP(wide, "spatialDry", 1.0);
    pumpBlocks(wide, 16, 256, 64);

    juce::AudioBuffer<float> buf(64, 256);
    juce::MidiBuffer midi;
    auto runAllInputs = [&] {
        buf.clear();
        for (int c = 0; c < 64; ++c)
            for (int i = 0; i < 256; ++i) buf.setSample(c, i, 0.25f);
        wide.processBlock(buf, midi);
        double mx = 0.0;
        bool fin = true;
        for (int c = 0; c < 64; ++c)
            for (int i = 0; i < 256; ++i) {
                const float v = buf.getSample(c, i);
                fin &= std::isfinite(v);
                mx = std::max(mx, (double)std::abs(v));
            }
        return std::pair{mx, fin};
    };
    const auto [muteOnPeak, muteOnFin] = runAllInputs();
    check(muteOnFin && muteOnPeak <= 1e-7, "H2 64/64 Mute-all -> all 64 output channels silent (Positioned dry ON)");

    setP(wide, "spatialDry", 0.0);
    setP(wide, "mix", 0.0);
    pumpBlocks(wide, 4, 256, 64);
    const auto [muteOffPeak, muteOffFin] = runAllInputs();
    check(muteOffFin && muteOffPeak <= 1e-7, "H2 64/64 Mute-all -> all 64 output channels silent (Positioned dry OFF, Mix 0)");

    // Unmute source 16 (channels 30..31) with Positioned dry OFF & Mix 0 -> k->k raw dry on 30..31, rest 0
    setP(wide, "s16mute", 0.0);
    pumpBlocks(wide, 4, 256, 64);
    buf.clear();
    buf.setSample(30, 0, 0.75f);
    buf.setSample(31, 0, 0.5f);
    wide.processBlock(buf, midi);
    double otherPeak = 0.0;
    for (int c = 0; c < 64; ++c) {
        if (c == 30 || c == 31) continue;
        for (int i = 0; i < 256; ++i) otherPeak = std::max(otherPeak, (double)std::abs(buf.getSample(c, i)));
    }
    check(std::abs(buf.getSample(30, 0) - 0.75f) < 1e-4f && std::abs(buf.getSample(31, 0) - 0.5f) < 1e-4f && otherPeak < 1e-5,
          "H2 64/64 Positioned dry OFF preserves k->k dry rule on ch 31/32 without leaking elsewhere");

    ref.releaseResources();
    wide.releaseResources();
}

inline void testWideAmbiX3AndMatchTrack() {
    std::printf("  [H2] 64/64 AmbiX3 bit-identical to 16/16 + Match track (Ring 16/64)...\n");
    auto p16Ptr = std::make_unique<VerbProcessor>();
    auto p64Ptr = std::make_unique<VerbProcessor>();
    auto& p16 = *p16Ptr;
    auto& p64 = *p64Ptr;
    p16.setTailSettings({4000, false});
    p64.setTailSettings({4000, false});
    p16.setPlayConfigDetails(16, 16, 48000, 256);
    p64.setPlayConfigDetails(64, 64, 48000, 256);
    p16.prepareToPlay(48000, 256);
    p64.prepareToPlay(48000, 256);
    check(waitReady(p16, 90) && waitReady(p64, 90), "H2 16/16 and 64/64 AmbiX3 processors ready");

    setP(p16, "s1x", 4.5); setP(p16, "s1y", 4.8); setP(p16, "s1z", 2.2);
    setP(p64, "s1x", 4.5); setP(p64, "s1y", 4.8); setP(p64, "s1z", 2.2);
    auto outCfg = p16.getOutputCfg();
    outCfg.layout = (int)verb::Layout::AmbiX3;
    p16.setOutputCfg(outCfg);
    p64.setOutputCfg(outCfg);
    check(waitReady(p16, 90) && waitReady(p64, 90), "H2 AmbiX3 built on both 16/16 and 64/64");
    check(p64.numOutputChannels() == 16 && p64.hostOutputChannels() == 64 && p64.effectiveLayout() == (int)verb::Layout::AmbiX3,
          "H2 64/64 AmbiX3 renders 16 channels on 64 host outputs (16 / 64)");

    settleMotion(p16, 16);
    settleMotion(p64, 64);

    bool fin16 = false, fin64 = false;
    const auto cap16 = captureImpulse(p16, 16, 0, 24, 256, &fin16);
    const auto allocBefore = stage2AudioAllocations;
    const auto cap64 = captureImpulse(p64, 64, 0, 24, 256, &fin64);
    check(stage2AudioAllocations == allocBefore, "H2 64/64 AmbiX3 processBlock makes zero allocations");
    check(fin16 && fin64, "H2 64/64 AmbiX3 all 64 outputs finite");

    bool bitExact = true;
    int silentRendered = 0;
    for (int c = 0; c < 16; ++c) {
        if (!(channelEnergy(cap64[(size_t)c]) > 1e-12)) ++silentRendered;
        if (cap16[(size_t)c].size() != cap64[(size_t)c].size()
            || std::memcmp(cap16[(size_t)c].data(), cap64[(size_t)c].data(), cap16[(size_t)c].size() * sizeof(float)) != 0)
            bitExact = false;
    }
    double restMax = 0.0;
    for (int c = 16; c < 64; ++c)
        for (float v : cap64[(size_t)c]) restMax = std::max(restMax, (double)std::abs(v));
    check(silentRendered == 0 && bitExact, "H2 64/64 AmbiX3 channels 1..16 are active and bit-identical to 16/16");
    check(restMax == 0.0, "H2 64/64 AmbiX3 channels 17..64 are strictly silent");

    // Match track (-1) on 64/64 resolves to Ring (16 / 64), finite non-zero 1..16 and silent 17..64
    outCfg.layout = -1;
    p64.setOutputCfg(outCfg);
    check(waitReady(p64, 90), "H2 64/64 Match track (-1) rebuilds");
    check(p64.effectiveLayout() == (int)verb::Layout::Ring && p64.numOutputChannels() == 16 && p64.hostOutputChannels() == 64,
          "H2 64/64 Match track selects Ring 16 / 64 ch");
    settleMotion(p64, 64);
    bool finRing = false;
    const auto capRing = captureImpulse(p64, 64, 0, 24, 256, &finRing);
    int ringSilent = 0;
    for (int c = 0; c < 16; ++c) if (!(channelEnergy(capRing[(size_t)c]) > 1e-12)) ++ringSilent;
    double ringRest = 0.0;
    for (int c = 16; c < 64; ++c) for (float v : capRing[(size_t)c]) ringRest = std::max(ringRest, (double)std::abs(v));
    check(finRing && ringSilent == 0 && ringRest == 0.0, "H2 64/64 Match track renders Ring ch 1..16 and silences 17..64");

    p16.releaseResources();
    p64.releaseResources();
}

inline void testWideImpulseAndReload() {
    std::printf("  [H2] AGENTS.md impulse + state/preset save/reload on default room and VSL preset (64/64)...\n");
    // 1. Default room on 64/64 with AmbiX3: impulse + state & .verbroom reload
    {
        auto pPtr = std::make_unique<VerbProcessor>();
        auto& p = *pPtr;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(64, 64, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 90), "H2 default room on 64/64 ready");
        auto cfg = p.getOutputCfg();
        cfg.layout = (int)verb::Layout::AmbiX3;
        p.setOutputCfg(cfg);
        check(waitReady(p, 90), "H2 default room 64/64 AmbiX3 ready");
        settleMotion(p, 64);
        bool fin0 = false;
        const auto ir0 = captureImpulse(p, 64, 0, 24, 256, &fin0);

        const juce::String presetXml = p.getRoomPresetText();
        juce::MemoryBlock stateMb;
        p.getStateInformation(stateMb);

        auto qPtr = std::make_unique<VerbProcessor>();
        auto& q = *qPtr;
        q.setTailSettings({4000, false});
        q.setPlayConfigDetails(64, 64, 48000, 256);
        q.prepareToPlay(48000, 256);
        q.setStateInformation(stateMb.getData(), (int)stateMb.getSize());
        check(waitReady(q, 90) && q.numOutputChannels() == 16 && q.hostOutputChannels() == 64 && q.effectiveLayout() == (int)verb::Layout::AmbiX3,
              "H2 default room state round-trip keeps 16/64 AmbiX3");
        settleMotion(q, 64);
        bool finState = false;
        const auto irState = captureImpulse(q, 64, 0, 24, 256, &finState);

        juce::String err;
        auto rPtr = std::make_unique<VerbProcessor>();
        auto& r = *rPtr;
        r.setPlayConfigDetails(64, 64, 48000, 256);
        r.prepareToPlay(48000, 256);
        check(r.loadRoomPresetText(presetXml, err) && waitReady(r, 90) && r.numOutputChannels() == 16 && r.hostOutputChannels() == 64
              && r.effectiveLayout() == (int)verb::Layout::AmbiX3,
              "H2 default room .verbroom round-trip keeps 16/64 AmbiX3");
        settleMotion(r, 64);
        bool finPreset = false;
        const auto irPreset = captureImpulse(r, 64, 0, 24, 256, &finPreset);

        double maxDiffState = 0.0, maxDiffPreset = 0.0;
        for (int c = 0; c < 64; ++c)
            for (size_t i = 0; i < ir0[(size_t)c].size(); ++i) {
                maxDiffState = std::max(maxDiffState, (double)std::abs(ir0[(size_t)c][i] - irState[(size_t)c][i]));
                maxDiffPreset = std::max(maxDiffPreset, (double)std::abs(ir0[(size_t)c][i] - irPreset[(size_t)c][i]));
            }
        check(fin0 && finState && finPreset && channelEnergy(ir0[0]) > 1e-6 && maxDiffState < 1e-6 && maxDiffPreset < 1e-6,
              "H2 default room 64/64 impulse matches across state & .verbroom reload");
    }

    // 2. VSL Grand Hall preset (32 stereo groups) on 64/64: Stereo AND AmbiX3 impulse + preset reload
    {
        const auto vsl = verbp::vslGrandHall(false);
        auto pPtr = std::make_unique<VerbProcessor>();
        auto& p = *pPtr;
        p.setTailSettings({4000, false});
        p.setPlayConfigDetails(64, 64, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 90), "H2 VSL processor 64/64 starts");
        p.loadRoomWithSetup(juce::String(vsl.text), juce::String(vsl.name), vsl.setup, vsl.micPattern);
        check(waitReady(p, 180) && p.getSources().size() == 32 && p.numPoints() == 32
              && p.numOutputChannels() == 2 && p.hostOutputChannels() == 64,
              "H2 VSL Grand Hall starts at 2/64 ch on 64-ch bus");

        auto cfg = p.getOutputCfg();
        cfg.layout = (int)verb::Layout::AmbiX3;
        p.setOutputCfg(cfg);
        check(waitReady(p, 180) && p.numOutputChannels() == 16 && p.hostOutputChannels() == 64 && p.effectiveLayout() == (int)verb::Layout::AmbiX3,
              "H2 VSL Grand Hall switches to AmbiX3 16/64 ch on 64-ch bus");
        settleMotion(p, 64);

        bool finFirst = false, finLast = false;
        const auto irFirst = captureImpulse(p, 64, 0, 24, 256, &finFirst);
        const auto irLast = captureImpulse(p, 64, 62, 24, 256, &finLast);
        int silent16 = 0;
        for (int c = 0; c < 16; ++c) if (!(channelEnergy(irFirst[(size_t)c]) > 1e-12)) ++silent16;
        double restMax = 0.0;
        for (int c = 16; c < 64; ++c) for (float v : irFirst[(size_t)c]) restMax = std::max(restMax, (double)std::abs(v));
        check(finFirst && finLast && silent16 == 0 && channelEnergy(irLast[0]) > 1e-12 && restMax == 0.0,
              "H2 VSL 64/64 AmbiX3 impulse sounds across all 16 channels from source 1 & 32 while ch 17..64 stay silent");

        const juce::String presetXml = p.getRoomPresetText();
        auto qPtr = std::make_unique<VerbProcessor>();
        auto& q = *qPtr;
        q.setTailSettings({4000, false});
        q.setPlayConfigDetails(64, 64, 48000, 256);
        q.prepareToPlay(48000, 256);
        juce::String err;
        check(q.loadRoomPresetText(presetXml, err) && waitReady(q, 180)
              && q.getSources().size() == 32 && q.getSources().back().firstIn == 62
              && q.numOutputChannels() == 16 && q.hostOutputChannels() == 64 && q.effectiveLayout() == (int)verb::Layout::AmbiX3,
              "H2 VSL .verbroom save/reload preserves 32 sources and 16/64 AmbiX3");
        settleMotion(q, 64);
        bool finReload = false;
        const auto irReload = captureImpulse(q, 64, 0, 24, 256, &finReload);
        int silentReload = 0;
        for (int c = 0; c < 16; ++c) if (!(channelEnergy(irReload[(size_t)c]) > 1e-12)) ++silentReload;
        check(finReload && silentReload == 0 && std::abs(channelEnergy(irReload[0]) - channelEnergy(irFirst[0])) < 1e-6 * channelEnergy(irFirst[0]) + 1e-12,
              "H2 VSL reloaded 16/64 AmbiX3 impulse matches pre-save");
    }
}

}  // namespace h2test

static int testH2Fast() {
    const int before = fails;
    std::printf("H2 fast: bus acceptance matrix & 64/64 stereo bit-identity\n");
    h2test::testBusAcceptanceMatrix();
    h2test::testWideStereoBitIdenticalAndSilence();
    return fails == before ? 0 : 1;
}

static int testH2Suite() {
    const int before = fails;
    std::printf("H2 full suite: wide output bus (16 < N <= 64), AmbiX3, Ring, VSL reload\n");
    h2test::testBusAcceptanceMatrix();
    h2test::testWideStereoBitIdenticalAndSilence();
    h2test::testWideAmbiX3AndMatchTrack();
    h2test::testWideImpulseAndReload();
    std::printf(fails == before ? "\nH2 suite PASSED\n" : "\nH2 suite FAILED (%d)\n", fails - before);
    return fails == before ? 0 : 1;
}
