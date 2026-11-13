// Offline hybrid renderer: deterministic image-source early reflections (arbitrary vertical wall segments,
// floor, optional ceiling, visibility checks) + stochastic ray-traced late tail, per octave band.
#include "render.hpp"

#include <algorithm>
#include <cstdio>

#include "geometry.hpp"
#include "tail.hpp"

namespace verb {
namespace {

// Windowed-sinc fractional impulse.
void addFrac(std::vector<double>& buf, double pos, double amp) {
    const int H = 16;
    int base = (int)std::floor(pos);
    double frac = pos - base;
    for (int k = -H + 1; k <= H; ++k) {
        long idx = (long)base + k;
        if (idx < 0 || idx >= (long)buf.size()) continue;
        double x = k - frac;
        double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        double win = 0.5 * (1 + std::cos(kPi * x / H));
        buf[idx] += amp * sinc * win;
    }
}

}  // namespace

Wav renderIR(const Scene& scIn, int srcIdx, bool verbose) {
    const Scene sc = withClutter(withRoughness(scIn));  // seeded clutter, if the scene asks for it
    const Geo g(sc);
    const Transducer& src = sc.sources[srcIdx];
    const int R = (int)sc.receivers.size();
    const double c = speedOfSound(sc.tempC);
    const size_t N = (size_t)std::ceil(sc.length * sc.sr);
    std::array<double, NB> airDb;
    for (int b = 0; b < NB; ++b) airDb[b] = airAbsDbPerM(kBandHz[b], sc.tempC, sc.humidity);

    BandBuf buf(R);
    for (auto& r : buf)
        for (auto& b : r) b.assign(N, 0.0);

    auto reflect = [&](int surf, Vec3 P, std::array<double, NB>& a) {
        const Material* m = g.matAt(surf, P.x, P.y);
        for (int b = 0; b < NB; ++b) a[b] *= m ? std::sqrt(reflEnergy(*m, b) * (1 - scatterAt(*m, b))) : 0.0;
    };
    auto deposit = [&](int r, double lenM, std::array<double, NB> amp) {
        double d = lenM / c * sc.sr;
        if (d >= (double)N - 16) return;
        double spread = 1.0 / std::max(lenM, 0.05);
        for (int b = 0; b < NB; ++b) {
            double a = amp[b] * spread * std::pow(10.0, -airDb[b] * lenM / 20.0);
            addFrac(buf[r][b], d, a);
        }
    };

    // ---- early reflections: image sources ----
    size_t nPaths = 0;
    {
        std::vector<int> chain;
        std::vector<Vec3> imgs{src.pos};
        std::vector<Vec3> pts;
        auto evaluate = [&](int r) {
            const Transducer& rc = sc.receivers[r];
            const int k = (int)chain.size();
            pts.assign(k + 1, {});
            Vec3 cur = rc.pos;
            float tr[NB];  // amplitude transmitted through walls the path crosses
            for (int b = 0; b < NB; ++b) tr[b] = 1.f;
            for (int j = k; j >= 1; --j) {
                Vec3 P;
                if (!crossSurface(g, cur, imgs[j], chain[j - 1], P)) return;
                if (!transmitLeg(g, cur, P, chain[j - 1], j < k ? chain[j] : -1, tr)) return;
                pts[j] = P;
                cur = P;
            }
            if (!transmitLeg(g, cur, src.pos, chain[0], -1, tr)) return;
            for (int j = 1; j <= k; ++j)  // a reflection exactly on another wall's plane (wall foot, corner) is a crossing
                if (!touchCross(g, j < k ? pts[(size_t)j + 1] : rc.pos, pts[(size_t)j], j > 1 ? pts[(size_t)j - 1] : src.pos, tr)) return;
            double len = 0;
            Vec3 prev = rc.pos;
            for (int j = k; j >= 1; --j) { len += norm(pts[j] - prev); prev = pts[j]; }
            len += norm(src.pos - prev);
            Vec3 first = pts[1] - src.pos, last = pts[k] - rc.pos;
            std::array<double, NB> amp;
            double gain = dirGain(src, unit(first)) * dirGain(rc, unit(last));
            amp.fill(gain);
            for (int j = 1; j <= k; ++j) reflect(chain[(size_t)j - 1], pts[(size_t)j], amp);
            for (int b = 0; b < NB; ++b) amp[b] *= tr[b];
            deposit(r, len, amp);
            ++nPaths;
        };
        auto rec = [&](auto&& self, int order) -> void {
            if (order >= 1)
                for (int r = 0; r < R; ++r) evaluate(r);
            if (order == sc.order) return;
            for (int s = 0; s < g.nSurf(); ++s) {
                if (!chain.empty() && s == chain.back()) continue;
                if (s == g.ceilId() && !g.hasCeil) continue;
                chain.push_back(s);
                imgs.push_back(mirror(g, imgs.back(), s));
                self(self, order + 1);
                chain.pop_back();
                imgs.pop_back();
            }
        };
        for (int r = 0; r < R; ++r) {  // direct sound
            const Transducer& rc = sc.receivers[r];
            float tr[NB];
            for (int b = 0; b < NB; ++b) tr[b] = 1.f;
            if (!transmitLeg(g, src.pos, rc.pos, -1, -1, tr)) continue;
            Vec3 d = unit(rc.pos - src.pos);
            std::array<double, NB> amp;
            amp.fill(dirGain(src, d) * dirGain(rc, d * -1.0));
            for (int b = 0; b < NB; ++b) amp[b] *= tr[b];
            deposit(r, norm(rc.pos - src.pos), amp);
            ++nPaths;
        }
        if (sc.order > 0) rec(rec, 0);
    }

    // ---- late tail ----
    TailHist hist = traceTail(sc, src, sc.receivers, sc.order, sc.rays, sc.seed);
    addTailNoise(sc, hist, sc.seed, buf);

    Wav out;
    out.sr = (int)sc.sr;
    out.ch = combineBands(buf, sc.sr);
    if (verbose) {
        std::printf("  source '%s': %zu specular paths (order<=%d), %d rays, c=%.1f m/s\n", src.name.c_str(), nPaths, sc.order, sc.rays, c);
        for (int r = 0; r < R; ++r) {
            double rt = estimateRT60(out.ch[r], sc.sr);
            if (rt > 0) std::printf("  receiver '%s': RT60 ~ %.2f s\n", sc.receivers[r].name.c_str(), rt);
            else std::printf("  receiver '%s': RT60 n/a (IR too short or too quiet)\n", sc.receivers[r].name.c_str());
        }
    }
    return out;
}

double estimateRT60(const std::vector<float>& ir, double sr) {
    const size_t n = ir.size();
    std::vector<double> edc(n);
    double acc = 0;
    for (size_t i = n; i-- > 0;) { acc += (double)ir[i] * ir[i]; edc[i] = acc; }
    if (acc <= 0) return -1;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0;  // dB slope between -5 and -25 dB
    for (size_t i = 0; i < n; ++i) {
        double db = 10 * std::log10(edc[i] / edc[0] + 1e-30);
        if (db > -5 || db < -25) continue;
        double t = i / sr;
        sx += t; sy += db; sxx += t * t; sxy += t * db; ++cnt;
    }
    if (cnt < 10) return -1;
    double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
    if (slope >= 0) return -1;
    return -60.0 / slope;
}

}  // namespace verb
