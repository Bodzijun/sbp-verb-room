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
#include "creative_wet.hpp"
#include "geometry.hpp"
#include "tail.hpp"
#include "energy.hpp"

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
    float fadeStart[NB] = {}, fadeTarget[NB] = {};
    int fadeLeft = 0, fadeTotal = 0;
    bool seen = false;
    bool jumpFadeIn = false;
    bool jumpFadeOut = false;
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
    bool isJump = false;
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
    // Legacy output fade fields. E7 settle transitions use hoSlot plus the
    // secondary causal FDL below; hoAge includes the old-history drain.
    int longN = 0, longAt = 0;
    float rho = 0.f, rhoA = 0.f;      // correlation of the two outputs: starts from the IRs', then measured on the signals
    double cAB = 0, cAA = 0, cBB = 0;  // smoothed products of the two outputs during the long crossfade
    // per-source late field: the worker asks for a long crossfade of its next pending IR (written before `pending`)
    int longReq = 0;
    float rhoReq = 0.f;
    bool longFromPending = false;     // audio: the running long crossfade came from `pending` (cleared at its end)
    int hoSlot = -1, hoAge = 0;
    // E7: the new static normalization has its own causal input history. The
    // worker clears this inactive bank before publication; process never allocates.
    std::vector<float> freshRe, freshIm, freshPrev, freshIn;
    // E8: clean unboosted input history for per-source (!shared) jump handover abort.
    std::vector<float> cleanRe, cleanIm, cleanPrev, cleanIn;
    std::atomic<int> cleanSynced{0};
    uint64_t inputStart=0; int inputFadeBlocks=0;
    std::atomic<int> publishedHandover{-1};
    bool legacyHandover=false;
    std::array<float,NB> bandRho{};

};

// Immutable snapshot of everything geometric. The audio thread reads one World; the editor publishes a new one.
struct World {
    Scene sc;
    Geo geo;
    Scene lengthProbe;  // authored geometry/minimum, before generated furniture/absorption
    double tailAbsorption=1.;
    std::vector<std::array<float, NB>> refl;  // [surface][band] specular reflection amplitude
    uint64_t serial = 0;
    double constructionPoseScale=1.;
    double c = 343.2;                         // speed of sound for this temperature
    std::array<double, NB> airAmp{};          // per-metre amplitude factor of the air (temperature, humidity)
    // `sc` is the scene including its seeded clutter (capped so the total fits the engine)
    World(const Scene& s, int maxWalls, double absorption = 1., double minimumLength=-1.) : sc(withClutter(withRoughness(s), maxWalls)), geo(sc), lengthProbe(s), tailAbsorption(absorption) {
        if(minimumLength>=0)lengthProbe.length=minimumLength;
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
static bool tailSecondsCache(bool computeIfMiss, Scene s, int order, double absorption, const std::function<bool()>& cancelled, double& outLen) {
    if(cancelled && cancelled())throw std::runtime_error("Scene superseded");
    absorption=clampTo(absorption,.3,3.,1.);
    for(auto& m:s.mats)for(int b=0;b<NB;++b)m.alpha[b]=std::clamp(m.alpha[b]*absorption,0.,std::max(0.,1.-m.tau[b]));
    const auto key=std::to_string(order)+serializeScene(s);
    static std::mutex mx;static std::map<std::string,double> cache;
    {std::lock_guard<std::mutex> lock(mx);auto it=cache.find(key);if(it!=cache.end()){outLen=it->second;return true;}}
    if(!computeIfMiss)return false;
    const double length=effectiveTailSeconds(s,order,cancelled);
    {std::lock_guard<std::mutex> lock(mx);if(cache.size()>=64)cache.erase(cache.begin());cache[key]=length;}
    outLen=length;
    return true;
}

static double cachedTailSeconds(Scene s, int order, double absorption, const std::function<bool()>& cancelled) {
    double length = s.length;
    tailSecondsCache(true, std::move(s), order, absorption, cancelled, length);
    return length;
}

static bool tryCachedTailSeconds(Scene s, int order, double absorption, double& outLen) {
    return tailSecondsCache(false, std::move(s), order, absorption, {}, outLen);
}

struct Engine::Impl {
    std::mutex irSnapshotMx; // worker/control only; never acquired by process()
    std::atomic<uint64_t> publishedDiffractionMisses{0};
    Scene sc;
    EngineConfig cfg;
    std::atomic<double> roomTailLength{0.0};
    bool sharedLengthCheck = false;
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
    std::atomic<World*> audioWorld{nullptr};
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
        double solvedRawPose[4] = {1e30, 0, 0, 0};  // raw pose before Rule D (audio jump detection)
        std::vector<Vec3> hist;                  // source position at each control period (retarded-time delays)
        uint64_t period = 0;                     // index of the current control period
        uint64_t histMinPeriod = 0;              // earliest valid history period after a jump
        uint64_t earlyEpoch=0;
        int quiet = 0;                           // periods since the source/listener last moved
        int fadeLeftPeriods = 0;                 // remaining periods of jump crossfade
        Transducer t;
    };
    std::vector<std::unique_ptr<Src>> srcs;
    size_t ringMask = 0;
    std::vector<std::vector<Tap>> links;  // [s*R + r]
    // E3b: the way round the wall ends is a function of (source position, pickup position, needClear, world, speed of sound)
    // only. A source that stands still is solved again every control period while its delay history settles; the same
    // answer is replayed instead of searching again. [(s*RM + r)*2 + needClear]; filled on the audio thread only.
    // E3d: ... and the search itself is bounded. The one-end stage (linear in the walls, like the direct path) runs at once
    // and decides most queries exactly. A query that needs the multi-end search (pairs and triples of wall ends) becomes a
    // request; runDiffractionJobs() works on one request at a time with a fixed work budget per control period
    // (DiffractionSearch is resumable and bit-identical however it is split). Until the answer for the current positions
    // is there, the link follows the wall ends of its last answer, re-evaluated exactly at the current positions every
    // period (evalDiffSeq: legs, corners and needClear tested again, so a way that closes stops at once). A path that has
    // just closed fades out over kDiffHold periods unless the new answer arrives first; that is the longest a stale path
    // can sound. The constructor solves the initial positions completely, so a scene that does not move sounds as before.
    struct DiffMemo {
        Vec3 S{}, R{}; double c = 0; uint64_t serial = 0; bool valid = false, found = false; DiffPath dp;  // exact answer for (S, R)
        DiffSeq seq;                                    // wall ends of the newest answer (any positions), followed meanwhile
        bool want = false, urgent = false;              // the multi-end search is wanted for (wS, wR)
        Vec3 wS{}, wR{}; uint64_t wantPeriod = 0, wantSince = 0;
        DiffPath held; int hold = 0;                    // the last path given, and how many periods it may still fade
        uint64_t epoch = 0;
        bool exact = true;                              // the last answer given was the exact one for its positions
    };
    struct SoftMemo {Vec3 a{},b{},point{};uint64_t serial=0,generation=0;double speed=0;float gain[NB]={};bool ok=false;};
    std::vector<SoftMemo> softMemo;
    struct AmpMemo {uint64_t serial=0,epoch=0,revision=0;double len=0,early=0,speed=0;Found value;};
    std::vector<AmpMemo> ampMemo;std::vector<double> pathPeakGain;bool warmingSoft=false;
    int softStride=0;int64_t softSpent=0;
    size_t softCursor=0,softNext=0;bool softDeferred=false;
    uint64_t softEvals=0,softHits=0;
    static int64_t softWork(const SoftEdgeProfile& p){return p.boxes+p.walls+p.vertices+p.junctionRays+p.validationScans+4*p.knife+8*p.powers;}
    // E6: side 0 (receiver leg) also depends on the source position `far` (whole-path hand-over at vertical shared
    // edges for reflections on horizontal faces); it is part of the memo key (stored in `point`).
    bool cachedSoftLeg(int source,int receiver,int surf,int side,Vec3 a,Vec3 b,float* amp,bool warm=false,Vec3 far={}){
        if(!wd->sc.diffraction || (G().freeScreenWalls.empty() && G().screenRims.empty() && G().legWelds.empty()))return transmitLeg(G(),a,b,surf,-1,amp);
        const size_t slot=(((size_t)source*RM+receiver)*softStride+surf)*4+side+1;
        auto& m=softMemo[slot];
        const bool valid=m.serial==wd->serial && m.speed==c;
        const bool same=valid && a.x==m.a.x&&a.y==m.a.y&&a.z==m.a.z&&b.x==m.b.x&&b.y==m.b.y&&b.z==m.b.z
            && (side!=0 || (far.x==m.point.x&&far.y==m.point.y&&far.z==m.point.z));
        if(same)++softHits;
        else if(warm || (slot>=softCursor && softSpent<diffBudget/8)){
            const auto before=softProfile;float v[NB];std::fill_n(v,NB,1.f);
            const bool ok=softReflectionLeg(G(),a,b,surf,-1,c,v,side==0?1:2,far);
            const auto after=softProfile;
            softSpent+=softWork(after)-softWork(before);++softEvals;
            m.a=a;m.b=b;m.point=far;m.serial=wd->serial;m.speed=c;m.ok=ok;++m.generation;std::copy_n(v,NB,m.gain);
        } else {diffPendingNow=true;++softHits;if(slot>=softCursor&&!softDeferred){softDeferred=true;softNext=slot;}}
        if(m.serial!=wd->serial || m.speed!=c)return false;
        // The surrounding tap solver already glides these bounded amplitudes over every control period.
        if(!m.ok)return false;
        for(int band=0;band<NB;++band)amp[band]*=m.gain[band];
        return true;
    }
    bool cachedSupport(int source,int receiver,int surf,Vec3 a,Vec3 b,Vec3& point,float* amp,bool warm=false){
        if(!wd->sc.diffraction)return crossSurface(G(),a,b,surf,point);
        // Empty closed boxes use the original inexpensive face solver, with no memo work.
        if(G().freeScreenWalls.empty() && G().screenRims.empty() && !G().freeReflectorHeights)return crossSurfaceLegacy(G(),a,b,surf,point,c,amp);
        const size_t slot=(((size_t)source*RM+receiver)*softStride+surf)*4;
        auto& m=softMemo[slot];
        const bool valid=m.serial==wd->serial&&m.speed==c;
        const bool same=valid&&a.x==m.a.x&&a.y==m.a.y&&a.z==m.a.z&&b.x==m.b.x&&b.y==m.b.y&&b.z==m.b.z;
        if(same)++softHits;
        else if(warm || (slot>=softCursor&&softSpent<diffBudget/8)){
            const auto before=softProfile;const bool previous=profilingSoftLeg;profilingSoftLeg=true;
            float v[NB];std::fill_n(v,NB,1.f);Vec3 P;
            bool ok=crossSurfaceSoft(G(),a,b,surf,P,c,v);
            profilingSoftLeg=previous;
            softSpent+=softWork(softProfile)-softWork(before)+1;++softEvals;
            m.a=a;m.b=b;m.point=P;m.serial=wd->serial;m.speed=c;m.ok=ok;++m.generation;std::copy_n(v,NB,m.gain);
        } else {diffPendingNow=true;++softHits;if(slot>=softCursor&&!softDeferred){softDeferred=true;softNext=slot;}}
        if(m.serial!=wd->serial||m.speed!=c||!m.ok)return false;
        point=m.point;for(int band=0;band<NB;++band)amp[band]*=m.gain[band];return true;
    }
    std::vector<DiffMemo> diffMemo;
    static constexpr int kDiffHold = 6;                 // periods (16 ms at 48 kHz / 128)
    DiffractionScratch quickScratch;                    // one-end stage (nodes only)
    DiffractionSearch quickSearch, jobSearch;           // jobSearch uses diffractionScratch
    int jobSlot = -1;
    uint64_t jobMisses = 0, jobSince = 0;
    uint64_t diffPeriod = 0, diffJumpEpoch = 0;
    int64_t diffBudget = 0;                             // work units per control period for the multi-end search
    mutable int64_t softEdgeUnitsNow = 0;
    int64_t diffUnitsNow = 0;                           // all diffraction work in the current period (one-end, follow, search)
    bool diffPendingNow = false;
    Engine::DiffractionStats dstats;
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
    // E4a: the audio thread copies every target it accepts into lateGlocal (lateGtarget points there).
    std::vector<float> lateGlocal, lateGjump0;
    std::array<uint64_t, 2> lateGepoch{{0, 0}}, lateGjumpSerial{{0, 0}};    // normalization epoch / jump tag of each lateGbuf
    int lateGjumpLeft = 0, lateGjumpTotal = 0;
    uint64_t audioLastJumpTag = 0;
    std::atomic<uint64_t> lateGseenPub{0};         // lateGseen, for the worker
    int gainMode = 0;                              // 0: the pre-E4a easing; 1: motion easing (slew-limited, ends exactly);
                                                   // 2: accept exact settle target; glide0 is the captured old-bank gain
    std::vector<float> glide0;
    int glideN = 1, glideAt = 0;
    std::vector<int> glideSteps;
    std::vector<float> feedA0,feedAd,feedB0,feedBd;
    uint64_t handoverStart = 0;
    bool handoverLegacy = false;
    bool jumpHandoverAbort = false;
    bool motionExact = false;                      // the accepted motion target is the exact settle-pose gain
    float slewUp = 1.f, slewDn = 1.f;
    std::atomic<uint64_t> streamPos{0};            // samples processed (audio writes, worker reads)
    std::atomic<uint64_t> audioEpoch{0};           // normalization epoch of the gains the audio thread applies
    std::atomic<uint64_t> jumpSerial{0};
    uint64_t audioJumpSeen = 0, workerJumpSeen = 0;
    // motion targets: triple buffer, worker -> audio
    std::array<std::vector<float>, 3> mBuf;
    std::array<uint64_t, 3> mEpoch{{0, 0, 0}}, mJumpSerial{{0, 0, 0}};
    std::array<bool, 3> mExact{{false, false, false}};
    std::atomic<int> mMid{1};
    int mBack = 0, mFront = 2;
    // settle transition, worker -> audio (worker writes only while transState == 0)
    struct Transition {
        std::vector<int> slotX, slotH;   // [R] IR of the new pose in the old normalization / exactly as a static engine
        std::vector<float> rho;          // [R] correlation of the old IR and slotX
        std::vector<float> gNew;         // [R*S*8] gains of the new pose, exactly as a static engine
        std::vector<float> cmul;         // [R*8] old-normalization gain -> new-normalization gain
        int fadeBlocks = 1;
        uint64_t epoch = 0;
    } trans;
    std::atomic<int> transState{0};      // 0 idle, 1 published, 2 crossfading, 3 normalization handover
    // worker-side motion state
    struct MotionRef {
        uint64_t epoch = 0;
        std::vector<double> E;           // [(s*R + r)*NB + b] late energy of each source (srcHist sums)
        std::vector<double> N;           // [r*NB + b] band energy of the normalized common IR
        std::vector<float> G;            // [R*S*8] the gains published with it
        double lst[4] = {0, 0, 0, 0};
        std::vector<std::array<double, 4>> pose;
        bool recValid = false;
        std::vector<std::array<double, NB>> rec, recSq;  // reciprocal trace at the reference pose
    } mref;
    uint64_t epochCounter = 0;
    mutable std::mutex epochMx;
    std::vector<std::pair<uint64_t, std::vector<double>>> epochN;  // recent epochs and their N
    std::vector<double> anchor, lastEstPose;
    bool anchorValid = false, pubIsRef = true;
    uint64_t anchorStream = 0, lastEstStream = 0, lastEstEpoch = ~0ull;
    std::chrono::steady_clock::time_point anchorWall{}, lastEstWall{}, lastLegacy{};
    std::vector<float> prevPub;                    // last published motion gains, in mref normalization
    std::atomic<unsigned long long> stLegacySwaps{0}, stTransStarted{0}, stTransDone{0}, stEstimates{0}, stHeld{0}, stPublished{0}, stAccepted{0}, stCancelled{0};
    std::atomic<double> stLastEstMs{0}, stMaxEstMs{0}, stLastRetraceMs{0}, stMaxRetraceMs{0};
    std::atomic<long long> stLastWork{0};
    std::array<std::atomic<double>, 4> stRefLst{};
    std::vector<float> lateY;                      // [R][ctrl] scratch
    std::vector<TailHist> shapedHist;
    std::vector<double> sourceSeconds;
    std::vector<double> sourcePhysicalSeconds;
    std::vector<TailHist> srcHist;                 // worker: cached late-field histogram per source
    std::vector<int> histRays;
    std::vector<bool> histSparse;
    mutable std::mutex diagnosticsMx;
    std::shared_ptr<const TailDiagnostics> diagnostics;
    std::shared_ptr<const AutoLevelEnergy> autoEnergy;
    std::vector<AutoLevelEnergy::Point> autoPoints;
    std::atomic<uint64_t> autoMovingUntil{0};

    void updateAutoPoint(int s, const Scene& scn, Transducer source, const std::vector<Transducer>& rc, const TailHist& h) {
        if (autoPoints.size() != (size_t)S) autoPoints.resize(S);
        auto& p = autoPoints[s]; p = {}; source.gainDb = 0;
        for (size_t r=0; r<rc.size(); ++r) {
            const auto ef = earlyFieldEnergy(scn, source, rc[r].pos, 0);
            for (int b=0;b<NB;++b) {
                p.direct[b] += ef.direct[b]/rc.size(); p.early[b] += ef.early[b]/rc.size();
                for(double v:h.h[r][b]) p.late[b] += v/rc.size();
            }
        }
    }
    void publishAutoEnergy() {
        auto p = std::make_shared<AutoLevelEnergy>(); p->points=autoPoints;
        std::atomic_store(&autoEnergy, std::shared_ptr<const AutoLevelEnergy>(p));
    }
    std::atomic<double> poseScaleReady{1.};
    std::atomic<double> requestedPredelay{0}, requestedWidth{100}, requestedDuck{0}, requestedRelease{300};
    std::atomic<bool> requestedFreeze{false};
    std::atomic<float> publishedDuck{0};
    std::array<std::atomic<double>,7> freezeRt60{};
    std::atomic<uint64_t> freezeRtRevision{0};uint64_t freezeRtSeen=~0ull;
    WetPredelay wetPredelay; TailFreeze tailFreeze; WetDuck wetDuck;
    std::atomic<bool> freezeAvailable{true};
    std::vector<std::pair<int,int>> widthPairs;
    std::vector<float> creativeTail, creativeDirect,creativeFrame,creativeHeld;
    float widthCurrent=1;
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
    double lastRawListener[4] = {1e30, 0, 0, 0};
    SurfaceSideHist audioLstHist, workerLstHist;
    std::vector<SurfaceSideHist> audioSrcHist, workerSrcHist;
    uint64_t workerHistJumpSeen = 0;
    std::vector<double> rawAnchor;
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
        roomTailLength = sc.length;
        if (cfg.tailBlock % cfg.ctrlInterval) throw std::runtime_error("tailBlock must be a multiple of ctrlInterval");
        maxWalls = cfg.erOrder <= 1 ? 128 : cfg.erOrder == 2 ? 64 : cfg.erOrder == 3 ? 24 : 12;
        diffractionScratch.prepare((size_t)maxWalls);
        quickScratch.prepareNodes((size_t)maxWalls);
        diffBudget = cfg.diffractionUnits > 0 ? cfg.diffractionUnits : kDefaultDiffractionUnits;
        dstats.budget = diffBudget;
        if ((int)scene.walls.size() > maxWalls) throw std::runtime_error("too many walls for this reflection order (max " + std::to_string(maxWalls) + ")");
        B = cfg.tailBlock;
        latest = std::make_shared<World>(sc, maxWalls, cfg.absorptionScale,scene.length);
        analysisScene = std::shared_ptr<const Scene>(latest, &latest->sc);
        latest->serial = ++serialCounter;
        worlds.push_back(latest);
        wd = latest.get();audioWorld.store(wd,std::memory_order_relaxed);
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
        wetPredelay.prepare(sr,R);tailFreeze.prepare(sr,R,cfg.ctrlInterval);wetDuck.prepare(sr);
        {auto rt=estimateRoomRT60(sc);tailFreeze.setRoomDecay(rt);for(int b=0;b<7;++b)freezeRt60[b]=rt[b];}
        creativeTail.assign((size_t)R*cfg.ctrlInterval,0);creativeDirect=creativeTail;creativeHeld=creativeTail;creativeFrame.assign((size_t)R,0);
        if(R==2)widthPairs.push_back({0,1});
        else for(int a=0;a<R;++a)for(int b=a+1;b<R;++b) {
            const auto& x=rcvLocal[a];const auto& y=rcvLocal[b];
            if(x.gainDb<=-100||y.gainDb<=-100)continue;
            const double az=std::remainder(x.azimuthDeg+y.azimuthDeg,360.);
            if(std::abs(az)<.01&&std::abs(std::sin(x.azimuthDeg*kPi/180))>.001&&std::abs(x.elevationDeg-y.elevationDeg)<.01)widthPairs.push_back({a,b});
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

        audioSrcHist.assign((size_t)S, SurfaceSideHist{});
        workerSrcHist.assign((size_t)S, SurfaceSideHist{});
        audioLstHist.reset(latest->serial);
        workerLstHist.reset(latest->serial);
        double effL0[4] = {l0.x, l0.y, l0.z, yaw0};
        resolvePose4(G(), latest->serial, workerLstHist, effL0, effL0);
        audioLstHist = workerLstHist;
        std::vector<double> effSrc0((size_t)S * 4, 0.0);
        bool initAdjusted = (effL0[0] != l0.x || effL0[1] != l0.y || effL0[2] != l0.z);
        for (int s = 0; s < S; ++s) {
            double rawS[4] = {sc.sources[s].pos.x, sc.sources[s].pos.y, sc.sources[s].pos.z, sc.sources[s].azimuthDeg};
            double effS[4];
            resolvePose4(G(), latest->serial, workerSrcHist[(size_t)s], rawS, effS);
            audioSrcHist[(size_t)s] = workerSrcHist[(size_t)s];
            for (int k = 0; k < 4; ++k) effSrc0[(size_t)s * 4 + k] = effS[k];
            initAdjusted |= (effS[0] != rawS[0] || effS[1] != rawS[1] || effS[2] != rawS[2]);
        }
        std::vector<Transducer> initRc;
        resolvedReceivers(G(), effL0, initRc, true);
        for (int r = 0; r < R; ++r) {
            const Vec3& rp = sc.receivers[(size_t)r].pos;
            initAdjusted |= (initRc[(size_t)r].pos.x != rp.x || initRc[(size_t)r].pos.y != rp.y || initRc[(size_t)r].pos.z != rp.z);
        }
        if (initAdjusted) {
            std::vector<std::array<double, 4>> probeSrcs((size_t)S);
            for (int s = 0; s < S; ++s) for (int k = 0; k < 4; ++k) probeSrcs[(size_t)s][(size_t)k] = effSrc0[(size_t)s * 4 + k];
            sc.length = cachedTailSeconds(buildLengthProbe(*latest, effL0, initRc, &probeSrcs), cfg.erOrder, cfg.absorptionScale, cfg.cancelled);
            latest->sc.length = sc.length;
            roomTailLength = sc.length;
        }

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
            t.freshRe.assign(t.fdlRe.size(),0.f);t.freshIm.assign(t.fdlIm.size(),0.f);
            t.freshPrev.assign(B,0.f);t.freshIn.assign(B,0.f);
            if (!shared) {
                t.cleanRe.assign(t.fdlRe.size(), 0.f); t.cleanIm.assign(t.fdlIm.size(), 0.f);
                t.cleanPrev.assign(B, 0.f); t.cleanIn.assign(B, 0.f);
                t.cleanSynced.store(P, std::memory_order_relaxed);
            }
            for (auto& sl : t.slot) { sl.partitions = initialP; sl.re.assign((size_t)channels * initialP * t.bins, 0.f); sl.im.assign((size_t)channels * initialP * t.bins, 0.f); sl.isJump = false; }
        };
        for (int s = 0; s < S; ++s) {
            auto sp = std::make_unique<Src>();
            sp->ring.assign(frames * 8, 0.f);
            sp->split.init(sr);
            sp->t = sc.sources[s];
            sp->t.pos = {effSrc0[(size_t)s * 4 + 0], effSrc0[(size_t)s * 4 + 1], effSrc0[(size_t)s * 4 + 2]};
            if (!shared) initConv(sp->tail, R);
            srcs.push_back(std::move(sp));
        }
        if (shared) {
            for (int r = 0; r < R; ++r) {
                sconv.push_back(std::make_unique<TailConv>()); auto& t=*sconv.back(); initConv(t, 1);
            }
            lateGbuf[0].assign((size_t)R * S * 8, 0.f);
            lateGbuf[1].assign((size_t)R * S * 8, 0.f);
            lateGuse.assign((size_t)R * S * 8, 0.f);
            lateGlocal.assign((size_t)R * S * 8, 0.f);
            lateGjump0.assign((size_t)R * S * 8, 0.f);
            glide0.assign((size_t)R * S * 8, 0.f);
            glideSteps.assign((size_t)R * S * 8,1);
            feedA0.resize(lateGuse.size());feedAd.resize(lateGuse.size());feedB0.resize(lateGuse.size());feedBd.resize(lateGuse.size());
            glideN = std::max(1, (int)std::lround(cfg.motionFadeSec * sr / cfg.ctrlInterval));
            for (auto& b : mBuf) b.assign((size_t)R * S * 8, 0.f);
            lateY.assign((size_t)R * cfg.ctrlInterval, 0.f);
            const double perPeriod = std::max(0.1, cfg.motionSlewDbPerSec) * cfg.ctrlInterval / sr;
            slewUp = (float)std::pow(10.0, perPeriod / 20.0); slewDn = 1.f / slewUp;
            trans.slotX.assign((size_t)R, 0); trans.slotH.assign((size_t)R, 0); trans.rho.assign((size_t)R, 0.f);
            trans.gNew.assign((size_t)R * S * 8, 0.f); trans.cmul.assign((size_t)R * 8, 1.f);
        }
        srcHist.resize((size_t)S); shapedHist.resize((size_t)S); sourceSeconds.assign((size_t)S,0.);
        sourcePhysicalSeconds.assign(S,sc.length);
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
        diffMemo.assign((size_t)S * (size_t)RM * 2, DiffMemo{});
        softStride=maxWalls+2+kMaxPlates;
        softMemo.resize((size_t)S*RM*softStride*4);
        ampMemo.resize((size_t)S*RM*softStride);
        pathPeakGain.resize((size_t)S*RM);
        for(int source=0;source<S;++source)for(int receiver=0;receiver<RM;++receiver)pathPeakGain[(size_t)source*RM+receiver]=std::pow(10.,(sc.sources[source].gainDb+sc.receivers[(size_t)(receiver/M)].gainDb)/20.);
        // order 1: at most direct + one reflection per surface, doubled for the fading copies of moving paths
        const size_t tapCap = cfg.erOrder <= 1 ? (size_t)(2 * (maxWalls + 4 + kMaxPlates)) : 1024;
        for (auto& l : links) l.reserve(tapCap);

        // initial late field (synchronous), then the optional refresher
        for (int s = 0; s < S; ++s) {
            for (int k = 0; k < 4; ++k) srcs[s]->lastPose[k] = effSrc0[(size_t)s * 4 + k];
            for (int k = 0; k < 4; ++k) srcs[s]->lastLst[k] = effL0[k];
            srcs[s]->lastGeom = geomSerial.load();
            srcs[s]->hist.assign(kHist, srcs[s]->t.pos);
            if (!shared) {
                Transducer st = srcs[s]->t;
                buildTail(s, latest->sc, st, initRc, 0, latest->serial);
                srcs[s]->lastPattern = st.pattern;
                srcs[s]->tail.cur = 0;
                srcs[s]->tail.curAtomic = 0;
            }
        }
        if (shared) sharedRebuild(true);
        if (shared) for (auto& c : sconv) c->appliedScene = c->slot[0].sceneRevision;
        else for (auto& s : srcs) s->tail.appliedScene = s->tail.slot[0].sceneRevision;
        placeReceivers(effL0, std::min(std::max(params[S * 4 + 4].load(), 0.0), 3.0));
        presolveDiffraction();
        // E5b exact initial early transfers are prepared off the audio thread.
        for(int source=0;source<S;++source)for(int receiver=0;receiver<RM;++receiver)for(int surf=0;surf<G().nSurf();++surf){
            if(surf==G().ceilId()&&!G().hasCeil)continue;
            Vec3 P;float a[NB];std::fill_n(a,NB,1.f);
            const auto sp=srcs[source]->t.pos,rp=rcvAbs[receiver].pos;
            if(cachedSupport(source,receiver,surf,rp,mirror(G(),sp,surf),P,a,true)){
                cachedSoftLeg(source,receiver,surf,0,rp,P,a,true,sp);
                cachedSoftLeg(source,receiver,surf,1,P,sp,a,true);
            }
        }
        warmingSoft=true;
        for(int source=0;source<S;++source){buildImages(srcs[source]->t.pos);for(int receiver=0;receiver<RM;++receiver)for(size_t idx=1;idx<nodes.size();++idx)if(nodes[idx].order==1){Found f;validate((int)idx,*srcs[source],rcvAbs[receiver],srcs[source]->t,f,source,receiver);}}
        warmingSoft=false;
        softSpent=0;softEvals=softHits=0;softProfile={};softCursor=0;softDeferred=false;
        cfg.cancelled = {};
        lastLegacy = std::chrono::steady_clock::now();
        if (cfg.tailThread && !cfg.manualWorker) worker = std::thread([this] { if (shared) workerLoopShared(); else workerLoop(); });
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
        for(int b=0;b<NB;++b)if(d->rt60[b]>0)freezeRt60[b].store(d->rt60[b],std::memory_order_relaxed);
        freezeRtRevision.fetch_add(1,std::memory_order_release);
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

    void buildTail(int s, const Scene& scn, const Transducer& st, const std::vector<Transducer>& rc, int slotIdx, uint64_t sceneRevision, const std::function<bool()>& extraCancel = {}) {
        TailConv& t = srcs[s]->tail;
        bool sparse = false;
        const auto cancelled = [&]{return !running || geomSerial.load()!=sceneRevision || qualitySerial.load()!=workerQuality || (cfg.cancelled && cfg.cancelled()) || (extraCancel && extraCancel());};
        TailHist h = traceTailAdaptive(scn, st, rc, cfg.erOrder, cfg.tailRays, rayLimit(cfg.tailRays), scn.seed, histRays[s], &sparse, cancelled);
        histSparse[s] = sparse;
        srcHist[s] = h;
        srcs[s]->lastQuality = workerQuality;
        reshapeTailDecay(h, cfg.decayMultipliers);
        shapedHist[s] = h;
        updateAutoPoint(s,scn,st,rc,h);
        publishAutoEnergy();
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
        sourcePhysicalSeconds[s]=scn.length;
        publishSourceDiagnostics();
        ++refreshCount;
    }

    static void resolvePose4(const Geo& g, uint64_t worldSerial, SurfaceSideHist& hist, const double raw[4], double out[4]) {
        const Vec3 p = resolveWallLinePose(g, {raw[0], raw[1], raw[2]}, hist, worldSerial);
        out[0] = p.x; out[1] = p.y; out[2] = p.z; out[3] = raw[3];
    }
    void resolvedReceivers(const Geo& g, const double* effLst, std::vector<Transducer>& out, bool exactYaw0 = false) const {
        listenerReceivers(effLst, out);
        if (exactYaw0 && effLst[3] == yaw0) {
            for (int r = 0; r < R; ++r)
                out[(size_t)r].pos = sc.receivers[(size_t)r].pos + Vec3{effLst[0] - l0.x, effLst[1] - l0.y, effLst[2] - l0.z};
        }
        containCapsulesInRoom(g, {effLst[0], effLst[1], effLst[2]}, out.data(), R);
    }
    std::vector<double> workerResolvedPose(const World& w) {
        std::vector<double> P((size_t)(S + 1) * 4);
        double raw[4], eff[4];
        for (int i = 0; i < 4; ++i) raw[i] = params[S * 4 + i].load();
        resolvePose4(w.geo, w.serial, workerLstHist, raw, eff);
        for (int i = 0; i < 4; ++i) P[(size_t)i] = eff[i];
        for (int s = 0; s < S; ++s) {
            for (int i = 0; i < 4; ++i) raw[i] = params[s * 4 + i].load();
            resolvePose4(w.geo, w.serial, workerSrcHist[(size_t)s], raw, eff);
            for (int i = 0; i < 4; ++i) P[(size_t)(4 + 4 * s + i)] = eff[i];
        }
        return P;
    }

    Scene buildLengthProbe(const World& world,const double* listener,const std::vector<Transducer>& receivers,const std::vector<std::array<double, 4>>* effSrcs = nullptr) const {
        Scene probe=world.lengthProbe;
        Vec3 origin=probe.listenerPos;
        if(!probe.hasListener){origin={};for(const auto& r:probe.receivers)origin=origin+r.pos;origin=origin*(1./probe.receivers.size());}
        if(listener[3]==probe.listenerYaw) {
            const Vec3 delta{listener[0]-origin.x,listener[1]-origin.y,listener[2]-origin.z};
            bool capsSame = probe.receivers.size() == receivers.size();
            for(size_t r=0;capsSame && r<receivers.size();++r) {
                const Vec3 d = probe.receivers[r].pos + delta - receivers[r].pos;
                capsSame &= (std::fabs(d.x) <= 1e-6 && std::fabs(d.y) <= 1e-6 && std::fabs(d.z) <= 1e-6);
            }
            if (capsSame) for(auto& r:probe.receivers)r.pos=r.pos+delta;
            else probe.receivers=receivers;
        } else probe.receivers=receivers;
        probe.hasListener=true;probe.listenerPos={listener[0],listener[1],listener[2]};probe.listenerYaw=listener[3];
        for(int s=0;s<S;++s){
            if (effSrcs) {
                probe.sources[s].pos={(*effSrcs)[(size_t)s][0],(*effSrcs)[(size_t)s][1],(*effSrcs)[(size_t)s][2]};
                probe.sources[s].azimuthDeg=(*effSrcs)[(size_t)s][3];
            } else {
                probe.sources[s].pos={params[s*4].load(),params[s*4+1].load(),params[s*4+2].load()};
                probe.sources[s].azimuthDeg=params[s*4+3].load();
            }
            probe.sources[s].pattern=params[S*4+8+s].load();
        }
        return probe;
    }
    double settledTailLength(const World& world,const double* listener,const std::vector<Transducer>& receivers,const std::function<bool()>& cancel,const std::vector<std::array<double, 4>>* effSrcs = nullptr) {
        return cachedTailSeconds(buildLengthProbe(world,listener,receivers,effSrcs),cfg.erOrder,world.tailAbsorption,cancel);
    }
    double jumpTailLength(const World& world,const double* listener,const std::vector<Transducer>& receivers,bool& cacheHit,const std::vector<std::array<double, 4>>* effSrcs = nullptr) {
        double cached = world.sc.length;
        if (tryCachedTailSeconds(buildLengthProbe(world, listener, receivers, effSrcs), cfg.erOrder, world.tailAbsorption, cached)) {
            cacheHit = true;
            return cached;
        }
        cacheHit = false;
        return std::min(std::max(std::max(world.sc.length, roomTailLength.load(std::memory_order_relaxed)), 0.2), 20.0);
    }
    void publishTailLength(const std::shared_ptr<World>& old,double length) {
        if(length==old->sc.length)return;
        Scene modeled=old->sc;modeled.length=length;modeled.clutter.count=0;modeled.roughness=0;
        auto updated=std::make_shared<World>(modeled,maxWalls,1.);
        updated->lengthProbe=old->lengthProbe;updated->tailAbsorption=old->tailAbsorption;
        updated->serial=old->serial;updated->constructionPoseScale=old->constructionPoseScale;
        std::lock_guard<std::mutex> lock(worldMx);
        if(latest!=old)return; // a newer geometry request owns the publication
        worlds.push_back(updated);latest=updated;
        std::atomic_store(&analysisScene,std::shared_ptr<const Scene>(updated,&updated->sc));
    }

    // ---- shared late field (worker, or the constructor with initial = true) ----
    // Re-traces the sources that moved, rebuilds the common per-receiver tail IRs from the average energy
    // envelope and publishes the per-source/per-receiver/per-band gains. Returns false if nothing changed.
    // E4a: with `settle` (the pose snapshot [listener 4][source 4 each] a move came to rest at) it re-traces every source
    // whose pose differs from the one its histogram was traced at (all of them when the listener moved) on copies, so a
    // cancelled re-trace changes nothing, and publishes a Transition (long crossfade, then the normalization handover)
    // instead of the short swap. The IR and gains it ends with are computed exactly as the constructor computes them.
    bool sharedRebuild(bool initial, const std::vector<double>* settle = nullptr, const std::function<bool()>& settleCancel = {}, bool jumpRebuild = false, uint64_t jumpTag = 0) {
        std::shared_ptr<World> wsnap;
        {
            std::lock_guard<std::mutex> lk(worldMx);
            wsnap = latest;
        }
        if(!initial&&poseScaleReady.load(std::memory_order_acquire)!=wsnap->constructionPoseScale)return false;
        Scene scn = wsnap->sc;
        double lst[4];
        {
            double rawLst[4];
            for (int i = 0; i < 4; ++i) rawLst[i] = settle ? (*settle)[(size_t)i] : params[S * 4 + i].load();
            resolvePose4(wsnap->geo, wsnap->serial, workerLstHist, rawLst, lst);
        }
        std::vector<Transducer> rc;
        resolvedReceivers(wsnap->geo, lst, rc, false);
        std::vector<std::array<double, 4>> poses((size_t)S);
        for (int s = 0; s < S; ++s) {
            double rawS[4], effS[4];
            for (int i = 0; i < 4; ++i) rawS[i] = settle ? (*settle)[(size_t)(4 + 4 * s + i)] : params[s * 4 + i].load();
            resolvePose4(wsnap->geo, wsnap->serial, workerSrcHist[(size_t)s], rawS, effS);
            for (int i = 0; i < 4; ++i) poses[(size_t)s][(size_t)i] = effS[i];
        }
        if (settle) {
            scn.length = settledTailLength(*wsnap, lst, rc, [&]{return !running||geomSerial.load()!=wsnap->serial||qualitySerial.load()!=workerQuality||(settleCancel&&settleCancel())||(cfg.cancelled&&cfg.cancelled());}, &poses);
            sharedLengthCheck = false;
        } else if (jumpRebuild) {
            bool cacheHit = false;
            scn.length = jumpTailLength(*wsnap, lst, rc, cacheHit, &poses);
            sharedLengthCheck = !cacheHit;
        }
        const bool durationChanged=scn.length!=wsnap->sc.length;
        const double dl = std::hypot(std::hypot(lst[0] - sLastLst[0], lst[1] - sLastLst[1]), lst[2] - sLastLst[2]);
        const double dYawL = std::fabs(std::remainder(lst[3] - sLastLst[3], 360.0));
        const int tsStart = transState.load(std::memory_order_acquire);
        const bool all = settle ? (lst[0] != sLastLst[0] || lst[1] != sLastLst[1] || lst[2] != sLastLst[2] || lst[3] != sLastLst[3])
                                : (jumpRebuild ? (initial || tsStart != 0 || !pubIsRef || wsnap->serial != sLastGeom || workerQuality != sLastQuality || dl > 0.001 || dYawL > 0.1)
                                               : (initial || wsnap->serial != sLastGeom || workerQuality != sLastQuality || dl > 0.05 || std::fabs(lst[3] - sLastLst[3]) > 2));
        const int rays = std::max(1500, cfg.tailRays / std::max(1, S));
        std::vector<int> dirty;
        for (int s = 0; s < S; ++s) {
            Src& sp = *srcs[(size_t)s];
            const auto& p = poses[(size_t)s];
            if (settle) {
                if (all || p[0] != sp.lastPose[0] || p[1] != sp.lastPose[1] || p[2] != sp.lastPose[2] || p[3] != sp.lastPose[3]) dirty.push_back(s);
                continue;
            }
            const double dp = std::hypot(std::hypot(p[0] - sp.lastPose[0], p[1] - sp.lastPose[1]), p[2] - sp.lastPose[2]);
            const double dYawS = std::fabs(std::remainder(p[3] - sp.lastPose[3], 360.0));
            if (jumpRebuild ? (all || dp > 0.001 || dYawS > 0.1 || params[S * 4 + 8 + s].load() != sp.lastPattern)
                            : (all || dp > 0.05 || std::fabs(p[3] - sp.lastPose[3]) > 2 || params[S * 4 + 8 + s].load() != sp.lastPattern)) dirty.push_back(s);
        }
        if (durationChanged) { dirty.clear(); for (int s = 0; s < S; ++s) dirty.push_back(s); }
        if (dirty.empty()) return false;
        sharedBusy.store(true,std::memory_order_release);
        // Settle and jump re-traces work on copies and commit only when complete.
        const bool useCopies = (settle != nullptr) || jumpRebuild;
        std::vector<TailHist> work;
        std::vector<int> workRays;
        if (useCopies) { work = srcHist; workRays = histRays; }
        std::vector<TailHist>& hist = useCopies ? work : srcHist;
        std::vector<int>& hRays = useCopies ? workRays : histRays;
        // Retain raw moments in the cache; assess confidence only after pooling.
        auto traceSource = [&](int s, int count) {
            Transducer st = srcs[(size_t)s]->t;
            st.pos = {poses[(size_t)s][0], poses[(size_t)s][1], poses[(size_t)s][2]};
            st.azimuthDeg = poses[(size_t)s][3];
            st.pattern = settle ? srcs[(size_t)s]->lastPattern : std::min(std::max(params[S * 4 + 8 + s].load(), 0.0), 1.0);
            hist[(size_t)s] = traceTail(scn, st, rc, cfg.erOrder, count, scn.seed + 131 * s, false, [&]{return !running || geomSerial.load()!=wsnap->serial || qualitySerial.load()!=workerQuality || (cfg.cancelled && cfg.cancelled()) || (settleCancel && settleCancel());});
            hRays[(size_t)s] = count;
            if (useCopies) return;
            srcs[(size_t)s]->lastPattern = st.pattern;
            for (int i = 0; i < 4; ++i) srcs[(size_t)s]->lastPose[i] = poses[(size_t)s][(size_t)i];
        };
        if(durationChanged){dirty.clear();for(int s=0;s<S;++s)dirty.push_back(s);}
        for (int s : dirty) traceSource(s, rays);
        const size_t nBins = hist[0].nBins;
        TailHist avg;
        std::vector<double> E((size_t)S * R * NB, 0.0), Eavg((size_t)R * NB, 0.0);
        bool sparse = false;
        int targetRays = rays;
        for (;;) {
            avg = TailHist{};
            avg.nBins = nBins; avg.binDur = hist[0].binDur;
            avg.h.resize(R); avg.h2.resize(R);
            for (int r = 0; r < R; ++r)
                for (int b = 0; b < NB; ++b) {
                    auto& dst = avg.h[r][b]; auto& dst2 = avg.h2[r][b];
                    dst.assign(nBins, 0.0); dst2.assign(nBins, 0.0);
                    for (int s = 0; s < S; ++s) {
                        const auto& h = hist[s].h[r][b]; const auto& h2 = hist[s].h2[r][b];
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
            for (int s = 0; s < S; ++s) if (hRays[s] < targetRays) {
                if (useCopies && std::find(dirty.begin(), dirty.end(), s) == dirty.end()) dirty.push_back(s);
                traceSource(s, targetRays);
            }
        }
        int usedRays = 0;
        for (int count : hRays) usedRays += count;
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
        std::vector<float> G((size_t)R * S * 8, 0.f);
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
        // E4a: the same IR expressed in the current normalization (band b of receiver r scaled by sqrt(Nold / Nnew)):
        // with the gains of the new pose in the old normalization it sounds exactly like the new IR with the new gains.
        std::vector<float> cmul((size_t)R * 8, 1.f);
        std::vector<std::vector<float>> irX;
        if (settle) {
            for (int r = 0; r < R; ++r)
                for (int b = 0; b < NB; ++b) {
                    const double o = mref.N[(size_t)r * NB + b], nn = Eavg[(size_t)r * NB + b];
                    cmul[(size_t)r * 8 + b] = (o > 0 && nn > 0) ? (float)std::sqrt(o / nn) : 1.f;
                }
            BandBuf bx = buf;
            for (int r = 0; r < R; ++r)
                for (int b = 0; b < NB; ++b) {
                    const double k = cmul[(size_t)r * 8 + b];
                    for (auto& v : bx[(size_t)r][(size_t)b]) v *= k;
                }
            irX = combineBands(bx, sr);
            fadeTailEnd(irX, N, sr);
        }
        auto ir = combineBands(buf, sr);
        fadeTailEnd(ir, N, sr);
        FFTf wf(2 * B);
        std::vector<cf> tmp(2 * B);
        auto writeSlot = [&](TailConv& t, Slot& sl, const std::vector<float>& x) {
            sl.samples=N;
            sl.revision = workerQuality; sl.sceneRevision = wsnap->serial;
            sl.partitions = std::min(t.P, (int)((N-1)/B));
            sl.re.resize((size_t)sl.partitions*t.bins); sl.im.resize(sl.re.size());
            for (int k = 0; k < sl.partitions; ++k) {
                for (int i = 0; i < 2 * B; ++i) tmp[(size_t)i] = {0, 0};
                for (int i = 0; i < B; ++i) {
                    const size_t idx = (size_t)(k + 1) * B + i;  // drop the first B samples: latency compensation
                    if (idx < N) tmp[(size_t)i].re = x[idx];
                }
                wf.run(tmp.data(), false);
                const size_t o = (size_t)k * t.bins;
                for (int b = 0; b < t.bins; ++b) { sl.re[o + (size_t)b] = tmp[(size_t)b].re; sl.im[o + (size_t)b] = tmp[(size_t)b].im; }
            }
        };
        const uint64_t epoch = ++epochCounter;
        std::vector<int> wslot((size_t)R);
        const int ts = transState.load(std::memory_order_acquire);
        for (int r = 0; r < R; ++r) {
            TailConv& t = *sconv[(size_t)r];
            int w = initial ? 0 : (t.curAtomic.load() == 0 ? 1 : 0);
            if (!initial && (ts == 2 || ts == 3)) w = trans.slotX[(size_t)r];  // handover: the third slot is free
            if (settle) {
                const int cur = t.curAtomic.load();
                trans.slotX[(size_t)r] = (cur + 1) % 3;
                trans.slotH[(size_t)r] = (cur + 2) % 3;
                std::fill(t.freshRe.begin(),t.freshRe.end(),0.f);std::fill(t.freshIm.begin(),t.freshIm.end(),0.f);
                std::fill(t.freshPrev.begin(),t.freshPrev.end(),0.f);std::fill(t.freshIn.begin(),t.freshIn.end(),0.f);
                writeSlot(t, t.slot[trans.slotX[(size_t)r]], irX[(size_t)r]);
                writeSlot(t, t.slot[trans.slotH[(size_t)r]], ir[(size_t)r]);
                handoverCorrelation(t,t.slot[cur],t.slot[trans.slotH[(size_t)r]]);
                // correlation of the old IR and the new one (white input), for the crossfade law
                const Slot& a = t.slot[cur]; const Slot& x = t.slot[trans.slotX[(size_t)r]];
                double ax = 0, aa = 0, xx = 0;
                for (int k = 0; k < std::max(a.partitions, x.partitions); ++k)
                    for (int b = 0; b < t.bins; ++b) {
                        const double wgt = (b == 0 || b == t.bins - 1) ? 1.0 : 2.0;
                        const size_t o = (size_t)k * t.bins + (size_t)b;
                        const bool ha = k < a.partitions, hx = k < x.partitions;
                        if (ha) aa += wgt * ((double)a.re[o] * a.re[o] + (double)a.im[o] * a.im[o]);
                        if (hx) xx += wgt * ((double)x.re[o] * x.re[o] + (double)x.im[o] * x.im[o]);
                        if (ha && hx) ax += wgt * ((double)a.re[o] * x.re[o] + (double)a.im[o] * x.im[o]);
                    }
                trans.rho[(size_t)r] = (aa > 0 && xx > 0) ? (float)std::min(1.0, std::max(0.0, ax / std::sqrt(aa * xx))) : 0.f;
                continue;
            }
            wslot[(size_t)r] = w;
            writeSlot(t, t.slot[w], ir[(size_t)r]);
        }
        if (useCopies) {
            if(!running||geomSerial.load()!=wsnap->serial||qualitySerial.load()!=workerQuality||(settleCancel&&settleCancel())||(cfg.cancelled&&cfg.cancelled())) {
                renderedSeconds.store(sconv.front()->slot[sconv.front()->curAtomic.load()].samples/sr);sharedBusy=false;return false;
            }
        }
        if (settle) {
            // the gains of the new pose in the old normalization, published first (exact), then the transition
            std::vector<float> Gp((size_t)R * S * 8, 0.f);
            for (int r = 0; r < R; ++r)
                for (int s = 0; s < S; ++s)
                    for (int b = 0; b < NB; ++b) {
                        const double o = mref.N[(size_t)r * NB + b];
                        Gp[((size_t)r * S + s) * 8 + b] = o > 0 ? (float)std::sqrt(E[((size_t)s * R + r) * NB + b] / o) : 0.f;
                    }
            publishMotion(Gp, true, mref.epoch);
            trans.gNew = G; trans.cmul = cmul; trans.epoch = epoch;
            trans.fadeBlocks = std::max(1, (int)std::lround(cfg.motionFadeSec * sr / B));
            lateGjumpSerial[(size_t)inactive] = 0;
            lateGbuf[(size_t)inactive] = G;  // what copyTailForOffline copies (no serial: the audio takes it from the transition)
            lateGepoch[(size_t)inactive] = epoch;
            lateGcur.store(inactive);
            transState.store(1, std::memory_order_release);
            srcHist.swap(work);
            histRays.swap(workRays);
            for (int s : dirty)
                for (int i = 0; i < 4; ++i) srcs[(size_t)s]->lastPose[i] = poses[(size_t)s][(size_t)i];
        } else {
            lateGjumpSerial[(size_t)inactive] = jumpRebuild ? jumpTag : 0;
            lateGbuf[(size_t)inactive] = G;
            lateGepoch[(size_t)inactive] = epoch;
            lateGcur.store(inactive);
            ++lateGserial;
            if (initial) {
                lateGuse = G;  // nothing is playing yet: no easing
                lateGlocal = G;
                lateGtarget = lateGlocal.data();
                lateGseen = lateGserial.load();
                lateGseenPub.store(lateGseen);
                audioEpoch.store(epoch);
            } else {
                for (int r = 0; r < R; ++r) sconv[(size_t)r]->pending.store(wslot[(size_t)r]);
            }
            if (jumpRebuild) {
                srcHist.swap(work);
                histRays.swap(workRays);
                for (int s : dirty) {
                    srcs[(size_t)s]->lastPattern = std::min(std::max(params[S * 4 + 8 + s].load(), 0.0), 1.0);
                    for (int i = 0; i < 4; ++i) srcs[(size_t)s]->lastPose[i] = poses[(size_t)s][(size_t)i];
                }
            }
        }
        // the reference of the motion estimates: the poses, energies and normalization of this build
        mref.epoch = epoch; mref.E = E; mref.N = Eavg; mref.G = G;
        for (int i = 0; i < 4; ++i) mref.lst[i] = lst[i];
        mref.pose.resize((size_t)S);
        for (int s = 0; s < S; ++s) for (int i = 0; i < 4; ++i) mref.pose[(size_t)s][(size_t)i] = srcs[(size_t)s]->lastPose[i];
        mref.recValid = false;
        prevPub = G; pubIsRef = true;
        {
            std::lock_guard<std::mutex> lk(epochMx);
            epochN.push_back({epoch, Eavg});
            if (epochN.size() > 6) epochN.erase(epochN.begin());
        }
        for (int i = 0; i < 4; ++i) { sLastLst[i] = lst[i]; stRefLst[(size_t)i].store(lst[i]); }
        sLastGeom = wsnap->serial;
        sLastQuality = workerQuality;
        for(int s=0;s<S;++s) {
            auto source=scn.sources[s]; source.pos={poses[s][0],poses[s][1],poses[s][2]}; source.azimuthDeg=poses[s][3];
            source.pattern=srcs[s]->lastPattern;
            updateAutoPoint(s,scn,source,rc,srcHist[s]);
        }
        publishAutoEnergy();
        if(settle||jumpRebuild)publishTailLength(wsnap,scn.length);
        sharedBusy.store(false,std::memory_order_release);
        ++refreshCount;
        return true;
    }

    // E4a: motion targets to the audio thread. G is in the normalization of mref; it is converted to the epoch the audio
    // thread is in (or `epoch`), whose IR the gains feed. Worker only.
    bool publishMotion(const std::vector<float>& G, bool exact, uint64_t epoch = 0) {
        const uint64_t te = epoch ? epoch : audioEpoch.load(std::memory_order_acquire);
        std::vector<float>& dst = mBuf[(size_t)mBack];
        if (te == mref.epoch) dst = G;
        else {
            std::vector<double> Nt;
            {
                std::lock_guard<std::mutex> lk(epochMx);
                for (const auto& e : epochN) if (e.first == te) Nt = e.second;
            }
            if (Nt.empty()) return false;
            for (int r = 0; r < R; ++r)
                for (int s = 0; s < S; ++s)
                    for (int b = 0; b < 8; ++b) {
                        const size_t i = ((size_t)r * S + s) * 8 + b;
                        const double nt = b < NB ? Nt[(size_t)r * NB + b] : 0.0, nm = b < NB ? mref.N[(size_t)r * NB + b] : 0.0;
                        dst[i] = (nt > 0 && nm > 0) ? (float)(G[i] * std::sqrt(nm / nt)) : 0.f;
                    }
        }
        mEpoch[(size_t)mBack] = te;
        mJumpSerial[(size_t)mBack] = workerJumpSeen;
        mExact[(size_t)mBack] = exact;
        mBack = mMid.exchange(mBack | 4, std::memory_order_acq_rel) & 3;
        ++stPublished;
        return true;
    }

    static bool poseFar(const double* a, const double* b, double dm, double ddeg) {
        return std::hypot(std::hypot(a[0] - b[0], a[1] - b[1]), a[2] - b[2]) > dm || std::fabs(std::remainder(a[3] - b[3], 360.0)) > ddeg;
    }
    bool posesFar(const std::vector<double>& a, const std::vector<double>& b, double dm, double ddeg) const {
        for (int k = 0; k <= S; ++k) if (poseFar(&a[(size_t)4 * k], &b[(size_t)4 * k], dm, ddeg)) return true;
        return false;
    }
    std::vector<double> livePose() const {
        std::vector<double> P((size_t)(S + 1) * 4);
        for (int i = 0; i < 4; ++i) P[(size_t)i] = params[S * 4 + i].load();
        for (int s = 0; s < S; ++s) for (int i = 0; i < 4; ++i) P[(size_t)(4 + 4 * s + i)] = params[s * 4 + i].load();
        return P;
    }
    std::vector<double> refPose() const {
        std::vector<double> P((size_t)(S + 1) * 4);
        for (int i = 0; i < 4; ++i) P[(size_t)i] = sLastLst[i];
        for (int s = 0; s < S; ++s) for (int i = 0; i < 4; ++i) P[(size_t)(4 + 4 * s + i)] = srcs[(size_t)s]->lastPose[i];
        return P;
    }

    // E4a motion estimate of the late gains at pose P (worker). Per source, receiver and band the late energy relative to
    // the reference build is fused in the log domain from (a) the prior "no change" (standard deviation 6 dB per metre
    // moved), (b) a forward trace of motionForwardRays rays with per-ray random streams (absolute, against the
    // reference's full trace) and (c) one reciprocal trace from the listener to all sources (ratio to the same trace at
    // the reference), each weighted by its inverse variance from the per-ray sums of squares. A value whose measured
    // variance says it is unreliable (standard deviation above motionHoldDb) keeps the previous gain.
    void motionEstimate(const std::vector<double>& P, const World& w) {
        const auto t0 = std::chrono::steady_clock::now();
        const Scene& scn = w.sc;
        long long work = 0, wk = 0;
        const auto cancel = [&]{ return !running || geomSerial.load() != w.serial || qualitySerial.load() != workerQuality || jumpSerial.load(std::memory_order_acquire) != workerJumpSeen; };
        const double stopDb = -20, dm = cfg.motionDeadbandM, dd = cfg.motionDeadbandDeg;
        const int KF = std::max(8, cfg.motionForwardRays), KR = std::max(64, cfg.motionReciprocalRays);
        auto srcAt = [&](int s, const double* p) {
            Transducer t = srcs[(size_t)s]->t;
            t.pos = {p[0], p[1], p[2]}; t.azimuthDeg = p[3]; t.pattern = srcs[(size_t)s]->lastPattern;
            return t;
        };
        if (!mref.recValid) {
            std::vector<Transducer> rc0, st0;
            resolvedReceivers(w.geo, mref.lst, rc0, false);
            for (int s = 0; s < S; ++s) st0.push_back(srcAt(s, mref.pose[(size_t)s].data()));
            mref.rec = traceLateReciprocal(scn, Vec3{mref.lst[0], mref.lst[1], mref.lst[2]}, rc0, st0, cfg.erOrder, KR, scn.seed + 4242, stopDb, cancel, &wk, &mref.recSq);
            work += wk; mref.recValid = true;
        }
        std::vector<Transducer> rc, st1;
        resolvedReceivers(w.geo, P.data(), rc, false);
        for (int s = 0; s < S; ++s) st1.push_back(srcAt(s, &P[(size_t)(4 + 4 * s)]));
        std::vector<std::array<double, NB>> recSq1;
        const auto rec1 = traceLateReciprocal(scn, Vec3{P[0], P[1], P[2]}, rc, st1, cfg.erOrder, KR, scn.seed + 4242, stopDb, cancel, &wk, &recSq1);
        work += wk;
        const bool lmoved = poseFar(P.data(), mref.lst, dm, dd);
        const double dl = std::hypot(std::hypot(P[0] - mref.lst[0], P[1] - mref.lst[1]), P[2] - mref.lst[2]) + 0.5 * std::fabs(std::remainder(P[3] - mref.lst[3], 360.0)) / 90;
        const double vHold = (cfg.motionHoldDb / 4.343) * (cfg.motionHoldDb / 4.343);  // variance of ln(energy)
        std::vector<float> G = prevPub;
        unsigned long long held = 0;
        for (int s = 0; s < S; ++s) {
            const double* ps = &P[(size_t)(4 + 4 * s)];
            const auto& p0 = mref.pose[(size_t)s];
            if (!lmoved && !poseFar(ps, p0.data(), dm, dd)) {
                for (int r = 0; r < R; ++r) for (int b = 0; b < 8; ++b) G[((size_t)r * S + s) * 8 + b] = mref.G[((size_t)r * S + s) * 8 + b];
                continue;
            }
            const double dist = dl + std::hypot(std::hypot(ps[0] - p0[0], ps[1] - p0[1]), ps[2] - p0[2]) + 0.5 * std::fabs(std::remainder(ps[3] - p0[3], 360.0)) / 90;
            std::vector<std::array<double, NB>> sqF;
            const auto F1 = traceTailEnergyCRN(scn, st1[(size_t)s], rc, cfg.erOrder, KF, scn.seed + 131 * s, stopDb, cancel, &wk, &sqF);
            work += wk;
            const double sd = std::max(1e-4, 6.0 * dist / 4.343), vA = sd * sd;
            for (int r = 0; r < R; ++r)
                for (int b = 0; b < NB; ++b) {
                    const size_t gi = ((size_t)r * S + s) * 8 + b, ei = ((size_t)s * R + r) * NB + b, ki = (size_t)s * R + r;
                    const double F = mref.E[ei], Nn = mref.N[(size_t)r * NB + b], f1 = F1[(size_t)r][(size_t)b];
                    if (Nn <= 0) { ++held; continue; }
                    double est;
                    if (F <= 0) {  // nothing at the reference: only the absolute forward trace can say something
                        const double vF = f1 > 0 ? sqF[(size_t)r][(size_t)b] / (f1 * f1) + 1e-6 : 1e30;
                        if (f1 > 0 && vF > vHold) { ++held; continue; }
                        est = f1;
                    } else {
                        double lB = 0, vB = 1e30, lr = 0, vr = 1e30;
                        if (f1 > 0) { lB = std::log(f1 / F); vB = sqF[(size_t)r][(size_t)b] / (f1 * f1) + 1e-6; }
                        const double c0 = mref.rec[ki][(size_t)b], c1 = rec1[ki][(size_t)b];
                        if (c0 > 0 && c1 > 0) { lr = std::log(c1 / c0); vr = recSq1[ki][(size_t)b] / (c1 * c1) + mref.recSq[ki][(size_t)b] / (c0 * c0) + 1e-6; }
                        const double vm = 1.0 / (1.0 / vB + 1.0 / vr);
                        if (vm > vHold) { ++held; continue; }
                        const double l = std::min(6.9, std::max(-6.9, (lB / vB + lr / vr) / (1.0 / vA + 1.0 / vB + 1.0 / vr)));
                        est = F * std::exp(l);
                    }
                    G[gi] = (float)std::sqrt(est / Nn);
                }
        }
        if (publishMotion(G, false)) { prevPub = G; pubIsRef = false; }
        const double ms = 1000 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        stLastEstMs.store(ms); if (ms > stMaxEstMs.load()) stMaxEstMs.store(ms);
        stLastWork.store(work);
        ++stEstimates; stHeld += held;
    }

    // One pass of the shared-tail worker (every 10 ms on the worker thread, or Engine::runWorkerStep()).
    void sharedWorkerStep() {
        syncQuality();
        {
            std::lock_guard<std::mutex> lk(worldMx);
            const uint64_t ack = ackSerial.load();
            worlds.erase(std::remove_if(worlds.begin(), worlds.end(), [&](const std::shared_ptr<World>& w) { return w->serial <= ack && w != latest && w.get()!=audioWorld.load(std::memory_order_acquire); }), worlds.end());
        }
        for (auto& c : sconv) if (c->pending.load() >= 0) return;
        const uint64_t jser = jumpSerial.load(std::memory_order_acquire);
        const bool jumpPending = (jser != workerJumpSeen);
        const auto now = std::chrono::steady_clock::now();
        if (cfg.motionMode <= 0) {  // as before E4a
            if (!jumpPending && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLegacy).count() < cfg.tailRefreshMs) return;
            if (jumpPending && jser != workerHistJumpSeen) {
                std::shared_ptr<World> wsnapLegacy;
                { std::lock_guard<std::mutex> lk(worldMx); wsnapLegacy = latest; }
                workerLstHist.reset(wsnapLegacy->serial);
                for (int s = 0; s < S; ++s) workerSrcHist[(size_t)s].reset(wsnapLegacy->serial);
                workerHistJumpSeen = jser;
            }
            try {
                sharedRebuild(false, nullptr, jumpPending ? std::function<bool()>([this, jser]{ return jumpSerial.load(std::memory_order_acquire) != jser; }) : std::function<bool()>{}, jumpPending, jser);
                if (jumpPending && jumpSerial.load(std::memory_order_acquire) == jser) workerJumpSeen = jser;
                lastLegacy = std::chrono::steady_clock::now();
            }
            catch(const std::runtime_error&) {sharedBusy=false;if(!jumpPending){sLastGeom=0;for(auto& sp:srcs)sp->lastGeom=0;}}
            return;
        }
        if (lateGseenPub.load(std::memory_order_acquire) != lateGserial.load(std::memory_order_acquire)) return;  // not yet taken
        const int ts = transState.load(std::memory_order_acquire);
        std::shared_ptr<World> wsnap;
        {std::lock_guard<std::mutex> lk(worldMx);wsnap=latest;}
        if (poseScaleReady.load(std::memory_order_acquire) != wsnap->constructionPoseScale) return;
        if (jumpPending) {
            anchorValid = false; lastEstEpoch = ~0ull; mref.recValid = false;
            if (jser != workerHistJumpSeen) {
                workerLstHist.reset(wsnap->serial);
                for (int s = 0; s < S; ++s) workerSrcHist[(size_t)s].reset(wsnap->serial);
                workerHistJumpSeen = jser;
            }
            int exp = 1;
            transState.compare_exchange_strong(exp, 0, std::memory_order_acq_rel);
            const auto t0 = std::chrono::steady_clock::now();
            try {
                if (sharedRebuild(false, nullptr, [this, jser] { return jumpSerial.load(std::memory_order_acquire) != jser; }, true, jser)) {
                    const double ms = 1000 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    stLastRetraceMs.store(ms); if (ms > stMaxRetraceMs.load()) stMaxRetraceMs.store(ms);
                }
                if (jumpSerial.load(std::memory_order_acquire) == jser) {
                    workerJumpSeen = jser;
                    anchor = workerResolvedPose(*wsnap); rawAnchor = livePose(); anchorStream = streamPos.load(std::memory_order_acquire);
                    anchorWall = std::chrono::steady_clock::now(); anchorValid = true;
                    lastLegacy = anchorWall;
                }
            } catch (const std::runtime_error&) { sharedBusy = false; ++stCancelled; }
            return;
        }
        // geometry, materials, quality, decay, directivity: the immediate re-trace and short swap, as before
        bool legacy = wsnap->serial != sLastGeom || workerQuality != sLastQuality;
        for (int s = 0; s < S; ++s) legacy |= std::min(std::max(params[S * 4 + 8 + s].load(), 0.0), 1.0) != srcs[(size_t)s]->lastPattern;
        if (legacy) {
            if (ts == 1 || ts == 2) return;  // the long crossfade finishes first (motionFadeSec at most)
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLegacy).count() < cfg.tailRefreshMs) return;
            try {if (sharedRebuild(false, nullptr, [this, jser]{ return jumpSerial.load(std::memory_order_acquire) != jser; })) lastLegacy = std::chrono::steady_clock::now();}
            catch(const std::runtime_error&) {sharedBusy=false;sLastGeom=0;for(auto& sp:srcs)sp->lastGeom=0;}
            return;
        }
        const double dm = cfg.motionDeadbandM, dd = cfg.motionDeadbandDeg;
        const std::vector<double> rawP = livePose();
        const std::vector<double> P = workerResolvedPose(*wsnap), ref = refPose();
        const uint64_t pos = streamPos.load(std::memory_order_acquire);
        if (!anchorValid || posesFar(P, anchor, dm, dd)) { anchor = P; rawAnchor = rawP; anchorStream = pos; anchorWall = now; anchorValid = true; }
        else rawAnchor = rawP;
        if (!posesFar(P, ref, dm, dd) && !sharedLengthCheck) {  // at the reference (or back at it): its exact gains
            if (!pubIsRef && publishMotion(mref.G, false)) { prevPub = mref.G; pubIsRef = true; }
            return;
        }
        const bool settled = (double)(pos - anchorStream) >= cfg.motionSettleSec * sr
            || (!cfg.manualWorker && std::chrono::duration<double>(now - anchorWall).count() >= cfg.motionSettleSec);
        if (settled) {
            if (ts != 0) return;  // the previous transition is still running
            const auto t0 = std::chrono::steady_clock::now();
            const std::vector<double> ra = rawAnchor.empty() ? rawP : rawAnchor;
            try {
                if (sharedRebuild(false, &ra, [this, ra, dm, dd, jser] { return posesFar(livePose(), ra, dm, dd) || jumpSerial.load(std::memory_order_acquire) != jser; })) {
                    const double ms = 1000 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    stLastRetraceMs.store(ms); if (ms > stMaxRetraceMs.load()) stMaxRetraceMs.store(ms);
                }
            } catch (const std::runtime_error&) { sharedBusy = false; ++stCancelled; }
            return;
        }
        if (cfg.motionMode < 2) return;
        if (lastEstEpoch == mref.epoch && !posesFar(P, lastEstPose, 0.001, 0.1)) return;  // nothing new
        const bool due = cfg.manualWorker ? (double)(pos - lastEstStream) >= cfg.motionIntervalMs * 1e-3 * sr
                                          : std::chrono::duration<double>(now - lastEstWall).count() >= cfg.motionIntervalMs * 1e-3;
        if (!due && lastEstEpoch == mref.epoch) return;
        try {
            motionEstimate(P, *wsnap);
            // the interval counts from the end of an estimate: the worker rests at least motionIntervalMs between two
            lastEstPose = P; lastEstEpoch = mref.epoch; lastEstStream = pos; lastEstWall = std::chrono::steady_clock::now();
        } catch (const std::runtime_error&) {}
    }

    void workerLoopShared() {
        while (running) {
            if (jumpSerial.load(std::memory_order_acquire) == workerJumpSeen)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            sharedWorkerStep();
        }
    }

    void workerLoop() {
        while (running) {
            if (jumpSerial.load(std::memory_order_acquire) == workerJumpSeen)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            perSourceWorkerStep();
        }
    }

    // One pass of the per-source late-field worker. E4a (motionMode >= 1): a move of the listener or a source re-traces
    // nothing while it goes on (the tail keeps its level); once every pose has settled each source whose pose changed
    // beyond the dead-band is re-traced once and crossfaded in over motionFadeSec. Geometry, quality, decay and
    // directivity changes re-trace at once with the short swap, as before.
    std::vector<std::chrono::steady_clock::time_point> psLast;
    std::vector<uint64_t> psLastStream;  // manualWorker: the refresh interval in stream time (deterministic tests)
    std::vector<bool> psLengthCheck;
    int psRR = 0;
    void perSourceWorkerStep() {
        if (psLast.empty()) { psLast.assign((size_t)S, std::chrono::steady_clock::now()); psLastStream.assign((size_t)S, streamPos.load()); }
        if (psLengthCheck.empty()) psLengthCheck.assign((size_t)S, false);
        syncQuality();
        {   // free geometry snapshots the audio thread has moved past
            std::lock_guard<std::mutex> lk(worldMx);
            const uint64_t ack = ackSerial.load();
            worlds.erase(std::remove_if(worlds.begin(), worlds.end(), [&](const std::shared_ptr<World>& w) { return w->serial <= ack && w != latest && w.get()!=audioWorld.load(std::memory_order_acquire); }), worlds.end());
        }
        const uint64_t jser = jumpSerial.load(std::memory_order_acquire);
        const bool jumpPending = (jser != workerJumpSeen);
        if (jumpPending) anchorValid = false;
        std::shared_ptr<World> wsnap0;
        if (cfg.motionMode > 0 || jumpPending) {
            std::lock_guard<std::mutex> lk(worldMx);
            wsnap0 = latest;
        }
        if (jumpPending && jser != workerHistJumpSeen) {
            workerLstHist.reset(wsnap0->serial);
            for (int s = 0; s < S; ++s) workerSrcHist[(size_t)s].reset(wsnap0->serial);
            workerHistJumpSeen = jser;
        }
        const double dm = cfg.motionDeadbandM, dd = cfg.motionDeadbandDeg;
        bool settled = true;
        std::vector<double> anchorNow, rawAnchorNow;
        if (cfg.motionMode > 0) {
            const auto nowA = std::chrono::steady_clock::now();
            const std::vector<double> rawP = livePose();
            const std::vector<double> P = workerResolvedPose(*wsnap0);
            const uint64_t pos = streamPos.load(std::memory_order_acquire);
            if (!anchorValid || posesFar(P, anchor, dm, dd)) { anchor = P; rawAnchor = rawP; anchorStream = pos; anchorWall = nowA; anchorValid = true; }
            else rawAnchor = rawP;
            settled = (double)(pos - anchorStream) >= cfg.motionSettleSec * sr
                || (!cfg.manualWorker && std::chrono::duration<double>(nowA - anchorWall).count() >= cfg.motionSettleSec);
            anchorNow = anchor;
            rawAnchorNow = rawAnchor.empty() ? rawP : rawAnchor;
        }
        bool jumpDeferred = false;
        for (int k = 0; k < S && running; ++k) {
            int s = (psRR + k) % S;
            Src& sp = *srcs[s];
            if (sp.tail.pending.load() >= 0 && sp.tail.pending.load()!=sp.tail.publishedHandover.load(std::memory_order_acquire)) {
                jumpDeferred = true; continue;
            }
            auto now = std::chrono::steady_clock::now();
            if (!jumpPending && (cfg.manualWorker ? (double)(streamPos.load() - psLastStream[(size_t)s]) < cfg.tailRefreshMs * 1e-3 * sr
                                                  : std::chrono::duration_cast<std::chrono::milliseconds>(now - psLast[s]).count() < cfg.tailRefreshMs)) continue;
            std::shared_ptr<World> wsnap;
            {std::lock_guard<std::mutex> lk(worldMx);wsnap=latest;}
            // Size geometry may arrive before the next host block. Wait for
            // the audio thread to publish ALL matching source/listener poses.
            if(poseScaleReady.load(std::memory_order_acquire)!=wsnap->constructionPoseScale) {
                jumpDeferred = true; continue;
            }
            double rawPose[4], rawLst[4], pose[4], lst[4];
            for (int i = 0; i < 4; ++i) { rawPose[i] = params[s * 4 + i].load(); rawLst[i] = params[S * 4 + i].load(); }
            resolvePose4(wsnap->geo, wsnap->serial, workerSrcHist[(size_t)s], rawPose, pose);
            resolvePose4(wsnap->geo, wsnap->serial, workerLstHist, rawLst, lst);
            double dp = std::hypot(std::hypot(pose[0] - sp.lastPose[0], pose[1] - sp.lastPose[1]), pose[2] - sp.lastPose[2]);
            double dl = std::hypot(std::hypot(lst[0] - sp.lastLst[0], lst[1] - sp.lastLst[1]), lst[2] - sp.lastLst[2]);
            const uint64_t gser = geomSerial.load();
            const double pat = std::min(std::max(params[S * 4 + 8 + s].load(), 0.0), 1.0);
            const bool other = jumpPending || gser != sp.lastGeom || pat != sp.lastPattern || sp.lastQuality != workerQuality;
            const bool durationChanged=sourcePhysicalSeconds[s]!=wsnap->sc.length;
            bool moved = false, longFade = false, poseMoved = false;
            if (jumpPending) {
                poseMoved = dp > 0.001 || dl > 0.001 || std::fabs(std::remainder(pose[3] - sp.lastPose[3], 360.0)) > 0.1
                    || std::fabs(std::remainder(lst[3] - sp.lastLst[3], 360.0)) > 0.1;
                moved = poseMoved || durationChanged || gser != sp.lastGeom || pat != sp.lastPattern || sp.lastQuality != workerQuality;
            } else if (cfg.motionMode <= 0) {
                moved = dp > 0.05 || dl > 0.05 || std::fabs(pose[3] - sp.lastPose[3]) > 2 || std::fabs(lst[3] - sp.lastLst[3]) > 2 || other;
            } else {
                poseMoved = dp > dm || dl > dm || std::fabs(std::remainder(pose[3] - sp.lastPose[3], 360.0)) > dd
                    || std::fabs(std::remainder(lst[3] - sp.lastLst[3], 360.0)) > dd;
                if (other) moved = true;
                else if ((poseMoved || durationChanged || psLengthCheck[(size_t)s]) && settled) moved = longFade = true;
            }
            if ((sp.tail.publishedHandover.load(std::memory_order_acquire) >= 0 || sp.tail.cleanSynced.load(std::memory_order_acquire) < sp.tail.P) && !other) continue;
            if (!moved) continue;
            Transducer st = sp.t;
            st.pattern = pat;
            st.pos = {pose[0], pose[1], pose[2]};
            st.azimuthDeg = pose[3];
            std::vector<Transducer> rc;
            resolvedReceivers(wsnap->geo, lst, rc, longFade);
            std::vector<std::array<double, 4>> effSrcs((size_t)S);
            if (longFade || jumpPending) {
                for (int s2 = 0; s2 < S; ++s2) {
                    if (s2 == s) { for (int i = 0; i < 4; ++i) effSrcs[(size_t)s2][(size_t)i] = pose[i]; }
                    else {
                        double rp2[4], ep2[4];
                        for (int i = 0; i < 4; ++i) rp2[i] = params[s2 * 4 + i].load();
                        resolvePose4(wsnap->geo, wsnap->serial, workerSrcHist[(size_t)s2], rp2, ep2);
                        for (int i = 0; i < 4; ++i) effSrcs[(size_t)s2][(size_t)i] = ep2[i];
                    }
                }
            }
            int cur = sp.tail.curAtomic.load();
            const int active=sp.tail.publishedHandover.load(std::memory_order_acquire);
            int w=0;while(w==cur||w==active)++w;
            sp.tail.workerBusy.store(true,std::memory_order_release);
            const auto t0 = std::chrono::steady_clock::now();
            Scene settledScene=wsnap->sc;
            bool cacheHit = false;
            try {
                if(longFade) {
                    settledScene.length=settledTailLength(*wsnap,lst,rc,[&]{return !running||geomSerial.load()!=wsnap->serial||qualitySerial.load()!=workerQuality||posesFar(livePose(),rawAnchorNow,dm,dd)||jumpSerial.load(std::memory_order_acquire)!=workerJumpSeen||(cfg.cancelled&&cfg.cancelled());}, &effSrcs);
                    if (!poseMoved && !durationChanged && settledScene.length == wsnap->sc.length) {
                        psLengthCheck[(size_t)s] = false;
                        sp.tail.workerBusy.store(false, std::memory_order_release);
                        psLast[s] = std::chrono::steady_clock::now();
                        psLastStream[(size_t)s] = streamPos.load();
                        psRR = (s + 1) % S;
                        break;
                    }
                    buildTail(s, settledScene, st, rc, w, wsnap->serial, [this, rawAnchorNow, dm, dd, jser] { return posesFar(livePose(), rawAnchorNow, dm, dd) || jumpSerial.load(std::memory_order_acquire) != jser; });
                    psLengthCheck[(size_t)s] = false;
                } else if (jumpPending) {
                    settledScene.length = jumpTailLength(*wsnap, lst, rc, cacheHit, &effSrcs);
                    buildTail(s, settledScene, st, rc, w, wsnap->serial, [this, jser] { return jumpSerial.load(std::memory_order_acquire) != jser; });
                    psLengthCheck[(size_t)s] = !cacheHit;
                } else buildTail(s, wsnap->sc, st, rc, w, wsnap->serial);
            }
            catch(const std::runtime_error&) {
                sp.tail.workerBusy=false;if(longFade||jumpPending)++stCancelled;else sp.lastGeom=0;if(jumpPending){jumpDeferred=true;break;}continue;
            }
            if (longFade || jumpPending) {
                publishTailLength(wsnap,settledScene.length);
                const double ms = 1000 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                stLastRetraceMs.store(ms); if (ms > stMaxRetraceMs.load()) stMaxRetraceMs.store(ms);
            }
            sp.lastPattern = pat;
            for (int i = 0; i < 4; ++i) sp.lastPose[i] = pose[i];
            for (int i = 0; i < 4; ++i) sp.lastLst[i] = lst[i];
            sp.lastGeom = wsnap->serial;
            psLast[s] = std::chrono::steady_clock::now();
            psLastStream[(size_t)s] = streamPos.load();
            sp.tail.longReq = 0;
            sp.tail.slot[w].isJump = jumpPending;
            if(longFade){std::fill(sp.tail.freshRe.begin(),sp.tail.freshRe.end(),0.f);std::fill(sp.tail.freshIm.begin(),sp.tail.freshIm.end(),0.f);std::fill(sp.tail.freshPrev.begin(),sp.tail.freshPrev.end(),0.f);std::fill(sp.tail.freshIn.begin(),sp.tail.freshIn.end(),0.f);}
            if(longFade)handoverCorrelation(sp.tail,sp.tail.slot[cur],sp.tail.slot[w]);
            if (longFade) {  // correlation of the old and the new IR (white input) for the first block of the crossfade
                const Slot& a = sp.tail.slot[cur]; const Slot& x = sp.tail.slot[w];
                double ax = 0, aa = 0, xx = 0;
                const int bins = sp.tail.bins;
                for (int r = 0; r < R; ++r)
                    for (int k2 = 0; k2 < std::max(a.partitions, x.partitions); ++k2)
                        for (int b = 0; b < bins; ++b) {
                            const double wgt = (b == 0 || b == bins - 1) ? 1.0 : 2.0;
                            const bool ha = k2 < a.partitions, hx = k2 < x.partitions;
                            const size_t oa = ((size_t)r * a.partitions + k2) * bins + b, ox = ((size_t)r * x.partitions + k2) * bins + b;
                            if (ha) aa += wgt * ((double)a.re[oa] * a.re[oa] + (double)a.im[oa] * a.im[oa]);
                            if (hx) xx += wgt * ((double)x.re[ox] * x.re[ox] + (double)x.im[ox] * x.im[ox]);
                            if (ha && hx) ax += wgt * ((double)a.re[oa] * x.re[ox] + (double)a.im[oa] * x.im[ox]);
                        }
                sp.tail.rhoReq = (aa > 0 && xx > 0) ? (float)std::min(1.0, std::max(0.0, ax / std::sqrt(aa * xx))) : 0.f;
                sp.tail.longReq = std::max(1, (int)std::lround(cfg.motionFadeSec * sr / B));
                for (int i = 0; i < 4; ++i) stRefLst[(size_t)i].store(lst[i]);
            }
            sp.tail.pending.store(w);
            sp.tail.workerBusy.store(false,std::memory_order_release);
            if (!jumpPending) { psRR = (s + 1) % S; break; }
            if (jumpSerial.load(std::memory_order_acquire) != jser) { jumpDeferred = true; break; }
        }
        if (jumpPending && !jumpDeferred && jumpSerial.load(std::memory_order_acquire) == jser) {
            std::shared_ptr<World> wsnapEnd;
            { std::lock_guard<std::mutex> lk(worldMx); wsnapEnd = latest; }
            workerJumpSeen = jser;
            anchor = workerResolvedPose(*wsnapEnd); rawAnchor = livePose(); anchorStream = streamPos.load(std::memory_order_acquire);
            anchorWall = std::chrono::steady_clock::now(); anchorValid = true;
        }
    }
    // Publishes new walls/materials/floor/ceiling/height (sources, receivers and length stay). Any thread.
    bool setScene(const Scene& ns, double absorption, const std::function<bool()>& cancelled, double constructionPoseScale=1.) {
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
        auto releaseRt=estimateRoomRT60(ns);for(int b=0;b<7;++b)freezeRt60[b].store(releaseRt[b],std::memory_order_relaxed);
        Scene n = sc;
        // Preserve construction anchors for ordinary edits, and scale those same
        // anchors with Size. Mic capsule offsets remain physical distances.
        if(constructionPoseScale!=1.) {
            for(auto& t:n.sources)t.pos=t.pos*constructionPoseScale;
            const auto centre=sc.hasListener?sc.listenerPos:l0;
            const auto scaledCentre=centre*constructionPoseScale;
            for(auto& r:n.receivers)r.pos=scaledCentre+(r.pos-centre);
            if(n.hasListener)n.listenerPos=scaledCentre;
        }
        n.walls = ns.walls; n.mats = ns.mats; n.floorMat = ns.floorMat; n.ceilMat = ns.ceilMat; n.height = ns.height;
        n.tempC = ns.tempC; n.humidity = ns.humidity; n.clutter = ns.clutter;
        n.floorZones = ns.floorZones; n.ceilZones = ns.ceilZones;
        n.roughness = ns.roughness; n.roughSeed = ns.roughSeed; n.diffraction = ns.diffraction;
        n.length = ns.length;
        n.length = cachedTailSeconds(n, cfg.erOrder, absorption, cancelled);
        roomTailLength.store(n.length, std::memory_order_relaxed);
        auto w = std::make_shared<World>(n, maxWalls, absorption,ns.length);
        w->constructionPoseScale=constructionPoseScale;
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
        const uint64_t avail = sp.period >= sp.histMinPeriod ? (sp.period - sp.histMinPeriod) : 0;
        const double maxU = std::min((double)(kHist - 3), (double)avail);
        u = std::min(std::max(u, 0.0), maxU);
        if (u <= 0.0) return sp.hist[sp.period & (kHist - 1)];
        uint64_t j = (uint64_t)u;
        if (j >= avail) return sp.hist[sp.histMinPeriod & (kHist - 1)];
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

    bool validate(int idx, const Src& sp, const Transducer& rc, const Transducer& src, Found& f,int source,int receiver) {
        if(G().separatedShell(src.pos,rc.pos,false))return false;
        Vec3 cur = rc.pos, first{}, last{};
        int prevSurf = -1, n = idx;
        float amp[NB];
        for (int b = 0; b < NB; ++b) amp[b] = 1.f;
        Vec3 pts[16];  // the reflection points, receiver side first (order <= 4; stack only)
        int np = 0;
        while (nodes[n].order >= 1) {
            const Node& nd = nodes[n];
            Vec3 P;
            if (!(nodes[idx].order==1 ? cachedSupport(source,receiver,nd.surf,cur,nd.pos,P,amp) : crossSurfaceLegacy(G(),cur,nd.pos,nd.surf,P,c,amp))) return false;
            if(nodes[idx].order==1 && wd->sc.diffraction)softEdgeUnitsNow+=softEdgeWorkBound(G());
            if (!(nodes[idx].order==1 ? cachedSoftLeg(source,receiver,nd.surf,0,cur,P,amp,false,src.pos) : transmitLeg(G(), cur, P, nd.surf, prevSurf, amp))) return false;
            if (np < 16) pts[np++] = P;
            if (n == idx) first = P;
            last = P;
            cur = P;
            prevSurf = nd.surf;
            const float* rf = wd->reflAt(nd.surf, P.x, P.y);
            for (int b = 0; b < NB; ++b) amp[b] *= rf[b];
            n = nd.parent;
        }
        if(nodes[idx].order==1 && wd->sc.diffraction)softEdgeUnitsNow+=softEdgeWorkBound(G());
        if (!(nodes[idx].order==1 ? cachedSoftLeg(source,receiver,prevSurf,1,cur,src.pos,amp) : transmitLeg(G(), cur, src.pos, prevSurf, -1, amp))) return false;
        for (int j = 0; j < np; ++j)  // a reflection exactly on another wall's plane (wall foot, corner) is a crossing
            if (!touchCross(G(), j > 0 ? pts[j - 1] : rc.pos, pts[j], j + 1 < np ? pts[j + 1] : src.pos, amp)) return false;
        const Vec3 D=retardedD(sp,idx,rc.pos);const double length=norm(D);
        AmpMemo* memo=nullptr;
        if(nodes[idx].order==1 && wd->sc.diffraction && (!G().freeScreenWalls.empty()||!G().screenRims.empty()||G().freeReflectorHeights||!G().legWelds.empty())){
            const size_t mi=((size_t)source*RM+receiver)*softStride+nodes[idx].surf;
            memo=&ampMemo[mi];const size_t slot=mi*4+3;
            const size_t base=mi*4;
            const uint64_t revision=softMemo[base].generation+softMemo[base+1].generation+softMemo[base+2].generation;
            const bool same=memo->serial==wd->serial&&memo->speed==c&&memo->epoch==sp.earlyEpoch&&memo->revision==revision&&memo->len==length&&memo->early==earlyG;
            if(same){f=memo->value;++softHits;return true;}
            if(!warmingSoft && (slot<softCursor || softSpent>=diffBudget/8)){
                diffPendingNow=true;if(slot>=softCursor&&!softDeferred){softDeferred=true;softNext=slot;}++softHits;
                if(memo->serial!=wd->serial||memo->speed!=c)return false;
                f=memo->value;f.delay=std::max(2.,length/c*sr);
                const float cap=(float)(pathPeakGain[(size_t)source*RM+receiver]*earlyG/std::max(.05,length));
                for(int band=0;band<NB;++band)f.amp[band]=std::clamp(f.amp[band],-cap,cap);
                return true;
            }
            softSpent+=NB*8+16;++softEvals;
            memo->serial=wd->serial;memo->speed=c;memo->epoch=sp.earlyEpoch;memo->revision=revision;memo->len=length;memo->early=earlyG;
        }
        double rg[NB];
        dirGainBands(rc, unit(first - rc.pos), rg);  // the receiver's pattern may depend on frequency
        for (int b = 0; b < NB; ++b) amp[b] *= (float)rg[b];
        double gain = dirGain(src, unit(last - src.pos));
        finish(f, nodes[idx].key, D, gain, amp);
        if(memo)memo->value=f;
        return true;
    }

    void finish(Found& f, uint32_t key, Vec3 D, double gain, const float* amp) const {
        double len = norm(D);
        f.key = key;
        f.delay = std::max(2.0, len / c * sr);
        double spread = (key == 0 || key == kDiffKey ? gain : gain * earlyG) / std::max(len, 0.05);
        for (int b = 0; b < NB; ++b) f.amp[b] = (float)(amp[b] * spread * std::pow(airAmp[b], len));
    }

    void solveLink(int s, int r, const Transducer& src, bool jumpNow, int fadePeriods) {
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
            if(!G().plates.empty())softEdgeUnitsNow+=softEdgeWorkBound(G());
            const bool wallFound=queryDiffraction(s,r,needClear,src.pos,rc.pos,dp);
            DiffPath platePath;const bool plateFound=singleEdgePath(G(),src.pos,rc.pos,c,platePath,-1,true);
            if(plateFound && (!wallFound || platePath.length<dp.length))dp=platePath;
            if(!wallFound && !plateFound)return;
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
            if (found.size() < found.capacity() && validate((int)i, sp, rc, src, f,s,r)) found.push_back(f);
        }
        const double maxDelay = (double)ringMask - 16;
        auto& taps = links[(size_t)s * RM + r];
        const int fadeN = std::max(1, fadePeriods);
        if (jumpNow) {
            for (auto& t : taps) {
                t.d0 = t.d1;
                std::memcpy(t.g0, t.g1, sizeof t.g0);
                t.jumpFadeIn = false;
                if (!t.jumpFadeOut && t.key != 0xFFFFFFFFu) {
                    t.jumpFadeOut = true;
                    t.fadeTotal = fadeN;
                    t.fadeLeft = fadeN - 1;
                    std::memcpy(t.fadeStart, t.g0, sizeof t.fadeStart);
                } else if (t.fadeTotal > 0 && t.fadeLeft > 0) {
                    --t.fadeLeft;
                }
                if (t.fadeTotal > 0) {
                    const float frac = (float)t.fadeLeft / (float)t.fadeTotal;
                    for (int b = 0; b < NB; ++b) t.g1[b] = t.fadeStart[b] * frac;
                } else {
                    for (int b = 0; b < NB; ++b) t.g1[b] = 0.f;
                }
                t.seen = true;
            }
            auto appendTap = [&](const Tap& nt) {
                if (taps.size() < taps.capacity()) { taps.push_back(nt); return; }
                int bestIdx = -1;
                float bestPeak = 1e30f;
                for (size_t ti = 0; ti < taps.size(); ++ti) {
                    if (!taps[ti].jumpFadeOut && taps[ti].key != 0xFFFFFFFFu) continue;
                    float pk = 0.f;
                    for (int b = 0; b < NB; ++b) pk = std::max(pk, std::max(taps[ti].g0[b], taps[ti].g1[b]));
                    if (pk < bestPeak) { bestPeak = pk; bestIdx = (int)ti; }
                }
                if (bestIdx >= 0) taps[(size_t)bestIdx] = nt;
            };
            for (const Found& fc : found) {
                if (fc.delay >= maxDelay) continue;
                Tap t;
                t.key = fc.key; t.seen = true; t.d0 = t.d1 = fc.delay; t.ref = fc.delay;
                t.jumpFadeIn = true;
                t.fadeTotal = fadeN;
                t.fadeLeft = fadeN - 1;
                std::memcpy(t.fadeTarget, fc.amp, sizeof t.fadeTarget);
                const float frac = 1.f - (float)t.fadeLeft / (float)t.fadeTotal;
                for (int b = 0; b < NB; ++b) { t.g0[b] = 0.f; t.g1[b] = fc.amp[b] * frac; }
                appendTap(t);
            }
            return;
        }
        for (auto& t : taps) {
            t.d0 = t.d1;
            std::memcpy(t.g0, t.g1, sizeof t.g0);
            if ((t.jumpFadeOut || t.key == 0xFFFFFFFFu) && t.fadeTotal > 0) {
                if (t.fadeLeft > 0) --t.fadeLeft;
                const float frac = (float)t.fadeLeft / (float)t.fadeTotal;
                for (int b = 0; b < NB; ++b) t.g1[b] = t.fadeStart[b] * frac;
                t.seen = true;
            } else {
                t.seen = false;
            }
        }
        for (const Found& fc : found) {
            Found f = fc;
            if (f.delay >= maxDelay) continue;
            Tap* tp = nullptr;
            for (auto& t : taps)
                if (!t.jumpFadeOut && t.key == f.key && !t.seen) { tp = &t; break; }
            const double tru = f.delay;
            if (tp) {
                // Doppler strength: blend the true delay with its slow average (k=1: physical, 0: pitch-free)
                if (firstSolve) tp->ref = tru;
                else tp->ref += dopA * (tru - tp->ref);
                if (dopK != 1.0) f.delay = std::max(2.0, tp->ref + dopK * (tru - tp->ref));
                if (!firstSolve && std::fabs(f.delay - tp->d0) > 0.5 * cfg.ctrlInterval && taps.size() < taps.capacity()) {
                    Tap dying = *tp;  // teleport: let the old one fade out in place, start a new one
                    dying.jumpFadeOut = true; dying.seen = true; dying.d1 = dying.d0;
                    dying.jumpFadeIn = false; dying.fadeTotal = 0; dying.fadeLeft = 0;
                    for (int b = 0; b < NB; ++b) dying.g1[b] = 0.f;
                    for (int b = 0; b < NB; ++b) { tp->g0[b] = 0.f; }
                    tp->d0 = f.delay;
                    tp->jumpFadeIn = false; tp->fadeTotal = 0; tp->fadeLeft = 0;
                    taps.push_back(dying);
                    tp = &taps[0];  // pointer invalid after push_back; re-find below
                    for (auto& t : taps) if (!t.jumpFadeOut && t.key == f.key && !t.seen) { tp = &t; break; }
                    tp->ref = tru;
                }
                tp->seen = true;
                tp->d1 = f.delay;
                if (tp->jumpFadeIn && tp->fadeTotal > 0) {
                    if (tp->fadeLeft > 0) --tp->fadeLeft;
                    const float frac = 1.f - (float)tp->fadeLeft / (float)tp->fadeTotal;
                    std::memcpy(tp->fadeTarget, f.amp, sizeof tp->fadeTarget);
                    for (int b = 0; b < NB; ++b) tp->g1[b] = f.amp[b] * frac;
                    if (tp->fadeLeft == 0) { tp->jumpFadeIn = false; tp->fadeTotal = 0; }
                } else {
                    std::memcpy(tp->g1, f.amp, sizeof tp->g1);
                }
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
            if (!t.seen) { t.jumpFadeIn = false; t.fadeTotal = 0; t.fadeLeft = 0; for (int b = 0; b < NB; ++b) t.g1[b] = 0.f; }
    }

    void placeReceivers(const double* lst, double spread) {
        listenerReceivers(lst, rcvBase);
        containCapsulesInRoom(G(), {lst[0], lst[1], lst[2]}, rcvBase.data(), R);
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
        if (M > 1) containCapsulesInRoom(G(), {lst[0], lst[1], lst[2]}, rcvAbs.data(), RM);
    }

    // ---- E3d: bounded diffraction search ----
    size_t diffSlot(int s, int r, bool needClear) const { return ((size_t)s * (size_t)RM + (size_t)r) * 2 + (needClear ? 1 : 0); }
    void storeAnswer(DiffMemo& m, const DiffractionSearch& q, bool noMiss) {
        m.S = q.S; m.R = q.R; m.c = q.c; m.serial = wd->serial;
        m.valid = noMiss;  // an undersized scratch is reported on every call, as before
        m.found = q.found;
        if (q.found) { m.dp = q.out; m.seq = q.seq; }
        else m.seq = DiffSeq{};
    }
    // The way round the wall ends for link (s, r) at positions (S, Rp); false = none. Bounded work: see DiffMemo.
    bool queryDiffraction(int s, int r, bool needClear, Vec3 S, Vec3 Rp, DiffPath& dp) {
        DiffMemo& m = diffMemo[diffSlot(s, r, needClear)];
        if (m.epoch != diffJumpEpoch) { m = DiffMemo{}; m.epoch = diffJumpEpoch; }
        auto exactAnswer = [&](bool f, const DiffPath& p) {
            m.want = false; m.urgent = false; m.exact = true;
            if (f) { dp = p; m.held = p; m.hold = kDiffHold; }
            else m.hold = 0;
            return f;
        };
        if (m.valid && m.serial == wd->serial && m.c == c && m.S.x == S.x && m.S.y == S.y && m.S.z == S.z
            && m.R.x == Rp.x && m.R.y == Rp.y && m.R.z == Rp.z)
            return exactAnswer(m.found, m.dp);
        // one wall end: cheap, and exact whenever it decides
        const uint64_t misses = quickScratch.capacityMisses;
        quickSearch.start(G(), S, Rp, c, needClear, quickScratch);
        quickSearch.run(-1, true);
        diffUnitsNow += quickSearch.units;
        ++dstats.quick;
        if (quickSearch.done) {
            storeAnswer(m, quickSearch, quickScratch.capacityMisses == misses);
            return exactAnswer(quickSearch.found, quickSearch.out);
        }
        // the multi-end search is needed: request it, and follow the last answer's wall ends meanwhile
        if (!m.want) m.wantSince = diffPeriod;
        m.want = true; m.wS = S; m.wR = Rp; m.wantPeriod = diffPeriod;
        m.exact = false;
        diffPendingNow = true;
        if (m.seq.n > 0) {
            int64_t u = 0;
            DiffPath e;
            const bool ok = evalDiffSeq(G(), S, Rp, c, needClear, m.seq, e, u);
            diffUnitsNow += u;
            ++dstats.follows;
            if (ok) { dp = e; m.held = e; m.hold = kDiffHold; return true; }
        }
        if (m.hold > 0) {  // the way followed so far has closed: fade it out unless the new answer comes first
            m.urgent = true;
            dp = m.held;
            const float f = (float)m.hold / (float)(kDiffHold + 1);
            for (int b = 0; b < NB; ++b) dp.amp[b] *= f;
            --m.hold;
            ++dstats.holdPeriods;
            return true;
        }
        return false;
    }
    // Works on the requests within the period's budget: the urgent ones (a followed way has closed) first, then the
    // longest-waiting. One search at a time; it may span many periods. Its answer is stamped with the positions it was
    // asked for; the link follows that answer's wall ends from the next period on.
    void runDiffractionJobs() {
        int64_t budget = std::max<int64_t>(0,diffBudget-softSpent), spent = 0;
        while (budget > 0) {
            if (jobSlot < 0) {
                int best = -1;
                for (size_t q = 0; q < diffMemo.size(); ++q) {
                    const DiffMemo& m = diffMemo[q];
                    if (!m.want || m.wantPeriod != diffPeriod || m.epoch != diffJumpEpoch) continue;
                    if (best < 0) { best = (int)q; continue; }
                    const DiffMemo& b = diffMemo[(size_t)best];
                    if ((m.urgent && !b.urgent) || (m.urgent == b.urgent && m.wantSince < b.wantSince)) best = (int)q;
                }
                if (best < 0) break;
                DiffMemo& m = diffMemo[(size_t)best];
                jobSearch.start(G(), m.wS, m.wR, c, (best & 1) != 0, diffractionScratch);
                jobSlot = best; jobMisses = diffractionScratch.capacityMisses; jobSince = m.wantSince;
                m.want = false; m.urgent = false;  // a newer request may follow while this one is worked on
                ++dstats.jobsStarted;
            }
            const int64_t before = jobSearch.units;
            const bool fin = jobSearch.run(budget);
            const int64_t used = jobSearch.units - before;
            budget -= used; spent += used;
            if (!fin) break;
            storeAnswer(diffMemo[(size_t)jobSlot], jobSearch, diffractionScratch.capacityMisses == jobMisses);
            ++dstats.jobsFinished;
            dstats.maxLatencyPeriods = std::max<unsigned long long>(dstats.maxLatencyPeriods, diffPeriod - jobSince + 1);
            dstats.latencySumPeriods += diffPeriod - jobSince + 1;
            dstats.jobUnitsMax = std::max<long long>(dstats.jobUnitsMax, jobSearch.units);
            jobSlot = -1;
        }
        diffUnitsNow += spent;
        dstats.searchUnitsMax = std::max<long long>(dstats.searchUnitsMax, spent);
    }
    // Constructor: the initial positions are solved completely (off the audio thread), so the first periods give the same
    // answers as an unbounded search would.
    void presolveDiffraction() {
        if (!wd->sc.diffraction) return;
        double rawLst[4], lst[4];
        for (int i = 0; i < 4; ++i) rawLst[i] = params[S * 4 + i].load();
        resolvePose4(G(), wd->serial, audioLstHist, rawLst, lst);
        placeReceivers(lst, std::min(std::max(params[S * 4 + 4].load(), 0.0), 3.0));
        for (int s = 0; s < S; ++s) {
            double rawS[4], effS[4];
            for (int i = 0; i < 4; ++i) rawS[i] = params[s * 4 + i].load();
            resolvePose4(G(), wd->serial, audioSrcHist[(size_t)s], rawS, effS);
            const Vec3 sp{effS[0], effS[1], effS[2]};
            for (int r = 0; r < RM; ++r) {
                const Vec3 rp = rcvAbs[(size_t)r].pos;
                float damp[NB];
                for (int b = 0; b < NB; ++b) damp[b] = 1.f;
                bool nc = false;
                if (transmitLeg(G(), sp, rp, -1, -1, damp)) { if (!(damp[3] < 0.5f)) continue; nc = true; }
                const uint64_t misses = diffractionScratch.capacityMisses;
                jobSearch.start(G(), sp, rp, c, nc, diffractionScratch);
                jobSearch.run(-1);
                storeAnswer(diffMemo[diffSlot(s, r, nc)], jobSearch, diffractionScratch.capacityMisses == misses);
            }
        }
        jobSlot = -1;
    }

    void startPeriod() {
        const auto freezeRevision=freezeRtRevision.load(std::memory_order_acquire);
        if(freezeRtSeen!=freezeRevision){std::array<double,7> rt;for(int b=0;b<7;++b)rt[b]=freezeRt60[b].load(std::memory_order_relaxed);tailFreeze.setRoomDecay(rt);freezeRtSeen=freezeRevision;}
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
            wd = pw;audioWorld.store(wd,std::memory_order_release);
            c = pw->c;  // temperature changes the speed of sound, humidity the air absorption
            airAmp = pw->airAmp;
            ackSerial.store(pw->serial);
            jobSlot = -1;  // a search in progress belongs to the old geometry
            std::array<double,7> rt;for(int b=0;b<7;++b)rt[b]=freezeRt60[b].load(std::memory_order_relaxed);tailFreeze.setRoomDecay(rt);
            worldChanged = true;
        }
        double rawLst[4];
        for (int i = 0; i < 4; ++i) rawLst[i] = params[S * 4 + i].load(std::memory_order_relaxed);
        const double spread = std::min(std::max(params[S * 4 + 4].load(std::memory_order_relaxed), 0.0), 3.0);
        const double earlyDb = std::min(std::max(params[S * 4 + 5].load(std::memory_order_relaxed), -60.0), 6.0);
        const uint64_t jBefore = jumpSerial.load(std::memory_order_acquire);
        if (firstSolve) audioJumpSeen = jBefore;
        bool audioJumped = (!firstSolve && jBefore != audioJumpSeen);
        if (!firstSolve && !audioJumped) {
            const double maxStep = std::max(0.5, 100.0 * cfg.ctrlInterval / sr);
            const double dl = std::hypot(std::hypot(rawLst[0] - lastRawListener[0], rawLst[1] - lastRawListener[1]), rawLst[2] - lastRawListener[2]);
            if (dl > maxStep || std::fabs(std::remainder(rawLst[3] - lastRawListener[3], 360.0)) > 45.0) audioJumped = true;
            for (int s = 0; s < S && !audioJumped; ++s) {
                double pose[4];
                for (int i = 0; i < 4; ++i) pose[i] = params[s * 4 + i].load(std::memory_order_relaxed);
                const double dp = std::hypot(std::hypot(pose[0] - srcs[s]->solvedRawPose[0], pose[1] - srcs[s]->solvedRawPose[1]), pose[2] - srcs[s]->solvedRawPose[2]);
                if (dp > maxStep || std::fabs(std::remainder(pose[3] - srcs[s]->solvedRawPose[3], 360.0)) > 45.0) audioJumped = true;
            }
            if (audioJumped) jumpSerial.fetch_add(1, std::memory_order_acq_rel);
        }
        const uint64_t jCur = audioJumped ? jumpSerial.load(std::memory_order_acquire) : audioJumpSeen;
        const int fadePeriods = std::max(1, (int)std::lround(0.030 * sr / cfg.ctrlInterval));
        if (audioJumped) {
            audioJumpSeen = jCur;
            ++diffJumpEpoch;
            jobSlot = -1;
            softCursor = 0;
            softDeferred = false;
            audioLstHist.reset(wd->serial);
            for (int s = 0; s < S; ++s) {
                audioSrcHist[(size_t)s].reset(wd->serial);
                srcs[s]->histMinPeriod = srcs[s]->period + 1;
                srcs[s]->fadeLeftPeriods = fadePeriods;
                srcs[s]->quiet = 0;
            }
            lateGjumpLeft = 0;
            if (shared) {
                int exp = 1;
                transState.compare_exchange_strong(exp, 0, std::memory_order_acq_rel);
                const int tsNow = transState.load(std::memory_order_acquire);
                if (tsNow == 2 || tsNow == 3) {
                    handoverLegacy = true;
                    jumpHandoverAbort = true;
                }
                if (mMid.load(std::memory_order_acquire) & 4)
                    mFront = mMid.exchange(mFront, std::memory_order_acq_rel) & 3;
            }
        }
        double lst[4];
        resolvePose4(G(), wd->serial, audioLstHist, rawLst, lst);
        bool lmoved = firstSolve || worldChanged || audioJumped || spread != lastSpread || earlyDb != lastEarlyDb;
        // Settings that ease towards their targets do so once per control period (fixed in stream time), so the output
        // does not depend on how the host chops the audio into blocks.
        if (shared) {
            if (lateGserial.load(std::memory_order_acquire) != lateGseen) {
                lateGseen = lateGserial.load(std::memory_order_acquire);
                const int gc = lateGcur.load(std::memory_order_acquire);
                std::memcpy(lateGlocal.data(), lateGbuf[(size_t)gc].data(), sizeof(float) * lateGlocal.size());
                audioEpoch.store(lateGepoch[(size_t)gc], std::memory_order_release);
                lateGtarget = lateGlocal.data();
                gainMode = 0; motionExact = false;
                if(transState.load(std::memory_order_relaxed)==3)handoverLegacy=true;
                const uint64_t gJ = lateGjumpSerial[(size_t)gc];
                if (gJ != 0 && gJ > audioLastJumpTag) {
                    audioLastJumpTag = gJ;
                    std::memcpy(lateGjump0.data(), lateGuse.data(), sizeof(float) * lateGjump0.size());
                    lateGjumpTotal = fadePeriods;
                    lateGjumpLeft = fadePeriods;
                }
                lateGseenPub.store(lateGseen, std::memory_order_release);
            }
            // E4a: motion targets (only for the normalization this thread is in) and the settle transition
            if (mMid.load(std::memory_order_acquire) & 4) {
                mFront = mMid.exchange(mFront, std::memory_order_acq_rel) & 3;
                if (mEpoch[(size_t)mFront] == audioEpoch.load(std::memory_order_relaxed) && mJumpSerial[(size_t)mFront] >= audioJumpSeen) {
                    std::memcpy(lateGlocal.data(), mBuf[(size_t)mFront].data(), sizeof(float) * lateGlocal.size());
                    lateGtarget = lateGlocal.data();
                    motionExact = mExact[(size_t)mFront];
                    // Exact targets feed the fresh bank; causal per-band weights bring it in.
                    // Estimates outside a settle still follow the existing slew-limited easing.
                    if (motionExact) {
                        std::memcpy(glide0.data(), lateGuse.data(), sizeof(float) * glide0.size()); glideAt=0;glideN=1;gainMode=2;
                        for(size_t i=0;i<glide0.size();++i) {
                            const double mismatch=std::abs(20.*std::log10(std::max(1e-12f,lateGlocal[i])/std::max(1e-12f,glide0[i])));
                            glideSteps[i]=std::max(1,(int)std::ceil(std::clamp(.5+mismatch/24.,.5,.75)*sr/cfg.ctrlInterval));
                            glideN=std::max(glideN,glideSteps[i]);
                        }
                    }
                    else gainMode = 1;
                    stAccepted.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (transState.load(std::memory_order_acquire) == 1) {
                for (int r = 0; r < R; ++r) {
                    TailConv& t = *sconv[(size_t)r];
                    t.hoSlot=trans.slotH[(size_t)r]; t.hoAge=0;t.inputStart=gcount;t.inputFadeBlocks=trans.fadeBlocks;t.cAA=t.cBB=t.cAB=0;
                    // Keep the old IR/bank. The fresh bank always uses the exact new IR.
                    t.longN=0;t.longAt=0;
                }
                handoverStart=gcount;handoverLegacy=false;
                transState.store(2, std::memory_order_release);
                stTransStarted.fetch_add(1, std::memory_order_relaxed);
            }
            if (lateGtarget) {
                if (lateGjumpLeft > 0) {
                    --lateGjumpLeft;
                    const float alpha = 1.f - (float)lateGjumpLeft / (float)std::max(1, lateGjumpTotal);
                    for (size_t i = 0; i < lateGuse.size(); ++i)
                        lateGuse[i] = lateGjump0[i] + (lateGtarget[i] - lateGjump0[i]) * alpha;
                    if (lateGjumpLeft == 0) {
                        std::memcpy(lateGuse.data(), lateGtarget, sizeof(float) * lateGuse.size());
                        gainMode = 1;
                    }
                } else if (gainMode == 0)
                    for (size_t i = 0; i < lateGuse.size(); ++i) lateGuse[i] += (lateGtarget[i] - lateGuse[i]) * 0.15f;
                else if (gainMode == 2) {
                    // The new bank uses the exact target. Its per-band causal
                    // injection fades from the captured old bank, so there is no
                    // instantaneous change to the audible old response.
                    std::memcpy(lateGuse.data(),lateGtarget,sizeof(float)*lateGuse.size());
                    glideAt=glideN;gainMode=1;
                } else
                    // following a move: the same easing, but never faster than motionSlewDbPerSec, and ending exactly
                    for (size_t i = 0; i < lateGuse.size(); ++i) {
                        const float tg = lateGtarget[i], u = lateGuse[i];
                        float v = u + (tg - u) * 0.15f;
                        v = std::min(std::max(v, u * slewDn), std::max(u * slewUp, u + 2e-4f));
                        if (std::fabs(tg - v) <= 1e-5f * tg + 1e-9f) v = tg;
                        lateGuse[i] = v;
                    }
            }
            // Fixed control-period interpolation keeps work bounded and independent
            // of host block boundaries. No transcendental functions in the feed loop.
            if (jumpHandoverAbort) {
                for(int r=0;r<R;++r)if(sconv[r]->hoSlot>=0)for(int s=0;s<S;++s)for(int b=0;b<7;++b) {
                    const size_t i=((size_t)r*S+s)*8+b;
                    feedA0[i]=lateGuse[i];feedAd[i]=0.f;
                    feedB0[i]=0.f;feedBd[i]=0.f;
                }
            } else {
                for(int r=0;r<R;++r)if(sconv[r]->hoSlot>=0)for(int s=0;s<S;++s)for(int b=0;b<7;++b) {
                    const size_t i=((size_t)r*S+s)*8+b;float a[2],n[2];
                    for(int endpoint=0;endpoint<2;++endpoint) {
                        const double u=std::clamp(double(gcount+endpoint*(cfg.ctrlInterval-1)-handoverStart)/(glideSteps[i]*cfg.ctrlInterval),0.,1.);
                        const float w=handoverLegacy?1.f:(float)(.5-.5*std::cos(kPi*u));
                        const float norm=handoverLegacy?1.f:std::pow((1-w)*(1-w)+w*w+2*sconv[r]->bandRho[b]*w*(1-w),-.28f);
                        a[endpoint]=glide0[i]*norm*(1-w);
                        const float g=handoverLegacy?lateGuse[i]:(motionExact?trans.gNew[i]:lateGuse[i]*trans.cmul[(size_t)r*8+b]);
                        n[endpoint]=g*norm*w;
                    }
                    feedA0[i]=a[0];feedAd[i]=(a[1]-a[0])/std::max(1,cfg.ctrlInterval-1);
                    feedB0[i]=n[0];feedBd[i]=(n[1]-n[0])/std::max(1,cfg.ctrlInterval-1);
                }
            }
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
        if (!firstSolve) for(int i=0;i<4;++i) if(lst[i]!=lastListener[i]) autoMovingUntil.store(gcount+(uint64_t)(.4*sr),std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) lastRawListener[i] = rawLst[i];
        if (lmoved) {
            placeReceivers(lst, spread);
            for (int i = 0; i < 4; ++i) lastListener[i] = lst[i];
            lastSpread = spread;
        }
        for (int s = 0; s < S; ++s) {
            Src& sp = *srcs[s];
            double rawPose[4], pose[4];
            for (int i = 0; i < 4; ++i) rawPose[i] = params[s * 4 + i].load(std::memory_order_relaxed);
            resolvePose4(G(), wd->serial, audioSrcHist[(size_t)s], rawPose, pose);
            Vec3 pnow{pose[0], pose[1], pose[2]};
            ++sp.period;
            sp.hist[sp.period & (kHist - 1)] = pnow;
            const double pat = std::min(std::max(params[S * 4 + 8 + s].load(std::memory_order_relaxed), 0.0), 1.0);
            bool changed = lmoved || firstSolve || worldChanged || dopK != lastDopK || pat != sp.solvedPattern;
            sp.solvedPattern = pat;
            for (int i = 0; i < 4; ++i) if(pose[i]!=sp.solvedPose[i]) {changed=true; if(!firstSolve)autoMovingUntil.store(gcount+(uint64_t)(.4*sr),std::memory_order_relaxed);}
            if(changed&&!firstSolve)++sp.earlyEpoch;
            if (changed) sp.quiet = 0;
            else if (sp.quiet < (1 << 30)) ++sp.quiet;
            for (int i = 0; i < 4; ++i) { sp.solvedPose[i] = pose[i]; sp.solvedRawPose[i] = rawPose[i]; }
            // Delays depend on where the source was up to one max-delay ago: keep solving until the history is still.
            // (with reduced/boosted Doppler the smoothed reference delay needs ~5 time constants to converge)
            const int settle = (int)(ringMask / cfg.ctrlInterval) + 2 + (dopK != 1.0 ? (int)(50.0 * sr / cfg.ctrlInterval) : 0);
            if (sp.quiet > settle && sp.fadeLeftPeriods <= 0) {
                for (int r = 0; r < RM; ++r)
                    for (auto& t : links[(size_t)s * RM + r]) { t.d0 = t.d1; std::memcpy(t.g0, t.g1, sizeof t.g0); }
                continue;
            }
            if (sp.fadeLeftPeriods > 0) --sp.fadeLeftPeriods;
            // Acoustic fields only: a long source name must never allocate here.
            Transducer st;
            st.elevationDeg=sp.t.elevationDeg;st.patternHf=sp.t.patternHf;st.gainDb=sp.t.gainDb;
            st.pattern = pat;
            st.pos = pnow;
            st.azimuthDeg = pose[3];
            if (!buildImages(st.pos)) nodes.resize(1);
            diffPendingNow = false;
            for (int r = 0; r < RM; ++r) solveLink(s, r, st, audioJumped, fadePeriods);
            if (diffPendingNow) sp.quiet = 0;  // an answer is still being searched for: keep solving this source
        }
        runDiffractionJobs();
        ++diffPeriod;
        ++dstats.periods;
        softCursor=softDeferred?softNext:0;softDeferred=false;
        dstats.softActualUnitsLast=softSpent;
        dstats.softActualUnitsMax=std::max<long long>(dstats.softActualUnitsMax,softSpent);
        dstats.softEvaluations+=softEvals;dstats.softCacheHits+=softHits;
        softSpent=0;softEvals=softHits=0;
        dstats.softEdgeUnitsLast=softEdgeUnitsNow;
        dstats.softEdgeUnitsMax=std::max<long long>(dstats.softEdgeUnitsMax,softEdgeUnitsNow);
        softEdgeUnitsNow=0;
        dstats.unitsLast = diffUnitsNow;
        dstats.unitsMax = std::max<long long>(dstats.unitsMax, diffUnitsNow);
        dstats.unitsTotal += (unsigned long long)diffUnitsNow;
        diffUnitsNow = 0;
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
            float gmax = 0;bool gainsStat=true;
            for (int k = 0; k < NB; ++k) {gmax = std::max(gmax, std::max(t.g0[k], t.g1[k]));gainsStat=gainsStat&&t.g0[k]==t.g1[k];}
            if (gmax < 3e-5f) continue;  // below -90 dB: inaudible, not worth rendering
            float* const dst = (t.key == 0 || t.key == kDiffKey) ? out : early;
            const bool stat = t.d0 == t.d1 && gainsStat;
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
                if(gainsStat){foldFrames<false>(t.g0,nullptr,ring,iLo,cnt,s0,nullptr);s1=s0;}
                else foldFrames<true>(t.g0,t.g1,ring,iLo,cnt,s0,s1);
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
                    const auto a=interpolate(s0),b=gainsStat?a:interpolate(s1);
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
                    const float Bv = gainsStat?A:w0 * s1[u] + w1 * s1[u + 1] + w2 * s1[u + 2] + w3 * s1[u + 3];
                    dst[j] += A + tt * (Bv - A);
                }
            }
        }
    }

    // ---- tail convolution ----
    void accumulateFdl(const TailConv& t, const Slot& sl, int r, const float* fdlRe, const float* fdlIm, float* ar, float* ai) const {
        std::fill(ar, ar + t.bins, 0.f);
        std::fill(ai, ai + t.bins, 0.f);
        const size_t base = (size_t)r * sl.partitions * t.bins;
        for (int k = 0; k < sl.partitions; ++k) {
            int e = t.head - k;
            if (e < 0) e += t.P;
            const float* xr = &fdlRe[(size_t)e * t.bins];
            const float* xi = &fdlIm[(size_t)e * t.bins];
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
    void accumulate(const TailConv& t, const Slot& sl, int r, float* ar, float* ai) const {
        const bool useClean = !shared && t.hoSlot < 0 && t.cleanSynced.load(std::memory_order_acquire) < t.P;
        accumulateFdl(t, sl, r, useClean ? t.cleanRe.data() : t.fdlRe.data(), useClean ? t.cleanIm.data() : t.fdlIm.data(), ar, ai);
    }
    void accumulateClean(const TailConv& t, const Slot& sl, int r, float* ar, float* ai) const {
        accumulateFdl(t, sl, r, t.cleanRe.data(), t.cleanIm.data(), ar, ai);
    }

    void inverse(const float* ar, const float* ai, float* out) {
        for (int b = 0; b <= B; ++b) fftBuf[b] = {ar[b], ai[b]};
        fftBuf[0].im = 0; fftBuf[B].im = 0;
        for (int b = 1; b < B; ++b) fftBuf[2 * B - b] = {ar[b], -ai[b]};
        fft.run(fftBuf.data(), true);
        for (int i = 0; i < B; ++i) out[i] = fftBuf[B + i].re;
    }

    void handoverCorrelation(TailConv& t,const Slot& old,const Slot& fresh) {
        double aa[NB]={},bb[NB]={},ab[NB]={};
        for(int bin=0;bin<t.bins;++bin) {
            const double w=2*kPi*bin/(2*B);const std::complex<double> z{std::cos(w),-std::sin(w)};
            std::complex<double> low[NB-1];double weights[NB];
            for(int b=0;b<NB-1;++b){double a=1-std::exp(-2*kPi*kBandHz[b]*std::sqrt(2.)/sr);low[b]=a/(1.-(1.-a)*z);}
            weights[0]=std::norm(low[0]);for(int b=1;b<NB-1;++b)weights[b]=std::norm(low[b]-low[b-1]);weights[NB-1]=std::norm(1.-low[NB-2]);
            const double edge=(bin==0||bin==B)?1.:2.;
            for(int r=0;r<t.R;++r)for(int k=0;k<std::max(old.partitions,fresh.partitions);++k) {
                const size_t i=((size_t)r*old.partitions+k)*t.bins+bin,j=((size_t)r*fresh.partitions+k)*t.bins+bin;
                double a=k<old.partitions?double(old.re[i])*old.re[i]+double(old.im[i])*old.im[i]:0;
                double b=k<fresh.partitions?double(fresh.re[j])*fresh.re[j]+double(fresh.im[j])*fresh.im[j]:0;
                double c=k<old.partitions&&k<fresh.partitions?double(old.re[i])*fresh.re[j]+double(old.im[i])*fresh.im[j]:0;
                for(int band=0;band<NB;++band){double v=edge*weights[band];aa[band]+=a*v;bb[band]+=b*v;ab[band]+=c*v;}
            }
        }
        for(int b=0;b<NB;++b)t.bandRho[b]=aa[b]>1e-30&&bb[b]>1e-30?(float)std::clamp(ab[b]/std::sqrt(aa[b]*bb[b]),0.,1.):1.f;
    }
    void accumulateFresh(TailConv& t,const Slot& sl,int r,float* ar,float* ai) {
        t.fdlRe.swap(t.freshRe);t.fdlIm.swap(t.freshIm);
        accumulateFdl(t,sl,r,t.fdlRe.data(),t.fdlIm.data(),ar,ai);
        t.fdlRe.swap(t.freshRe);t.fdlIm.swap(t.freshIm);
    }
    void promoteFresh(TailConv& t) {
        t.fdlRe.swap(t.freshRe);t.fdlIm.swap(t.freshIm);
        t.prevIn.swap(t.freshPrev);t.inAcc.swap(t.freshIn);
    }
    void accumulateHandover(TailConv& t,int old,int r,float* ar,float* ai) {
        accumulateFdl(t,t.slot[old],r,t.fdlRe.data(),t.fdlIm.data(),ar,ai);
        accumulateFresh(t,t.slot[t.hoSlot],r,accRe2.data(),accIm2.data());
        for(int b=0;b<t.bins;++b){ar[b]+=accRe2[b];ai[b]+=accIm2[b];}
    }

    // Like accumulate(), but adds partitions k0 .. k1-1 of `sl` (E4a transitions only).
    void accumulateRange(const TailConv& t, const Slot& sl, int r, int k0, int k1, float* ar, float* ai) const {
        k1 = std::min(k1, sl.partitions);
        const size_t base = (size_t)r * sl.partitions * t.bins;
        for (int k = std::max(0, k0); k < k1; ++k) {
            int e = t.head - k;
            if (e < 0) e += t.P;
            const float* xr = &t.fdlRe[(size_t)e * t.bins];
            const float* xi = &t.fdlIm[(size_t)e * t.bins];
            const float* hr = &sl.re[base + (size_t)k * t.bins];
            const float* hi = &sl.im[base + (size_t)k * t.bins];
            int b = 0;
           #if VERB_REQUIRE_AVX2
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
    // E4a normalization handover: the newest `age` FDL entries (input weighted with the new gains) use slot `fresh`,
    // older ones slot `old`.
    void accumulateSplit(const TailConv& t, int fresh, int old, int age, int r, float* ar, float* ai) const {
        std::fill(ar, ar + t.bins, 0.f);
        std::fill(ai, ai + t.bins, 0.f);
        accumulateRange(t, t.slot[fresh], r, 0, age, ar, ai);
        accumulateRange(t, t.slot[old], r, age, t.slot[old].partitions, ar, ai);
    }

    // One block of a partitioned convolver; its `t.R` output channels accumulate into rows firstRow.. of tailTmp.
    void convBlock(TailConv& t, int firstRow) {
        int p = t.pending.load();
        bool fade = false, fromSplit = false;
        const int oldCur = t.cur;
        if (p >= 0 && (!t.longFromPending || p!=t.hoSlot)) {
            if (t.longReq > 0 && t.hoSlot < 0 && t.longN == 0 && p != t.cur) {
                // E4a, per-source late field: the re-trace after a move settled is crossfaded over longReq blocks;
                // `pending` stays set (the worker waits) until the crossfade has ended
                t.hoSlot=p;t.hoAge=0;t.inputStart=gcount;t.inputFadeBlocks=t.longReq;t.cAA=t.cBB=t.cAB=0;
                t.longN=0;t.longAt=0;t.longFromPending=true;t.legacyHandover=false;
                t.publishedHandover.store(p,std::memory_order_release);
                stTransStarted.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (t.hoSlot >= 0) fromSplit = true;  // E4a: a re-trace interrupts the handover; fade out of the split sum
                else t.prev = t.cur;
                t.cur = p; fade = true;
                t.longN = 0;  // (the worker never publishes a short swap during a long crossfade)
            }
        }
        // spectrum of [prevIn | inAcc]
        for (int i = 0; i < B; ++i) { fftBuf[i] = {t.prevIn[i], 0}; fftBuf[B + i] = {t.inAcc[i], 0}; }
        fft.run(fftBuf.data(), false);
        t.head = (t.head + 1) % t.P;
        for (int b = 0; b <= B; ++b) { t.fdlRe[(size_t)t.head * t.bins + b] = fftBuf[b].re; t.fdlIm[(size_t)t.head * t.bins + b] = fftBuf[b].im; }
        std::memcpy(t.prevIn.data(), t.inAcc.data(), sizeof(float) * B);
        if (t.hoSlot >= 0) {
            for(int i=0;i<B;++i){fftBuf[i]={t.freshPrev[i],0};fftBuf[B+i]={t.freshIn[i],0};}
            fft.run(fftBuf.data(),false);
            for(int b=0;b<=B;++b){t.freshRe[(size_t)t.head*t.bins+b]=fftBuf[b].re;t.freshIm[(size_t)t.head*t.bins+b]=fftBuf[b].im;}
            std::memcpy(t.freshPrev.data(),t.freshIn.data(),sizeof(float)*B);
        }
        if (!shared) {
            if (t.hoSlot >= 0) {
                for (int i = 0; i < B; ++i) { fftBuf[i] = {t.cleanPrev[i], 0}; fftBuf[B + i] = {t.cleanIn[i], 0}; }
                fft.run(fftBuf.data(), false);
                for (int b = 0; b <= B; ++b) { t.cleanRe[(size_t)t.head * t.bins + b] = fftBuf[b].re; t.cleanIm[(size_t)t.head * t.bins + b] = fftBuf[b].im; }
                std::memcpy(t.cleanPrev.data(), t.cleanIn.data(), sizeof(float) * B);
                t.cleanSynced.store(0, std::memory_order_release);
            } else {
                const size_t off = (size_t)t.head * t.bins;
                std::memcpy(&t.cleanRe[off], &t.fdlRe[off], sizeof(float) * (size_t)t.bins);
                std::memcpy(&t.cleanIm[off], &t.fdlIm[off], sizeof(float) * (size_t)t.bins);
                std::memcpy(t.cleanPrev.data(), t.cleanIn.data(), sizeof(float) * B);
                const int synced = t.cleanSynced.load(std::memory_order_relaxed);
                if (synced < t.P) {
                    const int nextSynced = std::min(t.P, synced + 64);
                    for (int k = synced; k < nextSynced; ++k) {
                        int e = t.head - k;
                        if (e < 0) e += t.P;
                        const size_t eoff = (size_t)e * t.bins;
                        std::memcpy(&t.fdlRe[eoff], &t.cleanRe[eoff], sizeof(float) * (size_t)t.bins);
                        std::memcpy(&t.fdlIm[eoff], &t.cleanIm[eoff], sizeof(float) * (size_t)t.bins);
                    }
                    t.cleanSynced.store(nextSynced, std::memory_order_release);
                }
            }
        }
        if (t.hoSlot >= 0) ++t.hoAge;
        double ab = 0, aa = 0, bb = 0;  // products of the two outputs of a long crossfade, all channels
        for (int r = 0; r < t.R; ++r) {
            float* dst = &tailTmp[(size_t)(firstRow + r) * B];
            if (fade) {
                if(fromSplit && shared && !jumpHandoverAbort)accumulateFresh(t,t.slot[t.cur],r,accRe.data(),accIm.data());
                else if(fromSplit && !shared && t.slot[t.cur].isJump) {
                    accumulateClean(t,t.slot[t.cur],r,accRe.data(),accIm.data());
                } else if(fromSplit && !shared) {
                    accumulate(t,t.slot[t.cur],r,accRe.data(),accIm.data());
                    accumulateFresh(t,t.slot[t.cur],r,accRe2.data(),accIm2.data());
                    for(int b=0;b<t.bins;++b){accRe[b]+=accRe2[b];accIm[b]+=accIm2[b];}
                } else accumulate(t, t.slot[t.cur], r, accRe.data(), accIm.data());
                inverse(accRe.data(), accIm.data(), blkA.data());
                if (fromSplit) {
                    accumulateHandover(t,oldCur,r,accRe.data(),accIm.data());
                    inverse(accRe.data(),accIm.data(),blkB.data());
                } else {
                    accumulate(t,t.slot[t.prev],r,accRe2.data(),accIm2.data());
                    inverse(accRe2.data(),accIm2.data(),blkB.data());
                }
                for (int i = 0; i < B; ++i) {
                    float w = (i + 0.5f) / B;
                    dst[i] += w * blkA[i] + (1 - w) * blkB[i];
                }
            } else if (t.longN > 0) {
                // E4a long crossfade: equal power, corrected for the correlation rho of the two IRs (rho = 1: linear)
                accumulate(t, t.slot[t.cur], r, accRe.data(), accIm.data());
                inverse(accRe.data(), accIm.data(), blkA.data());
                accumulate(t, t.slot[t.prev], r, accRe2.data(), accIm2.data());
                inverse(accRe2.data(), accIm2.data(), blkB.data());
                // The correlation that matters is the one of the two outputs for the actual input (its spectrum weights
                // the IRs' bands), so it is measured on them (all channels, smoothed over blocks) and glides from the
                // previous block's value to the newest within a block.
                for (int i = 0; i < B; ++i) { ab += (double)blkA[i] * blkB[i]; aa += (double)blkA[i] * blkA[i]; bb += (double)blkB[i] * blkB[i]; }
                const float inv = 1.f / (float)((size_t)t.longN * B);
                for (int i = 0; i < B; ++i) {
                    const float u = ((float)((size_t)t.longAt * B + i) + 0.5f) * inv;
                    const float sN = 0.5f - 0.5f * std::cos((float)kPi * u), aO = 1.f - sN;
                    const float rh = t.rhoA + (t.rho - t.rhoA) * (float)(i + 1) / (float)B;
                    const float k = 1.f / std::sqrt(std::max(1e-6f, aO * aO + sN * sN + 2.f * rh * aO * sN));
                    dst[i] += k * (sN * blkA[i] + aO * blkB[i]);
                }
            } else if (t.hoSlot >= 0) {
                accumulateHandover(t,t.cur,r,accRe.data(),accIm.data());
                inverse(accRe.data(),accIm.data(),blkA.data());
                for(int i=0;i<B;++i)dst[i]+=blkA[i];
            } else {
                accumulate(t, t.slot[t.cur], r, accRe.data(), accIm.data());
                inverse(accRe.data(), accIm.data(), blkA.data());
                for (int i = 0; i < B; ++i) dst[i] += blkA[i];
            }
        }
        if (fade) {
            t.curAtomic.store(t.cur);
            t.appliedRevision.store(t.slot[t.cur].revision,std::memory_order_release);
            t.appliedScene.store(t.slot[t.cur].sceneRevision,std::memory_order_release);
            t.pending.store(-1);
            if (fromSplit) {
                if(shared){
                    if (!jumpHandoverAbort) promoteFresh(t);
                    t.hoSlot=-1;t.hoAge=0;
                    t.publishedHandover.store(-1,std::memory_order_release);
                } else if (t.slot[t.cur].isJump) {
                    std::memcpy(t.prevIn.data(), t.cleanPrev.data(), sizeof(float) * B);
                    t.hoSlot = -1; t.hoAge = 0;
                    t.legacyHandover = false; t.longFromPending = false;
                    t.publishedHandover.store(-1, std::memory_order_release);
                    t.cleanSynced.store(0, std::memory_order_release);
                } else {t.hoSlot=t.cur;t.legacyHandover=true;t.publishedHandover.store(t.cur,std::memory_order_release);}
            }
            stLegacySwaps.fetch_add(1, std::memory_order_relaxed);
        } else if (t.longN > 0) {
            const double sm = t.longAt == 0 ? 0.0 : 0.75;
            t.cAB = sm * t.cAB + ab; t.cAA = sm * t.cAA + aa; t.cBB = sm * t.cBB + bb;
            t.rhoA = t.rho;
            if (t.cAA > 1e-30 && t.cBB > 1e-30) t.rho = (float)std::min(1.0, std::max(0.0, t.cAB / std::sqrt(t.cAA * t.cBB)));
            if (++t.longAt >= t.longN) {
                t.longN = 0; t.longAt = 0; t.prev = -1;
                if (t.longFromPending) {
                    t.longFromPending = false;
                    t.curAtomic.store(t.cur);
                    t.appliedRevision.store(t.slot[t.cur].revision,std::memory_order_release);
                    t.appliedScene.store(t.slot[t.cur].sceneRevision,std::memory_order_release);
                    t.pending.store(-1);
                    stTransDone.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else if (t.hoSlot >= 0 && t.hoAge >= std::max(t.slot[t.hoSlot].partitions, t.slot[t.cur].partitions)+t.inputFadeBlocks+2) {
            if (jumpHandoverAbort && shared) {
                t.hoSlot = -1; t.hoAge = 0;
                t.publishedHandover.store(-1, std::memory_order_release); t.legacyHandover = false;
            } else {
                promoteFresh(t);
                t.cur = t.hoSlot; t.hoSlot = -1; t.hoAge = 0;
                t.publishedHandover.store(-1,std::memory_order_release);t.legacyHandover=false;
                t.curAtomic.store(t.cur);
                t.appliedRevision.store(t.slot[t.cur].revision,std::memory_order_release);
                t.appliedScene.store(t.slot[t.cur].sceneRevision,std::memory_order_release);
                if(t.longFromPending){t.longFromPending=false;t.pending.store(-1);stTransDone.fetch_add(1,std::memory_order_relaxed);}
            }
        }
    }

    void tailBlock() {
        std::fill(tailTmp.begin(), tailTmp.end(), 0.f);
        if (shared) {
            for (int r = 0; r < R; ++r) convBlock(*sconv[(size_t)r], r);
            const int ts=transState.load(std::memory_order_relaxed);
            if(ts==2 && gcount>=handoverStart+(uint64_t)trans.fadeBlocks*B)transState.store(3,std::memory_order_release);
            if(ts==2 || ts==3) {
                bool done=true;for(auto& t:sconv)done &= t->hoSlot<0;
                if(done) {
                    if(!handoverLegacy) {
                        for(int r=0;r<R;++r)for(int s=0;s<S;++s)for(int b=0;b<8;++b) {
                            size_t i=((size_t)r*S+s)*8+b;float c=trans.cmul[(size_t)r*8+b];
                            lateGuse[i]=motionExact&&gainMode!=2?trans.gNew[i]:lateGuse[i]*c;
                            glide0[i]*=c;lateGlocal[i]=motionExact?trans.gNew[i]:lateGlocal[i]*c;
                        }
                        lateGtarget=lateGlocal.data();audioEpoch.store(trans.epoch,std::memory_order_release);
                        motionExact=false;if(gainMode!=2)gainMode=1;
                    }
                    jumpHandoverAbort=false;
                    transState.store(0,std::memory_order_release);stTransDone.fetch_add(1,std::memory_order_relaxed);
                }
            } else {
                jumpHandoverAbort=false;
            }
        } else {
            for (int s = 0; s < S; ++s) convBlock(srcs[(size_t)s]->tail, 0);
        }
        const size_t tr = tailMask + 1;
        for (int r = 0; r < R; ++r)
            for (int i = 0; i < B; ++i) tailRing[(size_t)r * tr + ((gcount + i) & tailMask)] = tailTmp[(size_t)r * B + i];
    }
    std::vector<float> blkA, blkB;

    void process(const float* const* in, float* const* out, int n, const float* dryDetector) {
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
                    if (!shared) {
                        auto& t=sp.tail;const bool frozen=requestedFreeze.load(std::memory_order_relaxed);
                        const float cleanSample = frozen ? 0.f : x;
                        t.cleanIn[(gcount + j) % B] = cleanSample;
                        if(t.hoSlot>=0) {
                            const double u=std::clamp(double(gcount+j-t.inputStart)/(t.inputFadeBlocks*B),0.,1.);
                            const float w=t.legacyHandover?1.f:(float)(.5-.5*std::cos(kPi*u));
                            if(w==0){t.inAcc[(gcount+j)%B]=cleanSample;t.freshIn[(gcount+j)%B]=0;}
                            else if(w==1){t.inAcc[(gcount+j)%B]=0;t.freshIn[(gcount+j)%B]=cleanSample;}
                            else {
                                const float* bands=&sp.ring[(((size_t)(gcount+j))&ringMask)*8];float v=0;
                                for(int b=0;b<NB;++b){float k=std::pow((1-w)*(1-w)+w*w+2*t.bandRho[b]*w*(1-w),-.35f);v+=k*bands[b];}
                                t.inAcc[(gcount+j)%B]=frozen?0.f:v*(1-w);t.freshIn[(gcount+j)%B]=frozen?0.f:v*w;
                            }
                        } else t.inAcc[(gcount+j)%B]=cleanSample;
                    }
                }
            }
            if (shared) {
                // late-field feed: per receiver, the sum over sources of band-weighted source signals
                for (int r = 0; r < R; ++r) {
                    TailConv& t = *sconv[(size_t)r];
                    float* y = &lateY[(size_t)r * ctrl];
                    if(t.hoSlot<0) {
                    std::fill(y, y + m, 0.f);
                    for (int s = 0; s < S; ++s) {
                        const float* g = &lateGuse[((size_t)r * S + s) * 8];
                        const float* fr = srcs[(size_t)s]->ring.data();
                        for (int j = 0; j < m; ++j) {
                            const float* f = fr + (((size_t)(gcount + j)) & ringMask) * 8;
                            y[j] += g[0] * f[0] + g[1] * f[1] + g[2] * f[2] + g[3] * f[3] + g[4] * f[4] + g[5] * f[5] + g[6] * f[6];
                        }
                    }
                    }
                    if(t.hoSlot>=0) {
                        for(int j=0;j<m;++j) {
                            float oldValue=0,newValue=0;
                            for(int s=0;s<S;++s) {
                                const float* f=srcs[s]->ring.data()+(((size_t)(gcount+j)&ringMask)*8);
                                float go[7],gn[7];for(int b=0;b<7;++b) {
                                    const size_t i=((size_t)r*S+s)*8+b;
                                    go[b]=feedA0[i]+feedAd[i]*(float)(ph+j);
                                    gn[b]=feedB0[i]+feedBd[i]*(float)(ph+j);
                                }
                                oldValue+=go[0]*f[0]+go[1]*f[1]+go[2]*f[2]+go[3]*f[3]+go[4]*f[4]+go[5]*f[5]+go[6]*f[6];
                                newValue+=gn[0]*f[0]+gn[1]*f[1]+gn[2]*f[2]+gn[3]*f[3]+gn[4]*f[4]+gn[5]*f[5]+gn[6]*f[6];
                            }
                            const bool frozen=requestedFreeze.load(std::memory_order_relaxed);
                            t.inAcc[(gcount+j)%B]=frozen?0.f:oldValue;
                            t.freshIn[(gcount+j)%B]=frozen?0.f:newValue;
                        }
                    } else for (int j = 0; j < m; ++j) t.inAcc[(gcount + j) % B] = requestedFreeze.load(std::memory_order_relaxed) ? 0.f : y[j];
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
                    creativeDirect[(size_t)r*ctrl+j]=componentMix[j]*acc[j];
                    creativeTail[(size_t)r*ctrl+j]=tg*tailRing[(size_t)r*tr+((gcount+j)&tailMask)];
                    // Preserve the exact pre-E1 arithmetic on the neutral path.
                    out[r][done+j]=componentMix[j]*acc[j]+componentMix[ctrl+j]*ear[j]
                        +componentMix[2*ctrl+j]*tg*tailRing[(size_t)r*tr+((gcount+j)&tailMask)];
                }
            }
            const double pd=requestedPredelay.load(std::memory_order_relaxed);
            const float width=(float)(requestedWidth.load(std::memory_order_relaxed)*.01);
            const float duck=(float)requestedDuck.load(std::memory_order_relaxed);
            const bool frozen=requestedFreeze.load(std::memory_order_relaxed);
            wetDuck.settings(requestedRelease.load(std::memory_order_relaxed));
            // Freeze: block-wise; when idle it only copies the tail into a short history (Creative)
            // or does nothing (Physical), and the held tail is the tail itself (bit-exact).
            tailFreeze.setArmed(freezeAvailable.load(std::memory_order_relaxed));
            const float* heldTail=tailFreeze.process(creativeTail.data(),creativeHeld.data(),ctrl,R,m,frozen)?creativeHeld.data():creativeTail.data();
            for(int j=0;j<m;++j) {
                wetPredelay.begin(pd);
                float detector=0;
                if(dryDetector)detector=dryDetector[done+j];
                else {double power=0;for(int s=0;s<S;++s)power+=(double)in[s][done+j]*in[s][done+j];detector=(float)std::sqrt(power);}
                const float dg=wetDuck.process(detector,duck);
                const float wstep=1.f/(float)(.02*sr);
                widthCurrent=std::abs(width-widthCurrent)<=wstep?width:widthCurrent+(width>widthCurrent?wstep:-wstep);
                float* wet=creativeFrame.data();
                bool neutral=true;
                for(int r=0;r<R;++r) {
                    const float originalTail=creativeTail[(size_t)r*ctrl+j];
                    const float held=heldTail[(size_t)r*ctrl+j];
                    const float early=componentMix[ctrl+j]*tmpEarly[(size_t)r*ctrl+j];
                    const float raw=early+componentMix[2*ctrl+j]*held;
                    const float delayed=wetPredelay.process(r,raw);
                    wet[r]=delayed;
                    neutral&=held==originalTail&&delayed==raw;
                }
                if(widthCurrent!=1)for(auto pair:widthPairs)widthPair(wet[pair.first],wet[pair.second],widthCurrent);
                if(!neutral||widthCurrent!=1||dg!=1)for(int r=0;r<R;++r)out[r][done+j]=creativeDirect[(size_t)r*ctrl+j]+wet[r]*dg;
                wetPredelay.end();
            }
            publishedDuck.store(wetDuck.reductionDb(),std::memory_order_relaxed);
            gcount += m;
            done += m;
            if (ph + m == ctrl) endPeriod();
            if (gcount % B == 0) tailBlock();
        }
        publishedDiffractionMisses.store(diffractionScratch.capacityMisses + quickScratch.capacityMisses,std::memory_order_relaxed);
        streamPos.store(gcount, std::memory_order_release);
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
bool Engine::setScene(const Scene& s, double absorption, const std::function<bool()>& cancelled, double poseScale) { return p_->setScene(s,absorption,cancelled,poseScale); }
void Engine::setReceiverSpread(double m) { p_->params[p_->S * 4 + 4].store(m); }
void Engine::setEarlyLevel(double dB) { p_->params[p_->S * 4 + 5].store(dB); }
void Engine::setEarlyDiffusion(double a) { p_->params[p_->S * 4 + 6].store(a); }
void Engine::setTailLevel(double dB) { p_->params[p_->S * 5 + 8].store(dB); }
void Engine::acknowledgePoseScale(double scale){p_->poseScaleReady.store(scale,std::memory_order_release);}
void Engine::setCreativeWet(double pd,double width,bool freeze,double duck,double release) {
    p_->requestedPredelay.store(clampTo(pd,0.,250.,0.),std::memory_order_relaxed);
    p_->requestedWidth.store(clampTo(width,0.,200.,100.),std::memory_order_relaxed);
    p_->requestedFreeze.store(freeze,std::memory_order_relaxed);
    p_->requestedDuck.store(clampTo(duck,0.,24.,0.),std::memory_order_relaxed);
    p_->requestedRelease.store(clampTo(release,50.,2000.,300.),std::memory_order_relaxed);
}
float Engine::duckReductionDb() const {return p_->publishedDuck.load(std::memory_order_relaxed);}
void Engine::setFreezeAvailable(bool available) {p_->freezeAvailable.store(available,std::memory_order_relaxed);}
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
void Engine::notifyJump() { p_->jumpSerial.fetch_add(1, std::memory_order_acq_rel); }
void Engine::process(const float* const* in, float* const* out, int n, const float* detector) { p_->process(in, out, n, detector); }
int Engine::activeTaps() const { return p_->tapsActive; }
int Engine::tailRefreshCount() const { return p_->refreshCount.load(); }
bool Engine::autoLevelMoving() const {return p_->streamPos.load(std::memory_order_relaxed)<p_->autoMovingUntil.load(std::memory_order_relaxed);}
std::shared_ptr<const AutoLevelEnergy> Engine::autoLevelEnergy() const { return std::atomic_load(&p_->autoEnergy); }
void Engine::runWorkerStep() { if (p_->shared) p_->sharedWorkerStep(); else p_->perSourceWorkerStep(); }
Engine::MotionStats Engine::motionStats() const {
    MotionStats m;
    const auto& q = *p_;
    m.legacySwaps = q.stLegacySwaps.load(); m.transitionsStarted = q.stTransStarted.load(); m.transitionsDone = q.stTransDone.load();
    m.estimates = q.stEstimates.load(); m.held = q.stHeld.load(); m.published = q.stPublished.load(); m.accepted = q.stAccepted.load();
    m.retraceCancelled = q.stCancelled.load(); m.transState = q.transState.load(); m.audioEpoch = q.audioEpoch.load();
    m.lastEstimateMs = q.stLastEstMs.load(); m.maxEstimateMs = q.stMaxEstMs.load(); m.lastRetraceMs = q.stLastRetraceMs.load(); m.maxRetraceMs = q.stMaxRetraceMs.load();
    m.lastEstimateWork = q.stLastWork.load();
    for (int i = 0; i < 4; ++i) m.refListener[i] = q.stRefLst[(size_t)i].load();
    return m;
}
uint64_t Engine::lateStateHash() const {
    const auto& q = *p_;
    uint64_t h = 1469598103934665603ull;
    auto add = [&](const void* d, size_t n) { auto b = (const unsigned char*)d; for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; } };
    std::vector<const TailConv*> convs;
    for (const auto& c : q.sconv) convs.push_back(c.get());
    if (!q.shared) for (const auto& sp : q.srcs) convs.push_back(&sp->tail);
    for (const TailConv* c : convs) {
        const Slot& sl = c->slot[c->hoSlot >= 0 ? c->hoSlot : c->cur];
        add(&sl.partitions, sizeof sl.partitions);
        add(sl.re.data(), sl.re.size() * sizeof(float)); add(sl.im.data(), sl.im.size() * sizeof(float));
        const int busy = c->longN > 0 || c->hoSlot >= 0 || c->pending.load() >= 0;
        add(&busy, sizeof busy);
    }
    add(q.lateGuse.data(), q.lateGuse.size() * sizeof(float));
    return h;
}
std::vector<double> Engine::lateAppliedEnergy() const {
    const auto& q = *p_;
    std::vector<double> out;
    if (!q.shared) return out;
    std::vector<double> Ne;
    {
        std::lock_guard<std::mutex> lk(q.epochMx);
        for (const auto& e : q.epochN) if (e.first == q.audioEpoch.load()) Ne = e.second;
    }
    if (Ne.empty()) return out;
    std::vector<double> freshN;
    if(q.transState.load(std::memory_order_acquire)>=2) {
        std::lock_guard<std::mutex> lk(q.epochMx);
        for(const auto& e:q.epochN)if(e.first==q.trans.epoch)freshN=e.second;
    }
    out.assign((size_t)q.R * q.S * NB, 0.0);
    for (int r = 0; r < q.R; ++r)
        for (int s = 0; s < q.S; ++s)
            for (int b = 0; b < NB; ++b) {
                const double g = q.lateGuse[((size_t)r * q.S + s) * 8 + b];
                double energy=g*g*Ne[(size_t)r*NB+b];
                // Instantaneous injected model energy (not the delayed response):
                // both causal banks, their own normalization and covariance.
                if(!freshN.empty() && q.sconv[r]->hoSlot>=0) {
                    size_t i=((size_t)r*q.S+s)*8+b;
                    double a=q.feedA0[i]+q.feedAd[i]*(q.cfg.ctrlInterval-1),n=q.feedB0[i]+q.feedBd[i]*(q.cfg.ctrlInterval-1);
                    double ea=Ne[(size_t)r*NB+b],en=freshN[(size_t)r*NB+b];
                    energy=a*a*ea+n*n*en+2*q.sconv[r]->bandRho[b]*a*n*std::sqrt(ea*en);
                }
                out[((size_t)r * q.S + s) * NB + b] = energy;
            }
    return out;
}
uint64_t Engine::diffractionCapacityMisses() const { return p_->publishedDiffractionMisses.load(std::memory_order_relaxed); }
Engine::DiffractionStats Engine::diffractionStats() const { return p_->dstats; }
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
    // E4a: during a settle transition the target state (the static engine's IR and gains) is copied.
    const bool transition=src.shared&&src.transState.load(std::memory_order_acquire)!=0;
    int rowIndex=0;
    auto copy=[&](TailConv& to,const TailConv& from) {
        const int r=rowIndex++;
        const auto& active=from.slot[transition?src.trans.slotH[(size_t)r]:from.curAtomic.load(std::memory_order_acquire)];
        auto& target=to.slot[to.cur];target=active;
        target.revision=dst.qualitySerial.load();target.sceneRevision=dst.geomSerial.load();
        to.pending=-1;to.appliedRevision=target.revision;to.appliedScene=target.sceneRevision;
        longest=std::max(longest,target.samples);
    };
    if(dst.shared) {
        for(int r=0;r<dst.R;++r)copy(*dst.sconv[r],*src.sconv[r]);
        // Gain buffers are immutable to audio and protected from worker reuse here.
        const auto& gain=src.lateGbuf[src.lateGcur.load(std::memory_order_acquire)];
        dst.lateGbuf[0]=dst.lateGbuf[1]=dst.lateGuse=dst.lateGlocal=gain;
        dst.lateGcur=0;dst.lateGtarget=dst.lateGlocal.data();dst.lateGseen=dst.lateGserial.load();dst.lateGseenPub=dst.lateGseen;dst.gainMode=0;
    } else for(int s=0;s<dst.S;++s)copy(dst.srcs[s]->tail,src.srcs[s]->tail);
    dst.renderedSeconds.store(longest/dst.sr);
    return true;
}

}  // namespace verb

