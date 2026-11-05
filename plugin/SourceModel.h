// A "source" is a group of input channels placed in the room. Mono = one point. A multichannel source is a set
// of point sources on a circle of `spread` metres around its centre, at the loudspeaker angles of its format
// (angle 0 = the direction the source faces). The LFE channel of 5.1/7.1 sources is not spatialised: the plugin
// adds it straight to the output LFE channel. A multichannel source can also be summed into ONE point (a mono
// voice that sits on a stereo track arrives identically in every channel; played from two places it would add
// coherently and cause position-dependent phase holes).
#pragma once
#include <cmath>
#include <string>
#include <vector>

namespace verbp {

constexpr int kMaxSources = 32;
constexpr int kMaxPoints = 64;
constexpr int kMaxInputChannels = 64;

struct FormatDef {
    const char* name;
    int channels;     // 0: variable (ring)
    int lfe;          // index of the LFE channel, -1 if none
    double angle[8];  // degrees, per channel (unused for the ring)
};

inline const std::vector<FormatDef>& sourceFormats() {
    static const std::vector<FormatDef> f = {
        {"Mono", 1, -1, {0}},
        {"Stereo", 2, -1, {30, -30}},
        {"LCR", 3, -1, {30, -30, 0}},
        {"Quad", 4, -1, {45, -45, 135, -135}},
        {"5.0", 5, -1, {30, -30, 0, 110, -110}},
        {"5.1", 6, 3, {30, -30, 0, 0, 110, -110}},
        {"7.1", 8, 3, {30, -30, 0, 0, 90, -90, 150, -150}},
        {"Ring (N channels)", 0, -1, {0}},
    };
    return f;
}

struct SourceCfg {
    std::string name = "Source";
    int format = 0;       // index into sourceFormats()
    int channels = 1;     // number of input channels (fixed by the format, free for the ring)
    int firstIn = 0;      // first input channel (0-based)
    bool sum = false;     // multichannel only: average all channels into one point at the centre
    double spread = 0.5;  // radius of the circle the channels are placed on, metres
    double pattern = 0.0;  // directivity: 0 omni, 0.5 cardioid, 1 figure-8 (faces the source azimuth)
    // Free points: every channel is an independent point; its offset from the source centre (in the source's own
    // frame: x forward along the azimuth, y to the left, z up) is stored here and can be dragged on the map.
    bool freePoints = false;
    std::vector<float> ox, oy, oz;

    int effectiveChannels() const {
        const auto& f = sourceFormats()[(size_t)format];
        return f.channels > 0 ? f.channels : std::max(1, channels);
    }
    int lfeChannel() const { return sourceFormats()[(size_t)format].lfe; }
};

// Offset of one channel on the format's circle (source frame).
inline void layoutOffset(const SourceCfg& c, int ch, float& x, float& y) {
    const auto& f = sourceFormats()[(size_t)c.format];
    const int n = c.effectiveChannels();
    const double a = (f.channels > 0 ? f.angle[ch] : 360.0 * ch / n) * 3.14159265358979323846 / 180.0;
    x = (float)(c.spread * std::cos(a));
    y = (float)(c.spread * std::sin(a));
}

// Turns the current circular layout into explicit per-channel offsets (the starting point for free placement).
inline void freezeLayout(SourceCfg& c) {
    const int n = c.effectiveChannels();
    c.ox.assign((size_t)n, 0.f);
    c.oy.assign((size_t)n, 0.f);
    c.oz.assign((size_t)n, 0.f);
    if (n > 1)
        for (int ch = 0; ch < n; ++ch) layoutOffset(c, ch, c.ox[(size_t)ch], c.oy[(size_t)ch]);
}

// One engine point: source index, channel within the source (-1 = the sum of the channels), placement.
struct PointMap {
    int source = 0;
    int channel = -1;
    double angleDeg = 0;  // relative to the direction the source faces
    double radius = 0;
};

inline std::vector<PointMap> buildPoints(const std::vector<SourceCfg>& cfg) {
    std::vector<PointMap> pts;
    for (size_t s = 0; s < cfg.size(); ++s) {
        const SourceCfg& c = cfg[s];
        const int n = c.effectiveChannels();
        if (n == 1 || c.sum) { pts.push_back({(int)s, n == 1 ? 0 : -1, 0.0, 0.0}); continue; }
        const auto& f = sourceFormats()[(size_t)c.format];
        for (int ch = 0; ch < n; ++ch) {
            if (ch == c.lfeChannel()) continue;
            const double a = f.channels > 0 ? f.angle[ch] : 360.0 * ch / n;
            pts.push_back({(int)s, ch, a, c.spread});
        }
    }
    return pts;
}

}  // namespace verbp
