// Real-time engine.
//  - Early field: per-source 7-band delay line, one interpolated tap per valid image-source path and receiver.
//    Paths are re-solved every `ctrlInterval` samples when the source/listener moved; delays and gains glide
//    linearly between solves (=> physically correct Doppler). Occluded/new paths fade out/in.
//  - Late field: ray-traced tail IR (refreshed by a worker thread when poses change) applied by a uniform
//    partitioned FFT convolution. Tail IRs are shifted by the convolver block so the total latency is zero.
//  - process() never allocates or locks. Setters are thread-safe (atomics, last value wins).
#pragma once
#include <memory>

#include "scene.hpp"
#include "tail.hpp"

namespace verb {

struct EngineConfig {
    std::function<bool()> cancelled; // construction/control only, never called from process()
    int erOrder = 2;          // discrete reflection order (1..4); higher orders come from the tail
    int ctrlInterval = 128;   // path solve / glide period, samples
    int tailBlock = 512;      // convolver block (multiple of ctrlInterval)
    int tailRays = 20000;     // rays per tail refresh
    bool adaptiveTail = false; // refine sparse traces off-thread, up to 4x tailRays
    int tailRefreshMs = 300;  // minimum time between tail refreshes of one source
    bool tailThread = true;   // false: tail is computed once at construction only
    // Many sources: instead of one late-field convolver per source (memory and CPU grow with sources x outputs),
    // keep ONE convolver per output channel and feed it the sum of all sources, each weighted per band and per
    // channel by its traced late-field energy. Exact for a diffuse late field; the temporal shape of the tail is
    // the average over sources. tailRays is then the total ray budget (split over the sources).
    bool sharedTail = false;
    double maxEarlyDelaySec = 0.75;  // length of the per-source delay line (longest early path)
    // Experimental: each output channel is the coherent average over `spatialTaps` (1, 2 or 4) points around the
    // microphone. Measured: it does NOT reduce the Rayleigh-like ripple of a diffuse field (only the comb of a
    // single dominant reflection) and costs spatialTaps x early-field CPU, so it is off by default.
    double absorptionScale = 1.; // applied after seeded geometry generation, never to authored materials
    std::array<double, 3> decayMultipliers{{1, 1, 1}}; // initial creative decay, worker updates below
    // E3d: work units per control period for the multi-end diffraction search on the audio thread (0 = the default,
    // Engine::kDefaultDiffractionUnits). One-end answers are always immediate; see DiffMemo in rt_engine.cpp.
    long long diffractionUnits = 0;
    int spatialTaps = 1;
    double spatialRadius = 0.3;  // initial spread in metres, can be automated with setReceiverSpread
    // E4a: shared late field (sharedTail) while the listener or a source moves.
    //  0 = as before E4a: a full re-trace and a 10 ms IR swap every tailRefreshMs while the pose keeps moving.
    //  1 = no re-trace while moving (late gains held); one full re-trace after the pose has settled, crossfaded in.
    //  2 = as 1, and the late gains follow the move from a cheap estimate on the worker (default).
    // A pose has settled when it stayed within the dead-band of one point for motionSettleSec. The re-trace is then
    // crossfaded over motionFadeSec (equal power, corrected for the correlation of the two IRs) and ends exactly in
    // the state a static engine computes for that pose. Geometry, material, quality, decay and directivity changes
    // keep the immediate re-trace and the short swap. Static scenes are not affected (bit-identical output).
    int motionMode = 2;
    double motionSettleSec = 0.4;
    double motionDeadbandM = 0.008, motionDeadbandDeg = 1.0;  // held-mouse jitter of +-2 mm per axis spans up to 6.9 mm
    double motionFadeSec = 0.75;
    double motionSlewDbPerSec = 24;  // largest change of any late gain while following a move
    double motionHoldDb = 3;         // an estimate whose own standard deviation exceeds this keeps the previous gain
    int motionForwardRays = 128, motionReciprocalRays = 2048;
    int motionIntervalMs = 50;       // shortest time between two estimates
    // Tests: no worker thread even with tailThread; the caller runs the worker with Engine::runWorkerStep().
    bool manualWorker = false;
};

// Last complete worker result, unit source excitation, omni listener energy.
struct AutoLevelEnergy {
    struct Point { std::array<double, NB> direct{}, early{}, late{}; };
    std::vector<Point> points;
};
class Engine {
public:
    Engine(const Scene& scene, const EngineConfig& cfg);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    int numSources() const;
    int numReceivers() const;
    double sampleRate() const;
    double tailSeconds() const;  // length of the late tail the engine renders (at least the scene length, longer for a long decay)

    // Replaces walls, surface materials, floor/ceiling and height while the engine runs (sources, receivers and the
    // topology are kept; IR length is recomputed). Early paths follow within one control period; the late field is re-traced in the
    // background and crossfaded in. Returns false (and changes nothing) if the scene does not fit the engine.
    // Any thread, lock-free for the audio thread.
    bool setScene(const Scene& scene); // retains the current absorption
    bool setScene(const Scene& scene, double absorptionScale, const std::function<bool()>& cancelled = {}, double constructionPoseScale = 1.);

    // Thread-safe; positions in metres, angles in degrees.
    void setSourcePosition(int src, double x, double y, double z);
    void setSourceAzimuth(int src, double deg);
    void setListener(double x, double y, double z, double yawDeg);
    void notifyJump();
    void setReceiverSpread(double metres);  // radius of the pickup cloud (no effect when spatialTaps == 1)
    // Level of the discrete early reflections relative to the direct sound, dB (-60..+6). Lowering it reduces the
    // depth of the comb filtering that direct sound + a strong reflection produce (the "flanger" colour).
    void setEarlyLevel(double dB);
    void setSourcePattern(int s, double p);  // directivity 0 omni .. 0.5 cardioid .. 1 figure-8, live
    void setTailLevel(double dB);  // level of the late field (the reverberant tail) alone, -60..+12 dB, live
    // 0 = all components; bits 1 direct, 2 early, 4 tail. Changes ramp over 10 ms.
    void setSoloMask(int mask);
    // Positioned dry/wet: keep the direct field intact, scale early reflections and tail (0..1, 10 ms ramp).
    void setReverbMix(double amount);
    // Retraces and crossfades the tail on its worker without resetting audio history.
    // With tailThread=false, later quality requests are not applied.
    void setTailQuality(int baseRays, bool adaptive);
    void setDecayMultipliers(double low, double mid, double high); // atomics only; worker rebuild + crossfade
    void acknowledgePoseScale(double constructionRelativeScale); // after publishing every scaled pose, before process()
    void setCreativeWet(double predelayMs, double widthPercent, bool freeze, double duckDb, double duckReleaseMs);
    float duckReductionDb() const;
    // Creative mode: Freeze can be engaged, so a short tail history (one store per output sample) is kept.
    // Physical mode: false -> the Freeze component does no work at all. Default true.
    void setFreezeAvailable(bool available);
    void setDopplerStrength(double k);  // 0 = none, 1 = physical, up to 2 = exaggerated; live
    // 0..1: smears every early reflection (not the direct sound) in time with energy-preserving all-pass
    // diffusers, like rough real surfaces do. Turns the regular comb ("flanger/phaser") of mirror-like
    // reflections into an irregular, diffuse colouration.
    void setEarlyDiffusion(double amount);
    // Note: a mono voice on a stereo track arrives identically in every input; fed to several sources in different
    // places it adds coherently with deep, position-dependent phase holes (see rt_test, Test 9). Hosts/plugins
    // should sum such inputs into ONE source before calling process().

    // Audio thread. in[numSources()], out[numReceivers()], any n >= 0.
    void process(const float* const* in, float* const* out, int n, const float* dryDetector = nullptr);

    // Diagnostics (audio thread safe).
    int activeTaps() const;
    int tailRefreshCount() const;
    uint64_t diffractionCapacityMisses() const; // published by process(); zero with a correctly prepared wall budget
    // E3d: the bounded diffraction search. Plain audio-thread counters: read them only while process() is not running.
    static constexpr long long kDefaultDiffractionUnits = 12000;  // ~60 us per period here (~5 ns per unit)
    struct DiffractionStats {
        long long budget = 0;                   // work units per control period for the multi-end search
        unsigned long long periods = 0;         // control periods so far
        long long unitsLast = 0, unitsMax = 0;  // all diffraction work in one period (one-end + follow + search)
        long long softActualUnitsLast=0,softActualUnitsMax=0;
        unsigned long long softEvaluations=0,softCacheHits=0;
        long long softEdgeUnitsLast = 0, softEdgeUnitsMax = 0; // E5 conservative linear-work bound, separate from resumable search
        long long searchUnitsMax = 0;           // multi-end search work in one period, largest (<= budget + one loop step)
        unsigned long long unitsTotal = 0;
        unsigned long long quick = 0, follows = 0, jobsStarted = 0, jobsFinished = 0, holdPeriods = 0;
        unsigned long long maxLatencyPeriods = 0;  // longest wait from a request to its finished search (control periods)
        unsigned long long latencySumPeriods = 0;  // sum over finished searches (mean = latencySumPeriods / jobsFinished)
        long long jobUnitsMax = 0;            // the largest single search, work units
    };
    DiffractionStats diffractionStats() const;
    bool tailIsCurrent() const; // all audio convolvers have consumed the latest geometry/decay/quality revision
    // Message/control threads only. Immutable snapshot; never called by process().
    std::shared_ptr<const TailDiagnostics> tailDiagnostics() const;
    // Actual immutable modeled geometry, including the seeded roughness/clutter.
    // Control threads only; retaining it never involves the audio callback.
    std::shared_ptr<const Scene> sceneSnapshot() const;
    // Control/offline only: destination must have tailThread=false and no concurrent
    // process(). Copies the reference's audible IR and immutable modeled geometry,
    // never its audio history.
    // Only the reference worker is locked; its process() does not acquire this mutex.
    bool copyTailForOffline(const Engine& reference);

    // E4a. Tests with EngineConfig::manualWorker: one pass of the late-field worker, synchronously (control thread,
    // never while process() runs).
    void runWorkerStep();
    std::shared_ptr<const AutoLevelEnergy> autoLevelEnergy() const;
    bool autoLevelMoving() const;
    struct MotionStats {
        unsigned long long legacySwaps = 0;       // short (one block) IR swaps: geometry, quality, decay, directivity, mode 0
        unsigned long long transitionsStarted = 0, transitionsDone = 0;  // settle re-traces crossfaded in
        unsigned long long estimates = 0, held = 0, published = 0, accepted = 0;  // motion gain cycles; held (r,s,band) values
        unsigned long long retraceCancelled = 0;
        int transState = 0;                       // 0 idle, 1 published, 2 crossfading, 3 normalization handover
        uint64_t audioEpoch = 0;
        double lastEstimateMs = 0, maxEstimateMs = 0, lastRetraceMs = 0, maxRetraceMs = 0;
        long long lastEstimateWork = 0;
        double refListener[4] = {0, 0, 0, 0};    // listener pose of the newest late-field build
    };
    MotionStats motionStats() const;
    // Late energy actually applied per receiver/source/band: (applied gain)^2 x (band energy of the IR it feeds),
    // [(r*S + s)*7 + b]. Control thread, only while process() is not running.
    std::vector<double> lateAppliedEnergy() const;
    // Hash of the late-field state that decides the sound from now on: the IR spectra each shared convolver uses and
    // the gains applied. Equal hashes = the same late field for the same input. Control thread, process() not running.
    uint64_t lateStateHash() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace verb
