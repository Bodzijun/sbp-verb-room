#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace verbp {
using PathPoint = std::array<double, 3>;
inline constexpr double kMinPathWindowGap = 0.01;
inline constexpr size_t kMaxAzKeys = 128;
inline constexpr double kAzKeyMinGap = 1e-6;
enum class PathEasing : uint8_t { Linear = 0, EaseIn = 1, EaseOut = 2, EaseInOut = 3 };
enum class AzimuthEasing : uint8_t { Linear = 0, EaseIn = 1, EaseOut = 2, EaseInOut = 3, Hold = 4 };
struct AzimuthKey {
    double t = 0.0;        // Timeline Position axis 0.0 .. 100.0 (%)
    double azDeg = 0.0;    // Degrees [-180.0, 180.0]
    AzimuthEasing easing = AzimuthEasing::Linear;
    bool operator==(const AzimuthKey& b) const noexcept {
        return t == b.t && azDeg == b.azDeg && easing == b.easing;
    }
};

inline double applyPathEasing(PathEasing e, double s) noexcept {
    s = std::clamp(s, 0.0, 1.0);
    switch (e) {
    case PathEasing::EaseIn: return s * s;
    case PathEasing::EaseOut: { const double inv = 1.0 - s; return 1.0 - inv * inv; }
    case PathEasing::EaseInOut: return s * s * (3.0 - 2.0 * s);
    case PathEasing::Linear:
    default: return s;
    }
}

inline double invertPathEasing(PathEasing e, double r) noexcept {
    r = std::clamp(r, 0.0, 1.0);
    switch (e) {
    case PathEasing::EaseIn: return std::sqrt(r);
    case PathEasing::EaseOut: return 1.0 - std::sqrt(std::max(0.0, 1.0 - r));
    case PathEasing::EaseInOut: return std::clamp(0.5 - std::sin(std::asin(std::clamp(1.0 - 2.0 * r, -1.0, 1.0)) / 3.0), 0.0, 1.0);
    case PathEasing::Linear:
    default: return r;
    }
}

inline constexpr int kOrientMaxObject = 32;  // == kListener; static_assert'ed in PluginProcessor.h
enum class OrientMode : uint8_t { Manual = 0, AlongPath = 1, LookAt = 2 };

inline constexpr double kMaxPathLfoRateHz = 8.0;
inline constexpr double kMinPathWanderRateHz = 0.05;
inline constexpr double kMaxPathWanderRateHz = 8.0;
inline constexpr double kMaxPathWanderHorizM = 20.0;
inline constexpr double kMaxPathWanderVertM = 10.0;
inline constexpr double kDefaultEndlessPeriodS = 8.0;
inline constexpr double kMinEndlessPeriodS = 0.25;
inline constexpr double kMaxEndlessPeriodS = 3600.0;

inline double endlessTransportPhase(int64_t samplePos, double sampleRate, double periodS) noexcept {
    if (!(sampleRate > 0.0) || !(periodS > 0.0) || !std::isfinite(sampleRate) || !std::isfinite(periodS)) return 0.0;
    const double periodSamples = periodS * sampleRate;
    if (!(periodSamples > 0.0) || !std::isfinite(periodSamples)) return 0.0;
    double rem = std::fmod((double)samplePos, periodSamples);
    if (rem < 0.0) rem += periodSamples;
    double phase = rem / periodSamples;
    if (phase >= 1.0 || phase < 0.0 || !std::isfinite(phase)) phase = 0.0;
    return phase;
}

inline double endlessTransportPhaseSec(double tSec, double periodS) noexcept {
    if (!(periodS > 0.0) || !std::isfinite(tSec) || !std::isfinite(periodS)) return 0.0;
    double rem = std::fmod(tSec, periodS);
    if (rem < 0.0) rem += periodS;
    double phase = rem / periodS;
    if (phase >= 1.0 || phase < 0.0 || !std::isfinite(phase)) phase = 0.0;
    return phase;
}

enum class LfoShape : uint8_t { Sine = 0, Triangle = 1, Saw = 2, Square = 3, SmoothRandom = 4 };

inline const char* lfoShapeName(LfoShape s) noexcept {
    switch (s) {
    case LfoShape::Sine: return "sine";
    case LfoShape::Triangle: return "triangle";
    case LfoShape::Saw: return "saw";
    case LfoShape::Square: return "square";
    case LfoShape::SmoothRandom: return "smoothRandom";
    default: return "sine";
    }
}

inline bool parseLfoShape(const char* name, LfoShape& out) noexcept {
    if (!name) return false;
    std::string_view sv(name);
    if (sv == "sine") { out = LfoShape::Sine; return true; }
    if (sv == "triangle") { out = LfoShape::Triangle; return true; }
    if (sv == "saw") { out = LfoShape::Saw; return true; }
    if (sv == "square") { out = LfoShape::Square; return true; }
    if (sv == "smoothRandom") { out = LfoShape::SmoothRandom; return true; }
    return false;
}

inline double lfoHashNode(uint32_t seed, int64_t idx) noexcept {
    uint64_t z = ((uint64_t)seed * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)idx + 0xBF58476D1CE4E5B9ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    const double u = (double)(z >> 11) * (1.0 / 9007199254740992.0); // [0, 1)
    return u * 2.0 - 1.0; // [-1, 1)
}

struct PathLfo {
    bool enabled = false;
    LfoShape shape = LfoShape::Sine;
    double rateHz = 0.5;
    double depth = 0.0;
    double phaseDeg = 0.0;
    uint32_t seed = 1;

    bool isDefault() const noexcept {
        return !enabled && shape == LfoShape::Sine && rateHz == 0.5 && depth == 0.0 && phaseDeg == 0.0 && seed == 1;
    }
    bool operator==(const PathLfo& b) const noexcept {
        return enabled == b.enabled && shape == b.shape && rateHz == b.rateHz &&
               depth == b.depth && phaseDeg == b.phaseDeg && seed == b.seed;
    }
    double evaluate(double tSec) const noexcept {
        const double cycles = std::max(0.0, tSec) * rateHz + phaseDeg / 360.0;
        const int64_t baseIdx = (int64_t)std::floor(cycles);
        const double frac = cycles - (double)baseIdx;
        switch (shape) {
        case LfoShape::Sine:
            return std::sin(2.0 * 3.14159265358979323846 * frac);
        case LfoShape::Triangle:
            if (frac < 0.25) return 4.0 * frac;
            if (frac < 0.75) return 2.0 - 4.0 * frac;
            return 4.0 * frac - 4.0;
        case LfoShape::Saw:
            return 2.0 * frac - 1.0;
        case LfoShape::Square:
            return frac < 0.5 ? 1.0 : -1.0;
        case LfoShape::SmoothRandom: {
            const double p0 = lfoHashNode(seed, baseIdx - 1);
            const double p1 = lfoHashNode(seed, baseIdx);
            const double p2 = lfoHashNode(seed, baseIdx + 1);
            const double p3 = lfoHashNode(seed, baseIdx + 2);
            const double t = frac;
            const double t2 = t * t, t3 = t2 * t;
            const double v = 0.5 * ((2.0 * p1) +
                (-p0 + p2) * t +
                (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
            return std::clamp(v, -1.0, 1.0);
        }
        default:
            return 0.0;
        }
    }
    double applyToU(double uBase, double tSec, bool closed, double depthScale = 1.0) const noexcept {
        if (!enabled || depth <= 0.0 || depthScale <= 0.0) return std::clamp(uBase, 0.0, 1.0);
        const double uRaw = uBase + (depth * depthScale) * evaluate(tSec);
        if (closed) {
            double wrapped = uRaw - std::floor(uRaw);
            if (wrapped >= 1.0 || wrapped < 0.0) wrapped = 0.0;
            return wrapped;
        }
        return std::clamp(uRaw, 0.0, 1.0);
    }
};

inline double wanderHashNode(uint32_t seed, int axis, int64_t idx) noexcept {
    uint64_t z = ((uint64_t)seed * 0x9E3779B97F4A7C15ULL)
               ^ ((uint64_t)(axis + 1) * 0xD1B54A32D192ED03ULL)
               ^ ((uint64_t)idx * 0x94D049BB133111EBULL + 0xBF58476D1CE4E5B9ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    const double u = (double)(z >> 11) * (1.0 / 9007199254740992.0); // [0, 1)
    return u * 2.0 - 1.0; // [-1, 1)
}

inline double evaluateWanderAxis(uint32_t seed, int axis, double phase, double smoothness) noexcept {
    const double s = std::clamp(std::isfinite(smoothness) ? smoothness : 0.8, 0.0, 1.0);
    const double p = std::isfinite(phase) ? phase : 0.0;
    const int64_t baseIdx = (int64_t)std::floor(p);
    const double t = p - (double)baseIdx;
    const double t2 = t * t, t3 = t2 * t;
    const double n0 = wanderHashNode(seed, axis, baseIdx - 1);
    const double n1 = wanderHashNode(seed, axis, baseIdx);
    const double n2 = wanderHashNode(seed, axis, baseIdx + 1);
    const double n3 = wanderHashNode(seed, axis, baseIdx + 2);
    const double cr = 0.5 * ((2.0 * n1) +
        (-n0 + n2) * t +
        (2.0 * n0 - 5.0 * n1 + 4.0 * n2 - n3) * t2 +
        (-n0 + 3.0 * n1 - 3.0 * n2 + n3) * t3);
    const double omt = 1.0 - t;
    const double bs = (1.0 / 6.0) * (
        (omt * omt * omt) * n0 +
        (3.0 * t3 - 6.0 * t2 + 4.0) * n1 +
        (-3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0) * n2 +
        t3 * n3);
    const double blended = (1.0 - s) * cr + s * bs;
    const double rms = std::sqrt(std::max(1e-6, 0.27143476 - 0.14339413 * s + 0.03174841 * s * s));
    return blended / rms;
}

struct PathWander {
    bool enabled = false;
    double ampHorizM = 0.5;
    double ampVertM = 0.25;
    double rateHz = 0.5;
    double smoothness = 0.8;
    bool pinEnds = true;
    bool horizOnly = false;
    uint32_t seed = 1;

    bool isDefault() const noexcept {
        return !enabled && ampHorizM == 0.5 && ampVertM == 0.25 && rateHz == 0.5 &&
               smoothness == 0.8 && pinEnds == true && horizOnly == false && seed == 1;
    }
    bool isActive() const noexcept {
        return enabled && (ampHorizM > 0.0 || (!horizOnly && ampVertM > 0.0));
    }
    bool operator==(const PathWander& b) const noexcept {
        return enabled == b.enabled && ampHorizM == b.ampHorizM && ampVertM == b.ampVertM &&
               rateHz == b.rateHz && smoothness == b.smoothness && pinEnds == b.pinEnds &&
               horizOnly == b.horizOnly && seed == b.seed;
    }
    double endEnvelope(double uPath, bool closed) const noexcept {
        if (closed || !pinEnds) return 1.0;
        const double uc = std::clamp(std::isfinite(uPath) ? uPath : 0.0, 0.0, 1.0);
        const double edge = std::min(uc, 1.0 - uc);
        const double s = std::clamp(edge / 0.15, 0.0, 1.0);
        return s * s * (3.0 - 2.0 * s);
    }
    double computePhase(double tSec, double uUnwrapped = 0.0) const noexcept {
        return std::max(0.0, std::isfinite(tSec) ? tSec : 0.0) * rateHz
             + (std::isfinite(uUnwrapped) ? uUnwrapped : 0.0);
    }
    void evaluateLocalOffsets(double tSec, double uUnwrapped, double uPath, bool closed,
                              double& outHorizM, double& outVertM) const noexcept {
        if (!isActive()) { outHorizM = 0.0; outVertM = 0.0; return; }
        const double env = endEnvelope(uPath, closed);
        if (env <= 0.0) { outHorizM = 0.0; outVertM = 0.0; return; }
        const double phase = computePhase(tSec, uUnwrapped);
        outHorizM = ampHorizM > 0.0 ? (ampHorizM * env * evaluateWanderAxis(seed, 0, phase, smoothness)) : 0.0;
        outVertM = (!horizOnly && ampVertM > 0.0) ? (ampVertM * env * evaluateWanderAxis(seed, 1, phase, smoothness)) : 0.0;
    }
    void evaluateWorldDelta(double tSec, double uUnwrapped, double uPath, bool closed, double tangentYawDeg,
                            double& dx, double& dy, double& dz) const noexcept {
        double h = 0.0, v = 0.0;
        evaluateLocalOffsets(tSec, uUnwrapped, uPath, closed, h, v);
        const double rad = (std::isfinite(tangentYawDeg) ? tangentYawDeg : 0.0) * (3.14159265358979323846 / 180.0);
        // Horizontal perpendicular unit vector in plan XY: (-sin(yaw), cos(yaw))
        dx = -std::sin(rad) * h;
        dy = std::cos(rad) * h;
        dz = v;
    }
};

// V04d: "Link to path" (rig). A follower has no own geometry: it takes the target's effective path parameter
// (Position / Laps / Endless / LFO / timing) and pose, plus a manual offset in the target's path frame:
// forward = horizontal tangent of the target's drawn path at the current parameter (held across horizontally
// degenerate segments), left = horizontal perpendicular, up = world Z. Absent (target < 0) = ordinary path.
inline constexpr double kMaxFollowOffsetM = 50.0;
struct PathFollow {
    int target = -1;                    // source index 0..kOrientMaxObject-1; -1 = not linked
    PathPoint offset{0.0, 0.0, 0.0};    // forward, left, up (m)
    bool followWander = true;           // false: ignore the target's Path wander displacement
    bool active() const noexcept { return target >= 0; }
    bool operator==(const PathFollow& b) const noexcept {
        return target == b.target && offset == b.offset && followWander == b.followWander;
    }
};
inline bool validPathFollow(const PathFollow& f) noexcept {
    if (f.target < -1 || f.target >= kOrientMaxObject) return false;
    for (double v : f.offset) if (!std::isfinite(v) || std::abs(v) > kMaxFollowOffsetM) return false;
    return true;
}

struct MovementPath {
    std::vector<PathPoint> points;
    bool closed = false, smooth = false, orient = false, enabled = true;
    // E19 orientation: Manual (default), Along path (orient) or Look at. Offset is degrees, added to Along path / Look at.
    bool lookAt = false, lookPoint = false;
    int lookTarget = -1;             // object index (0..kOrientMaxObject-1 source, kOrientMaxObject listener); ignored when lookPoint
    PathPoint lookPos{0.0, 0.0, 0.0}; // fixed target when lookPoint
    double orientOffset = 0.0;
    int laps = 1;
    bool endless = false;
    double endlessPeriodS = kDefaultEndlessPeriodS;
    int group = -1;
    double start = 0.0, end = 100.0;
    std::vector<std::optional<double>> times;
    std::vector<PathEasing> easings;
    std::vector<AzimuthKey> azKeys;
    PathLfo lfo;
    PathWander wander;
    PathFollow follow;  // V04d
    bool hasCustomTiming() const noexcept {
        if (start != 0.0 || end != 100.0) return true;
        for (const auto& t : times) if (t.has_value()) return true;
        for (auto e : easings) if (e != PathEasing::Linear) return true;
        return false;
    }
    bool hasAzimuthKeys() const noexcept { return !azKeys.empty(); }
    bool operator==(const MovementPath& b) const {
        if (!(points == b.points && closed == b.closed && smooth == b.smooth && orient == b.orient && lookAt == b.lookAt && lookPoint == b.lookPoint && lookTarget == b.lookTarget && lookPos == b.lookPos && orientOffset == b.orientOffset && enabled == b.enabled && laps == b.laps && endless == b.endless && endlessPeriodS == b.endlessPeriodS && group == b.group && start == b.start && end == b.end && azKeys == b.azKeys && lfo == b.lfo && wander == b.wander && follow == b.follow))
            return false;
        const size_t n = std::max({points.size(), times.size(), b.times.size(), easings.size(), b.easings.size()});
        for (size_t i = 0; i < n; ++i) {
            const auto ta = i < times.size() ? times[i] : std::optional<double>{};
            const auto tb = i < b.times.size() ? b.times[i] : std::optional<double>{};
            if (ta != tb) return false;
            const auto ea = i < easings.size() ? easings[i] : PathEasing::Linear;
            const auto eb = i < b.easings.size() ? b.easings[i] : PathEasing::Linear;
            if (ea != eb) return false;
        }
        return true;
    }
};

inline void normalizePathTiming(MovementPath& p) {
    if (p.points.size() < 2) { p.times.clear(); p.easings.clear(); return; }
    const size_t n = p.points.size();
    if (!p.times.empty()) {
        p.times.resize(n);
        p.times[0].reset();
        if (!p.closed) p.times[n - 1].reset();
        if (std::none_of(p.times.begin(), p.times.end(), [](const auto& t) { return t.has_value(); }))
            p.times.clear();
    }
    if (!p.easings.empty()) {
        p.easings.resize(n, PathEasing::Linear);
        for (size_t i = 0; i < n; ++i) {
            const bool isKey = (i == 0) || (!p.times.empty() && p.times[i].has_value());
            const bool isLast = (!p.closed && i + 1 == n);
            if (!isKey || isLast) p.easings[i] = PathEasing::Linear;
        }
        if (std::all_of(p.easings.begin(), p.easings.end(), [](PathEasing e) { return e == PathEasing::Linear; }))
            p.easings.clear();
    }
}

inline void normalizePathAzKeys(MovementPath& p) {
    if (p.points.size() < 2 && !p.follow.active()) { p.azKeys.clear(); return; }
    if (p.azKeys.empty()) return;
    std::stable_sort(p.azKeys.begin(), p.azKeys.end(), [](const AzimuthKey& a, const AzimuthKey& b) {
        const bool af = std::isfinite(a.t), bf = std::isfinite(b.t);
        if (af != bf) return af > bf;
        return af && a.t < b.t;
    });
    std::vector<AzimuthKey> dedup;
    dedup.reserve(p.azKeys.size());
    for (auto k : p.azKeys) {
        if (k.t == 0.0) k.t = 0.0;
        if (k.azDeg == 0.0) k.azDeg = 0.0;
        if (!dedup.empty() && std::isfinite(k.t) && std::isfinite(dedup.back().t) && std::abs(k.t - dedup.back().t) < kAzKeyMinGap)
            dedup.back() = k;
        else
            dedup.push_back(k);
    }
    p.azKeys = std::move(dedup);
}

inline std::optional<double> evaluateAzimuthKeys(const std::vector<AzimuthKey>& keys, double u) noexcept {
    if (keys.empty()) return std::nullopt;
    const double tPct = (std::isfinite(u) ? std::clamp(u, 0.0, 1.0) : 0.0) * 100.0;
    if (tPct <= keys.front().t) return keys.front().azDeg;
    if (tPct >= keys.back().t) return keys.back().azDeg;
    auto it = std::upper_bound(keys.begin(), keys.end(), tPct,
        [](double val, const AzimuthKey& k) { return val < k.t; });
    const size_t hi = std::clamp<size_t>((size_t)(it - keys.begin()), 1, keys.size() - 1);
    const size_t lo = hi - 1;
    if (keys[lo].easing == AzimuthEasing::Hold || keys[hi].t <= keys[lo].t)
        return keys[lo].azDeg;
    const double s = (tPct - keys[lo].t) / (keys[hi].t - keys[lo].t);
    const double e = applyPathEasing((PathEasing)keys[lo].easing, s);
    const double delta = std::remainder(keys[hi].azDeg - keys[lo].azDeg, 360.0);
    const double out = std::remainder(keys[lo].azDeg + delta * e, 360.0);
    return out == 0.0 ? 0.0 : out;
}

inline bool validPathLfo(const PathLfo& l) noexcept {
    if ((uint8_t)l.shape > (uint8_t)LfoShape::SmoothRandom) return false;
    if (!std::isfinite(l.rateHz) || l.rateHz < 0.0 || l.rateHz > kMaxPathLfoRateHz) return false;
    if (!std::isfinite(l.depth) || l.depth < 0.0 || l.depth > 1.0) return false;
    if (!std::isfinite(l.phaseDeg) || l.phaseDeg < 0.0 || l.phaseDeg > 360.0) return false;
    if (l.seed < 1 || l.seed > 999999) return false;
    return true;
}

inline bool validPathWander(const PathWander& w) noexcept {
    if (!std::isfinite(w.ampHorizM) || w.ampHorizM < 0.0 || w.ampHorizM > kMaxPathWanderHorizM) return false;
    if (!std::isfinite(w.ampVertM) || w.ampVertM < 0.0 || w.ampVertM > kMaxPathWanderVertM) return false;
    if (!std::isfinite(w.rateHz) || w.rateHz < kMinPathWanderRateHz || w.rateHz > kMaxPathWanderRateHz) return false;
    if (!std::isfinite(w.smoothness) || w.smoothness < 0.0 || w.smoothness > 1.0) return false;
    if (w.seed < 1 || w.seed > 999999) return false;
    return true;
}

inline bool validPath(const MovementPath& p) {
    if (!validPathLfo(p.lfo)) return false;
    if (!validPathWander(p.wander)) return false;
    if (!std::isfinite(p.orientOffset) || std::abs(p.orientOffset) > 360.0) return false;
    if (p.lookTarget < -1 || p.lookTarget > kOrientMaxObject) return false;
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(p.lookPos[(size_t)k]) || p.lookPos[(size_t)k] < (k == 2 ? 0 : -200) || p.lookPos[(size_t)k] > (k == 2 ? 30 : 200)) return false;
    if (p.laps < 1 || p.laps > 64) return false;
    if (!std::isfinite(p.endlessPeriodS) || p.endlessPeriodS < kMinEndlessPeriodS || p.endlessPeriodS > kMaxEndlessPeriodS) return false;
    if (p.group < -1 || p.group > 31) return false;
    if (!std::isfinite(p.start) || !std::isfinite(p.end) || p.start < 0.0 || p.end > 100.0 || p.end - p.start < kMinPathWindowGap)
        return false;
    if (p.azKeys.size() > kMaxAzKeys) return false;
    if (!validPathFollow(p.follow)) return false;
    if (p.follow.active()) {
        // A follower stores no own geometry, timing, LFO, wander, Endless, Laps or Coupling (all inherited from the target).
        if (!p.points.empty() || !p.times.empty() || !p.easings.empty() || !p.lfo.isDefault() || !p.wander.isDefault()
            || p.endless || p.endlessPeriodS != kDefaultEndlessPeriodS || p.laps != 1 || p.group != -1 || p.start != 0.0 || p.end != 100.0)
            return false;
        for (size_t i = 0; i < p.azKeys.size(); ++i) {
            const auto& k = p.azKeys[i];
            if (!std::isfinite(k.t) || k.t < 0.0 || k.t > 100.0) return false;
            if (!std::isfinite(k.azDeg) || k.azDeg < -180.0 || k.azDeg > 180.0) return false;
            if ((uint8_t)k.easing > (uint8_t)AzimuthEasing::Hold) return false;
            if (i > 0 && !(k.t > p.azKeys[i - 1].t)) return false;
        }
        return true;
    }
    if (p.points.empty()) return p.times.empty() && p.easings.empty() && p.azKeys.empty() && p.lfo.isDefault() && p.wander.isDefault() && !p.endless && p.endlessPeriodS == kDefaultEndlessPeriodS;
    if (p.points.size() < 2 || p.points.size() > 64) return false;
    for (const auto& q : p.points) for (int k = 0; k < 3; ++k)
        if (!std::isfinite(q[k]) || q[k] < (k == 2 ? 0 : -200) || q[k] > (k == 2 ? 30 : 200)) return false;
    if (!p.times.empty() && p.times.size() != p.points.size()) return false;
    if (!p.easings.empty() && p.easings.size() != p.points.size()) return false;
    for (auto e : p.easings)
        if ((uint8_t)e > (uint8_t)PathEasing::EaseInOut) return false;
    double prevTime = 0.0;
    for (size_t i = 0; i < p.times.size(); ++i) {
        if (!p.times[i].has_value()) continue;
        const double t = *p.times[i];
        if (!std::isfinite(t) || t < 0.0 || t > 100.0) return false;
        if (i == 0 && t != 0.0) return false;
        if (!p.closed && i + 1 == p.points.size() && t != 100.0) return false;
        if (t < prevTime) return false;
        prevTime = t;
    }
    for (size_t i = 0; i < p.azKeys.size(); ++i) {
        const auto& k = p.azKeys[i];
        if (!std::isfinite(k.t) || k.t < 0.0 || k.t > 100.0) return false;
        if (!std::isfinite(k.azDeg) || k.azDeg < -180.0 || k.azDeg > 180.0) return false;
        if ((uint8_t)k.easing > (uint8_t)AzimuthEasing::Hold) return false;
        if (i > 0 && !(k.t > p.azKeys[i - 1].t)) return false;
    }
    return true;
}

// V04d: the path a follower actually runs: the target's geometry, timing, Laps, Endless, LFO (and wander when
// followWander), with the follower's own enable / orientation / azimuth keys. An unusable target (missing, disabled,
// fewer than 2 points, itself a follower, or the follower itself) yields an empty path: the follower stays static.
inline MovementPath resolveFollowerPath(const MovementPath& f, const MovementPath* target, int self = -1) {
    MovementPath r;
    r.follow = f.follow;
    r.enabled = f.enabled;
    r.orient = f.orient; r.lookAt = f.lookAt; r.lookPoint = f.lookPoint; r.lookTarget = f.lookTarget; r.lookPos = f.lookPos;
    r.orientOffset = f.orientOffset;
    r.azKeys = f.azKeys;
    if (!target || f.follow.target == self || target->follow.active() || !target->enabled || target->points.size() < 2) {
        r.enabled = false;
        return r;
    }
    const auto& t = *target;
    r.points = t.points; r.closed = t.closed; r.smooth = t.smooth; r.laps = t.laps;
    r.endless = t.endless; r.endlessPeriodS = t.endlessPeriodS;
    r.start = t.start; r.end = t.end; r.times = t.times; r.easings = t.easings;
    r.lfo = t.lfo;
    if (f.follow.followWander) r.wander = t.wander;
    r.group = -1;  // a follower is never a Coupling driver; its parameter comes from the target (and the target's group)
    return r;
}
// V04d: local path frame -> world delta. forward = (cos yaw, sin yaw), left = (-sin yaw, cos yaw), up = world Z.
inline void followOffsetDelta(double yawDeg, const PathPoint& off, double& dx, double& dy, double& dz) noexcept {
    const double r = (std::isfinite(yawDeg) ? yawDeg : 0.0) * (3.14159265358979323846 / 180.0);
    const double c = std::cos(r), s = std::sin(r);
    dx = c * off[0] - s * off[1];
    dy = s * off[0] + c * off[1];
    dz = off[2];
}

template <size_t N>
inline int pathGroupDriver(const std::array<MovementPath, N>& paths, int numSources, int object) {
    if (object < 0 || object >= (int)N) return object;
    const auto& p = paths[(size_t)object];
    if (p.group < 0 || !p.enabled || p.points.size() < 2) return object;
    for (int o = 0; o < numSources && o + 1 < (int)N; ++o) {
        const auto& c = paths[(size_t)o];
        if (c.enabled && c.points.size() >= 2 && c.group == p.group) return o;
    }
    return object;
}
inline PathPoint pathMix(const PathPoint& a,const PathPoint& b,double u) {
    return {a[0]+(b[0]-a[0])*u,a[1]+(b[1]-a[1])*u,a[2]+(b[2]-a[2])*u};
}
inline double pathDistance(const PathPoint& a,const PathPoint& b) { return std::hypot(std::hypot(a[0]-b[0],a[1]-b[1]),a[2]-b[2]); }
// Centripetal Barry-Goldman interpolation. Open end spans use the three-point
// knot parabola (no phantom), preserving its tangent at the neighbouring interior span.
inline PathPoint pathCurve(const MovementPath& p,int segment,double u) {
    const int n=(int)p.points.size();
    auto at=[&](int i)->PathPoint { if(p.closed)return p.points[(size_t)((i%n+n)%n)];
        if(i<0)return pathMix(p.points[1],p.points[0],2);
        if(i>=n)return pathMix(p.points[(size_t)n-2],p.points.back(),2);
        return p.points[(size_t)i]; };
    const auto a=at(segment-1),b=at(segment),c=at(segment+1),d=at(segment+2);
    if(!p.smooth)return pathMix(b,c,u);
    if(!p.closed&&n==2)return pathMix(b,c,u);
    if(pathDistance(b,c)<1e-10)return b;
    auto blend=[](const PathPoint& x,const PathPoint& y,double lo,double hi,double v){return pathMix(x,y,(v-lo)/(hi-lo));};
    if(!p.closed&&segment==0){
        const double t1=0,t2=std::max(1e-5,std::sqrt(pathDistance(b,c))),t3=t2+std::max(1e-5,std::sqrt(pathDistance(c,d))),t=t1+u*(t2-t1);
        return blend(blend(b,c,t1,t2,t),blend(c,d,t2,t3,t),t1,t3,t);
    }
    const double t0=0,t1=std::max(1e-5,std::sqrt(pathDistance(a,b))),t2=t1+std::max(1e-5,std::sqrt(pathDistance(b,c))),t3=t2+std::max(1e-5,std::sqrt(pathDistance(c,d))),t=t1+u*(t2-t1);
    if(!p.closed&&segment==n-2)return blend(blend(a,b,t0,t1,t),blend(b,c,t1,t2,t),t0,t2,t);
    auto a1=blend(a,b,t0,t1,t),a2=blend(b,c,t1,t2,t),a3=blend(c,d,t2,t3,t);
    return blend(blend(a1,a2,t0,t2,t),blend(a2,a3,t1,t3,t),t1,t2,t);
}
struct PathTimeKey {
    double t = 0.0;
    double f = 0.0;
    PathEasing easing = PathEasing::Linear;
};
struct CompiledPath {
    std::vector<PathPoint> points;
    std::vector<double> lengths;
    bool enabled=false,orient=false,closed=false;
    bool lookAt=false,lookPoint=false;
    int lookTarget=-1;
    PathPoint lookPos{0.0,0.0,0.0};
    double orientOffset=0.0;
    int laps=1, group=-1;
    bool endless = false;
    double endlessPeriodS = kDefaultEndlessPeriodS;
    double startNorm = 0.0, endNorm = 1.0;
    bool hasTiming = false;
    std::vector<PathTimeKey> timeKeys;
    std::vector<AzimuthKey> azKeys;
    PathLfo lfo{};
    PathWander wander{};
    // V04d follower (built from resolveFollowerPath): target index, offset and the held horizontal heading per polyline segment.
    int followTarget = -1;
    PathPoint followOffset{0.0, 0.0, 0.0};
    std::vector<double> followYawTab;
    bool hasAzimuthKeys() const noexcept { return !azKeys.empty(); }
    void build(const MovementPath& p) {
        points.clear(); lengths.clear(); timeKeys.clear(); azKeys.clear(); lfo = p.lfo; wander = p.wander;
        followTarget = p.follow.active() ? p.follow.target : -1; followOffset = p.follow.offset; followYawTab.clear();
        enabled=p.enabled && p.points.size()>=2;orient=p.orient;closed=p.closed;lookAt=p.lookAt;lookPoint=p.lookPoint;lookTarget=p.lookTarget;lookPos=p.lookPos;orientOffset=p.orientOffset;laps=std::clamp(p.laps,1,64);group=p.group;
        endless = p.closed && p.endless;
        endlessPeriodS = std::clamp(std::isfinite(p.endlessPeriodS) ? p.endlessPeriodS : kDefaultEndlessPeriodS, kMinEndlessPeriodS, kMaxEndlessPeriodS);
        startNorm = std::clamp(p.start, 0.0, 100.0) * 0.01;
        endNorm = std::clamp(p.end, 0.0, 100.0) * 0.01;
        hasTiming = p.hasCustomTiming();
        if(p.points.size()<2)return;
        azKeys = p.azKeys;
        const int n=(int)p.points.size();
        const int segments=n-(p.closed?0:1), steps=p.smooth?128:1;
        points.push_back(p.points.front()); lengths.push_back(0);
        for(int s=0;s<segments;++s)for(int j=1;j<=steps;++j){
            auto q=(p.closed && s==segments-1 && j==steps)?p.points.front():pathCurve(p,s,(double)j/steps);
            lengths.push_back(lengths.back()+pathDistance(points.back(),q));points.push_back(q);
        }
        const double total = length();
        auto knotFrac = [&](int k) -> double {
            if (k <= 0) return 0.0;
            if (k >= segments) return 1.0;
            return total > 0.0 ? lengths[(size_t)k * (size_t)steps] / total : (double)k / (double)segments;
        };
        auto easingOf = [&](int k) -> PathEasing {
            return (k >= 0 && (size_t)k < p.easings.size()) ? p.easings[(size_t)k] : PathEasing::Linear;
        };
        timeKeys.push_back({0.0, 0.0, easingOf(0)});
        for (int k = 1; k < n; ++k) {
            if (!p.closed && k == n - 1) break;
            if ((size_t)k < p.times.size() && p.times[(size_t)k].has_value()) {
                timeKeys.push_back({std::clamp(*p.times[(size_t)k] * 0.01, 0.0, 1.0), knotFrac(k), easingOf(k)});
            }
        }
        timeKeys.push_back({1.0, 1.0, PathEasing::Linear});
        if (followTarget >= 0) buildFollowYaw();
    }
    // V04d: heading of segment i (points[i-1] -> points[i]) in plan; a segment without horizontal extent (vertical
    // or zero length) holds the previous valid heading (the next one before the first valid; closed paths wrap).
    void buildFollowYaw() {
        const size_t n = points.size();
        followYawTab.assign(n, std::numeric_limits<double>::quiet_NaN());
        if (n < 2) { followYawTab.assign(n, 0.0); return; }
        int firstValid = -1, lastValid = -1;
        for (size_t i = 1; i < n; ++i) {
            const double dx = points[i][0] - points[i - 1][0], dy = points[i][1] - points[i - 1][1];
            if (std::hypot(dx, dy) > 1e-9) {
                followYawTab[i] = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
                if (firstValid < 0) firstValid = (int)i;
                lastValid = (int)i;
            }
        }
        if (firstValid < 0) { std::fill(followYawTab.begin(), followYawTab.end(), 0.0); return; }
        double held = closed ? followYawTab[(size_t)lastValid] : followYawTab[(size_t)firstValid];
        for (size_t i = 1; i < n; ++i) {
            if (std::isnan(followYawTab[i])) followYawTab[i] = held;
            else held = followYawTab[i];
        }
        followYawTab[0] = followYawTab[1];
    }
    // Single-lap arc fraction of a (non-Endless) parameter u, exactly as evaluate() maps it.
    double singleLapFrac(double u) const noexcept {
        double frac = std::clamp(hasTiming ? evaluateTimeMap(u) : std::clamp(u, 0.0, 1.0), 0.0, 1.0);
        if (closed) {
            const int effLaps = endless ? 1 : laps;
            const double phase = frac * effLaps;
            frac = phase - std::floor(phase);
        }
        return frac;
    }
    // Follower frame heading at a single-lap arc fraction (same segment choice as evaluateSingleLapArcFraction).
    double followYawAtSingleLap(double clamped) const noexcept {
        if (points.size() < 2 || followYawTab.size() != points.size()) return 0.0;
        double frac = std::clamp(clamped, 0.0, 1.0);
        if (closed) frac = frac - std::floor(frac);
        if (closed && frac == 0.0) return followYawTab[1];
        // lower_bound: at a vertex (and during a stop on it) the incoming segment's heading is held until the
        // target moves on; inside a segment it is the same segment as evaluateSingleLapArcFraction.
        const double distance = frac * length();
        auto it = std::lower_bound(lengths.begin(), lengths.end(), distance);
        const size_t i = std::clamp<size_t>((size_t)(it - lengths.begin()), 1, points.size() - 1);
        return followYawTab[i];
    }
    // Closest point of the drawn (target) polyline and the follower heading there.
    bool projectFollowBase(double& x, double& y, double& z, double& yawOut) const noexcept {
        if (points.size() < 2 || lengths.size() != points.size() || followYawTab.size() != points.size() || lengths.back() <= 1e-12) return false;
        double best = 1e30, fBest = 0; size_t jBest = 0;
        const double pp[3] = {x, y, z};
        for (size_t j = 1; j < points.size(); ++j) {
            double dot = 0, span = 0;
            for (int k = 0; k < 3; ++k) { const double d = points[j][(size_t)k] - points[j - 1][(size_t)k]; dot += (pp[k] - points[j - 1][(size_t)k]) * d; span += d * d; }
            const double f = span > 0 ? std::clamp(dot / span, 0., 1.) : 0;
            double e = 0;
            for (int k = 0; k < 3; ++k) { const double d = pp[k] - points[j - 1][(size_t)k] - f * (points[j][(size_t)k] - points[j - 1][(size_t)k]); e += d * d; }
            if (e < best) { best = e; jBest = j; fBest = f; }
        }
        if (!jBest) return false;
        const auto q = pathMix(points[jBest - 1], points[jBest], fBest);
        x = q[0]; y = q[1]; z = q[2];
        yawOut = followYawTab[jBest];
        return true;
    }
    std::vector<double> computeTimes(const MovementPath& p) const {
        const int n = (int)p.points.size();
        std::vector<double> out((size_t)n, 0.0);
        if (n < 2) return out;
        const int segments = n - (p.closed ? 0 : 1), steps = p.smooth ? 128 : 1;
        const double total = length();
        auto knotFrac = [&](int k) -> double {
            if (k <= 0) return 0.0;
            if (k >= segments) return 1.0;
            return total > 0.0 ? lengths[(size_t)k * (size_t)steps] / total : (double)k / (double)segments;
        };
        std::vector<int> keyKnotIndices;
        keyKnotIndices.push_back(0);
        for (int k = 1; k < n; ++k) {
            if (!p.closed && k == n - 1) break;
            if ((size_t)k < p.times.size() && p.times[(size_t)k].has_value()) keyKnotIndices.push_back(k);
        }
        keyKnotIndices.push_back(segments);
        size_t keyIdx = 0;
        for (int k = 0; k < n; ++k) {
            if (k == 0) { out[0] = 0.0; continue; }
            if (!p.closed && k == n - 1) { out[(size_t)k] = 100.0; continue; }
            if ((size_t)k < p.times.size() && p.times[(size_t)k].has_value()) {
                out[(size_t)k] = *p.times[(size_t)k];
                continue;
            }
            while (keyIdx + 1 < keyKnotIndices.size() && keyKnotIndices[keyIdx + 1] < k) ++keyIdx;
            const int ka = keyKnotIndices[keyIdx];
            const int kb = keyKnotIndices[keyIdx + 1];
            const auto& a = timeKeys[keyIdx];
            const auto& b = timeKeys[keyIdx + 1];
            const double df = b.f - a.f;
            double s = 0.0;
            if (df > 1e-12) s = invertPathEasing(a.easing, (knotFrac(k) - a.f) / df);
            else if (kb > ka) s = (double)(k - ka) / (double)(kb - ka);
            out[(size_t)k] = (a.t + s * (b.t - a.t)) * 100.0;
        }
        return out;
    }
    double length() const { return lengths.empty()?0:lengths.back(); }
    double evaluateTimeMap(double u) const noexcept {
        const double clamped = std::clamp(u, 0.0, 1.0);
        if (!hasTiming) return clamped;
        if (clamped <= startNorm) return 0.0;
        if (clamped >= endNorm) return 1.0;
        const double wSpan = endNorm - startNorm;
        const double w = wSpan > 0.0 ? std::clamp((clamped - startNorm) / wSpan, 0.0, 1.0) : 1.0;
        if (w <= 0.0) return 0.0;
        if (w >= 1.0) return 1.0;
        if (timeKeys.size() < 2) return w;
        auto it = std::upper_bound(timeKeys.begin(), timeKeys.end(), w,
            [](double val, const PathTimeKey& k) { return val < k.t; });
        const size_t hi = std::clamp<size_t>((size_t)(it - timeKeys.begin()), 1, timeKeys.size() - 1);
        const size_t lo = hi - 1;
        const double tSpan = timeKeys[hi].t - timeKeys[lo].t;
        const double s = tSpan > 0.0 ? (w - timeKeys[lo].t) / tSpan : 0.0;
        const double e = applyPathEasing(timeKeys[lo].easing, s);
        return timeKeys[lo].f + e * (timeKeys[hi].f - timeKeys[lo].f);
    }
    PathPoint evaluateSingleLapArcFraction(double clamped,double* yaw=nullptr) const noexcept {
        if(points.empty())return {};
        double frac=std::clamp(clamped,0.0,1.0);
        if(closed){frac=frac-std::floor(frac);}
        if(closed && frac==0.0){
            size_t direction=1;
            while(direction+1<points.size() && lengths[direction]-lengths[direction-1]<=1e-12)++direction;
            if(yaw && lengths[direction]-lengths[direction-1]>1e-12)*yaw=std::atan2(points[direction][1]-points[direction-1][1],points[direction][0]-points[direction-1][0])*180.0/3.14159265358979323846;
            return points.front();
        }
        const double distance=frac*length();
        auto it=std::upper_bound(lengths.begin(),lengths.end(),distance);
        const size_t i=std::clamp<size_t>((size_t)(it-lengths.begin()),1,points.size()-1);
        const double span=lengths[i]-lengths[i-1];
        if(yaw){
            size_t ti=i;
            if(lengths[ti]-lengths[ti-1]<=1e-12){
                while(ti>1 && lengths[ti]-lengths[ti-1]<=1e-12)--ti;
                if(lengths[ti]-lengths[ti-1]<=1e-12){
                    ti=i;
                    while(ti+1<points.size() && lengths[ti]-lengths[ti-1]<=1e-12)++ti;
                }
            }
            if(lengths[ti]-lengths[ti-1]>1e-12)*yaw=std::atan2(points[ti][1]-points[ti-1][1],points[ti][0]-points[ti-1][0])*180.0/3.14159265358979323846;
        }
        return pathMix(points[i-1],points[i],span>1e-12?(distance-lengths[i-1])/span:0);
    }
    PathPoint evaluateArcFraction(double clamped,double* yaw=nullptr) const noexcept {
        if(points.empty())return {};
        double frac=std::clamp(clamped,0.0,1.0);
        if(closed){
            const int effLaps = endless ? 1 : laps;
            const double phase=frac*effLaps;
            frac=phase-std::floor(phase);
        }
        return evaluateSingleLapArcFraction(frac, yaw);
    }
    double endlessEffectiveU(double uBase, double transportPhase) const noexcept {
        const double u0 = hasTiming ? evaluateTimeMap(uBase) : std::clamp(uBase, 0.0, 1.0);
        if (!closed || !endless) return u0;
        double u = u0 + transportPhase;
        u -= std::floor(u);
        if (u >= 1.0 || u < 0.0 || !std::isfinite(u)) u = 0.0;
        return u;
    }
    PathPoint evaluateEndless(double uBase, double transportPhase, double* yaw = nullptr) const noexcept {
        if (points.empty()) return {};
        const double uEff = endlessEffectiveU(uBase, transportPhase);
        return evaluateSingleLapArcFraction(uEff, yaw);
    }
    PathPoint evaluate(double u,double* yaw=nullptr) const noexcept {
        if(points.empty())return {};
        const double mapped=hasTiming?evaluateTimeMap(u):std::clamp(u,0.0,1.0);
        return evaluateArcFraction(mapped,yaw);
    }
    double evaluateAzimuth(double u, double fallback) const noexcept {
        if (!enabled || points.size() < 2 || orient || lookAt || azKeys.empty()) return fallback;
        if (const auto v = evaluateAzimuthKeys(azKeys, u)) return *v;
        return fallback;
    }
    bool projectOnPath(double& x,double& y,double& z,double* yawOut=nullptr) const noexcept {
        if(points.size()<2||lengths.size()!=points.size()||lengths.back()<=1e-12)return false;
        double best=1e30,fBest=0;size_t jBest=0;
        const double p[3]={x,y,z};
        for(size_t j=1;j<points.size();++j){
            double dot=0,span=0;
            for(int k=0;k<3;++k){const double d=points[j][(size_t)k]-points[j-1][(size_t)k];dot+=(p[k]-points[j-1][(size_t)k])*d;span+=d*d;}
            const double f=span>0?std::clamp(dot/span,0.,1.):0;
            double e=0;
            for(int k=0;k<3;++k){const double d=p[k]-points[j-1][(size_t)k]-f*(points[j][(size_t)k]-points[j-1][(size_t)k]);e+=d*d;}
            if(e<best){best=e;jBest=j;fBest=f;}
        }
        if(!jBest)return false;
        const auto q=pathMix(points[jBest-1],points[jBest],fBest);
        x=q[0];y=q[1];z=q[2];
        if(yawOut){
            size_t ti=jBest;
            if(lengths[ti]-lengths[ti-1]<=1e-12){
                while(ti>1&&lengths[ti]-lengths[ti-1]<=1e-12)--ti;
                if(lengths[ti]-lengths[ti-1]<=1e-12){ti=jBest;while(ti+1<points.size()&&lengths[ti]-lengths[ti-1]<=1e-12)++ti;}
            }
            if(lengths[ti]-lengths[ti-1]>1e-12)*yawOut=std::atan2(points[ti][1]-points[ti-1][1],points[ti][0]-points[ti-1][0])*180.0/3.14159265358979323846;
        }
        return true;
    }
};
// ---- E19 orientation: pure functions of current positions (no history, so a bake stays valid) ----
template <class P> inline OrientMode orientModeOf(const P& p) noexcept {
    return p.lookAt ? OrientMode::LookAt : p.orient ? OrientMode::AlongPath : OrientMode::Manual;
}
// Azimuth in degrees (0 = +x, counter-clockwise, same convention as path tangents and orientObject). False when the two points coincide in plan.
inline bool lookAtYaw(double fx, double fy, double tx, double ty, double& yawDeg) noexcept {
    const double dx = tx - fx, dy = ty - fy;
    if (!(std::hypot(dx, dy) >= 1e-3)) return false;  // also rejects NaN
    yawDeg = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
    return true;
}
// manual: the object's own azimuth value; tangent: path tangent yaw (0 when none); look: result of lookAtYaw when lookValid.
// Along path keeps the pre-E19 expression exactly (offset only added when non-zero). Look at = bearing to the target + the manual azimuth (the azimuth control acts as the offset, like Along path); it falls back to the
// manual value when the target is missing/coincident.
inline double orientedAzimuth(OrientMode m, double manual, double tangent, bool lookValid, double look, double offset) noexcept {
    switch (m) {
    case OrientMode::AlongPath: return std::remainder(offset != 0.0 ? manual + tangent + offset : manual + tangent, 360.0);
    case OrientMode::LookAt: return lookValid ? std::remainder(look + manual + offset, 360.0) : manual;
    case OrientMode::Manual: default: return manual;
    }
}
// Target position (plan xy) of a Look at object. False: target missing (index >= numSources and not the listener), the object itself,
// or the target coincides with the object -> the caller keeps the manual orientation.
template <class P> inline bool lookTargetXY(const P& path, int self, int numSources, const double (*cur)[4], double& tx, double& ty) noexcept {
    if (path.lookPoint) { tx = path.lookPos[0]; ty = path.lookPos[1]; return true; }
    const int t = path.lookTarget;
    if (t < 0 || t == self || t > kOrientMaxObject || (t >= numSources && t != kOrientMaxObject)) return false;
    tx = cur[t][0]; ty = cur[t][1];
    return true;
}
// Resolves the azimuth (cur[o][3]) of every object from current positions. cur[o] = {x,y,z,manual azimuth}; tangent/active describe the
// object's own path (active = enabled path with >= 2 points, tangent = its heading). pathAt(o) -> orientation fields of object o.
// Pure function of the positions: a table baked from the same function stays valid.
template <class PathAt> inline void applyOrientation(int numSources, double (*cur)[4], const double* tangent, const bool* active, PathAt&& pathAt) noexcept {
    for (int o = 0; o <= kOrientMaxObject; ++o) {
        if (o >= numSources && o != kOrientMaxObject) continue;
        const auto& p = pathAt(o);
        const OrientMode mode = orientModeOf(p);
        if (mode == OrientMode::AlongPath) {
            if (active[o]) cur[o][3] = orientedAzimuth(mode, cur[o][3], tangent[o], false, 0.0, p.orientOffset);
        } else if (mode == OrientMode::LookAt) {
            double tx = 0, ty = 0, look = 0;
            const bool ok = lookTargetXY(p, o, numSources, cur, tx, ty) && lookAtYaw(cur[o][0], cur[o][1], tx, ty, look);
            cur[o][3] = orientedAzimuth(mode, cur[o][3], 0.0, ok, look, p.orientOffset);
        }
    }
}
// SPSC triple mailbox. Producer owns back, consumer owns front; only middle is exchanged.
// Producer is serialised by the processor's control lock. No memory is freed by consume().
class PathMailbox {
    std::array<CompiledPath,3> slots;
    std::atomic<int> middle{1};
    int back=2,front=0;
public:
    void publish(const MovementPath& p) { slots[(size_t)back].build(p);back=middle.exchange(back|4)&3; }
    const CompiledPath& consume() noexcept { if(middle.load()&4)front=middle.exchange(front)&3;return slots[(size_t)front]; }
};
// ---- V04d: follower pose (pure) and Unlink conversion ----
// Drawn pose of a follower at Position u (no wander, no room walls): target pose + offset in the path frame, z clamped
// to 0..zTop. `c` is the follower's compiled (resolved) path. transportPhase is used when the target is Endless.
inline PathPoint followerPoseAt(const CompiledPath& c, double u, double transportPhase, double zTop, double* yawOut = nullptr) noexcept {
    if (c.points.size() < 2) return {};
    double frac = 0.0;
    PathPoint q;
    if (c.closed && c.endless) { frac = c.endlessEffectiveU(u, transportPhase); q = c.evaluateSingleLapArcFraction(frac); }
    else { frac = c.singleLapFrac(u); q = c.evaluate(u); }
    const double yaw = c.followYawAtSingleLap(frac);
    if (yawOut) *yawOut = yaw;
    double dx = 0, dy = 0, dz = 0;
    followOffsetDelta(yaw, c.followOffset, dx, dy, dz);
    return {q[0] + dx, q[1] + dy, std::clamp(std::clamp(q[2], 0.0, zTop) + dz, 0.0, zTop)};
}
// Unlink: the follower becomes an ordinary path = its offset curve over one lap of the target's drawn geometry,
// sampled uniformly in the target's arc fraction (samples points, smooth). Laps / Endless / period / window / LFO
// (and wander when followWander) are copied from the target; per-point key times and easings are not (the points
// differ); orientation, enable and azimuth keys stay the follower's own.
inline MovementPath followerToOwnPath(const MovementPath& f, const MovementPath& t, double zTop, int samples = 64) {
    MovementPath out;
    out.enabled = f.enabled;
    out.orient = f.orient; out.lookAt = f.lookAt; out.lookPoint = f.lookPoint; out.lookTarget = f.lookTarget; out.lookPos = f.lookPos;
    out.orientOffset = f.orientOffset;
    CompiledPath c;
    c.build(resolveFollowerPath(f, &t));
    if (c.points.size() < 2) return out;
    const int n = std::clamp(samples, 2, 64);
    for (int k = 0; k < n; ++k) {
        const double sfrac = t.closed ? (double)k / (double)n : (double)k / (double)(n - 1);
        const auto q = c.evaluateSingleLapArcFraction(sfrac);
        double dx = 0, dy = 0, dz = 0;
        followOffsetDelta(c.followYawAtSingleLap(sfrac), c.followOffset, dx, dy, dz);
        out.points.push_back({std::clamp(q[0] + dx, -200.0, 200.0), std::clamp(q[1] + dy, -200.0, 200.0),
                              std::clamp(std::clamp(q[2], 0.0, zTop) + dz, 0.0, std::min(30.0, zTop))});
    }
    out.closed = t.closed; out.smooth = true; out.laps = t.laps;
    out.endless = t.endless; out.endlessPeriodS = t.endlessPeriodS;
    out.start = t.start; out.end = t.end;
    out.lfo = t.lfo;
    if (f.follow.followWander) out.wander = t.wander;
    out.azKeys = f.azKeys;
    normalizePathAzKeys(out);
    return out;
}
}
