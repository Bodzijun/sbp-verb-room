#include "rt_engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <map>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
#if VERB_REQUIRE_AVX2
#include <immintrin.h>
#endif

#include "dsp.hpp"
#include "geometry.hpp"
#include "tail.hpp"

namespace verb {
namespace {

struct cf {
    float re, im;
};

class FFTf {
public:
    explicit FFTf(int n) : n_(n), rev_(n), twr_(n / 2), twi_(n / 2) {
        int lg = 0;
        while ((1 << lg) < n) ++lg;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < lg; ++b)
                if (i & (1 << b)) r |= 1 << (lg - 1 - b);
            rev_[i] = r;
        }
        for (int k = 0; k < n / 2; ++k) {
            double a = -2 * kPi * k / n;
            twr_[k] = (float)std::cos(a);
            twi_[k] = (float)std::sin(a);
        }
       #if VERB_REQUIRE_AVX2
        // Duplicate each scalar twiddle for interleaved real/imag lanes once,
        // rather than rebuilding SIMD vectors in every audio FFT butterfly.
        stageRe_.resize(2 * (n - 1)); stageIm_.resize(2 * (n - 1));
        for (int half = 1; half < n; half <<= 1) {
            const int step = n / (2 * half), offset = 2 * (half - 1);
            for (int k = 0; k < half; ++k) {
                stageRe_[offset + 2*k] = stageRe_[offset + 2*k + 1] = twr_[k*step];
                stageIm_[offset + 2*k] = stageIm_[offset + 2*k + 1] = twi_[k*step];
            }
        }
       #endif
    }
    void run(cf* a, bool inv) const {
       #if VERB_REQUIRE_AVX2
        const auto imaginarySign = _mm256_set1_ps(inv ? -0.f : 0.f);
       #endif
        for (int i = 0; i < n_; ++i)
            if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
        for (int len = 2; len <= n_; len <<= 1) {
            int half = len / 2, step = n_ / len;
            for (int i = 0; i < n_; i += len) {
                int k = 0;
               #if VERB_REQUIRE_AVX2
                for (; k + 4 <= half; k += 4) {
                    const int offset = 2 * (half - 1 + k);
                    const auto wr = _mm256_loadu_ps(stageRe_.data() + offset);
                    const auto wi = _mm256_xor_ps(_mm256_loadu_ps(stageIm_.data() + offset), imaginarySign);
                    const auto u=_mm256_loadu_ps(&a[i+k].re), x=_mm256_loadu_ps(&a[i+k+half].re);
                    const auto swapped=_mm256_permute_ps(x,0xb1); // [im,re] within each complex pair
                    const auto v=_mm256_addsub_ps(_mm256_mul_ps(x,wr),_mm256_mul_ps(swapped,wi));
                    _mm256_storeu_ps(&a[i+k].re,_mm256_add_ps(u,v));
                    _mm256_storeu_ps(&a[i+k+half].re,_mm256_sub_ps(u,v));
                }
               #endif
                for (; k < half; ++k) {
                    float wr = twr_[k * step], wi = inv ? -twi_[k * step] : twi_[k * step];
                    cf u = a[i + k], x = a[i + k + half];
                    float vr = x.re * wr - x.im * wi, vi = x.re * wi + x.im * wr;
                    a[i + k] = {u.re + vr, u.im + vi};
                    a[i + k + half] = {u.re - vr, u.im - vi};
                }
            }
        }
        if (inv) {
            float s = 1.f / n_;
            int i = 0;
           #if VERB_REQUIRE_AVX2
            const auto scale=_mm256_set1_ps(s);
            for (; i+4<=n_; i+=4) _mm256_storeu_ps(&a[i].re,_mm256_mul_ps(_mm256_loadu_ps(&a[i].re),scale));
           #endif
            for (; i < n_; ++i) { a[i].re *= s; a[i].im *= s; }
        }
    }
    int size() const { return n_; }

private:
    int n_;
    std::vector<int> rev_;
    std::vector<float> twr_, twi_;
   #if VERB_REQUIRE_AVX2
    std::vector<float> stageRe_, stageIm_;
   #endif
};

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void lowpass(double fc, double sr, double Q) {
        double w = 2 * kPi * fc / sr, c = std::cos(w), al = std::sin(w) / (2 * Q), a0 = 1 + al;
        b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0;
        a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
    double run(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Zero-latency band split by differences of FIRST-order lowpasses: bands sum exactly to the input.
// First order on purpose: all lowpasses stay within 90 degrees of each other, so weighting the bands with
// different gains (a reflection's material) gives a smooth response. Steeper (Butterworth) splits have
// mismatched phase at the crossovers and produced artificial notches of up to -7 dB on carpet/curtain reflections.
struct BandSplit {
    double a[NB - 1] = {};
    double y[NB - 1] = {};
    void init(double sr) {
        for (int i = 0; i < NB - 1; ++i) a[i] = 1.0 - std::exp(-2.0 * kPi * kBandHz[i] * std::sqrt(2.0) / sr);
    }
    void process(float x, float* frame) {  // frame[8]
        double lp[NB - 1];
        for (int i = 0; i < NB - 1; ++i) { y[i] += a[i] * (x - y[i]); lp[i] = y[i]; }
        frame[0] = (float)lp[0];
        for (int i = 1; i < NB - 1; ++i) frame[i] = (float)(lp[i] - lp[i - 1]);
        frame[NB - 1] = (float)(x - lp[NB - 2]);
        frame[7] = 0;
    }
};

struct Found {
    uint32_t key;
    double delay;  // samples
    float amp[NB];
};

constexpr uint32_t kDiffKey = 0x7FFFFFFFu;  // tap key of the edge-diffracted direct path

struct Tap {
    uint32_t key = 0;
    double d0 = 0, d1 = 0;
    double ref = 0;  // slowly smoothed true delay: the Doppler-free reference
    float g0[NB] = {}, g1[NB] = {};
    bool seen = false;
};

struct Node {
    Vec3 pos;
    int parent;
    int surf;
    int order;
    uint32_t key;
};

struct Slot {
    uint64_t revision = 0, sceneRevision = 0;
    size_t samples = 0;
    int partitions = 0; // immutable while published; worker may resize only an inactive slot
    std::vector<float> re, im;  // [receiver][partition][bin]
};

struct TailConv {
    int B = 0, P = 0, R = 0, bins = 0;
    std::vector<float> fdlRe, fdlIm;  // [partition][bin] ring of input spectra
    int head = 0;
    std::vector<float> prevIn, inAcc;
    Slot slot[3];
    std::atomic<int> pending{-1};
    std::atomic<bool> workerBusy{false};
    std::atomic<int> curAtomic{0};
    std::atomic<uint64_t> appliedRevision{0}, appliedScene{0};
    int cur = 0;
    int prev = -1;
};

// Immutable snapshot of everything geometric. The audio thread reads one World; the editor publishes a new one.
struct World {
    Scene sc;
    Geo geo;
    std::vector<std::array<float, NB>> refl;  // [surface][band] specular reflection amplitude
    uint64_t serial = 0;
    double c = 343.2;                         // speed of sound for this temperature
    std::array<double, NB> airAmp{};          // per-metre amplitude factor of the air (temperature, humidity)
    // `sc` is the scene including its seeded clutter (capped so the total fits the engine)
    World(const Scene& s, int maxWalls, double absorption = 1.) : sc(withClutter(withRoughness(s), maxWalls)), geo(sc) {
        // Scale AFTER geometry generation: automation must not relocate clutter or change roughness.
        absorption = clampTo(absorption,.3,3.,1.);
        if (absorption != 1.) for (auto& m : sc.mats) for (int b=0;b<NB;++b)
            m.alpha[b] = std::clamp(m.alpha[b]*absorption,0.,std::max(0.,1.-m.tau[b]));
        c = speedOfSound(sc.tempC);
        for (int b = 0; b < NB; ++b) airAmp[(size_t)b] = std::pow(10.0, -airAbsDbPerM(kBandHz[b], sc.tempC, sc.humidity) / 20.0);
        refl.resize((size_t)geo.nSurf());
        auto fill = [&](std::array<float, NB>& r, const Material& m) {
            for (int b = 0; b < NB; ++b) r[(size_t)b] = (float)std::sqrt(reflEnergy(m, b) * (1 - scatterAt(m, b)));
        };
        for (int i = 0; i < geo.nSurf(); ++i) {
            if (i == geo.ceilId() && sc.ceilMat < 0) { refl[(size_t)i].fill(0.f); continue; }  // open sky outside the ceiling zones
            fill(refl[(size_t)i], geo.mat(i));
        }
        floorZoneRefl.resize(sc.floorZones.size());
        for (size_t i = 0; i < sc.floorZones.size(); ++i) fill(floorZoneRefl[i], sc.mats[(size_t)sc.floorZones[i].mat]);
        ceilZoneRefl.resize(sc.ceilZones.size());
        for (size_t i = 0; i < sc.ceilZones.size(); ++i) {
            if (sc.ceilZones[i].mat < 0) ceilZoneRefl[i].fill(0.f);
            else fill(ceilZoneRefl[i], sc.mats[(size_t)sc.ceilZones[i].mat]);
        }
    }
    std::vector<std::array<float, NB>> floorZoneRefl, ceilZoneRefl;
    // Specular reflection amplitude of surface `surf` at the hit point (x, y); floor and ceiling zones are looked up.
    const float* reflAt(int surf, double x, double y) const {
        if (surf == geo.floorId()) {
            const int z = zoneAt(sc.floorZones, x, y);
            return z >= 0 ? floorZoneRefl[(size_t)z].data() : refl[(size_t)surf].data();
        }
        if (surf == geo.ceilId()) {
            const int z = zoneAt(sc.ceilZones, x, y);
            return z >= 0 ? ceilZoneRefl[(size_t)z].data() : refl[(size_t)surf].data();
        }
        return refl[(size_t)surf].data();
    }
    World(const World&) = delete;
    World& operator=(const World&) = delete;
};

}  // namespace

static_assert(std::atomic<double>::is_always_lock_free, "RT decay automation requires lock-free double atomics");

// Length probes deliberately use the authored geometry at EVERY scale. This retains
// the legacy unity probe and avoids a discontinuity from expanding clutter only at !=1.
// Cache only completed probes; cancellation never commits a partial estimate.
static double cachedTailSeconds(Scene s, int order, double absorption, const std::function<bool()>& cancelled) {
    if(cancelled && cancelled())throw std::runtime_error("Scene superseded");
    absorption=clampTo(absorption,.3,3.,1.);
    for(auto& m:s.mats)for(int b=0;b<NB;++b)m.alpha[b]=std::clamp(m.alpha[b]*absorption,0.,std::max(0.,1.-m.tau[b]));
    const auto key=std::to_string(order)+serializeScene(s);
    static std::mutex mx;static std::map<std::string,double> cache;
    {std::lock_guard<std::mutex> lock(mx);auto it=cache.find(key);if(it!=cache.end())return it->second;}
    const double length=effectiveTailSeconds(s,order,cancelled);
    {std::lock_guard<std::mutex> lock(mx);if(cache.size()>=64)cache.erase(cache.begin());cache[key]=length;}
    return length;
}

struct Engine::Impl {
    std::mutex irSnapshotMx; // worker/control only; never acquired by process()
    std::atomic<uint64_t> publishedDiffractionMisses{0};
    Scene sc;
    EngineConfig cfg;
    std::atomic<double> currentAbsorption{1.};
    int S, R;
    double sr, c;
    // geometry: audio thread uses `wd`; setScene() publishes through `pendingWorld`; the worker frees old worlds
    std::mutex worldMx;
    std::vector<std::shared_ptr<World>> worlds;
    std::shared_ptr<World> latest;
    std::shared_ptr<const Scene> analysisScene;
    uint64_t serialCounter = 0;
    World* wd = nullptr;
    std::atomic<World*> pendingWorld{nullptr};
    std::atomic<uint64_t> ackSerial{0}, geomSerial{0};
    int maxWalls = 128;
    DiffractionScratch diffractionScratch; // process-only, reserved before audio starts
    const Geo& G() const { return wd->geo; }
    std::array<double, NB> airAmp;                   // per-metre amplitude factor
    std::vector<Transducer> rcvLocal;                 // receivers relative to the listener (position rotated into listener frame)
    Vec3 l0;
    double yaw0 = 0;

    std::unique_ptr<std::atomic<double>[]> params;  // [S*4 sources (x,y,z,az)] + [4 listener]
    // per source
    struct Src {
        std::vector<float> ring;  // frames of 8 floats
        BandSplit split;
        TailConv tail;
        double lastPose[4] = {1e30, 0, 0, 0};    // pose the current tail IR was built for (worker)
        double lastPattern = -1, solvedPattern = -1;  // directivity the tail IR / the early paths were built for
        double lastLst[4] = {1e30, 0, 0, 0};     // listener pose it was built for
        uint64_t lastGeom = 0;                   // geometry serial it was built for
        uint64_t lastQuality = 0;
        double solvedPose[4] = {1e30, 0, 0, 0};  // pose the early paths were solved for (audio thread)
        std::vector<Vec3> hist;                  // source position at each control period (retarded-time delays)
        uint64_t period = 0;                     // index of the current control period
        int quiet = 0;                           // periods since the source/listener last moved
        Transducer t;
    };
    std::vector<std::unique_ptr<Src>> srcs;
    size_t ringMask = 0;
    std::vector<std::vector<Tap>> links;  // [s*R + r]
    std::vector<Found> found;
    std::vector<Node> nodes;
    std::vector<Transducer> rcvAbs;   // R*M virtual pickup points (receiver r, point k at r*M+k)
    std::vector<Transducer> rcvBase;  // R receivers at the current listener pose
    std::vector<Vec3> offs;           // pickup point offsets, M entries
    // shared late field (cfg.sharedTail): one convolver per receiver, fed with per-source/per-band weighted sums
    bool shared = false;
    std::atomic<bool> sharedBusy{false};
    std::vector<std::unique_ptr<TailConv>> sconv;  // [R], each a single-channel convolver
    std::array<std::vector<float>, 2> lateGbuf;    // [r][s][8] target gains, double-buffered by the worker
    std::atomic<int> lateGcur{0};
    std::atomic<uint64_t> lateGserial{0};
    uint64_t lateGseen = 0;
    const float* lateGtarget = nullptr;
    std::vector<float> lateGuse;                   // gains actually applied (eased towards the target)
    std::vector<float> lateY;                      // [R][ctrl] scratch
    std::vector<TailHist> shapedHist;
    std::vector<double> sourceSeconds;
    std::vector<TailHist> srcHist;                 // worker: cached late-field histogram per source
    std::vector<int> histRays;
    std::vector<bool> histSparse;
    mutable std::mutex diagnosticsMx;
    std::shared_ptr<const TailDiagnostics> diagnostics;
    std::atomic<int> soloMask{0};
    std::atomic<float> reverbMix{1.f};
    float componentGain[3] = {1.f, 1.f, 1.f};
    float componentTarget[3] = {1.f, 1.f, 1.f};
    float componentStep[3] = {};
    int componentRamp[3] = {};
    double sLastLst[4] = {1e30, 0, 0, 0};
    uint64_t sLastGeom = 0;
    uint64_t sLastQuality = 0, workerQuality = 0;
    std::atomic<int> requestedRays{20000};
    std::atomic<bool> requestedAdaptive{false};
    std::atomic<uint64_t> qualitySerial{0};
    std::array<std::atomic<double>, 3> requestedDecay{{1., 1., 1.}};
    std::atomic<double> renderedSeconds{0.};
    int M = 1, RM = 0;
    double lastSpread = -1;
    double earlyG = 1.0, lastEarlyDb = 1e30;
    float tailGa = 1.f, tailGb = 1.f;  // tail gain at the start / end of the control period (eased towards the setting)
    double dopK = 1.0, lastDopK = 1.0, dopA = 0.0;  // Doppler strength and per-period smoothing of the reference delay (tau 10 s)
    static constexpr int kVelvetN = 48;     // pulses per decorrelator
    std::vector<int> vPos;                  // [receiver*kVelvetN + k] pulse delays (samples)
    std::vector<float> vSgn, vHist;         // pulse signs; per-receiver history of the early-reflection sum
    size_t vMask = 0;
    std::vector<float> tmpEarly, sbA, sbB;  // sbA/sbB: folded frame streams of one tap
    std::vector<float> componentMix;
    float diffG = 0.f;                    // smoothed diffusion amount 0..1 (0 = bypass)
    double lastListener[4] = {1e30, 0, 0, 0};
    bool firstSolve = true;
    bool listenerMoved = true;

    uint64_t gcount = 0;
    int B = 0;
    std::vector<float> tailRing;  // [receiver][ringSize]
    size_t tailMask = 0;
    std::vector<float> tmpOut, tailTmp, accRe, accIm, accRe2, accIm2;
    std::vector<cf> fftBuf;
    FFTf fft;
    int tapsActive = 0;
    std::atomic<int> refreshCount{0};

    std::atomic<bool> running{true};
    std::thread worker;

    Impl(const Scene& scene, const EngineConfig& c_)
        : sc(scene), cfg(c_), S((int)scene.sources.size()), R((int)scene.receivers.size()), sr(scene.sr),
          c(speedOfSound(scene.tempC)), fft(c_.tailBlock * 2) {
        if (cfg.erOrder < 1 || cfg.erOrder > 4) throw std::runtime_error("erOrder must be 1..4");
        currentAbsorption=cfg.absorptionScale;
        requestedRays = cfg.tailRays; requestedAdaptive = cfg.adaptiveTail;
        for (int g=0;g<3;++g) requestedDecay[g] = cfg.decayMultipliers[g];
        sc.length = cachedTailSeconds(sc, cfg.erOrder, cfg.absorptionScale, cfg.cancelled);  // never shorter than the scene says, long enough that the tail is not cut
        if (cfg.tailBlock % cfg.ctrlInterval) throw std::runtime_error("tailBlock must be a multiple of ctrlInterval");
        maxWalls = cfg.erOrder <= 1 ? 128 : cfg.erOrder == 2 ? 64 : cfg.erOrder == 3 ? 24 : 12;
        diffractionScratch.prepare((size_t)maxWalls);
        if ((int)scene.walls.size() > maxWalls) throw std::runtime_error("too many walls for this reflection order (max " + std::to_string(maxWalls) + ")");
        B = cfg.tailBlock;
        latest = std::make_shared<World>(sc, maxWalls, cfg.absorptionScale);
        analysisScene = std::shared_ptr<const Scene>(latest, &latest->sc);
        latest->serial = ++serialCounter;
        worlds.push_back(latest);
        wd = latest.get();
        ackSerial = geomSerial = latest->serial;
        for (int b = 0; b < NB; ++b) airAmp[b] = std::pow(10.0, -airAbsDbPerM(kBandHz[b], sc.tempC, sc.humidity) / 20.0);
        // listener frame
        if (sc.hasListener) { l0 = sc.listenerPos; yaw0 = sc.listenerYaw; }
        else {
            for (auto& r : sc.receivers) l0 = l0 + r.pos;
            l0 = l0 * (1.0 / R);
        }
        double cy = std::cos(-yaw0 * kPi / 180), sy = std::sin(-yaw0 * kPi / 180);
        for (auto& r : sc.receivers) {
            Transducer t = r;
            Vec3 d = r.pos - l0;
            t.pos = {d.x * cy - d.y * sy, d.x * sy + d.y * cy, d.z};
            t.azimuthDeg = r.azimuthDeg - yaw0;
            rcvLocal.push_back(t);
        }
        M = (cfg.spatialTaps == 2 || cfg.spatialTaps == 4) ? cfg.spatialTaps : 1;
        RM = R * M;
        rcvBase = sc.receivers;
        rcvAbs.resize((size_t)RM);
        for (int r = 0; r < R; ++r)
            for (int k = 0; k < M; ++k) rcvAbs[(size_t)(r * M + k)] = sc.receivers[(size_t)r];
        offs.assign((size_t)M, Vec3{});
        params.reset(new std::atomic<double>[S * 5 + 9]);
        params[S * 5 + 8] = 0.0;  // late-field (tail) level, dB
        params[S * 4 + 7] = 1.0;  // Doppler strength 0..2 (1 = physical)
        for (int s = 0; s < S; ++s) params[S * 4 + 8 + s] = sc.sources[(size_t)s].pattern;  // live source directivity
        params[S * 4 + 4] = cfg.spatialRadius;
        params[S * 4 + 5] = 0.0;  // early level, dB
        params[S * 4 + 6] = 0.0;  // early diffusion 0..1
        // Velvet-noise decorrelators (one random sparse FIR per channel) for the early reflections.
        {
            const int span = (int)std::lround(0.012 * sr);  // 12 ms
            const size_t hist = nextPow2((size_t)span + (size_t)cfg.ctrlInterval + 8);
            vMask = hist - 1;
            vHist.assign((size_t)R * hist, 0.f);
            vPos.assign((size_t)R * kVelvetN, 0);
            vSgn.assign((size_t)R * kVelvetN, 0.f);
            const double cell = (double)span / kVelvetN;
            for (int r = 0; r < R; ++r) {
                std::mt19937 rng(9001u + 7919u * (unsigned)r + (unsigned)sc.seed);  // different per channel
                std::uniform_real_distribution<double> U(0, 1);
                for (int k = 0; k < kVelvetN; ++k) {
                    vPos[(size_t)(r * kVelvetN + k)] = (int)std::floor((k + U(rng)) * cell);
                    vSgn[(size_t)(r * kVelvetN + k)] = U(rng) < 0.5 ? -1.f : 1.f;
                }
            }
        }
        tmpEarly.assign((size_t)R * cfg.ctrlInterval, 0.f);
        componentMix.assign((size_t)3 * cfg.ctrlInterval, 1.f);
        sbA.assign((size_t)cfg.ctrlInterval * 2 + 64, 0.f);
        sbB.assign((size_t)cfg.ctrlInterval * 2 + 64, 0.f);
        for (int s = 0; s < S; ++s) {
            const Transducer& t = sc.sources[s];
            params[s * 4 + 0] = t.pos.x; params[s * 4 + 1] = t.pos.y; params[s * 4 + 2] = t.pos.z; params[s * 4 + 3] = t.azimuthDeg;
        }
        params[S * 4 + 0] = l0.x; params[S * 4 + 1] = l0.y; params[S * 4 + 2] = l0.z; params[S * 4 + 3] = yaw0;

        // delay rings
        size_t frames = nextPow2((size_t)(cfg.maxEarlyDelaySec * sr) + 8192);
        ringMask = frames - 1;
        // tail
        shared = cfg.sharedTail;
        size_t N = (size_t)std::ceil(sc.length * sr);
        if (N <= (size_t)B * 2) throw std::runtime_error("scene length too short for the tail block size");
        const int initialP = (int)((N - 1) / B);
        const int P = (int)std::ceil(20. * sr / B); // retain input history for later longer worker IRs
        auto initConv = [&](TailConv& t, int channels) {
            t.B = B; t.P = P; t.R = channels; t.bins = B + 1;
            t.fdlRe.assign((size_t)P * t.bins, 0.f); t.fdlIm.assign((size_t)P * t.bins, 0.f);
            t.prevIn.assign(B, 0.f); t.inAcc.assign(B, 0.f);
            for (auto& sl : t.slot) { sl.partitions = initialP; sl.re.assign((size_t)channels * initialP * t.bins, 0.f); sl.im.assign((size_t)channels * initialP * t.bins, 0.f); }
        };
        for (int s = 0; s < S; ++s) {
            auto sp = std::make_unique<Src>();
            sp->ring.assign(frames * 8, 0.f);
            sp->split.init(sr);
            sp->t = sc.sources[s];
            if (!shared) initConv(sp->tail, R);
            srcs.push_back(std::move(sp));
        }
        if (shared) {
            for (int r = 0; r < R; ++r) { sconv.push_back(std::make_unique<TailConv>()); initConv(*sconv.back(), 1); }
            lateGbuf[0].assign((size_t)R * S * 8, 0.f);
            lateGbuf[1].assign((size_t)R * S * 8, 0.f);
            lateGuse.assign((size_t)R * S * 8, 0.f);
            lateY.assign((size_t)R * cfg.ctrlInterval, 0.f);
        }
        srcHist.resize((size_t)S); shapedHist.resize((size_t)S); sourceSeconds.assign((size_t)S,0.);
        histRays.assign((size_t)S, 0);
        histSparse.assign((size_t)S, false);
        size_t tr = nextPow2((size_t)B * 4);
        tailMask = tr - 1;
        tailRing.assign((size_t)R * tr, 0.f);
        tmpOut.assign((size_t)R * cfg.ctrlInterval, 0.f);
        tailTmp.assign((size_t)R * B, 0.f);
        accRe.assign(B + 1, 0.f); accIm.assign(B + 1, 0.f); accRe2.assign(B + 1, 0.f); accIm2.assign(B + 1, 0.f);
        fftBuf.assign(2 * B, {0, 0});
        blkA.assign(B, 0.f); blkB.assign(B, 0.f);

        // path storage
        size_t cnt = 1, lvl = 1;
        for (int k = 1; k <= cfg.erOrder; ++k) { lvl *= (size_t)(k == 1 ? maxWalls + 2 + kMaxPlates : maxWalls + 1 + kMaxPlates); cnt += lvl; }
        nodes.reserve(cnt + 8);
        found.reserve(2048);
        links.resize((size_t)S * RM);
        // order 1: at most direct + one reflection per surface, doubled for the fading copies of moving paths
        const size_t tapCap = cfg.erOrder <= 1 ? (size_t)(2 * (maxWalls + 4 + kMaxPlates)) : 1024;
        for (auto& l : links) l.reserve(tapCap);

        // initial late field (synchronous), then the optional refresher
        for (int s = 0; s < S; ++s) {
            for (int k = 0; k < 4; ++k) srcs[s]->lastPose[k] = params[s * 4 + k].load();
            for (int k = 0; k < 4; ++k) srcs[s]->lastLst[k] = params[S * 4 + k].load();
            srcs[s]->lastGeom = geomSerial.load();
            srcs[s]->hist.assign(kHist, srcs[s]->t.pos);
            if (!shared) {
                Transducer st = srcs[s]->t;
                std::vector<Transducer> rc = sc.receivers;
                buildTail(s, latest->sc, st, rc, 0, latest->serial);
                srcs[s]->tail.cur = 0;
                srcs[s]->tail.curAtomic = 0;
            }
        }
        if (shared) sharedRebuild(true);
        if (shared) for (auto& c : sconv) c->appliedScene = c->slot[0].sceneRevision;
        else for (auto& s : srcs) s->tail.appliedScene = s->tail.slot[0].sceneRevision;
        cfg.cancelled = {};
        if (cfg.tailThread) worker = std::thread([this] { if (shared) workerLoopShared(); else workerLoop(); });
    }

    ~Impl() {
        running = false;
        if (worker.joinable()) worker.join();
    }

    // ---- tail IR generation (worker or constructor) ----
    int rayLimit(int base) const { return cfg.adaptiveTail ? (int)std::min<int64_t>(2000000, 4ll * base) : base; }

    void syncQuality() {
        const auto serial = qualitySerial.load(std::memory_order_acquire);
        if (serial == workerQuality) return;
        cfg.tailRays = requestedRays.load(); cfg.adaptiveTail = requestedAdaptive.load();
        for (int g=0;g<3;++g) cfg.decayMultipliers[g] = requestedDecay[g].load();
        workerQuality = serial;
    }

    void publishDiagnostics(const TailHist& h, int used, int limit, bool sparse, const TailHist* result = nullptr, const std::vector<TailHist>* sourceResults = nullptr) {
        auto adjusted = result ? *result : h;
        if (!result) reshapeTailDecay(adjusted, cfg.decayMultipliers);
        auto d = std::make_shared<TailDiagnostics>(analyzeTail(adjusted));
        const auto physical = analyzeTail(h); d->physicalRT60 = physical.rt60;
        if(sourceResults) {
            for(const auto& source:*sourceResults) {
                d->sourceEffectiveMultipliers.push_back(source.effectiveMultipliers);
                d->sourceReshapeStatus.push_back(source.reshapeStatus);
                for(int b=0;b<NB;++b)d->reshapeStatus[b]=std::max(d->reshapeStatus[b],source.reshapeStatus[b]);
            }
            for(int b=0;b<NB;++b) {
                bool mixed=false;
                for(const auto& source:*sourceResults)mixed|=source.reshapeStatus[b]!=sourceResults->front().reshapeStatus[b]
                    ||std::abs(source.effectiveMultipliers[b]-sourceResults->front().effectiveMultipliers[b])>1e-9;
                if(mixed)d->effectiveMultipliers[b]=physical.rt60[b]>0&&d->rt60[b]>0?d->rt60[b]/physical.rt60[b]:1.;
            }
        }
        auto padded = h; padded.nBins = adjusted.nBins;
        for (auto& r : padded.h) for (auto& b : r) b.resize(padded.nBins, 0.);
        d->physicalDecayDb = analyzeTail(padded).decayDb;
        d->multipliers = cfg.decayMultipliers;
        d->raysUsed = used; d->rayLimit = limit; d->shared = shared;
        d->baseRays = cfg.tailRays;
        d->adaptive = cfg.adaptiveTail; d->needsMoreRays = sparse;
        std::lock_guard<std::mutex> lock(diagnosticsMx);
        diagnostics = std::move(d);
    }

    void publishSourceDiagnostics() {
        for (int s = 0; s < S; ++s) if (srcHist[s].h.empty() || srcs[s]->lastQuality != workerQuality) return;
        auto average = [&](const std::vector<TailHist>& cache) {
            TailHist avg; avg.binDur = cache[0].binDur;
            for (const auto& h : cache) avg.nBins = std::max(avg.nBins,h.nBins);
            avg.h.resize(R); avg.reliableEnd.resize(R);
            avg.effectiveMultipliers=cache[0].effectiveMultipliers;avg.reshapeStatus=cache[0].reshapeStatus;
            for (int r=0;r<R;++r) {
                avg.reliableEnd[r].fill(avg.nBins);
                for (int b=0;b<NB;++b) {
                    avg.h[r][b].assign(avg.nBins,0.);
                    for (const auto& h : cache) {
                        double sum=0;
                        for(size_t k=0;k<h.nBins;++k){avg.h[r][b][k]+=h.h[r][b][k]/S;sum+=h.h[r][b][k];}
                        if(sum>0) avg.reliableEnd[r][b]=std::min(avg.reliableEnd[r][b],h.reliableEnd[r][b]);
                    }
                }
            }
            return avg;
        };
        const auto avg = average(srcHist), shaped = average(shapedHist);
        int used = 0; bool sparse = false;
        for (int s = 0; s < S; ++s) { used += histRays[s]; sparse |= histSparse[s]; }
        publishDiagnostics(avg, used, S * rayLimit(cfg.tailRays), sparse, &shaped, &shapedHist);
    }

    void buildTail(int s, const Scene& scn, const Transducer& st, const std::vector<Transducer>& rc, int slotIdx, uint64_t sceneRevision) {
        TailConv& t = srcs[s]->tail;
        bool sparse = false;
        const auto cancelled = [&]{return !running || geomSerial.load()!=sceneRevision || qualitySerial.load()!=workerQuality || (cfg.cancelled && cfg.cancelled());};
        TailHist h = traceTailAdaptive(scn, st, rc, cfg.erOrder, cfg.tailRays, rayLimit(cfg.tailRays), scn.seed, histRays[s], &sparse, cancelled);
        histSparse[s] = sparse;
        srcHist[s] = h;
        srcs[s]->lastQuality = workerQuality;
        reshapeTailDecay(h, cfg.decayMultipliers);
        shapedHist[s] = h;
        const size_t N = cfg.decayMultipliers == std::array<double,3>{{1,1,1}} ? (size_t)std::ceil(scn.length * sr) : (size_t)std::ceil(h.nBins*h.binDur*sr);
        sourceSeconds[s] = N/sr;
        renderedSeconds.store(*std::max_element(sourceSeconds.begin(),sourceSeconds.end()));
        BandBuf buf(R);
        for (auto& r : buf)
            for (auto& b : r) b.assign(N, 0.0);
        addTailNoise(scn, h, scn.seed, buf);
        auto ir = combineBands(buf, sr);
        fadeTailEnd(ir, N, sr);
        FFTf wf(2 * B);
        std::vector<cf> tmp(2 * B);
        Slot& sl = t.slot[slotIdx];
        std::lock_guard<std::mutex> snapshotLock(irSnapshotMx);
        sl.samples=N;
        sl.revision = workerQuality; sl.sceneRevision = sceneRevision;
        sl.partitions = std::min(t.P, (int)((N-1)/B));
        sl.re.resize((size_t)R*sl.partitions*t.bins); sl.im.resize(sl.re.size());
        for (int r = 0; r < R; ++r)
            for (int k = 0; k < sl.partitions; ++k) {
                for (int i = 0; i < 2 * B; ++i) tmp[i] = {0, 0};
                for (int i = 0; i < B; ++i) {
                    size_t idx = (size_t)(k + 1) * B + i;  // drop the first B samples: latency compensation
                    if (idx < N) tmp[i].re = ir[r][idx];
                }
                wf.run(tmp.data(), false);
                size_t o = ((size_t)r * sl.partitions + k) * t.bins;
                for (int b = 0; b < t.bins; ++b) { sl.re[o + b] = tmp[b].re; sl.im[o + b] = tmp[b].im; }
            }
        publishSourceDiagnostics();
        ++refreshCount;
    }

    // ---- shared late field (worker, or the constructor with initial = true) ----
    // Re-traces the sources that moved, rebuilds the common per-receiver tail IRs from the average energy
    // envelope and publishes the per-source/per-receiver/per-band gains. Returns false if nothing changed.
    bool sharedRebuild(bool initial) {
        std::shared_ptr<World> wsnap;
        {
            std::lock_guard<std::mutex> lk(worldMx);
            wsnap = latest;
        }
        const Scene& scn = wsnap->sc;
        double lst[4];
        for (int i = 0; i < 4; ++i) lst[i] = params[S * 4 + i].load();
        std::vector<Transducer> rc;
        listenerReceivers(lst, rc);
        const double dl = std::hypot(std::hypot(lst[0] - sLastLst[0], lst[1] - sLastLst[1]), lst[2] - sLastLst[2]);
        const bool all = initial || wsnap->serial != sLastGeom || workerQuality != sLastQuality || dl > 0.05 || std::fabs(lst[3] - sLastLst[3]) > 2;
        const int rays = std::max(1500, cfg.tailRays / std::max(1, S));
        std::vector<int> dirty;
        std::vector<std::array<double, 4>> poses((size_t)S);
        for (int s = 0; s < S; ++s) {
            Src& sp = *srcs[(size_t)s];
            for (int i = 0; i < 4; ++i) poses[(size_t)s][(size_t)i] = params[s * 4 + i].load();
            const auto& p = poses[(size_t)s];
            const double dp = std::hypot(std::hypot(p[0] - sp.lastPose[0], p[1] - sp.lastPose[1]), p[2] - sp.lastPose[2]);
            if (all || dp > 0.05 || std::fabs(p[3] - sp.lastPose[3]) > 2 || params[S * 4 + 8 + s].load() != sp.lastPattern) dirty.push_back(s);
        }
        if (dirty.empty()) return false;
        sharedBusy.store(true,std::memory_order_release);
        // Retain raw moments in the cache; assess confidence only after pooling.
        auto traceSource = [&](int s, int count) {
            Transducer st = srcs[(size_t)s]->t;
            st.pos = {poses[(size_t)s][0], poses[(size_t)s][1], poses[(size_t)s][2]};
            st.azimuthDeg = poses[(size_t)s][3];
            st.pattern = std::min(std::max(params[S * 4 + 8 + s].load(), 0.0), 1.0);
            srcHist[(size_t)s] = traceTail(scn, st, rc, cfg.erOrder, count, scn.seed + 131 * s, false, [&]{return !running || geomSerial.load()!=wsnap->serial || qualitySerial.load()!=workerQuality || (cfg.cancelled && cfg.cancelled());});
            histRays[s] = count;
            srcs[(size_t)s]->lastPattern = st.pattern;
            for (int i = 0; i < 4; ++i) srcs[(size_t)s]->lastPose[i] = poses[(size_t)s][(size_t)i];
        };
        for (int s : dirty) traceSource(s, rays);
        const size_t nBins = srcHist[0].nBins;
        TailHist avg;
        std::vector<double> E((size_t)S * R * NB, 0.0), Eavg((size_t)R * NB, 0.0);
        bool sparse = false;
        int targetRays = rays;
        for (;;) {
            avg = TailHist{};
            avg.nBins = nBins; avg.binDur = srcHist[0].binDur;
            avg.h.resize(R); avg.h2.resize(R);
            for (int r = 0; r < R; ++r)
                for (int b = 0; b < NB; ++b) {
                    auto& dst = avg.h[r][b]; auto& dst2 = avg.h2[r][b];
                    dst.assign(nBins, 0.0); dst2.assign(nBins, 0.0);
                    for (int s = 0; s < S; ++s) {
                        const auto& h = srcHist[s].h[r][b]; const auto& h2 = srcHist[s].h2[r][b];
                        double e = 0;
                        for (size_t k = 0; k < nBins; ++k) {
                            dst[k] += h[k] / S; dst2[k] += h2[k] / ((double)S * S); e += h[k];
                        }
                        E[((size_t)s * R + r) * NB + b] = e;
                    }
                }
            TailHist raw = avg;
            finalizeTail(avg);
            sparse = tailNeedsMoreRays(raw, avg);
            if (!sparse || targetRays >= rayLimit(rays)) break;
            targetRays = std::min(rayLimit(rays), targetRays * 2);
            for (int s = 0; s < S; ++s) if (histRays[s] < targetRays) traceSource(s, targetRays);
        }
        int usedRays = 0;
        for (int count : histRays) usedRays += count;
        publishDiagnostics(avg, usedRays, S * rayLimit(rays), sparse);
        // Normalize against the finalized IR, preserving each source's measured
        // band energy even when extrapolation changes the common envelope area.
        for (int r = 0; r < R; ++r)
            for (int b = 0; b < NB; ++b) {
                double e = 0;
                for (double v : avg.h[(size_t)r][(size_t)b]) e += v;
                Eavg[(size_t)r * NB + b] = e;
            }
        const int inactive = 1 - lateGcur.load();
        std::lock_guard<std::mutex> snapshotLock(irSnapshotMx);
        std::vector<float>& G = lateGbuf[(size_t)inactive];
        for (int r = 0; r < R; ++r)
            for (int s = 0; s < S; ++s)
                for (int b = 0; b < 8; ++b) {
                    const double ea = b < NB ? Eavg[(size_t)r * NB + b] : 0.0;
                    G[((size_t)r * S + s) * 8 + b] = (b < NB && ea > 0) ? (float)std::sqrt(E[((size_t)s * R + r) * NB + b] / ea) : 0.f;
                }
        // Apply AFTER computing the physical source/average gain ratio. Renormalizing
        // against a boosted tail would cancel its onset-preserving decay change.
        reshapeTailDecay(avg, cfg.decayMultipliers);
        const size_t N = cfg.decayMultipliers == std::array<double,3>{{1,1,1}} ? (size_t)std::ceil(scn.length * sr) : (size_t)std::ceil(avg.nBins*avg.binDur*sr);
        renderedSeconds.store(N/sr);
        // common IRs
        BandBuf buf((size_t)R);
        for (auto& r : buf)
            for (auto& b : r) b.assign(N, 0.0);
        addTailNoise(scn, avg, scn.seed, buf);
        auto ir = combineBands(buf, sr);
        fadeTailEnd(ir, N, sr);
        FFTf wf(2 * B);
        std::vector<cf> tmp(2 * B);
        std::vector<int> wslot((size_t)R);
        for (int r = 0; r < R; ++r) {
            TailConv& t = *sconv[(size_t)r];
            const int w = initial ? 0 : (t.curAtomic.load() == 0 ? 1 : 0);
            wslot[(size_t)r] = w;
            Slot& sl = t.slot[w];
            sl.samples=N;
            sl.revision = workerQuality; sl.sceneRevision = wsnap->serial;
            sl.partitions = std::min(t.P, (int)((N-1)/B));
            sl.re.resize((size_t)sl.partitions*t.bins); sl.im.resize(sl.re.size());
            for (int k = 0; k < sl.partitions; ++k) {
                for (int i = 0; i < 2 * B; ++i) tmp[(size_t)i] = {0, 0};
                for (int i = 0; i < B; ++i) {
                    const size_t idx = (size_t)(k + 1) * B + i;  // drop the first B samples: latency compensation
                    if (idx < N) tmp[(size_t)i].re = ir[(size_t)r][idx];
                }
                wf.run(tmp.data(), false);
                const size_t o = (size_t)k * t.bins;
                for (int b = 0; b < t.bins; ++b) { sl.re[o + (size_t)b] = tmp[(size_t)b].re; sl.im[o + (size_t)b] = tmp[(size_t)b].im; }
            }
        }
        lateGcur.store(inactive);
        ++lateGserial;
        if (initial) {
            lateGuse = G;  // nothing is playing yet: no easing
            lateGtarget = lateGbuf[(size_t)inactive].data();
            lateGseen = lateGserial.load();
        } else {
            for (int r = 0; r < R; ++r) sconv[(size_t)r]->pending.store(wslot[(size_t)r]);
        }
        for (int i = 0; i < 4; ++i) sLastLst[i] = lst[i];
        sLastGeom = wsnap->serial;
        sLastQuality = workerQuality;
        sharedBusy.store(false,std::memory_order_release);
        ++refreshCount;
        return true;
    }

    void workerLoopShared() {
        auto last = std::chrono::steady_clock::now();
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            syncQuality();
            {
                std::lock_guard<std::mutex> lk(worldMx);
                const uint64_t ack = ackSerial.load();
                worlds.erase(std::remove_if(worlds.begin(), worlds.end(), [&](const std::shared_ptr<World>& w) { return w->serial < ack && w != latest; }), worlds.end());
            }
            bool busy = false;
            for (auto& c : sconv) busy |= c->pending.load() >= 0;
            if (busy) continue;
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() < cfg.tailRefreshMs) continue;
            try {if (sharedRebuild(false)) last = std::chrono::steady_clock::now();}
            catch(const std::runtime_error&) {sharedBusy=false;sLastGeom=0;for(auto& sp:srcs)sp->lastGeom=0;}
        }
    }

    void workerLoop() {
        std::vector<std::chrono::steady_clock::time_point> last(S, std::chrono::steady_clock::now());
        int rr = 0;
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            syncQuality();
            {   // free geometry snapshots the audio thread has moved past
                std::lock_guard<std::mutex> lk(worldMx);
                const uint64_t ack = ackSerial.load();
                worlds.erase(std::remove_if(worlds.begin(), worlds.end(), [&](const std::shared_ptr<World>& w) { return w->serial < ack && w != latest; }), worlds.end());
            }
            for (int k = 0; k < S && running; ++k) {
                int s = (rr + k) % S;
                Src& sp = *srcs[s];
                if (sp.tail.pending.load() >= 0) continue;
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last[s]).count() < cfg.tailRefreshMs) continue;
                double pose[4], lst[4];
                for (int i = 0; i < 4; ++i) { pose[i] = params[s * 4 + i].load(); lst[i] = params[S * 4 + i].load(); }
                double dp = std::hypot(std::hypot(pose[0] - sp.lastPose[0], pose[1] - sp.lastPose[1]), pose[2] - sp.lastPose[2]);
                double dl = std::hypot(std::hypot(lst[0] - sp.lastLst[0], lst[1] - sp.lastLst[1]), lst[2] - sp.lastLst[2]);
                const uint64_t gser = geomSerial.load();
                bool moved = dp > 0.05 || dl > 0.05 || std::fabs(pose[3] - sp.lastPose[3]) > 2 || std::fabs(lst[3] - sp.lastLst[3]) > 2 || gser != sp.lastGeom;
                const double pat = std::min(std::max(params[S * 4 + 8 + s].load(), 0.0), 1.0);
                moved |= pat != sp.lastPattern || sp.lastQuality != workerQuality;
                if (!moved) continue;
                sp.lastPattern = pat;
                Transducer st = sp.t;
                st.pattern = pat;
                st.pos = {pose[0], pose[1], pose[2]};
                st.azimuthDeg = pose[3];
                std::vector<Transducer> rc;
                listenerReceivers(lst, rc);
                int cur = sp.tail.curAtomic.load();
                int w = cur == 0 ? 1 : 0;
                std::shared_ptr<World> wsnap;
                {
                    std::lock_guard<std::mutex> lk(worldMx);
                    wsnap = latest;
                }
                sp.tail.workerBusy.store(true,std::memory_order_release);
                try {buildTail(s, wsnap->sc, st, rc, w, wsnap->serial);}
                catch(const std::runtime_error&) {sp.tail.workerBusy=false;sp.lastGeom=0;continue;}
                for (int i = 0; i < 4; ++i) sp.lastPose[i] = pose[i];
                for (int i = 0; i < 4; ++i) sp.lastLst[i] = lst[i];
                sp.lastGeom = wsnap->serial;
                last[s] = std::chrono::steady_clock::now();
                sp.tail.pending.store(w);
                sp.tail.workerBusy.store(false,std::memory_order_release);
                rr = (s + 1) % S;
                break;
            }
        }
    }
    // Publishes new walls/materials/floor/ceiling/height (sources, receivers and length stay). Any thread.
    bool setScene(const Scene& ns, double absorption, const std::function<bool()>& cancelled) {
        if ((int)ns.walls.size() > maxWalls || ns.floorMat < 0 || ns.floorMat >= (int)ns.mats.size() || ns.ceilMat >= (int)ns.mats.size()) return false;
        for (auto& w : ns.walls)
            if (w.mat < 0 || w.mat >= (int)ns.mats.size()) return false;
        for (auto& z : ns.floorZones) if (z.mat < 0 || z.mat >= (int)ns.mats.size() || z.poly.size() < 3) return false;
        for (auto& z : ns.ceilZones) if (z.mat >= (int)ns.mats.size() || z.poly.size() < 3) return false;
        {
            int plates = 0;
            for (auto* zl : {&ns.floorZones, &ns.ceilZones}) for (auto& z : *zl) plates += z.z >= 0 && z.mat >= 0;
            if (plates > kMaxPlates) return false;
        }
        Scene n = sc;
        n.walls = ns.walls; n.mats = ns.mats; n.floorMat = ns.floorMat; n.ceilMat = ns.ceilMat; n.height = ns.height;
        n.tempC = ns.tempC; n.humidity = ns.humidity; n.clutter = ns.clutter;
        n.floorZones = ns.floorZones; n.ceilZones = ns.ceilZones;
        n.roughness = ns.roughness; n.roughSeed = ns.roughSeed; n.diffraction = ns.diffraction;
        n.length = ns.length;
        n.length = cachedTailSeconds(n, cfg.erOrder, absorption, cancelled);
        auto w = std::make_shared<World>(n, maxWalls, absorption);
        {
            std::lock_guard<std::mutex> lk(worldMx);
            w->serial = ++serialCounter;
            worlds.push_back(w);
            latest = w;
            currentAbsorption = absorption;
            std::atomic_store(&analysisScene, std::shared_ptr<const Scene>(w, &w->sc));
        }
        pendingWorld.store(w.get(), std::memory_order_release);
        geomSerial.store(w->serial);
        return true;
    }

    void listenerReceivers(const double* lst, std::vector<Transducer>& out) const {
        double cy = std::cos(lst[3] * kPi / 180), sy = std::sin(lst[3] * kPi / 180);
        out = rcvLocal;
        for (int r = 0; r < R; ++r) {
            const Vec3& d = rcvLocal[r].pos;
            out[r].pos = {lst[0] + d.x * cy - d.y * sy, lst[1] + d.x * sy + d.y * cy, lst[2] + d.z};
            out[r].azimuthDeg = rcvLocal[r].azimuthDeg + lst[3];
        }
    }

    // ---- early-field path solver ----
    static constexpr size_t kHist = 2048;  // control periods of source-position history

    // Source position `u` control periods ago (linear between periods).
    Vec3 histPos(const Src& sp, double u) const {
        u = std::min(std::max(u, 0.0), (double)(kHist - 3));
        uint64_t j = (uint64_t)u;
        double fr = u - (double)j;
        Vec3 a = sp.hist[(sp.period - j) & (kHist - 1)], b = sp.hist[(sp.period - j - 1) & (kHist - 1)];
        return a + (b - a) * fr;
    }

    // Image (through the reflection chain of node idx) of an arbitrary source position.
    Vec3 imageOf(int idx, Vec3 p) const {
        int surfs[8], k = 0;
        for (int n = idx; nodes[n].order >= 1; n = nodes[n].parent) surfs[k++] = nodes[n].surf;
        for (int j = k - 1; j >= 0; --j) p = mirror(G(), p, surfs[j]);
        return p;
    }

    // Vector from the image source to the receiver, with the image taken where the source *was* when the
    // sound that arrives now was emitted (retarded time, solved from the position history; no velocity needed).
    Vec3 retardedD(const Src& sp, int idx, Vec3 R) const {
        Vec3 D = R - nodes[idx].pos;
        for (int it = 0; it < 3; ++it) {
            double u = norm(D) / c * sr / cfg.ctrlInterval;
            D = R - imageOf(idx, histPos(sp, u));
        }
        return D;
    }

    uint32_t buildImages(Vec3 srcPos) {
        nodes.clear();
        nodes.push_back({srcPos, -1, -1, 0, 0});
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].order >= cfg.erOrder) continue;
            for (int s = 0; s < G().nSurf(); ++s) {
                if (s == nodes[i].surf) continue;
                if (s == G().ceilId() && !G().hasCeil) continue;
                if (nodes.size() == nodes.capacity()) return 0;
                Node n;
                n.pos = mirror(G(), nodes[i].pos, s);
                n.parent = (int)i; n.surf = s; n.order = nodes[i].order + 1;
                n.key = nodes[i].key | ((uint32_t)(s + 1) << (8 * (n.order - 1)));
                nodes.push_back(n);
            }
        }
        return 1;
    }

    bool validate(int idx, const Src& sp, const Transducer& rc, const Transducer& src, Found& f) const {
        Vec3 cur = rc.pos, first{}, last{};
        int prevSurf = -1, n = idx;
        float amp[NB];
        for (int b = 0; b < NB; ++b) amp[b] = 1.f;
        while (nodes[n].order >= 1) {
            const Node& nd = nodes[n];
            Vec3 P;
            if (!crossSurfaceSoft(G(), cur, nd.pos, nd.surf, P, c, amp)) return false;
            if (!transmitLeg(G(), cur, P, nd.surf, prevSurf, amp)) return false;
            if (n == idx) first = P;
            last = P;
            cur = P;
            prevSurf = nd.surf;
            const float* rf = wd->reflAt(nd.surf, P.x, P.y);
            for (int b = 0; b < NB; ++b) amp[b] *= rf[b];
            n = nd.parent;
        }
        if (!transmitLeg(G(), cur, src.pos, prevSurf, -1, amp)) return false;
        double rg[NB];
        dirGainBands(rc, unit(first - rc.pos), rg);  // the receiver's pattern may depend on frequency
        for (int b = 0; b < NB; ++b) amp[b] *= (float)rg[b];
        double gain = dirGain(src, unit(last - src.pos));
        finish(f, nodes[idx].key, retardedD(sp, idx, rc.pos), gain, amp);
        return true;
    }

    void finish(Found& f, uint32_t key, Vec3 D, double gain, const float* amp) const {
        double len = norm(D);
        f.key = key;
        f.delay = std::max(2.0, len / c * sr);
        double spread = (key == 0 || key == kDiffKey ? gain : gain * earlyG) / std::max(len, 0.05);
        for (int b = 0; b < NB; ++b) f.amp[b] = (float)(amp[b] * spread * std::pow(airAmp[b], len));
    }

    void solveLink(int s, int r, const Transducer& src) {
        found.clear();
        const Src& sp = *srcs[s];
        const Transducer& rc = rcvAbs[r];
        float damp[NB];
        for (int b = 0; b < NB; ++b) damp[b] = 1.f;
        const bool diffr = wd->sc.diffraction;
        // The sound that bends round a wall end (one or more): added as its own path whenever the straight one is blocked,
        // or only goes through walls that take a good part of it (doors, glass, body panels).
        auto addDiffracted = [&](bool needClear) {
            DiffPath dp;
            if (!diffractedPath(G(), src.pos, rc.pos, c, dp, needClear, &diffractionScratch)) return;
            double L = dp.length;
            for (int it = 0; it < 3; ++it) {  // retarded time, as for the direct path
                const double u = L / c * sr / cfg.ctrlInterval;
                L = norm(histPos(sp, u) - dp.first) + dp.inner + norm(rc.pos - dp.last);
            }
            const double gain = dirGain(src, unit(dp.first - src.pos));
            double rg[NB];
            dirGainBands(rc, unit(dp.last - rc.pos), rg);
            for (int b = 0; b < NB; ++b) dp.amp[b] *= (float)rg[b];
            Found f;
            finish(f, kDiffKey, Vec3{L, 0, 0}, gain, dp.amp);
            found.push_back(f);
        };
        if (transmitLeg(G(), src.pos, rc.pos, -1, -1, damp)) {  // through open/partial walls, attenuated
            const bool screened = damp[3] < 0.5f;  // the straight path loses 6 dB or more on the way (mid band)
            if (diffr) litEdgeAttenuation(G(), src.pos, rc.pos, c, damp);  // a wall end right beside the path trims it
            Vec3 d = unit(rc.pos - src.pos);
            double rg[NB];
            dirGainBands(rc, d * -1.0, rg);
            for (int b = 0; b < NB; ++b) damp[b] *= (float)rg[b];
            const float* amp = damp;
            Found f;
            finish(f, 0, retardedD(sp, 0, rc.pos), dirGain(src, d), amp);
            found.push_back(f);
            if (diffr && screened) addDiffracted(true);
        } else if (diffr) {  // blocked: the sound bends round the nearest wall end
            addDiffracted(false);
        }
        for (size_t i = 1; i < nodes.size(); ++i) {
            Found f;
            if (found.size() < found.capacity() && validate((int)i, sp, rc, src, f)) found.push_back(f);
        }
        const double maxDelay = (double)ringMask - 16;
        auto& taps = links[(size_t)s * RM + r];
        for (auto& t : taps) { t.seen = false; t.d0 = t.d1; std::memcpy(t.g0, t.g1, sizeof t.g0); }
        for (const Found& fc : found) {
            Found f = fc;
            if (f.delay >= maxDelay) continue;
            Tap* tp = nullptr;
            for (auto& t : taps)
                if (t.key == f.key && !t.seen) { tp = &t; break; }
            const double tru = f.delay;
            if (tp) {
                // Doppler strength: blend the true delay with its slow average (k=1: physical, 0: pitch-free)
                if (firstSolve) tp->ref = tru;
                else tp->ref += dopA * (tru - tp->ref);
                if (dopK != 1.0) f.delay = std::max(2.0, tp->ref + dopK * (tru - tp->ref));
                if (!firstSolve && std::fabs(f.delay - tp->d0) > 0.5 * cfg.ctrlInterval && taps.size() < taps.capacity()) {
                    Tap dying = *tp;  // teleport: let the old one fade out in place, start a new one
                    dying.key = 0xFFFFFFFFu; dying.seen = true; dying.d1 = dying.d0;
                    for (int b = 0; b < NB; ++b) dying.g1[b] = 0.f;
                    for (int b = 0; b < NB; ++b) { tp->g0[b] = 0.f; }
                    tp->d0 = f.delay;
                    taps.push_back(dying);
                    tp = &taps[0];  // pointer invalid after push_back; re-find below
                    for (auto& t : taps) if (t.key == f.key && !t.seen) { tp = &t; break; }
                    tp->ref = tru;
                }
                tp->seen = true;
                tp->d1 = f.delay;
                std::memcpy(tp->g1, f.amp, sizeof tp->g1);
                if (firstSolve) { tp->d0 = tp->d1; std::memcpy(tp->g0, tp->g1, sizeof tp->g0); }
            } else if (taps.size() < taps.capacity()) {
                Tap t;
                t.key = f.key; t.seen = true; t.d0 = t.d1 = f.delay; t.ref = tru;
                std::memcpy(t.g1, f.amp, sizeof t.g1);
                if (firstSolve) std::memcpy(t.g0, t.g1, sizeof t.g0);
                taps.push_back(t);
            }
        }
        for (auto& t : taps)
            if (!t.seen) for (int b = 0; b < NB; ++b) t.g1[b] = 0.f;
    }

    void startPeriod() {
        const int mask = soloMask.load(std::memory_order_relaxed);
        for (int k = 0; k < 3; ++k) {
            const float target = (mask == 0 || (mask & (1 << k)))
                ? (k == 0 ? 1.f : reverbMix.load(std::memory_order_relaxed)) : 0.f;
            if (target != componentTarget[k]) {
                componentTarget[k] = target;
                componentRamp[k] = std::max(1, (int)std::lround(0.01 * sr));
                componentStep[k] = (target - componentGain[k]) / componentRamp[k];
            }
        }
        bool worldChanged = false;
        if (World* pw = pendingWorld.exchange(nullptr, std::memory_order_acq_rel)) {
            wd = pw;
            c = pw->c;  // temperature changes the speed of sound, humidity the air absorption
            airAmp = pw->airAmp;
            ackSerial.store(pw->serial);
            worldChanged = true;
        }
        double lst[4];
        for (int i = 0; i < 4; ++i) lst[i] = params[S * 4 + i].load(std::memory_order_relaxed);
        const double spread = std::min(std::max(params[S * 4 + 4].load(std::memory_order_relaxed), 0.0), 3.0);
        const double earlyDb = std::min(std::max(params[S * 4 + 5].load(std::memory_order_relaxed), -60.0), 6.0);
        bool lmoved = firstSolve || spread != lastSpread || earlyDb != lastEarlyDb;
        // Settings that ease towards their targets do so once per control period (fixed in stream time), so the output
        // does not depend on how the host chops the audio into blocks.
        if (shared) {
            if (lateGserial.load(std::memory_order_acquire) != lateGseen) {
                lateGseen = lateGserial.load(std::memory_order_acquire);
                lateGtarget = lateGbuf[(size_t)lateGcur.load(std::memory_order_acquire)].data();
            }
            if (lateGtarget)
                for (size_t i = 0; i < lateGuse.size(); ++i) lateGuse[i] += (lateGtarget[i] - lateGuse[i]) * 0.15f;
        }
        {
            const float gt = (float)std::min(std::max(params[S * 4 + 6].load(std::memory_order_relaxed), 0.0), 1.0);
            diffG += (gt - diffG) * 0.2f;
            if (diffG < 1e-3f && gt == 0.f) diffG = 0.f;
        }
        tailGa = tailGb;
        tailGb += ((float)std::pow(10.0, std::min(std::max(params[S * 5 + 8].load(std::memory_order_relaxed), -60.0), 12.0) / 20.0) - tailGb) * 0.3f;
        const double targetDoppler=std::min(std::max(params[S * 4 + 7].load(std::memory_order_relaxed), 0.0), 2.0);
        // A mode change also changes the reference-delay mixture. Ease that
        // coefficient in stream time, rather than adding a sudden phase slew.
        dopK=firstSolve?targetDoppler:dopK+(targetDoppler-dopK)*(1.-std::exp(-cfg.ctrlInterval/(.02*sr)));
        if(std::abs(dopK-targetDoppler)<1e-9)dopK=targetDoppler;
        dopA = 1.0 - std::exp(-(double)cfg.ctrlInterval / sr / 10.0);
        const double targetEarly=std::pow(10.0,earlyDb/20.0);
        if(earlyG!=targetEarly) {
            earlyG=firstSolve?targetEarly:earlyG+(targetEarly-earlyG)*(1.-std::exp(-cfg.ctrlInterval/(.02*sr)));
            if(std::abs(earlyG-targetEarly)<1e-9)earlyG=targetEarly;
            lmoved=true;
        }
        lastEarlyDb=earlyDb;
        for (int i = 0; i < 4; ++i) lmoved |= lst[i] != lastListener[i];
        if (lmoved) {
            listenerReceivers(lst, rcvBase);
            if (M == 4) {  // tetrahedron around the pickup point
                const double a = spread / std::sqrt(3.0);
                offs[0] = {a, a, a}; offs[1] = {a, -a, -a}; offs[2] = {-a, a, -a}; offs[3] = {-a, -a, a};
            } else if (M == 2) {
                offs[0] = {spread, 0, 0}; offs[1] = {-spread, 0, 0};
            }
            for (int r = 0; r < R; ++r)
                for (int k = 0; k < M; ++k) {
                    Transducer& t = rcvAbs[(size_t)(r * M + k)];
                    t.pos = rcvBase[(size_t)r].pos + (M > 1 ? offs[(size_t)k] : Vec3{});
                    t.azimuthDeg = rcvBase[(size_t)r].azimuthDeg;
                }
            for (int i = 0; i < 4; ++i) lastListener[i] = lst[i];
            lastSpread = spread;
        }
        for (int s = 0; s < S; ++s) {
            Src& sp = *srcs[s];
            double pose[4];
            for (int i = 0; i < 4; ++i) pose[i] = params[s * 4 + i].load(std::memory_order_relaxed);
            Vec3 pnow{pose[0], pose[1], pose[2]};
            ++sp.period;
            sp.hist[sp.period & (kHist - 1)] = pnow;
            const double pat = std::min(std::max(params[S * 4 + 8 + s].load(std::memory_order_relaxed), 0.0), 1.0);
            bool changed = lmoved || firstSolve || worldChanged || dopK != lastDopK || pat != sp.solvedPattern;
            sp.solvedPattern = pat;
            for (int i = 0; i < 4; ++i) changed |= pose[i] != sp.solvedPose[i];
            if (changed) sp.quiet = 0;
            else if (sp.quiet < (1 << 30)) ++sp.quiet;
            for (int i = 0; i < 4; ++i) sp.solvedPose[i] = pose[i];
            // Delays depend on where the source was up to one max-delay ago: keep solving until the history is still.
            // (with reduced/boosted Doppler the smoothed reference delay needs ~5 time constants to converge)
            const int settle = (int)(ringMask / cfg.ctrlInterval) + 2 + (dopK != 1.0 ? (int)(50.0 * sr / cfg.ctrlInterval) : 0);
            if (sp.quiet > settle) {
                for (int r = 0; r < RM; ++r)
                    for (auto& t : links[(size_t)s * RM + r]) { t.d0 = t.d1; std::memcpy(t.g0, t.g1, sizeof t.g0); }
                continue;
            }
            Transducer st = sp.t;
            st.pattern = pat;
            st.pos = pnow;
            st.azimuthDeg = pose[3];
            if (!buildImages(st.pos)) nodes.resize(1);
            for (int r = 0; r < RM; ++r) solveLink(s, r, st);
        }
        firstSolve = false;
        lastDopK = dopK;
    }

    void endPeriod() {
        tapsActive = 0;
        for (auto& l : links) {
            for (size_t i = 0; i < l.size();) {
                Tap& t = l[i];
                float m = 0;
                for (int b = 0; b < NB; ++b) m = std::max(m, t.g1[b]);
                if (m == 0.f) { l[i] = l.back(); l.pop_back(); }
                else ++i;
            }
            tapsActive += (int)l.size();
        }
    }

    // Early-reflection sum of receiver r: keep its history, and (amount > 0) decorrelate it with the channel's
    // velvet-noise FIR (energy ~ preserved: pulses have amplitude 1/sqrt(N)); amount crossfades dry -> decorrelated.
    void diffuse(int r, float* x, int m, float amount) {
        float* h = &vHist[(size_t)r * (vMask + 1)];
        for (int j = 0; j < m; ++j) h[(gcount + (uint64_t)j) & vMask] = x[j];
        if (amount <= 0.f) return;
        const int* pos = &vPos[(size_t)r * kVelvetN];
        const float* sgn = &vSgn[(size_t)r * kVelvetN];
        const float norm = 1.f / std::sqrt((float)kVelvetN);
        const float cd_ = std::cos(amount * 1.57079633f) , sd_ = std::sin(amount * 1.57079633f) * norm;  // equal-power crossfade
        for (int j = 0; j < m; ++j) {
            const uint64_t t = gcount + (uint64_t)j;
            float y = 0.f;
            for (int k = 0; k < kVelvetN; ++k) y += sgn[k] * h[(t - (uint64_t)pos[k]) & vMask];
            x[j] = cd_ * x[j] + sd_ * y;
        }
    }

    static float foldFrame(const float* g, const float* fr) {
        return g[0]*fr[0]+g[1]*fr[1]+g[2]*fr[2]+g[3]*fr[3]+g[4]*fr[4]+g[5]*fr[5]+g[6]*fr[6];
    }

    template<bool dual>
    void foldFrames(const float* g0, const float* g1, const float* ring, int64_t begin, int count, float* s0, float* s1) const {
        int u=0;
        for(;u<count;++u) {
            const float* fr=ring+(((size_t)(begin+u)&ringMask)*8);
            s0[u]=foldFrame(g0,fr);
            if constexpr(dual)s1[u]=foldFrame(g1,fr);
        }
    }

    // Direct-path taps (key 0) accumulate into `out`, reflections into `early`.
    void renderTaps(int s, int r, int ph, int m, float* out, float* early) {
        Src& sp = *srcs[s];
        const float* ring = sp.ring.data();
        const float invCtrl = 1.f / cfg.ctrlInterval;
        for (const Tap& t : links[(size_t)s * RM + r]) {
            float gmax = 0;
            for (int k = 0; k < NB; ++k) gmax = std::max(gmax, std::max(t.g0[k], t.g1[k]));
            if (gmax < 3e-5f) continue;  // below -90 dB: inaudible, not worth rendering
            float* const dst = (t.key == 0 || t.key == kDiffKey) ? out : early;
            bool stat = t.d0 == t.d1;
            for (int k = 0; k < NB && stat; ++k) stat = t.g0[k] == t.g1[k];
            // The band gains do not depend on the interpolation phase, so each frame is first folded into ONE value
            // (7 products), and the 4-point interpolation then runs on those values: ~3x cheaper than weighting
            // all 7 bands of 4 frames per output sample.
            if (stat) {
                // constant delay and gains for the whole period: fractional position and weights are constant
                const double pos0 = (double)(int64_t)gcount - t.d1;
                const int64_t i00 = (int64_t)std::floor(pos0);
                const float f = (float)(pos0 - (double)i00), f2 = f * f, f3 = f2 * f;
                const float w0 = -0.5f * f3 + f2 - 0.5f * f, w1 = 1.5f * f3 - 2.5f * f2 + 1.f, w2 = -1.5f * f3 + 2.f * f2 + 0.5f * f, w3 = 0.5f * f3 - 0.5f * f2;
                float* sb = sbA.data();
                foldFrames<false>(t.g1,nullptr,ring,i00-1,m+3,sb,nullptr);
                for (int j = 0; j < m; ++j) dst[j] += w0 * sb[j] + w1 * sb[j + 1] + w2 * sb[j + 2] + w3 * sb[j + 3];
                continue;
            }
            // moving tap: delay and gains glide over the period. Two folded streams (gains at the period start and
            // end) over the frames this tap touches; interpolate, then cross-fade between them.
            {
                const double dA = t.d0 + (t.d1 - t.d0) * (ph + 1) * invCtrl, dB = t.d0 + (t.d1 - t.d0) * (ph + m) * invCtrl;
                const double pA = (double)(int64_t)gcount - dA, pB = (double)(int64_t)(gcount + m - 1) - dB;
                const int64_t iLo = (int64_t)std::floor(std::min(pA, pB)) - 1, iHi = (int64_t)std::floor(std::max(pA, pB)) + 2;
                const int cnt = (int)(iHi - iLo) + 1;
                if (cnt > (int)sbA.size()) continue;  // cannot happen (delay steps are limited), but never overrun
                float* s0 = sbA.data();
                float* s1 = sbB.data();
                foldFrames<true>(t.g0,t.g1,ring,iLo,cnt,s0,s1);
                int j=0;
               #if VERB_REQUIRE_AVX2
                const auto d0=_mm256_set1_pd(t.d0), dd=_mm256_set1_pd(t.d1-t.d0);
                const auto origin=_mm256_set1_pd((double)(iLo+1));
                for(;j+4<=m;j+=4) {
                    const auto tt=_mm_mul_ps(_mm_setr_ps((float)(ph+j+1),(float)(ph+j+2),(float)(ph+j+3),(float)(ph+j+4)),_mm_set1_ps(invCtrl));
                    const auto time=_mm256_setr_pd((double)(int64_t)(gcount+j),(double)(int64_t)(gcount+j+1),(double)(int64_t)(gcount+j+2),(double)(int64_t)(gcount+j+3));
                    const auto pos=_mm256_sub_pd(time,_mm256_add_pd(d0,_mm256_mul_pd(dd,_mm256_cvtps_pd(tt))));
                    const auto whole=_mm256_floor_pd(pos);
                    const auto indices=_mm256_cvtpd_epi32(_mm256_sub_pd(whole,origin));
                    const auto f=_mm256_cvtpd_ps(_mm256_sub_pd(pos,whole));
                    const auto f2=_mm_mul_ps(f,f),f3=_mm_mul_ps(f2,f);
                    const auto w0=_mm_sub_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(-.5f),f3),f2),_mm_mul_ps(_mm_set1_ps(.5f),f));
                    const auto w1=_mm_add_ps(_mm_sub_ps(_mm_mul_ps(_mm_set1_ps(1.5f),f3),_mm_mul_ps(_mm_set1_ps(2.5f),f2)),_mm_set1_ps(1.f));
                    const auto w2=_mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(-1.5f),f3),_mm_mul_ps(_mm_set1_ps(2.f),f2)),_mm_mul_ps(_mm_set1_ps(.5f),f));
                    const auto w3=_mm_sub_ps(_mm_mul_ps(_mm_set1_ps(.5f),f3),_mm_mul_ps(_mm_set1_ps(.5f),f2));
                    const int index=_mm_cvtsi128_si32(indices);
                    const bool contiguous=_mm_movemask_epi8(_mm_cmpeq_epi32(indices,_mm_add_epi32(_mm_set1_epi32(index),_mm_setr_epi32(0,1,2,3))))==0xffff;
                    auto interpolate=[&](const float* values) {
                        auto read=[&](int offset) {return contiguous?_mm_loadu_ps(values+index+offset):_mm_i32gather_ps(values+offset,indices,4);};
                        auto a=_mm_add_ps(_mm_mul_ps(w0,read(0)),_mm_mul_ps(w1,read(1)));
                        a=_mm_add_ps(a,_mm_mul_ps(w2,read(2)));
                        return _mm_add_ps(a,_mm_mul_ps(w3,read(3)));
                    };
                    const auto a=interpolate(s0),b=interpolate(s1);
                    const auto value=_mm_add_ps(a,_mm_mul_ps(tt,_mm_sub_ps(b,a)));
                    _mm_storeu_ps(dst+j,_mm_add_ps(_mm_loadu_ps(dst+j),value));
                }
               #endif
                for (; j < m; ++j) {
                    const float tt = (ph + j + 1) * invCtrl;
                    const double pos = (double)(int64_t)(gcount + j) - (t.d0 + (t.d1 - t.d0) * tt);
                    const int64_t i0 = (int64_t)std::floor(pos);
                    const float f = (float)(pos - (double)i0), f2 = f * f, f3 = f2 * f;
                    const float w0 = -0.5f * f3 + f2 - 0.5f * f, w1 = 1.5f * f3 - 2.5f * f2 + 1.f, w2 = -1.5f * f3 + 2.f * f2 + 0.5f * f, w3 = 0.5f * f3 - 0.5f * f2;
                    const int u = (int)(i0 - 1 - iLo);
                    const float A = w0 * s0[u] + w1 * s0[u + 1] + w2 * s0[u + 2] + w3 * s0[u + 3];
                    const float Bv = w0 * s1[u] + w1 * s1[u + 1] + w2 * s1[u + 2] + w3 * s1[u + 3];
                    dst[j] += A + tt * (Bv - A);
                }
            }
        }
    }

    // ---- tail convolution ----
    void accumulate(const TailConv& t, const Slot& sl, int r, float* ar, float* ai) const {
        std::fill(ar, ar + t.bins, 0.f);
        std::fill(ai, ai + t.bins, 0.f);
        const size_t base = (size_t)r * sl.partitions * t.bins;
        for (int k = 0; k < sl.partitions; ++k) {
            int e = t.head - k;
            if (e < 0) e += t.P;
            const float* xr = &t.fdlRe[(size_t)e * t.bins];
            const float* xi = &t.fdlIm[(size_t)e * t.bins];
            const float* hr = &sl.re[base + (size_t)k * t.bins];
            const float* hi = &sl.im[base + (size_t)k * t.bins];
            int b = 0;
           #if VERB_REQUIRE_AVX2
            // Independent frequency bins; keep partition order and separate mul/add
            // rounding exactly as in the scalar path (no FMA reassociation).
            for (; b + 8 <= t.bins; b += 8) {
                const auto xre=_mm256_loadu_ps(xr+b), xim=_mm256_loadu_ps(xi+b);
                const auto hre=_mm256_loadu_ps(hr+b), him=_mm256_loadu_ps(hi+b);
                const auto re=_mm256_sub_ps(_mm256_mul_ps(xre,hre),_mm256_mul_ps(xim,him));
                const auto im=_mm256_add_ps(_mm256_mul_ps(xre,him),_mm256_mul_ps(xim,hre));
                _mm256_storeu_ps(ar+b,_mm256_add_ps(_mm256_loadu_ps(ar+b),re));
                _mm256_storeu_ps(ai+b,_mm256_add_ps(_mm256_loadu_ps(ai+b),im));
            }
           #endif
            for (; b < t.bins; ++b) {
                ar[b] += xr[b] * hr[b] - xi[b] * hi[b];
                ai[b] += xr[b] * hi[b] + xi[b] * hr[b];
            }
        }
    }

    void inverse(const float* ar, const float* ai, float* out) {
        for (int b = 0; b <= B; ++b) fftBuf[b] = {ar[b], ai[b]};
        fftBuf[0].im = 0; fftBuf[B].im = 0;
        for (int b = 1; b < B; ++b) fftBuf[2 * B - b] = {ar[b], -ai[b]};
        fft.run(fftBuf.data(), true);
        for (int i = 0; i < B; ++i) out[i] = fftBuf[B + i].re;
    }

    // One block of a partitioned convolver; its `t.R` output channels accumulate into rows firstRow.. of tailTmp.
    void convBlock(TailConv& t, int firstRow) {
        int p = t.pending.load();
        bool fade = false;
        if (p >= 0) { t.prev = t.cur; t.cur = p; fade = true; }
        // spectrum of [prevIn | inAcc]
        for (int i = 0; i < B; ++i) { fftBuf[i] = {t.prevIn[i], 0}; fftBuf[B + i] = {t.inAcc[i], 0}; }
        fft.run(fftBuf.data(), false);
        t.head = (t.head + 1) % t.P;
        for (int b = 0; b <= B; ++b) { t.fdlRe[(size_t)t.head * t.bins + b] = fftBuf[b].re; t.fdlIm[(size_t)t.head * t.bins + b] = fftBuf[b].im; }
        std::memcpy(t.prevIn.data(), t.inAcc.data(), sizeof(float) * B);
        for (int r = 0; r < t.R; ++r) {
            float* dst = &tailTmp[(size_t)(firstRow + r) * B];
            accumulate(t, t.slot[t.cur], r, accRe.data(), accIm.data());
            inverse(accRe.data(), accIm.data(), blkA.data());
            if (!fade) {
                for (int i = 0; i < B; ++i) dst[i] += blkA[i];
            } else {
                accumulate(t, t.slot[t.prev], r, accRe2.data(), accIm2.data());
                inverse(accRe2.data(), accIm2.data(), blkB.data());
                for (int i = 0; i < B; ++i) {
                    float w = (i + 0.5f) / B;
                    dst[i] += w * blkA[i] + (1 - w) * blkB[i];
                }
            }
        }
        if (fade) {
            t.curAtomic.store(t.cur);
            t.appliedRevision.store(t.slot[t.cur].revision,std::memory_order_release);
            t.appliedScene.store(t.slot[t.cur].sceneRevision,std::memory_order_release);
            t.pending.store(-1);
        }
    }

    void tailBlock() {
        std::fill(tailTmp.begin(), tailTmp.end(), 0.f);
        if (shared) {
            for (int r = 0; r < R; ++r) convBlock(*sconv[(size_t)r], r);
        } else {
            for (int s = 0; s < S; ++s) convBlock(srcs[(size_t)s]->tail, 0);
        }
        const size_t tr = tailMask + 1;
        for (int r = 0; r < R; ++r)
            for (int i = 0; i < B; ++i) tailRing[(size_t)r * tr + ((gcount + i) & tailMask)] = tailTmp[(size_t)r * B + i];
    }
    std::vector<float> blkA, blkB;

    void process(const float* const* in, float* const* out, int n) {
        const int ctrl = cfg.ctrlInterval;
        int done = 0;
        while (done < n) {
            int ph = (int)(gcount % ctrl);
            int m = std::min(n - done, ctrl - ph);
            if (ph == 0) startPeriod();
            for (int s = 0; s < S; ++s) {
                Src& sp = *srcs[s];
                for (int j = 0; j < m; ++j) {
                    float x = in[s][done + j];
                    sp.split.process(x, &sp.ring[(((size_t)(gcount + j)) & ringMask) * 8]);
                    if (!shared) sp.tail.inAcc[(gcount + j) % B] = x;
                }
            }
            if (shared) {
                // late-field feed: per receiver, the sum over sources of band-weighted source signals
                for (int r = 0; r < R; ++r) {
                    float* y = &lateY[(size_t)r * ctrl];
                    std::fill(y, y + m, 0.f);
                    for (int s = 0; s < S; ++s) {
                        const float* g = &lateGuse[((size_t)r * S + s) * 8];
                        const float* fr = srcs[(size_t)s]->ring.data();
                        for (int j = 0; j < m; ++j) {
                            const float* f = fr + (((size_t)(gcount + j)) & ringMask) * 8;
                            y[j] += g[0] * f[0] + g[1] * f[1] + g[2] * f[2] + g[3] * f[3] + g[4] * f[4] + g[5] * f[5] + g[6] * f[6];
                        }
                    }
                    TailConv& t = *sconv[(size_t)r];
                    for (int j = 0; j < m; ++j) t.inAcc[(gcount + j) % B] = y[j];
                }
            }
            for (int j = 0; j < m; ++j)
                for (int k = 0; k < 3; ++k) {
                    if (componentRamp[k] > 0) {
                        componentGain[k] += componentStep[k];
                        if (--componentRamp[k] == 0) componentGain[k] = componentTarget[k];
                    }
                    componentMix[(size_t)k * ctrl + j] = componentGain[k];
                }
            for (int r = 0; r < R; ++r) {
                float* acc = &tmpOut[(size_t)r * ctrl];
                float* ear = &tmpEarly[(size_t)r * ctrl];
                std::fill(acc, acc + m, 0.f);
                std::fill(ear, ear + m, 0.f);
                for (int s = 0; s < S; ++s)
                    for (int k = 0; k < M; ++k) renderTaps(s, r * M + k, ph, m, acc, ear);
                if (M > 1) {
                    const float inv = 1.f / (float)M;
                    for (int j = 0; j < m; ++j) { acc[j] *= inv; ear[j] *= inv; }
                }
                diffuse(r, ear, m, diffG);
                const size_t tr = tailMask + 1;
                for (int j = 0; j < m; ++j) {
                    const float tg = tailGa + (tailGb - tailGa) * (float)(ph + j + 1) / (float)ctrl;
                    out[r][done + j] = componentMix[j] * acc[j] + componentMix[ctrl + j] * ear[j]
                        + componentMix[2 * ctrl + j] * tg * tailRing[(size_t)r * tr + ((gcount + j) & tailMask)];
                }
            }
            gcount += m;
            done += m;
            if (ph + m == ctrl) endPeriod();
            if (gcount % B == 0) tailBlock();
        }
        publishedDiffractionMisses.store(diffractionScratch.capacityMisses,std::memory_order_relaxed);
    }
};

Engine::Engine(const Scene& scene, const EngineConfig& cfg) : p_(new Impl(scene, cfg)) {}
Engine::~Engine() = default;
int Engine::numSources() const { return p_->S; }
int Engine::numReceivers() const { return p_->R; }
double Engine::sampleRate() const { return p_->sr; }
double Engine::tailSeconds() const { return p_->renderedSeconds.load(); }
void Engine::setSourcePosition(int s, double x, double y, double z) {
    p_->params[s * 4 + 0].store(x); p_->params[s * 4 + 1].store(y); p_->params[s * 4 + 2].store(z);
}
bool Engine::setScene(const Scene& s) { return p_->setScene(s,p_->currentAbsorption.load(),{}); }
bool Engine::setScene(const Scene& s, double absorption, const std::function<bool()>& cancelled) { return p_->setScene(s,absorption,cancelled); }
void Engine::setReceiverSpread(double m) { p_->params[p_->S * 4 + 4].store(m); }
void Engine::setEarlyLevel(double dB) { p_->params[p_->S * 4 + 5].store(dB); }
void Engine::setEarlyDiffusion(double a) { p_->params[p_->S * 4 + 6].store(a); }
void Engine::setTailLevel(double dB) { p_->params[p_->S * 5 + 8].store(dB); }
void Engine::setSoloMask(int mask) { p_->soloMask.store(mask & 7, std::memory_order_relaxed); }
void Engine::setReverbMix(double amount) { p_->reverbMix.store((float)clampTo(amount, 0.0, 1.0, 1.0), std::memory_order_relaxed); }
void Engine::setTailQuality(int rays, bool adaptive) {
    p_->requestedRays.store(std::min(std::max(rays, 8000), 384000));
    p_->requestedAdaptive.store(adaptive);
    p_->qualitySerial.fetch_add(1, std::memory_order_release);
}
void Engine::setDecayMultipliers(double low, double mid, double high) {
    const double v[] = {low,mid,high}; bool changed = false;
    for (int g=0;g<3;++g) {
        const double x = clampTo(v[g],.3,3.,1.);
        if (p_->requestedDecay[g].exchange(x, std::memory_order_relaxed) != x) changed = true;
    }
    if (changed) p_->qualitySerial.fetch_add(1,std::memory_order_release);
}
void Engine::setDopplerStrength(double k) { p_->params[p_->S * 4 + 7].store(k); }
void Engine::setSourcePattern(int s, double p) { if (s >= 0 && s < p_->S) p_->params[p_->S * 4 + 8 + s].store(p); }
void Engine::setSourceAzimuth(int s, double deg) { p_->params[s * 4 + 3].store(deg); }
void Engine::setListener(double x, double y, double z, double yaw) {
    int o = p_->S * 4;
    p_->params[o].store(x); p_->params[o + 1].store(y); p_->params[o + 2].store(z); p_->params[o + 3].store(yaw);
}
void Engine::process(const float* const* in, float* const* out, int n) { p_->process(in, out, n); }
int Engine::activeTaps() const { return p_->tapsActive; }
int Engine::tailRefreshCount() const { return p_->refreshCount.load(); }
uint64_t Engine::diffractionCapacityMisses() const { return p_->publishedDiffractionMisses.load(std::memory_order_relaxed); }
bool Engine::tailIsCurrent() const {
    const auto revision = p_->qualitySerial.load(std::memory_order_acquire);
    const auto scene = p_->geomSerial.load(std::memory_order_acquire);
    if (p_->shared) {
        if (p_->sharedBusy.load(std::memory_order_acquire)) return false;
        for (const auto& c : p_->sconv) if (c->pending.load()>=0 || c->appliedRevision.load(std::memory_order_acquire)!=revision || c->appliedScene.load(std::memory_order_acquire)!=scene) return false;
    } else for (const auto& s : p_->srcs) if (s->tail.workerBusy.load(std::memory_order_acquire) || s->tail.pending.load()>=0 || s->tail.appliedRevision.load(std::memory_order_acquire)!=revision || s->tail.appliedScene.load(std::memory_order_acquire)!=scene) return false;
    return true;
}
std::shared_ptr<const TailDiagnostics> Engine::tailDiagnostics() const {
    std::lock_guard<std::mutex> lock(p_->diagnosticsMx);
    return p_->diagnostics;
}
std::shared_ptr<const Scene> Engine::sceneSnapshot() const { return std::atomic_load(&p_->analysisScene); }
bool Engine::copyTailForOffline(const Engine& reference) {
    auto& dst=*p_;auto& src=*reference.p_;
    if(&dst==&src||dst.cfg.tailThread||dst.S!=src.S||dst.R!=src.R||dst.sr!=src.sr||dst.B!=src.B||dst.shared!=src.shared)return false;
    std::lock_guard<std::mutex> lock(src.irSnapshotMx);
    // Retain the audible immutable world as well: reconstructing seeded furniture
    // from rounded project coordinates must not move an obstacle in an export.
    std::shared_ptr<World> audibleWorld;
    {
        std::lock_guard<std::mutex> worldLock(src.worldMx);
        const auto audibleSerial=src.ackSerial.load(std::memory_order_acquire);
        for(const auto& world:src.worlds)if(world->serial==audibleSerial){audibleWorld=world;break;}
    }
    if(!audibleWorld)return false;
    dst.worlds.push_back(audibleWorld);dst.latest=audibleWorld;
    dst.geomSerial=audibleWorld->serial;dst.serialCounter=std::max(dst.serialCounter,audibleWorld->serial);
    std::atomic_store(&dst.analysisScene,std::shared_ptr<const Scene>(audibleWorld,&audibleWorld->sc));
    dst.pendingWorld.store(audibleWorld.get(),std::memory_order_release);
    size_t longest=0;
    auto copy=[&](TailConv& to,const TailConv& from) {
        const auto& active=from.slot[from.curAtomic.load(std::memory_order_acquire)];
        auto& target=to.slot[to.cur];target=active;
        target.revision=dst.qualitySerial.load();target.sceneRevision=dst.geomSerial.load();
        to.pending=-1;to.appliedRevision=target.revision;to.appliedScene=target.sceneRevision;
        longest=std::max(longest,target.samples);
    };
    if(dst.shared) {
        for(int r=0;r<dst.R;++r)copy(*dst.sconv[r],*src.sconv[r]);
        // Gain buffers are immutable to audio and protected from worker reuse here.
        const auto& gain=src.lateGbuf[src.lateGcur.load(std::memory_order_acquire)];
        dst.lateGbuf[0]=dst.lateGbuf[1]=dst.lateGuse=gain;
        dst.lateGcur=0;dst.lateGtarget=dst.lateGbuf[0].data();dst.lateGseen=dst.lateGserial.load();
    } else for(int s=0;s<dst.S;++s)copy(dst.srcs[s]->tail,src.srcs[s]->tail);
    dst.renderedSeconds.store(longest/dst.sr);
    return true;
}

}  // namespace verb
