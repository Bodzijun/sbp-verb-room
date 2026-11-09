// Scene description: 2.5D room (vertical wall segments + constant height), materials, sources, receivers.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace verb {

constexpr double kPi = 3.14159265358979323846;
constexpr int NB = 7;  // octave bands 125 Hz .. 8 kHz
constexpr double kBandHz[NB] = {125, 250, 500, 1000, 2000, 4000, 8000};

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double norm(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 unit(Vec3 a) {
    double n = norm(a);
    return n > 0 ? a * (1.0 / n) : a;
}

struct Material {
    std::string name;
    std::array<double, NB> alpha{};  // energy absorption per octave band
    std::array<double, NB> tau{};    // energy transmission per octave band (0 = opaque, 1 = open); alpha+tau <= 1
    double scatter = 0.05;           // share of reflected energy that is diffuse (at 125 Hz)
    double scatterHf = -1;           // same at 8 kHz; <0: constant. Real surfaces scatter more at high frequencies.
    int cls = 10;                    // surface class (see surfaceClasses()); only used for colouring and grouping, 10 = custom
};

// Scattering coefficient of band b: interpolated over the octave index (= log frequency) between LF and HF.
inline double scatterAt(const Material& m, int b) {
    if (m.scatterHf < 0) return m.scatter;
    return m.scatter + (m.scatterHf - m.scatter) * ((double)b / (double)(NB - 1));
}

// Reflected energy share of band b: what is neither absorbed nor transmitted.
inline double reflEnergy(const Material& m, int b) { return std::max(0.0, 1.0 - m.alpha[b] - m.tau[b]); }

inline double tlToTau(double tlDb) { return std::pow(10.0, -std::max(0.0, tlDb) / 10.0); }
inline double tauToTl(double tau) { return tau <= 1e-12 ? 99.0 : std::min(99.0, -10.0 * std::log10(tau)); }

// Surface classes: group the presets, and give every class its own colour on the map.
enum SurfClass { SC_Masonry = 0, SC_Glass, SC_Wood, SC_Partition, SC_Fabric, SC_Absorber, SC_Metal, SC_Opening, SC_Outdoor, SC_Furniture, SC_Custom, SC_Count };
struct ClassDef {
    const char* name;
    unsigned argb;
};
inline const std::vector<ClassDef>& surfaceClasses() {
    static const std::vector<ClassDef> c = {
        {"Hard shell (masonry, stone)", 0xffd9b38c}, {"Glass", 0xff6fd3ff},          {"Wood", 0xffc88a4a},
        {"Light partition", 0xffb9b9e0},             {"Soft fabric / carpet", 0xffb77bd6}, {"Absorber / treatment", 0xff6fe08a},
        {"Metal", 0xff9fb4c7},                       {"Opening / screen", 0xffffffff}, {"Outdoor / ground", 0xffa8c64f},
        {"Furniture / objects", 0xfff08bb4},         {"Custom", 0xffe8e8e8}};
    return c;
}

// Built-in surface library: typical octave-band absorption (125 Hz .. 8 kHz), transmission loss in dB (99 =
// opaque), scattering at LF/HF. Values are textbook-style approximations: a starting point to tune from.
inline const std::vector<Material>& surfacePresets() {
    static const std::vector<Material> p = [] {
        auto mk = [](const char* n, int cls, std::array<double, NB> a, std::array<double, NB> tl, double s0, double s1) {
            Material m;
            m.name = n; m.cls = cls; m.alpha = a; m.scatter = s0; m.scatterHf = s1;
            for (int b = 0; b < NB; ++b) {
                m.tau[b] = tl[(size_t)b] >= 90 ? 0.0 : tlToTau(tl[(size_t)b]);
                if (m.alpha[b] + m.tau[b] > 1.0) m.alpha[b] = 1.0 - m.tau[b];
            }
            return m;
        };
        const double X = 99;  // opaque
        const std::array<double, NB> OP = {X, X, X, X, X, X, X};
        std::vector<Material> v;
        // -- keep the first three (Concrete, Brick, Plaster) where they are: code refers to them by position --
        v.push_back(mk("Concrete",           SC_Masonry, {.01, .01, .02, .02, .02, .03, .04}, OP, .05, .30));
        v.push_back(mk("Brick",              SC_Masonry, {.03, .03, .03, .04, .05, .07, .07}, OP, .10, .50));
        v.push_back(mk("Plaster",            SC_Masonry, {.02, .03, .04, .05, .06, .07, .08}, OP, .05, .40));
        v.push_back(mk("Stone_marble",       SC_Masonry, {.01, .01, .01, .02, .02, .02, .03}, OP, .02, .15));
        v.push_back(mk("Ceramic_tiles",      SC_Masonry, {.01, .01, .01, .01, .02, .02, .02}, OP, .02, .10));
        v.push_back(mk("Rough_rock",         SC_Masonry, {.02, .03, .03, .04, .05, .05, .06}, OP, .30, .80));
        v.push_back(mk("Glass_window",       SC_Glass,   {.18, .06, .04, .03, .02, .02, .02}, {10, 13, 16, 19, 22, 24, 25}, .02, .05));
        v.push_back(mk("Double_glazing",     SC_Glass,   {.15, .05, .03, .03, .02, .02, .02}, {18, 22, 26, 30, 33, 36, 38}, .02, .05));
        v.push_back(mk("Thin_acrylic",       SC_Glass,   {.10, .06, .04, .03, .03, .03, .03}, {6, 8, 10, 12, 14, 15, 15}, .02, .06));
        v.push_back(mk("Wood_panel",         SC_Wood,    {.15, .11, .10, .07, .06, .07, .07}, {14, 17, 20, 23, 26, 28, 30}, .10, .40));
        v.push_back(mk("Parquet_floor",      SC_Wood,    {.15, .11, .10, .07, .06, .07, .07}, OP, .05, .30));
        v.push_back(mk("Wooden_door",        SC_Wood,    {.14, .10, .06, .08, .10, .10, .10}, {12, 15, 17, 19, 21, 22, 22}, .10, .30));
        v.push_back(mk("Plywood_thin",       SC_Wood,    {.28, .22, .17, .09, .10, .11, .11}, {8, 10, 12, 14, 16, 18, 18}, .10, .35));
        v.push_back(mk("Drywall_partition",  SC_Partition, {.15, .10, .06, .05, .05, .06, .08}, {15, 18, 22, 26, 30, 33, 35}, .05, .40));
        v.push_back(mk("Double_drywall",     SC_Partition, {.15, .10, .06, .05, .05, .06, .08}, {24, 28, 32, 37, 41, 44, 46}, .05, .40));
        v.push_back(mk("Light_screen",       SC_Partition, {.10, .12, .15, .15, .15, .15, .15}, {4, 6, 8, 9, 10, 11, 11}, .10, .50));
        v.push_back(mk("Thin_curtain",       SC_Fabric,  {.03, .04, .11, .17, .24, .35, .35}, {2, 2.5, 3, 3.5, 4, 4, 4}, .15, .50));
        v.push_back(mk("Heavy_curtain",      SC_Fabric,  {.10, .30, .55, .70, .75, .75, .70}, {3, 4.5, 6, 7.5, 8.5, 8.5, 8.5}, .20, .60));
        v.push_back(mk("Velvet_drapes",      SC_Fabric,  {.14, .35, .55, .72, .70, .65, .60}, {6, 8, 10, 12, 13, 13, 13}, .20, .60));
        v.push_back(mk("Carpet_thin",        SC_Fabric,  {.02, .03, .05, .10, .30, .50, .60}, OP, .15, .50));
        v.push_back(mk("Carpet",             SC_Fabric,  {.05, .10, .25, .45, .65, .70, .70}, OP, .20, .60));
        v.push_back(mk("Upholstery_sofa",    SC_Fabric,  {.20, .35, .45, .55, .60, .60, .55}, OP, .40, .80));
        v.push_back(mk("Audience_seated",    SC_Fabric,  {.30, .45, .60, .75, .85, .85, .80}, {3, 4, 5, 6, 7, 7, 7}, .50, .80));
        v.push_back(mk("Acoustic_panel",     SC_Absorber, {.20, .55, .90, .95, .95, .95, .90}, OP, .10, .30));
        v.push_back(mk("Mineral_wool_100mm", SC_Absorber, {.35, .80, .98, .99, .98, .96, .94}, OP, .05, .20));
        v.push_back(mk("Foam_50mm",          SC_Absorber, {.08, .20, .55, .85, .95, .95, .90}, OP, .05, .20));
        v.push_back(mk("Bass_trap",          SC_Absorber, {.85, .95, .75, .45, .30, .25, .20}, OP, .10, .30));
        v.push_back(mk("Ceiling_tiles",      SC_Absorber, {.10, .30, .55, .70, .75, .75, .70}, OP, .10, .50));
        v.push_back(mk("Diffuser",           SC_Absorber, {.10, .15, .20, .25, .20, .15, .15}, OP, .30, .90));
        v.push_back(mk("Absorber",           SC_Absorber, {1, 1, 1, 1, 1, 1, 1}, OP, .00, .00));
        v.push_back(mk("Metal",              SC_Metal,   {.01, .01, .02, .02, .03, .03, .03}, OP, .02, .10));
        v.push_back(mk("Metal_door",         SC_Metal,   {.02, .02, .03, .03, .04, .04, .04}, {20, 24, 28, 32, 35, 37, 38}, .02, .10));
        v.push_back(mk("Open",               SC_Opening, {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}, .00, .00));  // doorway / opening
        v.push_back(mk("Mesh_screen",        SC_Opening, {.02, .02, .03, .04, .05, .06, .06}, {.5, .5, .7, 1, 1.5, 2, 2.5}, .10, .50));
        v.push_back(mk("Fence_railing",      SC_Opening, {.02, .02, .02, .03, .03, .04, .04}, {.3, .3, .4, .5, .6, .8, 1}, .20, .60));
        v.push_back(mk("Grass",              SC_Outdoor, {.11, .26, .60, .69, .92, .99, .99}, OP, .20, .60));
        v.push_back(mk("Soil",               SC_Outdoor, {.15, .25, .40, .55, .60, .60, .60}, OP, .20, .50));
        v.push_back(mk("Asphalt",            SC_Outdoor, {.02, .03, .03, .03, .04, .05, .05}, OP, .05, .20));
        v.push_back(mk("Hedge_foliage",      SC_Outdoor, {.08, .15, .30, .45, .60, .70, .75}, {2, 2.5, 3, 4, 5, 6, 7}, .50, .90));
        v.push_back(mk("Snow",               SC_Outdoor, {.45, .75, .90, .95, .95, .95, .95}, OP, .10, .30));
        v.push_back(mk("Water",              SC_Outdoor, {.01, .01, .01, .02, .02, .03, .04}, OP, .02, .15));
        v.push_back(mk("Bookshelf",          SC_Furniture, {.30, .40, .35, .30, .30, .30, .30}, {10, 12, 14, 16, 18, 20, 20}, .50, .90));
        v.push_back(mk("Table_desk",         SC_Furniture, {.10, .07, .05, .04, .04, .04, .05}, {10, 12, 14, 16, 18, 20, 22}, .30, .70));
        v.push_back(mk("Wardrobe_cabinet",   SC_Furniture, {.25, .20, .15, .12, .10, .10, .10}, {14, 16, 18, 20, 22, 24, 24}, .30, .70));
        v.push_back(mk("Plants",             SC_Furniture, {.08, .12, .20, .30, .40, .45, .45}, {3, 3, 4, 4, 5, 5, 5}, .60, .90));
        v.push_back(mk("Furniture_clutter",  SC_Furniture, {.20, .25, .30, .35, .35, .35, .35}, {4, 5, 6, 7, 8, 9, 9}, .50, .90));
        return v;
    }();
    return p;
}

inline int presetIndexByName(const std::string& n) {
    const auto& p = surfacePresets();
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i].name == n) return (int)i;
    return -1;
}
inline const Material& presetByName(const std::string& n) {
    const int i = presetIndexByName(n);
    return surfacePresets()[(size_t)(i < 0 ? 0 : i)];
}

struct Wall {  // vertical rectangle over the 2D segment a-b, from z0 up to z1 (default: floor to ceiling)
    double ax, ay, bx, by;
    int mat;
    double z0 = 0;    // bottom edge, metres above the floor
    double z1 = 1e9;  // top edge; >= the ceiling height means a full-height wall
};

// A region of the floor or the ceiling with its own material (a carpet here, tiles there, a suspended ceiling over part of
// the room). Where zones overlap the later one wins; outside every zone the plain floorMat/ceilMat applies. A ceiling
// zone with mat < 0 is an opening (skylight, void).
struct Zone {
    std::vector<std::pair<double, double>> poly;  // x, y vertices, at least 3
    int mat = 0;
    // z < 0: a material patch of the plain floor / ceiling. z >= 0: a horizontal plate at that height (a lowered
    // ceiling such as a bonnet, a raised platform, a mezzanine): it reflects from both sides and, if its material lets
    // sound through (transmission loss), passes some of it. Plates are separate surfaces for the reflection tree.
    double z = -1;
};
constexpr int kMaxPlates = 8;

struct Transducer {  // source or receiver
    std::string name;
    Vec3 pos;
    double azimuthDeg = 0;  // counter-clockwise from +x
    double elevationDeg = 0;  // above the horizontal plane (microphones of height channels)
    double pattern = 0;     // 0 omni, 0.5 cardioid, 1 figure-8 (at 125 Hz when patternHf is set)
    double patternHf = -1;  // pattern at 8 kHz, interpolated over the octave bands; < 0: the same at all frequencies (head shadow grows with frequency)
    double gainDb = 0;
};

// "Fullness" of the space: seeded random short wall segments (furniture, objects) that are only half obstacles:
// each absorbs a little, lets part of the sound through (random transmission loss in [tlMin, tlMax] dB) and
// scatters strongly. They make the early field and the tail less like the response of an empty box.
struct ClutterCfg {
    int count = 0;           // 0 = off
    double minLen = 0.4, maxLen = 1.6;  // segment length, metres
    double tlMin = 2.0, tlMax = 10.0;   // transmission loss range, dB
    double hMin = 0.5, hMax = 1.8;      // height of the objects above the floor, metres (sound passes over them)
    double minDist = 0.5;    // keep clear of sources and receivers
    int seed = 1;
};

struct Scene {
    double sr = 48000;
    double length = 2.0;  // rendered IR length, seconds
    double height = 3.0;  // ceiling height (floor is z=0)
    double tempC = 20, humidity = 50;
    ClutterCfg clutter;
    bool diffraction = true;  // sound bends round wall ends (soft shadow edges, doors) instead of being cut off
    double roughness = 0;  // 0..1: seeded random variation of every surface's absorption and scattering (see withRoughness)
    int roughSeed = 1;
    int order = 3;       // image-source order
    int rays = 50000;
    int seed = 1;
    double detectRadius = 0.5;
    bool hasListener = false;         // optional anchor of the receiver array (default: centroid, yaw 0)
    Vec3 listenerPos;
    double listenerYaw = 0;           // degrees, counter-clockwise from +x
    int floorMat = -1, ceilMat = -1;  // ceilMat<0 => open sky
    std::vector<Material> mats;
    std::vector<Wall> walls;
    std::vector<Zone> floorZones, ceilZones;
    std::vector<Transducer> sources, receivers;
};

inline bool pointInZone(const Zone& z, double x, double y) {  // even-odd rule
    bool in = false;
    const size_t n = z.poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const auto &a = z.poly[i], &b = z.poly[j];
        if ((a.second > y) != (b.second > y) && x < (b.first - a.first) * (y - a.second) / (b.second - a.second) + a.first) in = !in;
    }
    return in;
}

// Index of the zone that decides the material at (x, y), or -1 for the plain floor / ceiling.
inline int zoneAt(const std::vector<Zone>& zones, double x, double y) {
    for (int i = (int)zones.size() - 1; i >= 0; --i)
        if (zones[(size_t)i].z < 0 && zones[(size_t)i].poly.size() >= 3 && pointInZone(zones[(size_t)i], x, y)) return i;
    return -1;
}

inline int floorMatAt(const Scene& s, double x, double y) {
    const int z = zoneAt(s.floorZones, x, y);
    return z >= 0 ? s.floorZones[(size_t)z].mat : s.floorMat;
}
inline int ceilMatAt(const Scene& s, double x, double y) {  // < 0: open
    const int z = zoneAt(s.ceilZones, x, y);
    return z >= 0 ? s.ceilZones[(size_t)z].mat : s.ceilMat;
}

inline double speedOfSound(double tempC) { return 343.2 * std::sqrt((tempC + 273.15) / 293.15); }

// ISO 9613-1 pure-tone air absorption, dB/m.
inline double airAbsDbPerM(double f, double tempC, double rh) {
    const double T = tempC + 273.15, T0 = 293.15, T01 = 273.16, pa = 101.325, pr = 101.325;
    double C = -6.8346 * std::pow(T01 / T, 1.261) + 4.6151;
    double h = rh * (pr * std::pow(10.0, C)) / pa;
    double frO = (pa / pr) * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h));
    double frN = (pa / pr) * std::pow(T / T0, -0.5) *
                 (9.0 + 280.0 * h * std::exp(-4.170 * (std::pow(T / T0, -1.0 / 3.0) - 1.0)));
    double f2 = f * f;
    return 8.686 * f2 *
           (1.84e-11 * (pr / pa) * std::sqrt(T / T0) +
            std::pow(T / T0, -2.5) * (0.01275 * std::exp(-2239.1 / T) / (frO + f2 / frO) +
                                      0.1068 * std::exp(-3352.0 / T) / (frN + f2 / frN)));
}

inline double finiteOr(double v, double def) { return std::isfinite(v) ? v : def; }
inline double clampTo(double v, double lo, double hi, double def) { return std::min(std::max(finiteOr(v, def), lo), hi); }

// Makes any scene safe for the engine: finite, in range, no degenerate walls, consistent materials. Used for every
// scene that comes from outside (files, project state) and after every edit.
inline void sanitizeScene(Scene& s) {
    s.length = clampTo(s.length, 0.2, 20.0, 2.0);
    s.height = clampTo(s.height, 0.3, 100.0, 3.0);
    s.tempC = clampTo(s.tempC, -60.0, 70.0, 20.0);
    s.humidity = clampTo(s.humidity, 0.0, 100.0, 50.0);
    s.detectRadius = clampTo(s.detectRadius, 0.05, 5.0, 0.5);
    s.order = std::min(std::max(s.order, 0), 4);
    s.rays = std::min(std::max(s.rays, 100), 10000000);
    s.listenerPos = {clampTo(s.listenerPos.x, -1e5, 1e5, 0), clampTo(s.listenerPos.y, -1e5, 1e5, 0), clampTo(s.listenerPos.z, 0, 1e3, 1.5)};
    s.listenerYaw = finiteOr(s.listenerYaw, 0);
    s.roughness = clampTo(s.roughness, 0.0, 1.0, 0.0);
    s.roughSeed = std::min(std::max(s.roughSeed, -1000000), 1000000);
    ClutterCfg& c = s.clutter;
    c.count = std::min(std::max(c.count, 0), 1000);
    c.minLen = clampTo(c.minLen, 0.05, 20.0, 0.4);
    c.maxLen = std::max(c.minLen, clampTo(c.maxLen, 0.05, 40.0, 1.6));
    c.tlMin = clampTo(c.tlMin, 0.0, 60.0, 2.0);
    c.tlMax = std::max(c.tlMin, clampTo(c.tlMax, 0.0, 60.0, 10.0));
    c.hMin = clampTo(c.hMin, 0.05, 30.0, 0.5);
    c.hMax = std::max(c.hMin, clampTo(c.hMax, 0.05, 60.0, 1.8));
    c.minDist = clampTo(c.minDist, 0.0, 10.0, 0.5);
    for (auto& m : s.mats) {
        for (int b = 0; b < NB; ++b) {
            m.alpha[(size_t)b] = clampTo(m.alpha[(size_t)b], 0.0, 1.0, 0.1);
            m.tau[(size_t)b] = clampTo(m.tau[(size_t)b], 0.0, 1.0, 0.0);
            if (m.alpha[(size_t)b] + m.tau[(size_t)b] > 1.0) m.alpha[(size_t)b] = 1.0 - m.tau[(size_t)b];
        }
        m.scatter = clampTo(m.scatter, 0.0, 1.0, 0.05);
        if (m.scatterHf >= 0 || !std::isfinite(m.scatterHf)) m.scatterHf = clampTo(m.scatterHf, 0.0, 1.0, m.scatter);
        m.cls = std::min(std::max(m.cls, 0), (int)SC_Custom);
    }
    s.floorMat = s.floorMat < (int)s.mats.size() ? s.floorMat : -1;
    s.ceilMat = s.ceilMat < (int)s.mats.size() ? s.ceilMat : -1;
    std::vector<Wall> keep;
    for (Wall w : s.walls) {
        const bool finite = std::isfinite(w.ax) && std::isfinite(w.ay) && std::isfinite(w.bx) && std::isfinite(w.by);
        if (!finite || std::fabs(w.ax) > 1e5 || std::fabs(w.ay) > 1e5 || std::fabs(w.bx) > 1e5 || std::fabs(w.by) > 1e5) continue;
        if (std::hypot(w.bx - w.ax, w.by - w.ay) < 1e-4) continue;  // a zero-length wall has no direction: it would poison the maths
        if (w.mat < 0 || w.mat >= (int)s.mats.size()) continue;
        w.z0 = clampTo(w.z0, 0.0, 1e6, 0.0);
        w.z1 = std::isnan(w.z1) ? 1e9 : std::max(w.z0 + 0.01, std::min(w.z1, 1e9));
        keep.push_back(w);
    }
    s.walls = keep;
    for (int ceiling = 0; ceiling < 2; ++ceiling) {  // zones: finite vertices, a valid material, 3..64 corners, at most 32 per surface
        auto& zones = ceiling ? s.ceilZones : s.floorZones;
        std::vector<Zone> ok;
        for (auto& z : zones) {
            if (z.poly.size() < 3 || z.poly.size() > 64 || ok.size() >= 32) continue;
            if (z.mat >= (int)s.mats.size() || (z.mat < 0 && !ceiling)) continue;
            bool fin = true;
            for (auto& p : z.poly) fin &= std::isfinite(p.first) && std::isfinite(p.second) && std::fabs(p.first) < 1e5 && std::fabs(p.second) < 1e5;
            if (!fin) continue;
            z.z = std::isfinite(z.z) && z.z >= 0 ? std::min(z.z, 100.0) : -1.0;
            if (z.mat < 0 || (!ceiling && z.z <= 1e-6)) z.z = -1;  // an opening or a floor "plate" at 0 is a plain zone
            ok.push_back(z);
        }
        zones = ok;

    }
    {   // at most kMaxPlates plates in all; the rest become plain patches
        int cnt = 0;
        for (auto* zl : {&s.floorZones, &s.ceilZones})
            for (auto& z : *zl)
                if (z.z >= 0 && ++cnt > kMaxPlates) z.z = -1;    }
    for (auto* list : {&s.sources, &s.receivers})
        for (auto& t : *list) {
            t.pos = {clampTo(t.pos.x, -1e5, 1e5, 0), clampTo(t.pos.y, -1e5, 1e5, 0), clampTo(t.pos.z, -1e3, 1e3, 1.5)};
            t.azimuthDeg = finiteOr(t.azimuthDeg, 0);
            t.elevationDeg = clampTo(t.elevationDeg, -90, 90, 0);
            t.pattern = clampTo(t.pattern, 0.0, 1.0, 0.0);
            t.patternHf = (std::isfinite(t.patternHf) && t.patternHf >= 0) ? std::min(t.patternHf, 1.0) : -1.0;
            t.gainDb = clampTo(t.gainDb, -120.0, 60.0, 0.0);
        }
}

// `path` is only used in error messages.
inline Scene parseScene(std::istream& in, const std::string& path) {
    Scene s;
    std::map<std::string, int> matIdx;
    std::string line;
    int ln = 0;
    auto fail = [&](const std::string& m) { throw std::runtime_error(path + ":" + std::to_string(ln) + ": " + m); };
    auto findMat = [&](const std::string& n) {
        auto it = matIdx.find(n);
        if (it == matIdx.end()) fail("unknown material '" + n + "'");
        return it->second;
    };
    while (std::getline(in, line)) {
        ++ln;
        auto h = line.find('#');
        if (h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        std::string cmd;
        if (!(ss >> cmd)) continue;
        auto num = [&]() {
            double v;
            if (!(ss >> v)) fail("expected number for '" + cmd + "'");
            return v;
        };
        if (cmd == "sr") s.sr = num();
        else if (cmd == "length") s.length = num();
        else if (cmd == "height") s.height = num();
        else if (cmd == "temp") s.tempC = num();
        else if (cmd == "humidity") s.humidity = num();
        else if (cmd == "order") s.order = (int)num();
        else if (cmd == "rays") s.rays = (int)num();
        else if (cmd == "seed") s.seed = (int)num();
        else if (cmd == "detect_radius") s.detectRadius = num();
        else if (cmd == "listener") {
            s.hasListener = true;
            s.listenerPos.x = num(); s.listenerPos.y = num(); s.listenerPos.z = num();
            s.listenerYaw = num();
        }
        else if (cmd == "material") {
            Material m;
            ss >> m.name;
            for (int b = 0; b < NB; ++b) m.alpha[b] = num();
            double sc;
            if (ss >> sc) {
                m.scatter = sc;
                if (ss >> sc) m.scatterHf = sc;  // optional: scatter at 8 kHz
            }
            matIdx[m.name] = (int)s.mats.size();
            s.mats.push_back(m);
        } else if (cmd == "transmit") {  // transmit <material> <TL dB x7>: sound passes through with this loss
            std::string n;
            ss >> n;
            Material& m = s.mats[(size_t)findMat(n)];
            for (int b = 0; b < NB; ++b) m.tau[b] = tlToTau(num());
        } else if (cmd == "class") {  // class <material> <class index>
            std::string n;
            ss >> n;
            s.mats[(size_t)findMat(n)].cls = std::min(std::max((int)num(), 0), (int)SC_Custom);
        } else if (cmd == "clutter") {  // clutter <count> [minLen maxLen tlMin tlMax seed]
            s.clutter.count = std::max(0, (int)num());
            double v;
            if (ss >> v) s.clutter.minLen = v;
            if (ss >> v) s.clutter.maxLen = v;
            if (ss >> v) s.clutter.tlMin = v;
            if (ss >> v) s.clutter.tlMax = v;
            if (ss >> v) s.clutter.seed = (int)v;
            if (ss >> v) s.clutter.hMin = v;
            if (ss >> v) s.clutter.hMax = v;
        } else if (cmd == "zone") {  // zone floor|ceiling <material|open> x1 y1 x2 y2 x3 y3 ...
            std::string kind, mn;
            ss >> kind >> mn;
            if (kind != "floor" && kind != "ceiling") fail("zone must be 'floor' or 'ceiling'");
            Zone z;
            z.mat = (kind == "ceiling" && mn == "open") ? -1 : findMat(mn);
            {   // optional "at <height>": a plate instead of a material patch
                const std::streampos here = ss.tellg();
                std::string kw;
                if ((ss >> kw) && kw == "at") ss >> z.z;
                else { ss.clear(); ss.seekg(here); }
            }
            double x, y;
            while (ss >> x >> y) z.poly.push_back({x, y});
            if (z.poly.size() < 3) fail("a zone needs at least 3 corners");
            (kind == "floor" ? s.floorZones : s.ceilZones).push_back(z);
        } else if (cmd == "diffraction") {  // diffraction <0|1>
            s.diffraction = num() != 0;
        } else if (cmd == "roughness") {  // roughness <0..1> [seed]
            s.roughness = num();
            double v;
            if (ss >> v) s.roughSeed = (int)v;
        } else if (cmd == "floor" || cmd == "ceiling") {
            std::string n;
            ss >> n;
            if (cmd == "ceiling" && n == "open") s.ceilMat = -1;
            else (cmd == "floor" ? s.floorMat : s.ceilMat) = findMat(n);
        } else if (cmd == "wall") {
            Wall w;
            w.ax = num(); w.ay = num(); w.bx = num(); w.by = num();
            std::string n;
            ss >> n;
            w.mat = findMat(n);
            double zv;
            if (ss >> zv) w.z0 = zv;  // optional: bottom and top edge in metres
            if (ss >> zv) w.z1 = zv;
            s.walls.push_back(w);
        } else if (cmd == "source" || cmd == "receiver") {
            Transducer t;
            ss >> t.name;
            t.pos.x = num(); t.pos.y = num(); t.pos.z = num();
            double v;
            if (ss >> v) t.azimuthDeg = v;
            if (ss >> v) t.pattern = v;
            if (ss >> v) t.gainDb = v;
            if (ss >> v) t.elevationDeg = v;
            (cmd == "source" ? s.sources : s.receivers).push_back(t);
        } else fail("unknown command '" + cmd + "'");
    }
    if (s.floorMat < 0) throw std::runtime_error("scene needs a 'floor <material>' line");
    if (s.sources.empty() || s.receivers.empty()) throw std::runtime_error("scene needs at least one source and one receiver");
    sanitizeScene(s);
    if (s.floorMat < 0) throw std::runtime_error("scene needs a valid floor material");
    return s;
}

// Roughness: no two real surfaces are alike. Every wall, the floor and the ceiling get their own copy of their
// material whose absorption (a random level and a random tilt over frequency) and scattering (more, mostly at
// high frequencies) are varied by a seeded random amount scaled by `roughness`. Deterministic for a given seed.
inline Scene withRoughness(const Scene& s) {
    if (s.roughness <= 0) return s;
    Scene out = s;
    const double r = s.roughness;
    std::mt19937_64 rng((uint64_t)(int64_t)(s.roughSeed + 1000001) * 0x9E3779B97F4A7C15ull + 7ull);
    auto gauss = [&]() { return std::normal_distribution<double>(0.0, 1.0)(rng); };
    auto U01 = [&]() { return std::uniform_real_distribution<double>(0, 1)(rng); };
    auto roughen = [&](int& idx) {
        if (idx < 0 || idx >= (int)s.mats.size()) return;
        Material m = s.mats[(size_t)idx];
        const double level = std::max(-1.5, std::min(1.5, gauss())), tilt = std::max(-1.5, std::min(1.5, gauss()));
        for (int b = 0; b < NB; ++b) {
            const double f = std::exp(r * 0.7 * (level + tilt * ((double)b / (NB - 1) - 0.5) * 2.0));
            m.alpha[(size_t)b] = std::min(0.98, m.alpha[(size_t)b] * f);
            if (m.alpha[(size_t)b] + m.tau[(size_t)b] > 1.0) m.alpha[(size_t)b] = 1.0 - m.tau[(size_t)b];
        }
        const double lf = m.scatter, hf = m.scatterHf < 0 ? m.scatter : m.scatterHf;
        m.scatter = std::min(0.9, lf + (0.9 - lf) * r * 0.5 * U01());
        m.scatterHf = std::min(0.95, std::max(m.scatter, hf + (0.95 - hf) * r * 0.9 * (0.4 + 0.6 * U01())));
        out.mats.push_back(m);
        idx = (int)out.mats.size() - 1;
    };
    for (auto& w : out.walls) roughen(w.mat);
    roughen(out.floorMat);
    roughen(out.ceilMat);
    for (auto& z : out.floorZones) roughen(z.mat);
    for (auto& z : out.ceilZones) roughen(z.mat);
    return out;
}

// The scene with its clutter segments added (deterministic for a given seed and scene). `maxTotalWalls` caps
// the wall count (the real-time engine has a fixed capacity).
inline Scene withClutter(const Scene& s, int maxTotalWalls = 1 << 20) {
    if (s.clutter.count <= 0 || s.walls.empty()) return s;
    Scene out = s;
    const ClutterCfg& c = s.clutter;
    double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    for (const auto& w : s.walls) {
        x0 = std::min({x0, w.ax, w.bx}); x1 = std::max({x1, w.ax, w.bx});
        y0 = std::min({y0, w.ay, w.by}); y1 = std::max({y1, w.ay, w.by});
    }
    auto inside = [&](double px, double py) {  // even-odd rule against all wall segments
        int cnt = 0;
        for (const auto& w : s.walls)
            if ((w.ay > py) != (w.by > py) && w.ax + (py - w.ay) * (w.bx - w.ax) / (w.by - w.ay) > px) ++cnt;
        return (cnt & 1) != 0;
    };
    auto distWall = [&](double px, double py) {
        double best = 1e9;
        for (const auto& w : s.walls) {
            const double dx = w.bx - w.ax, dy = w.by - w.ay, l2 = dx * dx + dy * dy;
            double t = l2 > 0 ? ((px - w.ax) * dx + (py - w.ay) * dy) / l2 : 0;
            t = std::min(1.0, std::max(0.0, t));
            best = std::min(best, std::hypot(px - (w.ax + t * dx), py - (w.ay + t * dy)));
        }
        return best;
    };
    std::mt19937_64 rng((uint64_t)c.seed * 2654435761ull + 12345ull);
    auto U = [&](double a, double b) { return a + (b - a) * std::uniform_real_distribution<double>(0, 1)(rng); };
    // a closed outline makes "inside" meaningful; for open scenes anything inside the bounding box is allowed
    int hits = 0;
    for (int i = 0; i < 200; ++i) hits += inside(U(x0, x1), U(y0, y1));
    const bool useInside = hits >= 10;
    const int room = std::max(0, maxTotalWalls - (int)s.walls.size());
    const int want = std::min(c.count, room);
    auto clear = [&](double px, double py) {
        for (const auto& t : s.sources) if (std::hypot(px - t.pos.x, py - t.pos.y) < c.minDist) return false;
        for (const auto& t : s.receivers) if (std::hypot(px - t.pos.x, py - t.pos.y) < c.minDist) return false;
        return true;
    };
    int made = 0;
    for (int tries = 0; made < want && tries < want * 80; ++tries) {
        const double px = U(x0, x1), py = U(y0, y1);
        if (useInside && !inside(px, py)) continue;
        if (distWall(px, py) < 0.25 || !clear(px, py)) continue;
        const double len = U(c.minLen, std::max(c.minLen, c.maxLen)), ang = U(0, kPi), hx = 0.5 * len * std::cos(ang), hy = 0.5 * len * std::sin(ang);
        const double ax = px - hx, ay = py - hy, bx = px + hx, by = py + hy;
        if (useInside && (!inside(ax, ay) || !inside(bx, by))) continue;
        if (distWall(ax, ay) < 0.1 || distWall(bx, by) < 0.1) continue;
        Material m = presetByName("Furniture_clutter");
        m.name = "Clutter";
        m.cls = SC_Furniture;
        const double tl0 = U(c.tlMin, std::max(c.tlMin, c.tlMax));
        for (int b = 0; b < NB; ++b) {
            m.alpha[(size_t)b] = std::min(0.95, m.alpha[(size_t)b] * U(0.7, 1.3));
            m.tau[(size_t)b] = tlToTau(tl0 + 0.5 * b);
            if (m.alpha[(size_t)b] + m.tau[(size_t)b] > 1.0) m.alpha[(size_t)b] = 1.0 - m.tau[(size_t)b];
        }
        m.scatter = U(0.3, 0.7);
        m.scatterHf = U(0.7, 0.95);
        out.mats.push_back(m);
        // objects stand on the floor and have their own height: higher sources/listeners hear straight over them
        out.walls.push_back(Wall{ax, ay, bx, by, (int)out.mats.size() - 1, 0.0, U(c.hMin, std::max(c.hMin, c.hMax))});
        ++made;
    }
    return out;
}

// Drops materials no wall/floor/ceiling uses and renumbers the rest.
inline void compactMaterials(Scene& s) {
    std::vector<int> remap(s.mats.size(), -1);
    std::vector<Material> keep;
    auto take = [&](int& idx) {
        if (idx < 0 || idx >= (int)s.mats.size()) return;
        if (remap[(size_t)idx] < 0) { remap[(size_t)idx] = (int)keep.size(); keep.push_back(s.mats[(size_t)idx]); }
        idx = remap[(size_t)idx];
    };
    for (auto& w : s.walls) take(w.mat);
    take(s.floorMat);
    take(s.ceilMat);
    for (auto& z : s.floorZones) take(z.mat);
    for (auto& z : s.ceilZones) take(z.mat);
    s.mats = keep;
}

// Text form of a scene that parseScene() reads back (every referenced material is written once; names are tokens).
inline std::string serializeScene(const Scene& s) {
    std::ostringstream o;
    o.precision(6);
    auto tok = [](std::string n) {
        for (char& c : n) if (c == ' ' || c == '\t' || c == '#') c = '_';
        return n.empty() ? std::string("mat") : n;
    };
    o << "sr " << s.sr << "\nlength " << s.length << "\nheight " << s.height << "\ntemp " << s.tempC << "\nhumidity " << s.humidity
      << "\norder " << s.order << "\nrays " << s.rays << "\nseed " << s.seed << "\ndetect_radius " << s.detectRadius << "\n";
    std::vector<int> used;
    auto use = [&](int m) {
        if (m >= 0 && std::find(used.begin(), used.end(), m) == used.end()) used.push_back(m);
    };
    for (auto& w : s.walls) use(w.mat);
    use(s.floorMat);
    use(s.ceilMat);
    for (auto& z : s.floorZones) use(z.mat);
    for (auto& z : s.ceilZones) use(z.mat);
    std::map<int, std::string> name;
    for (int m : used) {
        std::string n = tok(s.mats[(size_t)m].name);
        for (auto& kv : name) if (kv.second == n) n += "_" + std::to_string(m);
        name[m] = n;
        const Material& mt = s.mats[(size_t)m];
        o << "material " << n;
        for (int b = 0; b < NB; ++b) o << ' ' << mt.alpha[b];
        o << ' ' << mt.scatter << ' ' << (mt.scatterHf < 0 ? mt.scatter : mt.scatterHf) << "\n";
        o << "class " << n << ' ' << mt.cls << "\n";
        bool any = false;
        for (int b = 0; b < NB; ++b) any |= mt.tau[b] > 0;
        if (any) {
            o << "transmit " << n;
            for (int b = 0; b < NB; ++b) o << ' ' << tauToTl(mt.tau[b]);
            o << "\n";
        }
    }
    if (s.clutter.count > 0)
        o << "clutter " << s.clutter.count << ' ' << s.clutter.minLen << ' ' << s.clutter.maxLen << ' ' << s.clutter.tlMin << ' ' << s.clutter.tlMax << ' ' << s.clutter.seed << ' '
          << s.clutter.hMin << ' ' << s.clutter.hMax << "\n";
    if (!s.diffraction) o << "diffraction 0\n";
    if (s.roughness > 0) o << "roughness " << s.roughness << ' ' << s.roughSeed << "\n";
    o << "floor " << name[s.floorMat] << "\nceiling " << (s.ceilMat < 0 ? std::string("open") : name[s.ceilMat]) << "\n";
    for (int ceiling = 0; ceiling < 2; ++ceiling)
        for (auto& z : ceiling ? s.ceilZones : s.floorZones) {
            o << "zone " << (ceiling ? "ceiling " : "floor ") << (z.mat < 0 ? std::string("open") : name[z.mat]);
            if (z.z >= 0) o << " at " << z.z;
            for (auto& p : z.poly) o << ' ' << p.first << ' ' << p.second;
            o << "\n";
        }
    for (auto& w : s.walls) {
        o << "wall " << w.ax << ' ' << w.ay << ' ' << w.bx << ' ' << w.by << ' ' << name[w.mat];
        if (w.z0 != 0 || w.z1 < 1e8) o << ' ' << w.z0 << ' ' << std::min(w.z1, 1e6);  // only when the wall is not full height
        o << "\n";
    }
    if (s.hasListener) o << "listener " << s.listenerPos.x << ' ' << s.listenerPos.y << ' ' << s.listenerPos.z << ' ' << s.listenerYaw << "\n";
    for (auto& t : s.sources) o << "source " << tok(t.name) << ' ' << t.pos.x << ' ' << t.pos.y << ' ' << t.pos.z << ' ' << t.azimuthDeg << ' ' << t.pattern << ' ' << t.gainDb << ' ' << t.elevationDeg << "\n";
    for (auto& t : s.receivers) o << "receiver " << tok(t.name) << ' ' << t.pos.x << ' ' << t.pos.y << ' ' << t.pos.z << ' ' << t.azimuthDeg << ' ' << t.pattern << ' ' << t.gainDb << ' ' << t.elevationDeg << "\n";
    return o.str();
}

inline Scene loadScene(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open scene file: " + path);
    return parseScene(in, path);
}

}  // namespace verb
