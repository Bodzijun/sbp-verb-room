#pragma once
// E2: Freeze as a true tail hold.
//
// DiffusingNetwork — a 16-line feedback delay network, one identical copy per output channel.
//   * Hadamard feedback (orthonormal, fast transform: 64 add/sub, exact 1/4 scale) -> lossless when
//     the absorption is bypassed; energy is preserved up to float rounding.
//   * Delay lengths: distinct primes, geometric between 45 and 160 ms at every sample rate
//     (sum ~1.4 s -> ~0.7 Hz mean mode spacing, ~70 000 modes at 48 kHz).
//   * Per-line absorption: a 7-band complementary one-pole bank (octave centres 125 Hz..8 kHz).
//     Each line's gain per band is 10^(-3 m_i / (RT60_b fs)) (delay-proportional => one RT60 for all
//     lines), refined so that the response at the 7 centres is exact. Sample-rate independent cost.
//   * Identical networks per channel: the hold is the same linear operator on every channel, so the
//     inter-channel coherence and the per-channel spectra of the captured tail are kept.
//   * Seeding: for the first m_i samples after a capture, line i reads a prepared copy of the recent
//     tail instead of its buffer ("instant bloom": the network is full at the first sample).
//   This class is meant to become the hybrid late tail later (input vector b, absorption filters).
// TailFreeze — the Freeze controller (history, capture, crossfades, release, re-engage).
// No allocation, lock, FFT or library RNG in process(); all memory is prepared on the loader.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>
#include "dsp.hpp"
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define VERB_FREEZE_SSE 1
#endif

namespace verb {

class DiffusingNetwork {
public:
    static constexpr int N = 16, bands = 7;
    static constexpr double minDelaySec = .045, maxDelaySec = .160;
private:
    struct Absorb { std::array<float,6> d{}; float g6 = 1; };   // H = g6 + sum d_k LP_k
    double sr = 48000;
    int channels = 0, total = 0;
    std::array<int,N> m{}, offset{}, pos{};
    std::array<float,N> c{};          // output vector (+-1/4)
    std::array<float,6> alpha{};      // one-pole coefficients of the band bank
    std::array<Absorb,N> absorb{};
    static constexpr int probes = 5;  // response points per octave (-0.4..+0.4 octave)
    std::array<std::array<double,2>,bands * probes * 6> lpAt{}; // LP_k response inside each octave (re, im)
    Absorb eq{};                      // calibration EQ on the network output (same bank structure)
    std::vector<float> buf;           // channels x total
    std::vector<float> lp;            // channels x N x 6 one-pole states
    std::vector<float> eqState;       // channels x 2 x 6 (output, old output)
    int seamLen = 1;                  // seed-to-recirculation crossfade per line (10 ms)
    std::vector<float> seamSeed, seamBuf;  // equal-power gains over the seam
    static bool prime(int n) { if (n < 2) return false; for (int k = 2; (long long)k * k <= n; ++k) if (n % k == 0) return false; return true; }
    static void fwht(float* s) {
        for (int h = 1; h < N; h <<= 1)
            for (int i = 0; i < N; i += h << 1)
                for (int j = i; j < i + h; ++j) { const float a = s[j], b = s[j + h]; s[j] = a + b; s[j + h] = a - b; }
        for (int i = 0; i < N; ++i) s[i] *= .25f;
    }
public:
    void prepare(double rate, int count) {
        sr = rate; channels = count; total = 0;
        int last = 0;
        for (int i = 0; i < N; ++i) {
            const double t = minDelaySec * std::pow(maxDelaySec / minDelaySec, i / (double)(N - 1));
            int d = std::max(last + 1, (int)std::lround(t * sr));
            while (!prime(d)) ++d;
            m[i] = d; last = d; offset[i] = total; total += d; pos[i] = 0;
        }
        // Output signs: not a Hadamard row (avoids reading one mixed line only).
        const int sign[N] = {1,-1,1,1,-1,1,-1,-1,1,1,-1,1,1,-1,-1,1};
        for (int i = 0; i < N; ++i) c[i] = .25f * sign[i];
        for (int k = 0; k < 6; ++k) alpha[k] = (float)(1 - std::exp(-2 * 3.141592653589793 * 125 * std::pow(2., k) * std::sqrt(2.) / sr));
        buf.assign((size_t)channels * total, 0.f);
        lp.assign((size_t)channels * N * 12, 0.f);
        eqState.assign((size_t)channels * 12, 0.f);
        for (int b = 0; b < bands; ++b) for (int q = 0; q < probes; ++q) for (int k = 0; k < 6; ++k) {
            const double hz = std::min(.45 * sr, 125 * std::pow(2., b + (q - (probes - 1) / 2.) * .2));
            const double w = 2 * 3.141592653589793 * hz / sr, a = alpha[k];
            const double dr = 1 - (1 - a) * std::cos(w), di = (1 - a) * std::sin(w), den = dr * dr + di * di;
            lpAt[((size_t)b * probes + q) * 6 + k] = {a * dr / den, -a * di / den};
        }
        eq = Absorb{}; eq.g6 = 1;
        seamLen = std::max(1, (int)std::lround(.01 * sr)); seamSeed.resize((size_t)seamLen); seamBuf.resize((size_t)seamLen);
        for (int k = 0; k < seamLen; ++k) { const double x = 1.5707963267948966 * (k + 1) / (seamLen + 1); seamSeed[(size_t)k] = (float)std::cos(x); seamBuf[(size_t)k] = (float)std::sin(x); }
        std::array<double,7> rt; rt.fill(2.); setDecay(rt);
    }
    // dB response of a bank (H = g6 + sum d_k LP_k) over octave b: the mean, or (decay = true) the
    // per-pass loss an octave energy-decay curve shows around -15 dB. Absorption that changes inside
    // an octave makes its slowest part dominate the decay (T20 of the octave EDC), so the decay fit
    // matches that effective loss, not the mean.
    double bandDb(const Absorb& a, int b, bool decay = false) const {
        std::array<double,probes> v{}; double sum = 0;
        for (int q = 0; q < probes; ++q) { double re = a.g6, im = 0;
            for (int k = 0; k < 6; ++k) { const auto& r = lpAt[((size_t)b * probes + q) * 6 + k]; re += a.d[k] * r[0]; im += a.d[k] * r[1]; }
            v[(size_t)q] = 10 * std::log10(re * re + im * im); sum += v[(size_t)q]; }
        const double mean = sum / probes;
        if (!decay || mean > -1e-6) return mean;
        const double passes = 15 / -mean; double e = 0;
        for (double x : v) e += std::pow(10., passes * x / 10) / probes;
        return 10 * std::log10(e) / passes;
    }
    // Bank whose octave-averaged dB response equals target[b] (fixed-point iterations, no trigonometry).
    Absorb fitBank(const std::array<double,7>& target, bool decay = false) const {
        std::array<double,7> g = target; Absorb a;
        auto load = [&] { a.g6 = (float)std::pow(10., g[6] / 20); for (int k = 0; k < 6; ++k) a.d[k] = (float)(std::pow(10., g[k] / 20) - std::pow(10., g[k + 1] / 20)); };
        for (int it = 0; it < 40; ++it) { load(); for (int b = 0; b < bands; ++b) g[b] += target[b] - bandDb(a, b, decay); }
        load(); return a;
    }
    double centreDb(int line, int b) const { return 2 * bandDb(absorb[(size_t)line], b); }
    double eqDb(int b) const { return bandDb(eq, b); }
    double eqCorrectionDb(int b) const { return eqDb(b); }
    // A fixed lossless network colours what it holds: equal-delay paths (i->k, k->i) correlate the
    // line outputs (about -0.2 dB broadband) and at low frequencies, with few modes per octave, the
    // octave levels depend on the exact prime delays (up to -0.7 dB at 125 Hz). The held/seed octave
    // ratios were measured offline with measureColouration() (300..400 white seeds per rate, exact FFT
    // octaves, held output 1.5..3.5 s; statistical error < 0.05 dB) and are removed by an output EQ.
    // Other rates use the mean of the table. e2_freeze_test re-measures the 48 kHz row.
    static std::array<double,7> colourationDb(double rate) {
        static const double rates[6] = {44100, 48000, 88200, 96000, 176400, 192000};
        static const double table[6][7] = {
            {-0.007, -0.191, -0.228, -0.183, -0.244, -0.261, -0.191},
            {-0.245, -0.035, -0.369, -0.187, -0.269, -0.170, -0.226},
            {-0.461, +0.056, -0.198, -0.149, -0.324, -0.265, -0.215},
            {-0.686, -0.024, -0.129, -0.060, -0.121, -0.225, -0.260},
            {-0.392, +0.039, -0.203, -0.094, -0.159, -0.253, -0.225},
            {-0.458, +0.070, -0.236, -0.295, -0.361, -0.259, -0.235}};
        std::array<double,7> out{};
        for (int r = 0; r < 6; ++r) if (std::abs(rate - rates[r]) < 1) { for (int b = 0; b < 7; ++b) out[b] = table[r][b]; return out; }
        for (int r = 0; r < 6; ++r) for (int b = 0; b < 7; ++b) out[b] += table[r][b] / 6;
        return out;
    }
    // Offline/test measurement of the held/seed octave ratio, dB (not used on the audio or prepare path).
    static std::array<double,7> measureColouration(double rate, int trials, uint32_t seedState = 0x9e3779b9u) {
        DiffusingNetwork ref; ref.prepare(rate, 1);
        const int S = ref.maxDelay() + ref.seam(); std::vector<float> seed((size_t)S), out(128);
        const int total = (int)(3.5 * rate), skip = (int)(1.5 * rate);
        std::vector<float> held((size_t)(total - skip));
        auto octaves = [&](const float* x, int n, std::array<double,7>& e) {
            const size_t L = nextPow2((size_t)n); std::vector<cd> a(L); for (int i = 0; i < n; ++i) a[(size_t)i] = x[i]; fft(a, false);
            for (size_t k = 1; k < L / 2; ++k) { const double f = k * rate / L;
                for (int b = 0; b < 7; ++b) { const double c = 125 * std::pow(2., b); if (f >= c / std::sqrt(2.) && f < c * std::sqrt(2.)) e[b] += std::norm(a[k]) / ((double)L * n); } }
        };
        std::array<double,7> seedE{}, outE{}; uint32_t r = seedState;
        for (int trial = 0; trial < trials; ++trial) {
            DiffusingNetwork probe; probe.prepare(rate, 1);
            for (int k = 0; k < S; ++k) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; seed[(size_t)k] = (float)((r >> 8) * (1. / 16777216.) - .5); }
            int done = 0;
            while (done < total) { const int n = std::min(128, total - done); probe.process(n, seed.data(), S, S, done, false, out.data(), 128, nullptr);
                for (int k = 0; k < n; ++k) if (done + k >= skip) held[(size_t)(done + k - skip)] = out[(size_t)k];
                done += n; }
            octaves(seed.data(), S, seedE); octaves(held.data(), total - skip, outE);
        }
        std::array<double,7> d{}; for (int b = 0; b < 7; ++b) d[b] = 10 * std::log10(outE[b] / seedE[b]); return d;
    }
    // Installs the output EQ. Returns the mean correction applied, dB.
    double calibrate() {
        std::array<double,7> corr = colourationDb(sr); for (double& v : corr) v = -v;
        eq = fitBank(corr);
        double mean = 0; for (double v : corr) mean += v / 7; return mean;
    }
    int delay(int i) const { return m[(size_t)i]; }
    int minDelay() const { return m[0]; }
    int maxDelay() const { return m[N - 1]; }
    size_t memoryBytes() const { return (buf.size() + lp.size()) * sizeof(float); }
    // Complex response of one line's absorption at frequency hz.
    double responseDb(int line, double hz) const {
        const double w = 2 * 3.141592653589793 * hz / sr; double re = absorb[(size_t)line].g6, im = 0;
        for (int k = 0; k < 6; ++k) { const double a = alpha[k]; // LP(z) = a / (1 - (1-a) z^-1)
            const double dr = 1 - (1 - a) * std::cos(w), di = (1 - a) * std::sin(w), den = dr * dr + di * di;
            re += absorb[(size_t)line].d[k] * a * dr / den; im += absorb[(size_t)line].d[k] * a * (-di) / den; }
        return 2 * 10 * std::log10(re * re + im * im);   // two stages
    }
    // Absorption for the decaying (non-frozen) state: per line gain 10^(-3 m_i/(RT60_b fs)).
    void setDecay(const std::array<double,7>& rt60) {
        for (int i = 0; i < N; ++i) {
            std::array<double,7> target{};
            for (int b = 0; b < bands; ++b) target[b] = -30. * m[i] / (std::clamp(rt60[b], .05, 60.) * sr);  // per stage
            absorb[i] = fitBank(target, true);
        }
    }
    // Samples of seed beyond the longest line: the continuation used by the seam crossfade.
    int seam() const { return seamLen; }
    // One block of m <= minDelay() samples for every channel. The caller provides, per line, where
    // the line output comes from: the buffer (normal), or a seed (first m_i samples after a capture).
    // seed: channel-major contiguous prepared tail, seedLen samples each (oldest first), whose last
    // seam() samples follow the "capture point". A line with age = m_i - elapsed - j > 0 reads
    // seed[seedLen - seam - age] (0 if beyond the seed). Over the next seam() samples (age 0..-seam+1)
    // it crossfades (equal power) from the seed's continuation to the recirculated buffer: an abrupt
    // switch would put a discontinuity into every line, which the lossless loop keeps forever
    // (broadband spray, and wide modal clusters around held tones).
    // lossy: apply absorption. out: per-channel network output. old: if non-null, also the output the
    // previous network state would produce (buffer reads with absorption), used for re-capture crossfade.
    void process(int n, const float* seed, int seedStride, int seedLen, int elapsed, bool lossy, float* out, int outStride, float* old) {
        for (int ch = 0; ch < channels; ++ch) {
            float* b = &buf[(size_t)ch * total];
            float* st = &lp[(size_t)ch * N * 12];
            const float* sd = seed ? seed + (size_t)ch * seedStride : nullptr;
            std::array<int,N> p = pos;
            for (int j = 0; j < n; ++j) {
                float s[N];
                float yo = 0;
                for (int i = 0; i < N; ++i) {
                    const float stored = b[offset[i] + p[i]];
                    float v = stored;
                    const int age = m[i] - elapsed - j;
                    if (age > -seamLen) {
                        const int idx = seedLen - seamLen - age;
                        const float sv = (sd && idx >= 0 && idx < seedLen) ? sd[idx] : 0.f;
                        v = age > 0 ? sv : seamSeed[(size_t)-age] * sv + seamBuf[(size_t)-age] * stored;
                    }
                    if (old) {          // previous (decaying) network, read before it is overwritten
                        yo += c[i] * absorbLine(i, stored, st + i * 12);
                    } else if (lossy) {
                        v = absorbLine(i, v, st + i * 12);
                    }
                    s[i] = v;
                }
                float y = 0; for (int i = 0; i < N; ++i) y += c[i] * s[i];
                out[(size_t)ch * outStride + j] = applyEq(y, &eqState[(size_t)ch * 12]);
                if (old) old[(size_t)ch * outStride + j] = applyEq(yo, &eqState[(size_t)ch * 12 + 6]);
                fwht(s);
                for (int i = 0; i < N; ++i) { b[offset[i] + p[i]] = s[i]; if (++p[i] == m[i]) p[i] = 0; }
            }
        }
        for (int i = 0; i < N; ++i) pos[i] = (pos[i] + n) % m[i];
    }
    // Line absorption: the same 7-band bank twice in cascade (half the dB loss each). One first-order
    // bank cannot fall fast enough between octaves for short high-frequency RT60s on the long lines.
    float absorbLine(int i, float v, float* z) const {
        const Absorb& a = absorb[(size_t)i];
        for (int stage = 0; stage < 2; ++stage, z += 6) { float y = a.g6 * v; for (int k = 0; k < 6; ++k) { z[k] += alpha[k] * (v - z[k]); y += a.d[k] * z[k]; } v = y; }
        return v;
    }
    float applyEq(float x, float* z) const { float y = eq.g6 * x; for (int k = 0; k < 6; ++k) { z[k] += alpha[k] * (x - z[k]); y += eq.d[k] * z[k]; } return y; }
    void clearFilters() { std::fill(lp.begin(), lp.end(), 0.f); std::fill(eqState.begin(), eqState.end(), 0.f); }
};

class TailFreeze {
public:
    enum class State { Idle, Holding, Releasing };
private:
    DiffusingNetwork net;
    double sr = 48000;
    int channels = 0, maxBlock = 128, histLen = 0, seedCap = 0, head = 0, filled = 0, seedLen = 0, elapsed = 0, xfade = 1, phase = 0;
    int64_t countdown = 0;
    std::vector<float> hist, seed, newOut, oldOut, gConv, gOld, gNew, zb;
    static constexpr int G = 4, Z = 6 * G;   // channels per analysis group; lanes = 6 cut-offs x G
    std::array<float,Z> laneCoef{};
    std::array<float,4> deltaCoef{};
    struct Job {   // deferred per-octave un-decay of the seed, one channel group (G) at a time
        int group = 0, phase = 0, row = 0; bool active = false;
        std::array<double,7> g0{}, step{}, g{};
        std::array<float,Z> z1{}, z2{};
    } job;
    bool pending = false;   // Freeze pressed, seed being prepared; the natural tail keeps playing
    std::array<double,7> rt{};
    bool armed = true, oldValid = false;
    float convFrom = 1, convGain = 1, convStep = 0;
    State state = State::Idle;
    float lastDelta = 0;
    double calibrationDb = 0;
    void record(const float* x, int stride, int n) {
        for (int ch = 0; ch < channels; ++ch) {
            float* h = &hist[(size_t)ch * histLen]; const float* s = x + (size_t)ch * stride; int w = head;
            for (int j = 0; j < n; ++j) { h[w] = s[j]; if (++w == histLen) w = 0; }
        }
        head = (head + n) % histLen; filled = std::min(histLen, filled + n);
    }
    // One sweep of two cascaded one-pole low-passes over rows [k0, k1) of d (4V lanes per row, in
    // place), forward or backward; z1/z2 carry the lane states between calls. Two forward plus two
    // backward passes give a zero-phase |H|^4 (24 dB/oct); LTI passes commute, so the two forward
    // passes share one sweep and the two backward passes another (half the memory traffic).
    template <int V> static void sweep(float* d, int k0, int k1, bool backward, const float* coef, float* z1s, float* z2s) {
#if VERB_FREEZE_SSE
        __m128 a[V], z1[V], z2[V];
        for (int q = 0; q < V; ++q) { a[q] = _mm_loadu_ps(coef + 4 * q); z1[q] = _mm_loadu_ps(z1s + 4 * q); z2[q] = _mm_loadu_ps(z2s + 4 * q); }
        auto step = [&](float* y) { for (int q = 0; q < V; ++q) { const __m128 x = _mm_loadu_ps(y + 4 * q);
            z1[q] = _mm_add_ps(z1[q], _mm_mul_ps(a[q], _mm_sub_ps(x, z1[q]))); z2[q] = _mm_add_ps(z2[q], _mm_mul_ps(a[q], _mm_sub_ps(z1[q], z2[q]))); _mm_storeu_ps(y + 4 * q, z2[q]); } };
        if (!backward) for (int k = k0; k < k1; ++k) step(d + (size_t)k * 4 * V);
        else for (int k = k1 - 1; k >= k0; --k) step(d + (size_t)k * 4 * V);
        for (int q = 0; q < V; ++q) { _mm_storeu_ps(z1s + 4 * q, z1[q]); _mm_storeu_ps(z2s + 4 * q, z2[q]); }
#else
        constexpr int W = 4 * V;
        auto step = [&](float* y) { for (int l = 0; l < W; ++l) { z1s[l] += coef[l] * (y[l] - z1s[l]); z2s[l] += coef[l] * (z1s[l] - z2s[l]); y[l] = z2s[l]; } };
        if (!backward) for (int k = k0; k < k1; ++k) step(d + (size_t)k * W);
        else for (int k = k1 - 1; k >= k0; --k) step(d + (size_t)k * W);
#endif
    }
    // Copy the last seedLen samples and estimate how much of the free decay is present. A decaying
    // seed is then "un-decayed" per octave (job below) so it is stationary at the level of its newest part.
    void captureStart(int n) {
        const int C = channels, S = seedCap;
        seedLen = std::min(filled, S);
        const int L = seedLen, start = ((head - L) % histLen + histLen) % histLen, first = std::min(L, histLen - start);
        for (int ch = 0; ch < C; ++ch) {   // channel-major copy (the raw seed)
            const float* h = &hist[(size_t)ch * histLen]; float* sd = &seed[(size_t)ch * S];
            std::copy(h + start, h + start + first, sd); std::copy(h, h + (L - first), sd + first);
        }
        lastDelta = 0; job = Job{};
        if (L < (int)(.03 * sr)) return;
        // Decay rate of the 1, 2 and 4 kHz octaves (older vs newer half, edges excluded) of the channel
        // sum (or channel 0 if the sum cancels), relative to the room.
        float* mono = zb.data();           // L samples (the lanes below are filled from it in place)
        std::fill(mono, mono + L, 0.f); double chE = 0;
        for (int ch = 0; ch < C; ++ch) { const float* sd = &seed[(size_t)ch * S]; for (int k = 0; k < L; ++k) { mono[k] += sd[k]; chE += (double)sd[k] * sd[k]; } }
        double sumE = 0; for (int k = 0; k < L; ++k) sumE += (double)mono[k] * mono[k];
        if (!(sumE > 1e-3 * chE)) std::copy(seed.begin(), seed.begin() + L, mono);
        for (int k = L - 1; k >= 0; --k) { const float m = mono[k]; float* y = zb.data() + (size_t)k * 4; y[0] = y[1] = y[2] = y[3] = m; }
        {   // lanes: cut-offs 2..5 (707 Hz .. 5.7 kHz)
            float z1[4], z2[4];
            std::copy(zb.data(), zb.data() + 4, z1); std::copy(z1, z1 + 4, z2); sweep<1>(zb.data(), 0, L, false, deltaCoef.data(), z1, z2);
            std::copy(zb.data() + (size_t)(L - 1) * 4, zb.data() + (size_t)L * 4, z1); std::copy(z1, z1 + 4, z2); sweep<1>(zb.data(), 0, L, true, deltaCoef.data(), z1, z2);
        }
        const int edge = (int)(.002 * sr), half = (L - 2 * edge) / 2;
        double deltaSum = 0; int deltaCount = 0;
        for (int b = 3; b <= 5; ++b) {
            double older = 0, newer = 0;
            for (int k = edge; k < edge + 2 * half; ++k) { const float* y = zb.data() + (size_t)k * 4; const double v = (double)y[b - 2] - y[b - 3]; (k < edge + half ? older : newer) += v * v; }
            if (older > 1e-30 && newer > 1e-30) { const double rate = std::log(older / newer) / (half / sr), room = 6 * std::log(10.) / std::max(.05, rt[(size_t)b]);
                deltaSum += std::clamp(rate / room, 0., 3.); ++deltaCount; }
        }
        // 0 = stationary seed, 1 = free decay at the room's RT60 (>1 tolerated: RT60 estimate too long).
        const double delta = deltaCount ? deltaSum / deltaCount : 0;
        lastDelta = (float)delta;
        if (delta < .2) return;   // below ~2.5 sigma of the estimate on stationary input
        // y = g6 x + sum_b (g_b - g_{b+1}) ZLP_b, g_b(age) = exp(-delta beta_b (age + D) / 2): per-band
        // un-decay to the level the tail will have when the hold starts, D samples after the capture
        // (D > 0 when the job is spread over several blocks; the natural tail keeps playing meanwhile).
        const int64_t rows = (int64_t)((C + G - 1) / G) * 4 * L, budget = rowBudget(n);
        const int64_t D = (int64_t)((rows + budget - 1) / budget - 1) * n;
        for (int b = 0; b < 7; ++b) { const double beta = delta * 6 * std::log(10.) / std::max(.05, rt[(size_t)b]);
            job.g0[(size_t)b] = std::exp(-beta * ((double)L - 1 + (double)D) / (2 * sr)); job.step[(size_t)b] = std::exp(beta / (2 * sr)); }
        job.active = true;
    }
    // Rows of 24 lanes the un-decay job may process in one n-sample block: about a fifth of the
    // block period on a current desktop core (~10 ns per row).
    int64_t rowBudget(int n) const { return std::max<int64_t>(1024, (int64_t)((double)n * 2.0e7 / sr)); }
    // Advances the un-decay job by up to `budget` rows; true when the seed is ready.
    bool advanceJob(int64_t budget) {
        const int C = channels, S = seedCap, L = seedLen;
        while (job.active && budget > 0) {
            const int c0 = job.group * G, gc = std::min(G, C - c0);
            const int k0 = job.row, k1 = (int)std::min<int64_t>(L, k0 + budget);
            if (job.phase == 0) {          // gather the group into lanes (cut-off b, channel c)
                for (int k = k0; k < k1; ++k) { float* y = zb.data() + (size_t)k * Z;
                    for (int c = 0; c < G; ++c) { const float v = c < gc ? seed[(size_t)(c0 + c) * S + k] : 0.f; for (int b = 0; b < 6; ++b) y[b * G + c] = v; } }
                if (k0 == 0) { std::copy(zb.data(), zb.data() + Z, job.z1.data()); job.z2 = job.z1; }
            } else if (job.phase == 1) {
                sweep<Z / 4>(zb.data(), k0, k1, false, laneCoef.data(), job.z1.data(), job.z2.data());
            } else if (job.phase == 2) {   // backward, rows L-1-k0 .. L-k1 (counted from the end)
                if (k0 == 0) { std::copy(zb.data() + (size_t)(L - 1) * Z, zb.data() + (size_t)L * Z, job.z1.data()); job.z2 = job.z1; }
                sweep<Z / 4>(zb.data(), L - k1, L - k0, true, laneCoef.data(), job.z1.data(), job.z2.data());
            } else {                       // weighted sum back into the seed
                if (k0 == 0) job.g = job.g0;
                for (int k = k0; k < k1; ++k) {
                    const float* y = zb.data() + (size_t)k * Z;
                    float w[7]; for (int b = 0; b < 7; ++b) { w[b] = (float)job.g[(size_t)b]; job.g[(size_t)b] *= job.step[(size_t)b]; }
                    for (int c = 0; c < gc; ++c) { float& x = seed[(size_t)(c0 + c) * S + k]; float v = w[6] * x; for (int b = 0; b < 6; ++b) v += (w[b] - w[b + 1]) * y[b * G + c]; x = v; }
                }
            }
            budget -= k1 - k0; job.row = k1;
            if (job.row >= L) { job.row = 0; if (++job.phase == 4) { job.phase = 0; if (++job.group * G >= C) job.active = false; } }
        }
        return !job.active;
    }
public:
    void prepare(double rate, int count, int block) {
        sr = rate; channels = count; maxBlock = std::max(1, block);
        net.prepare(sr, count); calibrationDb = net.calibrate();
        seedCap = net.maxDelay() + net.seam();
        histLen = seedCap + maxBlock + 1;
        hist.assign((size_t)count * histLen, 0.f); seed.assign((size_t)count * seedCap, 0.f);
        zb.assign((size_t)seedCap * Z, 0.f);
        for (int b = 0; b < 6; ++b) for (int c = 0; c < G; ++c) laneCoef[(size_t)(b * G + c)] = (float)(1 - std::exp(-2 * 3.141592653589793 * 125 * std::pow(2., b) * std::sqrt(2.) / sr));
        for (int b = 2; b < 6; ++b) deltaCoef[(size_t)(b - 2)] = laneCoef[(size_t)(b * G)];
        newOut.assign((size_t)count * maxBlock, 0.f); oldOut = newOut;
        gConv.assign((size_t)maxBlock, 1.f); gOld = gConv; gNew = gConv;
        xfade = std::max(1, (int)std::lround(.02 * sr)); convStep = 1.f / xfade;
        rt.fill(2.); net.setDecay(rt);
        head = filled = elapsed = phase = 0; state = State::Idle; convGain = 1; pending = false; job = Job{};
    }
    void setRoomDecay(const std::array<double,7>& rt60) { rt = rt60; net.setDecay(rt); }
    // false in Physical mode: no history is written, the component costs one branch per block.
    void setArmed(bool a) { if (a && !armed) filled = 0; armed = a; }
    State currentState() const { return state; }
    bool capturePending() const { return pending; }
    float lastDecayFraction() const { return lastDelta; }
    double outputCalibrationDb() const { return calibrationDb; }
    const float* seedData(int ch) const { return &seed[(size_t)ch * seedCap]; }   // diagnostics/tests
    int seedLength() const { return seedLen; }
    const DiffusingNetwork& network() const { return net; }
    size_t memoryBytes() const { return net.memoryBytes() + (hist.size() + seed.size() + newOut.size() * 2 + zb.size()) * sizeof(float); }
    // tail: channel-major, stride per channel, n <= block samples. Returns false when held output
    // equals the tail exactly (caller keeps using tail); true when `held` was written.
    bool process(const float* tail, float* held, int stride, int count, int n, bool frozen) {
        (void)count;
        if (frozen && state != State::Holding && !pending) { captureStart(n); pending = true; }
        if (pending) {
            if (!frozen) { pending = false; job.active = false; }      // released before the hold began
            else if (advanceJob(rowBudget(n))) {
                pending = false;
                oldValid = state == State::Releasing && elapsed >= net.maxDelay() + net.seam();
                elapsed = 0; phase = 0; convFrom = convGain; state = State::Holding;
            }
        } else if (!frozen && state == State::Holding) {
            state = State::Releasing;
            double longest = 0; for (double r : rt) longest = std::max(longest, r);
            countdown = (int64_t)(2.2 * longest * sr) + net.maxDelay();
        }
        if (state == State::Idle) { if (armed) record(tail, stride, n); return false; }
        const float halfPi = 1.5707963f;
        for (int j = 0; j < n; ++j) {
            float in = 1, out = 0;   // equal-power engage crossfade (20 ms)
            if (phase + j + 1 < xfade) { const float x = (phase + j + 1) / (float)xfade; in = std::sin(halfPi * x); out = std::cos(halfPi * x); }
            gNew[j] = in; gOld[j] = oldValid ? out : 0.f;
            if (state == State::Holding) gConv[j] = convFrom * out;
            else { convGain = std::min(1.f, convGain + convStep); gConv[j] = convGain; }
        }
        if (state == State::Holding) convGain = gConv[(size_t)n - 1];
        const bool needOld = oldValid && phase < xfade;
        net.process(n, seed.data(), seedCap, seedLen, elapsed, state == State::Releasing, newOut.data(), maxBlock, needOld ? oldOut.data() : nullptr);
        for (int ch = 0; ch < channels; ++ch) {
            const float* t = tail + (size_t)ch * stride; float* h = held + (size_t)ch * stride;
            const float* a = &newOut[(size_t)ch * maxBlock]; const float* o = &oldOut[(size_t)ch * maxBlock];
            for (int j = 0; j < n; ++j) h[j] = gConv[j] * t[j] + gNew[j] * a[j] + (needOld ? gOld[j] * o[j] : 0.f);
        }
        elapsed = (int)std::min<int64_t>((int64_t)elapsed + n, 1 << 30); phase = std::min(xfade, phase + n);
        if (state == State::Releasing) {
            if (armed) record(held, stride, n);
            countdown -= n;
            if (countdown <= 0 && convGain >= 1.f) { state = State::Idle; oldValid = false; net.clearFilters(); }
        }
        return true;
    }
};

} // namespace verb
