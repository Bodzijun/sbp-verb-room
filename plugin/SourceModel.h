// A "source" is a group of input channels placed in the room. Mono = one point. A multichannel source is a set
// of point sources on a circle of `spread` metres around its centre, at the loudspeaker angles of its format
// (angle 0 = the direction the source faces). The LFE channel of 5.1/7.1 sources is not spatialised: the plugin
// adds it straight to the output LFE channel. A multichannel source can also be summed into ONE point (a mono
// voice that sits on a stereo track arrives identically in every channel; played from two places it would add
// coherently and cause position-dependent phase holes).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "directivity.hpp"

namespace verbp {

constexpr int kMaxSources = 32;
constexpr int kMaxPoints = 64;
constexpr int kMaxInputChannels = 64;

inline bool isValidSourceColorHex(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (size_t i = 1; i < 7; ++i) {
        const unsigned char ch = (unsigned char)s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
            return false;
    }
    return true;
}

inline std::string normalizeSourceColor(const std::string& s) {
    if (!isValidSourceColorHex(s)) return {};
    std::string out = s;
    for (size_t i = 1; i < 7; ++i)
        out[i] = (char)std::tolower((unsigned char)out[i]);
    return out;
}

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
    std::string color;    // optional #rrggbb lowercase editor tag; empty = automatic index colour
    int format = 0;       // index into sourceFormats()
    int channels = 1;     // number of input channels (fixed by the format, free for the ring)
    int firstIn = 0;      // first input channel (0-based)
    bool sum = false;     // multichannel only: average all channels into one point at the centre
    double spread = 0.5;  // radius of the circle the channels are placed on, metres
    double pattern = 0.0;  // directivity: 0 omni, 0.5 cardioid, 1 figure-8 (faces the source azimuth)
    std::string instrument = "none";       // "none", built-in directivity id, or "custom"
    double instrumentAmount = 1.0;         // 0..1 power-preserving blend with omnidirectional
    double aimAzDeg = 0.0, aimElDeg = 0.0, aimRollDeg = 0.0;  // per-source profile orientation correction
    std::string customDirectivityData;     // size-capped (<= 64 KB) embedded .verbdir text for portable project state
    std::string customDirectivityName;     // display name for custom profile
    std::string customDirectivityPath;     // optional path on disk (for reload / missing-file diagnostics)
    std::string directivityWarning;        // non-empty when custom file is missing/damaged or id is unavailable
    std::shared_ptr<const verb::DirectivityProfile> directivity;
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

inline void resolveSourceDirectivity(SourceCfg& c) {
    c.instrumentAmount = std::isfinite(c.instrumentAmount) ? std::min(std::max(c.instrumentAmount, 0.0), 1.0) : 1.0;
    c.aimAzDeg = std::isfinite(c.aimAzDeg) ? std::min(std::max(c.aimAzDeg, -180.0), 180.0) : 0.0;
    c.aimElDeg = std::isfinite(c.aimElDeg) ? std::min(std::max(c.aimElDeg, -90.0), 90.0) : 0.0;
    c.aimRollDeg = std::isfinite(c.aimRollDeg) ? std::min(std::max(c.aimRollDeg, -180.0), 180.0) : 0.0;
    c.directivityWarning.clear();
    if (c.instrument.empty() || c.instrument == "none") {
        c.instrument = "none";
        c.aimAzDeg = 0.0;
        c.aimElDeg = 0.0;
        c.aimRollDeg = 0.0;
        c.directivity.reset();
        return;
    }
    if (c.instrument == "custom") {
        if (!c.customDirectivityData.empty()) {
            if (c.customDirectivityData.size() > verb::kMaxVerbdirBytes) {
                c.directivity.reset();
                c.directivityWarning = "Custom directivity exceeds 64 KB size cap; degraded to omnidirectional.";
                return;
            }
            std::string err;
            verb::DirectivityProfile prof;
            if (!verb::parseVerbdir(c.customDirectivityData, prof, err)) {
                c.directivity.reset();
                c.directivityWarning = "Custom directivity invalid (" + err + "); degraded to omnidirectional.";
                return;
            }
            if (!c.customDirectivityName.empty()) prof.name = c.customDirectivityName;
            c.directivity = std::make_shared<const verb::DirectivityProfile>(std::move(prof));
            return;
        }
        if (!c.customDirectivityPath.empty()) {
            std::ifstream in(c.customDirectivityPath, std::ios::binary);
            if (!in) {
                c.directivity.reset();
                c.directivityWarning = "Missing user directivity file '" + c.customDirectivityPath + "'; degraded to omnidirectional.";
                return;
            }
            std::string text;
            char buf[4096];
            while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
                text.append(buf, (size_t)in.gcount());
                if (text.size() > verb::kMaxVerbdirBytes) {
                    c.directivity.reset();
                    c.directivityWarning = "Directivity file exceeds 64 KB size cap; degraded to omnidirectional.";
                    return;
                }
            }
            std::string err;
            verb::DirectivityProfile prof;
            if (!verb::parseVerbdir(text, prof, err)) {
                c.directivity.reset();
                c.directivityWarning = "Invalid directivity file (" + err + "); degraded to omnidirectional.";
                return;
            }
            c.customDirectivityData = std::move(text);
            if (!c.customDirectivityName.empty()) prof.name = c.customDirectivityName;
            c.directivity = std::make_shared<const verb::DirectivityProfile>(std::move(prof));
            return;
        }
        c.directivity.reset();
        c.directivityWarning = "Missing custom directivity data; degraded to omnidirectional.";
        return;
    }
    c.directivity = verb::findBuiltinDirectivity(c.instrument);
    if (!c.directivity) {
        c.directivityWarning = "Directivity profile '" + c.instrument + "' unavailable; degraded to omnidirectional.";
    }
}

// When a source's directivity profile is missing or damaged, degrade to omnidirectional (pattern = 0.0).
inline double effectiveAnalyticPattern(const SourceCfg& c) {
    return !c.directivityWarning.empty() ? 0.0 : c.pattern;
}

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
