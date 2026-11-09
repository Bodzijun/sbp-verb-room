#pragma once
#include "scene.hpp"

namespace verbp {
// Matches the surface kinds used by the material editor; indices move with their surface metadata.
struct SurfaceRef {
    int kind = 0, index = -1;  // 1 wall, 2 floor, 3 ceiling, 4 floor zone, 5 ceiling zone
    bool operator==(const SurfaceRef& other) const { return kind == other.kind && index == other.index; }
};
inline verb::SurfaceTag* surfaceTag(verb::Scene& s, SurfaceRef r) {
    if (r.kind == 1 && r.index >= 0 && r.index < (int)s.walls.size()) return &s.walls[(size_t)r.index].editor;
    if (r.kind == 2) return &s.floorEditor;
    if (r.kind == 3) return &s.ceilEditor;
    if (r.kind == 4 && r.index >= 0 && r.index < (int)s.floorZones.size()) return &s.floorZones[(size_t)r.index].editor;
    if (r.kind == 5 && r.index >= 0 && r.index < (int)s.ceilZones.size()) return &s.ceilZones[(size_t)r.index].editor;
    return nullptr;
}
inline const verb::SurfaceTag* surfaceTag(const verb::Scene& s, SurfaceRef r) {
    return surfaceTag(const_cast<verb::Scene&>(s), r);
}
inline std::vector<SurfaceRef> surfaceRefs(const verb::Scene& s) {
    std::vector<SurfaceRef> result{{2, -1}, {3, -1}};
    for (size_t i = 0; i < s.walls.size(); ++i) result.push_back({1, (int)i});
    for (size_t i = 0; i < s.floorZones.size(); ++i) result.push_back({4, (int)i});
    for (size_t i = 0; i < s.ceilZones.size(); ++i) result.push_back({5, (int)i});
    return result;
}
inline std::vector<SurfaceRef> expandSurfaceGroups(const verb::Scene& s, const std::vector<SurfaceRef>& input) {
    std::vector<SurfaceRef> result;
    const auto all = surfaceRefs(s);
    auto add = [&](SurfaceRef r) {
        if (surfaceTag(s, r) && std::find(result.begin(), result.end(), r) == result.end()) result.push_back(r);
    };
    for (auto r : input) {
        add(r);
        const auto* tag = surfaceTag(s, r);
        if (tag && !tag->group.empty()) for (auto member : all)
            if (surfaceTag(s, member)->group == tag->group) add(member);
    }
    return result;
}
inline bool surfacesLocked(const verb::Scene& s, const std::vector<SurfaceRef>& refs) {
    for (auto r : refs) if (auto* tag = surfaceTag(s, r)) if (tag->locked) return true;
    return false;
}
inline void translateSurfaces(verb::Scene& s, const verb::Scene& original, const std::vector<SurfaceRef>& refs,
                              double dx, double dy, double dz = 0) {
    if (surfacesLocked(s, refs)) return;  // a linked construction never moves only some of its members
    for (auto r : refs) {
        if (!surfaceTag(s, r) || !surfaceTag(original, r)) continue;
        if (r.kind == 1) {
            auto& wall = s.walls[(size_t)r.index]; const auto& from = original.walls[(size_t)r.index];
            wall.ax = from.ax + dx; wall.ay = from.ay + dy; wall.bx = from.bx + dx; wall.by = from.by + dy;
            wall.z0 = std::max(0.0, from.z0 + dz);
            if (from.z1 < 1e8) wall.z1 = from.z1 + dz;
        } else if (r.kind == 4 || r.kind == 5) {
            auto& zone = (r.kind == 4 ? s.floorZones : s.ceilZones)[(size_t)r.index];
            const auto& from = (r.kind == 4 ? original.floorZones : original.ceilZones)[(size_t)r.index];
            zone.poly = from.poly;
            for (auto& p : zone.poly) { p.first += dx; p.second += dy; }
            if (from.z >= 0) zone.z = std::max(0.0, from.z + dz);
        }
    }
}
}  // namespace verbp
