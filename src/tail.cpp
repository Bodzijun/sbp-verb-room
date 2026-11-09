#include "tail.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>

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

}  // namespace

TailHist traceTail(const Scene& sc, const Transducer& src, const std::vector<Transducer>& rcv, int order, int rays, int seed, bool finalize) {
    const Geo g(sc);
    const int R = (int)rcv.size();
    const double c = speedOfSound(sc.tempC);
    std::array<double, NB> airDb;
    for (int b = 0; b < NB; ++b) airDb[b] = airAbsDbPerM(kBandHz[b], sc.tempC, sc.humidity);

    TailHist out;
    out.binDur = 0.001;
    out.nBins = (size_t)std::ceil(sc.length / out.binDur) + 2;
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
    for (int i = 0; i < rays; ++i) {
        double zc = 2 * U(rng) - 1, ph = 2 * kPi * U(rng), sr_ = std::sqrt(1 - zc * zc);
        Vec3 d{sr_ * std::cos(ph), sr_ * std::sin(ph), zc};
        double gs = dirGain(src, d);
        std::array<double, NB> e;
        e.fill(gs * gs / rays);
        Vec3 p = src.pos;
        double path = 0;
        int refl = 0;
        while (true) {
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
                for (int r = 0; r < R; ++r) {
                    Vec3 oc = p - rcv[r].pos;
                    double bq = dot(oc, d), cc = dot(oc, oc) - rd * rd, disc = bq * bq - cc;
                    if (disc <= 0) continue;
                    double sq = std::sqrt(disc);
                    double s1 = std::max(-bq - sq, 0.0), s2 = std::min(-bq + sq, L);
                    if (s2 <= s1) continue;
                    double mid = path + 0.5 * (s1 + s2);
                    size_t bin = (size_t)(mid / c / out.binDur);
                    if (bin >= out.nBins) continue;
                    double grb[NB];
                    dirGainBands(rcv[r], d * -1.0, grb);
                    for (int b = 0; b < NB; ++b) {
                        const double gr = grb[b];
                        const double contrib = 4 * kPi * e[b] * std::pow(10.0, -airDb[b] * mid / 10.0) * (s2 - s1) / Vs * gr * gr;
                        out.h[r][b][bin] += contrib;
                        out.h2[r][b][bin] += contrib * contrib;
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
    }
    if (finalize) finalizeTail(out);
    return out;
}

void finalizeTail(TailHist& out) {
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
                          int order, int baseRays, int maxRays, int seed, int& raysUsed, bool* needsMore) {
    raysUsed = std::max(0, baseRays);
    maxRays = std::max(raysUsed, maxRays);
    for (;;) {
        auto raw = traceTail(sc, src, rc, order, raysUsed, seed, false);
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
    return out;
}

double effectiveTailSeconds(const Scene& sc, int erOrder) {
    double len = sc.length;
    if (!sc.sources.empty() && !sc.receivers.empty() && !sc.walls.empty()) {
        Scene t = sc;
        t.length = 20.0;  // allow long-decay rooms to reach the target before the final fade
        const std::vector<Transducer> rc{t.receivers[0]};
        // A 6000-ray probe often loses confidence during the initial build-up in
        // absorbing halls, making a tiny slope error inflate the allocation to
        // several seconds. Use the same order of ray budget as the live tail.
        const TailHist h = traceTail(t, t.sources[0], rc, erOrder, 48000, t.seed);
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
