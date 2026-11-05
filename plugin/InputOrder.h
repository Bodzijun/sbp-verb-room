// Channel order of the host's input bus.
//
// A VST3 host hands the plug-in its channels in the order of the speaker bits of the bus arrangement (ascending), and
// JUCE reorders them into its own AudioChannelSet order (the order of the ChannelType enum). For a stereo or a 5.1 bus
// the two orders agree, but a host that describes a 30- or 42-channel bus as a run of speaker bits (REAPER does) gets a
// set that contains "discreteChannel0" (the VST3 "mono" speaker, bit 19) and the ambisonic channels: they sit at other
// places in JUCE's order, and every channel after bit 19 arrives shifted. The plug-in needs "channel n of the host's
// track" for its sources, so it undoes JUCE's reordering with the same rule JUCE uses.
#pragma once
#include <algorithm>
#include <utility>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

namespace verbp {

// The VST3 speaker bit of a JUCE channel type (Steinberg::Vst::Speaker), or -1 when there is none.
inline int vst3SpeakerBit(const juce::AudioChannelSet& set, juce::AudioChannelSet::ChannelType t) {
    using C = juce::AudioChannelSet;
    switch (t) {
        case C::left: return 0;
        case C::right: return 1;
        case C::centre: return set == C::mono() ? 19 : 2;
        case C::LFE: return 3;
        case C::leftSurround: return 4;
        case C::rightSurround: return 5;
        case C::leftCentre: return 6;
        case C::rightCentre: return 7;
        case C::centreSurround: return 8;
        case C::leftSurroundSide: return 9;
        case C::rightSurroundSide: return 10;
        case C::topMiddle: return 11;
        case C::topFrontLeft: return 12;
        case C::topFrontCentre: return 13;
        case C::topFrontRight: return 14;
        case C::topRearLeft: return 15;
        case C::topRearCentre: return 16;
        case C::topRearRight: return 17;
        case C::LFE2: return 18;
        case C::discreteChannel0: return 19;
        case C::topSideLeft: return 24;
        case C::topSideRight: return 25;
        case C::leftSurroundRear: return 26;
        case C::rightSurroundRear: return 27;
        case C::bottomFrontLeft: return 28;
        case C::bottomFrontCentre: return 29;
        case C::bottomFrontRight: return 30;
        case C::proximityLeft: return 31;
        case C::proximityRight: return 32;
        case C::bottomSideLeft: return 33;
        case C::bottomSideRight: return 34;
        case C::bottomRearLeft: return 35;
        case C::bottomRearCentre: return 36;
        case C::bottomRearRight: return 37;
        case C::wideLeft: return 59;
        case C::wideRight: return 60;
        default: break;
    }
    if (t >= C::ambisonicACN0 && t <= C::ambisonicACN24) {  // ACN 0..3 are bits 20..23, ACN 4..24 are bits 38..58
        const int n = (int)t - (int)C::ambisonicACN0;
        return n <= 3 ? 20 + n : 34 + n;
    }
    return -1;
}

// For each channel of the host (0-based, in the host's order) the index of the buffer channel JUCE put it in.
// The identity for anything that cannot be worked out (the plug-in then behaves as before).
inline std::vector<int> hostToBufferIndices(const juce::AudioChannelSet& set) {
    const int n = set.size();
    std::vector<int> identity((size_t)n);
    for (int i = 0; i < n; ++i) identity[(size_t)i] = i;
    std::vector<std::pair<int, juce::AudioChannelSet::ChannelType>> bits;
    for (const auto t : set.getChannelTypes()) {
        const int b = vst3SpeakerBit(set, t);
        if (b < 0) return identity;
        bits.push_back({b, t});
    }
    std::sort(bits.begin(), bits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<int> out;
    for (const auto& p : bits) {
        const int idx = set.getChannelIndexForType(p.second);
        if (idx < 0 || idx >= n) return identity;
        out.push_back(idx);
    }
    return (int)out.size() == n ? out : identity;
}

}  // namespace verbp
