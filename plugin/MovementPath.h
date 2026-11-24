#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
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

struct MovementPath {
    std::vector<PathPoint> points;
    bool closed = false, smooth = false, orient = false, enabled = true;
    // E19 orientation: Manual (default), Along path (orient) or Look at. Offset is degrees, added to Along path / Look at.
    bool lookAt = false, lookPoint = false;
    int lookTarget = -1;             // object index (0..kOrientMaxObject-1 source, kOrientMaxObject listener); ignored when lookPoint
    PathPoint lookPos{0.0, 0.0, 0.0}; // fixed target when lookPoint
    double orientOffset = 0.0;
    int laps = 1;
    int group = -1;
    double start = 0.0, end = 100.0;
    std::vector<std::optional<double>> times;
    std::vector<PathEasing> easings;
    std::vector<AzimuthKey> azKeys;
    PathLfo lfo;
    bool hasCustomTiming() const noexcept {
        if (start != 0.0 || end != 100.0) return true;
        for (const auto& t : times) if (t.has_value()) return true;
        for (auto e : easings) if (e != PathEasing::Linear) return true;
        return false;
    }
    bool hasAzimuthKeys() const noexcept { return !azKeys.empty(); }
    bool operator==(const MovementPath& b) const {
        if (!(points == b.points && closed == b.closed && smooth == b.smooth && orient == b.orient && lookAt == b.lookAt && lookPoint == b.lookPoint && lookTarget == b.lookTarget && lookPos == b.lookPos && orientOffset == b.orientOffset && enabled == b.enabled && laps == b.laps && group == b.group && start == b.start && end == b.end && azKeys == b.azKeys && lfo == b.lfo))
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
    if (p.points.size() < 2) { p.azKeys.clear(); return; }
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

inline bool validPath(const MovementPath& p) {
    if (!validPathLfo(p.lfo)) return false;
    if (!std::isfinite(p.orientOffset) || std::abs(p.orientOffset) > 360.0) return false;
    if (p.lookTarget < -1 || p.lookTarget > kOrientMaxObject) return false;
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(p.lookPos[(size_t)k]) || p.lookPos[(size_t)k] < (k == 2 ? 0 : -200) || p.lookPos[(size_t)k] > (k == 2 ? 30 : 200)) return false;
    if (p.laps < 1 || p.laps > 64) return false;
    if (p.group < -1 || p.group > 31) return false;
    if (!std::isfinite(p.start) || !std::isfinite(p.end) || p.start < 0.0 || p.end > 100.0 || p.end - p.start < kMinPathWindowGap)
        return false;
    if (p.azKeys.size() > kMaxAzKeys) return false;
    if (p.points.empty()) return p.times.empty() && p.easings.empty() && p.azKeys.empty() && p.lfo.isDefault();
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
    double startNorm = 0.0, endNorm = 1.0;
    bool hasTiming = false;
    std::vector<PathTimeKey> timeKeys;
    std::vector<AzimuthKey> azKeys;
    PathLfo lfo{};
    bool hasAzimuthKeys() const noexcept { return !azKeys.empty(); }
    void build(const MovementPath& p) {
        points.clear(); lengths.clear(); timeKeys.clear(); azKeys.clear(); lfo = p.lfo;
        enabled=p.enabled && p.points.size()>=2;orient=p.orient;closed=p.closed;lookAt=p.lookAt;lookPoint=p.lookPoint;lookTarget=p.lookTarget;lookPos=p.lookPos;orientOffset=p.orientOffset;laps=std::clamp(p.laps,1,64);group=p.group;
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
    PathPoint evaluateArcFraction(double clamped,double* yaw=nullptr) const noexcept {
        if(points.empty())return {};
        double frac=std::clamp(clamped,0.0,1.0);
        if(closed){const double phase=frac*laps;frac=phase-std::floor(phase);}
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
}
