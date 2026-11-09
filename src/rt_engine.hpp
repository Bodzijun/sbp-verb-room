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
    int spatialTaps = 1;
    double spatialRadius = 0.3;  // initial spread in metres, can be automated with setReceiverSpread
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
    // IR length are kept). Early paths follow within one control period; the late field is re-traced in the
    // background and crossfaded in. Returns false (and changes nothing) if the scene does not fit the engine.
    // Any thread, lock-free for the audio thread.
    bool setScene(const Scene& scene);

    // Thread-safe; positions in metres, angles in degrees.
    void setSourcePosition(int src, double x, double y, double z);
    void setSourceAzimuth(int src, double deg);
    void setListener(double x, double y, double z, double yawDeg);
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
    void setDopplerStrength(double k);  // 0 = none, 1 = physical, up to 2 = exaggerated; live
    // 0..1: smears every early reflection (not the direct sound) in time with energy-preserving all-pass
    // diffusers, like rough real surfaces do. Turns the regular comb ("flanger/phaser") of mirror-like
    // reflections into an irregular, diffuse colouration.
    void setEarlyDiffusion(double amount);
    // Note: a mono voice on a stereo track arrives identically in every input; fed to several sources in different
    // places it adds coherently with deep, position-dependent phase holes (see rt_test, Test 9). Hosts/plugins
    // should sum such inputs into ONE source before calling process().

    // Audio thread. in[numSources()], out[numReceivers()], any n >= 0.
    void process(const float* const* in, float* const* out, int n);

    // Diagnostics (audio thread safe).
    int activeTaps() const;
    int tailRefreshCount() const;
    // Message/control threads only. Immutable snapshot; never called by process().
    std::shared_ptr<const TailDiagnostics> tailDiagnostics() const;
    // Actual immutable modeled geometry, including the seeded roughness/clutter.
    // Control threads only; retaining it never involves the audio callback.
    std::shared_ptr<const Scene> sceneSnapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace verb
