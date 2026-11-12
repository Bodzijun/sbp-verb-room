// Shared 2.5D geometry helpers: surfaces are vertical wall segments (ids 0..W-1), floor (W), ceiling (W+1).
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "scene.hpp"

namespace verb {

struct Geo {
    const Scene& s;
    int W;  // wall count
    bool hasCeil;
    double H;
    std::vector<const Zone*> plates;  // horizontal plates (zones with a height): surface ids W+2 ...
    explicit Geo(const Scene& sc) : s(sc), W((int)sc.walls.size()), hasCeil(sc.ceilMat >= 0), H(sc.height) {
        for (const auto& z : sc.ceilZones) {
            hasCeil |= z.mat >= 0 && z.z < 0;  // a ceiling over part of an open sky
            if (z.z >= 0 && z.mat >= 0 && z.poly.size() >= 3) plates.push_back(&z);
        }
        for (const auto& z : sc.floorZones)
            if (z.z >= 0 && z.mat >= 0 && z.poly.size() >= 3) plates.push_back(&z);
    }
    int floorId() const { return W; }
    int ceilId() const { return W + 1; }
    int nSurf() const { return W + 2 + (int)plates.size(); }
    bool isPlate(int surf) const { return surf >= W + 2; }
    const Zone& plate(int surf) const { return *plates[(size_t)(surf - W - 2)]; }
    const Material& mat(int surf) const {
        if (surf >= W + 2) return s.mats[(size_t)plate(surf).mat];
        return s.mats[surf < W ? s.walls[surf].mat : (surf == W ? s.floorMat : s.ceilMat)];
    }
    // Material of surface `surf` at the hit point (x, y): floor and ceiling zones are looked up there.
    // nullptr = nothing there (an opening in the ceiling): the sound passes.
    const Material* matAt(int surf, double x, double y) const {
        if (surf < W) return &s.mats[(size_t)s.walls[(size_t)surf].mat];
        if (surf >= W + 2) return &s.mats[(size_t)plate(surf).mat];
        const int m = surf == W ? floorMatAt(s, x, y) : ceilMatAt(s, x, y);
        return m >= 0 ? &s.mats[(size_t)m] : nullptr;
    }
};

inline double cross2(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }

inline Vec3 mirror(const Geo& g, Vec3 p, int surf) {
    if (surf < g.W) {
        const Wall& w = g.s.walls[surf];
        double dx = w.bx - w.ax, dy = w.by - w.ay;
        double t = ((p.x - w.ax) * dx + (p.y - w.ay) * dy) / (dx * dx + dy * dy);
        double fx = w.ax + t * dx, fy = w.ay + t * dy;
        return {2 * fx - p.x, 2 * fy - p.y, p.z};
    }
    if (surf == g.W) return {p.x, p.y, -p.z};
    if (surf >= g.W + 2) return {p.x, p.y, 2 * g.plate(surf).z - p.z};
    return {p.x, p.y, 2 * g.H - p.z};
}

// Where segment cur->target crosses surface `surf` (within its extent).
inline bool crossSurface(const Geo& g, Vec3 cur, Vec3 target, int surf, Vec3& P) {
    double t;
    if (surf < g.W) {
        const Wall& w = g.s.walls[surf];
        double rx = target.x - cur.x, ry = target.y - cur.y, ex = w.bx - w.ax, ey = w.by - w.ay;
        double den = cross2(rx, ry, ex, ey);
        if (std::fabs(den) < 1e-12) return false;
        t = cross2(w.ax - cur.x, w.ay - cur.y, ex, ey) / den;
        double u = cross2(w.ax - cur.x, w.ay - cur.y, rx, ry) / den;
        if (t <= 1e-9 || t >= 1 - 1e-9 || u < 0 || u > 1) return false;
        double z = cur.z + t * (target.z - cur.z);
        if (z < std::max(0.0, w.z0) || z > std::min(w.z1, g.H)) return false;  // a reflection point must lie on the wall's face
    } else if (surf >= g.W + 2) {
        const Zone& pl = g.plate(surf);
        double dz = target.z - cur.z;
        if (std::fabs(dz) < 1e-12) return false;
        t = (pl.z - cur.z) / dz;
        if (t <= 1e-9 || t >= 1 - 1e-9) return false;
        if (!pointInZone(pl, cur.x + t * (target.x - cur.x), cur.y + t * (target.y - cur.y))) return false;
    } else {
        double dz = target.z - cur.z;
        if (std::fabs(dz) < 1e-12) return false;
        t = ((surf == g.W ? 0.0 : g.H) - cur.z) / dz;
        if (t <= 1e-9 || t >= 1 - 1e-9) return false;
    }
    P = cur + (target - cur) * t;
    return true;
}

// Walks the segment a-b past every wall it crosses (except ign1/ign2). Each crossing multiplies `amp[band]`
// (if given) by the wall's amplitude transmission sqrt(tau_b). Returns false when a crossed wall is opaque
// (tau == 0 in all bands): the path does not exist. Open segments (tau = 1) pass the sound unchanged.
inline bool transmitLeg(const Geo& g, Vec3 a, Vec3 b, int ign1, int ign2, float* amp) {
    double rx = b.x - a.x, ry = b.y - a.y;
    for (int i = 0; i < g.W; ++i) {
        if (i == ign1 || i == ign2) continue;
        const Wall& w = g.s.walls[i];
        double ex = w.bx - w.ax, ey = w.by - w.ay;
        double den = cross2(rx, ry, ex, ey);
        if (std::fabs(den) < 1e-12) continue;
        double t = cross2(w.ax - a.x, w.ay - a.y, ex, ey) / den;
        double u = cross2(w.ax - a.x, w.ay - a.y, rx, ry) / den;
        if (t > 1e-6 && t < 1 - 1e-6 && u >= 0 && u <= 1) {
            const double zc = a.z + t * (b.z - a.z);  // height of the path where it crosses the wall's line
            if (zc < w.z0 || zc > w.z1) continue;      // it passes over (or under) this wall
            const Material& m = g.s.mats[(size_t)w.mat];
            double mx = 0;
            for (int k = 0; k < NB; ++k) mx = std::max(mx, m.tau[k]);
            if (mx <= 0) return false;
            if (amp)
                for (int k = 0; k < NB; ++k) amp[k] *= (float)std::sqrt(m.tau[k]);
        }
    }
    const double dz = b.z - a.z;
    if (!g.plates.empty() && std::fabs(dz) > 1e-12)
        for (int k = 0; k < (int)g.plates.size(); ++k) {
            const int id = g.W + 2 + k;
            if (id == ign1 || id == ign2) continue;
            const Zone& pl = *g.plates[(size_t)k];
            const double t = (pl.z - a.z) / dz;
            if (t <= 1e-6 || t >= 1 - 1e-6) continue;
            if (!pointInZone(pl, a.x + t * (b.x - a.x), a.y + t * (b.y - a.y))) continue;
            const Material& m = g.s.mats[(size_t)pl.mat];
            double mx = 0;
            for (int q = 0; q < NB; ++q) mx = std::max(mx, m.tau[q]);
            if (mx <= 0) return false;
            if (amp)
                for (int q = 0; q < NB; ++q) amp[q] *= (float)std::sqrt(m.tau[q]);
        }
    return true;
}

inline bool blocked(const Geo& g, Vec3 a, Vec3 b, int ign1, int ign2) { return !transmitLeg(g, a, b, ign1, ign2, nullptr); }

// ---- edge diffraction (vertical edges = wall ends) ----
// Knife-edge attenuation, dB, of a path whose detour is `delta` metres (ITU-R P.526 approximation of the Fresnel
// integral). `shadow` = true: the edge is in the way (the sound has to bend round it); false: the edge is just beside
// the clear path and still trims the first Fresnel zone. Continuous across the shadow boundary (6 dB there).
inline double knifeEdgeDb(double delta, double freq, double c, bool shadow) {
    double v = std::sqrt(std::max(0.0, 2.0 * delta * freq / c));
    if (!shadow) v = -v;
    if (v <= -0.78) return 0.0;
    const double x = v - 0.1;
    return 6.9 + 20.0 * std::log10(std::sqrt(x * x + 1.0) + x);
}

// Like crossSurface, but the end of a wall is soft: a reflection point a little beyond the wall's end is still
// allowed and a point right at the end is attenuated, by the knife-edge loss of the point's distance from the end
// (6 dB exactly at the end, none well inside, falling away outside). A specular reflection therefore fades in and out
// as the source or the listener moves past the end of the wall instead of switching on and off. `amp[band]` is
// multiplied by the loss. Floors, ceilings and scenes without diffraction keep the hard edge.
inline bool crossSurfaceSoft(const Geo& g, Vec3 cur, Vec3 target, int surf, Vec3& P, double c, float* amp) {
    if (surf >= g.W || !g.s.diffraction) return crossSurface(g, cur, target, surf, P);
    const Wall& w = g.s.walls[surf];
    const double rx = target.x - cur.x, ry = target.y - cur.y, ex = w.bx - w.ax, ey = w.by - w.ay;
    const double den = cross2(rx, ry, ex, ey);
    if (std::fabs(den) < 1e-12) return false;
    const double t = cross2(w.ax - cur.x, w.ay - cur.y, ex, ey) / den;
    const double u = cross2(w.ax - cur.x, w.ay - cur.y, rx, ry) / den;
    if (t <= 1e-9 || t >= 1 - 1e-9) return false;
    const double wl = std::hypot(ex, ey);
    const double margin = 2.5;  // metres beyond the end where the reflection has died away at every frequency
    if (u * wl < -margin || (u - 1.0) * wl > margin) return false;
    const double z = cur.z + t * (target.z - cur.z);
    if (z < std::max(0.0, w.z0) || z > std::min(w.z1, g.H)) return false;
    P = cur + (target - cur) * t;
    const double dIn = std::min(u, 1.0 - u) * wl;  // >0 inside the wall, <0 beyond its end
    if (dIn >= 2.5) return true;
    const double d1 = std::max(0.05, norm(P - cur)), d2 = std::max(0.05, norm(target - P));
    const double delta = 0.5 * dIn * dIn * (1.0 / d1 + 1.0 / d2);
    for (int b = 0; b < NB; ++b) amp[b] *= (float)std::pow(10.0, -knifeEdgeDb(delta, kBandHz[b], c, dIn < 0) / 20.0);
    return true;
}

// An opaque wall can be diffracted around only if it really blocks at the path height.
inline bool edgeRelevant(const Wall& w, const Material& m, double z) {
    if (z < w.z0 || z > w.z1) return false;
    for (int k = 0; k < NB; ++k) if (m.tau[k] > 0.2) return false;  // lets 7 dB or less through: a screen, not a curtain
    return true;
}

// Direct path S->R is clear: walls whose end lies close beside it still take a share of the first Fresnel zone.
// Multiplies att[band] (amplitude) by the strongest such edge's attenuation.
inline void litEdgeAttenuation(const Geo& g, Vec3 S, Vec3 R, double c, float* att) {
    const double rx = R.x - S.x, ry = R.y - S.y, len2 = rx * rx + ry * ry, len = std::sqrt(len2);
    if (len < 1e-6) return;
    const double dist3 = std::sqrt(len2 + (R.z - S.z) * (R.z - S.z));
    double bestDb[NB] = {};
    for (int i = 0; i < g.W; ++i) {
        const Wall& w = g.s.walls[i];
        const Material& m = g.s.mats[(size_t)w.mat];
        for (int e = 0; e < 2; ++e) {
            const double ex = e ? w.bx : w.ax, ey = e ? w.by : w.ay, fx = e ? w.ax : w.bx, fy = e ? w.ay : w.by;
            const double t = ((ex - S.x) * rx + (ey - S.y) * ry) / len2;
            if (t <= 0.0 || t >= 1.0) continue;
            const double perp = std::fabs(cross2(rx, ry, ex - S.x, ey - S.y)) / len;
            if (perp > 3.0) continue;
            const double z = S.z + t * (R.z - S.z);
            if (!edgeRelevant(w, m, z)) continue;
            // a wall running along the path hardly screens it; one across it screens fully
            const double sinTheta = std::fabs(cross2(rx, ry, fx - ex, fy - ey)) / (len * std::max(1e-9, std::hypot(fx - ex, fy - ey)));
            const Vec3 E{ex, ey, z};
            const double delta = norm(E - S) + norm(R - E) - dist3;
            for (int b = 0; b < NB; ++b) bestDb[b] = std::max(bestDb[b], knifeEdgeDb(delta, kBandHz[b], c, false) * std::min(1.0, 2.0 * sinTheta));
        }
    }
    for (int b = 0; b < NB; ++b) att[b] *= (float)std::pow(10.0, -bestDb[b] / 20.0);
}

struct DiffPath {
    Vec3 first, last;   // the wall end the sound bends round first (next to the source) and last (next to the receiver)
    double inner = 0;   // length of the legs between the first and the last end (0 for a single end)
    double length = 0;  // S -> ... -> R
    float amp[NB] = {};
};

// Audio callers reserve once for the scene's wall budget; offline callers may use
// the local fallback. Reusing these buffers does not change path order or gains.
struct DiffractionScratch {
    uint64_t capacityMisses=0; // owner may inspect; never silently discard a path
    struct Node { Vec3 p; int wall; };
    std::vector<Node> nodes;
    std::vector<int> a, b;
    std::vector<float> ampA, ampB;
    void prepare(size_t walls) {
        const auto n=walls*2;
        nodes.reserve(n);a.reserve(n);b.reserve(n);
        ampA.resize(n*NB);ampB.resize(n*NB);
    }
};

// The direct path is blocked: find the way round the wall ends that the sound takes best. One end first (both legs
// clear); if none works (a closed building: the sound has to go round two corners), two, then three ends in a row.
// Every bend attenuates by its own knife-edge loss, so the level falls smoothly while the source goes behind a
// building and never switches on or off.
inline bool diffractedPath(const Geo& g, Vec3 S, Vec3 R, double c, DiffPath& out, bool needClear = false, DiffractionScratch* scratch = nullptr) {
    if(!scratch){DiffractionScratch local;local.prepare((size_t)g.W);return diffractedPath(g,S,R,c,out,needClear,&local);}
    auto& work=*scratch;
    using Node=DiffractionScratch::Node;
    auto& nodes=work.nodes;nodes.clear();
    if(nodes.capacity()<(size_t)g.W*2) {++work.capacityMisses;return false;}
    const double dist3 = norm(R - S);
    const double rx = R.x - S.x, ry = R.y - S.y, len2 = std::max(1e-12, rx * rx + ry * ry);
    for (int i = 0; i < g.W; ++i) {
        const Wall& w = g.s.walls[i];
        for (int e = 0; e < 2; ++e) {
            const double ex = e ? w.bx : w.ax, ey = e ? w.by : w.ay;
            double t = ((ex - S.x) * rx + (ey - S.y) * ry) / len2;
            t = std::min(1.0, std::max(0.0, t));
            const double z = S.z + t * (R.z - S.z);
            if (z < 0 || z > g.H) continue;
            nodes.push_back({Vec3{ex, ey, z}, i});
        }
    }
    const int N = (int)nodes.size();
    auto setAmp = [&](const Vec3* pts, int n, const float* legAmp) {  // pts[0]=S ... pts[n-1]=R: knife-edge loss at every bend
        for (int b = 0; b < NB; ++b) {
            double db = 0;
            for (int k = 1; k + 1 < n; ++k) {
                const double delta = norm(pts[k] - pts[k - 1]) + norm(pts[k + 1] - pts[k]) - norm(pts[k + 1] - pts[k - 1]);
                db += knifeEdgeDb(delta, kBandHz[b], c, true);
            }
            out.amp[b] = legAmp[b] * (float)std::pow(10.0, -db / 20.0);
        }
    };
    // 1. one wall end (the smallest detours first)
    {
        struct Cand { double delta; int idx; };
        Cand best[4];
        int nb = 0;
        for (int j = 0; j < N; ++j) {
            const double delta = norm(nodes[j].p - S) + norm(R - nodes[j].p) - dist3;
            int pos = nb;
            while (pos > 0 && best[pos - 1].delta > delta) --pos;
            if (pos >= 4) continue;
            for (int k = std::min(nb, 3); k > pos; --k) best[k] = best[k - 1];
            best[pos] = {delta, j};
            nb = std::min(nb + 1, 4);
        }
        for (int k = 0; k < nb; ++k) {
            const Node& nd = nodes[best[k].idx];
            float amp[NB];
            for (int b = 0; b < NB; ++b) amp[b] = 1.f;
            if (!transmitLeg(g, S, nd.p, nd.wall, -1, amp) || !transmitLeg(g, nd.p, R, nd.wall, -1, amp)) continue;
            if (needClear && amp[3] < 0.7f) continue;  // both legs must be in the open: the sound goes round, not through
            litEdgeAttenuation(g, S, nd.p, c, amp);  // other wall ends close beside the two legs still trim them,
            litEdgeAttenuation(g, nd.p, R, c, amp);  // so the level is continuous where a second corner takes over
            const Vec3 pts[3] = {S, nd.p, R};
            out.first = out.last = nd.p;
            out.inner = 0;
            out.length = norm(nd.p - S) + norm(R - nd.p);
            setAmp(pts, 3, amp);
            return true;
        }
    }
    if (N > 160) return false;  // a scene this busy: keep the cost bounded
    // 2. round several ends: which ends does the source see, which does the receiver see
    auto& A=work.a;auto& B=work.b;A.clear();B.clear();
    auto& ampA=work.ampA;auto& ampB=work.ampB;
    if(A.capacity()<(size_t)N || B.capacity()<(size_t)N || ampA.size()<(size_t)N*NB || ampB.size()<(size_t)N*NB) {++work.capacityMisses;return false;}
    for (int j = 0; j < N; ++j) {
        float a[NB], bb[NB];
        for (int b = 0; b < NB; ++b) a[b] = bb[b] = 1.f;
        if (transmitLeg(g, S, nodes[j].p, nodes[j].wall, -1, a)) { A.push_back(j); std::copy(a, a + NB, ampA.begin() + (long)j * NB); }
        if (transmitLeg(g, nodes[j].p, R, nodes[j].wall, -1, bb)) { B.push_back(j); std::copy(bb, bb + NB, ampB.begin() + (long)j * NB); }
    }
    auto close = [](Vec3 a, Vec3 b) { return norm(a - b) < 1e-6; };
    double bestLen = 1e30;
    int bj = -1, bm = -1, bk = -1;
    float bestAmp[NB] = {};
    for (int j : A)  // two ends
        for (int k : B) {
            if (j == k || close(nodes[j].p, nodes[k].p)) continue;
            const double len = norm(nodes[j].p - S) + norm(nodes[k].p - nodes[j].p) + norm(R - nodes[k].p);
            if (len >= bestLen) continue;
            float a[NB];
            for (int b = 0; b < NB; ++b) a[b] = ampA[(size_t)j * NB + b] * ampB[(size_t)k * NB + b];
            if (!transmitLeg(g, nodes[j].p, nodes[k].p, nodes[j].wall, nodes[k].wall, a)) continue;
            if (needClear && a[3] < 0.7f) continue;
            bestLen = len; bj = j; bm = -1; bk = k;
            std::copy(a, a + NB, bestAmp);
        }
    if (bj < 0)  // three ends, only if two did not work
        for (int j : A)
            for (int m = 0; m < N; ++m) {
                if (m == j || close(nodes[j].p, nodes[m].p)) continue;
                const double l1 = norm(nodes[j].p - S) + norm(nodes[m].p - nodes[j].p);
                if (l1 >= bestLen) continue;
                float a1[NB];
                for (int b = 0; b < NB; ++b) a1[b] = ampA[(size_t)j * NB + b];
                if (!transmitLeg(g, nodes[j].p, nodes[m].p, nodes[j].wall, nodes[m].wall, a1)) continue;
                for (int k : B) {
                    if (k == m || close(nodes[m].p, nodes[k].p) || close(nodes[j].p, nodes[k].p)) continue;
                    const double len = l1 + norm(nodes[k].p - nodes[m].p) + norm(R - nodes[k].p);
                    if (len >= bestLen) continue;
                    float a[NB];
                    for (int b = 0; b < NB; ++b) a[b] = a1[b] * ampB[(size_t)k * NB + b];
                    if (!transmitLeg(g, nodes[m].p, nodes[k].p, nodes[m].wall, nodes[k].wall, a)) continue;
                    if (needClear && a[3] < 0.7f) continue;
                    bestLen = len; bj = j; bm = m; bk = k;
                    std::copy(a, a + NB, bestAmp);
                }
            }
    if (bj < 0) return false;
    Vec3 pts[5];
    int n = 0;
    pts[n++] = S;
    pts[n++] = nodes[bj].p;
    if (bm >= 0) pts[n++] = nodes[bm].p;
    pts[n++] = nodes[bk].p;
    pts[n++] = R;
    out.first = nodes[bj].p;
    out.last = nodes[bk].p;
    out.length = bestLen;
    out.inner = bestLen - norm(nodes[bj].p - S) - norm(R - nodes[bk].p);
    setAmp(pts, n, bestAmp);
    return true;
}

// Directivity gain of every octave band (the pattern may change with frequency: `pattern` at 125 Hz, `patternHf` at 8 kHz).
// The angle is evaluated once.
inline void dirGainBands(const Transducer& t, Vec3 d, double* g /* NB values */) {
    const double a = t.azimuthDeg * kPi / 180.0;
    double c;
    if (t.elevationDeg == 0.0) {
        c = d.x * std::cos(a) + d.y * std::sin(a);
    } else {
        const double e = t.elevationDeg * kPi / 180.0, ce = std::cos(e);
        c = d.x * ce * std::cos(a) + d.y * ce * std::sin(a) + d.z * std::sin(e);
    }
    const double gdb = t.gainDb == 0.0 ? 1.0 : std::pow(10.0, t.gainDb / 20.0);
    for (int b = 0; b < NB; ++b) {
        const double p = t.patternHf < 0 ? t.pattern : t.pattern + (t.patternHf - t.pattern) * ((double)b / (double)(NB - 1));
        g[b] = ((1 - p) + p * c) * gdb;
    }
}

inline double dirGain(const Transducer& t, Vec3 d) {
    const double a = t.azimuthDeg * kPi / 180.0;
    double c;
    if (t.elevationDeg == 0.0) {
        c = d.x * std::cos(a) + d.y * std::sin(a);  // horizontal axis (the common case, cheap)
    } else {
        const double e = t.elevationDeg * kPi / 180.0, ce = std::cos(e);
        c = d.x * ce * std::cos(a) + d.y * ce * std::sin(a) + d.z * std::sin(e);
    }
    return ((1 - t.pattern) + t.pattern * c) * std::pow(10.0, t.gainDb / 20.0);
}

}  // namespace verb
