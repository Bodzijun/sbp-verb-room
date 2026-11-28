#pragma once
#include "MovementPath.h"
#include "scene.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <vector>

namespace verbp {

enum class PathShapeKind : uint8_t {
    Circle = 0,
    Ellipse = 1,
    Figure8 = 2,
    Square = 3,
    Rectangle = 4,
    Triangle = 5,
    Star = 6,
    Spiral = 7
};

inline const char* pathShapeName(PathShapeKind k) noexcept {
    switch (k) {
    case PathShapeKind::Circle: return "circle";
    case PathShapeKind::Ellipse: return "ellipse";
    case PathShapeKind::Figure8: return "figure8";
    case PathShapeKind::Square: return "square";
    case PathShapeKind::Rectangle: return "rectangle";
    case PathShapeKind::Triangle: return "triangle";
    case PathShapeKind::Star: return "star";
    case PathShapeKind::Spiral: return "spiral";
    default: return "circle";
    }
}

inline bool parsePathShapeKind(std::string_view sv, PathShapeKind& out) noexcept {
    if (sv == "circle") { out = PathShapeKind::Circle; return true; }
    if (sv == "ellipse") { out = PathShapeKind::Ellipse; return true; }
    if (sv == "figure8") { out = PathShapeKind::Figure8; return true; }
    if (sv == "square") { out = PathShapeKind::Square; return true; }
    if (sv == "rectangle") { out = PathShapeKind::Rectangle; return true; }
    if (sv == "triangle") { out = PathShapeKind::Triangle; return true; }
    if (sv == "star") { out = PathShapeKind::Star; return true; }
    if (sv == "spiral") { out = PathShapeKind::Spiral; return true; }
    return false;
}

struct PathShapeParams {
    PathShapeKind kind = PathShapeKind::Circle;
    double cx = 0.0;
    double cy = 0.0;
    double z = 1.5;
    double zEnd = std::numeric_limits<double>::quiet_NaN(); // Spiral end height Z (m); NaN = same as start height z
    double radiusX = 2.0;      // radius / half-axis X / half-side X / outer radius (m)
    double radiusY = 1.2;      // half-axis Y / lobe half-width / half-side Y / inner radius (m)
    double rotationDeg = 0.0;  // rotation in plan view (deg)
    double startPhaseDeg = 0.0;// starting phase along the loop (deg, 0..360)
    bool clockwise = false;    // false = CCW, true = CW
    int nodes = 16;            // node count for smooth curves (Circle/Ellipse/Figure8: 12..64, Spiral: 16..64)
    int spiralTurns = 2;       // closed in/out spiral turns (1..6)
    bool spiralOutIn = true;   // true = inner->outer->inner, false = outer->inner->outer

    double effectiveZEnd() const noexcept {
        return std::isfinite(zEnd) ? zEnd : z;
    }
};

namespace detail {
inline constexpr double kShapePi = 3.14159265358979323846;

inline std::vector<PathPoint> applyPolygonStartPhase(const std::vector<PathPoint>& corners, double startPhaseDeg) {
    const size_t m = corners.size();
    if (m < 3) return corners;
    double norm = (startPhaseDeg / 360.0) - std::floor(startPhaseDeg / 360.0);
    if (!std::isfinite(norm) || norm < 0.0 || norm >= 1.0) norm = 0.0;
    if (norm < 1e-7 || 1.0 - norm < 1e-7) return corners;

    std::vector<double> edgeLen(m, 0.0);
    double perim = 0.0;
    for (size_t i = 0; i < m; ++i) {
        edgeLen[i] = pathDistance(corners[i], corners[(i + 1) % m]);
        perim += edgeLen[i];
    }
    if (perim <= 1e-9) return corners;

    const double targetDist = norm * perim;
    double acc = 0.0;
    size_t edgeIdx = 0;
    double localT = 0.0;
    for (size_t i = 0; i < m; ++i) {
        const double nextAcc = acc + edgeLen[i];
        if (targetDist <= nextAcc || i + 1 == m) {
            edgeIdx = i;
            localT = edgeLen[i] > 1e-12 ? std::clamp((targetDist - acc) / edgeLen[i], 0.0, 1.0) : 0.0;
            break;
        }
        acc = nextAcc;
    }

    std::vector<PathPoint> out;
    if (localT < 1e-5 || 1.0 - localT < 1e-5) {
        const size_t startCorner = (1.0 - localT < 1e-5) ? ((edgeIdx + 1) % m) : edgeIdx;
        out.reserve(m);
        for (size_t k = 0; k < m; ++k) out.push_back(corners[(startCorner + k) % m]);
        return out;
    }

    out.reserve(m + 1);
    out.push_back(pathMix(corners[edgeIdx], corners[(edgeIdx + 1) % m], localT));
    for (size_t k = 1; k <= m; ++k) out.push_back(corners[(edgeIdx + k) % m]);
    return out;
}

inline double pointToSegmentDist2D(double px, double py, double ax, double ay, double bx, double by) noexcept {
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    const double t = len2 > 1e-12 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / len2, 0.0, 1.0) : 0.0;
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

inline double cross2D(double ax, double ay, double bx, double by, double cx, double cy) noexcept {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

inline bool segmentsIntersect2D(double ax, double ay, double bx, double by,
                                double cx, double cy, double dx, double dy) noexcept {
    if (std::max(ax, bx) < std::min(cx, dx) - 1e-9 || std::max(cx, dx) < std::min(ax, bx) - 1e-9 ||
        std::max(ay, by) < std::min(cy, dy) - 1e-9 || std::max(cy, dy) < std::min(ay, by) - 1e-9)
        return false;
    const double d1 = cross2D(ax, ay, bx, by, cx, cy);
    const double d2 = cross2D(ax, ay, bx, by, dx, dy);
    const double d3 = cross2D(cx, cy, dx, dy, ax, ay);
    const double d4 = cross2D(cx, cy, dx, dy, bx, by);
    return ((d1 <= 0.0 && d2 >= 0.0) || (d1 >= 0.0 && d2 <= 0.0)) &&
           ((d3 <= 0.0 && d4 >= 0.0) || (d3 >= 0.0 && d4 <= 0.0));
}
} // namespace detail

inline MovementPath generateRawPathShape(const PathShapeParams& p) {
    using detail::kShapePi;
    MovementPath out;
    out.enabled = true;
    out.closed = true;
    const double z = std::clamp(std::isfinite(p.z) ? p.z : 1.5, 0.0, 30.0);
    const double cx = std::clamp(std::isfinite(p.cx) ? p.cx : 0.0, -200.0, 200.0);
    const double cy = std::clamp(std::isfinite(p.cy) ? p.cy : 0.0, -200.0, 200.0);
    const double dir = p.clockwise ? -1.0 : 1.0;
    const double rot = (std::isfinite(p.rotationDeg) ? p.rotationDeg : 0.0) * kShapePi / 180.0;
    const double cr = std::cos(rot), sr = std::sin(rot);
    const double phaseDeg = std::isfinite(p.startPhaseDeg) ? p.startPhaseDeg : 0.0;
    const double phaseRad = phaseDeg * kShapePi / 180.0;

    switch (p.kind) {
    case PathShapeKind::Circle: {
        out.smooth = true;
        const int n = std::clamp(p.nodes, 12, 64);
        const double r = std::max(0.05, std::isfinite(p.radiusX) ? p.radiusX : 2.0);
        const double a0 = rot + phaseRad;
        out.points.reserve((size_t)n);
        for (int k = 0; k < n; ++k) {
            const double a = a0 + dir * (2.0 * kShapePi * (double)k / (double)n);
            out.points.push_back({cx + r * std::cos(a), cy + r * std::sin(a), z});
        }
        break;
    }
    case PathShapeKind::Ellipse: {
        out.smooth = true;
        const int n = std::clamp(p.nodes, 12, 64);
        const double rx = std::max(0.05, std::isfinite(p.radiusX) ? p.radiusX : 2.5);
        const double ry = std::max(0.05, (std::isfinite(p.radiusY) && p.radiusY > 0.0) ? p.radiusY : 0.6 * rx);
        out.points.reserve((size_t)n);
        for (int k = 0; k < n; ++k) {
            const double phi = phaseRad + dir * (2.0 * kShapePi * (double)k / (double)n);
            const double u = rx * std::cos(phi), v = ry * std::sin(phi);
            out.points.push_back({cx + u * cr - v * sr, cy + u * sr + v * cr, z});
        }
        break;
    }
    case PathShapeKind::Figure8: {
        // Lemniscate of Gerono: u = a * sin(phi), v = b * sin(2 * phi).
        // With even N and startPhaseDeg = 0, node 0 (phi = 0) and node N/2 (phi = pi) cross (cx, cy) exactly.
        out.smooth = true;
        int n = std::clamp(p.nodes, 12, 64);
        if (n % 2 != 0) n = (n < 64) ? n + 1 : 64;
        const double rx = std::max(0.05, std::isfinite(p.radiusX) ? p.radiusX : 2.0);
        const double ry = std::max(0.05, (std::isfinite(p.radiusY) && p.radiusY > 0.0) ? p.radiusY : 0.5 * rx);
        out.points.reserve((size_t)n);
        for (int k = 0; k < n; ++k) {
            const double phi = phaseRad + dir * (2.0 * kShapePi * (double)k / (double)n);
            const double u = rx * std::sin(phi), v = ry * std::sin(2.0 * phi);
            out.points.push_back({cx + u * cr - v * sr, cy + u * sr + v * cr, z});
        }
        break;
    }
    case PathShapeKind::Square:
    case PathShapeKind::Rectangle: {
        out.smooth = false;
        const double rx = std::max(0.05, std::isfinite(p.radiusX) ? p.radiusX : 2.0);
        const double ry = (p.kind == PathShapeKind::Square)
            ? rx
            : std::max(0.05, (std::isfinite(p.radiusY) && p.radiusY > 0.0) ? p.radiusY : 0.6 * rx);
        const std::array<std::pair<double, double>, 4> local = p.clockwise
            ? std::array<std::pair<double, double>, 4>{{{rx, ry}, {rx, -ry}, {-rx, -ry}, {-rx, ry}}}
            : std::array<std::pair<double, double>, 4>{{{rx, ry}, {-rx, ry}, {-rx, -ry}, {rx, -ry}}};
        std::vector<PathPoint> corners;
        corners.reserve(4);
        for (const auto& uv : local) {
            corners.push_back({cx + uv.first * cr - uv.second * sr, cy + uv.first * sr + uv.second * cr, z});
        }
        out.points = detail::applyPolygonStartPhase(corners, phaseDeg);
        break;
    }
    case PathShapeKind::Triangle: {
        out.smooth = false;
        const double r = std::max(0.05, std::isfinite(p.radiusX) ? p.radiusX : 2.0);
        std::vector<PathPoint> corners;
        corners.reserve(3);
        for (int k = 0; k < 3; ++k) {
            const double a = rot + dir * (2.0 * kShapePi * (double)k / 3.0);
            corners.push_back({cx + r * std::cos(a), cy + r * std::sin(a), z});
        }
        out.points = detail::applyPolygonStartPhase(corners, phaseDeg);
        break;
    }
    case PathShapeKind::Star: {
        out.smooth = false;
        const double rOut = std::max(0.05, std::isfinite(p.radiusX) ? p.radiusX : 2.0);
        const double rIn = (std::isfinite(p.radiusY) && p.radiusY > 0.0)
            ? std::clamp(p.radiusY, 0.05 * rOut, 0.95 * rOut)
            : (0.3819660112501051 * rOut);
        std::vector<PathPoint> corners;
        corners.reserve(10);
        for (int k = 0; k < 10; ++k) {
            const double rad = (k % 2 == 0) ? rOut : rIn;
            const double a = rot + dir * (kShapePi * (double)k / 5.0);
            corners.push_back({cx + rad * std::cos(a), cy + rad * std::sin(a), z});
        }
        out.points = detail::applyPolygonStartPhase(corners, phaseDeg);
        break;
    }
    case PathShapeKind::Spiral: {
        out.smooth = true;
        int n = std::clamp(p.nodes, 16, 64);
        if (n % 2 != 0) n = (n < 64) ? n + 1 : 64;
        const int turns = std::clamp(p.spiralTurns, 1, 6);
        const double rOut = std::max(0.1, std::isfinite(p.radiusX) ? p.radiusX : 2.0);
        const double rIn = (std::isfinite(p.radiusY) && p.radiusY > 0.0)
            ? std::clamp(p.radiusY, 0.02, 0.92 * rOut)
            : (0.25 * rOut);
        const double zStart = z;
        const double zEndVal = std::clamp(std::isfinite(p.zEnd) ? p.zEnd : zStart, 0.0, 30.0);
        double phase0 = (phaseDeg / 360.0) - std::floor(phaseDeg / 360.0);
        if (!std::isfinite(phase0) || phase0 < 0.0 || phase0 >= 1.0) phase0 = 0.0;
        out.points.reserve((size_t)n);
        for (int k = 0; k < n; ++k) {
            double s = phase0 + (double)k / (double)n;
            s -= std::floor(s);
            const double w = 0.5 * (1.0 - std::cos(2.0 * kShapePi * s));
            const double rad = p.spiralOutIn ? (rIn + (rOut - rIn) * w) : (rOut - (rOut - rIn) * w);
            const double a = rot + dir * (2.0 * kShapePi * (double)turns * s);
            const double tZ = (n > 1) ? ((double)k / (double)(n - 1)) : 0.0;
            const double zk = zStart + (zEndVal - zStart) * tZ;
            out.points.push_back({cx + rad * std::cos(a), cy + rad * std::sin(a), zk});
        }
        break;
    }
    }

    for (auto& pt : out.points) {
        pt[0] = std::clamp(pt[0], -200.0, 200.0);
        pt[1] = std::clamp(pt[1], -200.0, 200.0);
        pt[2] = std::clamp(pt[2], 0.0, 30.0);
    }
    return out;
}

struct ShapeRoomFitResult {
    MovementPath path;
    PathShapeParams fittedParams;
    bool outsideRoom = false;
    bool clamped = false;
    double appliedScale = 1.0;
};

inline void roomVerticalBoundsAt(const verb::Scene& sc, double x, double y, double& zFloor, double& zCeil) noexcept {
    zFloor = 0.0;
    for (const auto& fz : sc.floorZones) {
        if (fz.z >= 0.0 && fz.poly.size() >= 3 && verb::pointInZone(fz, x, y))
            zFloor = std::max(zFloor, fz.z);
    }
    zCeil = (sc.ceilMat < 0) ? 30.0 : std::max(1.0, sc.height);
    for (const auto& cz : sc.ceilZones) {
        if (cz.z > 0.0 && cz.poly.size() >= 3 && verb::pointInZone(cz, x, y))
            zCeil = std::min(zCeil, cz.z);
    }
}

inline bool isPointInsideRoom(const verb::Scene& sc, const verb::CeilingPlan& plan,
                              double x, double y, double z,
                              double clearanceXY = 0.08, double clearanceZ = 0.05) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    if (x < -200.0 + clearanceXY || x > 200.0 - clearanceXY ||
        y < -200.0 + clearanceXY || y > 200.0 - clearanceXY)
        return false;
    double zFloor = 0.0, zCeil = 30.0;
    roomVerticalBoundsAt(sc, x, y, zFloor, zCeil);
    if (z < zFloor + clearanceZ || z > zCeil - clearanceZ) return false;
    if (!plan.polygons.empty() && !plan.contains(x, y)) return false;
    for (const auto& w : sc.walls) {
        if (z < w.z0 - clearanceZ || z > w.z1 + clearanceZ) continue;
        if (detail::pointToSegmentDist2D(x, y, w.ax, w.ay, w.bx, w.by) < clearanceXY) return false;
    }
    return true;
}

inline bool isCompiledPathInsideRoom(const verb::Scene& sc, const verb::CeilingPlan& plan,
                                     const MovementPath& mp,
                                     double clearanceXY = 0.08, double clearanceZ = 0.05) {
    if (mp.points.size() < 2) return false;
    for (const auto& q : mp.points) {
        if (!isPointInsideRoom(sc, plan, q[0], q[1], q[2], clearanceXY, clearanceZ)) return false;
    }
    CompiledPath cp;
    cp.build(mp);
    for (size_t i = 0; i < cp.points.size(); ++i) {
        const auto& q = cp.points[i];
        if (!isPointInsideRoom(sc, plan, q[0], q[1], q[2], clearanceXY, clearanceZ)) return false;
        if (i > 0) {
            const auto& prev = cp.points[i - 1];
            const double zMid = 0.5 * (prev[2] + q[2]);
            for (const auto& w : sc.walls) {
                if (zMid < w.z0 - clearanceZ || zMid > w.z1 + clearanceZ) continue;
                if (detail::segmentsIntersect2D(prev[0], prev[1], q[0], q[1], w.ax, w.ay, w.bx, w.by))
                    return false;
            }
        }
    }
    return true;
}

inline ShapeRoomFitResult generatePathShapeForRoom(const PathShapeParams& params,
                                                   const verb::Scene* room = nullptr,
                                                   bool clampToRoom = true,
                                                   double clearanceXY = 0.08,
                                                   double clearanceZ = 0.05) {
    ShapeRoomFitResult res;
    res.fittedParams = params;
    res.path = generateRawPathShape(params);
    if (!room) return res;

    const verb::Scene& sc = *room;
    const verb::CeilingPlan plan(sc);
    if (isCompiledPathInsideRoom(sc, plan, res.path, clearanceXY, clearanceZ)) {
        return res;
    }
    res.outsideRoom = true;
    if (!clampToRoom) return res;

    PathShapeParams fit = params;
    // 1. Clamp Z (and optional Spiral zEnd) to safe vertical bounds at (cx, cy)
    double zFloor = 0.0, zCeil = (sc.ceilMat < 0) ? 30.0 : std::max(1.0, sc.height);
    roomVerticalBoundsAt(sc, fit.cx, fit.cy, zFloor, zCeil);
    if (zCeil - zFloor <= 2.0 * clearanceZ) {
        fit.z = 0.5 * (zFloor + zCeil);
        if (std::isfinite(fit.zEnd)) fit.zEnd = fit.z;
    } else {
        fit.z = std::clamp(fit.z, zFloor + clearanceZ, zCeil - clearanceZ);
        if (std::isfinite(fit.zEnd)) fit.zEnd = std::clamp(fit.zEnd, zFloor + clearanceZ, zCeil - clearanceZ);
    }

    // 2. Ensure (cx, cy) is safely inside the room with margin >= max(0.20, clearanceXY * 2)
    const double centreClearance = std::max(0.20, clearanceXY * 2.0);
    if (!isPointInsideRoom(sc, plan, fit.cx, fit.cy, fit.z, centreClearance, clearanceZ)) {
        // Compute safe interior anchor (listener if inside, or polygon centroid / wall bbox midpoint)
        double anchorX = fit.cx, anchorY = fit.cy;
        bool foundAnchor = false;
        if (sc.hasListener && isPointInsideRoom(sc, plan, sc.listenerPos.x, sc.listenerPos.y, fit.z, centreClearance, clearanceZ)) {
            anchorX = sc.listenerPos.x;
            anchorY = sc.listenerPos.y;
            foundAnchor = true;
        }
        if (!foundAnchor && !plan.polygons.empty() && !plan.polygons[0].poly.empty()) {
            double sx = 0.0, sy = 0.0;
            for (const auto& pt : plan.polygons[0].poly) { sx += pt.first; sy += pt.second; }
            const double mx = sx / (double)plan.polygons[0].poly.size();
            const double my = sy / (double)plan.polygons[0].poly.size();
            if (isPointInsideRoom(sc, plan, mx, my, fit.z, centreClearance, clearanceZ)) {
                anchorX = mx; anchorY = my; foundAnchor = true;
            }
        }
        if (!foundAnchor && !sc.walls.empty()) {
            double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
            for (const auto& w : sc.walls) {
                x0 = std::min({x0, w.ax, w.bx}); x1 = std::max({x1, w.ax, w.bx});
                y0 = std::min({y0, w.ay, w.by}); y1 = std::max({y1, w.ay, w.by});
            }
            const double mx = 0.5 * (x0 + x1), my = 0.5 * (y0 + y1);
            if (isPointInsideRoom(sc, plan, mx, my, fit.z, clearanceXY, clearanceZ)) {
                anchorX = mx; anchorY = my; foundAnchor = true;
            }
        }
        if (foundAnchor) {
            // Pull from anchor toward requested (cx, cy) as far as stays valid with centreClearance
            double lo = 0.0, hi = 1.0;
            for (int it = 0; it < 20; ++it) {
                const double mid = 0.5 * (lo + hi);
                const double tx = anchorX + (fit.cx - anchorX) * mid;
                const double ty = anchorY + (fit.cy - anchorY) * mid;
                if (isPointInsideRoom(sc, plan, tx, ty, fit.z, centreClearance, clearanceZ)) lo = mid;
                else hi = mid;
            }
            fit.cx = anchorX + (fit.cx - anchorX) * lo;
            fit.cy = anchorY + (fit.cy - anchorY) * lo;
            roomVerticalBoundsAt(sc, fit.cx, fit.cy, zFloor, zCeil);
            if (zCeil - zFloor > 2.0 * clearanceZ) {
                fit.z = std::clamp(fit.z, zFloor + clearanceZ, zCeil - clearanceZ);
                if (std::isfinite(fit.zEnd)) fit.zEnd = std::clamp(fit.zEnd, zFloor + clearanceZ, zCeil - clearanceZ);
            }
        }
    }

    // 3. Binary-search uniform scale s in [0.02, 1.0] so the entire compiled path fits inside the room
    const double baseRx = std::max(0.05, std::isfinite(params.radiusX) ? params.radiusX : 2.0);
    const double baseRy = (std::isfinite(params.radiusY) && params.radiusY > 0.0) ? params.radiusY : (
        params.kind == PathShapeKind::Figure8 ? 0.5 * baseRx :
        params.kind == PathShapeKind::Star ? 0.3819660112501051 * baseRx :
        params.kind == PathShapeKind::Spiral ? 0.25 * baseRx :
        params.kind == PathShapeKind::Square ? baseRx : 0.6 * baseRx);

    auto makeScaled = [&](double s) {
        PathShapeParams cand = fit;
        cand.radiusX = std::max(0.05, baseRx * s);
        cand.radiusY = std::max(0.02, baseRy * s);
        return cand;
    };

    if (isCompiledPathInsideRoom(sc, plan, generateRawPathShape(makeScaled(1.0)), clearanceXY, clearanceZ)) {
        res.fittedParams = makeScaled(1.0);
        res.path = generateRawPathShape(res.fittedParams);
        res.clamped = true;
        res.appliedScale = 1.0;
        return res;
    }

    double lo = 0.02, hi = 1.0;
    for (int it = 0; it < 24; ++it) {
        const double mid = 0.5 * (lo + hi);
        const auto candPath = generateRawPathShape(makeScaled(mid));
        if (isCompiledPathInsideRoom(sc, plan, candPath, clearanceXY, clearanceZ)) lo = mid;
        else hi = mid;
    }
    res.fittedParams = makeScaled(lo);
    res.path = generateRawPathShape(res.fittedParams);
    res.clamped = true;
    res.appliedScale = lo;
    return res;
}

// Offset the default centre of a newly created Figure-8 shape so its crossing point (cx, cy)
// is at least minDistanceM (default 1.5 m) away from the listener (lx, ly) and clamped inside the room.
inline PathShapeParams offsetFigure8DefaultCentre(const PathShapeParams& initial,
                                                  double lx, double ly,
                                                  const verb::Scene* room = nullptr,
                                                  double minDistanceM = 1.5,
                                                  double preferredX = std::numeric_limits<double>::quiet_NaN(),
                                                  double preferredY = std::numeric_limits<double>::quiet_NaN()) {
    PathShapeParams p = initial;
    p.kind = PathShapeKind::Figure8;
    const double curDist = std::hypot(p.cx - lx, p.cy - ly);
    if (curDist >= minDistanceM - 1e-6) {
        if (room) {
            const auto fit = generatePathShapeForRoom(p, room, true);
            if (std::hypot(fit.fittedParams.cx - lx, fit.fittedParams.cy - ly) >= minDistanceM - 1e-6)
                return fit.fittedParams;
        } else {
            return p;
        }
    }

    std::vector<std::pair<double, double>> dirs;
    auto pushDir = [&](double dx, double dy) {
        const double len = std::hypot(dx, dy);
        if (len > 1e-6) dirs.push_back({dx / len, dy / len});
    };
    if (std::isfinite(preferredX) && std::isfinite(preferredY))
        pushDir(preferredX - lx, preferredY - ly);
    pushDir(p.cx - lx, p.cy - ly);
    if (room && !room->walls.empty()) {
        double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
        for (const auto& w : room->walls) {
            x0 = std::min({x0, w.ax, w.bx}); x1 = std::max({x1, w.ax, w.bx});
            y0 = std::min({y0, w.ay, w.by}); y1 = std::max({y1, w.ay, w.by});
        }
        pushDir(0.5 * (x0 + x1) - lx, 0.5 * (y0 + y1) - ly);
    }
    pushDir(0.0, 1.0);
    pushDir(0.0, -1.0);
    pushDir(1.0, 0.0);
    pushDir(-1.0, 0.0);
    pushDir(0.70710678, 0.70710678);
    pushDir(-0.70710678, 0.70710678);
    pushDir(0.70710678, -0.70710678);
    pushDir(-0.70710678, -0.70710678);

    const double targetDist = std::max(minDistanceM, 1.6);
    if (!room) {
        const auto d = dirs.empty() ? std::pair<double, double>{0.0, 1.0} : dirs.front();
        p.cx = lx + d.first * targetDist;
        p.cy = ly + d.second * targetDist;
        return p;
    }

    const verb::CeilingPlan plan(*room);
    PathShapeParams best = p;
    double bestScore = -1e30;
    const double testDists[] = {targetDist, minDistanceM, 1.8, 2.0};
    for (double dist : testDists) {
        for (const auto& dir : dirs) {
            PathShapeParams cand = p;
            cand.cx = lx + dir.first * dist;
            cand.cy = ly + dir.second * dist;
            if (!isPointInsideRoom(*room, plan, cand.cx, cand.cy, cand.z, 0.20, 0.05)) continue;
            const auto fit = generatePathShapeForRoom(cand, room, true);
            const double actualDist = std::hypot(fit.fittedParams.cx - lx, fit.fittedParams.cy - ly);
            if (actualDist < minDistanceM - 1e-4) continue;
            const double score = fit.appliedScale * 10.0 - std::fabs(actualDist - targetDist);
            if (score > bestScore) {
                bestScore = score;
                best = fit.fittedParams;
            }
        }
        if (bestScore > -1e20) break;
    }
    if (bestScore > -1e20) return best;

    // Fallback for very small rooms: pick the valid interior direction that maximizes distance from listener.
    double maxDist = -1.0;
    for (const auto& dir : dirs) {
        PathShapeParams cand = p;
        cand.cx = lx + dir.first * targetDist;
        cand.cy = ly + dir.second * targetDist;
        const auto fit = generatePathShapeForRoom(cand, room, true);
        const double actualDist = std::hypot(fit.fittedParams.cx - lx, fit.fittedParams.cy - ly);
        if (actualDist > maxDist) {
            maxDist = actualDist;
            best = fit.fittedParams;
        }
    }
    return best;
}

} // namespace verbp
