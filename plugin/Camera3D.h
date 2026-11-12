#pragma once
// Camera of the editor's 3D view, kept free of JUCE so that its maths can be unit-tested.
//
// World: x, y on the plan, z up (metres). The camera looks at `target` from a direction given by `yaw` (turn about z) and
// `pitch` (elevation) through a perspective of constant distance D = 3.2 * radius, where `radius` is the radius of the
// whole scene. `zoom` multiplies the scale that fits the whole scene into the viewport (zoom 1 = "frame all").
// Everything is a pure function of this state: no persistent camera data, the state lives in the editor session only.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace verbp {

struct Cam3D {
    struct Vec { double x = 0, y = 0, z = 0; };
    struct Pt { double x = 0, y = 0, depth = 0; };  // screen position (pixels) and distance behind the target plane

    static constexpr double kMinZoom = 0.25, kMaxZoom = 64.0;

    double yaw = -0.75, pitch = 0.5, zoom = 1.0;
    Vec target;                   // the point the camera orbits around and that is drawn at the viewport centre
    bool followScene = true;      // the target tracks the centre of the scene until the user moves or centres it
    Vec centre;                   // centre of the scene's bounding box
    double radius = 5.0;          // half the diagonal of the scene's bounding box (+0.5 m)
    double areaCx = 0, areaCy = 0, areaW = 100, areaH = 100;  // viewport centre and size, pixels

    double distance() const { return 3.2 * radius; }
    double scale() const { return std::min(areaW, areaH) / (2.2 * radius) * zoom; }  // pixels per metre at the target depth

    void setScene(Vec c, double r) {
        centre = c;
        radius = std::max(r, 0.5);
        if (followScene) target = c;
    }
    void setViewport(double cx, double cy, double w, double h) { areaCx = cx; areaCy = cy; areaW = std::max(w, 1.0); areaH = std::max(h, 1.0); }

    // Camera axes in world coordinates: screen right, screen up, and the viewing direction (depth grows away from the camera).
    Vec right() const { return {std::cos(yaw), -std::sin(yaw), 0}; }
    Vec up() const { return {std::sin(pitch) * std::sin(yaw), std::sin(pitch) * std::cos(yaw), std::cos(pitch)}; }
    Vec forward() const { return {std::cos(pitch) * std::sin(yaw), std::cos(pitch) * std::cos(yaw), -std::sin(pitch)}; }

    Pt project(Vec p) const {
        const Vec d{p.x - target.x, p.y - target.y, p.z - target.z};
        const Vec r = right(), u = up(), f = forward();
        const double xr = r.x * d.x + r.y * d.y + r.z * d.z, zr = u.x * d.x + u.y * d.y + u.z * d.z;
        const double depth = f.x * d.x + f.y * d.y + f.z * d.z;
        const double persp = distance() / std::max(distance() + depth, 0.05 * distance());
        return {areaCx + xr * scale() * persp, areaCy - zr * scale() * persp, depth};
    }
    // The world point on the plane through the target (perpendicular to the viewing direction) that is drawn at (px, py).
    Vec unprojectTargetPlane(double px, double py) const {
        const Vec r = right(), u = up();
        const double a = (px - areaCx) / scale(), b = -(py - areaCy) / scale();
        return {target.x + a * r.x + b * u.x, target.y + a * r.y + b * u.y, target.z + a * r.z + b * u.z};
    }

    void clampZoom() { zoom = std::min(kMaxZoom, std::max(kMinZoom, zoom)); }

    // Drag: the content at `depth` behind the target plane follows the pointer by (dx, dy) pixels.
    void pan(double dx, double dy, double depth = 0.0) {
        const double persp = distance() / std::max(distance() + depth, 0.05 * distance());
        const double k = 1.0 / (scale() * persp);
        const Vec r = right(), u = up();
        target.x += (-dx * r.x + dy * u.x) * k;
        target.y += (-dx * r.y + dy * u.y) * k;
        target.z += (-dx * r.z + dy * u.z) * k;
        followScene = false;
    }
    // Wheel: multiplies the scale by `factor`; the point of the target plane under (px, py) stays under it.
    void zoomAt(double factor, double px, double py) {
        const Vec q = unprojectTargetPlane(px, py);
        const double before = zoom;
        zoom *= factor;
        clampZoom();
        const double applied = zoom / before;
        // shift the target towards q so that q keeps its screen position: target' = target + (1 - 1/applied) * (q - target)
        const double s = 1.0 - 1.0 / applied;
        const Vec shift{s * (q.x - target.x), s * (q.y - target.y), s * (q.z - target.z)};
        target.x += shift.x; target.y += shift.y; target.z += shift.z;
        if (std::abs(shift.x) + std::abs(shift.y) + std::abs(shift.z) > 1e-9) followScene = false;  // zooming at the centre keeps following the scene
    }
    // Orbit about `p` from now on (double-click on an object). The zoom stays.
    void centreOn(Vec p) { target = p; followScene = false; }

    // Fits the given world points into the viewport (with `margin` of the half-size left free), keeping yaw and pitch.
    void frame(const std::vector<Vec>& pts, double margin = 0.92) {
        if (pts.empty()) return;
        zoom = 1.0;
        Vec mid;
        for (auto& p : pts) { mid.x += p.x; mid.y += p.y; mid.z += p.z; }
        mid = {mid.x / (double)pts.size(), mid.y / (double)pts.size(), mid.z / (double)pts.size()};
        target = mid;
        for (int pass = 0; pass < 4; ++pass) {  // perspective makes the screen box move a little with the target: iterate
            double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
            for (auto& p : pts) { const Pt q = project(p); x0 = std::min(x0, q.x); x1 = std::max(x1, q.x); y0 = std::min(y0, q.y); y1 = std::max(y1, q.y); }
            const double mx = 0.5 * (x0 + x1) - areaCx, my = 0.5 * (y0 + y1) - areaCy;
            if (std::abs(mx) < 1e-6 && std::abs(my) < 1e-6) break;
            pan(-mx, -my, 0.0);  // content moves by -(offset) so that the box centre meets the viewport centre
        }
        double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
        for (auto& p : pts) { const Pt q = project(p); x0 = std::min(x0, q.x); x1 = std::max(x1, q.x); y0 = std::min(y0, q.y); y1 = std::max(y1, q.y); }
        const double halfW = std::max(0.5 * (x1 - x0), 1e-6), halfH = std::max(0.5 * (y1 - y0), 1e-6);
        // screen offsets from the target are proportional to the zoom, depths do not depend on it
        zoom = std::min(margin * 0.5 * areaW / halfW, margin * 0.5 * areaH / halfH);
        clampZoom();
        followScene = false;
    }
    // Frame the whole scene (the corners of its bounding box).
    void frameAll(const std::vector<Vec>& corners) { frame(corners); }

    // True when the screen position is inside the viewport.
    bool inside(const Pt& q, double slack = 0.5) const { return std::abs(q.x - areaCx) <= 0.5 * areaW + slack && std::abs(q.y - areaCy) <= 0.5 * areaH + slack; }
};

}  // namespace verbp
