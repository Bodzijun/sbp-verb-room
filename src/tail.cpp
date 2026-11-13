#include "tail.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <numeric>
#include <stdexcept>

#include "dsp.hpp"
#include "geometry.hpp"

namespace verb {
namespace {

// Complementary octave filterbank: the sum of all bands is exactly a (delayed) unit impulse.
std::vector<std::vector<double>> makeBandFilters(double sr, int taps) {
    const int M = (taps - 1) / 2;
    std::vector<std::vector<double>> lp(NB - 1, std::vector<double>(taps));
    for (int i = 0; i < NB - 1; ++i) {
        double fc = kBandHz[i] * std::sqrt(2.0) / sr;  // band edge, cycles/sample
        double sum = 0;
        for (int n = 0; n < taps; ++n) {
            double x = n - M;
            double s = std::fabs(x) < 1e-9 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
            double w = 0.42 - 0.5 * std::cos(2 * kPi * n / (taps - 1)) + 0.08 * std::cos(4 * kPi * n / (taps - 1));
            lp[i][n] = s * w;
            sum += lp[i][n];
        }
        for (auto& v : lp[i]) v /= sum;
    }
    std::vector<std::vector<double>> bands(NB, std::vector<double>(taps, 0.0));
    for (int n = 0; n < taps; ++n) {
        bands[0][n] = lp[0][n];
        for (int i = 1; i < NB - 1; ++i) bands[i][n] = lp[i][n] - lp[i - 1][n];
        bands[NB - 1][n] = (n == M ? 1.0 : 0.0) - lp[NB - 2][n];
    }
    return bands;
}


// ---- the receiver's detection sphere (E3 part B) ----
// The late field is estimated as the ray path length inside a sphere around the receiver divided by the sphere's volume.
// That is only right where the whole sphere is reachable by the sound. Where it sticks out through a wall (a small
// booth, a listener near a wall or a partition) or through the floor / ceiling, the part behind the boundary is another
// space: rays travelling there must not be counted, and the volume to divide by is the reachable part of the sphere.
// `clean` spheres (nothing within the radius) keep the original arithmetic exactly.
struct SphereClip {
    struct WallBlock { double ax, ay, ex, ey, z0, z1, sideC; };  // sideC: which side of the wall's line the centre is on
    bool clean = true;
    std::vector<WallBlock> walls;
    std::vector<int> plates;         // plates within the radius (indices into Geo::plates)
    bool floorNear = false, ceilNear = false;
    double reach = 1.0;              // reachable fraction of the sphere volume (1 for a clean sphere)
};

// Can the sound at q reach the centre c without leaving the space it is in (straight line, no wall / floor / ceiling /
// plate in between)? Only the blockers recorded in `sc` are tested.
static bool reachable(const Geo& g, const SphereClip& sc, Vec3 c, Vec3 q) {
    if (sc.floorNear && q.z < 0.0) return false;
    if (sc.ceilNear && q.z > g.H) {
        const double t = (g.H - c.z) / (q.z - c.z);
        const Vec3 P = c + (q - c) * t;
        if (g.matAt(g.ceilId(), P.x, P.y)) return false;  // nullptr = an opening in the ceiling: the sound passes
    }
    const double rx = q.x - c.x, ry = q.y - c.y, dz = q.z - c.z;
    for (int k : sc.plates) {
        const Zone& pl = *g.plates[(size_t)k];
        if (std::fabs(dz) < 1e-12) continue;
        const double t = (pl.z - c.z) / dz;
        if (t > 0.0 && t < 1.0 && pointInZone(pl, c.x + t * rx, c.y + t * ry)) return false;
    }
    for (const auto& w : sc.walls) {
        if (cross2(w.ex, w.ey, q.x - w.ax, q.y - w.ay) * w.sideC > 0.0) continue;  // same side as the centre: not crossed
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

static double halton(int i, int base) {
    double f = 1.0, r = 0.0;
    for (; i > 0; i /= base) { f /= base; r += f * (double)(i % base); }
    return r;
}

static SphereClip makeSphereClip(const Geo& g, Vec3 c, double rd) {
    SphereClip sc;
    for (int i = 0; i < g.W; ++i) {
        const Wall& w = g.s.walls[(size_t)i];
        if (!wallBlocks(g, w)) continue;
        if (w.z1 <= c.z - rd || w.z0 >= c.z + rd) continue;  // entirely below / above the sphere
        const double ex = w.bx - w.ax, ey = w.by - w.ay, l2 = ex * ex + ey * ey;
        double t = l2 > 1e-18 ? ((c.x - w.ax) * ex + (c.y - w.ay) * ey) / l2 : 0.0;
        t = std::min(1.0, std::max(0.0, t));
        if (std::hypot(c.x - (w.ax + t * ex), c.y - (w.ay + t * ey)) < rd) {
            const double side = cross2(ex, ey, c.x - w.ax, c.y - w.ay);
            sc.walls.push_back({w.ax, w.ay, ex, ey, std::max(0.0, w.z0), std::min(w.z1, g.H), side > 0 ? 1.0 : side < 0 ? -1.0 : 0.0});
        }
    }
    for (int k = 0; k < (int)g.plates.size(); ++k)
        if (g.plates[(size_t)k]->z > c.z - rd && g.plates[(size_t)k]->z < c.z + rd) sc.plates.push_back(k);
    sc.floorNear = c.z - rd < 0.0;
    sc.ceilNear = g.hasCeil && c.z + rd > g.H;
    sc.clean = sc.walls.empty() && sc.plates.empty() && !sc.floorNear && !sc.ceilNear;
    if (sc.clean) return sc;
    // reachable fraction of the sphere: a fixed (deterministic, independent of the ray generator) Halton point set
    int n = 0, hit = 0;
    for (int i = 1; n < 6000 && i < 40000; ++i) {
        const Vec3 u{2.0 * halton(i, 2) - 1.0, 2.0 * halton(i, 3) - 1.0, 2.0 * halton(i, 5) - 1.0};
        if (dot(u, u) > 1.0) continue;
        ++n;
        if (reachable(g, sc, c, c + u * rd)) ++hit;
    }
    sc.reach = std::max(0.02, n > 0 ? (double)hit / n : 1.0);
    return sc;
}

}  // namespace

// E4a: options of the cheap motion estimate. Default-constructed = the production trace, unchanged.
struct TraceCRN {
    bool perRay = false;   // ray i draws from its own generator seeded from (seed, i): common random numbers between poses
    double stopRel = 0;    // > 0: a ray ends once its band-maximum energy has fallen below stopRel of a full-sphere start
    std::vector<double> rayAcc, sumSq;  // per-ray totals [r*NB+b] and the sum of their squares (variance of the estimate)
    long long work = 0;    // out: deterministic work units (ray segments + receiver records)
};

static TailHist traceTailImpl(const Scene& sc, const Transducer& src, const std::vector<Transducer>& rcv, int order, int rays, int seed, bool finalize,
    bool integrated = false, const std::function<bool()>& cancelled = {}, const std::function<void(double)>& progress = {}, TraceCRN* crn = nullptr) {
    const Geo g(sc);
    const int R = (int)rcv.size();
    const double c = speedOfSound(sc.tempC);
    std::array<double, NB> airDb;
    for (int b = 0; b < NB; ++b) airDb[b] = airAbsDbPerM(kBandHz[b], sc.tempC, sc.humidity);

    TailHist out;
    out.binDur = 0.001;
    out.nBins = integrated ? 1 : (size_t)std::ceil(sc.length / out.binDur) + 2;
    out.h.resize(R);
    for (auto& r : out.h)
        for (auto& b : r) b.assign(out.nBins, 0.0);
    out.h2.resize(R);
    for (auto& r : out.h2)
        for (auto& b : r) b.assign(out.nBins, 0.0);
    out.reliableEnd.resize(R);
    for (auto& r : out.reliableEnd) r.fill(out.nBins);

    std::mt19937_64 rng((uint64_t)seed * 7919ull + 13);
    std::uniform_real_distribution<double> U(0, 1);
    const double maxPath = c * sc.length;
    // The receiver sphere grows with the room: in a hall only a handful of rays cross a 0.5 m sphere per millisecond,
    // and the late histogram turns into Poisson noise (heard as modulation / fragmentation of the tail). The late field
    // is diffuse, so averaging over a larger sphere loses nothing.
    double rd = sc.detectRadius;
    {
        double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
        for (const auto& w : sc.walls) { x0 = std::min({x0, w.ax, w.bx}); x1 = std::max({x1, w.ax, w.bx}); y0 = std::min({y0, w.ay, w.by}); y1 = std::max({y1, w.ay, w.by}); }
        if (x1 > x0 && y1 > y0) {
            const double vol = (x1 - x0) * (y1 - y0) * std::max(1.0, std::min(sc.height, 40.0));
            rd = std::min(3.0, std::max(rd, 0.05 * std::cbrt(vol)));
        }
    }
    const double Vs = 4.0 / 3.0 * kPi * rd * rd * rd;
    std::vector<SphereClip> clip((size_t)R);
    for (int r = 0; r < R; ++r) clip[(size_t)r] = makeSphereClip(g, rcv[(size_t)r].pos, rd);
    // Spatial queries are used only for analysis. Clip each segment to the
    // receivers' height slab, then search their X-sorted sphere bounds.
    std::vector<int> receiverOrder;
    double rz0 = 1e30, rz1 = -1e30;
    if (integrated) {
        receiverOrder.resize((size_t)R); std::iota(receiverOrder.begin(), receiverOrder.end(), 0);
        std::sort(receiverOrder.begin(), receiverOrder.end(), [&](int a, int b) { return rcv[a].pos.x < rcv[b].pos.x; });
        for (const auto& r : rcv) { rz0 = std::min(rz0, r.pos.z - rd); rz1 = std::max(rz1, r.pos.z + rd); }
    }
    for (int i = 0; i < rays; ++i) {
        if (i % 32 == 0) {
            if (cancelled && cancelled()) throw std::runtime_error("Analysis cancelled");
            if (progress) progress((double)i / rays);
        }
        if (crn && crn->perRay) rng.seed((uint64_t)seed * 7919ull + 13 + 0x9E3779B97F4A7C15ull * (uint64_t)(i + 1));
        if (crn) crn->rayAcc.assign((size_t)R * NB, 0.0);
        double zc = 2 * U(rng) - 1, ph = 2 * kPi * U(rng), sr_ = std::sqrt(1 - zc * zc);
        Vec3 d{sr_ * std::cos(ph), sr_ * std::sin(ph), zc};
        double gs = dirGain(src, d);
        std::array<double, NB> e;
        e.fill(gs * gs / rays);
        Vec3 p = src.pos;
        double path = 0;
        int refl = 0;
        int steps = 0;
        while (true) {
            if (++steps % 32 == 0 && cancelled && cancelled()) throw std::runtime_error("Analysis cancelled");
            if (crn) crn->work += 1 + R;
            double tBest = std::numeric_limits<double>::infinity();
            int surf = -1;
            for (int w = 0; w < g.W; ++w) {
                const Wall& wl = sc.walls[w];
                double ex = wl.bx - wl.ax, ey = wl.by - wl.ay;
                double den = cross2(d.x, d.y, ex, ey);
                if (std::fabs(den) < 1e-12) continue;
                double t = cross2(wl.ax - p.x, wl.ay - p.y, ex, ey) / den;
                double u = cross2(wl.ax - p.x, wl.ay - p.y, d.x, d.y) / den;
                if (t > 1e-7 && t < tBest && u >= 0 && u <= 1) {
                    double z = p.z + d.z * t;
                    if (z >= std::max(0.0, wl.z0) && z <= std::min(wl.z1, g.H)) { tBest = t; surf = w; }  // over a low wall: no hit
                }
            }
            if (d.z < -1e-12) {
                double t = -p.z / d.z;
                if (t > 1e-7 && t < tBest) { tBest = t; surf = g.floorId(); }
            }
            if (g.hasCeil && d.z > 1e-12) {
                double t = (g.H - p.z) / d.z;
                if (t > 1e-7 && t < tBest) { tBest = t; surf = g.ceilId(); }
            }
            for (int k = 0; k < (int)g.plates.size() && std::fabs(d.z) > 1e-12; ++k) {  // horizontal plates
                const Zone& pl = *g.plates[(size_t)k];
                const double t = (pl.z - p.z) / d.z;
                if (t > 1e-7 && t < tBest && pointInZone(pl, p.x + d.x * t, p.y + d.y * t)) { tBest = t; surf = g.W + 2 + k; }
            }
            double L = surf < 0 ? maxPath - path : std::min(tBest, maxPath - path);
            if (L <= 0) break;
            if (refl > order) {
                auto record = [&](int r) {
                    Vec3 oc = p - rcv[r].pos;
                    double bq = dot(oc, d), cc = dot(oc, oc) - rd * rd, disc = bq * bq - cc;
                    if (disc <= 0) return;
                    double sq = std::sqrt(disc);
                    double s1 = std::max(-bq - sq, 0.0), s2 = std::min(-bq + sq, L);
                    if (s2 <= s1) return;
                    double mid = path + 0.5 * (s1 + s2);
                    double chord = s2 - s1, vol = Vs;
                    const SphereClip& sphere = clip[(size_t)r];
                    if (!sphere.clean) {  // count only the part of the chord the receiver can actually be reached from
                        // Three probes first (both ends and the middle of the chord): all hidden or all reachable ends
                        // the question; a chord that crosses a boundary is cut into 8 pieces, each judged at its middle.
                        const Vec3 c0 = rcv[r].pos;
                        const bool v0 = reachable(g, sphere, c0, p + d * s1), v1 = reachable(g, sphere, c0, p + d * (0.5 * (s1 + s2))),
                                   v2 = reachable(g, sphere, c0, p + d * s2);
                        if (v0 == v1 && v1 == v2) {
                            if (!v0) return;
                        } else {
                            constexpr int K = 8;
                            const double ds = chord / K;
                            int seen = 0;
                            for (int k = 0; k < K; ++k)
                                if (reachable(g, sphere, c0, p + d * (s1 + (k + 0.5) * ds))) ++seen;
                            if (seen == 0) return;
                            chord = ds * seen;
                        }
                        vol = Vs * sphere.reach;
                    }
                    size_t bin = integrated ? 0 : (size_t)(mid / c / out.binDur);
                    if (bin >= out.nBins) return;
                    double grb[NB];
                    dirGainBands(rcv[r], d * -1.0, grb);
                    for (int b = 0; b < NB; ++b) {
                        const double gr = grb[b];
                        const double contrib = 4 * kPi * e[b] * std::pow(10.0, -airDb[b] * mid / 10.0) * chord / vol * gr * gr;
                        out.h[r][b][bin] += contrib;
                        out.h2[r][b][bin] += contrib * contrib;
                        if (crn) crn->rayAcc[(size_t)r * NB + b] += contrib;
                    }
                };
                if (!integrated) {
                    for (int r = 0; r < R; ++r) record(r);
                } else if (R > 0) {
                    double a = 0, b = L;
                    if (std::fabs(d.z) > 1e-12) {
                        double lo = (rz0 - p.z) / d.z, hi = (rz1 - p.z) / d.z;
                        if (lo > hi) std::swap(lo, hi);
                        a = std::max(a, lo); b = std::min(b, hi);
                    } else if (p.z < rz0 || p.z > rz1) b = -1;
                    if (b >= a) {
                        const Vec3 q0 = p + d * a, q1 = p + d * b;
                        const double x0 = std::min(q0.x, q1.x) - rd, x1 = std::max(q0.x, q1.x) + rd;
                        const double y0 = std::min(q0.y, q1.y) - rd, y1 = std::max(q0.y, q1.y) + rd;
                        auto it = std::lower_bound(receiverOrder.begin(), receiverOrder.end(), x0,
                            [&](int r, double x) { return rcv[r].pos.x < x; });
                        for (; it != receiverOrder.end() && rcv[*it].pos.x <= x1; ++it)
                            if (rcv[*it].pos.y >= y0 && rcv[*it].pos.y <= y1) record(*it);
                    }
                }
            }
            if (surf < 0 || tBest > maxPath - path) break;
            p = p + d * tBest;
            path += tBest;
            const Material* mp = g.matAt(surf, p.x, p.y);  // floor/ceiling zones are looked up at the hit point
            if (!mp) { p = p + d * 1e-6; continue; }       // an opening in the ceiling: the ray leaves
            const Material& m = *mp;
            // Each hit: transmitted (continues straight through, walls only), reflected, or absorbed. One outcome is
            // drawn with probability proportional to the band-averaged shares; band energies are re-weighted so the
            // expectation per band is exact.
            double Tm = 0, Rm = 0;
            std::array<double, NB> rho;
            for (int b = 0; b < NB; ++b) {
                rho[b] = reflEnergy(m, b);
                Rm += rho[b] / NB;
                if (surf < g.W || g.isPlate(surf)) Tm += m.tau[b] / NB;  // walls and plates let some sound through
            }
            if (Tm + Rm <= 1e-12) break;  // fully absorbed
            const double pT = Tm / (Tm + Rm);
            if (pT > 0 && U(rng) < pT) {
                for (int b = 0; b < NB; ++b) e[b] *= m.tau[b] / pT;
                p = p + d * 1e-6;  // step through the wall; not a reflection
                continue;
            }
            double emax = 0;
            for (int b = 0; b < NB; ++b) {
                e[b] *= rho[b] / (1.0 - pT);
                emax = std::max(emax, e[b] * std::pow(10.0, -airDb[b] * path / 10.0));
            }
            ++refl;
            if (emax < 1e-10 / rays) break;
            if (crn && crn->stopRel > 0 && emax < crn->stopRel / rays) break;
            Vec3 n;
            if (surf < g.W) {
                const Wall& wl = sc.walls[surf];
                double ex = wl.bx - wl.ax, ey = wl.by - wl.ay, l = std::sqrt(ex * ex + ey * ey);
                n = {-ey / l, ex / l, 0};
            } else n = {0, 0, surf == g.floorId() ? 1.0 : -1.0};
            if (dot(n, d) > 0) n = n * -1.0;
            if (U(rng) < scatterAt(m, 3)) {  // one direction per ray: use the 1 kHz value
                double u1 = U(rng), u2 = 2 * kPi * U(rng), rr = std::sqrt(u1);
                Vec3 a = std::fabs(n.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
                Vec3 t1 = unit(Vec3{n.y * a.z - n.z * a.y, n.z * a.x - n.x * a.z, n.x * a.y - n.y * a.x});
                Vec3 t2{n.y * t1.z - n.z * t1.y, n.z * t1.x - n.x * t1.z, n.x * t1.y - n.y * t1.x};
                d = unit(t1 * (rr * std::cos(u2)) + t2 * (rr * std::sin(u2)) + n * std::sqrt(1 - u1));
            } else {
                d = unit(d - n * (2 * dot(d, n)));
            }
            p = p + n * 1e-6;
        }
        if (crn) {
            crn->sumSq.resize((size_t)R * NB, 0.0);
            for (size_t k = 0; k < crn->rayAcc.size(); ++k) crn->sumSq[k] += crn->rayAcc[k] * crn->rayAcc[k];
        }
    }
    if (finalize) finalizeTail(out);
    return out;
}

TailHist traceTail(const Scene& sc, const Transducer& src, const std::vector<Transducer>& rcv, int order, int rays, int seed, bool finalize, const std::function<bool()>& cancelled) {
    return traceTailImpl(sc, src, rcv, order, rays, seed, finalize, false, cancelled);
}

std::vector<std::array<double, NB>> traceTailEnergy(const Scene& sc, const Transducer& src, const std::vector<Transducer>& rcv,
    int order, int rays, int seed, const std::function<bool()>& cancelled, const std::function<void(double)>& progress) {
    const auto hist = traceTailImpl(sc, src, rcv, order, rays, seed, false, true, cancelled, progress);
    std::vector<std::array<double, NB>> result(rcv.size());
    for (size_t r = 0; r < rcv.size(); ++r) for (int b = 0; b < NB; ++b) result[r][b] = hist.h[r][b][0];
    if (progress) progress(1.0);
    return result;
}

std::vector<std::array<double, NB>> traceTailEnergyCRN(const Scene& sc, const Transducer& src, const std::vector<Transducer>& rcv,
    int order, int rays, int seed, double stopDb, const std::function<bool()>& cancelled, long long* work, std::vector<std::array<double, NB>>* sumSq) {
    TraceCRN crn;
    crn.perRay = true;
    crn.stopRel = stopDb < 0 ? std::pow(10.0, stopDb / 10.0) : 0.0;
    const auto hist = traceTailImpl(sc, src, rcv, order, rays, seed, false, true, cancelled, {}, &crn);
    std::vector<std::array<double, NB>> result(rcv.size());
    for (size_t r = 0; r < rcv.size(); ++r) for (int b = 0; b < NB; ++b) result[r][b] = hist.h[r][b][0];
    if (work) *work = crn.work;
    if (sumSq) {
        sumSq->assign(rcv.size(), std::array<double, NB>{});
        for (size_t r = 0; r < rcv.size(); ++r) for (int b = 0; b < NB; ++b) (*sumSq)[r][(size_t)b] = r * NB + b < crn.sumSq.size() ? crn.sumSq[r * NB + b] : 0.0;
    }
    return result;
}

// E4a: reciprocal (reverse) late-energy estimate for ALL sources at once. Rays start at the listener (head centre) and the
// sources' detection spheres record them; by reciprocity this is the source->receiver late transfer. Each ray carries one
// weight set per ear (the ear's pattern for the ray's direction), so one trace serves every source and both ears. The
// propagation (walls, plates, floor/ceiling zones, transmission/reflection/scatter sampling, air) is traceTail's, step for
// step; the detection sphere (with E3's clipping) sits round the source instead of the receiver. Ray i uses its own random
// generator seeded from (seed, i) (common random numbers between poses). Result [s * ears + r][band].
std::vector<std::array<double, NB>> traceLateReciprocal(const Scene& sc, Vec3 origin, const std::vector<Transducer>& ears,
    const std::vector<Transducer>& srcs, int order, int rays, int seed, double stopDb, const std::function<bool()>& cancelled, long long* work,
    std::vector<std::array<double, NB>>* sumSq) {
    const Geo g(sc);
    const int R = (int)ears.size(), S = (int)srcs.size();
    std::vector<std::array<double, NB>> out((size_t)S * R);
    for (auto& v : out) v.fill(0.0);
    if (rays <= 0 || R == 0 || S == 0) return out;
    const double c = speedOfSound(sc.tempC);
    std::array<double, NB> airDb;
    for (int b = 0; b < NB; ++b) airDb[b] = airAbsDbPerM(kBandHz[b], sc.tempC, sc.humidity);
    const double maxPath = c * sc.length;
    double rd = sc.detectRadius;  // as traceTail
    {
        double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
        for (const auto& w : sc.walls) { x0 = std::min({x0, w.ax, w.bx}); x1 = std::max({x1, w.ax, w.bx}); y0 = std::min({y0, w.ay, w.by}); y1 = std::max({y1, w.ay, w.by}); }
        if (x1 > x0 && y1 > y0) {
            const double vol = (x1 - x0) * (y1 - y0) * std::max(1.0, std::min(sc.height, 40.0));
            rd = std::min(3.0, std::max(rd, 0.05 * std::cbrt(vol)));
        }
    }
    const double Vs = 4.0 / 3.0 * kPi * rd * rd * rd;
    std::vector<SphereClip> clip((size_t)S);
    for (int s = 0; s < S; ++s) clip[(size_t)s] = makeSphereClip(g, srcs[(size_t)s].pos, rd);
    std::vector<int> byX((size_t)S);
    std::iota(byX.begin(), byX.end(), 0);
    std::sort(byX.begin(), byX.end(), [&](int a, int b) { return srcs[(size_t)a].pos.x < srcs[(size_t)b].pos.x; });
    double sz0 = 1e30, sz1 = -1e30;
    for (const auto& t : srcs) { sz0 = std::min(sz0, t.pos.z - rd); sz1 = std::max(sz1, t.pos.z + rd); }
    const double stopRel = stopDb < 0 ? std::pow(10.0, stopDb / 10.0) : 0.0;
    long long units = 0;
    std::mt19937_64 rng;
    std::uniform_real_distribution<double> U(0, 1);
    std::vector<double> w((size_t)R * NB);
    std::vector<std::array<double, NB>> rayAcc;     // this ray's totals, for the variance
    std::vector<int> touched;
    std::vector<char> isTouched;
    if (sumSq) { sumSq->assign((size_t)S * R, std::array<double, NB>{}); rayAcc.assign((size_t)S * R, std::array<double, NB>{}); isTouched.assign((size_t)S, 0); }
    for (int i = 0; i < rays; ++i) {
        if (i % 32 == 0 && cancelled && cancelled()) throw std::runtime_error("Analysis cancelled");
        rng.seed((uint64_t)seed * 7919ull + 13 + 0x9E3779B97F4A7C15ull * (uint64_t)(i + 1));
        double zc = 2 * U(rng) - 1, ph = 2 * kPi * U(rng), sr_ = std::sqrt(1 - zc * zc);
        Vec3 d{sr_ * std::cos(ph), sr_ * std::sin(ph), zc};
        for (int r = 0; r < R; ++r) {
            double grb[NB];
            dirGainBands(ears[(size_t)r], d, grb);  // the sound that reaches ear r from direction d
            for (int b = 0; b < NB; ++b) w[(size_t)r * NB + b] = grb[b] * grb[b] / rays;
        }
        std::array<double, NB> f;
        f.fill(1.0);
        Vec3 p = origin;
        double path = 0;
        int refl = 0, steps = 0;
        while (true) {
            if (++steps % 32 == 0 && cancelled && cancelled()) throw std::runtime_error("Analysis cancelled");
            ++units;
            double tBest = std::numeric_limits<double>::infinity();
            int surf = -1;
            for (int wI = 0; wI < g.W; ++wI) {
                const Wall& wl = sc.walls[wI];
                double ex = wl.bx - wl.ax, ey = wl.by - wl.ay;
                double den = cross2(d.x, d.y, ex, ey);
                if (std::fabs(den) < 1e-12) continue;
                double t = cross2(wl.ax - p.x, wl.ay - p.y, ex, ey) / den;
                double u = cross2(wl.ax - p.x, wl.ay - p.y, d.x, d.y) / den;
                if (t > 1e-7 && t < tBest && u >= 0 && u <= 1) {
                    double z = p.z + d.z * t;
                    if (z >= std::max(0.0, wl.z0) && z <= std::min(wl.z1, g.H)) { tBest = t; surf = wI; }
                }
            }
            if (d.z < -1e-12) {
                double t = -p.z / d.z;
                if (t > 1e-7 && t < tBest) { tBest = t; surf = g.floorId(); }
            }
            if (g.hasCeil && d.z > 1e-12) {
                double t = (g.H - p.z) / d.z;
                if (t > 1e-7 && t < tBest) { tBest = t; surf = g.ceilId(); }
            }
            for (int k = 0; k < (int)g.plates.size() && std::fabs(d.z) > 1e-12; ++k) {
                const Zone& pl = *g.plates[(size_t)k];
                const double t = (pl.z - p.z) / d.z;
                if (t > 1e-7 && t < tBest && pointInZone(pl, p.x + d.x * t, p.y + d.y * t)) { tBest = t; surf = g.W + 2 + k; }
            }
            double L = surf < 0 ? maxPath - path : std::min(tBest, maxPath - path);
            if (L <= 0) break;
            if (refl > order) {
                double a = 0, bEnd = L;
                if (std::fabs(d.z) > 1e-12) {
                    double lo = (sz0 - p.z) / d.z, hi = (sz1 - p.z) / d.z;
                    if (lo > hi) std::swap(lo, hi);
                    a = std::max(a, lo); bEnd = std::min(bEnd, hi);
                } else if (p.z < sz0 || p.z > sz1) bEnd = -1;
                if (bEnd >= a) {
                    const Vec3 q0 = p + d * a, q1 = p + d * bEnd;
                    const double xa = std::min(q0.x, q1.x) - rd, xb = std::max(q0.x, q1.x) + rd;
                    const double ya = std::min(q0.y, q1.y) - rd, yb = std::max(q0.y, q1.y) + rd;
                    auto it = std::lower_bound(byX.begin(), byX.end(), xa, [&](int s, double x) { return srcs[(size_t)s].pos.x < x; });
                    for (; it != byX.end() && srcs[(size_t)*it].pos.x <= xb; ++it) {
                        const int s = *it;
                        const Transducer& st = srcs[(size_t)s];
                        if (st.pos.y < ya || st.pos.y > yb) continue;
                        ++units;
                        Vec3 oc = p - st.pos;
                        double bq = dot(oc, d), cc = dot(oc, oc) - rd * rd, disc = bq * bq - cc;
                        if (disc <= 0) continue;
                        double sq = std::sqrt(disc);
                        double s1 = std::max(-bq - sq, 0.0), s2 = std::min(-bq + sq, L);
                        if (s2 <= s1) continue;
                        double mid = path + 0.5 * (s1 + s2);
                        double chord = s2 - s1, vol = Vs;
                        const SphereClip& sphere = clip[(size_t)s];
                        if (!sphere.clean) {
                            const Vec3 c0 = st.pos;
                            const bool v0 = reachable(g, sphere, c0, p + d * s1), v1 = reachable(g, sphere, c0, p + d * (0.5 * (s1 + s2))),
                                       v2 = reachable(g, sphere, c0, p + d * s2);
                            if (v0 == v1 && v1 == v2) {
                                if (!v0) continue;
                            } else {
                                constexpr int K = 8;
                                const double ds = chord / K;
                                int seen = 0;
                                for (int k = 0; k < K; ++k)
                                    if (reachable(g, sphere, c0, p + d * (s1 + (k + 0.5) * ds))) ++seen;
                                if (seen == 0) continue;
                                chord = ds * seen;
                            }
                            vol = Vs * sphere.reach;
                        }
                        const double gs = dirGain(st, d * -1.0);  // the source sends this energy along -d
                        for (int b = 0; b < NB; ++b) {
                            const double base = 4 * kPi * f[(size_t)b] * std::pow(10.0, -airDb[(size_t)b] * mid / 10.0) * chord / vol * gs * gs;
                            for (int r = 0; r < R; ++r) out[(size_t)s * R + r][(size_t)b] += base * w[(size_t)r * NB + b];
                            if (sumSq) for (int r = 0; r < R; ++r) rayAcc[(size_t)s * R + r][(size_t)b] += base * w[(size_t)r * NB + b];
                        }
                        if (sumSq && !isTouched[(size_t)s]) { isTouched[(size_t)s] = 1; touched.push_back(s); }
                    }
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
                rho[(size_t)b] = reflEnergy(m, b);
                Rm += rho[(size_t)b] / NB;
                if (surf < g.W || g.isPlate(surf)) Tm += m.tau[b] / NB;
            }
            if (Tm + Rm <= 1e-12) break;
            const double pT = Tm / (Tm + Rm);
            if (pT > 0 && U(rng) < pT) {
                for (int b = 0; b < NB; ++b) f[(size_t)b] *= m.tau[b] / pT;
                p = p + d * 1e-6;
                continue;
            }
            double emax = 0;
            for (int b = 0; b < NB; ++b) {
                f[(size_t)b] *= rho[(size_t)b] / (1.0 - pT);
                emax = std::max(emax, f[(size_t)b] * std::pow(10.0, -airDb[(size_t)b] * path / 10.0));
            }
            ++refl;
            if (emax < 1e-10 || emax < stopRel) break;
            Vec3 n;
            if (surf < g.W) {
                const Wall& wl = sc.walls[surf];
                double ex = wl.bx - wl.ax, ey = wl.by - wl.ay, l = std::sqrt(ex * ex + ey * ey);
                n = {-ey / l, ex / l, 0};
            } else n = {0, 0, surf == g.floorId() ? 1.0 : -1.0};
            if (dot(n, d) > 0) n = n * -1.0;
            if (U(rng) < scatterAt(m, 3)) {
                double u1 = U(rng), u2 = 2 * kPi * U(rng), rr = std::sqrt(u1);
                Vec3 a = std::fabs(n.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
                Vec3 t1 = unit(Vec3{n.y * a.z - n.z * a.y, n.z * a.x - n.x * a.z, n.x * a.y - n.y * a.x});
                Vec3 t2{n.y * t1.z - n.z * t1.y, n.z * t1.x - n.x * t1.z, n.x * t1.y - n.y * t1.x};
                d = unit(t1 * (rr * std::cos(u2)) + t2 * (rr * std::sin(u2)) + n * std::sqrt(1 - u1));
            } else {
                d = unit(d - n * (2 * dot(d, n)));
            }
            p = p + n * 1e-6;
        }
        if (sumSq) {
            for (int s : touched) {
                for (int r = 0; r < R; ++r) for (int b = 0; b < NB; ++b) {
                    double& a = rayAcc[(size_t)s * R + r][(size_t)b];
                    (*sumSq)[(size_t)s * R + r][(size_t)b] += a * a;
                    a = 0;
                }
                isTouched[(size_t)s] = 0;
            }
            touched.clear();
        }
    }
    if (work) *work = units;
    return out;
}

static void smoothTailTransition(TailHist& out, const std::vector<std::array<std::vector<double>,NB>>& raw) {
    if(out.h.empty()||out.h2.size()!=out.h.size())return;
    const auto before=analyzeTail(out);
    const auto original=out.h;
    const size_t stop=std::min(out.nBins,(size_t)std::ceil(.65/out.binDur));
    for(int b=0;b<NB;++b) {
        const double rt=before.rt60[b];
        if(rt<.12||!std::isfinite(rt))continue; // insufficient temporal support
        const double rate=6.*std::log(10.)/rt;
        std::vector<std::vector<double>> mixWeights(out.h.size());
        std::vector<size_t> onsets(out.h.size());
        for(size_t r=0;r<out.h.size();++r) {
            const auto& src=original[r][b];const auto& moment=out.h2[r][b];
            if(moment.size()!=out.nBins)continue;
            size_t onset=0;while(onset<src.size()&&src[onset]<=0)++onset;
            if(onset>=stop)continue;
            std::vector<double> prefix(out.nBins+1),ess(out.nBins+1),candidate(stop),weights(stop);
            std::vector<bool> protectedBin(stop,false);
            for(size_t k=0;k<out.nBins;++k) {
                prefix[k+1]=prefix[k]+src[k];
                const double e=raw[r][b][k],e2=moment[k];
                ess[k+1]=ess[k]+(e2>0?e*e/e2:0.);
            }
            // A distinct high-confidence reflection remains discrete. Low-ESS
            // ray survivors cannot qualify merely by being loud in one bin.
            for(size_t k=onset;k<stop;++k) {
                const size_t lo=k>15?k-15:0,hi=std::min(out.nBins,k+16);
                const double local=(prefix[hi]-prefix[lo])/(hi-lo);
                const double e=raw[r][b][k],e2=moment[k];
                if(e2>0&&e*e/e2>=32.&&e>6.*local)
                    for(size_t j=k>3?k-3:0;j<std::min(stop,k+4);++j)protectedBin[j]=true;
            }
            double oldArea=0,newArea=0;
            for(size_t k=onset;k<stop;++k) {
                const size_t lo=k>5?k-5:0,hi=std::min(out.nBins,k+6);
                const double density=(ess[hi]-ess[lo])/(hi-lo);
                // About 100 effective contributions per kernel standard deviation;
                // 16..80 ms sigma, wider where a few rays carry the envelope.
                const double sigma=std::min(rt/(6.*out.binDur),std::clamp(.100/out.binDur/std::sqrt(std::max(1.,density)),.016/out.binDur,.080/out.binDur));
                const size_t radius=(size_t)std::ceil(3.*sigma);
                const size_t a=k>radius?std::max(onset,k-radius):onset,z=std::min(out.nBins,k+radius+1);
                double sum=0,norm=0;
                for(size_t j=a;j<z;++j) {
                    const double delta=(double)j-(double)k;
                    const double w=std::exp(-.5*delta*delta/(sigma*sigma));
                    // Removing/restoring the physical slope makes an exponential
                    // a fixed point of the smoother, even in a short studio tail.
                    sum+=src[j]*std::exp(rate*delta*out.binDur)*w;norm+=w;
                }
                candidate[k]=norm>0?sum/norm:src[k];
                const double t=k*out.binDur;
                const double w=protectedBin[k]||src[k]<=0?0.:std::min(std::clamp(((k-onset)*out.binDur-.005)/.025,0.,1.),std::clamp((.65-t)/.15,0.,1.));
                weights[k]=w;oldArea+=w*src[k];newArea+=w*candidate[k];
            }
            const double scale=newArea>0?oldArea/newArea:1.;
            auto& dst=out.h[r][b];
            for(size_t k=onset;k<stop;++k)dst[k]=(1.-weights[k])*src[k]+weights[k]*candidate[k]*scale;
            mixWeights[r]=std::move(weights);onsets[r]=onset;
        }
        // Restore the measured physical slope by tilting only the redistributed
        // component, renormalizing its area on every accepted trial. This keeps
        // short high-frequency tails smooth instead of undoing the smoothing.
        for(int iteration=0;iteration<8;++iteration) {
            const double actual=analyzeTail(out).rt60[b];
            if(actual<=0||std::abs(actual/rt-1.)<=.005)break;
            const double correction=std::clamp(6.*std::log(10.)*(1./actual-1./rt),-3./rt,3./rt);
            std::vector<std::vector<double>> prior(out.h.size());
            for(size_t r=0;r<out.h.size();++r) {
                prior[r]=out.h[r][b];const auto& weights=mixWeights[r];if(weights.empty())continue;
                auto& dst=out.h[r][b];const auto& src=original[r][b];
                std::vector<double> adjustable(stop);double oldArea=0,newArea=0;
                for(size_t k=onsets[r];k<stop;++k)if(weights[k]>0) {
                    adjustable[k]=std::max(0.,dst[k]-(1.-weights[k])*src[k])*std::exp(correction*(k-onsets[r])*out.binDur);
                    oldArea+=weights[k]*src[k];newArea+=adjustable[k];
                }
                const double scale=newArea>0?oldArea/newArea:1.;
                for(size_t k=onsets[r];k<stop;++k)if(weights[k]>0)dst[k]=(1.-weights[k])*src[k]+adjustable[k]*scale;
            }
            const double improved=analyzeTail(out).rt60[b];
            if(improved<=0||std::abs(improved/rt-1.)>=std::abs(actual/rt-1.)) {
                for(size_t r=0;r<out.h.size();++r)out.h[r][b]=std::move(prior[r]);break;
            }
        }
        // Preserve T20 as well as exact area. A short non-exponential room can
        // need a partial step; this guard never changes the original regression.
        for(int iteration=0;iteration<8;++iteration) {
            const double actual=analyzeTail(out).rt60[b];
            if(actual>0&&std::abs(actual/rt-1.)<=.025)break;
            for(size_t r=0;r<out.h.size();++r)for(size_t k=0;k<stop;++k)out.h[r][b][k]=.5*(out.h[r][b][k]+original[r][b][k]);
        }
    }
}

void finalizeTail(TailHist& out, bool smoothTransition) {
    const int R = (int)out.h.size();
    const auto measured = out.h;
    out.reliableEnd.resize(R);
    for (auto& r : out.reliableEnd) r.fill(out.nBins);
    // Where a band runs out of rays: the energy of the late field of a band can rest on a handful of rays (the ones that kept clear of
    // the absorbing audience, say), and then the histogram has steps and holes (heard as a decay in stages, and as a tail that stops
    // while it is still audible). The number of rays that really carry it is (sum w)^2 / sum w^2 over a 100 ms window; where it falls
    // below 10 the decay measured just before is continued as a straight line in dB.
    // A cutoff of 40 often selected the room's initial build-up instead of its
    // decay, producing position-dependent, nearly flat extrapolated tails.
    const double essMin = 10.0;
    for (int r = 0; r < R; ++r)
        for (int b = 0; b < NB; ++b) {
            const auto& e = out.h[(size_t)r][(size_t)b];
            const auto& e2 = out.h2[(size_t)r][(size_t)b];
            const size_t n = out.nBins, win = 100;
            std::vector<double> p1(n + 1, 0.0), p2(n + 1, 0.0);
            for (size_t k = 0; k < n; ++k) { p1[k + 1] = p1[k] + e[k]; p2[k + 1] = p2[k] + e2[k]; }
            size_t peak = 0;
            double pv = -1;
            for (size_t k = 0; k + win < n; ++k) if (p1[k + win] - p1[k] > pv) { pv = p1[k + win] - p1[k]; peak = k; }
            size_t kEnd = n;
            for (size_t k = peak + win; k + win < n; ++k) {
                const double s1 = p1[k + win] - p1[k], s2 = p2[k + win] - p2[k];
                const double ess = s2 > 0 ? s1 * s1 / s2 : 0.0;
                if (ess < essMin) { kEnd = k + win / 2; break; }
            }
            out.reliableEnd[(size_t)r][(size_t)b] = kEnd;
        }
    // Smooth the histogram in time, the more the later it is (moving average, mean-preserving): the late tail is a
    // slowly decaying diffuse field, only its envelope matters, and ray statistics get sparser with time and with
    // absorbing surfaces (a few strong survivors dominate). Early bins stay nearly untouched.
    for (auto& rc : out.h)
        for (auto& band : rc) {
            const size_t nb = band.size();
            std::vector<double> prefix(nb + 1, 0.0);
            for (size_t k = 0; k < nb; ++k) prefix[k + 1] = prefix[k] + band[k];
            for (size_t k = 0; k < nb; ++k) {
                const size_t w = std::min<size_t>(60, (size_t)(0.05 * (double)k));
                const size_t lo = k > w ? k - w : 0, hi = std::min(nb, k + w + 1);
                band[k] = (prefix[hi] - prefix[lo]) / (double)(hi - lo);
            }
        }
    for (int r = 0; r < R; ++r) {
        for (int bi = 0; bi < NB; ++bi) {
            const size_t kEnd = out.reliableEnd[(size_t)r][(size_t)bi];
            if (kEnd >= out.nBins || kEnd <= 150) continue;
            // kEnd is the CENTRE of the first unreliable 100 ms window.
            // Fit only before that window, using the unsmoothed measurements:
            // including its holes (or smoothing them backwards) biases the slope.
            const size_t fitEnd = kEnd - 50;
            // A short, absorbing room can run out of effective rays at 150-300 ms.
            // Requiring 300 ms of fit data used to leave its entire sparse tail
            // untouched. Adapt the averaging interval to the available decay,
            // retaining the longer 50 ms windows for well-sampled, long tails.
            const size_t blk = std::min<size_t>(50, (fitEnd - 100) / 6);
            if (blk == 0) continue;
            const size_t nBlk = std::min<size_t>(12, (fitEnd - 100) / blk);
            auto& band = out.h[(size_t)r][(size_t)bi];
            std::vector<double> xs, ys;
            double peakE = 0;
            for (double v : band) peakE = std::max(peakE, v);
            for (size_t j = 0; j < nBlk; ++j) {
                const size_t a = fitEnd - blk * (nBlk - j);
                double m = 0;
                for (size_t k = a; k < a + blk; ++k) m += measured[(size_t)r][(size_t)bi][k];
                m /= (double)blk;
                if (m <= 1e-14 * peakE || m <= 0) continue;
                xs.push_back((double)(a + blk / 2));
                ys.push_back(10.0 * std::log10(m));
            }
            if (xs.size() < 4) continue;  // this band has already died away
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            const double n = (double)xs.size();
            for (size_t i = 0; i < xs.size(); ++i) { sx += xs[i]; sy += ys[i]; sxx += xs[i] * xs[i]; sxy += xs[i] * ys[i]; }
            double slope = (n * sxy - sx * sy) / std::max(1e-9, n * sxx - sx * sx);  // dB per bin
            const double icpt = (sy - slope * sx) / n;
            slope = std::min(slope, -0.003);  // at least 3 dB per second: the field never grows
            const double kref = (double)kEnd;
            const double lvl = icpt + (n * sxy - sx * sy) / std::max(1e-9, n * sxx - sx * sx) * kref;  // the fitted line at the end
            const double l0 = std::min(lvl, ys.back() + 3.0);  // do not start above what was measured
            // Extrapolation redistributes uncertain late energy; it must not
            // manufacture more of it. In an open/absorbing room a short, noisy
            // fit can be nearly flat, otherwise adding seconds of audible sound
            // where the trace contains almost none. Budget only the replaced
            // portion, leaving the measured onset and crossfade continuous.
            double measuredEnergy = 0, fittedEnergy = 0;
            for (size_t k = kEnd - 100; k < out.nBins; ++k) {
                const double w = std::min(1.0, ((double)k - (kref - 100.0)) / 200.0);
                measuredEnergy += w * band[k];
                fittedEnergy += w * std::pow(10.0, (l0 + slope * ((double)k - kref)) / 10.0);
            }
            const double fitScale = fittedEnergy > 0 ? std::min(1.0, measuredEnergy / fittedEnergy) : 0;
            for (size_t k = kEnd - 100; k < out.nBins; ++k) {  // a 200 ms cross-fade from the measured histogram to the fitted line
                const double fit = fitScale * std::pow(10.0, (l0 + slope * ((double)k - kref)) / 10.0);
                const double w = std::min(1.0, ((double)k - (kref - 100.0)) / 200.0);
                band[k] = (1.0 - w) * band[k] + w * fit;
            }
        }
    }
    if(smoothTransition)smoothTailTransition(out,measured);
}

bool tailNeedsMoreRays(const TailHist& raw, const TailHist& finalized) {
    double largest = 0;
    for (const auto& r : raw.h) for (const auto& band : r) {
        double e = 0; for (double v : band) e += v;
        largest = std::max(largest, e);
    }
    if (largest <= 0) return false;
    for (size_t r = 0; r < raw.h.size(); ++r)
        for (int b = 0; b < NB; ++b) {
            const size_t end = finalized.reliableEnd[r][b];
            if (end >= raw.nBins) continue;
            double total = 0, remaining = 0;
            for (size_t k = 0; k < raw.nBins; ++k) {
                total += raw.h[r][b][k];
                if (k >= end) remaining += raw.h[r][b][k];
            }
            if (total > largest * 1e-5 && remaining > total * 0.005) return true;
        }
    return false;
}

TailHist traceTailAdaptive(const Scene& sc, const Transducer& src, const std::vector<Transducer>& rc,
                          int order, int baseRays, int maxRays, int seed, int& raysUsed, bool* needsMore, const std::function<bool()>& cancelled) {
    raysUsed = std::max(0, baseRays);
    maxRays = std::max(raysUsed, maxRays);
    for (;;) {
        auto raw = traceTail(sc, src, rc, order, raysUsed, seed, false, cancelled);
        auto h = raw;
        finalizeTail(h);
        const bool sparse = tailNeedsMoreRays(raw, h);
        if (needsMore) *needsMore = sparse;
        if (raysUsed == 0 || raysUsed >= maxRays || !sparse) return h;
        raysUsed = (int)std::min<int64_t>(maxRays, 2ll * raysUsed);
    }
}

TailDiagnostics analyzeTail(const TailHist& h) {
    TailDiagnostics out;
    out.duration = h.nBins * h.binDur;
    out.rt60.fill(-1);
    for (int b = 0; b < NB; ++b) {
        std::vector<double> energy(h.nBins + 1, 0.0);
        double reliable = out.duration;
        for (size_t r = 0; r < h.h.size(); ++r) {
            double sum = 0;
            for (size_t k = 0; k < h.nBins; ++k) { energy[k] += h.h[r][b][k]; sum += h.h[r][b][k]; }
            if (sum > 0 && r < h.reliableEnd.size() && h.reliableEnd[r][b] < h.nBins)
                reliable = std::min(reliable, std::max(0.0, h.reliableEnd[r][b] * h.binDur - 0.1));
        }
        for (size_t k = h.nBins; k-- > 0;) energy[k] += energy[k + 1];
        out.active[b] = energy[0] > 0;
        out.reliableSeconds[b] = reliable;
        auto dbAt = [&](size_t k) { return energy[0] > 0 ? std::max(-100.0, 10 * std::log10(std::max(energy[k] / energy[0], 1e-10))) : -100.0; };
        for (size_t i = 0; i < TailDiagnostics::points; ++i)
            out.decayDb[b][i] = (float)dbAt(i * h.nBins / (TailDiagnostics::points - 1));
        // T20: fit -5..-25 dB of the backward-integrated energy, then extrapolate to -60 dB.
        double sx = 0, sy = 0, sxx = 0, sxy = 0; size_t count = 0;
        for (size_t k = 0; k < h.nBins && out.active[b]; ++k) {
            const double db = dbAt(k), t = k * h.binDur;
            if (db < -25) break;
            if (db > -5) continue;
            sx += t; sy += db; sxx += t * t; sxy += t * db; ++count;
        }
        const double denom = count * sxx - sx * sx;
        if (count >= 4 && denom > 1e-12) {
            const double slope = (count * sxy - sx * sy) / denom;
            if (slope < -1e-6) out.rt60[b] = -60.0 / slope;
        }
    }
    out.effectiveMultipliers = h.effectiveMultipliers;
    out.reshapeStatus = h.reshapeStatus;
    out.physicalRT60 = out.rt60;
    out.physicalDecayDb = out.decayDb;
    return out;
}

void reshapeTailDecay(TailHist& h, const std::array<double, 3>& multipliers) {
    if (multipliers == std::array<double, 3>{{1, 1, 1}} || h.h.empty()) return;
    const auto original = h;
    const auto physical = analyzeTail(original);
    const double rate = 6. * std::log(10.);
    constexpr double peakMargin = 1.1220184543; // +0.5 dB in energy, never +0.5 dB per iteration
    const size_t limit = (size_t)std::floor(20. / h.binDur);
    std::array<double, NB> targets{};
    size_t bins = std::min(h.nBins, limit);
    for (int b=0;b<NB;++b) {
        const double m = clampTo(multipliers[b<2?0:b<5?1:2], .3, 3., 1.);
        if (m==1.) continue;
        const double rt=physical.rt60[b];
        if (!(rt>0) || !std::isfinite(rt)) { h.reshapeStatus[b]=1; continue; }
        size_t onset=original.nBins;
        for (const auto& rc:original.h) for(size_t k=0;k<rc[b].size();++k)
            if(rc[b][k]>0){onset=std::min(onset,k);break;}
        const double target=std::min(rt*m, std::max(0.,20.-onset*h.binDur-.8)*.75);
        if (target<12*h.binDur) { h.reshapeStatus[b]=2; continue; }
        targets[b]=target;
        h.effectiveMultipliers[b]=target/rt;
        if(target<rt*m-1e-9)h.reshapeStatus[b]=3;
        bins=std::max(bins,std::min(limit,(size_t)std::ceil((onset*h.binDur+target*80./60.+.8)/h.binDur)));
    }
    h.nBins=bins;
    for(auto& rc:h.h)for(auto& band:rc)band.resize(bins,0.);
    for(int b=0;b<NB;++b) {
        if(targets[b]<=0)continue;
        // Every trial is rebuilt from the physical data, never from a previously lifted residue.
        auto trial=[&](double tilt) {
            for(size_t r=0;r<h.h.size();++r) {
                const auto& src=original.h[r][b];auto& dst=h.h[r][b];
                const double peak=*std::max_element(src.begin(),src.end());
                std::fill(dst.begin(),dst.end(),0.);
                if(!(peak>0))continue;
                size_t onset=0;while(onset<src.size()&&src[onset]<=0)++onset;
                size_t anchor=onset;
                // -85 dB in ENERGY relative to the band peak.
                // Below it, use a fitted physical exponential; do not lift numerical noise.
                for(size_t k=onset;k<src.size();++k)if(src[k]>=peak*3.1622776602e-9)anchor=k;
                const double logPeak=std::log(peak*peakMargin), logAnchor=std::log(src[anchor]);
                for(size_t k=onset;k<bins;++k) {
                    const bool reliable=k<=anchor&&k<src.size()&&src[k]>=peak*3.1622776602e-9;
                    const double base=reliable ? std::log(src[k])
                        : k>anchor ? logAnchor-rate*(k-anchor)*h.binDur/physical.rt60[b]
                        // Holes inside the measured region are never lifted from sub-floor residue.
                        : k<src.size()&&src[k]>0 ? std::log(src[k])-std::max(0.,tilt)*(k-onset)*h.binDur : -INFINITY;
                    const double logE=base+tilt*(k-onset)*h.binDur;
                    dst[k]=std::exp(std::min(logPeak,logE));
                }
                dst[onset]=src[onset]; // exact onset level, including after a rejected refinement
            }
        };
        const double initial=rate*(1./physical.rt60[b]-1./targets[b]);
        double bestError=INFINITY;
        std::vector<std::vector<double>> best(h.h.size());
        auto assess=[&](double tilt) {
            trial(tilt);const double actual=analyzeTail(h).rt60[b];
            const double error=actual>0?std::abs(std::log(actual/targets[b])):INFINITY;
            if(error<bestError) {bestError=error;for(size_t r=0;r<h.h.size();++r)best[r]=h.h[r][b];}
            return actual;
        };
        double actual=assess(initial), lastTilt=initial;
        // Bounded slope search. A step is retained only if it reduces T20 error.
        // Peak clipping makes the response nonlinear, so also bracket the solution.
        double lo=-rate/(12*h.binDur),hi=rate/physical.rt60[b];
        for(int i=0;i<24&&bestError>std::log(1.005);++i) {
            if(actual>0&&actual<targets[b])lo=std::max(lo,lastTilt);
            else hi=std::min(hi,lastTilt);
            double next=(lo+hi)*.5;
            if(actual>0) {
                const double correction=std::clamp(rate*(1./actual-1./targets[b]),-rate/targets[b],rate/targets[b]);
                const double candidate=lastTilt+correction;
                if(candidate>lo&&candidate<hi)next=candidate;
            }
            const double tested=next;
            actual=assess(tested);lastTilt=tested;
            if(actual>0&&actual<targets[b])lo=tested;else hi=tested;
            // Subsequent brackets depend on the tested slope, not on the retained best slope.
            if(hi-lo<1e-10)break;
        }
        if(!std::isfinite(bestError)||bestError>std::log(1.005)) {
            h.reshapeStatus[b]=4;h.effectiveMultipliers[b]=1.;
            for(size_t r=0;r<h.h.size();++r){h.h[r][b]=original.h[r][b];h.h[r][b].resize(bins,0.);}
        } else {
            for(size_t r=0;r<h.h.size();++r)h.h[r][b]=std::move(best[r]);
            // Reliable time follows the modeled time scale instead of shrinking in the longer IR.
            for(size_t r=0;r<h.reliableEnd.size();++r) {
                size_t onset=0;const auto& band=original.h[r][b];while(onset<band.size()&&band[onset]<=0)++onset;
                const size_t end=std::max(onset,original.reliableEnd[r][b]);
                h.reliableEnd[r][b]=std::min(bins,onset+(size_t)std::ceil((end-onset)*h.effectiveMultipliers[b]));
            }
        }
    }
    h.h2.clear();
}

Scene withAbsorptionScale(const Scene& scene, double scale) {
    if (scale == 1.) return scene; // preserve the exact legacy numerical path
    Scene out = withClutter(withRoughness(scene), 128);
    out.roughness = 0; out.clutter.count = 0;
    scale = clampTo(scale, .3, 3., 1.);
    for (auto& m : out.mats) for (int b = 0; b < NB; ++b)
        m.alpha[b] = std::clamp(m.alpha[b] * scale, 0., std::max(0., 1. - m.tau[b]));
    return out;
}

std::array<double, NB> estimateRoomRT60(const Scene& scene) {
    const Scene s = withClutter(withRoughness(scene), 128);
    // Even-odd scan integration handles concave outlines and overlapping material patches.
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    for (const auto& w : scene.walls) { x0 = std::min({x0,w.ax,w.bx}); x1 = std::max({x1,w.ax,w.bx}); y0 = std::min({y0,w.ay,w.by}); y1 = std::max({y1,w.ay,w.by}); }
    double area = 0, surface = 0; std::array<double, NB> loss{};
    auto add = [&](double a, int mat) {
        surface += a;
        for (int b = 0; b < NB; ++b) loss[b] += a * (mat < 0 ? 1. : 1. - reflEnergy(s.mats[(size_t)mat], b));
    };
    for (const auto& w : s.walls) add(std::hypot(w.bx-w.ax,w.by-w.ay) * std::max(0., std::min(w.z1,s.height)-w.z0), w.mat);
    if (x1 > x0 && y1 > y0) for (int iy = 0; iy < 160; ++iy) for (int ix = 0; ix < 160; ++ix) {
        const double x = x0+(ix+.5)*(x1-x0)/160, y = y0+(iy+.5)*(y1-y0)/160;
        int hits = 0;
        for (const auto& w : scene.walls) if ((w.ay > y) != (w.by > y) && w.ax+(y-w.ay)*(w.bx-w.ax)/(w.by-w.ay)>x) ++hits;
        if (!(hits & 1)) continue;
        const double a = (x1-x0)*(y1-y0)/25600.; area += a;
        add(a, floorMatAt(s,x,y)); add(a,ceilMatAt(s,x,y));
    }
    for (const auto* zones : {&s.floorZones,&s.ceilZones}) for (const auto& z : *zones) if (z.z >= 0 && z.mat >= 0) {
        double twice = 0; for (size_t i=0;i<z.poly.size();++i) { const auto a=z.poly[i], b=z.poly[(i+1)%z.poly.size()]; twice += a.first*b.second-b.first*a.second; }
        add(std::abs(twice),z.mat); // both sides
    }
    std::array<double, NB> out{};
    const double volume = area * s.height, c = speedOfSound(s.tempC);
    for (int b=0;b<NB;++b) {
        const double eyring = surface > 0 ? -surface * std::log(std::max(1e-8,1.-loss[b]/surface)) : 0;
        const double air = 4.*volume*airAbsDbPerM(kBandHz[b],s.tempC,s.humidity)*std::log(10.)/10.;
        out[b] = volume > 0 ? 24.*std::log(10.)*volume/(c*std::max(1e-8,eyring+air)) : -1.;
    }
    return out;
}

double effectiveTailSeconds(const Scene& sc, int erOrder, const std::function<bool()>& cancelled) {
    double len = sc.length;
    if (!sc.sources.empty() && !sc.receivers.empty() && !sc.walls.empty()) {
        Scene t = sc;
        t.length = 20.0;  // allow long-decay rooms to reach the target before the final fade
        const std::vector<Transducer> rc{t.receivers[0]};
        // A 6000-ray probe often loses confidence during the initial build-up in
        // absorbing halls, making a tiny slope error inflate the allocation to
        // several seconds. Use the same order of ray budget as the live tail.
        auto h = traceTail(t, t.sources[0], rc, erOrder, 48000, t.seed, false, cancelled);
        finalizeTail(h,false); // preserve the legacy unity length probe exactly
        double need = 0;
        std::array<double, NB> totals{};
        double largest = 0;
        for (int b = 0; b < NB; ++b) {
            for (double v : h.h[0][(size_t)b]) totals[(size_t)b] += v;
            largest = std::max(largest, totals[(size_t)b]);
        }
        for (int b = 0; b < NB; ++b) {
            const auto& e = h.h[0][(size_t)b];
            const double total = totals[(size_t)b];
            if (total <= largest * 1e-5) continue;
            double rem = total;
            size_t k = 0, k5 = 0, k35 = 0;
            for (; k < e.size(); ++k) {
                rem -= e[k];
                if (k5 == 0 && rem < 0.316 * total) k5 = k;     // -5 dB
                if (k35 == 0 && rem < 3.16e-4 * total) k35 = k;  // -35 dB
                if (rem < 1e-8 * total) break;  // 80 dB of the energy has passed
            }
            need = std::max(need, (double)k * h.binDur);
            // A truncated or sparse trace underestimates the remaining energy.
            // Extrapolate the measured -5..-35 dB segment to -80 dB from its actual onset.
            if (k5 > 0 && k35 > k5)
                need = std::max(need, ((double)k5 + 2.5 * (double)(k35 - k5)) * h.binDur);
        }
        len = std::max(len, need + 0.8);  // leave the maximum 0.6 s end fade beyond the target
    }
    return std::min(std::max(len, 0.2), 20.0);
}

void fadeTailEnd(std::vector<std::vector<float>>& ir, size_t n, double sr) {
    const size_t fade = std::min(n / 2, (size_t)std::min(std::max(0.12 * (double)n, 0.04 * sr), 0.6 * sr));
    if (fade < 2) return;
    for (auto& ch : ir) {
        const size_t m = std::min(n, ch.size());
        for (size_t i = 0; i < fade && i < m; ++i) {
            const double x = (double)i / (double)fade;  // 0 at the start of the fade, 1 at the end
            ch[m - fade + i] *= (float)(0.5 * (1.0 + std::cos(kPi * x)));
        }
    }
}

void addTailNoise(const Scene& sc, const TailHist& hist, int seed, BandBuf& buf) {
    const int smoothK = 3;
    const double binSamples = hist.binDur * sc.sr;
    const size_t nBins = hist.nBins;
    for (size_t r = 0; r < buf.size(); ++r)
        for (int b = 0; b < NB; ++b) {
            const size_t N = buf[r][b].size();
            std::vector<double> sm(nBins, 0.0);
            for (size_t k = 0; k < nBins; ++k) {
                double acc = 0;
                int cnt = 0;
                for (int j = -smoothK; j <= smoothK; ++j) {
                    long q = (long)k + j;
                    if (q < 0 || q >= (long)nBins) continue;
                    acc += hist.h[r][b][q];
                    ++cnt;
                }
                sm[k] = acc / (cnt * binSamples);  // energy per sample
            }
            std::mt19937_64 rng((uint64_t)seed * 104729ull + r * 131 + b * 7 + 1);
            std::normal_distribution<double> Nd(0, 1);
            for (size_t i = 0; i < N; ++i) {
                double pos = (double)i / binSamples - 0.5;
                long k0 = (long)std::floor(pos);
                double f = pos - k0;
                double v0 = k0 >= 0 && k0 < (long)nBins ? sm[k0] : 0.0;
                double v1 = k0 + 1 >= 0 && k0 + 1 < (long)nBins ? sm[k0 + 1] : 0.0;
                double var = v0 * (1 - f) + v1 * f;
                if (var > 0) buf[r][b][i] += std::sqrt(var) * Nd(rng);
            }
        }
}

std::vector<std::vector<float>> combineBands(const BandBuf& buf, double sr) {
    const int taps = 2047, delay = (taps - 1) / 2;
    const size_t R = buf.size(), N = buf[0][0].size();
    auto bands = makeBandFilters(sr, taps);
    size_t FN = nextPow2(N + taps);
    std::vector<std::vector<cd>> Hf(NB, std::vector<cd>(FN));
    for (int b = 0; b < NB; ++b) {
        for (int n = 0; n < taps; ++n) Hf[b][n] = bands[b][n];
        fft(Hf[b], false);
    }
    std::vector<std::vector<float>> out(R, std::vector<float>(N));
    for (size_t r = 0; r < R; ++r) {
        std::vector<cd> acc(FN, cd(0)), tmp(FN);
        for (int b = 0; b < NB; ++b) {
            std::fill(tmp.begin(), tmp.end(), cd(0));
            for (size_t i = 0; i < N; ++i) tmp[i] = buf[r][b][i];
            fft(tmp, false);
            for (size_t i = 0; i < FN; ++i) acc[i] += tmp[i] * Hf[b][i];
        }
        fft(acc, true);
        for (size_t i = 0; i + delay < FN && i < N; ++i) out[r][i] = (float)acc[i + delay].real();
    }
    return out;
}

}  // namespace verb
