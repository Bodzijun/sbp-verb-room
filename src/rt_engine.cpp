#include "rt_engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

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
    }
    void run(cf* a, bool inv) const {
        for (int i = 0; i < n_; ++i)
            if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
        for (int len = 2; len <= n_; len <<= 1) {
            int half = len / 2, step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int k = 0; k < half; ++k) {
                    float wr = twr_[k * step], wi = inv ? -twi_[k * step] : twi_[k * step];
                    cf u = a[i + k], x = a[i + k + half];
                    float vr = x.re * wr - x.im * wi, vi = x.re * wi + x.im * wr;
                    a[i + k] = {u.re + vr, u.im + vi};
                    a[i + k + half] = {u.re - vr, u.im - vi};
                }
        }
        if (inv) {
            float s = 1.f / n_;
            for (int i = 0; i < n_; ++i) { a[i].re *= s; a[i].im *= s; }
        }
    }
    int size() const { return n_; }

private:
    int n_;
    std::vector<int> rev_;
    std::vector<float> twr_, twi_;
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
    std::vector<float> re, im;  // [receiver][partition][bin]
};

struct TailConv {
    int B = 0, P = 0, R = 0, bins = 0;
    std::vector<float> fdlRe, fdlIm;  // [partition][bin] ring of input spectra
    int head = 0;
    std::vector<float> prevIn, inAcc;
    Slot slot[3];
    std::atomic<int> pending{-1};
    std::atomic<int> curAtomic{0};
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
    World(const Scene& s, int maxWalls) : sc(withClutter(withRoughness(s), maxWalls)), geo(sc) {
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

struct Engine::Impl {
    Scene sc;
    EngineConfig cfg;
    int S, R;
    double sr, c;
    // geometry: audio thread uses `wd`; setScene() publishes through `pendingWorld`; the worker frees old worlds
    std::mutex worldMx;
    std::vector<std::shared_ptr<World>> worlds;
    std::shared_ptr<World> latest;
    uint64_t serialCounter = 0;
    World* wd = nullptr;
    std::atomic<World*> pendingWorld{nullptr};
    std::atomic<uint64_t> ackSerial{0}, geomSerial{0};
    int maxWalls = 128;
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
    std::vector<std::unique_ptr<TailConv>> sconv;  // [R], each a single-channel convolver
    std::array<std::vector<float>, 2> lateGbuf;    // [r][s][8] target gains, double-buffered by the worker
    std::atomic<int> lateGcur{0};
    std::atomic<uint64_t> lateGserial{0};
    uint64_t lateGseen = 0;
    const float* lateGtarget = nullptr;
    std::vector<float> lateGuse;                   // gains actually applied (eased towards the target)
    std::vector<float> lateY;                      // [R][ctrl] scratch
    std::vector<TailHist> srcHist;                 // worker: cached late-field histogram per source
    double sLastLst[4] = {1e30, 0, 0, 0};
    uint64_t sLastGeom = 0;
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
        sc.length = effectiveTailSeconds(sc, cfg.erOrder);  // never shorter than the scene says, long enough that the tail is not cut
        if (cfg.tailBlock % cfg.ctrlInterval) throw std::runtime_error("tailBlock must be a multiple of ctrlInterval");
        maxWalls = cfg.erOrder <= 1 ? 128 : cfg.erOrder == 2 ? 64 : cfg.erOrder == 3 ? 24 : 12;
        if ((int)scene.walls.size() > maxWalls) throw std::runtime_error("too many walls for this reflection order (max " + std::to_string(maxWalls) + ")");
        B = cfg.tailBlock;
        latest = std::make_shared<World>(sc, maxWalls);
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
        int P = (int)((N - B + B - 1) / B);
        auto initConv = [&](TailConv& t, int channels) {
            t.B = B; t.P = P; t.R = channels; t.bins = B + 1;
            t.fdlRe.assign((size_t)P * t.bins, 0.f); t.fdlIm.assign((size_t)P * t.bins, 0.f);
            t.prevIn.assign(B, 0.f); t.inAcc.assign(B, 0.f);
            for (auto& sl : t.slot) { sl.re.assign((size_t)channels * P * t.bins, 0.f); sl.im.assign((size_t)channels * P * t.bins, 0.f); }
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
            srcHist.resize((size_t)S);
        }
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
                buildTail(s, latest->sc, st, rc, 0);
                srcs[s]->tail.cur = 0;
                srcs[s]->tail.curAtomic = 0;
            }
        }
        if (shared) sharedRebuild(true);
        if (cfg.tailThread) worker = std::thread([this] { if (shared) workerLoopShared(); else workerLoop(); });
    }

    ~Impl() {
        running = false;
        if (worker.joinable()) worker.join();
    }

    // ---- tail IR generation (worker or constructor) ----
    void buildTail(int s, const Scene& scn, const Transducer& st, const std::vector<Transducer>& rc, int slotIdx) {
        TailConv& t = srcs[s]->tail;
        size_t N = (size_t)std::ceil(scn.length * sr);
        TailHist h = traceTail(scn, st, rc, cfg.erOrder, cfg.tailRays, scn.seed);
        BandBuf buf(R);
        for (auto& r : buf)
            for (auto& b : r) b.assign(N, 0.0);
        addTailNoise(scn, h, scn.seed, buf);
        auto ir = combineBands(buf, sr);
        fadeTailEnd(ir, N, sr);
        FFTf wf(2 * B);
        std::vector<cf> tmp(2 * B);
        Slot& sl = t.slot[slotIdx];
        for (int r = 0; r < R; ++r)
            for (int k = 0; k < t.P; ++k) {
                for (int i = 0; i < 2 * B; ++i) tmp[i] = {0, 0};
                for (int i = 0; i < B; ++i) {
                    size_t idx = (size_t)(k + 1) * B + i;  // drop the first B samples: latency compensation
                    if (idx < N) tmp[i].re = ir[r][idx];
                }
                wf.run(tmp.data(), false);
                size_t o = ((size_t)r * t.P + k) * t.bins;
                for (int b = 0; b < t.bins; ++b) { sl.re[o + b] = tmp[b].re; sl.im[o + b] = tmp[b].im; }
            }
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
        const bool all = initial || wsnap->serial != sLastGeom || dl > 0.05 || std::fabs(lst[3] - sLastLst[3]) > 2;
        const int rays = std::max(1500, cfg.tailRays / std::max(1, S));
        std::vector<int> dirty;
        std::vector<std::array<double, 4>> poses((size_t)S);
        for (int s = 0; s < S; ++s) {
            Src& sp = *srcs[(size_t)s];
            for (int i = 0; i < 4; ++i) poses[(size_t)s][(size_t)i] = params[s * 4 + i].load();
            const auto& p = poses[(size_t)s];
            const double dp = std::hypot(std::hypot(p[0] - sp.lastPose[0], p[1] - sp.lastPose[1]), p[2] - sp.lastPose[2]);
            if (all || dp > 0.05 || std::fabs(p[3] - sp.lastPose[3]) > 2) dirty.push_back(s);
        }
        if (dirty.empty()) return false;
        for (int s : dirty) {
            Transducer st = srcs[(size_t)s]->t;
            st.pos = {poses[(size_t)s][0], poses[(size_t)s][1], poses[(size_t)s][2]};
            st.azimuthDeg = poses[(size_t)s][3];
            srcHist[(size_t)s] = traceTail(scn, st, rc, cfg.erOrder, rays, scn.seed + 131 * s);
            for (int i = 0; i < 4; ++i) srcs[(size_t)s]->lastPose[i] = poses[(size_t)s][(size_t)i];
        }
        // average envelope + per-source band energies
        const size_t nBins = srcHist[0].nBins;
        TailHist avg;
        avg.nBins = nBins;
        avg.binDur = srcHist[0].binDur;
        avg.h.resize((size_t)R);
        std::vector<double> E((size_t)S * R * NB, 0.0), Eavg((size_t)R * NB, 0.0);
        for (int r = 0; r < R; ++r)
            for (int b = 0; b < NB; ++b) {
                auto& dst = avg.h[(size_t)r][(size_t)b];
                dst.assign(nBins, 0.0);
                for (int s = 0; s < S; ++s) {
                    const auto& h = srcHist[(size_t)s].h[(size_t)r][(size_t)b];
                    double e = 0;
                    for (size_t k = 0; k < nBins; ++k) { dst[k] += h[k] / S; e += h[k]; }
                    E[((size_t)s * R + r) * NB + b] = e;
                    Eavg[(size_t)r * NB + b] += e / S;
                }
            }
        const int inactive = 1 - lateGcur.load();
        std::vector<float>& G = lateGbuf[(size_t)inactive];
        for (int r = 0; r < R; ++r)
            for (int s = 0; s < S; ++s)
                for (int b = 0; b < 8; ++b) {
                    const double ea = b < NB ? Eavg[(size_t)r * NB + b] : 0.0;
                    G[((size_t)r * S + s) * 8 + b] = (b < NB && ea > 0) ? (float)std::sqrt(E[((size_t)s * R + r) * NB + b] / ea) : 0.f;
                }
        // common IRs
        const size_t N = (size_t)std::ceil(scn.length * sr);
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
            for (int k = 0; k < t.P; ++k) {
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
        ++refreshCount;
        return true;
    }

    void workerLoopShared() {
        auto last = std::chrono::steady_clock::now();
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
            if (sharedRebuild(false)) last = std::chrono::steady_clock::now();
        }
    }

    void workerLoop() {
        std::vector<std::chrono::steady_clock::time_point> last(S, std::chrono::steady_clock::now());
        int rr = 0;
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
                moved |= pat != sp.lastPattern;
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
                buildTail(s, wsnap->sc, st, rc, w);
                for (int i = 0; i < 4; ++i) sp.lastPose[i] = pose[i];
                for (int i = 0; i < 4; ++i) sp.lastLst[i] = lst[i];
                sp.lastGeom = wsnap->serial;
                last[s] = std::chrono::steady_clock::now();
                sp.tail.pending.store(w);
                rr = (s + 1) % S;
                break;
            }
        }
    }
    // Publishes new walls/materials/floor/ceiling/height (sources, receivers and length stay). Any thread.
    bool setScene(const Scene& ns) {
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
        auto w = std::make_shared<World>(n, maxWalls);
        {
            std::lock_guard<std::mutex> lk(worldMx);
            w->serial = ++serialCounter;
            worlds.push_back(w);
            latest = w;
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
            if (!diffractedPath(G(), src.pos, rc.pos, c, dp, needClear)) return;
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
        dopK = std::min(std::max(params[S * 4 + 7].load(std::memory_order_relaxed), 0.0), 2.0);
        dopA = 1.0 - std::exp(-(double)cfg.ctrlInterval / sr / 10.0);
        if (earlyDb != lastEarlyDb) { earlyG = std::pow(10.0, earlyDb / 20.0); lastEarlyDb = earlyDb; }
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

    // Direct-path taps (key 0) accumulate into `out`, reflections into `early`.
    void renderTaps(int s, int r, int ph, int m, float* out, float* early) {
        Src& sp = *srcs[s];
        const float* ring = sp.ring.data();
        const float invCtrl = 1.f / cfg.ctrlInterval;
        for (const Tap& t : links[(size_t)s * RM + r]) {
            float gmax = 0;
            for (int k = 0; k < NB; ++k) gmax = std::max(gmax, std::max(t.g0[k], t.g1[k]));
            if (gmax < 3e-5f) continue;  // below -90 dB: inaudible, not worth rendering
            float* const dst = t.key == 0 ? out : early;
            bool stat = t.d0 == t.d1;
            for (int k = 0; k < NB && stat; ++k) stat = t.g0[k] == t.g1[k];
            // The band gains do not depend on the interpolation phase, so each frame is first folded into ONE value
            // (7 products), and the 4-point interpolation then runs on those values: ~3x cheaper than weighting
            // all 7 bands of 4 frames per output sample.
            auto fold = [](const float* g, const float* fr) {
                return g[0] * fr[0] + g[1] * fr[1] + g[2] * fr[2] + g[3] * fr[3] + g[4] * fr[4] + g[5] * fr[5] + g[6] * fr[6];
            };
            if (stat) {
                // constant delay and gains for the whole period: fractional position and weights are constant
                const double pos0 = (double)(int64_t)gcount - t.d1;
                const int64_t i00 = (int64_t)std::floor(pos0);
                const float f = (float)(pos0 - (double)i00), f2 = f * f, f3 = f2 * f;
                const float w0 = -0.5f * f3 + f2 - 0.5f * f, w1 = 1.5f * f3 - 2.5f * f2 + 1.f, w2 = -1.5f * f3 + 2.f * f2 + 0.5f * f, w3 = 0.5f * f3 - 0.5f * f2;
                float* sb = sbA.data();
                for (int u = 0; u < m + 3; ++u) sb[u] = fold(t.g1, ring + (((size_t)(i00 - 1 + u)) & ringMask) * 8);
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
                for (int u = 0; u < cnt; ++u) {
                    const float* fr = ring + (((size_t)(iLo + u)) & ringMask) * 8;
                    s0[u] = fold(t.g0, fr);
                    s1[u] = fold(t.g1, fr);
                }
                for (int j = 0; j < m; ++j) {
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
        const size_t base = (size_t)r * t.P * t.bins;
        for (int k = 0; k < t.P; ++k) {
            int e = t.head - k;
            if (e < 0) e += t.P;
            const float* xr = &t.fdlRe[(size_t)e * t.bins];
            const float* xi = &t.fdlIm[(size_t)e * t.bins];
            const float* hr = &sl.re[base + (size_t)k * t.bins];
            const float* hi = &sl.im[base + (size_t)k * t.bins];
            for (int b = 0; b < t.bins; ++b) {
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
                    out[r][done + j] = acc[j] + ear[j] + tg * tailRing[(size_t)r * tr + ((gcount + j) & tailMask)];
                }
            }
            gcount += m;
            done += m;
            if (ph + m == ctrl) endPeriod();
            if (gcount % B == 0) tailBlock();
        }
    }
};

Engine::Engine(const Scene& scene, const EngineConfig& cfg) : p_(new Impl(scene, cfg)) {}
Engine::~Engine() = default;
int Engine::numSources() const { return p_->S; }
int Engine::numReceivers() const { return p_->R; }
double Engine::sampleRate() const { return p_->sr; }
double Engine::tailSeconds() const { return p_->sc.length; }
void Engine::setSourcePosition(int s, double x, double y, double z) {
    p_->params[s * 4 + 0].store(x); p_->params[s * 4 + 1].store(y); p_->params[s * 4 + 2].store(z);
}
bool Engine::setScene(const Scene& s) { return p_->setScene(s); }
void Engine::setReceiverSpread(double m) { p_->params[p_->S * 4 + 4].store(m); }
void Engine::setEarlyLevel(double dB) { p_->params[p_->S * 4 + 5].store(dB); }
void Engine::setEarlyDiffusion(double a) { p_->params[p_->S * 4 + 6].store(a); }
void Engine::setTailLevel(double dB) { p_->params[p_->S * 5 + 8].store(dB); }
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

}  // namespace verb
