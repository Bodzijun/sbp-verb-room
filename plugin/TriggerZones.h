// V04c: trigger zones -- named areas of the room (box or polygon in plan, between zMin and zMax) whose occupancy by a
// moving object (source or listener) drives the modulation matrix ("In zone", "Zone index", "Distance to zone").
// Zones are document state (scene), NOT APVTS parameters and NOT acoustics: they never touch the engine.
//
// Threads:
//   * TriggerZone (editable, std::string / std::vector) lives on the message thread (processor + command model).
//   * TriggerZoneSet is a fixed-size POD snapshot published through TriggerZoneMailbox (lock-free triple buffer).
//   * TriggerZoneTracker runs on the audio thread once per control period (<= 128 samples) on the EFFECTIVE poses:
//     zero allocation, no locks.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace verbp {

inline constexpr int kMaxTriggerZones = 16;
inline constexpr int kMaxTriggerZoneVerts = 32;
inline constexpr int kTriggerZoneObjects = 33;        // 0..31 sources, 32 listener
inline constexpr int kMaxTriggerZoneId = 9999;
inline constexpr double kTriggerZoneDefaultHyst = 0.05;
inline constexpr double kTriggerZoneMaxEdge = 10.0;
inline constexpr double kTriggerZoneMaxHyst = 2.0;
inline constexpr double kTriggerZoneFar = 1000.0;     // signed distance reported for a missing / disabled zone

// ------------------------------------------------------------------------------------------------ editable model
struct TriggerZone {
    int id = 1;                                        // stable, 1..9999, unique in the list
    std::string name;
    bool polygon = false;                              // false: box (two opposite corners), true: polygon (>= 3 vertices)
    std::vector<std::pair<double, double>> points;     // plan coordinates in metres
    double zMin = 0.0, zMax = 3.0;                     // metres
    double edge = 0.0;                                 // soft-edge width in metres, 0 = hard trigger
    double hysteresis = kTriggerZoneDefaultHyst;       // metres (hard trigger only)
    std::string colour;                                // optional "#rrggbb", empty = default
    bool enabled = true;
    bool visible = true;                               // view only

    bool operator==(const TriggerZone& b) const noexcept {
        return id == b.id && name == b.name && polygon == b.polygon && points == b.points && zMin == b.zMin &&
               zMax == b.zMax && edge == b.edge && hysteresis == b.hysteresis && colour == b.colour &&
               enabled == b.enabled && visible == b.visible;
    }
    bool operator!=(const TriggerZone& b) const noexcept { return !(*this == b); }

    // Polygon vertices in plan order (a box expands to its 4 corners).
    std::vector<std::pair<double, double>> outline() const {
        if (!polygon && points.size() == 2) {
            const double x0 = std::min(points[0].first, points[1].first), x1 = std::max(points[0].first, points[1].first);
            const double y0 = std::min(points[0].second, points[1].second), y1 = std::max(points[0].second, points[1].second);
            return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        }
        return points;
    }
};

inline bool triggerZoneSegmentsCross(double ax, double ay, double bx, double by,
                                     double cx, double cy, double dx, double dy) noexcept {
    auto orient = [](double px, double py, double qx, double qy, double rx, double ry) noexcept {
        const double v = (qx - px) * (ry - py) - (qy - py) * (rx - px);
        return v > 1e-12 ? 1 : (v < -1e-12 ? -1 : 0);
    };
    auto onSeg = [](double px, double py, double qx, double qy, double rx, double ry) noexcept {
        return std::min(px, qx) - 1e-12 <= rx && rx <= std::max(px, qx) + 1e-12 &&
               std::min(py, qy) - 1e-12 <= ry && ry <= std::max(py, qy) + 1e-12;
    };
    const int o1 = orient(ax, ay, bx, by, cx, cy), o2 = orient(ax, ay, bx, by, dx, dy);
    const int o3 = orient(cx, cy, dx, dy, ax, ay), o4 = orient(cx, cy, dx, dy, bx, by);
    if (o1 != o2 && o3 != o4) return true;
    if (o1 == 0 && onSeg(ax, ay, bx, by, cx, cy)) return true;
    if (o2 == 0 && onSeg(ax, ay, bx, by, dx, dy)) return true;
    if (o3 == 0 && onSeg(cx, cy, dx, dy, ax, ay)) return true;
    if (o4 == 0 && onSeg(cx, cy, dx, dy, bx, by)) return true;
    return false;
}

// Simple polygon: >= 3 vertices, finite, non-zero area, no two non-adjacent edges touch, no repeated vertex.
inline bool triggerZonePolygonSimple(const std::vector<std::pair<double, double>>& v) noexcept {
    const int n = (int)v.size();
    if (n < 3) return false;
    double area2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const auto& a = v[(size_t)i];
        const auto& b = v[(size_t)((i + 1) % n)];
        if (!std::isfinite(a.first) || !std::isfinite(a.second)) return false;
        if (std::hypot(b.first - a.first, b.second - a.second) < 1e-6) return false;
        area2 += a.first * b.second - b.first * a.second;
    }
    if (std::abs(area2) < 1e-6) return false;
    for (int i = 0; i < n; ++i) {
        const auto& a = v[(size_t)i];
        const auto& b = v[(size_t)((i + 1) % n)];
        for (int j = i + 1; j < n; ++j) {
            if (j == i || (j + 1) % n == i || (i + 1) % n == j) continue;  // adjacent edges share a vertex
            const auto& c = v[(size_t)j];
            const auto& d = v[(size_t)((j + 1) % n)];
            if (triggerZoneSegmentsCross(a.first, a.second, b.first, b.second, c.first, c.second, d.first, d.second))
                return false;
        }
    }
    return true;
}

inline bool triggerZoneColourValid(const std::string& c) noexcept {
    if (c.empty()) return true;
    if (c.size() != 7 || c[0] != '#') return false;
    for (size_t i = 1; i < 7; ++i) {
        const char ch = c[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
    }
    return true;
}

// Room-independent validity (the command layer additionally clamps to the room). Polygons must be simple.
inline bool validTriggerZone(const TriggerZone& z) noexcept {
    if (z.id < 1 || z.id > kMaxTriggerZoneId) return false;
    if (z.name.size() > 64) return false;
    if (!std::isfinite(z.zMin) || !std::isfinite(z.zMax) || z.zMin < 0.0 || z.zMax > 30.0 || z.zMax - z.zMin < 0.01) return false;
    if (!std::isfinite(z.edge) || z.edge < 0.0 || z.edge > kTriggerZoneMaxEdge) return false;
    if (!std::isfinite(z.hysteresis) || z.hysteresis < 0.0 || z.hysteresis > kTriggerZoneMaxHyst) return false;
    if (!triggerZoneColourValid(z.colour)) return false;
    for (const auto& pt : z.points)
        if (!std::isfinite(pt.first) || !std::isfinite(pt.second) || std::abs(pt.first) > 200.0 || std::abs(pt.second) > 200.0) return false;
    if (!z.polygon) {
        if (z.points.size() != 2) return false;
        return std::abs(z.points[0].first - z.points[1].first) >= 0.01 && std::abs(z.points[0].second - z.points[1].second) >= 0.01;
    }
    if (z.points.size() < 3 || (int)z.points.size() > kMaxTriggerZoneVerts) return false;
    return triggerZonePolygonSimple(z.points);
}

// ------------------------------------------------------------------------------------------------ realtime snapshot
struct TriggerZoneSnap {
    int id = 0;
    bool enabled = true;
    int vertCount = 0;
    std::array<double, kMaxTriggerZoneVerts> vx{};
    std::array<double, kMaxTriggerZoneVerts> vy{};
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
    double zMin = 0.0, zMax = 3.0;
    double edge = 0.0;
    double hysteresis = kTriggerZoneDefaultHyst;
};

struct TriggerZoneSet {
    std::array<TriggerZoneSnap, kMaxTriggerZones> zones{};
    int count = 0;
    uint64_t generation = 0;

    int slotForId(int id) const noexcept {
        if (id <= 0) return -1;
        for (int i = 0; i < count && i < kMaxTriggerZones; ++i)
            if (zones[(size_t)i].id == id) return i;
        return -1;
    }
};

inline TriggerZoneSnap makeTriggerZoneSnap(const TriggerZone& z) {
    TriggerZoneSnap s;
    s.id = z.id;
    s.enabled = z.enabled;
    const auto o = z.outline();
    s.vertCount = std::min((int)o.size(), kMaxTriggerZoneVerts);
    s.minX = s.minY = 1e30; s.maxX = s.maxY = -1e30;
    for (int i = 0; i < s.vertCount; ++i) {
        s.vx[(size_t)i] = o[(size_t)i].first;
        s.vy[(size_t)i] = o[(size_t)i].second;
        s.minX = std::min(s.minX, s.vx[(size_t)i]); s.maxX = std::max(s.maxX, s.vx[(size_t)i]);
        s.minY = std::min(s.minY, s.vy[(size_t)i]); s.maxY = std::max(s.maxY, s.vy[(size_t)i]);
    }
    s.zMin = std::min(z.zMin, z.zMax);
    s.zMax = std::max(z.zMin, z.zMax);
    s.edge = std::clamp(std::isfinite(z.edge) ? z.edge : 0.0, 0.0, kTriggerZoneMaxEdge);
    s.hysteresis = std::clamp(std::isfinite(z.hysteresis) ? z.hysteresis : kTriggerZoneDefaultHyst, 0.0, kTriggerZoneMaxHyst);
    return s;
}

class TriggerZoneMailbox {
    std::array<TriggerZoneSet, 3> slots{};
    std::atomic<int> middle{1};
    int back = 2;
    int front = 0;
    uint64_t genCounter = 0;
public:
    void publish(const std::vector<TriggerZone>& zones) noexcept {
        auto& dst = slots[(size_t)back];
        dst.count = std::min((int)zones.size(), kMaxTriggerZones);
        dst.generation = ++genCounter;
        for (int i = 0; i < dst.count; ++i) dst.zones[(size_t)i] = makeTriggerZoneSnap(zones[(size_t)i]);
        back = middle.exchange(back | 4) & 3;
    }
    const TriggerZoneSet& consume() noexcept {
        if (middle.load() & 4) front = middle.exchange(front) & 3;
        return slots[(size_t)front];
    }    uint64_t lastGeneration() const noexcept { return genCounter; }  // message thread (publisher) only
};

// ------------------------------------------------------------------------------------------------ geometry
inline bool triggerZonePointInPlan(const TriggerZoneSnap& z, double x, double y) noexcept {
    if (z.vertCount < 3) return false;
    bool in = false;
    for (int i = 0, j = z.vertCount - 1; i < z.vertCount; j = i++) {
        const double ax = z.vx[(size_t)i], ay = z.vy[(size_t)i];
        const double bx = z.vx[(size_t)j], by = z.vy[(size_t)j];
        if ((ay > y) != (by > y) && x < (bx - ax) * (y - ay) / (by - ay) + ax) in = !in;
    }
    return in;
}

// Signed distance in plan: negative inside, positive outside, 0 on the boundary.
inline double triggerZonePlanSignedDistance(const TriggerZoneSnap& z, double x, double y) noexcept {
    if (z.vertCount < 3) return kTriggerZoneFar;
    double best = 1e30;
    for (int i = 0, j = z.vertCount - 1; i < z.vertCount; j = i++) {
        const double ax = z.vx[(size_t)j], ay = z.vy[(size_t)j];
        const double bx = z.vx[(size_t)i], by = z.vy[(size_t)i];
        const double dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
        const double t = l > 1e-18 ? std::clamp(((x - ax) * dx + (y - ay) * dy) / l, 0.0, 1.0) : 0.0;
        const double ex = x - ax - t * dx, ey = y - ay - t * dy;
        best = std::min(best, ex * ex + ey * ey);
    }
    const double d = std::sqrt(best);
    return triggerZonePointInPlan(z, x, y) ? -d : d;
}

// Signed distance to the prism (plan polygon x [zMin, zMax]): negative inside (distance to the nearest face),
// positive outside (exact Euclidean distance to the prism).
inline double triggerZoneSignedDistance(const TriggerZoneSnap& z, double x, double y, double h) noexcept {
    if (z.vertCount < 3 || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(h)) return kTriggerZoneFar;
    const double dp = triggerZonePlanSignedDistance(z, x, y);
    const double dz = std::max(z.zMin - h, h - z.zMax);
    if (dp <= 0.0 && dz <= 0.0) return std::max(dp, dz);
    return std::hypot(std::max(dp, 0.0), std::max(dz, 0.0));
}

// Does the straight segment p0 -> p1 touch the (closed) zone prism? Exact: clip by the z slab, then test the clipped
// plan segment against the polygon (an end inside, or an edge crossing).
inline bool triggerZoneSegmentHits(const TriggerZoneSnap& z, const double* p0, const double* p1) noexcept {
    if (z.vertCount < 3) return false;
    double t0 = 0.0, t1 = 1.0;
    const double dzv = p1[2] - p0[2];
    if (std::abs(dzv) < 1e-12) {
        if (p0[2] < z.zMin || p0[2] > z.zMax) return false;
    } else {
        double ta = (z.zMin - p0[2]) / dzv, tb = (z.zMax - p0[2]) / dzv;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta); t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    const double ax = p0[0] + (p1[0] - p0[0]) * t0, ay = p0[1] + (p1[1] - p0[1]) * t0;
    const double bx = p0[0] + (p1[0] - p0[0]) * t1, by = p0[1] + (p1[1] - p0[1]) * t1;
    if (std::max(ax, bx) < z.minX || std::min(ax, bx) > z.maxX || std::max(ay, by) < z.minY || std::min(ay, by) > z.maxY)
        return false;
    if (triggerZonePointInPlan(z, ax, ay) || triggerZonePointInPlan(z, bx, by)) return true;
    for (int i = 0, j = z.vertCount - 1; i < z.vertCount; j = i++)
        if (triggerZoneSegmentsCross(ax, ay, bx, by, z.vx[(size_t)j], z.vy[(size_t)j], z.vx[(size_t)i], z.vy[(size_t)i]))
            return true;
    return false;
}

// Soft-edge profile: 0 at the border, 1 at `edge` metres inside, C1 smoothstep in between. edge <= 0: hard 0/1.
inline double triggerZoneSoftValue(double signedDist, double edge) noexcept {
    if (!(edge > 0.0)) return signedDist <= 0.0 ? 1.0 : 0.0;
    const double s = std::clamp(-signedDist / edge, 0.0, 1.0);
    return s * s * (3.0 - 2.0 * s);
}

// ------------------------------------------------------------------------------------------------ audio-thread tracker
// Hard occupancy per (object, zone) with hysteresis and a swept-segment test:
//   * out -> in : the current pose is inside (signed distance <= 0), or the segment from the previous control period's
//                 pose to the current one touched the zone (a fast object crossing a thin zone still triggers for one
//                 control period);
//   * in -> out : the pose is at least `hysteresis` metres OUTSIDE the zone (no chatter at the border).
// Reset (new engine, transport seek / loop, wall-clock gap, an object jump, a changed zone set): the state is
// re-evaluated from the pose alone (inside <=> signed distance <= 0); no sweep from the stale position.
class TriggerZoneTracker {
public:
    std::array<std::array<uint8_t, kMaxTriggerZones>, kTriggerZoneObjects> hard{};
    std::array<std::array<double, kMaxTriggerZones>, kTriggerZoneObjects> dist{};
    std::array<std::array<double, kTriggerZoneObjects>, 3> prevPos{};
    std::array<uint8_t, kTriggerZoneObjects> hasPrev{};
    std::array<int, kTriggerZoneObjects> index{};      // 0 = outside all zones, 1..N = first zone (list order) containing it
    uint64_t lastGen = ~0ull;
    int zoneCount = 0;
    const TriggerZoneSet* set = nullptr;

    void reset() noexcept {
        for (auto& row : hard) row.fill(0);
        for (auto& row : dist) row.fill(kTriggerZoneFar);
        hasPrev.fill(0);
        index.fill(0);
        lastGen = ~0ull;
    }

    // poses[o] = {x, y, z, az}; objects 0..numSources-1 and 32 are evaluated.
    void update(const TriggerZoneSet& zs, int numSources, const double (*poses)[4], const bool* objJumped, bool resetAll) noexcept {
        set = &zs;
        zoneCount = std::min(zs.count, kMaxTriggerZones);
        const bool genChanged = zs.generation != lastGen;
        if (genChanged) lastGen = zs.generation;
        for (int o = 0; o < kTriggerZoneObjects; ++o) {
            if (o >= numSources && o != kTriggerZoneObjects - 1) continue;
            const double* p = poses[o];
            const bool fresh = resetAll || genChanged || !hasPrev[(size_t)o] || (objJumped && objJumped[o]);
            int first = 0;
            for (int zi = 0; zi < zoneCount; ++zi) {
                const auto& z = zs.zones[(size_t)zi];
                uint8_t& st = hard[(size_t)o][(size_t)zi];
                if (!z.enabled || z.vertCount < 3) { st = 0; dist[(size_t)o][(size_t)zi] = kTriggerZoneFar; continue; }
                const double d = triggerZoneSignedDistance(z, p[0], p[1], p[2]);
                dist[(size_t)o][(size_t)zi] = d;
                if (fresh) st = d <= 0.0 ? 1 : 0;
                else if (st) st = d >= z.hysteresis && d > 0.0 ? 0 : 1;
                else {
                    const double prev[3] = {prevPos[0][(size_t)o], prevPos[1][(size_t)o], prevPos[2][(size_t)o]};
                    st = (d <= 0.0 || triggerZoneSegmentHits(z, prev, p)) ? 1 : 0;
                }
                if (st && first == 0) first = zi + 1;
            }
            for (int zi = zoneCount; zi < kMaxTriggerZones; ++zi) { hard[(size_t)o][(size_t)zi] = 0; dist[(size_t)o][(size_t)zi] = kTriggerZoneFar; }
            index[(size_t)o] = first;
            prevPos[0][(size_t)o] = p[0]; prevPos[1][(size_t)o] = p[1]; prevPos[2][(size_t)o] = p[2];
            hasPrev[(size_t)o] = 1;
        }
    }

    // ---- driver values (object o, zone id) ----
    double inZone(int o, int zoneId) const noexcept {
        if (!set || o < 0 || o >= kTriggerZoneObjects) return 0.0;
        const int zi = set->slotForId(zoneId);
        if (zi < 0 || zi >= zoneCount) return 0.0;
        const auto& z = set->zones[(size_t)zi];
        if (!z.enabled) return 0.0;
        if (z.edge > 0.0) return triggerZoneSoftValue(dist[(size_t)o][(size_t)zi], z.edge);
        return hard[(size_t)o][(size_t)zi] ? 1.0 : 0.0;
    }
    double distanceTo(int o, int zoneId) const noexcept {
        if (!set || o < 0 || o >= kTriggerZoneObjects) return kTriggerZoneFar;
        const int zi = set->slotForId(zoneId);
        if (zi < 0 || zi >= zoneCount || !set->zones[(size_t)zi].enabled) return kTriggerZoneFar;
        return dist[(size_t)o][(size_t)zi];
    }
    int zoneIndex(int o) const noexcept { return (o >= 0 && o < kTriggerZoneObjects) ? index[(size_t)o] : 0; }
    uint64_t occupancyMask(int zi, int numSources) const noexcept {
        if (zi < 0 || zi >= zoneCount) return 0;
        uint64_t m = 0;
        for (int o = 0; o < kTriggerZoneObjects; ++o) {
            if (o >= numSources && o != kTriggerZoneObjects - 1) continue;
            if (hard[(size_t)o][(size_t)zi]) m |= (uint64_t)1 << o;
        }
        return m;
    }
};

}  // namespace verbp
