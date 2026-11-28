#include "motion_grid.hpp"
#include "bake_pool.hpp"
#include "geometry.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <future>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace verb {
namespace {

struct GridSphereClip {
    struct WallBlock { double ax, ay, ex, ey, z0, z1, sideC; };
    bool clean = true;
    std::vector<WallBlock> walls;
    std::vector<int> plates;
    bool floorNear = false, ceilNear = false;
    double reach = 1.0;
};

static bool gridReachable(const Geo& g, const GridSphereClip& sc, Vec3 c, Vec3 q) {
    if (sc.floorNear && q.z < 0.0) return false;
    if (sc.ceilNear && q.z > g.H) {
        const double t = (g.H - c.z) / (q.z - c.z);
        const Vec3 P = c + (q - c) * t;
        if (g.matAt(g.ceilId(), P.x, P.y)) return false;
    }
    const double rx = q.x - c.x, ry = q.y - c.y, dz = q.z - c.z;
    for (int k : sc.plates) {
        const Zone& pl = *g.plates[(size_t)k];
        if (std::fabs(dz) < 1e-12) continue;
        const double t = (pl.z - c.z) / dz;
        if (t > 0.0 && t < 1.0 && pointInZone(pl, c.x + t * rx, c.y + t * ry)) return false;
    }
    for (const auto& w : sc.walls) {
        if (cross2(w.ex, w.ey, q.x - w.ax, q.y - w.ay) * w.sideC > 0.0) continue;
        const double den = cross2(rx, ry, w.ex, w.ey);
        if (std::fabs(den) < 1e-12) continue;
        const double t = cross2(w.ax - c.x, w.ay - c.y, w.ex, w.ey) / den;
        const double u = cross2(w.ax - c.x, w.ay - c.y, rx, ry) / den;
        if (t <= 0.0 || t >= 1.0 || u < 0.0 || u > 1.0) continue;
        const double z = c.z + t * dz;
        if (z < w.z0 || z > w.z1) continue;
        return false;
    }
    return true;
}

static double gridHalton(int i, int base) {
    double f = 1.0, r = 0.0;
    for (; i > 0; i /= base) { f /= base; r += f * (double)(i % base); }
    return r;
}

static GridSphereClip makeGridSphereClip(const Geo& g, Vec3 c, double rd) {
    GridSphereClip sc;
    for (int i = 0; i < g.W; ++i) {
        const Wall& w = g.s.walls[(size_t)i];
        if (!wallBlocks(g, w)) continue;
        if (w.z1 <= c.z - rd || w.z0 >= c.z + rd) continue;
        const double ex = w.bx - w.ax, ey = w.by - w.ay, l2 = ex * ex + ey * ey;
        double t = l2 > 1e-18 ? ((c.x - w.ax) * ex + (c.y - w.ay) * ey) / l2 : 0.0;
        t = std::min(1.0, std::max(0.0, t));
        if (std::hypot(c.x - (w.ax + t * ex), c.y - (w.ay + t * ey)) < rd) {
            const double side = cross2(ex, ey, c.x - w.ax, c.y - w.ay);
            sc.walls.push_back({w.ax, w.ay, ex, ey, std::max(0.0, w.z0), std::min(w.z1, g.H),
                                side > 0 ? 1.0 : side < 0 ? -1.0 : 0.0});
        }
    }
    for (int k = 0; k < (int)g.plates.size(); ++k)
        if (g.plates[(size_t)k]->z > c.z - rd && g.plates[(size_t)k]->z < c.z + rd) sc.plates.push_back(k);
    sc.floorNear = c.z - rd < 0.0;
    sc.ceilNear = g.hasCeil && c.z + rd > g.H;
    sc.clean = sc.walls.empty() && sc.plates.empty() && !sc.floorNear && !sc.ceilNear;
    if (sc.clean) return sc;
    int n = 0, hit = 0;
    for (int i = 1; n < 6000 && i < 40000; ++i) {
        const Vec3 u{2.0 * gridHalton(i, 2) - 1.0, 2.0 * gridHalton(i, 3) - 1.0, 2.0 * gridHalton(i, 5) - 1.0};
        if (dot(u, u) > 1.0) continue;
        ++n;
        if (gridReachable(g, sc, c, c + u * rd)) ++hit;
    }
    sc.reach = std::max(0.02, n > 0 ? (double)hit / n : 1.0);
    return sc;
}

static uint64_t gridRaySeed(int seed, uint64_t index) {
    uint64_t x = (uint64_t)(uint32_t)seed * 7919ull + 13 + 0x9e3779b97f4a7c15ull * (index + 1);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

struct GridCounter {
    uint64_t state = 0;
    explicit GridCounter(uint64_t initial = 0) : state(initial) {}
    void seed(uint64_t initial) { state = initial; }
    uint64_t operator()() {
        uint64_t x = (state += 0x9e3779b97f4a7c15ull);
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    }
};

struct GridUniform {
    double operator()(GridCounter& rng) const { return (rng() >> 11) * (1.0 / 9007199254740992.0); }
};

struct GridTracePrepared {
    Geo g;
    int P;
    double c, rd = 0., Vs = 0.;
    std::array<double, NB> airDb{};
    std::vector<GridSphereClip> clip;
    std::vector<int> probeOrder;
    double rz0 = 1e30, rz1 = -1e30;

    GridTracePrepared(const Scene& sc, const std::vector<Transducer>& probes, double minRd)
        : g(sc), P((int)probes.size()), c(speedOfSound(sc.tempC)) {
        for (int b = 0; b < NB; ++b) airDb[b] = airAbsDbPerM(kBandHz[b], sc.tempC, sc.humidity);
        rd = sc.detectRadius;
        {
            double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
            for (const auto& w : sc.walls) {
                x0 = std::min({x0, w.ax, w.bx});
                x1 = std::max({x1, w.ax, w.bx});
                y0 = std::min({y0, w.ay, w.by});
                y1 = std::max({y1, w.ay, w.by});
            }
            if (x1 > x0 && y1 > y0) {
                const double vol = (x1 - x0) * (y1 - y0) * std::max(1.0, std::min(sc.height, 40.0));
                rd = std::min(3.0, std::max(rd, 0.05 * std::cbrt(vol)));
            }
        }
        if (minRd > 0.) rd = std::min(3.0, std::max(rd, minRd));
        Vs = 4.0 / 3.0 * kPi * rd * rd * rd;
        clip.resize((size_t)P);
        for (int p = 0; p < P; ++p) clip[(size_t)p] = makeGridSphereClip(g, probes[(size_t)p].pos, rd);
        probeOrder.resize((size_t)P);
        std::iota(probeOrder.begin(), probeOrder.end(), 0);
        std::sort(probeOrder.begin(), probeOrder.end(),
                  [&](int a, int b) { return probes[(size_t)a].pos.x < probes[(size_t)b].pos.x; });
        for (const auto& pr : probes) {
            rz0 = std::min(rz0, pr.pos.z - rd);
            rz1 = std::max(rz1, pr.pos.z + rd);
        }
    }
};

struct GridChunkHist {
    // omni: [probe][band][win], ch: [probe * R + r][band][win]
    std::vector<std::array<std::array<double, kBakeWinCount>, NB>> omni;
    std::vector<std::array<std::array<double, kBakeWinCount>, NB>> ch;
};

static GridChunkHist traceGridChunk(const Scene& sc, Vec3 anchorPos,
                                    const std::vector<Transducer>& probes,
                                    const std::vector<Transducer>& receivers,
                                    bool sourceField, int order, int rays,
                                    int firstRay, int lastRay,
                                    const GridTracePrepared& prep,
                                    const std::function<bool()>& stop,
                                    GridChunkHist recycled = {}) {
    const auto& g = prep.g;
    const int P = prep.P;
    const int R = std::max(1, (int)receivers.size());
    const double c = prep.c, rd = prep.rd, Vs = prep.Vs;
    const auto& airDb = prep.airDb;
    const auto& clip = prep.clip;
    const auto& probeOrder = prep.probeOrder;
    const double rz0 = prep.rz0, rz1 = prep.rz1;

    GridChunkHist out = std::move(recycled);
    out.omni.assign((size_t)P, {});
    out.ch.assign((size_t)P * (size_t)R, {});

    GridCounter rng((uint64_t)sc.seed * 7919ull + 13);
    GridUniform U;
    const double maxPath = c * sc.length;
    const double binDur = 0.001;

    std::array<std::array<double, NB>, kMaxMotionChannels> rcvWeight2{};
    for (int r = 0; r < R; ++r) rcvWeight2[(size_t)r].fill(1.0);

    for (int i = firstRay; i < lastRay; ++i) {
        if (i % 32 == 0 && stop && stop()) throw std::runtime_error("Analysis cancelled");
        rng.seed(gridRaySeed(sc.seed, (uint64_t)i));
        const double zc = 2 * U(rng) - 1, ph = 2 * kPi * U(rng), sr_ = std::sqrt(1 - zc * zc);
        Vec3 d{sr_ * std::cos(ph), sr_ * std::sin(ph), zc};

        // For sourceField == true (reciprocal trace starting at listener), the ray leaves
        // the listener in direction d0 = d, so a reciprocal path from a probe arrives at
        // the listener from direction d0. Evaluate each receiver capsule's gain for d0 once.
        if (sourceField && !receivers.empty()) {
            for (int r = 0; r < R; ++r) {
                double grb[NB];
                dirGainBands(receivers[(size_t)r], d, grb);
                for (int b = 0; b < NB; ++b) rcvWeight2[(size_t)r][(size_t)b] = grb[b] * grb[b];
            }
        }

        std::array<double, NB> e;
        e.fill(1.0 / rays);
        Vec3 p = anchorPos;
        double path = 0;
        int refl = 0, steps = 0;
        while (true) {
            if (++steps % 32 == 0 && stop && stop()) throw std::runtime_error("Analysis cancelled");
            double tBest = std::numeric_limits<double>::infinity();
            int surf = -1;
            for (int w = 0; w < g.W; ++w) {
                const Wall& wl = sc.walls[(size_t)w];
                const double ex = wl.bx - wl.ax, ey = wl.by - wl.ay;
                const double den = cross2(d.x, d.y, ex, ey);
                if (std::fabs(den) < 1e-12) continue;
                const double t = cross2(wl.ax - p.x, wl.ay - p.y, ex, ey) / den;
                const double u = cross2(wl.ax - p.x, wl.ay - p.y, d.x, d.y) / den;
                if (t > 1e-7 && t < tBest && u >= 0 && u <= 1) {
                    const double z = p.z + d.z * t;
                    if (z >= std::max(0.0, wl.z0) && z <= std::min(wl.z1, g.H)) { tBest = t; surf = w; }
                }
            }
            if (d.z < -1e-12) {
                const double t = -p.z / d.z;
                if (t > 1e-7 && t < tBest) { tBest = t; surf = g.floorId(); }
            }
            if (g.hasCeil && d.z > 1e-12) {
                const double t = (g.H - p.z) / d.z;
                if (t > 1e-7 && t < tBest) { tBest = t; surf = g.ceilId(); }
            }
            for (int k = 0; k < (int)g.plates.size() && std::fabs(d.z) > 1e-12; ++k) {
                const Zone& pl = *g.plates[(size_t)k];
                const double t = (pl.z - p.z) / d.z;
                if (t > 1e-7 && t < tBest && pointInZone(pl, p.x + d.x * t, p.y + d.y * t)) {
                    tBest = t;
                    surf = g.W + 2 + k;
                }
            }
            const double L = surf < 0 ? maxPath - path : std::min(tBest, maxPath - path);
            if (L <= 0) break;
            if (refl > order && P > 0) {
                auto record = [&](int prIdx) {
                    const Vec3 oc = p - probes[(size_t)prIdx].pos;
                    const double bq = dot(oc, d), cc = dot(oc, oc) - rd * rd, disc = bq * bq - cc;
                    if (disc <= 0) return;
                    const double sq = std::sqrt(disc);
                    const double s1 = std::max(-bq - sq, 0.0), s2 = std::min(-bq + sq, L);
                    if (s2 <= s1) return;
                    const double mid = path + 0.5 * (s1 + s2);
                    double chord = s2 - s1, vol = Vs;
                    const GridSphereClip& sphere = clip[(size_t)prIdx];
                    if (!sphere.clean) {
                        const Vec3 c0 = probes[(size_t)prIdx].pos;
                        const bool v0 = gridReachable(g, sphere, c0, p + d * s1),
                                   v1 = gridReachable(g, sphere, c0, p + d * (0.5 * (s1 + s2))),
                                   v2 = gridReachable(g, sphere, c0, p + d * s2);
                        if (v0 == v1 && v1 == v2) {
                            if (!v0) return;
                        } else {
                            constexpr int K = 8;
                            const double ds = chord / K;
                            int seen = 0;
                            for (int k = 0; k < K; ++k)
                                if (gridReachable(g, sphere, c0, p + d * (s1 + (k + 0.5) * ds))) ++seen;
                            if (seen == 0) return;
                            chord = ds * seen;
                        }
                        vol = Vs * sphere.reach;
                    }
                    size_t bin = (size_t)(mid / c / binDur);
                    const double binCenter = (bin + .5) * binDur;
                    bin = static_cast<size_t>(
                        std::upper_bound(kBakeWinEndSec, kBakeWinEndSec + kBakeWinCount - 1, binCenter) - kBakeWinEndSec);
                    if (bin >= kBakeWinCount) return;

                    if (!sourceField && !receivers.empty()) {
                        const Vec3 inDir = d * -1.0;
                        for (int r = 0; r < R; ++r) {
                            double grb[NB];
                            dirGainBands(receivers[(size_t)r], inDir, grb);
                            for (int b = 0; b < NB; ++b) rcvWeight2[(size_t)r][(size_t)b] = grb[b] * grb[b];
                        }
                    }

                    std::array<double, NB> contribOmni;
                    const double geomFactor = 4 * kPi * chord / vol;
                    auto& dstOmni = out.omni[(size_t)prIdx];
                    for (int b = 0; b < NB; ++b) {
                        const double cVal = geomFactor * e[b] * std::pow(10.0, -airDb[b] * mid / 10.0);
                        contribOmni[b] = cVal;
                        dstOmni[b][bin] += cVal;
                    }
                    const size_t baseCh = (size_t)prIdx * (size_t)R;
                    for (int r = 0; r < R; ++r) {
                        auto& dstCh = out.ch[baseCh + (size_t)r];
                        const auto& w2 = rcvWeight2[(size_t)r];
                        for (int b = 0; b < NB; ++b) {
                            dstCh[b][bin] += contribOmni[b] * w2[b];
                        }
                    }
                };

                double a = 0, b = L;
                if (std::fabs(d.z) > 1e-12) {
                    double lo = (rz0 - p.z) / d.z, hi = (rz1 - p.z) / d.z;
                    if (lo > hi) std::swap(lo, hi);
                    a = std::max(a, lo);
                    b = std::min(b, hi);
                } else if (p.z < rz0 || p.z > rz1) {
                    b = -1;
                }
                if (b >= a) {
                    const Vec3 q0 = p + d * a, q1 = p + d * b;
                    const double x0 = std::min(q0.x, q1.x) - rd, x1 = std::max(q0.x, q1.x) + rd;
                    const double y0 = std::min(q0.y, q1.y) - rd, y1 = std::max(q0.y, q1.y) + rd;
                    auto it = std::lower_bound(probeOrder.begin(), probeOrder.end(), x0,
                                               [&](int idx, double x) { return probes[(size_t)idx].pos.x < x; });
                    for (; it != probeOrder.end() && probes[(size_t)*it].pos.x <= x1; ++it)
                        if (probes[(size_t)*it].pos.y >= y0 && probes[(size_t)*it].pos.y <= y1) record(*it);
                }
            }
            if (surf < 0 || tBest > maxPath - path) break;
            p = p + d * tBest;
            path += tBest;
            const Material* mp = g.matAt(surf, p.x, p.y);
            if (!mp) { p = p + d * 1e-6; continue; }
            const Material& m = *mp;
            double Tm = 0, Rm = 0;
            std::array<double, NB> rho;
            for (int b = 0; b < NB; ++b) {
                rho[b] = reflEnergy(m, b);
                Rm += rho[b] / NB;
                if (surf < g.W || g.isPlate(surf)) Tm += m.tau[b] / NB;
            }
            if (Tm + Rm <= 1e-12) break;
            const double pT = Tm / (Tm + Rm);
            if (pT > 0 && U(rng) < pT) {
                for (int b = 0; b < NB; ++b) e[b] *= m.tau[b] / pT;
                p = p + d * 1e-6;
                continue;
            }
            double emax = 0;
            for (int b = 0; b < NB; ++b) {
                e[b] *= rho[b] / (1.0 - pT);
                emax = std::max(emax, e[b] * std::pow(10.0, -airDb[b] * path / 10.0));
            }
            ++refl;
            if (emax < 1e-10 / rays) break;
            Vec3 n;
            if (surf < g.W) {
                const Wall& wl = sc.walls[(size_t)surf];
                const double ex = wl.bx - wl.ax, ey = wl.by - wl.ay, l = std::sqrt(ex * ex + ey * ey);
                n = {-ey / l, ex / l, 0};
            } else {
                n = {0, 0, surf == g.floorId() ? 1.0 : -1.0};
            }
            if (dot(n, d) > 0) n = n * -1.0;
            if (U(rng) < scatterAt(m, 3)) {
                const double u1 = U(rng), u2 = 2 * kPi * U(rng), rr = std::sqrt(u1);
                const Vec3 a = std::fabs(n.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
                const Vec3 t1 = unit(Vec3{n.y * a.z - n.z * a.y, n.z * a.x - n.x * a.z, n.x * a.y - n.y * a.x});
                const Vec3 t2{n.y * t1.z - n.z * t1.y, n.z * t1.x - n.x * t1.z, n.x * t1.y - n.y * t1.x};
                d = unit(t1 * (rr * std::cos(u2)) + t2 * (rr * std::sin(u2)) + n * std::sqrt(1 - u1));
            } else {
                d = unit(d - n * (2 * dot(d, n)));
            }
            p = p + n * 1e-6;
        }
    }
    return out;
}

static GridChunkHist traceGridWindows(const Scene& sc, Vec3 anchorPos,
                                      const std::vector<Transducer>& probes,
                                      const std::vector<Transducer>& receivers,
                                      bool sourceField, int order, int rays,
                                      const TailExecution& execution, double minDetectRadius) {
    constexpr int chunk = 1024;
    const GridTracePrepared prepared(sc, probes, minDetectRadius);
    GridChunkHist out;
    bool initialized = false;
    const int R = std::max(1, (int)receivers.size());
    const size_t bytes = (size_t)kBakeWinCount * std::max<size_t>(1, probes.size()) * (size_t)(R + 1) * NB * sizeof(double);
    const unsigned width = execution.pool
        ? std::max(1u, std::min<unsigned>(execution.workers,
                                          (unsigned)std::max<size_t>(1, 128u * 1024u * 1024u / std::max<size_t>(1, bytes))))
        : 1u;
    const int chunks = std::max(1, (rays + chunk - 1) / chunk);
    std::atomic<bool> failed{false};
    auto stop = [&] { return failed.load() || (execution.cancelled && execution.cancelled()); };
    auto run = [&](int j, GridChunkHist recycled = {}) {
        return traceGridChunk(sc, anchorPos, probes, receivers, sourceField, order, rays,
                              j * chunk, std::min(rays, (j + 1) * chunk), prepared, stop, std::move(recycled));
    };
    auto merge = [&](GridChunkHist& h) {
        if (!initialized) {
            out = std::move(h);
            initialized = true;
        } else {
            for (size_t p = 0; p < out.omni.size(); ++p)
                for (int b = 0; b < NB; ++b)
                    for (int w = 0; w < kBakeWinCount; ++w)
                        out.omni[p][b][w] += h.omni[p][b][w];
            for (size_t pr = 0; pr < out.ch.size(); ++pr)
                for (int b = 0; b < NB; ++b)
                    for (int w = 0; w < kBakeWinCount; ++w)
                        out.ch[pr][b][w] += h.ch[pr][b][w];
        }
    };
    if (width == 1) {
        GridChunkHist recycled;
        for (int j = 0; j < chunks; ++j) {
            recycled = run(j, std::move(recycled));
            merge(recycled);
        }
    } else {
        std::vector<std::future<GridChunkHist>> jobs((size_t)chunks);
        try {
            for (int j = 0; j < std::min<int>(chunks, (int)width); ++j)
                jobs[(size_t)j] = execution.pool->submit([&, j] { return run(j); }, true);
            for (int j = 0; j < chunks; ++j) {
                auto h = jobs[(size_t)j].get();
                merge(h);
                if (j + (int)width < chunks) {
                    const int next = j + (int)width;
                    jobs[(size_t)next] = execution.pool->submit(
                        [&, next, recycled = std::move(h)]() mutable { return run(next, std::move(recycled)); }, true);
                }
                if (stop()) throw std::runtime_error("Tail rebuild cancelled");
            }
        } catch (...) {
            failed = true;
            for (auto& f : jobs) if (f.valid()) try { f.get(); } catch (...) {}
            throw;
        }
    }
    return out;
}

} // namespace

MotionGrid::MotionGrid(const Geo& geo, Vec3 source, Vec3 listener, int order, int rays,
                       const TailExecution& execution, const std::vector<Transducer>& receivers) {
    const auto& scene = geo.s;
    if (scene.walls.empty()) return;
    channels_ = std::clamp((int)receivers.size(), 1, kMaxMotionChannels);
    std::vector<Transducer> rcvClamped = receivers;
    if ((int)rcvClamped.size() > channels_) rcvClamped.resize((size_t)channels_);

    walls_.reserve(scene.walls.size());
    double x1 = scene.walls.front().ax, y1 = scene.walls.front().ay;
    x0_ = x1;
    y0_ = y1;
    for (const auto& wall : scene.walls) {
        x0_ = std::min({x0_, wall.ax, wall.bx});
        y0_ = std::min({y0_, wall.ay, wall.by});
        x1 = std::max({x1, wall.ax, wall.bx});
        y1 = std::max({y1, wall.ay, wall.by});
        if (wallBlocks(geo, wall)) walls_.push_back(wall);
    }
    const double spanX = std::max(0.01, x1 - x0_), spanY = std::max(0.01, y1 - y0_);
    const double ceiling = std::max(0.2, scene.height);
    heights_[0] = std::clamp(0.5 * (source.z + listener.z), 0.1, ceiling - 0.1);
    if (ceiling > 5. || std::abs(source.z - listener.z) > 1.) {
        layers_ = 2;
        heights_[0] = std::clamp(std::min({source.z, listener.z, 0.25 * ceiling}), 0.1, ceiling - 0.1);
        heights_[1] = std::clamp(std::max({source.z, listener.z, 0.6 * ceiling}), heights_[0] + 0.05, ceiling - 0.05);
    }
    int interior = 0;
    constexpr int samples = 8;
    for (int y = 0; y < samples; ++y)
        for (int x = 0; x < samples; ++x)
            interior += isInsideEnclosure(geo, {x0_ + (x + .5) * spanX / samples,
                                                y0_ + (y + .5) * spanY / samples, heights_[0]});
    const double area = spanX * spanY * (interior ? double(interior) / (samples * samples) : 1.);
    const double step = std::clamp(std::sqrt(area / 64.), 1., 3.);
    nx_ = static_cast<int>(std::clamp(std::ceil(spanX / step), 1., 80.));
    ny_ = static_cast<int>(std::clamp(std::ceil(spanY / step), 1., 80.));
    while (nx_ * ny_ > 80) {
        if (spanX / nx_ < spanY / ny_) --nx_;
        else --ny_;
    }
    dx_ = spanX / nx_;
    dy_ = spanY / ny_;
    cells_.resize(static_cast<size_t>(nx_ * ny_ * layers_));
    sourceCh_.resize(cells_.size() * (size_t)channels_);
    listenerCh_.resize(cells_.size() * (size_t)channels_);

    std::vector<Transducer> probes;
    std::vector<size_t> indices;
    for (int z = 0; z < layers_; ++z) {
        for (int y = 0; y < ny_; ++y) {
            for (int x = 0; x < nx_; ++x) {
                const size_t index = static_cast<size_t>((z * ny_ + y) * nx_ + x);
                auto& cell = cells_[index];
                cell.position = {x0_ + (x + .5) * dx_, y0_ + (y + .5) * dy_, heights_[z]};
                cell.interior = isInsideEnclosure(geo, cell.position);
                if (!cell.interior) continue;
                Transducer probe;
                probe.pos = cell.position;
                probes.push_back(std::move(probe));
                indices.push_back(index);
            }
        }
    }
    if (probes.empty()) {
        cells_.clear();
        sourceCh_.clear();
        listenerCh_.clear();
        return;
    }

    // One multi-probe trace in each direction. As in MO1, the grid estimates
    // the level of the late window over 2.5 s. Only the photo supplies its decay.
    auto bounded = scene;
    bounded.length = std::min(scene.length, 2.5);
    // The low ceilings yield many probe crossings per ray. Reserve the full
    // ray budget for tall rooms, where the first window can be sparse.
    const int gridRays = ceiling <= 5. ? std::min(rays, 8000) : rays;
    const auto trace = [&](Vec3 anchor, bool sourceField) {
        const auto hist = traceGridWindows(bounded, anchor, probes, rcvClamped, sourceField,
                                           order, gridRays, execution, 1.);
        auto& chVec = sourceField ? sourceCh_ : listenerCh_;
        for (size_t i = 0; i < indices.size(); ++i) {
            const size_t cellIdx = indices[i];
            const auto& hOmni = hist.omni[i];
            auto& cell = cells_[cellIdx];
            auto& envelope = sourceField ? cell.source : cell.listener;
            for (int w = 0; w < kBakeWinCount; ++w)
                for (int b = 0; b < NB; ++b)
                    envelope[w][b] = static_cast<float>(std::sqrt(hOmni[b][w]));

            for (int r = 0; r < channels_; ++r) {
                const auto& hCh = hist.ch[i * (size_t)channels_ + (size_t)r];
                auto& chEnv = chVec[cellIdx * (size_t)channels_ + (size_t)r];
                for (int w = 0; w < kBakeWinCount; ++w)
                    for (int b = 0; b < NB; ++b)
                        chEnv[w][b] = static_cast<float>(std::sqrt(hCh[b][w]));
            }
        }
    };
    trace(listener, true);
    trace(source, false);
    // Edge corners borrow the nearest interior field. The query still sees
    // its actual corner and discounts it when interpolating across a wall.
    for (size_t cellIdx = 0; cellIdx < cells_.size(); ++cellIdx) {
        auto& cell = cells_[cellIdx];
        if (cell.interior) continue;
        size_t nearest = indices.front();
        double best = std::numeric_limits<double>::infinity();
        for (size_t index : indices) {
            const Vec3 p = cells_[index].position;
            const double score = norm(cell.position - p) + (blocked(cell.position, p) ? 20. : 0.);
            if (score < best) { best = score; nearest = index; }
        }
        cell.source = cells_[nearest].source;
        cell.listener = cells_[nearest].listener;
        for (int r = 0; r < channels_; ++r) {
            sourceCh_[cellIdx * (size_t)channels_ + (size_t)r] = sourceCh_[nearest * (size_t)channels_ + (size_t)r];
            listenerCh_[cellIdx * (size_t)channels_ + (size_t)r] = listenerCh_[nearest * (size_t)channels_ + (size_t)r];
        }
    }
}

bool MotionGrid::blocked(Vec3 a, Vec3 b) const noexcept {
    const Vec3 d = b - a;
    for (const auto& wall : walls_) {
        if ((a.x < wall.ax && a.x < wall.bx && b.x < wall.ax && b.x < wall.bx) ||
            (a.x > wall.ax && a.x > wall.bx && b.x > wall.ax && b.x > wall.bx) ||
            (a.y < wall.ay && a.y < wall.by && b.y < wall.ay && b.y < wall.by) ||
            (a.y > wall.ay && a.y > wall.by && b.y > wall.ay && b.y > wall.by)) continue;
        const double ex = wall.bx - wall.ax, ey = wall.by - wall.ay;
        const double den = d.x * ey - d.y * ex;
        if (std::abs(den) < 1e-10) continue;
        const double wx = wall.ax - a.x, wy = wall.ay - a.y;
        const double t = (wx * ey - wy * ex) / den;
        const double u = (wx * d.y - wy * d.x) / den;
        const double z = a.z + t * d.z;
        if (t > 1e-5 && t < 1. - 1e-5 && u > 1e-5 && u < 1. - 1e-5 &&
            z >= wall.z0 && z <= wall.z1) return true;
    }
    return false;
}

MotionEnvelope MotionGrid::lookup(Vec3 p, bool source, MotionEnvelope* outCh, int maxCh) const noexcept {
    MotionEnvelope out{};
    const int nCh = (outCh && maxCh > 0) ? std::min(maxCh, channels_) : 0;
    for (int r = 0; r < nCh; ++r) outCh[r] = MotionEnvelope{};
    if (cells_.empty() || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return out;
    const double x = std::clamp((p.x - x0_) / dx_ - .5, 0., double(nx_ - 1));
    const double y = std::clamp((p.y - y0_) / dy_ - .5, 0., double(ny_ - 1));
    const int ix = static_cast<int>(x), iy = static_cast<int>(y);
    const double fx = x - ix, fy = y - iy;
    const double fz = layers_ == 2 ? std::clamp((p.z - heights_[0]) / (heights_[1] - heights_[0]), 0., 1.) : 0.;

    size_t cornerIdx[8];
    float cornerW[8];
    int nCorners = 0;
    float sum = 0.f;
    for (int z = 0; z < layers_; ++z) {
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                float w = static_cast<float>((i ? fx : 1. - fx) * (j ? fy : 1. - fy) * (z ? fz : 1. - fz));
                if (w == 0.f) continue;
                const size_t cIdx = static_cast<size_t>((z * ny_ + std::min(iy + j, ny_ - 1)) * nx_ +
                                                        std::min(ix + i, nx_ - 1));
                const auto& cell = cells_[cIdx];
                if (!cell.interior) w *= .05f;
                if (blocked(p, cell.position)) w *= .01f;
                sum += w;
                cornerIdx[nCorners] = cIdx;
                cornerW[nCorners] = w;
                ++nCorners;
                const auto& field = source ? cell.source : cell.listener;
                for (int win = 0; win < kBakeWinCount; ++win)
                    for (int b = 0; b < NB; ++b) out[win][b] += w * field[win][b];
            }
        }
    }
    const float invSum = sum > 0.f ? (1.f / sum) : 0.f;
    for (auto& window : out)
        for (float& band : window) band *= invSum;

    if (nCh > 0) {
        const auto& chFieldVec = source ? sourceCh_ : listenerCh_;
        for (int k = 0; k < nCorners; ++k) {
            const float w = cornerW[k] * invSum;
            const size_t base = cornerIdx[k] * (size_t)channels_;
            for (int r = 0; r < nCh; ++r) {
                const auto& fCh = chFieldVec[base + (size_t)r];
                auto& dst = outCh[r];
                for (int win = 0; win < kBakeWinCount; ++win)
                    for (int b = 0; b < NB; ++b) dst[win][b] += w * fCh[win][b];
            }
        }
        for (int r = nCh; r < maxCh; ++r) outCh[r] = out;
    }
    return out;
}

MotionEnvelope MotionGrid::sourceField(Vec3 p) const noexcept { return lookup(p, true); }
MotionEnvelope MotionGrid::listenerField(Vec3 p) const noexcept { return lookup(p, false); }
MotionEnvelope MotionGrid::sourceFieldAndChannels(Vec3 p, MotionEnvelope* outCh, int maxCh) const noexcept {
    return lookup(p, true, outCh, maxCh);
}
MotionEnvelope MotionGrid::listenerFieldAndChannels(Vec3 p, MotionEnvelope* outCh, int maxCh) const noexcept {
    return lookup(p, false, outCh, maxCh);
}

void MotionGrid::applyCloseness(MotionEnvelope& envelope, const Transducer& source, Vec3 listener,
                                MotionEnvelope* chEnvelopes, int numCh) const noexcept {
    const Vec3 delta = listener - source.pos;
    const double distance = std::max(.4, norm(delta));
    if ((source.pattern == 0. && source.patternHf <= 0. && !source.profile()) || blocked(source.pos, listener)) return;
    const double proximity = std::exp(-distance / 3.5);
    double aim[NB];
    dirGainBands(source, unit(delta), aim);
    const double gain = std::pow(10., source.gainDb / 20.);
    for (int b = 0; b < NB; ++b) {
        const double p = source.patternHf < 0. ? source.pattern
            : source.pattern + (source.patternHf - source.pattern) * (double(b) / (NB - 1));
        const double mean = (1. - p) * (1. - p) + p * p / 3.;
        const double relative = aim[b] / gain;
        const double ratio = std::clamp(relative * relative / (source.profile() ? 1. : mean), .4, 2.2);
        const float f0 = static_cast<float>(std::sqrt(1. + .22 * proximity * (ratio - 1.)));
        const float f1 = static_cast<float>(std::sqrt(1. + .08 * proximity * (ratio - 1.)));
        envelope[0][b] *= f0;
        envelope[1][b] *= f1;
        if (chEnvelopes) {
            for (int r = 0; r < numCh; ++r) {
                chEnvelopes[r][0][b] *= f0;
                chEnvelopes[r][1][b] *= f1;
            }
        }
    }
}

} // namespace verb
