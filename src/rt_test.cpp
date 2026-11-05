// Real-time engine tests: rt_test <scenes_dir>
//  1. static scene vs offline renderer (random block sizes)
//  2. Doppler shift of a moving source in free field
//  3. CPU load with moving sources/listener
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>

#include "geometry.hpp"
#include "layouts.hpp"
#include "render.hpp"
#include "rt_engine.hpp"

using namespace verb;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

static double corr(const std::vector<float>& a, const std::vector<float>& b, size_t i0, size_t i1) {
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = i0; i < i1; ++i) { sab += (double)a[i] * b[i]; saa += (double)a[i] * a[i]; sbb += (double)b[i] * b[i]; }
    return sab / std::sqrt(saa * sbb + 1e-300);
}
static double energy(const std::vector<float>& a, size_t i0, size_t i1) {
    double s = 0;
    for (size_t i = i0; i < i1; ++i) s += (double)a[i] * a[i];
    return s;
}
static bool finite(const std::vector<float>& a) {
    for (float v : a) if (!std::isfinite(v)) return false;
    return true;
}

static void testCompare(const std::string& dir) {
    std::printf("Test 1: static scene, engine vs offline renderer\n");
    Scene sc = loadScene(dir + "/l_room_5_1.txt");
    sc.order = 2; sc.rays = 20000; sc.length = 2.0;
    Wav off = renderIR(sc, 0, false);
    EngineConfig cfg;
    cfg.erOrder = 2; cfg.tailRays = 20000; cfg.tailThread = false;
    auto t0 = std::chrono::steady_clock::now();
    Engine eng(sc, cfg);
    double ctor = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  engine construction (incl. initial tail synthesis): %.1f s\n", ctor);
    const size_t N = off.ch[0].size();
    const int R = eng.numReceivers();
    std::vector<std::vector<float>> out(R, std::vector<float>(N, 0.f));
    std::vector<float> in(N, 0.f), zero(N, 0.f);
    in[0] = 1.f;
    std::mt19937 rng(5);
    size_t pos = 0;
    int taps = 0;
    while (pos < N) {
        int n = (int)std::min<size_t>(N - pos, 1 + rng() % 300);
        const float* ip[2] = {in.data() + pos, zero.data()};
        std::vector<float*> op(R);
        for (int r = 0; r < R; ++r) op[r] = out[r].data() + pos;
        eng.process(ip, op.data(), n);
        pos += n;
        taps = eng.activeTaps();
    }
    std::printf("  active taps: %d\n", taps);
    bool allOk = true, allFinite = true;
    const size_t a = (size_t)(0.25 * sc.sr), b = (size_t)(1.9 * sc.sr);
    for (int r = 0; r < R; ++r) {
        double cl = corr(out[r], off.ch[r], a, b);
        double early = 10 * std::log10(energy(out[r], 0, a) / energy(off.ch[r], 0, a));
        double ce = corr(out[r], off.ch[r], 0, a);
        std::printf("  rcv %d: late corr %.4f | early corr %.3f | early energy diff %+.2f dB\n", r, cl, ce, early);
        allOk &= cl > 0.98 && std::fabs(early) < 1.5;
        allFinite &= finite(out[r]);
    }
    check(allFinite, "output finite");
    check(allOk, "late field matches offline (corr > 0.98, aligned => zero latency), early energy within 1.5 dB");
}

static void testDoppler(const std::string& dir, int ctrl, double strength = 1.0) {
    std::printf("Test 2: Doppler, source approaching at 20 m/s (free field), ctrlInterval %d, strength %.2f\n", ctrl, strength);
    Scene sc = loadScene(dir + "/free_field.txt");
    EngineConfig cfg;
    cfg.tailThread = false;
    cfg.ctrlInterval = ctrl;
    Engine eng(sc, cfg);
    eng.setDopplerStrength(strength);
    const double sr = sc.sr, f0 = 1000, v = 20, x0 = -80;
    const size_t N = (size_t)(2.0 * sr);
    std::vector<float> in(N), out(N, 0.f);
    for (size_t i = 0; i < N; ++i) in[i] = (float)std::sin(2 * kPi * f0 * i / sr);
    for (size_t pos = 0; pos < N; pos += 64) {
        eng.setSourcePosition(0, x0 + v * (pos / sr), 0, 1.5);
        int n = (int)std::min<size_t>(64, N - pos);
        const float* ip[1] = {in.data() + pos};
        float* op[1] = {out.data() + pos};
        eng.process(ip, op, n);
    }
    // frequency from rising zero crossings in 1.0 .. 1.8 s
    size_t i0 = (size_t)(1.0 * sr), i1 = (size_t)(1.8 * sr);
    double first = -1, last = -1;
    int cnt = 0;
    for (size_t i = i0 + 1; i < i1; ++i)
        if (out[i - 1] < 0 && out[i] >= 0) {
            double t = (i - 1) + (0 - out[i - 1]) / (out[i] - out[i - 1]);
            if (first < 0) first = t;
            last = t;
            ++cnt;
        }
    double f = (cnt - 1) * sr / (last - first);
    double c = speedOfSound(sc.tempC), expect = f0 * c / (c - v);
    std::printf("  measured %.2f Hz, expected %.2f Hz (%.3f %% off)\n", f, expect, 100 * (f - expect) / expect);
    if (strength == 1.0) check(std::fabs(f - expect) / expect < 0.003, "Doppler within 0.3 %");
    else {
        const double ratio = (f - f0) / (expect - f0);  // fraction of the physical shift that is left
        std::printf("  shift is %.2f of the physical one\n", ratio);
        if (strength == 0.0) check(ratio < 0.25, "Doppler strength 0: pitch shift mostly gone");
        else if (strength < 1.0) check(ratio > strength * 0.9 && ratio < strength * 1.0 + 0.25, "reduced Doppler strength scales the shift");
        else check(ratio > strength * 0.85 && ratio < strength * 1.1, "boosted Doppler strength scales the shift");
    }
    // click detector: largest sample-to-sample jump vs. the largest the sine itself can produce
    double amp = 0, jump = 0;
    for (size_t i = i0; i < i1; ++i) amp = std::max<double>(amp, std::fabs(out[i]));
    for (size_t i = i0 + 1; i < i1; ++i) jump = std::max<double>(jump, std::fabs(out[i] - out[i - 1]));
    double maxNat = amp * 2 * std::sin(kPi * f / sr) * 1.05;
    std::printf("  max step %.5f, natural max %.5f\n", jump, maxNat);
    check(jump < maxNat * 1.2 && finite(out), "no clicks / discontinuities, finite");
}

static void testPerf(const std::string& dir) {
    std::printf("Test 3: CPU load, 5.1, 2 moving sources + rotating listener, block 256\n");
    Scene sc = loadScene(dir + "/l_room_5_1.txt");
    sc.length = 3.0; sc.rays = 20000;
    EngineConfig cfg;
    cfg.erOrder = 2; cfg.tailRays = 8000; cfg.tailThread = !std::getenv("VERB_NO_TAIL_THREAD"); cfg.tailRefreshMs = 300;
    if (const char* tb = std::getenv("VERB_TB")) cfg.tailBlock = std::atoi(tb);    Engine eng(sc, cfg);
    const int R = eng.numReceivers(), S = eng.numSources(), blk = 256;
    const size_t N = (size_t)(10 * sc.sr);
    std::mt19937 rng(1);
    std::normal_distribution<float> nd(0, 0.1f);
    std::vector<std::vector<float>> in(S, std::vector<float>(blk)), out(R, std::vector<float>(blk));
    std::vector<const float*> ip(S);
    std::vector<float*> op(R);
    for (int s = 0; s < S; ++s) ip[s] = in[s].data();
    for (int r = 0; r < R; ++r) op[r] = out[r].data();
    double worst = 0, total = 0;
    bool ok = true;
    const std::clock_t cpu0 = std::clock();
    for (size_t pos = 0; pos < N; pos += blk) {
        double t = pos / sc.sr;
        if (!std::getenv("VERB_STATIC")) {
            eng.setSourcePosition(0, 2 + 1.0 * std::sin(t), 6 + 1.0 * std::cos(t), 1.7);
            eng.setSourcePosition(1, 8 + 0.5 * std::cos(2 * t), 1.5 + 0.5 * std::sin(2 * t), 1.2);
            eng.setListener(7, 3, 1.5, 20 * t);
        }
        for (int s = 0; s < S; ++s) for (auto& v : in[s]) v = nd(rng);
        auto t0 = std::chrono::steady_clock::now();
        eng.process(ip.data(), op.data(), blk);
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        total += dt;
        worst = std::max(worst, dt);
        for (int r = 0; r < R; ++r) ok &= finite(out[r]);
    }
    double budget = blk / sc.sr;
    std::printf("  process-wide CPU time %.1f %% of real time (all threads, excludes waiting/preemption)\n",
                100.0 * (double)(std::clock() - cpu0) / CLOCKS_PER_SEC / (N / sc.sr));
    std::printf("  mean load %.1f %% of one core, worst block %.2f ms (budget %.2f ms), taps %d, tail refreshes %d\n",
                100 * total / (N / sc.sr), worst * 1000, budget * 1000, eng.activeTaps(), eng.tailRefreshCount());
    check(ok, "output finite");
    check(total / (N / sc.sr) < 0.5, "mean load below 50 % of one core");
    check(worst < budget, "worst block within real-time budget");
}

// Std-dev (dB) of the magnitude response around its 1/6-octave trend, 200 Hz .. 6 kHz: a measure of comb filtering.
static double rippleStd(const std::vector<float>& x, size_t len, double sr) {
    const size_t N = 1 << 16;
    std::vector<cd> a(N);
    for (size_t i = 0; i < len && i < x.size(); ++i) a[i] = x[i] * (0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)len));
    fft(a, false);
    const double df = sr / (double)N;
    const size_t k0 = (size_t)(200 / df), k1 = (size_t)(6000 / df);
    std::vector<double> db(k1 + 1), pre(k1 + 2, 0.0);
    for (size_t k = 0; k <= k1; ++k) db[k] = 20 * std::log10(std::abs(a[k]) + 1e-12);
    for (size_t k = 0; k <= k1; ++k) pre[k + 1] = pre[k] + db[k];
    double s = 0, s2 = 0;
    size_t n = 0;
    for (size_t k = k0; k <= k1; ++k) {
        size_t lo = std::max<size_t>(1, (size_t)(k * 0.944)), hi = std::min(k1, (size_t)(k * 1.059) + 1);
        double r = db[k] - (pre[hi + 1] - pre[lo]) / (double)(hi - lo + 1);
        s += r; s2 += r * r; ++n;
    }
    double m = s / (double)n;
    return std::sqrt(s2 / (double)n - m * m);
}

static void testSpread(const std::string& dir) {
    std::printf("Test 5: comb ripple vs early-reflection level (demo room, 0..30 ms: direct + early)\n");
    Scene sc = loadScene(dir + "/demo_box.txt");
    auto measure = [&](double earlyDb) {
        EngineConfig cfg;
        cfg.tailThread = false; cfg.tailRays = 4000;
        Engine eng(sc, cfg);
        eng.setEarlyLevel(earlyDb);
        const int n = 8192;
        std::vector<float> in0(n, 0.f), in1(n, 0.f);
        in0[0] = 1.f;
        const int R = eng.numReceivers();
        std::vector<std::vector<float>> out(R, std::vector<float>(n));
        std::vector<float*> op(R);
        for (int r = 0; r < R; ++r) op[r] = out[r].data();
        const float* ip[2] = {in0.data(), in1.data()};
        eng.process(ip, op.data(), n);
        return rippleStd(out[0], 1440, sc.sr);
    };
    double r0 = measure(0.0), r6 = measure(-6.0), r12 = measure(-12.0), r24 = measure(-24.0);
    std::printf("  ripple std: 0 dB %.2f | -6 dB %.2f | -12 dB %.2f | -24 dB %.2f\n", r0, r6, r12, r24);
    // In this scene the cardioids face away from the source (direct ~ 0.2), so reflections dominate until they
    // are pushed well below the direct sound; the control must still clearly reduce the ripple at -24 dB.
    check(r24 < r0 * 0.6, "early level -24 dB cuts the comb ripple by more than 40 %");
}

// Listener is thrown far outside the room (and above the ceiling), then brought back: sound must come back.
static void testRecovery(const std::string& dir) {
    std::printf("Test 4: recovery after the listener leaves the room and returns\n");
    Scene sc = loadScene(dir + "/test_box.txt");
    sc.length = 2.0;
    EngineConfig cfg;
    cfg.tailRays = 8000; cfg.tailRefreshMs = 100;
    Engine eng(sc, cfg);
    const int R = eng.numReceivers(), blk = 256;
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0, 0.1f);
    std::vector<float> in(blk);
    std::vector<std::vector<float>> out(R, std::vector<float>(blk));
    std::vector<float*> op(R);
    for (int r = 0; r < R; ++r) op[r] = out[r].data();
    bool fin = true;
    auto phase = [&](double secs, double x, double y, double z, double yaw) {
        double sum = 0, cnt = 0;
        const int blocks = (int)(secs * sc.sr / blk);
        for (int b = 0; b < blocks; ++b) {
            for (auto& v : in) v = nd(rng);
            eng.setListener(x, y, z, yaw);
            const float* ip[1] = {in.data()};
            eng.process(ip, op.data(), blk);
            for (int r = 0; r < R; ++r)
                for (float v : out[r]) {
                    fin &= std::isfinite(v);
                    if (b >= blocks * 2 / 3) { sum += (double)v * v; cnt += 1; }  // measure the last third (settled)
                }
        }
        return std::sqrt(sum / cnt);
    };
    double a = phase(2.0, 6, 3, 1.5, 0);
    double b1 = phase(2.0, -59.2, -57.9, 3.0, 78);
    double b2 = phase(2.0, -59.2, -57.9, 12.4, 78);
    double c = phase(6.0, 6, 3, 1.5, 0);
    std::printf("  rms home %.4f | outside %.4f | outside+above ceiling %.4f | back home %.4f (%+.2f dB vs start)\n",
                a, b1, b2, c, 20 * std::log10(c / a));
    check(fin, "output finite throughout");
    check(std::fabs(20 * std::log10(c / a)) < 1.0, "level returns to within 1 dB after coming back");
}

// Exploration (VERB_EXPLORE=1): comb strength at a given listener pose vs early level and wall scatter. Not pass/fail.
static double cepRatio(const std::vector<float>& x, size_t len, double sr) {
    const size_t N = 1 << 16;
    std::vector<cd> a(N);
    for (size_t i = 0; i < len && i < x.size(); ++i) a[i] = x[i] * (0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)len));
    fft(a, false);
    const double df = sr / (double)N;
    std::vector<cd> lg(N);
    const size_t k0 = (size_t)(150 / df), k1 = (size_t)(8000 / df);
    double mean = 0;
    for (size_t k = k0; k <= k1; ++k) { double v = std::log(std::abs(a[k]) + 1e-9); lg[k] = v; mean += v; }
    mean /= (double)(k1 - k0 + 1);
    for (size_t k = k0; k <= k1; ++k) { lg[k] -= mean; lg[N - k] = lg[k]; }
    fft(lg, true);
    const size_t q0 = (size_t)(0.0007 * sr), q1 = (size_t)(0.025 * sr);
    double peak = 0, avg = 0;
    for (size_t q = q0; q < q1; ++q) { double v = std::abs(lg[q].real()); peak = std::max(peak, v); avg += v; }
    return peak / (avg / (double)(q1 - q0));
}

static void exploreComb(const std::string& dir) {
    std::printf("Explore: direct + early reflections only (no tail), listener (3.29, 3.93, 1.5) yaw 180, demo room, 0..50 ms\n");
    std::printf("  diffusion | ER energy (dB re direct) | periodic-comb strength L / R (x typical)\n");
    for (double diff : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        Scene sc = loadScene(dir + "/demo_box.txt");
        EngineConfig cfg;
        cfg.tailThread = false; cfg.tailRays = 0;
        Engine eng(sc, cfg);
        eng.setListener(3.29, 3.93, 1.5, 180);
        eng.setEarlyDiffusion(diff);
        const int n = 8192, R = eng.numReceivers();
        std::vector<float> in0(n, 0.f), in1(n, 0.f);
        in0[0] = 1.f;
        std::vector<std::vector<float>> out(R, std::vector<float>(n));
        std::vector<float*> op(R);
        for (int r = 0; r < R; ++r) op[r] = out[r].data();
        const float* ip[2] = {in0.data(), in1.data()};
        eng.process(ip, op.data(), n);
        // direct peak = first strong arrival; energy before 200 samples = direct, after = reflections (0..0.2 s)
        double ed = 0, er = 0;
        for (int i = 0; i < n; ++i) { double v = (double)out[0][i] * out[0][i]; if (i < 300) ed += v; else if (i < 9600) er += v; }
        std::printf("    %4.2f    |        %+6.2f           |   %5.1f / %5.1f\n", diff, 10 * std::log10(er / ed),
                    cepRatio(out[0], 2400, sc.sr), cepRatio(out[1], 2400, sc.sr));
    }
}

// A single reflection must have the smooth spectrum its material prescribes (regression: IIR band split with
// mismatched phase put -7 dB notches into carpet/curtain reflections).
static void testReflectionSpectrum(const std::string& dir) {
    std::printf("Test 7: spectrum of a single carpet reflection vs the material's intended curve\n");
    Scene sc = loadScene(dir + "/floor_carpet.txt");
    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 0;
    Engine eng(sc, cfg);
    const int n = 16384;
    std::vector<float> in(n, 0.f), out(n);
    in[0] = 1.f;
    const float* ip[1] = {in.data()};
    float* op[1] = {out.data()};
    eng.process(ip, op, n);
    const double c = speedOfSound(sc.tempC), len = std::sqrt(3.0 * 3.0 + 12.0 * 12.0);  // via the floor image
    const size_t start = (size_t)(len / c * sc.sr) - 48, count = (size_t)(0.03 * sc.sr);
    const size_t N = 1 << 17;
    std::vector<cd> a(N);
    for (size_t i = 0; i < count; ++i) a[i] = out[start + i];
    fft(a, false);
    const Material& m = sc.mats[(size_t)sc.floorMat];
    double g[NB];
    for (int b = 0; b < NB; ++b)
        g[b] = std::sqrt((1 - m.alpha[b]) * (1 - scatterAt(m, b))) * std::pow(10.0, -airAbsDbPerM(kBandHz[b], sc.tempC, sc.humidity) * len / 20.0);
    double worst = 0, wf = 0;
    for (double f = kBandHz[0]; f <= kBandHz[NB - 1]; f *= 1.05) {
        double lf = std::log(f / kBandHz[0]) / std::log(2.0);
        int b0 = std::min((int)lf, NB - 2);
        double t = lf - b0;
        double intent = std::exp(std::log(g[b0]) * (1 - t) + std::log(g[b0 + 1]) * t) / len;
        double got = std::abs(a[(size_t)(f / sc.sr * (double)N)]);
        double dev = 20 * std::log10(got / intent);
        if (std::fabs(dev) > std::fabs(worst)) { worst = dev; wf = f; }
    }
    std::printf("  worst deviation from the intended smooth curve: %+.2f dB at %.0f Hz\n", worst, wf);
    check(std::fabs(worst) < 2.0, "single reflection follows its material curve within 2 dB (no notches)");
}

static void testDiffusion(const std::string& dir) {
    std::printf("Test 6: early diffusion (velvet decorrelation): comb regularity vs energy, direct + early only\n");
    auto measure = [&](double diff, double& energyDb, double& cep) {
        Scene sc = loadScene(dir + "/demo_box.txt");
        EngineConfig cfg;
        cfg.tailThread = false; cfg.tailRays = 0;
        Engine eng(sc, cfg);
        eng.setListener(3.29, 3.93, 1.5, 180);
        eng.setEarlyDiffusion(diff);
        const int n = 8192, R = eng.numReceivers();
        std::vector<float> in0(n, 0.f), in1(n, 0.f);
        in0[0] = 1.f;
        std::vector<std::vector<float>> out(R, std::vector<float>(n));
        std::vector<float*> op(R);
        for (int r = 0; r < R; ++r) op[r] = out[r].data();
        const float* ip[2] = {in0.data(), in1.data()};
        eng.process(ip, op.data(), n);
        double ed = 0, er = 0;
        for (int i = 0; i < n; ++i) { double v = (double)out[0][i] * out[0][i]; if (i < 300) ed += v; else if (i < 9600) er += v; }
        energyDb = 10 * std::log10(er / ed);
        cep = 0.5 * (cepRatio(out[0], 2400, sc.sr) + cepRatio(out[1], 2400, sc.sr));
    };
    double e0, c0, e1, c1, e5, c5;
    measure(0.0, e0, c0); measure(0.5, e5, c5); measure(1.0, e1, c1);
    std::printf("  diffusion 0: ER %+.2f dB, comb %.1f | 0.5: %+.2f dB, %.1f | 1.0: %+.2f dB, %.1f\n", e0, c0, e5, c5, e1, c1);
    check(c5 < c0 * 0.8 && c1 < c0 * 0.85, "diffusion cuts the periodic comb strength by at least 15-20 %");
    check(std::fabs(e5 - e0) < 1.5 && std::fabs(e1 - e0) < 1.5, "early-reflection energy stays within 1.5 dB");
}

// Moving-comb ("swirl") metric: direct + early reflections only, listener walks 1 m/s; an impulse every 4096
// samples gives a snapshot of the frequency response every ~85 ms. Swirl = RMS change (dB) of the response
// between consecutive snapshots, 300 Hz .. 6 kHz, with the overall level change removed.
static double swirl(const Scene& sc, const EngineConfig& cfg, double earlyDb, double diffusion, double speed, bool auditory, bool dual = false) {
    Engine eng(sc, cfg);
    eng.setEarlyLevel(earlyDb);
    eng.setEarlyDiffusion(diffusion);
    const int K = 40, hop = 4096, ctrl = 128;
    const size_t total = (size_t)K * hop;
    std::vector<float> in0(total, 0.f), in1(total, 0.f);
    for (int k = 0; k < K; ++k) in0[(size_t)k * hop] = 1.f;
    if (dual) in1 = in0;  // the same (dual-mono) signal in both inputs
    const int R = eng.numReceivers();
    std::vector<std::vector<float>> out(R, std::vector<float>(total));
    for (size_t pos = 0; pos < total; pos += ctrl) {
        double t = (double)pos / sc.sr;
        eng.setListener(3.3 + speed * t, 3.93, 1.5, 180);
        const float* ip[2] = {in0.data() + pos, in1.data() + pos};
        std::vector<float*> op(R);
        for (int r = 0; r < R; ++r) op[r] = out[r].data() + pos;
        eng.process(ip, op.data(), ctrl);
    }
    const size_t W = 3000, N = 8192;
    const size_t k0 = (size_t)(300.0 / sc.sr * N), k1 = (size_t)(6000.0 / sc.sr * N);
    std::vector<std::vector<double>> L;
    for (int k = 2; k < K; ++k) {  // skip the start-up snapshots
        std::vector<cd> a(N);
        for (size_t i = 0; i < W; ++i) a[i] = out[0][(size_t)k * hop + i] * (0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)W));
        fft(a, false);
        std::vector<double> db;
        if (auditory) {
            // 1/3-octave power bands 300 Hz .. 6 kHz: roughly what the ear's critical bands resolve
            for (double fc = 300.0; fc < 6000.0; fc *= std::pow(2.0, 1.0 / 3.0)) {
                size_t lo = (size_t)(fc / std::pow(2.0, 1.0 / 6.0) / sc.sr * N), hi = (size_t)(fc * std::pow(2.0, 1.0 / 6.0) / sc.sr * N);
                double p = 0;
                for (size_t b = lo; b <= hi; ++b) p += std::norm(a[b]);
                db.push_back(10 * std::log10(p / (double)(hi - lo + 1) + 1e-18));
            }
        } else {
            db.resize(k1 - k0 + 1);
            for (size_t b = k0; b <= k1; ++b) db[b - k0] = 20 * std::log10(std::abs(a[b]) + 1e-9);
        }
        L.push_back(db);
    }
    double acc = 0;
    int cnt = 0;
    for (size_t i = 0; i + 1 < L.size(); ++i) {
        double mean = 0;
        std::vector<double> d(L[i].size());
        for (size_t b = 0; b < d.size(); ++b) { d[b] = L[i + 1][b] - L[i][b]; mean += d[b]; }
        mean /= (double)d.size();
        double s2 = 0;
        for (double v : d) s2 += (v - mean) * (v - mean);
        acc += std::sqrt(s2 / (double)d.size());
        ++cnt;
    }
    return acc / cnt;
}

// 1/3-octave band powers (300 Hz .. 6 kHz) of receiver 0 for impulse snapshots while the listener walks.
// in0On / in1On choose which inputs carry the (identical) impulse train.
static std::vector<std::vector<double>> bandPowerSnapshots(const Scene& sc, const EngineConfig& cfg, int inputMode, bool in0On, bool in1On,
                                                           double earlyDb = -9, double diffusion = 0.5) {
    Engine eng(sc, cfg);
    eng.setEarlyLevel(earlyDb);
    eng.setEarlyDiffusion(diffusion);
    const int K = 40, hop = 4096, ctrl = 128;
    const size_t total = (size_t)K * hop;
    std::vector<float> in0(total, 0.f), in1(total, 0.f);
    for (int k = 0; k < K; ++k) { if (in0On) in0[(size_t)k * hop] = 1.f; if (in1On) in1[(size_t)k * hop] = 1.f; }
    if (inputMode == 1) {  // "sum" mode as the plugin does it: average of the inputs drives source 1 only
        for (size_t i = 0; i < total; ++i) { in0[i] = 0.5f * (in0[i] + in1[i]); in1[i] = 0.f; }
    }
    const int R = eng.numReceivers();
    std::vector<std::vector<float>> out(R, std::vector<float>(total));
    for (size_t pos = 0; pos < total; pos += ctrl) {
        eng.setListener(3.3 + 1.0 * ((double)pos / sc.sr), 3.93, 1.5, 180);
        const float* ip[2] = {in0.data() + pos, in1.data() + pos};
        std::vector<float*> op(R);
        for (int r = 0; r < R; ++r) op[r] = out[r].data() + pos;
        eng.process(ip, op.data(), ctrl);
    }
    const size_t W = 3000, N = 8192;
    std::vector<std::vector<double>> res;
    for (int k = 2; k < K; ++k) {
        std::vector<cd> a(N);
        for (size_t i = 0; i < W; ++i) a[i] = out[0][(size_t)k * hop + i] * (0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)W));
        fft(a, false);
        std::vector<double> p;
        for (double fc = 300.0; fc < 6000.0; fc *= std::pow(2.0, 1.0 / 3.0)) {
            size_t lo = (size_t)(fc / std::pow(2.0, 1.0 / 6.0) / sc.sr * N), hi = (size_t)(fc * std::pow(2.0, 1.0 / 6.0) / sc.sr * N);
            double s = 0;
            for (size_t b = lo; b <= hi; ++b) s += std::norm(a[b]);
            p.push_back(s / (double)(hi - lo + 1));
        }
        res.push_back(p);
    }
    return res;
}

// Dual-mono voice into two sources: deepest 1/3-octave dip of the summed power relative to the power sum of the
// two sources alone (0 dB = no coherent cancellation), averaged over positions; also the typical |deviation|.
static void dualMonoDips(const Scene& sc, const EngineConfig& cfg, int mode, double& worstDip, double& meanAbsDev) {
    auto a = bandPowerSnapshots(sc, cfg, 0, true, false), b = bandPowerSnapshots(sc, cfg, 0, false, true);
    auto both = bandPowerSnapshots(sc, cfg, mode, true, true);
    double wsum = 0, dsum = 0;
    size_t n = 0, nb = 0;
    for (size_t k = 0; k < both.size(); ++k) {
        double worst = 0;
        for (size_t i = 0; i < both[k].size(); ++i) {
            double ref = a[k][i] + b[k][i];
            if (mode == 1) ref = a[k][i];  // sum mode: everything plays from source 1 only
            double d = 10 * std::log10(both[k][i] / ref + 1e-18);
            worst = std::min(worst, d);
            dsum += std::fabs(d);
            ++nb;
        }
        wsum += worst;
        ++n;
    }
    worstDip = wsum / (double)n;
    meanAbsDev = dsum / (double)nb;
}

static void exploreSwirl(const std::string& dir) {
    Scene sc = loadScene(dir + "/demo_furnished.txt");
    std::printf("Swirl: dB change of the spectrum per 85 ms while walking (lower = smoother), furnished demo room\n");
    std::printf("  %-52s %9s %9s\n", "", "fine", "1/3-oct");
    auto run = [&](const char* name, int order, double early, double diff, double speed = 1.0) {
        EngineConfig cfg;
        cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = order;
        std::printf("  %-52s %6.2f dB %6.2f dB\n", name, swirl(sc, cfg, early, diff, speed, false), swirl(sc, cfg, early, diff, speed, true));
    };
    run("standing still (reference, should be ~0)", 2, 0, 0, 0.0);
    run("order 2, early 0 dB, diffusion 0   (physical)", 2, 0, 0);
    run("order 2, early 0 dB, diffusion 0.5 (plugin default)", 2, 0, 0.5);
    run("order 2, early 0 dB, diffusion 1", 2, 0, 1.0);
    run("order 2, early -6 dB, diffusion 0.5", 2, -6, 0.5);
    run("order 2, early -12 dB, diffusion 0.5", 2, -12, 0.5);
    run("order 2, early -18 dB, diffusion 0.5", 2, -18, 0.5);
    run("order 1, early 0 dB, diffusion 0", 1, 0, 0);
    run("order 1, early -12 dB, diffusion 0.5", 1, -12, 0.5);
    {
        EngineConfig cfg;
        cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = 1;
        std::printf("  --- dual-mono voice in both inputs (walking 1 m/s): coherent cancellation between the two sources ---\n");
        std::printf("  %-34s %-26s %s\n", "input mode", "worst 1/3-oct dip (avg)", "mean |deviation| from power sum");
        for (int mode : {0, 1}) {
            double w, d;
            dualMonoDips(sc, cfg, mode, w, d);
            std::printf("  %-34s %6.1f dB                  %5.2f dB\n", mode == 0 ? "0 separate sources" : "1 sum into source 1", w, d);
        }
        std::printf("  --- plugin defaults (order 1, -9 dB, diffusion 0.5), walking 1 m/s ---\n");
        std::printf("  %-52s %6.2f dB %6.2f dB\n", "one source", swirl(sc, cfg, -9, 0.5, 1.0, false), swirl(sc, cfg, -9, 0.5, 1.0, true));
        std::printf("  %-52s %6.2f dB %6.2f dB\n", "TWO sources, same signal in both inputs (dual mono)", swirl(sc, cfg, -9, 0.5, 1.0, false, true), swirl(sc, cfg, -9, 0.5, 1.0, true, true));
    }
    run("order 1, early -6 dB, diffusion 0.5", 1, -6, 0.5);
    run("order 1, early -9 dB, diffusion 0.5", 1, -9, 0.5);
    run("order 1, early -9 dB, diffusion 0.5, 2 m/s", 1, -9, 0.5, 2.0);
    run("order 2, early 0 dB, diffusion 0, 0.25 m/s", 2, 0, 0, 0.25);
    run("order 2, early 0 dB, diffusion 0, 2 m/s", 2, 0, 0, 2.0);
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
static double workingSetMB() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    return -1;
}
#else
static double workingSetMB() { return -1; }
#endif

// Demo room with `n` mono sources on a grid and a virtual-microphone array for `layout` at the listener spot.
static Scene manySources(const std::string& dir, int n, verb::Layout layout) {
    Scene sc = loadScene(dir + "/demo_furnished.txt");
    sc.sources.clear();
    for (int i = 0; i < n; ++i) {
        Transducer t;
        t.name = "S" + std::to_string(i + 1);
        t.pos = {1.0 + 6.0 * ((i % 4) + 0.5) / 4.0, 0.8 + 4.4 * ((i / 4) + 0.5) / std::max(1, (n + 3) / 4), 1.5};
        sc.sources.push_back(t);
    }
    sc.receivers = verb::makeReceiverArray(layout, {4.0, 3.0, 1.5}, 0.1);
    sc.hasListener = true;
    sc.listenerPos = {4.0, 3.0, 1.5};
    sc.listenerYaw = 0;
    return sc;
}

static void testSharedTail(const std::string& dir) {
    std::printf("Test 11: shared late field vs per-source late field (2 sources, ORTF), energy of the tail per channel\n");
    Scene sc = manySources(dir, 2, verb::Layout::StereoORTF);
    sc.length = 2.0;
    auto tailEnergy = [&](bool shared, int src, std::vector<double>& e) {
        EngineConfig cfg;
        cfg.tailThread = false; cfg.erOrder = 1; cfg.sharedTail = shared;
        cfg.tailRays = shared ? 60000 : 30000;  // per source: equal rays per source in both modes
        Engine eng(sc, cfg);
        const int n = (int)(1.8 * sc.sr), R = eng.numReceivers();
        std::vector<float> a(n, 0.f), z(n, 0.f);
        a[0] = 1.f;
        std::vector<std::vector<float>> out(R, std::vector<float>(n));
        std::vector<float*> op(R);
        for (int r = 0; r < R; ++r) op[r] = out[r].data();
        const float* ip[2] = {src == 0 ? a.data() : z.data(), src == 1 ? a.data() : z.data()};
        eng.process(ip, op.data(), n);
        e.assign(R, 0.0);
        for (int r = 0; r < R; ++r) e[(size_t)r] = energy(out[(size_t)r], (size_t)(0.3 * sc.sr), (size_t)n);
    };
    bool ok = true;
    for (int src = 0; src < 2; ++src) {
        std::vector<double> ep, es;
        tailEnergy(false, src, ep);
        tailEnergy(true, src, es);
        for (size_t r = 0; r < ep.size(); ++r) {
            const double d = 10 * std::log10(es[r] / ep[r]);
            std::printf("  source %d, channel %zu: per-source %.3e, shared %.3e (%+.2f dB)\n", src + 1, r, ep[r], es[r], d);
            ok &= std::fabs(d) < 2.0;
        }
    }
    check(ok, "shared late field carries the same tail energy per source and channel (within 2 dB)");
}

static void testManySources(const std::string& dir) {
    std::printf("Test 12: scale target: 16 sources, 7.1.4 output (12 channels), 4 sources moving, shared late field\n");
    Scene sc = manySources(dir, 16, verb::Layout::Surround714);
    sc.length = 2.0;
    if (std::getenv("VERB_NO_DIFF")) sc.diffraction = false;  // for comparing the cost of diffraction
    EngineConfig cfg;
    cfg.erOrder = 1; cfg.sharedTail = true; cfg.maxEarlyDelaySec = 0.4;
    cfg.tailRays = 48000; cfg.tailThread = !std::getenv("VERB_NO_TAIL_THREAD");
    const double mem0 = workingSetMB();
    auto t0 = std::chrono::steady_clock::now();
    Engine eng(sc, cfg);
    const double ctor = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double mem1 = workingSetMB();
    const int S = eng.numSources(), R = eng.numReceivers(), blk = 256;
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0, 0.05f);
    std::vector<std::vector<float>> in(S, std::vector<float>(blk)), out(R, std::vector<float>(blk));
    std::vector<const float*> ip(S);
    std::vector<float*> op(R);
    for (int s = 0; s < S; ++s) ip[s] = in[s].data();
    for (int r = 0; r < R; ++r) op[r] = out[r].data();
    const size_t N = (size_t)(8 * sc.sr);
    const std::clock_t cpu0 = std::clock();
    double total = 0, worst = 0;
    bool fin = true;
    for (size_t pos = 0; pos < N; pos += blk) {
        const double t = (double)pos / sc.sr;
        for (int s = 0; s < (std::getenv("VERB_STATIC") ? 0 : 4); ++s)  // 4 of 16 sources move on small circles
            eng.setSourcePosition(s, sc.sources[(size_t)s].pos.x + 0.6 * std::sin(t + s), sc.sources[(size_t)s].pos.y + 0.6 * std::cos(t + s), 1.5);
        for (int s = 0; s < S; ++s) for (auto& v : in[(size_t)s]) v = nd(rng);
        auto a = std::chrono::steady_clock::now();
        eng.process(ip.data(), op.data(), blk);
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
        total += dt;
        worst = std::max(worst, dt);
        for (int r = 0; r < R; ++r) for (float v : out[(size_t)r]) fin &= std::isfinite(v);
    }
    const double load = 100 * total / (N / sc.sr);
    std::printf("  build %.1f s | memory +%.0f MB (total %.0f MB) | mean load %.1f %% of one core, worst block %.2f ms (budget %.2f ms), taps %d, tail refreshes %d\n",
                ctor, mem1 - mem0, mem1, load, worst * 1000, 1000.0 * blk / sc.sr, eng.activeTaps(), eng.tailRefreshCount());
    std::printf("  process-wide CPU time %.1f %% (all threads)\n", 100.0 * (double)(std::clock() - cpu0) / CLOCKS_PER_SEC / (N / sc.sr));
    check(fin, "output finite");
    check(mem1 - mem0 < 600, "memory growth below 600 MB");
    check(load < 70, "mean audio-thread load below 70 % of one core");
    // single outliers under foreign CPU load (a DAW running next to the test) are tolerated: 2x the budget at most
    check(worst < 2.0 * blk / sc.sr, "worst block within 2x the real-time budget");
}

static int presetIndex(const char* name) {
    const auto& p = surfacePresets();
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i].name == name) return (int)i;
    return -1;
}

// Free field (absorbing floor, open sky): source at x=0, receiver at x=4, optionally one wall segment at x=2.
static Scene wallScene(const char* preset) {
    Scene sc;
    sc.sr = 48000; sc.length = 1.0; sc.height = 4.0; sc.order = 1;
    sc.diffraction = false;  // these tests check plain blocking; the diffraction test switches it on
    sc.mats = surfacePresets();
    sc.floorMat = presetIndex("Absorber");
    sc.ceilMat = -1;
    if (preset) sc.walls.push_back(Wall{2, -5, 2, 5, presetIndex(preset)});
    Transducer s, r;
    s.name = "S"; s.pos = {0, 0, 1.5};
    r.name = "M"; r.pos = {4, 0, 1.5};
    sc.sources.push_back(s);
    sc.receivers.push_back(r);
    return sc;
}

static double bandPowerOf(const std::vector<float>& x, double sr, double f0, double f1) {
    const size_t N = 1 << 14;
    std::vector<cd> a(N);
    for (size_t i = 0; i < x.size() && i < N; ++i) a[i] = x[i];
    fft(a, false);
    double s = 0;
    size_t n = 0;
    for (size_t k = (size_t)(f0 / sr * N); k <= (size_t)(f1 / sr * N); ++k) { s += std::norm(a[k]); ++n; }
    return s / (double)n;
}

static void testTransmission() {
    std::printf("Test 10: sound through partial walls and runtime scene replacement\n");
    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = 1;
    auto ir = [&](const Scene& sc) {
        Engine eng(sc, cfg);
        std::vector<float> in(4096, 0.f), out(4096);
        in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[1] = {out.data()};
        eng.process(ip, op, 4096);
        return out;
    };
    auto ref = ir(wallScene(nullptr));
    auto drywall = ir(wallScene("Drywall_partition")), open = ir(wallScene("Open")), concrete = ir(wallScene("Concrete"));
    auto rel = [&](const std::vector<float>& x, double f0, double f1) {
        return 10 * std::log10(bandPowerOf(x, 48000, f0, f1) / bandPowerOf(ref, 48000, f0, f1) + 1e-30);
    };
    std::printf("  drywall partition: %.1f dB at 250 Hz (TL 18), %.1f dB at 4 kHz (TL 33) | open: %.2f dB | concrete: %.0f dB\n",
                rel(drywall, 200, 315), rel(drywall, 3150, 5000), rel(open, 200, 5000), rel(concrete, 200, 5000));
    // The 7-band split realises steep (> ~3 dB/octave) loss curves only approximately: about 5 dB at the deep end.
    check(std::fabs(rel(drywall, 200, 315) + 18) < 3.0 && std::fabs(rel(drywall, 3150, 5000) + 33) < 6.0 && rel(drywall, 3150, 5000) < rel(drywall, 200, 315) - 6,
          "direct sound through a drywall partition is attenuated by its transmission loss, more at high frequencies");
    check(std::fabs(rel(open, 200, 5000)) < 0.5, "an Open segment (doorway) passes the sound unchanged");
    check(rel(concrete, 200, 5000) < -80, "an opaque wall blocks the direct sound");

    // runtime replacement while processing continuous noise
    Engine eng(wallScene("Concrete"), cfg);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0, 0.1f);
    std::vector<float> in(256), out(256);
    bool fin = true;
    auto rms = [&](double secs) {
        double s = 0, cnt = 0;
        for (int b = 0; b < (int)(secs * 48000 / 256); ++b) {
            for (auto& v : in) v = nd(rng);
            const float* ip[1] = {in.data()};
            float* op[1] = {out.data()};
            eng.process(ip, op, 256);
            for (float v : out) { fin &= std::isfinite(v); s += (double)v * v; cnt += 1; }
        }
        return std::sqrt(s / cnt);
    };
    auto settle = [&](double secs) { rms(secs); };
    rms(0.3);
    const double r0 = rms(0.3);
    check(eng.setScene(wallScene("Open")), "setScene accepts a replacement geometry");
    settle(0.3);
    const double r1 = rms(0.3);
    eng.setScene(wallScene("Concrete"));
    settle(0.3);
    const double r2 = rms(0.3);
    eng.setScene(wallScene(nullptr));
    settle(0.3);
    const double r3 = rms(0.3);
    std::printf("  rms: concrete %.5f -> open %.5f -> concrete %.5f -> no wall %.5f\n", r0, r1, r2, r3);
    check(fin && r0 < 1e-4 && r1 > 1e-3 && r2 < 1e-4 && std::fabs(20 * std::log10(r3 / r1)) < 1.0, "walls replaced while running: sound appears/disappears as the geometry says, nothing breaks");
}

static void testZones() {
    std::printf("Test 20: floor and ceiling zones (material by region)\n");
    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = 1;
    auto square = [](double cx, double cy, double h, int mat) {
        Zone z;
        z.mat = mat;
        z.poly = {{cx - h, cy - h}, {cx + h, cy - h}, {cx + h, cy + h}, {cx - h, cy + h}};
        return z;
    };
    // energy after the direct sound = the floor/ceiling reflection (S=(0,0,1.5), M=(4,0,1.5): the bounce is at x=2)
    auto reflected = [&](const Scene& sc) {
        Engine eng(sc, cfg);
        std::vector<float> in(8192, 0.f), out(8192);
        in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[1] = {out.data()};
        eng.process(ip, op, 8192);
        size_t first = 0;
        while (first < out.size() && std::fabs(out[first]) < 1e-4f) ++first;
        return 10 * std::log10(energy(out, first + 40, out.size()) + 1e-30);
    };
    const int concrete = presetIndex("Concrete"), absorber = presetIndex("Absorber");
    Scene hard = wallScene(nullptr);
    hard.floorMat = concrete;
    const double base = reflected(hard);
    Scene carpetHere = hard, carpetFar = hard;
    carpetHere.floorZones.push_back(square(2, 0, 1.0, absorber));
    carpetFar.floorZones.push_back(square(12, 9, 1.0, absorber));
    const double here = reflected(carpetHere), elsewhere = reflected(carpetFar);
    std::printf("  floor bounce: hard floor %.1f dB, absorber zone under the bounce %.1f dB, absorber zone elsewhere %.1f dB\n", base, here, elsewhere);
    check(here < base - 10.0, "a floor zone under the reflection point decides the reflection");
    check(std::fabs(elsewhere - base) < 0.2, "a floor zone elsewhere does not touch it");
    Scene roof = wallScene(nullptr);
    roof.floorMat = absorber;
    roof.ceilMat = concrete;
    const double ceilBase = reflected(roof);
    Scene hole = roof, tiles = roof;
    hole.ceilZones.push_back(square(2, 0, 1.0, -1));
    tiles.ceilZones.push_back(square(2, 0, 1.0, absorber));
    const double ceilHole = reflected(hole), ceilTiles = reflected(tiles);
    std::printf("  ceiling bounce: concrete %.1f dB, opening above the bounce %.1f dB, absorber tiles %.1f dB\n", ceilBase, ceilHole, ceilTiles);
    check(ceilHole < ceilBase - 30.0, "an opening in the ceiling lets the sound out instead of reflecting it");
    check(ceilTiles < ceilBase - 10.0, "a ceiling zone changes the ceiling reflection");
    // a ceiling that exists only over part of an open sky
    Scene partial = wallScene(nullptr);
    partial.floorMat = absorber;
    partial.ceilMat = -1;
    partial.ceilZones.push_back(square(2, 0, 1.0, concrete));
    const double canopy = reflected(partial);
    std::printf("  canopy over the bounce point of an open sky: %.1f dB\n", canopy);
    check(canopy > ceilBase - 1.0, "a ceiling zone over open sky reflects like a ceiling");
    // text round trip and sanitising
    Scene rt = hard;
    rt.floorZones.push_back(square(1, 2, 0.5, absorber));
    rt.ceilZones.push_back(square(0, 0, 3, -1));
    std::istringstream is(serializeScene(rt));
    Scene back = parseScene(is, "zones");
    check(back.floorZones.size() == 1 && back.ceilZones.size() == 1 && back.ceilZones[0].mat < 0 && back.floorZones[0].poly.size() == 4 &&
              back.mats[(size_t)back.floorZones[0].mat].name == "Absorber",
          "zones survive serialise -> parse");
    Scene bad = hard;
    Zone z1; z1.mat = 99; z1.poly = {{0, 0}, {1, 0}, {1, 1}};
    Zone z2; z2.mat = absorber; z2.poly = {{0, 0}, {NAN, 0}, {1, 1}};
    Zone z3; z3.mat = absorber; z3.poly = {{0, 0}, {1, 1}};
    bad.floorZones = {z1, z2, z3};
    sanitizeScene(bad);
    check(bad.floorZones.empty(), "broken zones (bad material, NaN, fewer than 3 corners) are dropped");
}

static void testDiffraction() {
    std::printf("Test 19: edge diffraction (soft shadow boundary)\n");
    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = 1;  // (source and receiver are on opposite sides: the screen reflects nothing)
    // a screen x=2, y in [0,5]; source (0,1); receiver on x=4 moves from the lit side into the shadow (boundary at y=-1)
    auto level = [&](double yr, bool diffraction) {
        Scene sc = wallScene("Concrete");
        sc.walls[0] = Wall{2, 0, 2, 5, presetIndex("Concrete")};
        sc.diffraction = diffraction;
        sc.sources[0].pos = {0, 1, 1.5};
        sc.receivers[0].pos = {4, yr, 1.5};
        Engine eng(sc, cfg);
        std::vector<float> in(8192, 0.f), out(8192);
        in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[1] = {out.data()};
        eng.process(ip, op, 8192);
        const double dist = std::hypot(4.0, yr - 1.0);
        return 10 * std::log10(bandPowerOf(out, 48000, 2000, 4000) * dist * dist + 1e-30);  // distance-compensated
    };
    std::vector<double> ys, with, without;
    for (double y = -3.0; y <= 1.51; y += 0.1) { ys.push_back(y); with.push_back(level(y, true)); without.push_back(level(y, false)); }
    const double ref = with.front();
    double maxStep = 0, maxStepOff = 0, atEdge = 0;
    for (size_t i = 1; i < ys.size(); ++i) {
        maxStep = std::max(maxStep, std::fabs(with[i] - with[i - 1]));
        maxStepOff = std::max(maxStepOff, std::fabs(without[i] - without[i - 1]));
        if (std::fabs(ys[i] + 1.0) < 0.05) atEdge = with[i] - ref;
    }
    std::printf("  2-4 kHz level re lit side: at the boundary %+.1f dB, deep shadow (y=+1.5) %+.1f dB; biggest step per 0.1 m: %.1f dB with, %.1f dB without diffraction\n",
                atEdge, with.back() - ref, maxStep, maxStepOff);
    check(maxStep < 3.0, "with diffraction the level changes smoothly across the shadow boundary");
    check(maxStepOff > 20.0, "without diffraction the shadow is a hard cut");
    check(atEdge < -3.0 && atEdge > -10.0, "about -6 dB right at the boundary");
    check(with.back() < ref - 10.0 && with.back() > ref - 60.0, "deep in the shadow the level is much lower but sound still arrives");
    // low frequencies bend more than high ones
    auto band = [&](double yr, double f0, double f1) {
        Scene sc = wallScene("Concrete");
        sc.walls[0] = Wall{2, 0, 2, 5, presetIndex("Concrete")};
        sc.diffraction = true;
        sc.sources[0].pos = {0, 1, 1.5};
        sc.receivers[0].pos = {4, yr, 1.5};
        Engine eng(sc, cfg);
        std::vector<float> in(8192, 0.f), out(8192);
        in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[1] = {out.data()};
        eng.process(ip, op, 8192);
        return 10 * std::log10(bandPowerOf(out, 48000, f0, f1) + 1e-30);
    };
    const double lowDrop = band(1.0, 200, 400) - band(-3.0, 200, 400), highDrop = band(1.0, 3000, 5000) - band(-3.0, 3000, 5000);
    std::printf("  shadow depth at y=+1: %.1f dB at 200-400 Hz, %.1f dB at 3-5 kHz\n", lowDrop, highDrop);
    check(lowDrop > highDrop + 4.0, "the shadow is deeper at high frequencies");
}

// Plates: horizontal surfaces at their own height (lowered ceiling, platform).
static void testPlates() {
    std::printf("Test 25: horizontal plates at their own height\n");
    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = 1;
    auto rect = [](double x0, double y0, double x1, double y1, int mat, double z) {
        Zone zn;
        zn.mat = mat;
        zn.z = z;
        zn.poly = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        return zn;
    };
    auto ir = [&](const Scene& sc) {
        Engine eng(sc, cfg);
        std::vector<float> in(8192, 0.f), out(8192);
        in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[1] = {out.data()};
        eng.process(ip, op, 8192);
        return out;
    };
    auto firstIdx = [](const std::vector<float>& o) { size_t i = 0; while (i < o.size() && std::fabs(o[i]) < 1e-4f) ++i; return i; };
    const int concrete = presetIndex("Concrete"), absorber = presetIndex("Absorber"), drywall = presetIndex("Drywall_partition");
    // a source 4 m from the listener, both at 1.5 m; the sky is open and the floor absorbs
    Scene open = wallScene(nullptr);
    open.floorMat = absorber;
    open.ceilMat = -1;
    const auto base = ir(open);
    const size_t d0 = firstIdx(base);
    // 1. a lowered ceiling (plate at 2.5 m over the bounce point) reflects at the right time
    Scene low = open;
    low.ceilZones.push_back(rect(1, -1, 3, 1, concrete, 2.5));
    const auto lo = ir(low);
    // path via the plate: sqrt(4^2 + 2^2) = 4.472 m, 0.472 m longer than the direct sound
    const size_t expect = d0 + (size_t)std::lround(0.472 / 343.2 * 48000);
    double eAround = 0, eElse = 0;
    for (size_t i = d0 + 5; i < 4000; ++i) {
        const double v = (double)lo[i] * lo[i];
        if (i + 6 >= expect && i <= expect + 6) eAround += v; else eElse += v;
    }
    std::printf("  lowered ceiling: reflection energy at the expected delay %.2e, elsewhere %.2e\n", eAround, eElse);
    check(eAround > 20 * eElse && eAround > 1e-6, "a plate at 2.5 m reflects with the delay of its own height");
    // 2. a platform (plate over the floor) reflects from its top
    Scene plat = open;
    plat.floorZones.push_back(rect(1, -1, 3, 1, concrete, 0.5));
    const auto pl = ir(plat);
    const size_t expectP = d0 + (size_t)std::lround(0.472 / 343.2 * 48000);  // image at z=-0.5: dz = 2 m as well
    double eP = 0, eO = 0;
    for (size_t i = d0 + 5; i < 4000; ++i) {
        const double v = (double)pl[i] * pl[i];
        if (i + 6 >= expectP && i <= expectP + 6) eP += v; else eO += v;
    }
    check(eP > 20 * eO && eP > 1e-6, "a platform at 0.5 m reflects from its top");
    // 3. the plate from above: a listener above a closed plate hears the direct sound only through it
    auto level = [&](const Scene& sc) { const auto o = ir(sc); double e = 0; for (float v : o) e += (double)v * v; return 10 * std::log10(e + 1e-30); };
    Scene above = open;
    above.receivers[0].pos = {4, 0, 3.0};
    const double clear = level(above);
    Scene shut = above, thin = above;
    shut.ceilZones.push_back(rect(-2, -3, 6, 3, concrete, 2.2));
    thin.ceilZones.push_back(rect(-2, -3, 6, 3, drywall, 2.2));
    const double lc = level(shut), lt = level(thin);
    std::printf("  listener above a plate: open %.1f dB, concrete %.1f dB, drywall %.1f dB\n", clear, lc, lt);
    check(lc < clear - 25.0, "an opaque plate blocks the direct sound to a listener above it");
    check(lt < clear - 3.0 && lt > lc + 6.0, "a thin plate lets part of the sound through");
    // 4. text format and limits
    Scene t = open;
    t.mats = surfacePresets();
    t.ceilZones.push_back(rect(1, -1, 3, 1, concrete, 2.5));
    t.floorZones.push_back(rect(1, -1, 3, 1, concrete, 0.5));
    t.walls.push_back(Wall{0, -3, 0, 3, concrete});
    const std::string text = serializeScene(t);
    std::istringstream in(text);
    Scene back = parseScene(in, "plates");
    check(back.ceilZones.size() == 1 && std::fabs(back.ceilZones[0].z - 2.5) < 1e-9 && back.floorZones.size() == 1 && std::fabs(back.floorZones[0].z - 0.5) < 1e-9, "plates survive the text format");
    Scene many = open;
    for (int i = 0; i < 12; ++i) many.ceilZones.push_back(rect(i, 0, i + 0.5, 1, concrete, 2.0));
    sanitizeScene(many);
    int plates = 0;
    for (auto& z : many.ceilZones) plates += z.z >= 0;
    check(plates == kMaxPlates, "the number of plates is limited");
}

// A source walks out from behind a building (a closed box) into view of the listener: the level must not jump.
static void testBuildingCorner() {
    std::printf("Test 24: source emerging from behind a building corner\n");
    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = 1;
    auto level = [&](double xs, bool early) {
        Scene sc = wallScene(nullptr);
        sc.diffraction = true;
        const int m = presetIndex("Concrete");
        sc.walls = {Wall{-3, -2, 3, -2, m}, Wall{3, -2, 3, 2, m}, Wall{3, 2, -3, 2, m}, Wall{-3, 2, -3, -2, m}};
        sc.sources[0].pos = {xs, 4.0, 1.5};     // behind the building
        sc.receivers[0].pos = {0, -6.0, 1.5};   // in front of it
        (void)early;
        Engine eng(sc, cfg);
        std::vector<float> in(8192, 0.f), out(8192);
        in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[1] = {out.data()};
        eng.process(ip, op, 8192);
        const double dist = std::hypot(xs, 10.0);
        return 10 * std::log10(bandPowerOf(out, 48000, 1000, 4000) * dist * dist + 1e-30);
    };
    std::vector<double> xs, v;
    for (double x = 0.0; x <= 9.01; x += 0.25) { xs.push_back(x); v.push_back(level(x, true)); }
    double maxStep = 0, at = 0;
    for (size_t i = 1; i < xs.size(); ++i) {
        std::printf("   x=%5.2f  %7.1f dB\n", xs[i], v[i] - v.back());
        if (std::fabs(v[i] - v[i - 1]) > maxStep) { maxStep = std::fabs(v[i] - v[i - 1]); at = xs[i]; }
    }
    std::printf("  biggest step per 0.25 m: %.1f dB at x=%.2f\n", maxStep, at);
    check(maxStep < 4.0, "no jump when the source comes out from behind the corner");
}

static void testBuildingCornerPartial() {
    std::printf("Test 26: the same with walls that let some sound through (doors, panels)\n");
    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = 1;
    auto level = [&](double xs, bool early) {
        Scene sc = wallScene(nullptr);
        sc.diffraction = true;
        const int m = presetIndex("Metal_door");
        sc.walls = {Wall{-3, -2, 3, -2, m}, Wall{3, -2, 3, 2, m}, Wall{3, 2, -3, 2, m}, Wall{-3, 2, -3, -2, m}};
        sc.sources[0].pos = {xs, 4.0, 1.5};     // behind the building
        sc.receivers[0].pos = {0, -6.0, 1.5};   // in front of it
        (void)early;
        Engine eng(sc, cfg);
        std::vector<float> in(8192, 0.f), out(8192);
        in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[1] = {out.data()};
        eng.process(ip, op, 8192);
        const double dist = std::hypot(xs, 10.0);
        return 10 * std::log10(bandPowerOf(out, 48000, 1000, 4000) * dist * dist + 1e-30);
    };
    std::vector<double> xs, v;
    for (double x = 0.0; x <= 9.01; x += 0.25) { xs.push_back(x); v.push_back(level(x, true)); }
    double maxStep = 0, at = 0;
    for (size_t i = 1; i < xs.size(); ++i) {
        std::printf("   x=%5.2f  %7.1f dB\n", xs[i], v[i] - v.back());
        if (std::fabs(v[i] - v[i - 1]) > maxStep) { maxStep = std::fabs(v[i] - v[i - 1]); at = xs[i]; }
    }
    std::printf("  biggest step per 0.25 m: %.1f dB at x=%.2f\n", maxStep, at);
    check(maxStep < 4.0, "no jump at the corner of a building whose walls let some sound through");
}

// How smooth is the late tail? Window energies (10 ms) of an impulse response over 0.35..2.2 s, in dB, against the
// straight line (exponential decay) fitted through them: the residual's standard deviation. A diffuse noise tail is
// within ~0.5 dB; sparse ray statistics give a ragged envelope that is heard as modulation / fragmentation.
static double tailRoughnessDb(const Scene& sc, int tailRays, size_t* nWin = nullptr) {
    EngineConfig cfg;
    cfg.tailThread = false; cfg.erOrder = 1; cfg.tailRays = tailRays;
    Engine eng(sc, cfg);
    const size_t N = (size_t)(sc.length * sc.sr);
    const int S = eng.numSources(), R = eng.numReceivers();
    std::vector<std::vector<float>> in(S, std::vector<float>(512, 0.f)), out(R, std::vector<float>(N, 0.f));
    std::vector<const float*> ip(S);
    std::vector<float*> op(R);
    std::vector<std::vector<float>> blk(R, std::vector<float>(512));
    for (size_t pos = 0; pos < N; pos += 512) {
        const int n = (int)std::min<size_t>(512, N - pos);
        std::fill(in[0].begin(), in[0].end(), 0.f);
        if (pos == 0) in[0][0] = 1.f;
        for (int s = 0; s < S; ++s) ip[s] = in[s].data();
        for (int r = 0; r < R; ++r) op[r] = blk[r].data();
        eng.process(ip.data(), op.data(), n);
        for (int r = 0; r < R; ++r) std::copy(op[r], op[r] + n, out[r].begin() + (long)pos);
    }
    const size_t w = (size_t)(0.010 * sc.sr);
    std::vector<double> y;
    for (size_t s = (size_t)(0.35 * sc.sr); s + w < std::min(N, (size_t)(2.2 * sc.sr)); s += w) {
        double e = 0;
        for (int r = 0; r < R; ++r) for (size_t i = s; i < s + w; ++i) e += (double)out[r][i] * out[r][i];
        y.push_back(10 * std::log10(e / w / R + 1e-20));
    }
    const size_t m = y.size();
    if (nWin) *nWin = m;
    // second differences cancel any smooth (even curved, e.g. double-slope) decay and leave the window-to-window
    // scatter: for independent deviations of std s the second difference has std s*sqrt(6)
    double s2 = 0;
    for (size_t i = 1; i + 1 < m; ++i) { const double d2 = y[i - 1] - 2 * y[i] + y[i + 1]; s2 += d2 * d2; }
    return std::sqrt(s2 / (double)(m - 2) / 6.0);
}

static Scene hallScene() {
    Scene s;
    s.sr = 48000; s.length = 3.0; s.height = 12.0; s.order = 2;
    s.mats = surfacePresets();
    const int plaster = presetIndex("Plaster");
    s.floorMat = presetIndex("Parquet_floor");
    s.ceilMat = plaster;
    s.walls = {Wall{0, 0, 28, 0, plaster}, Wall{28, 0, 28, 18, plaster}, Wall{28, 18, 0, 18, plaster}, Wall{0, 18, 0, 0, plaster}};
    Zone aud;
    aud.mat = presetIndex("Audience_seated");
    aud.poly = {{10, 2}, {26, 2}, {26, 16}, {10, 16}};
    s.floorZones.push_back(aud);
    Transducer src, r1, r2;
    src.name = "S"; src.pos = {3, 9, 1.6};
    r1.name = "L"; r1.pos = {16, 9, 1.2}; r1.azimuthDeg = 55; r1.pattern = 0.5;
    r2 = r1; r2.name = "R"; r2.azimuthDeg = -55;
    s.sources.push_back(src);
    s.receivers = {r1, r2};
    s.hasListener = true; s.listenerPos = r1.pos;
    return s;
}

static void testTailSmoothness() {
    std::printf("Test 21: the late tail is a smooth noise, also in a big hall\n");
    const Scene hall = hallScene();
    Scene room = hall;  // the same material in a living-room sized box: the reference
    room.height = 2.6;
    room.walls = {Wall{0, 0, 6, 0, presetIndex("Plaster")}, Wall{6, 0, 6, 4.5, presetIndex("Plaster")}, Wall{6, 4.5, 0, 4.5, presetIndex("Plaster")}, Wall{0, 4.5, 0, 0, presetIndex("Plaster")}};
    room.floorZones.clear();
    room.sources[0].pos = {1.5, 2.2, 1.4};
    room.receivers[0].pos = room.receivers[1].pos = {4.2, 2.2, 1.2};
    room.listenerPos = room.receivers[0].pos;
    const double roomScatter = tailRoughnessDb(room, 20000), hallScatter = tailRoughnessDb(hall, 20000), hallMany = tailRoughnessDb(hall, 400000);
    std::printf("  scatter of the 10 ms tail energies: small room %.2f dB, hall %.2f dB (20000 rays), hall %.2f dB (400000 rays)\n", roomScatter, hallScatter, hallMany);
    // ~0.8 dB is what any Gaussian noise tail shows in 10 ms windows (a 125 Hz band has about one independent sample in
    // them): the hall must be at that floor, not above it (it was 7 dB above before the receiver sphere / smoothing fix)
    check(hallScatter < 1.25 * roomScatter + 0.1, "the hall's tail envelope is as smooth as the small room's with the plugin's ray budget");
    check(roomScatter < 1.2, "the small room's tail envelope sits at the noise floor");
}

// Direct / early / tail levels (dB) of the hall's impulse response for a source `dist` metres from the listener.
struct Balance { double direct, early, tail; };
static Balance balanceAt(double dist, double tailDb = 0.0) {
    Scene sc = hallScene();
    sc.sources[0].pos = {20.0 - dist, 9.0, 1.6};
    sc.receivers[0].pos = sc.receivers[1].pos = {20.0, 9.0, 1.2};
    sc.listenerPos = sc.receivers[0].pos;
    EngineConfig cfg;
    cfg.tailThread = false; cfg.erOrder = 1; cfg.tailRays = 20000;
    Engine eng(sc, cfg);
    eng.setTailLevel(tailDb);
    const size_t N = (size_t)(sc.length * sc.sr);
    std::vector<float> in(512), blk0(512), blk1(512);
    std::vector<float> L(N, 0.f);
    for (size_t pos = 0; pos < N; pos += 512) {
        const int n = (int)std::min<size_t>(512, N - pos);
        std::fill(in.begin(), in.end(), 0.f);
        if (pos == 0) in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[2] = {blk0.data(), blk1.data()};
        eng.process(ip, op, n);
        std::copy(blk0.begin(), blk0.begin() + n, L.begin() + (long)pos);
    }
    const double c = speedOfSound(sc.tempC), sr = sc.sr;
    const size_t td = (size_t)(dist / c * sr);
    auto en = [&](double t0, double t1) {
        double e = 0;
        for (size_t i = (size_t)std::max(0.0, t0 * sr); i < std::min(N, (size_t)(t1 * sr)); ++i) e += (double)L[i] * L[i];
        return 10 * std::log10(e + 1e-30);
    };
    const double tD = (double)td / sr;
    return {en(tD - 0.0005, tD + 0.0015), en(tD + 0.002, 0.12), en(0.25, 2.0)};
}

static void testBalance() {
    std::printf("Test 22: voice / early / tail balance with distance (concert hall, listener fixed)\n");
    const double dists[] = {2, 4, 8, 16};
    Balance b[4];
    for (int i = 0; i < 4; ++i) b[i] = balanceAt(dists[i]);
    for (int i = 0; i < 4; ++i)
        std::printf("  source %4.0f m: direct %+6.1f dB, early (to 120 ms) %+6.1f dB, tail (0.25..2 s) %+6.1f dB  (re 2 m: %+5.1f / %+5.1f / %+5.1f)\n", dists[i], b[i].direct, b[i].early, b[i].tail,
                    b[i].direct - b[0].direct, b[i].early - b[0].early, b[i].tail - b[0].tail);
    check(b[3].direct - b[0].direct < -15.0 && b[3].direct - b[0].direct > -21.0, "the direct sound falls by about 6 dB per doubling of the distance");
    // the tail level: -12 dB lowers only the tail
    const Balance q = balanceAt(8, -12.0), ref = b[2];
    std::printf("  tail level -12 dB at 8 m: direct %+.1f dB, early %+.1f dB, tail %+.1f dB (re the default)\n", q.direct - ref.direct, q.early - ref.early, q.tail - ref.tail);
    // (the 120 ms "early" window already contains the first part of the tail, so it moves a little)
    check(std::fabs(q.tail - ref.tail + 12.0) < 0.7 && std::fabs(q.direct - ref.direct) < 0.2 && q.early <= ref.early + 0.1 && q.early >= ref.early - 12.0, "the tail level moves the tail by exactly that amount and leaves the direct sound alone");
    // the reverberant field is nearly the same everywhere in a room: the tail does not follow the source distance
    check(std::fabs(b[2].tail - b[0].tail) < 1.5, "the tail level hardly depends on the source distance (diffuse field), as in a real room");
}

static void testMicPatterns() {
    std::printf("Test 23: listener microphone patterns and the human-ear preset\n");
    // free field, a source 2 m to the listener's LEFT (+y); two microphones at the listener
    auto levels = [&](MicPattern mp, double* lowDiff, double* highDiff) {
        Scene sc = wallScene(nullptr);
        sc.sources[0].pos = {0, 2, 1.5};
        sc.receivers = makeReceiverArray(Layout::StereoORTF, {0, 0, 1.5}, 0.1, 2);
        applyMicPattern(sc.receivers, mp);
        sc.hasListener = true; sc.listenerPos = {0, 0, 1.5};
        EngineConfig cfg;
        cfg.tailThread = false; cfg.tailRays = 2000; cfg.erOrder = 1;
        Engine eng(sc, cfg);
        const size_t N = 8192;
        std::vector<float> in(N, 0.f), o0(N), o1(N);
        in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[2] = {o0.data(), o1.data()};
        eng.process(ip, op, (int)N);
        *lowDiff = 10 * std::log10(bandPowerOf(o0, 48000, 200, 400) / bandPowerOf(o1, 48000, 200, 400));
        *highDiff = 10 * std::log10(bandPowerOf(o0, 48000, 3000, 5000) / bandPowerOf(o1, 48000, 3000, 5000));
    };
    double lo, hi, loOmni, hiOmni, loHuman, hiHuman, loCard, hiCard;
    levels(MicPattern::Omni, &loOmni, &hiOmni);
    levels(MicPattern::Cardioid, &loCard, &hiCard);
    levels(MicPattern::HumanEars, &loHuman, &hiHuman);
    levels(MicPattern::LayoutDefault, &lo, &hi);
    std::printf("  left minus right level for a source on the left: omni %+.1f/%+.1f dB, cardioid pair %+.1f/%+.1f dB, as layout %+.1f/%+.1f dB, human ears %+.1f/%+.1f dB (200-400 Hz / 3-5 kHz)\n",
                loOmni, hiOmni, loCard, hiCard, lo, hi, loHuman, hiHuman);
    check(std::fabs(loOmni) < 1.0 && std::fabs(hiOmni) < 1.0, "omni microphones see a side source equally (no level difference)");
    check(hiHuman > 8.0, "human ears: a source on the left is much louder in the left ear at high frequencies (head shadow)");
    check(loHuman < 4.0 && hiHuman > loHuman + 5.0, "human ears: the level difference is small at low and large at high frequencies");
    check(std::fabs(lo - loCard) < 0.3 && std::fabs(hi - hiCard) < 0.3, "a cardioid pair is the layout default");
    {   // the ears sit `radius` either side of the head centre
        auto rc = makeReceiverArray(Layout::StereoORTF, {0, 0, 1.5}, 0.0875, 2);
        applyMicPattern(rc, MicPattern::HumanEars, 0.0875);
        auto rc2 = makeReceiverArray(Layout::StereoORTF, {0, 0, 1.5}, 0.15, 2);
        applyMicPattern(rc2, MicPattern::HumanEars, 0.15);
        std::printf("  ear spacing: radius 0.0875 -> %.1f cm, radius 0.15 -> %.1f cm\n", 100 * (rc[0].pos.y - rc[1].pos.y), 100 * (rc2[0].pos.y - rc2[1].pos.y));
        check(std::fabs((rc[0].pos.y - rc[1].pos.y) - 0.175) < 1e-9 && std::fabs((rc2[0].pos.y - rc2[1].pos.y) - 0.30) < 1e-9, "the human-ear spacing follows the radius");
    }
}

static void testRoughness(const std::string& dir) {
    std::printf("Test 18: roughness (seeded variation of every surface)\n");
    Scene base = loadScene(dir + "/demo_furnished.txt");
    Scene a = base, a2 = base, b = base, off = base;
    a.roughness = a2.roughness = b.roughness = 0.8;
    a.roughSeed = a2.roughSeed = 3; b.roughSeed = 4;
    const Scene ra = withRoughness(a), ra2 = withRoughness(a2), rb = withRoughness(b), ro = withRoughness(off);
    bool same = ra.mats.size() == ra2.mats.size(), differs = false, valid = true;
    for (size_t i = 0; same && i < ra.mats.size(); ++i)
        for (int k = 0; k < NB; ++k) same &= ra.mats[i].alpha[(size_t)k] == ra2.mats[i].alpha[(size_t)k];
    for (size_t i = 0; i < std::min(ra.mats.size(), rb.mats.size()); ++i) differs |= ra.mats[i].alpha[3] != rb.mats[i].alpha[3];
    for (auto& m : ra.mats)
        for (int k = 0; k < NB; ++k) valid &= m.alpha[(size_t)k] >= 0 && m.alpha[(size_t)k] + m.tau[(size_t)k] <= 1.0 + 1e-9 && m.scatter <= 1 && scatterAt(m, k) <= 1;
    check(ro.mats.size() == base.mats.size() && ro.walls.size() == base.walls.size(), "roughness 0 leaves the scene alone");
    check(same, "the same seed gives the same surfaces");
    check(differs, "another seed gives other surfaces");
    check(valid, "roughened materials stay physical (absorption + transmission <= 1, scattering <= 1)");
    // identical walls no longer answer identically
    double lo = 1e9, hi = -1e9;
    for (size_t i = 0; i < ra.walls.size(); ++i) { const double v = ra.mats[(size_t)ra.walls[i].mat].alpha[3]; lo = std::min(lo, v); hi = std::max(hi, v); }
    std::printf("  absorption at 1 kHz over the walls: %.3f .. %.3f (roughness 0.8)\n", lo, hi);
    check(hi - lo > 0.02, "walls differ from each other");
    std::istringstream is(serializeScene(a));
    const Scene back = parseScene(is, "roughness");
    check(std::fabs(back.roughness - 0.8) < 1e-9 && back.roughSeed == 3, "roughness and seed survive serialise -> parse");
    // the engine renders it
    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 20000;
    Engine eng(a, cfg);
    const size_t nS = a.sources.size(), nR = a.receivers.size();
    std::vector<std::vector<float>> in(nS, std::vector<float>(4096, 0.f)), out(nR, std::vector<float>(4096, 0.f));
    in[0][0] = 1.f;
    std::vector<const float*> ip(nS);
    std::vector<float*> op(nR);
    for (size_t i = 0; i < nS; ++i) ip[i] = in[i].data();
    for (size_t i = 0; i < nR; ++i) op[i] = out[i].data();
    eng.process(ip.data(), op.data(), 4096);
    bool fin = true;
    double en = 0;
    for (auto& o : out) { fin &= finite(o); en += energy(o, 0, o.size()); }
    check(fin && en > 0, "the engine renders a rough room");
}

static void testSourcePattern() {
    std::printf("Test 17: source directivity (live)\n");
    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = 1;
    Scene sc = wallScene(nullptr);
    const Vec3 d = sc.receivers[0].pos - sc.sources[0].pos;
    const double toRcv = std::atan2(d.y, d.x) * 180.0 / kPi;
    auto level = [&](double az, double pattern, bool live) {
        Scene s = sc;
        s.sources[0].azimuthDeg = az;
        s.sources[0].pattern = live ? 0.0 : pattern;
        Engine eng(s, cfg);
        if (live) eng.setSourcePattern(0, pattern);
        std::vector<float> in(8192, 0.f), out(8192);
        in[0] = 1.f;
        for (size_t p = 0; p < in.size(); p += 256) {
            const float* ip[1] = {in.data() + p};
            float* op[1] = {out.data() + p};
            eng.process(ip, op, 256);
        }
        return 10 * std::log10(energy(out, 0, out.size()) + 1e-30);
    };
    const double omniToward = level(toRcv, 0.0, false), omniAway = level(toRcv + 180, 0.0, false);
    const double cardToward = level(toRcv, 0.5, false), cardAway = level(toRcv + 180, 0.5, false);
    const double liveAway = level(toRcv + 180, 0.5, true);
    std::printf("  omni toward/away %.1f/%.1f dB, cardioid toward/away %.1f/%.1f dB, cardioid set live (away) %.1f dB\n", omniToward, omniAway, cardToward, cardAway, liveAway);
    check(std::fabs(omniToward - omniAway) < 0.2, "an omni source does not care where it faces");
    check(cardToward > cardAway + 15, "a cardioid facing away is much quieter than one facing the listener");
    check(std::fabs(liveAway - cardAway) < 0.5, "the live directivity setter matches a scene built with that pattern");
}

static void testWallHeights(const std::string& dir) {
    std::printf("Test 16: wall height: sound passes over low walls and under hanging ones\n");
    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = 1;
    auto energyOf = [&](const Scene& sc) {
        Engine eng(sc, cfg);
        std::vector<float> in(4096, 0.f), out(4096);
        in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[1] = {out.data()};
        eng.process(ip, op, 4096);
        return energy(out, 0, out.size());
    };
    const double ref = energyOf(wallScene(nullptr));
    auto relDb = [&](double e) { return 10 * std::log10(e / ref + 1e-30); };
    auto withWall = [&](double z0, double z1, double srcZ = 1.5, double rcvZ = 1.5) {
        Scene sc = wallScene("Concrete");
        sc.walls[0].z0 = z0;
        sc.walls[0].z1 = z1;
        sc.sources[0].pos.z = srcZ;
        sc.receivers[0].pos.z = rcvZ;
        return sc;
    };
    const double low = relDb(energyOf(withWall(0, 1.0))), tall = relDb(energyOf(withWall(0, 2.0))), hung = relDb(energyOf(withWall(1.8, 3.0)));
    std::printf("  ears at 1.5 m: wall to 1.0 m %+.1f dB, wall to 2.0 m %.0f dB, hanging 1.8-3.0 m %+.1f dB (vs no wall)\n", low, tall, hung);
    check(std::fabs(low) < 0.5 && tall < -80 && std::fabs(hung) < 0.5, "a low wall is passed over, a tall one blocks, a hanging one is passed under");
    // slanted path: source 0.2 m, receiver 2.0 m high, the wall stands half-way: the path crosses it at 1.1 m
    Scene slantRef = wallScene(nullptr);  // same heights, no wall: the path is longer, so the reference must be too
    slantRef.sources[0].pos.z = 0.2;
    slantRef.receivers[0].pos.z = 2.0;
    const double slantRefE = energyOf(slantRef);
    const double over = 10 * std::log10(energyOf(withWall(0, 1.0, 0.2, 2.0)) / slantRefE), blockedSlant = 10 * std::log10(energyOf(withWall(0, 1.2, 0.2, 2.0)) / slantRefE + 1e-30);
    std::printf("  slanted path crossing at 1.1 m: wall top 1.0 m %+.1f dB, wall top 1.2 m %.0f dB\n", over, blockedSlant);
    check(std::fabs(over) < 0.5 && blockedSlant < -80, "the height of the path where it crosses the wall decides");

    // clutter lower than the ears does not touch the direct sound of a high source/listener
    Scene base = wallScene(nullptr);
    base.walls.push_back(Wall{-6, -6, 6, -6, presetIndex("Concrete")});  // an outline so that the clutter has a room
    base.walls.push_back(Wall{6, -6, 6, 6, presetIndex("Concrete")});
    base.walls.push_back(Wall{6, 6, -6, 6, presetIndex("Concrete")});
    base.walls.push_back(Wall{-6, 6, -6, -6, presetIndex("Concrete")});
    base.receivers[0].pos.x = 4;
    Scene lowC = base, tallC = base;
    lowC.clutter.count = 40; lowC.clutter.hMin = 0.4; lowC.clutter.hMax = 1.0; lowC.clutter.tlMin = 25; lowC.clutter.tlMax = 30; lowC.clutter.seed = 5;
    tallC.clutter = lowC.clutter;
    tallC.clutter.hMin = 2.5; tallC.clutter.hMax = 3.5;
    auto direct = [&](const Scene& sc) {  // direct sound level (first 700 samples = before any reflection)
        Engine eng(sc, cfg);
        std::vector<float> in(1024, 0.f), out(1024);
        in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[1] = {out.data()};
        eng.process(ip, op, 1024);
        return energy(out, 0, 700);
    };
    const double d0 = direct(base), dl = direct(lowC);
    // tall objects only shadow the direct path when one of them happens to stand on it: look for such a seed
    int shadowSeed = -1;
    double dt = d0;
    for (int seed = 1; seed <= 80 && shadowSeed < 0; ++seed) {
        tallC.clutter.seed = seed;
        dt = direct(tallC);
        if (dt < d0 * 0.5) shadowSeed = seed;
    }
    lowC.clutter.seed = shadowSeed > 0 ? shadowSeed : 5;  // the very same layout positions, but low objects
    const double dl2 = direct(lowC);
    std::printf("  direct sound with 40 heavy objects (25-30 dB loss): low (0.4-1.0 m) %+.2f dB; tall (2.5-3.5 m) seed %d: %+.1f dB, the same seed with low objects %+.2f dB\n",
                10 * std::log10(dl / d0), shadowSeed, 10 * std::log10(dt / d0 + 1e-30), 10 * std::log10(dl2 / d0));
    const Scene cl = withClutter(lowC, 128);
    bool hOk = true;
    for (size_t i = base.walls.size(); i < cl.walls.size(); ++i) hOk &= cl.walls[i].z0 == 0.0 && cl.walls[i].z1 >= 0.4 - 1e-9 && cl.walls[i].z1 <= 1.0 + 1e-9;
    check(std::fabs(10 * std::log10(dl / d0)) < 0.2 && std::fabs(10 * std::log10(dl2 / d0)) < 0.2 && hOk, "objects below the ears leave the direct sound alone; their heights follow the configured range");
    check(shadowSeed > 0, "tall objects do shadow the direct sound where one stands in the way (as full-height walls always did)");

    Scene rt = loadScene(dir + "/demo_furnished.txt");
    rt.walls.push_back(Wall{1, 1, 3, 1, 0, 0.0, 1.1});
    rt.walls.push_back(Wall{1, 2, 3, 2, 0, 1.8, 2.6});
    rt.clutter.count = 5; rt.clutter.hMin = 0.3; rt.clutter.hMax = 0.9;
    std::istringstream is(serializeScene(rt));
    const Scene back = parseScene(is, "heights");
    const size_t n = back.walls.size();
    check(n == rt.walls.size() && back.walls[n - 1].z0 == 1.8 && back.walls[n - 1].z1 == 2.6 && back.walls[n - 2].z1 == 1.1 && back.walls[0].z1 > 1e8 &&
              std::fabs(back.clutter.hMin - 0.3) < 1e-9 && std::fabs(back.clutter.hMax - 0.9) < 1e-9,
          "wall heights and clutter heights survive serialise -> parse (full-height walls stay full height)");
}

static void testPresetLibrary(const std::string& dir) {
    std::printf("Test 13: surface library and scene round trip (classes, clutter)\n");
    const auto& p = surfacePresets();
    bool energy = true, unique = true, classes = true;
    for (size_t i = 0; i < p.size(); ++i) {
        for (int b = 0; b < NB; ++b) energy &= p[i].alpha[(size_t)b] >= 0 && p[i].tau[(size_t)b] >= 0 && p[i].alpha[(size_t)b] + p[i].tau[(size_t)b] <= 1.0 + 1e-9;
        classes &= p[i].cls >= 0 && p[i].cls < SC_Custom;
        for (size_t j = i + 1; j < p.size(); ++j) unique &= p[i].name != p[j].name;
    }
    std::printf("  %zu presets in %zu classes\n", p.size(), surfaceClasses().size() - 1);
    check(p.size() >= 40 && energy && unique && classes, "40+ presets: unique names, classes set, absorption + transmission <= 1 in every band");
    check(presetIndexByName("Concrete") == 0 && presetIndexByName("Brick") == 1 && presetIndexByName("Plaster") == 2 && presetIndexByName("Open") >= 0,
          "code that refers to the first presets by position / by name still finds them");

    Scene sc = loadScene(dir + "/demo_furnished.txt");
    sc.mats.push_back(presetByName("Glass_window"));
    sc.walls[0].mat = (int)sc.mats.size() - 1;
    sc.clutter.count = 12; sc.clutter.seed = 7; sc.clutter.tlMin = 3; sc.clutter.tlMax = 9;
    sc.tempC = 12; sc.humidity = 35;
    std::istringstream is(serializeScene(sc));
    Scene back = parseScene(is, "roundtrip");
    const Material& g = back.mats[(size_t)back.walls[0].mat];
    check(back.walls.size() == sc.walls.size() && g.cls == SC_Glass && std::fabs(tauToTl(g.tau[0]) - 10) < 0.1 && back.clutter.count == 12 && back.clutter.seed == 7 &&
              std::fabs(back.tempC - 12) < 1e-9 && std::fabs(back.humidity - 35) < 1e-9,
          "class, transmission, clutter and air survive serialise -> parse");
}

static void testAir() {
    std::printf("Test 14: air (humidity, temperature) shapes the sound\n");
    auto farScene = [](double tempC, double rh) {
        Scene sc = wallScene(nullptr);
        sc.receivers[0].pos.x = 60.0;  // 60 m of air
        sc.tempC = tempC; sc.humidity = rh;
        return sc;
    };
    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = 1;
    auto response = [&](const Scene& sc) {
        Engine eng(sc, cfg);
        std::vector<float> in(16384, 0.f), out(16384);
        in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[1] = {out.data()};
        eng.process(ip, op, 16384);
        return out;
    };
    auto peakAt = [](const std::vector<float>& x) {
        size_t k = 0;
        for (size_t i = 0; i < x.size(); ++i) if (std::fabs(x[i]) > std::fabs(x[k])) k = i;
        return k;
    };
    auto dry = response(farScene(20, 10)), humid = response(farScene(20, 80));
    const double hfDry = bandPowerOf(dry, 48000, 6300, 8000), hfHumid = bandPowerOf(humid, 48000, 6300, 8000);
    const double lf = bandPowerOf(dry, 48000, 200, 400) / bandPowerOf(humid, 48000, 200, 400);
    const double hfDiff = 10 * std::log10(hfHumid / hfDry);
    std::printf("  60 m, 20 C: 6-8 kHz at 80 %% humidity vs 10 %%: %+.1f dB (200-400 Hz: %+.2f dB)\n", hfDiff, 10 * std::log10(1 / lf));
    check(std::fabs(hfDiff) > 3.0 && std::fabs(10 * std::log10(lf)) < 0.5, "humidity changes the high-frequency loss over distance, not the lows");
    auto cold = response(farScene(0, 50)), warm = response(farScene(30, 50));
    const double ratio = (double)peakAt(cold) / (double)peakAt(warm), expect = speedOfSound(30) / speedOfSound(0);
    std::printf("  arrival 0 C: sample %zu, 30 C: sample %zu (ratio %.4f, expected %.4f)\n", peakAt(cold), peakAt(warm), ratio, expect);
    check(std::fabs(ratio / expect - 1) < 0.005, "temperature sets the speed of sound (arrival time)");

    // live change through setScene: two impulses 15000 samples apart, humidity switched in between
    Engine eng(farScene(20, 80), cfg);
    std::vector<float> in(32768, 0.f), out(32768);
    in[0] = 1.f;
    in[15000] = 1.f;
    const float* ip[1];
    float* op[1];
    for (int pos = 0; pos < 32768; pos += 256) {
        // gains apply when the sound is output: switch after the first response (~8400) and before the second (~23400)
        if (pos == 12288) eng.setScene(farScene(20, 10));
        ip[0] = in.data() + pos; op[0] = out.data() + pos;
        eng.process(ip, op, 256);
    }
    std::vector<float> w1(out.begin() + 8200, out.begin() + 8200 + 2048), w2(out.begin() + 23200, out.begin() + 23200 + 2048);
    const double live = 10 * std::log10(bandPowerOf(w2, 48000, 6300, 8000) / bandPowerOf(w1, 48000, 6300, 8000));
    std::printf("  humidity switched 80 -> 10 %% while running: 6-8 kHz of the second impulse %+.1f dB (powers %.3e -> %.3e, peaks %.4f / %.4f)\n", live,
                bandPowerOf(w1, 48000, 6300, 8000), bandPowerOf(w2, 48000, 6300, 8000), *std::max_element(w1.begin(), w1.end()), *std::max_element(w2.begin(), w2.end()));
    check(live < -3.0, "humidity changes while the engine runs");
}

static void testClutter(const std::string& dir) {
    std::printf("Test 15: clutter (seeded semi-transparent obstacles)\n");
    Scene base = loadScene(dir + "/demo_furnished.txt");
    base.sources.resize(1);
    base.sources[0].pos = {2, 3, 1.5};
    base.receivers = verb::makeReceiverArray(verb::Layout::StereoORTF, {6, 3, 1.5}, 0.1);
    auto withSeed = [&](int count, int seed) {
        Scene s = base;
        s.clutter.count = count; s.clutter.seed = seed; s.clutter.tlMin = 2; s.clutter.tlMax = 8;
        return s;
    };
    const Scene a = withClutter(withSeed(24, 1), 128), a2 = withClutter(withSeed(24, 1), 128), b = withClutter(withSeed(24, 2), 128);
    bool same = a.walls.size() == a2.walls.size();
    for (size_t i = 0; same && i < a.walls.size(); ++i) same &= a.walls[i].ax == a2.walls[i].ax && a.walls[i].by == a2.walls[i].by;
    bool differs = false;
    for (size_t i = base.walls.size(); i < std::min(a.walls.size(), b.walls.size()); ++i) differs |= a.walls[i].ax != b.walls[i].ax;
    bool kept = true;
    for (size_t i = base.walls.size(); i < a.walls.size(); ++i) {
        const double mx = 0.5 * (a.walls[i].ax + a.walls[i].bx), my = 0.5 * (a.walls[i].ay + a.walls[i].by);
        kept &= mx > -0.01 && mx < 8.01 && my > -0.01 && my < 6.01;
        kept &= std::hypot(mx - 2, my - 3) >= 0.4 && std::hypot(mx - 6, my - 3) >= 0.4;  // clear of the source and the listener
    }
    std::printf("  24 requested -> %zu clutter segments\n", a.walls.size() - base.walls.size());
    check(same && differs && kept && a.walls.size() == base.walls.size() + 24, "same seed gives the same clutter, another seed another, all inside the room and clear of source/listener");
    check(withClutter(withSeed(500, 3), 128).walls.size() == 128, "the engine's wall capacity caps the clutter");

    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = 1;
    auto ir = [&](const Scene& s) {
        Engine eng(s, cfg);
        std::vector<float> in(8192, 0.f), out0(8192), out1(8192);
        in[0] = 1.f;
        const float* ip[1] = {in.data()};
        float* op[2] = {out0.data(), out1.data()};
        eng.process(ip, op, 8192);
        return out0;
    };
    const auto e0 = ir(withSeed(0, 1)), e1 = ir(withSeed(24, 1)), e1b = ir(withSeed(24, 1)), e2 = ir(withSeed(24, 2));
    double d11 = 0;
    for (size_t i = 0; i < e1.size(); ++i) d11 = std::max(d11, (double)std::fabs(e1[i] - e1b[i]));
    const double c01 = corr(e0, e1, 0, 6000), c12 = corr(e1, e2, 0, 6000);
    std::printf("  early response: no clutter vs clutter corr %.3f, seed 1 vs seed 2 corr %.3f, repeat max diff %.1e\n", c01, c12, d11);
    check(d11 == 0.0 && c01 < 0.995 && c12 < 0.995, "clutter changes the early response, reproducibly for a given seed");

    Engine eng(withSeed(500, 5), cfg);  // must run with the capped clutter
    std::vector<float> in(2048, 0.f), o0(2048), o1(2048);
    in[0] = 1.f;
    const float* ip[1] = {in.data()};
    float* op[2] = {o0.data(), o1.data()};
    eng.process(ip, op, 2048);
    check(finite(o0) && finite(o1), "an over-full request still renders");
}

static void testInputModes(const std::string& dir) {
    std::printf("Test 9: dual-mono voice in both inputs: coherent cancellation between two sources, separate vs sum mode\n");
    Scene sc = loadScene(dir + "/demo_furnished.txt");
    EngineConfig cfg;
    cfg.tailThread = false; cfg.tailRays = 0; cfg.erOrder = 1;
    double w0, d0, w1, d1;
    dualMonoDips(sc, cfg, 0, w0, d0);
    dualMonoDips(sc, cfg, 1, w1, d1);
    std::printf("  separate sources: worst dip %.1f dB, mean deviation %.2f dB | sum into source 1: %.1f dB, %.2f dB\n", w0, d0, w1, d1);
    check(w0 < -3.0, "separate sources fed with the same signal show clear coherent dips (the problem)");
    check(w1 > -1.5 && d1 < 1.0, "sum mode removes them (single source)");
}

static void testSwirlDefaults(const std::string& dir) {
    std::printf("Test 8: audible spectral swirl while walking at 1 m/s (1/3-octave), plugin defaults vs strict physical\n");
    Scene sc = loadScene(dir + "/demo_furnished.txt");
    EngineConfig phys, def;
    phys.tailThread = def.tailThread = false;
    phys.tailRays = def.tailRays = 0;
    phys.erOrder = 2;
    def.erOrder = 1;  // plugin default
    const double sp = swirl(sc, phys, 0.0, 0.0, 1.0, true);
    const double sd = swirl(sc, def, -9.0, 0.5, 1.0, true);
    std::printf("  physical (order 2, early 0 dB, diffusion 0): %.2f dB | plugin default (order 1, -9 dB, diffusion 0.5): %.2f dB\n", sp, sd);
    check(sd < sp * 0.6, "plugin defaults cut the audible swirl by more than 40 % versus strict physical settings");
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "scenes";
    if (std::getenv("VERB_SWIRL")) {
        try { exploreSwirl(dir); } catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 1; }
        return 0;
    }
    if (std::getenv("VERB_EXPLORE")) {
        try { exploreComb(dir); } catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 1; }
        return 0;
    }
    try {
        testSourcePattern();
        testTailSmoothness();
        testBalance();
        testMicPatterns();
        testRoughness(dir);
        testDiffraction();
        testBuildingCorner();
        testBuildingCornerPartial();
        testPlates();
        testZones();
        testWallHeights(dir);
        testPresetLibrary(dir);
        testAir();
        testClutter(dir);
        testTransmission();
        testSharedTail(dir);
        testManySources(dir);
        testInputModes(dir);
        testSwirlDefaults(dir);
        testReflectionSpectrum(dir);
        testDiffusion(dir);
        testSpread(dir);
        testRecovery(dir);
        testCompare(dir);
        testDoppler(dir, 128);
        testDoppler(dir, 32);
        testDoppler(dir, 128, 0.0);
        testDoppler(dir, 128, 0.5);
        testDoppler(dir, 128, 2.0);
        testPerf(dir);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf(fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
