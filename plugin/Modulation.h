#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace verbp {

inline constexpr int kModMaxSources = 32;
inline constexpr int kModListenerObject = 32;
inline constexpr int kModMaxPoints = 64;
inline constexpr int kMaxModRoutes = 64;
inline constexpr int kMinCurveNodes = 2;
inline constexpr int kMaxCurveNodes = 8;
inline constexpr int kMaxWallSnapshotSegs = 128;
inline constexpr int kMaxModHostOuts = 16;
inline constexpr double kModHostOutNeutral = 0.5;
inline constexpr float kHostOutEpsilon = 1e-4f;
inline constexpr double kModPi = 3.14159265358979323846;

enum class ModDriver : uint8_t {
    DistanceToListener = 0,
    DistanceToPoint = 1,
    AzimuthToListener = 2,
    Elevation = 3,
    X = 4,
    Y = 5,
    Z = 6,
    Speed = 7,
    RadialSpeed = 8,
    PathPosition = 9,
    DistanceToNearestWall = 10
};

enum class ModTarget : uint8_t {
    SourceGain = 0,
    SourceLowPass = 1,
    SourceHighPass = 2,
    SourceLowShelf = 3,
    SourceHighShelf = 4,
    EarlyLevel = 5,
    TailLevel = 6,
    Mix = 7,
    None = 8
};

inline const char* modDriverName(ModDriver d) noexcept {
    switch (d) {
    case ModDriver::DistanceToListener: return "distanceToListener";
    case ModDriver::DistanceToPoint: return "distanceToPoint";
    case ModDriver::AzimuthToListener: return "azimuthToListener";
    case ModDriver::Elevation: return "elevation";
    case ModDriver::X: return "x";
    case ModDriver::Y: return "y";
    case ModDriver::Z: return "z";
    case ModDriver::Speed: return "speed";
    case ModDriver::RadialSpeed: return "radialSpeed";
    case ModDriver::PathPosition: return "pathPosition";
    case ModDriver::DistanceToNearestWall: return "distanceToNearestWall";
    default: return "distanceToListener";
    }
}

inline bool parseModDriver(std::string_view s, ModDriver& out) noexcept {
    if (s == "distanceToListener") { out = ModDriver::DistanceToListener; return true; }
    if (s == "distanceToPoint") { out = ModDriver::DistanceToPoint; return true; }
    if (s == "azimuthToListener") { out = ModDriver::AzimuthToListener; return true; }
    if (s == "elevation") { out = ModDriver::Elevation; return true; }
    if (s == "x") { out = ModDriver::X; return true; }
    if (s == "y") { out = ModDriver::Y; return true; }
    if (s == "z") { out = ModDriver::Z; return true; }
    if (s == "speed") { out = ModDriver::Speed; return true; }
    if (s == "radialSpeed") { out = ModDriver::RadialSpeed; return true; }
    if (s == "pathPosition") { out = ModDriver::PathPosition; return true; }
    if (s == "distanceToNearestWall") { out = ModDriver::DistanceToNearestWall; return true; }
    return false;
}

inline const char* modTargetName(ModTarget t) noexcept {
    switch (t) {
    case ModTarget::SourceGain: return "sourceGain";
    case ModTarget::SourceLowPass: return "sourceLowPass";
    case ModTarget::SourceHighPass: return "sourceHighPass";
    case ModTarget::SourceLowShelf: return "sourceLowShelf";
    case ModTarget::SourceHighShelf: return "sourceHighShelf";
    case ModTarget::EarlyLevel: return "earlyLevel";
    case ModTarget::TailLevel: return "tailLevel";
    case ModTarget::Mix: return "mix";
    case ModTarget::None: return "none";
    default: return "sourceGain";
    }
}

inline bool parseModTarget(std::string_view s, ModTarget& out) noexcept {
    if (s == "sourceGain") { out = ModTarget::SourceGain; return true; }
    if (s == "sourceLowPass") { out = ModTarget::SourceLowPass; return true; }
    if (s == "sourceHighPass") { out = ModTarget::SourceHighPass; return true; }
    if (s == "sourceLowShelf") { out = ModTarget::SourceLowShelf; return true; }
    if (s == "sourceHighShelf") { out = ModTarget::SourceHighShelf; return true; }
    if (s == "earlyLevel") { out = ModTarget::EarlyLevel; return true; }
    if (s == "tailLevel") { out = ModTarget::TailLevel; return true; }
    if (s == "mix") { out = ModTarget::Mix; return true; }
    if (s == "none") { out = ModTarget::None; return true; }
    return false;
}

inline bool modTargetIsGlobal(ModTarget t) noexcept {
    return t == ModTarget::EarlyLevel || t == ModTarget::TailLevel || t == ModTarget::Mix || t == ModTarget::None;
}

inline bool modTargetIsRoomGlobal(ModTarget t) noexcept {
    return t == ModTarget::EarlyLevel || t == ModTarget::TailLevel || t == ModTarget::Mix;
}

inline double modTargetMin(ModTarget t) noexcept {
    switch (t) {
    case ModTarget::SourceGain: return -60.0;
    case ModTarget::SourceLowPass: return 20.0;
    case ModTarget::SourceHighPass: return 20.0;
    case ModTarget::SourceLowShelf: return -12.0;
    case ModTarget::SourceHighShelf: return -12.0;
    case ModTarget::EarlyLevel: return -60.0;
    case ModTarget::TailLevel: return -48.0;
    case ModTarget::Mix: return -100.0;
    case ModTarget::None: return 0.0;
    default: return -60.0;
    }
}

inline double modTargetMax(ModTarget t) noexcept {
    switch (t) {
    case ModTarget::SourceGain: return 12.0;
    case ModTarget::SourceLowPass: return 20000.0;
    case ModTarget::SourceHighPass: return 20000.0;
    case ModTarget::SourceLowShelf: return 12.0;
    case ModTarget::SourceHighShelf: return 12.0;
    case ModTarget::EarlyLevel: return 6.0;
    case ModTarget::TailLevel: return 12.0;
    case ModTarget::Mix: return 100.0;
    case ModTarget::None: return 1.0;
    default: return 12.0;
    }
}

struct CurveNode {
    double x = 0.0;
    double y = 0.0;
    bool smooth = false;
    bool operator==(const CurveNode& b) const noexcept {
        return x == b.x && y == b.y && smooth == b.smooth;
    }
};

struct ModCurve {
    double inputMin = 0.0;
    double inputMax = 10.0;
    double outputMin = 0.0;
    double outputMax = 1.0;
    bool invert = false;
    bool clamp = true;
    int nodeCount = 2;
    std::array<CurveNode, kMaxCurveNodes> nodes{
        CurveNode{0.0, 0.0, false}, CurveNode{1.0, 1.0, false}
    };

    bool operator==(const ModCurve& b) const noexcept {
        if (inputMin != b.inputMin || inputMax != b.inputMax ||
            outputMin != b.outputMin || outputMax != b.outputMax ||
            invert != b.invert || clamp != b.clamp || nodeCount != b.nodeCount)
            return false;
        for (int i = 0; i < nodeCount && i < kMaxCurveNodes; ++i)
            if (!(nodes[(size_t)i] == b.nodes[(size_t)i])) return false;
        return true;
    }

    double evaluateNormalized(double raw) const noexcept {
        const double span = inputMax - inputMin;
        double t = std::abs(span) > 1e-12 ? (raw - inputMin) / span : 0.0;
        if (!std::isfinite(t)) t = 0.0;
        if (clamp) t = std::clamp(t, 0.0, 1.0);
        if (invert) t = 1.0 - t;

        const int cnt = std::clamp(nodeCount, kMinCurveNodes, kMaxCurveNodes);
        if (t <= nodes[0].x) {
            if (!clamp && cnt >= 2 && nodes[1].x > nodes[0].x + 1e-12) {
                const double slope = (nodes[1].y - nodes[0].y) / (nodes[1].x - nodes[0].x);
                return nodes[0].y + slope * (t - nodes[0].x);
            }
            return nodes[0].y;
        }
        const int last = cnt - 1;
        if (t >= nodes[(size_t)last].x) {
            if (!clamp && cnt >= 2 && nodes[(size_t)last].x > nodes[(size_t)(last - 1)].x + 1e-12) {
                const double slope = (nodes[(size_t)last].y - nodes[(size_t)(last - 1)].y) /
                                     (nodes[(size_t)last].x - nodes[(size_t)(last - 1)].x);
                return nodes[(size_t)last].y + slope * (t - nodes[(size_t)last].x);
            }
            return nodes[(size_t)last].y;
        }
        for (int i = 1; i < cnt; ++i) {
            if (t <= nodes[(size_t)i].x) {
                const auto& a = nodes[(size_t)(i - 1)];
                const auto& b = nodes[(size_t)i];
                const double dx = b.x - a.x;
                if (dx <= 1e-12) return b.y;
                const double s = std::clamp((t - a.x) / dx, 0.0, 1.0);
                const double shaped = a.smooth ? (s * s * (3.0 - 2.0 * s)) : s;
                return a.y + (b.y - a.y) * shaped;
            }
        }
        return nodes[(size_t)last].y;
    }

    double evaluate(double raw) const noexcept {
        const double u = evaluateNormalized(raw);
        double val = outputMin + u * (outputMax - outputMin);
        if (!std::isfinite(val)) val = outputMin;
        if (clamp) {
            const double lo = std::min(outputMin, outputMax);
            const double hi = std::max(outputMin, outputMax);
            val = std::clamp(val, lo, hi);
        }
        return val;
    }
};

struct ModulationRoute {
    bool enabled = true;
    int object = 0; // 0..31 source index, 32 = listener
    ModDriver driver = ModDriver::DistanceToListener;
    double pointX = 0.0;
    double pointY = 0.0;
    double pointZ = 0.0;
    ModCurve curve{};
    int targetObject = -1; // -1 = same as object (or global), 0..31 = explicit source index
    ModTarget target = ModTarget::SourceGain;
    int hostOut = 0; // 0 = off, 1..16 = Mod Out 1..16
    double attackMs = 15.0;
    double releaseMs = 80.0;

    bool operator==(const ModulationRoute& b) const noexcept {
        return enabled == b.enabled && object == b.object && driver == b.driver &&
               pointX == b.pointX && pointY == b.pointY && pointZ == b.pointZ &&
               curve == b.curve && targetObject == b.targetObject && target == b.target &&
               hostOut == b.hostOut && attackMs == b.attackMs && releaseMs == b.releaseMs;
    }
};

inline bool validModCurve(const ModCurve& c) noexcept {
    if (!std::isfinite(c.inputMin) || !std::isfinite(c.inputMax) ||
        !std::isfinite(c.outputMin) || !std::isfinite(c.outputMax))
        return false;
    if (std::abs(c.inputMin) > 1e6 || std::abs(c.inputMax) > 1e6 ||
        std::abs(c.outputMin) > 1e6 || std::abs(c.outputMax) > 1e6)
        return false;
    if (c.nodeCount < kMinCurveNodes || c.nodeCount > kMaxCurveNodes) return false;
    for (int i = 0; i < c.nodeCount; ++i) {
        const auto& nd = c.nodes[(size_t)i];
        if (!std::isfinite(nd.x) || !std::isfinite(nd.y)) return false;
        if (nd.x < 0.0 || nd.x > 1.0 || nd.y < 0.0 || nd.y > 1.0) return false;
        if (i > 0 && nd.x < c.nodes[(size_t)(i - 1)].x) return false;
    }
    return true;
}

inline bool validModRoute(const ModulationRoute& r) noexcept {
    if (r.object < 0 || r.object > kModListenerObject) return false;
    if ((uint8_t)r.driver > (uint8_t)ModDriver::DistanceToNearestWall) return false;
    if ((uint8_t)r.target > (uint8_t)ModTarget::None) return false;
    if (r.hostOut < 0 || r.hostOut > kMaxModHostOuts) return false;
    if (!std::isfinite(r.pointX) || !std::isfinite(r.pointY) || !std::isfinite(r.pointZ)) return false;
    if (r.pointX < -200.0 || r.pointX > 200.0 || r.pointY < -200.0 || r.pointY > 200.0 ||
        r.pointZ < 0.0 || r.pointZ > 30.0)
        return false;
    if (!std::isfinite(r.attackMs) || r.attackMs < 0.0 || r.attackMs > 2000.0) return false;
    if (!std::isfinite(r.releaseMs) || r.releaseMs < 0.0 || r.releaseMs > 5000.0) return false;
    if (modTargetIsGlobal(r.target)) {
        if (r.targetObject != -1) return false;
    } else {
        if (r.targetObject < -1 || r.targetObject >= kModMaxSources) return false;
        if (r.targetObject == -1 && r.object == kModListenerObject) return false;
    }
    return validModCurve(r.curve);
}

inline void sanitizeModRoute(ModulationRoute& r) noexcept {
    r.object = std::clamp(r.object, 0, kModListenerObject);
    if ((uint8_t)r.driver > (uint8_t)ModDriver::DistanceToNearestWall) r.driver = ModDriver::DistanceToListener;
    if ((uint8_t)r.target > (uint8_t)ModTarget::None) r.target = ModTarget::SourceGain;
    r.hostOut = std::clamp(r.hostOut, 0, kMaxModHostOuts);
    r.pointX = std::clamp(std::isfinite(r.pointX) ? r.pointX : 0.0, -200.0, 200.0);
    r.pointY = std::clamp(std::isfinite(r.pointY) ? r.pointY : 0.0, -200.0, 200.0);
    r.pointZ = std::clamp(std::isfinite(r.pointZ) ? r.pointZ : 0.0, 0.0, 30.0);
    r.attackMs = std::clamp(std::isfinite(r.attackMs) ? r.attackMs : 15.0, 0.0, 2000.0);
    r.releaseMs = std::clamp(std::isfinite(r.releaseMs) ? r.releaseMs : 80.0, 0.0, 5000.0);
    if (modTargetIsGlobal(r.target)) {
        r.targetObject = -1;
    } else {
        if (r.targetObject < -1 || r.targetObject >= kModMaxSources) r.targetObject = -1;
        if (r.targetObject == -1 && r.object == kModListenerObject) r.targetObject = 0;
    }
    r.curve.inputMin = std::clamp(std::isfinite(r.curve.inputMin) ? r.curve.inputMin : 0.0, -1e6, 1e6);
    r.curve.inputMax = std::clamp(std::isfinite(r.curve.inputMax) ? r.curve.inputMax : 10.0, -1e6, 1e6);
    const double tgtLo = modTargetMin(r.target);
    const double tgtHi = modTargetMax(r.target);
    r.curve.outputMin = std::clamp(std::isfinite(r.curve.outputMin) ? r.curve.outputMin : tgtLo, tgtLo, tgtHi);
    r.curve.outputMax = std::clamp(std::isfinite(r.curve.outputMax) ? r.curve.outputMax : tgtHi, tgtLo, tgtHi);
    r.curve.nodeCount = std::clamp(r.curve.nodeCount, kMinCurveNodes, kMaxCurveNodes);
    double prevX = 0.0;
    for (int i = 0; i < r.curve.nodeCount; ++i) {
        auto& nd = r.curve.nodes[(size_t)i];
        nd.x = std::clamp(std::isfinite(nd.x) ? nd.x : (double)i / (r.curve.nodeCount - 1), prevX, 1.0);
        nd.y = std::clamp(std::isfinite(nd.y) ? nd.y : nd.x, 0.0, 1.0);
        prevX = nd.x;
    }
}

struct ModRouteSet {
    std::array<ModulationRoute, kMaxModRoutes> routes{};
    int count = 0;
    uint64_t generation = 0;
};

class ModRoutesMailbox {
    std::array<ModRouteSet, 3> slots{};
    std::atomic<int> middle{1};
    std::atomic<uint64_t> publishedGen{0};
    int back = 2;
    int front = 0;
    uint64_t genCounter = 0;
public:
    void publish(const ModulationRoute* src, int cnt) noexcept {
        const int clamped = std::clamp(cnt, 0, kMaxModRoutes);
        auto& dst = slots[(size_t)back];
        dst.count = clamped;
        dst.generation = ++genCounter;
        for (int i = 0; i < clamped; ++i) dst.routes[(size_t)i] = src[i];
        back = middle.exchange(back | 4) & 3;
        publishedGen.store(dst.generation, std::memory_order_release);
    }
    const ModRouteSet& consume() noexcept {
        if (middle.load() & 4) front = middle.exchange(front) & 3;
        return slots[(size_t)front];
    }
    uint64_t publishedGeneration() const noexcept {
        return publishedGen.load(std::memory_order_acquire);
    }
};

struct WallSeg {
    double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
    double z0 = 0.0, z1 = 1e9;
};

struct WallSnapshot {
    std::array<WallSeg, kMaxWallSnapshotSegs> segs{};
    int count = 0;
};

class WallSnapshotMailbox {
    std::array<WallSnapshot, 3> slots{};
    std::atomic<int> middle{1};
    int back = 2;
    int front = 0;
public:
    void publish(const WallSnapshot& ws) noexcept {
        slots[(size_t)back] = ws;
        back = middle.exchange(back | 4) & 3;
    }
    const WallSnapshot& consume() noexcept {
        if (middle.load() & 4) front = middle.exchange(front) & 3;
        return slots[(size_t)front];
    }
};

inline double distanceToWallSnapshot(const WallSnapshot& ws, double px, double py, double pz) noexcept {
    if (ws.count <= 0) return 0.0;
    double best = 1e12;
    for (int i = 0; i < ws.count; ++i) {
        const auto& w = ws.segs[(size_t)i];
        const double vx = w.bx - w.ax;
        const double vy = w.by - w.ay;
        const double len2 = vx * vx + vy * vy;
        const double t = len2 > 1e-12 ? std::clamp(((px - w.ax) * vx + (py - w.ay) * vy) / len2, 0.0, 1.0) : 0.0;
        const double dxy = std::hypot(px - (w.ax + t * vx), py - (w.ay + t * vy));
        const double dz = pz < w.z0 ? (w.z0 - pz) : (pz > w.z1 ? (pz - w.z1) : 0.0);
        const double d = std::hypot(dxy, dz);
        if (d < best) best = d;
    }
    return best;
}

// 2nd-order double-precision RBJ TDF-II biquad with per-sample coefficient interpolation
// and wet crossfade bypass. When wet == 0 and inactive, buffer passes through bit-identically.
struct BiquadFilter {
    double z1 = 0.0, z2 = 0.0;
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double wet = 0.0;

    void reset() noexcept {
        z1 = 0.0; z2 = 0.0;
        b0 = 1.0; b1 = 0.0; b2 = 0.0; a1 = 0.0; a2 = 0.0;
        wet = 0.0;
    }

    static void makeLowPass(double sr, double freqHz, double& ob0, double& ob1, double& ob2, double& oa1, double& oa2) noexcept {
        const double f = std::clamp(freqHz, 20.0, std::min(20000.0, 0.48 * sr));
        const double w0 = 2.0 * kModPi * f / sr;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / 1.4142135623730950488;
        const double a0 = 1.0 + alpha;
        ob0 = ((1.0 - cw) * 0.5) / a0;
        ob1 = (1.0 - cw) / a0;
        ob2 = ob0;
        oa1 = (-2.0 * cw) / a0;
        oa2 = (1.0 - alpha) / a0;
    }

    static void makeHighPass(double sr, double freqHz, double& ob0, double& ob1, double& ob2, double& oa1, double& oa2) noexcept {
        const double f = std::clamp(freqHz, 20.0, std::min(20000.0, 0.48 * sr));
        const double w0 = 2.0 * kModPi * f / sr;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / 1.4142135623730950488;
        const double a0 = 1.0 + alpha;
        ob0 = ((1.0 + cw) * 0.5) / a0;
        ob1 = -(1.0 + cw) / a0;
        ob2 = ob0;
        oa1 = (-2.0 * cw) / a0;
        oa2 = (1.0 - alpha) / a0;
    }

    static void makeLowShelf(double sr, double freqHz, double gainDb, double& ob0, double& ob1, double& ob2, double& oa1, double& oa2) noexcept {
        const double g = std::clamp(gainDb, -12.0, 12.0);
        if (std::abs(g) < 1e-4) {
            ob0 = 1.0; ob1 = 0.0; ob2 = 0.0; oa1 = 0.0; oa2 = 0.0;
            return;
        }
        const double f = std::clamp(freqHz, 20.0, std::min(20000.0, 0.48 * sr));
        const double A = std::pow(10.0, g / 40.0);
        const double w0 = 2.0 * kModPi * f / sr;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw * 0.7071067811865475244;
        const double beta = 2.0 * std::sqrt(A) * alpha;
        const double a0 = (A + 1.0) + (A - 1.0) * cw + beta;
        ob0 = (A * ((A + 1.0) - (A - 1.0) * cw + beta)) / a0;
        ob1 = (2.0 * A * ((A - 1.0) - (A + 1.0) * cw)) / a0;
        ob2 = (A * ((A + 1.0) - (A - 1.0) * cw - beta)) / a0;
        oa1 = (-2.0 * ((A - 1.0) + (A + 1.0) * cw)) / a0;
        oa2 = ((A + 1.0) + (A - 1.0) * cw - beta) / a0;
    }

    static void makeHighShelf(double sr, double freqHz, double gainDb, double& ob0, double& ob1, double& ob2, double& oa1, double& oa2) noexcept {
        const double g = std::clamp(gainDb, -12.0, 12.0);
        if (std::abs(g) < 1e-4) {
            ob0 = 1.0; ob1 = 0.0; ob2 = 0.0; oa1 = 0.0; oa2 = 0.0;
            return;
        }
        const double f = std::clamp(freqHz, 20.0, std::min(20000.0, 0.48 * sr));
        const double A = std::pow(10.0, g / 40.0);
        const double w0 = 2.0 * kModPi * f / sr;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw * 0.7071067811865475244;
        const double beta = 2.0 * std::sqrt(A) * alpha;
        const double a0 = (A + 1.0) - (A - 1.0) * cw + beta;
        ob0 = (A * ((A + 1.0) + (A - 1.0) * cw + beta)) / a0;
        ob1 = (-2.0 * A * ((A - 1.0) + (A + 1.0) * cw)) / a0;
        ob2 = (A * ((A + 1.0) + (A - 1.0) * cw - beta)) / a0;
        oa1 = (2.0 * ((A - 1.0) - (A + 1.0) * cw)) / a0;
        oa2 = ((A + 1.0) - (A - 1.0) * cw - beta) / a0;
    }

    void process(float* buf, int len,
                 double tb0, double tb1, double tb2, double ta1, double ta2,
                 double targetWet, bool snap) noexcept {
        if (snap) {
            b0 = tb0; b1 = tb1; b2 = tb2; a1 = ta1; a2 = ta2;
            wet = targetWet;
            z1 = 0.0; z2 = 0.0;
        }
        if (wet <= 0.0 && targetWet <= 0.0) {
            z1 = 0.0; z2 = 0.0;
            b0 = tb0; b1 = tb1; b2 = tb2; a1 = ta1; a2 = ta2;
            wet = 0.0;
            return;
        }
        if (!snap && wet <= 0.0 && targetWet > 0.0) {
            // Turning on from full bypass: snap coefficients and zero state before wet fades up.
            b0 = tb0; b1 = tb1; b2 = tb2; a1 = ta1; a2 = ta2;
            z1 = 0.0; z2 = 0.0;
        }
        const double invLen = len > 0 ? 1.0 / (double)len : 1.0;
        const double db0 = (tb0 - b0) * invLen;
        const double db1 = (tb1 - b1) * invLen;
        const double db2 = (tb2 - b2) * invLen;
        const double da1 = (ta1 - a1) * invLen;
        const double da2 = (ta2 - a2) * invLen;
        const double dw = (targetWet - wet) * invLen;
        double cb0 = b0, cb1 = b1, cb2 = b2, ca1 = a1, ca2 = a2, cw = wet;
        double lz1 = z1, lz2 = z2;
        const bool unityWet = (wet >= 1.0 - 1e-12 && targetWet >= 1.0 - 1e-12);
        for (int i = 0; i < len; ++i) {
            cb0 += db0; cb1 += db1; cb2 += db2; ca1 += da1; ca2 += da2; cw += dw;
            const double x = (double)buf[i];
            const double y = cb0 * x + lz1;
            lz1 = cb1 * x - ca1 * y + lz2;
            lz2 = cb2 * x - ca2 * y;
            if (!std::isfinite(lz1) || !std::isfinite(lz2)) { lz1 = 0.0; lz2 = 0.0; }
            const double out = unityWet ? y : (x + cw * (y - x));
            buf[i] = (float)out;
        }
        b0 = tb0; b1 = tb1; b2 = tb2; a1 = ta1; a2 = ta2;
        wet = targetWet;
        if (targetWet <= 0.0) {
            z1 = 0.0; z2 = 0.0;
        } else {
            z1 = lz1; z2 = lz2;
        }
    }
};

struct SourceFilterChain {
    BiquadFilter lpf, hpf, lowShelf, highShelf;
    void reset() noexcept {
        lpf.reset(); hpf.reset(); lowShelf.reset(); highShelf.reset();
    }
    bool anyWet() const noexcept {
        return lpf.wet > 0.0 || hpf.wet > 0.0 || lowShelf.wet > 0.0 || highShelf.wet > 0.0;
    }
};

struct SourceModState {
    double gainDb = 0.0;
    double lpfHz = 20000.0;
    double hpfHz = 20.0;
    double lowShelfDb = 0.0;
    double highShelfDb = 0.0;
    bool hasGain = false;
    bool hasLpf = false;
    bool hasHpf = false;
    bool hasLowShelf = false;
    bool hasHighShelf = false;
};

struct GlobalModState {
    double earlyDb = 0.0;
    double tailDb = 0.0;
    double mixPct = 0.0;
    bool hasEarly = false;
    bool hasTail = false;
    bool hasMix = false;
};

class ModulationProcessor {
public:
    static constexpr double kLowShelfHz = 200.0;
    static constexpr double kHighShelfHz = 4000.0;
    static constexpr double kFilterFadeSec = 0.020;
    static constexpr double kSpeedSmoothSec = 0.015;

    double creativeBlend = 0.0;
    bool snapped = true;
    uint64_t lastGen = ~0ull;

    std::array<std::array<double, 3>, kModMaxSources + 1> prevPos{};
    std::array<double, kModMaxSources + 1> prevDistToListener{};
    std::array<double, kModMaxSources + 1> smoothedSpeed{};
    std::array<double, kModMaxSources + 1> smoothedRadialSpeed{};
    std::array<bool, kModMaxSources + 1> hasPrevPos{};

    std::array<double, kMaxModRoutes> routeState{};
    std::array<double, kMaxModRoutes> routeNormState{};
    std::array<bool, kMaxModRoutes> routeWasActive{};
    std::array<double, kMaxModHostOuts> hostSlotState{};
    std::array<bool, kMaxModHostOuts> hostSlotHadOwner{};
    std::array<bool, kMaxModHostOuts> hostSlotFading{};

    std::array<float, kModMaxSources> prevSegGain{};
    bool prevSegGainValid = false;

    std::array<SourceFilterChain, kModMaxPoints> pointFilters{};

    ModulationProcessor() noexcept { reset(); }

    void reset() noexcept {
        snapped = true;
        prevSegGainValid = false;
        hasPrevPos.fill(false);
        smoothedSpeed.fill(0.0);
        smoothedRadialSpeed.fill(0.0);
        routeState.fill(0.0);
        routeNormState.fill(kModHostOutNeutral);
        routeWasActive.fill(false);
        hostSlotState.fill(kModHostOutNeutral);
        hostSlotHadOwner.fill(false);
        hostSlotFading.fill(false);
        for (auto& pf : pointFilters) pf.reset();
    }

    bool hasAnyWetFilter() const noexcept {
        for (const auto& pf : pointFilters)
            if (pf.anyWet()) return true;
        return false;
    }

    bool hasPendingHostFade() const noexcept {
        for (int s = 0; s < kMaxModHostOuts; ++s) {
            if (hostSlotHadOwner[(size_t)s] || hostSlotFading[(size_t)s] ||
                std::abs(hostSlotState[(size_t)s] - kModHostOutNeutral) > 1e-6)
                return true;
        }
        return false;
    }

    double currentCreativeBlend() const noexcept { return creativeBlend; }

    double advanceCreativeBlend(bool creative, int segSamples, double sampleRate) noexcept {
        const double target = creative ? 1.0 : 0.0;
        if (snapped) {
            creativeBlend = target;
            return creativeBlend;
        }
        const double dt = (double)std::max(1, segSamples) / std::max(8000.0, sampleRate);
        const double alpha = 1.0 - std::exp(-dt / kFilterFadeSec);
        creativeBlend += (target - creativeBlend) * alpha;
        if (std::abs(target - creativeBlend) < 1e-6) creativeBlend = target;
        return creativeBlend;
    }

    static double neutralRouteState(ModTarget t) noexcept {
        (void)t;
        return 0.0; // Gain/shelf/early/tail/mix deltas and cutoff log2-octaves all have neutral = 0.0
    }

    static double targetToFilterSmootherSpace(ModTarget t, double curveVal) noexcept {
        if (t == ModTarget::SourceLowPass) {
            const double f = std::clamp(curveVal, 20.0, 20000.0);
            return std::log2(f / 20000.0);
        }
        if (t == ModTarget::SourceHighPass) {
            const double f = std::clamp(curveVal, 20.0, 20000.0);
            return std::log2(f / 20.0);
        }
        return curveVal;
    }

    static double filterSmootherSpaceToTarget(ModTarget t, double smootherVal) noexcept {
        if (t == ModTarget::SourceLowPass)
            return std::clamp(20000.0 * std::pow(2.0, std::clamp(smootherVal, -15.0, 15.0)), 20.0, 20000.0);
        if (t == ModTarget::SourceHighPass)
            return std::clamp(20.0 * std::pow(2.0, std::clamp(smootherVal, -15.0, 15.0)), 20.0, 20000.0);
        return smootherVal;
    }

    void evaluateSegment(
        const ModRouteSet& set,
        int numSources,
        const double (*poses)[4],
        const double* uPath,
        const bool* objJumped,
        const WallSnapshot& walls,
        bool creative,
        double blend,
        int segSamples,
        double sampleRate,
        std::array<SourceModState, kModMaxSources>& outSources,
        GlobalModState& outGlobal,
        float* outRawDrivers = nullptr,
        float* outTargetValues = nullptr,
        uint8_t* outActive = nullptr,
        double* outHostSlotNorm = nullptr) noexcept
    {
        for (int s = 0; s < kModMaxSources; ++s) outSources[(size_t)s] = SourceModState{};
        outGlobal = GlobalModState{};

        const double dt = (double)std::max(1, segSamples) / std::max(8000.0, sampleRate);
        const bool genChanged = (set.generation != lastGen);
        if (genChanged) lastGen = set.generation;

        const double lx = poses[kModListenerObject][0];
        const double ly = poses[kModListenerObject][1];
        const double lz = poses[kModListenerObject][2];
        const double lyaw = poses[kModListenerObject][3];

        // Update velocity trackers for active objects.
        const double speedAlpha = 1.0 - std::exp(-dt / kSpeedSmoothSec);
        for (int o = 0; o <= kModListenerObject; ++o) {
            if (o >= numSources && o != kModListenerObject) continue;
            const double ox = poses[o][0], oy = poses[o][1], oz = poses[o][2];
            const double distL = std::hypot(std::hypot(ox - lx, oy - ly), oz - lz);
            const bool jump = snapped || !hasPrevPos[(size_t)o] || (objJumped && objJumped[o]);
            if (jump) {
                smoothedSpeed[(size_t)o] = 0.0;
                smoothedRadialSpeed[(size_t)o] = 0.0;
                hasPrevPos[(size_t)o] = true;
            } else {
                const double dx = ox - prevPos[(size_t)o][0];
                const double dy = oy - prevPos[(size_t)o][1];
                const double dz = oz - prevPos[(size_t)o][2];
                const double instSpeed = std::hypot(std::hypot(dx, dy), dz) / dt;
                const double instRad = (distL - prevDistToListener[(size_t)o]) / dt;
                smoothedSpeed[(size_t)o] += (instSpeed - smoothedSpeed[(size_t)o]) * speedAlpha;
                smoothedRadialSpeed[(size_t)o] += (instRad - smoothedRadialSpeed[(size_t)o]) * speedAlpha;
            }
            prevPos[(size_t)o][0] = ox;
            prevPos[(size_t)o][1] = oy;
            prevPos[(size_t)o][2] = oz;
            prevDistToListener[(size_t)o] = distL;
        }

        auto driverValue = [&](const ModulationRoute& r) noexcept -> double {
            const int o = r.object;
            if (o < 0 || (o >= numSources && o != kModListenerObject)) return 0.0;
            const double ox = poses[o][0], oy = poses[o][1], oz = poses[o][2];
            switch (r.driver) {
            case ModDriver::DistanceToListener:
                return std::hypot(std::hypot(ox - lx, oy - ly), oz - lz);
            case ModDriver::DistanceToPoint:
                return std::hypot(std::hypot(ox - r.pointX, oy - r.pointY), oz - r.pointZ);
            case ModDriver::AzimuthToListener: {
                if (o == kModListenerObject) return 0.0;
                const double dx = ox - lx, dy = oy - ly;
                if (std::hypot(dx, dy) < 1e-6) return 0.0;
                const double bearing = std::atan2(dy, dx) * 180.0 / kModPi;
                const double rel = std::remainder(bearing - lyaw, 360.0);
                return rel == 0.0 ? 0.0 : rel;
            }
            case ModDriver::Elevation: {
                if (o == kModListenerObject) return 0.0;
                const double dxy = std::hypot(ox - lx, oy - ly);
                const double dz = oz - lz;
                if (dxy < 1e-6 && std::abs(dz) < 1e-6) return 0.0;
                return std::atan2(dz, dxy) * 180.0 / kModPi;
            }
            case ModDriver::X: return ox;
            case ModDriver::Y: return oy;
            case ModDriver::Z: return oz;
            case ModDriver::Speed: return smoothedSpeed[(size_t)o];
            case ModDriver::RadialSpeed: return (o == kModListenerObject) ? 0.0 : smoothedRadialSpeed[(size_t)o];
            case ModDriver::PathPosition: return std::clamp(uPath ? uPath[o] : 0.0, 0.0, 1.0);
            case ModDriver::DistanceToNearestWall: return distanceToWallSnapshot(walls, ox, oy, oz);
            default: return 0.0;
            }
        };

        std::array<double, kModMaxSources> srcLpfOct{};
        std::array<double, kModMaxSources> srcHpfOct{};
        std::array<bool, kMaxModHostOuts> slotHasOwner{};
        std::array<double, kMaxModHostOuts> slotWinnerVal{};

        for (int idx = 0; idx < set.count && idx < kMaxModRoutes; ++idx) {
            const auto& r = set.routes[(size_t)idx];
            const bool srcValid = (r.object >= 0 && (r.object < numSources || r.object == kModListenerObject));
            const int tgtSrc = modTargetIsGlobal(r.target)
                ? -1
                : (r.targetObject >= 0 ? r.targetObject : (r.object < numSources ? r.object : -1));
            const bool tgtValid = modTargetIsGlobal(r.target) || (tgtSrc >= 0 && tgtSrc < numSources);
            const bool active = r.enabled && srcValid && tgtValid && blend > 1e-9;

            double raw = 0.0;
            double goal = 0.0;
            double normRaw = kModHostOutNeutral;
            if (r.enabled && srcValid && tgtValid && creative) {
                raw = driverValue(r);
                normRaw = std::clamp(r.curve.evaluateNormalized(raw), 0.0, 1.0);
                if (r.target != ModTarget::None) {
                    const double val = r.curve.evaluate(raw);
                    goal = targetToFilterSmootherSpace(r.target, val);
                }
            }
            const double normGoal = (r.enabled && srcValid && tgtValid && creative) ? normRaw : kModHostOutNeutral;

            if (snapped) {
                routeState[(size_t)idx] = active ? goal : 0.0;
                routeNormState[(size_t)idx] = (active && creative) ? normGoal : kModHostOutNeutral;
            } else {
                const double tauMs = (goal > routeState[(size_t)idx]) ? r.attackMs : r.releaseMs;
                if (tauMs <= 1e-4) {
                    routeState[(size_t)idx] = goal;
                } else {
                    const double alpha = 1.0 - std::exp(-dt / (tauMs * 0.001));
                    routeState[(size_t)idx] += (goal - routeState[(size_t)idx]) * alpha;
                }
                if (r.enabled && srcValid && tgtValid && !creative && blend > 1e-9) {
                    // Hold routeNormState during Creative->Physical fade so blend smoothly ramps host output to neutral 0.5 even when releaseMs == 0.
                } else if (!creative || !(r.enabled && srcValid && tgtValid)) {
                    routeNormState[(size_t)idx] = kModHostOutNeutral;
                } else {
                    const double normTauMs = (normGoal >= routeNormState[(size_t)idx]) ? r.attackMs : r.releaseMs;
                    if (normTauMs <= 1e-4) {
                        routeNormState[(size_t)idx] = normGoal;
                    } else {
                        const double normAlpha = 1.0 - std::exp(-dt / (normTauMs * 0.001));
                        routeNormState[(size_t)idx] += (normGoal - routeNormState[(size_t)idx]) * normAlpha;
                        if (std::abs(normGoal - routeNormState[(size_t)idx]) < 1e-7)
                            routeNormState[(size_t)idx] = normGoal;
                    }
                }
            }
            routeWasActive[(size_t)idx] = active;

            const double hostNormVal = std::clamp(
                kModHostOutNeutral + (routeNormState[(size_t)idx] - kModHostOutNeutral) * blend,
                0.0, 1.0);
            if (active && r.hostOut >= 1 && r.hostOut <= kMaxModHostOuts) {
                const int slot = r.hostOut - 1;
                slotHasOwner[(size_t)slot] = true;
                slotWinnerVal[(size_t)slot] = hostNormVal; // last enabled route in list order wins
            }

            const double val = active ? (routeState[(size_t)idx] * blend) : 0.0;
            if (outRawDrivers) outRawDrivers[idx] = (float)raw;
            if (outTargetValues) {
                if (r.target == ModTarget::None)
                    outTargetValues[idx] = (float)(active ? hostNormVal : 0.0);
                else
                    outTargetValues[idx] = (float)filterSmootherSpaceToTarget(r.target, active ? val : 0.0);
            }
            if (outActive) outActive[idx] = (active && creative) ? 1 : 0;

            if (!r.enabled || !srcValid || !tgtValid || blend <= 1e-9 || r.target == ModTarget::None) continue;

            switch (r.target) {
            case ModTarget::SourceGain:
                outSources[(size_t)tgtSrc].gainDb += val;
                outSources[(size_t)tgtSrc].hasGain = true;
                break;
            case ModTarget::SourceLowPass:
                srcLpfOct[(size_t)tgtSrc] += val;
                outSources[(size_t)tgtSrc].hasLpf = true;
                break;
            case ModTarget::SourceHighPass:
                srcHpfOct[(size_t)tgtSrc] += val;
                outSources[(size_t)tgtSrc].hasHpf = true;
                break;
            case ModTarget::SourceLowShelf:
                outSources[(size_t)tgtSrc].lowShelfDb += val;
                outSources[(size_t)tgtSrc].hasLowShelf = true;
                break;
            case ModTarget::SourceHighShelf:
                outSources[(size_t)tgtSrc].highShelfDb += val;
                outSources[(size_t)tgtSrc].hasHighShelf = true;
                break;
            case ModTarget::EarlyLevel:
                outGlobal.earlyDb += val;
                outGlobal.hasEarly = true;
                break;
            case ModTarget::TailLevel:
                outGlobal.tailDb += val;
                outGlobal.hasTail = true;
                break;
            case ModTarget::Mix:
                outGlobal.mixPct += val;
                outGlobal.hasMix = true;
                break;
            case ModTarget::None:
                break;
            }
        }
        for (int idx = set.count; idx < kMaxModRoutes; ++idx) {
            routeState[(size_t)idx] = 0.0;
            routeNormState[(size_t)idx] = kModHostOutNeutral;
            routeWasActive[(size_t)idx] = false;
            if (outRawDrivers) outRawDrivers[idx] = 0.f;
            if (outTargetValues) outTargetValues[idx] = 0.f;
            if (outActive) outActive[idx] = 0;
        }
        const double hostFadeAlpha = snapped ? 1.0 : (1.0 - std::exp(-dt / kFilterFadeSec));
        for (int s = 0; s < kMaxModHostOuts; ++s) {
            if (snapped) {
                hostSlotState[(size_t)s] = slotHasOwner[(size_t)s] ? slotWinnerVal[(size_t)s] : kModHostOutNeutral;
                hostSlotFading[(size_t)s] = false;
            } else if (slotHasOwner[(size_t)s]) {
                hostSlotState[(size_t)s] = slotWinnerVal[(size_t)s];
                hostSlotFading[(size_t)s] = false;
            } else {
                if (hostSlotHadOwner[(size_t)s] && std::abs(hostSlotState[(size_t)s] - kModHostOutNeutral) > 1e-6)
                    hostSlotFading[(size_t)s] = true;
                if (hostSlotFading[(size_t)s]) {
                    hostSlotState[(size_t)s] += (kModHostOutNeutral - hostSlotState[(size_t)s]) * hostFadeAlpha;
                    if (std::abs(hostSlotState[(size_t)s] - kModHostOutNeutral) < 1e-6) {
                        hostSlotState[(size_t)s] = kModHostOutNeutral;
                        hostSlotFading[(size_t)s] = false;
                    }
                } else {
                    hostSlotState[(size_t)s] = kModHostOutNeutral;
                }
            }
            hostSlotHadOwner[(size_t)s] = slotHasOwner[(size_t)s];
            if (outHostSlotNorm)
                outHostSlotNorm[s] = std::clamp(hostSlotState[(size_t)s], 0.0, 1.0);
        }

        for (int s = 0; s < numSources && s < kModMaxSources; ++s) {
            auto& st = outSources[(size_t)s];
            if (st.hasGain) st.gainDb = std::clamp(st.gainDb, -60.0, 12.0);
            if (st.hasLpf) st.lpfHz = std::clamp(20000.0 * std::pow(2.0, std::clamp(srcLpfOct[(size_t)s], -15.0, 15.0)), 20.0, 20000.0);
            if (st.hasHpf) st.hpfHz = std::clamp(20.0 * std::pow(2.0, std::clamp(srcHpfOct[(size_t)s], -15.0, 15.0)), 20.0, 20000.0);
            if (st.hasLowShelf) st.lowShelfDb = std::clamp(st.lowShelfDb, -12.0, 12.0);
            if (st.hasHighShelf) st.highShelfDb = std::clamp(st.highShelfDb, -12.0, 12.0);
        }
    }

    void processPointFilters(
        int pointIdx,
        float* buf,
        int len,
        double sampleRate,
        const SourceModState& st,
        bool creative,
        bool snapThisSegment) noexcept
    {
        if (pointIdx < 0 || pointIdx >= kModMaxPoints || len <= 0) return;
        auto& pf = pointFilters[(size_t)pointIdx];
        const double step = std::clamp((double)len / (kFilterFadeSec * std::max(8000.0, sampleRate)), 0.0, 1.0);
        auto nextWet = [&](bool active, double prevW) noexcept -> double {
            const double goal = active ? 1.0 : 0.0;
            if (snapThisSegment) return goal;
            if (goal > prevW) return std::min(1.0, prevW + step);
            if (goal < prevW) return std::max(0.0, prevW - step);
            return prevW;
        };

        const bool useLpf = creative && st.hasLpf;
        const bool useHpf = creative && st.hasHpf;
        const bool useLs = creative && st.hasLowShelf;
        const bool useHs = creative && st.hasHighShelf;

        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

        const double wLpf = nextWet(useLpf, pf.lpf.wet);
        if (useLpf || pf.lpf.wet > 0.0) {
            if (useLpf) BiquadFilter::makeLowPass(sampleRate, st.lpfHz, b0, b1, b2, a1, a2);
            else { b0 = pf.lpf.b0; b1 = pf.lpf.b1; b2 = pf.lpf.b2; a1 = pf.lpf.a1; a2 = pf.lpf.a2; }
            pf.lpf.process(buf, len, b0, b1, b2, a1, a2, wLpf, snapThisSegment);
        }

        const double wHpf = nextWet(useHpf, pf.hpf.wet);
        if (useHpf || pf.hpf.wet > 0.0) {
            if (useHpf) BiquadFilter::makeHighPass(sampleRate, st.hpfHz, b0, b1, b2, a1, a2);
            else { b0 = pf.hpf.b0; b1 = pf.hpf.b1; b2 = pf.hpf.b2; a1 = pf.hpf.a1; a2 = pf.hpf.a2; }
            pf.hpf.process(buf, len, b0, b1, b2, a1, a2, wHpf, snapThisSegment);
        }

        const double wLs = nextWet(useLs, pf.lowShelf.wet);
        if (useLs || pf.lowShelf.wet > 0.0) {
            if (useLs) BiquadFilter::makeLowShelf(sampleRate, kLowShelfHz, st.lowShelfDb, b0, b1, b2, a1, a2);
            else { b0 = pf.lowShelf.b0; b1 = pf.lowShelf.b1; b2 = pf.lowShelf.b2; a1 = pf.lowShelf.a1; a2 = pf.lowShelf.a2; }
            pf.lowShelf.process(buf, len, b0, b1, b2, a1, a2, wLs, snapThisSegment);
        }

        const double wHs = nextWet(useHs, pf.highShelf.wet);
        if (useHs || pf.highShelf.wet > 0.0) {
            if (useHs) BiquadFilter::makeHighShelf(sampleRate, kHighShelfHz, st.highShelfDb, b0, b1, b2, a1, a2);
            else { b0 = pf.highShelf.b0; b1 = pf.highShelf.b1; b2 = pf.highShelf.b2; a1 = pf.highShelf.a1; a2 = pf.highShelf.a2; }
            pf.highShelf.process(buf, len, b0, b1, b2, a1, a2, wHs, snapThisSegment);
        }
    }
};

inline void remapModRoutesOnSourceRemove(std::vector<ModulationRoute>& routes, int removedIndex) {
    if (removedIndex < 0 || removedIndex >= kModMaxSources) return;
    std::vector<ModulationRoute> kept;
    kept.reserve(routes.size());
    for (auto r : routes) {
        if (r.object == removedIndex || r.targetObject == removedIndex) continue;
        if (r.object > removedIndex && r.object < kModListenerObject) --r.object;
        if (r.targetObject > removedIndex && r.targetObject < kModListenerObject) --r.targetObject;
        if (validModRoute(r)) kept.push_back(r);
    }
    routes = std::move(kept);
}

} // namespace verbp
