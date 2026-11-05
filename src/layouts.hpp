// Output layouts: each channel is a virtual microphone (cardioid) aimed at its loudspeaker direction, placed
// `radius` metres from the listener centre towards that direction. The listener faces +x (yaw 0), +y is left.
// Channel order follows the usual film/ITU order: 5.1 = L R C LFE Ls Rs, 7.1 = L R C LFE Lss Rss Lrs Rrs,
// 7.1.4 = 7.1 + Ltf Rtf Ltr Rtr. The LFE channel is a silent placeholder (the plugin feeds it directly).
#pragma once
#include <string>
#include <vector>

#include "scene.hpp"

namespace verb {

enum class Layout { Mono, StereoORTF, StereoXY, LCR, Quad, Surround51, Surround71, Surround714, Ring };

struct LayoutDef {
    Layout id;
    const char* name;
    int channels;  // 0 = any (Ring)
};

inline const std::vector<LayoutDef>& layoutDefs() {
    static const std::vector<LayoutDef> d = {
        {Layout::Mono, "Mono", 1},          {Layout::StereoORTF, "Stereo (ORTF)", 2}, {Layout::StereoXY, "Stereo (XY)", 2},
        {Layout::LCR, "LCR", 3},            {Layout::Quad, "Quad", 4},                {Layout::Surround51, "5.1", 6},
        {Layout::Surround71, "7.1", 8},     {Layout::Surround714, "7.1.4", 12},       {Layout::Ring, "Ring (N channels)", 0}};
    return d;
}

// Layout whose channel count equals n (first match; Ring for anything else).
inline Layout layoutForChannels(int n) {
    switch (n) {
        case 1: return Layout::Mono;
        case 2: return Layout::StereoORTF;
        case 3: return Layout::LCR;
        case 4: return Layout::Quad;
        case 6: return Layout::Surround51;
        case 8: return Layout::Surround71;
        case 12: return Layout::Surround714;
        default: return Layout::Ring;
    }
}

struct SpeakerDef {
    const char* name;
    double az, el;  // degrees, counter-clockwise from the front, elevation up
    bool lfe = false;
};

inline std::vector<SpeakerDef> speakersFor(Layout l, int ringChannels) {
    switch (l) {
        case Layout::Mono: return {{"M", 0, 0}};
        case Layout::StereoORTF: return {{"L", 55, 0}, {"R", -55, 0}};
        case Layout::StereoXY: return {{"L", 45, 0}, {"R", -45, 0}};
        case Layout::LCR: return {{"L", 30, 0}, {"R", -30, 0}, {"C", 0, 0}};
        case Layout::Quad: return {{"FL", 45, 0}, {"FR", -45, 0}, {"RL", 135, 0}, {"RR", -135, 0}};
        case Layout::Surround51: return {{"L", 30, 0}, {"R", -30, 0}, {"C", 0, 0}, {"LFE", 0, 0, true}, {"Ls", 110, 0}, {"Rs", -110, 0}};
        case Layout::Surround71:
            return {{"L", 30, 0}, {"R", -30, 0}, {"C", 0, 0}, {"LFE", 0, 0, true}, {"Lss", 90, 0}, {"Rss", -90, 0}, {"Lrs", 150, 0}, {"Rrs", -150, 0}};
        case Layout::Surround714:
            return {{"L", 30, 0}, {"R", -30, 0}, {"C", 0, 0}, {"LFE", 0, 0, true}, {"Lss", 90, 0}, {"Rss", -90, 0}, {"Lrs", 150, 0}, {"Rrs", -150, 0},
                    {"Ltf", 45, 45}, {"Rtf", -45, 45}, {"Ltr", 135, 45}, {"Rtr", -135, 45}};
        case Layout::Ring: {
            std::vector<SpeakerDef> v;
            static std::vector<std::string> names;  // storage for the c-strings
            names.clear();
            for (int i = 0; i < ringChannels; ++i) names.push_back("Ch" + std::to_string(i + 1));
            for (int i = 0; i < ringChannels; ++i) v.push_back({names[(size_t)i].c_str(), 360.0 * i / std::max(1, ringChannels), 0});
            return v;
        }
    }
    return {};
}

// Virtual microphones for `layout` around `center` (listener at yaw 0). radius: distance of each capsule from the centre.
inline std::vector<Transducer> makeReceiverArray(Layout layout, Vec3 center, double radius, int ringChannels = 8) {
    std::vector<Transducer> out;
    const auto spk = speakersFor(layout, ringChannels);
    for (const auto& s : spk) {
        Transducer t;
        t.name = s.name;
        const double a = s.az * kPi / 180.0, e = s.el * kPi / 180.0;
        const double r = layout == Layout::Mono ? 0.0 : radius;
        t.pos = {center.x + r * std::cos(e) * std::cos(a), center.y + r * std::cos(e) * std::sin(a), center.z + r * std::sin(e)};
        t.azimuthDeg = s.az;
        t.elevationDeg = s.el;
        t.pattern = layout == Layout::Mono ? 0.0 : 0.5;  // cardioid towards the loudspeaker, omni for a mono output
        t.gainDb = s.lfe ? -120.0 : 0.0;
        out.push_back(t);
    }
    // ORTF is a spaced pair: 17 cm between capsules
    if (layout == Layout::StereoORTF && out.size() == 2) { out[0].pos = {center.x, center.y + 0.085, center.z}; out[1].pos = {center.x, center.y - 0.085, center.z}; }
    if (layout == Layout::StereoXY && out.size() == 2) { out[0].pos = center; out[1].pos = center; }
    return out;
}

// ---- microphone patterns of the listener ----
// The directivity of every virtual microphone: gain = (1 - p) + p cos(angle to the capsule's axis). p = 0 omni, 0.5 cardioid,
// 1 figure-8. "Human ears" is a head model: omni at low frequencies, a growing head shadow towards the high bands (p from 0 at
// 125 Hz to 0.45 at 8 kHz: about -20 dB behind the ear at 8 kHz, -5 dB at 1 kHz); with a stereo output the capsules also move to
// the ears (17.5 cm apart) and aim sideways (+-90 degrees).
enum class MicPattern { LayoutDefault, Omni, Subcardioid, Cardioid, Supercardioid, Hypercardioid, Figure8, HumanEars };

struct MicPatternDef {
    MicPattern id;
    const char* name;
    double p;
};

inline const std::vector<MicPatternDef>& micPatternDefs() {
    static const std::vector<MicPatternDef> d = {
        {MicPattern::LayoutDefault, "As the layout (cardioid per channel)", 0.5}, {MicPattern::Omni, "Omni", 0.0},
        {MicPattern::Subcardioid, "Subcardioid", 0.3},                            {MicPattern::Cardioid, "Cardioid", 0.5},
        {MicPattern::Supercardioid, "Supercardioid", 0.63},                       {MicPattern::Hypercardioid, "Hypercardioid", 0.75},
        {MicPattern::Figure8, "Figure-8", 1.0},                                   {MicPattern::HumanEars, "Human ears (head shadow)", 0.0}};
    return d;
}

// Human ears: a stereo pair sits `radius` metres either side of the head centre (the Mic radius slider; a head is
// about 0.0875 m), aimed at the ears with the head shadow.
inline void applyMicPattern(std::vector<Transducer>& rcv, MicPattern m, double radius = 0.0875) {
    if (m == MicPattern::LayoutDefault) return;
    if (m == MicPattern::HumanEars) {
        for (auto& t : rcv) { t.pattern = 0.0; t.patternHf = 0.45; }
        if (rcv.size() == 2) {  // a stereo pair becomes two ears
            const Vec3 mid = (rcv[0].pos + rcv[1].pos) * 0.5;
            rcv[0].pos = {mid.x, mid.y + radius, mid.z};
            rcv[1].pos = {mid.x, mid.y - radius, mid.z};
            rcv[0].azimuthDeg = 90;
            rcv[1].azimuthDeg = -90;
        }
        return;
    }
    for (const auto& d : micPatternDefs())
        if (d.id == m)
            for (auto& t : rcv) { if (t.gainDb > -100) t.pattern = d.p; t.patternHf = -1; }
}

}  // namespace verb
