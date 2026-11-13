// Late-field model: ray-traced energy histogram -> decorrelated band noise -> complementary filterbank.
#pragma once
#include <array>
#include <vector>
#include <functional>

#include "scene.hpp"

namespace verb {

using BandBuf = std::vector<std::array<std::vector<double>, NB>>;  // [receiver][band][sample]

struct TailHist {
    size_t nBins = 0;
    double binDur = 0.001;
    std::vector<std::array<std::vector<double>, NB>> h;  // [receiver][band][bin]: IR energy per bin
    std::vector<std::array<std::vector<double>, NB>> h2;  // [receiver][band][bin]: sum of the squared contributions (to tell a few strong rays from many)
    std::vector<std::array<size_t, NB>> reliableEnd;      // [receiver][band]: first bin from which the histogram rests on too few rays and is replaced by the extrapolated decay
    std::array<double, NB> effectiveMultipliers{{1,1,1,1,1,1,1}};
    // 0 unchanged/applied, 1 unmeasurable, 2 below 1 ms resolution, 3 duration limited, 4 fit rejected.
    std::array<int, NB> reshapeStatus{};
};

// Traces `rays` rays from `src`; only energy that has reflected more than `order` times is recorded
// (lower orders are covered by the deterministic image-source part).
// finalize=false retains raw first/second moments so independent traces can be
// pooled before estimating confidence and decay. Finalize a histogram only once.
TailHist traceTail(const Scene& sc, const Transducer& src, const std::vector<Transducer>& rcv, int order, int rays, int seed, bool finalize = true, const std::function<bool()>& cancelled = {});
// Integrated late energy for offline spatial analysis. No time histogram or
// extrapolation is allocated; callbacks run only on the caller's worker thread.
std::vector<std::array<double, NB>> traceTailEnergy(const Scene&, const Transducer&,
    const std::vector<Transducer>&, int order, int rays, int seed,
    const std::function<bool()>& cancelled = {}, const std::function<void(double)>& progress = {});
// E4a: the cheap late-energy estimate that lets the late field follow a moving listener/source. Same physics as traceTail
// (transmission, occlusion, detection-sphere clipping), integrated over time, but ray i uses its own random generator seeded
// from (seed, i): the same ray set traced from two nearby poses gives strongly correlated results (common random numbers), so
// the RATIO of two such estimates is far less noisy than either. stopDb < 0 ends a ray once its energy is that far below a
// full-sphere start (what it would still add is below that fraction of its total). `work` returns deterministic work units.
std::vector<std::array<double, NB>> traceTailEnergyCRN(const Scene&, const Transducer&, const std::vector<Transducer>&,
    int order, int rays, int seed, double stopDb, const std::function<bool()>& cancelled = {}, long long* work = nullptr,
    std::vector<std::array<double, NB>>* sumSq = nullptr);  // sumSq: per receiver/band sum of squared per-ray totals (variance)
// E4a: the same late transfer, traced backwards from the listener (head centre `origin`) to every source at once; see
// tail.cpp. Result [s * ears.size() + r][band]. The motion estimate is the ratio of two such traces (now / reference).
std::vector<std::array<double, NB>> traceLateReciprocal(const Scene&, Vec3 origin, const std::vector<Transducer>& ears,
    const std::vector<Transducer>& sources, int order, int rays, int seed, double stopDb, const std::function<bool()>& cancelled = {}, long long* work = nullptr,
    std::vector<std::array<double, NB>>* sumSq = nullptr);
// Length probes use the legacy envelope so scene duration remains compatible.
// Production tails additionally regularize the diffuse transition using raw h2.
void finalizeTail(TailHist& hist, bool smoothTransition = true);

// Immutable UI snapshot, generated off the audio thread from the modeled tail.
struct TailDiagnostics {
    static constexpr size_t points = 256;
    std::array<std::array<float, points>, NB> decayDb{};
    std::array<double, NB> rt60{}, physicalRT60{}, reliableSeconds{};
    std::array<std::array<float, points>, NB> physicalDecayDb{};
    std::array<double, 3> multipliers{{1, 1, 1}};
    std::array<double, NB> effectiveMultipliers{{1,1,1,1,1,1,1}};
    std::array<int, NB> reshapeStatus{};
    std::array<bool, NB> active{};
    // Independent tails may reach different limits; retain every source's status
    // instead of silently reporting only source zero. Shared tails use one model.
    std::vector<std::array<double,NB>> sourceEffectiveMultipliers;
    std::vector<std::array<int,NB>> sourceReshapeStatus;
    double duration = 0;
    int raysUsed = 0, rayLimit = 0, baseRays = 0;
    bool shared = false, adaptive = false, needsMoreRays = false;
};

// Is a significant part of the energy already relying on a sparse population?
bool tailNeedsMoreRays(const TailHist& raw, const TailHist& finalized);
TailHist traceTailAdaptive(const Scene&, const Transducer&, const std::vector<Transducer>&,
                           int order, int baseRays, int maxRays, int seed, int& raysUsed, bool* needsMore = nullptr, const std::function<bool()>& cancelled = {});
TailDiagnostics analyzeTail(const TailHist& finalized);
// Worker/offline only. Unity is an exact no-op; extends the finalized histogram up to 20 seconds.
void reshapeTailDecay(TailHist&, const std::array<double, 3>& multipliers);
// Expand seeded surfaces before scaling so generated clutter is included; materials in the input are untouched.
Scene withAbsorptionScale(const Scene&, double scale);
std::array<double, NB> estimateRoomRT60(const Scene&);

// How long the late tail has to be so that nothing audible is cut off: the time (a quick trace of the room, mid bands) at
// which the remaining energy has fallen 80 dB, plus a margin for the final fade. The scene's own length is the minimum; at most 20 s.
double effectiveTailSeconds(const Scene& sc, int erOrder, const std::function<bool()>& cancelled = {});

// Fades the last part of every channel of a tail IR to silence (a raised cosine), so that a tail that is still audible at the
// end of its length ends smoothly instead of being cut.
void fadeTailEnd(std::vector<std::vector<float>>& ir, size_t n, double sr);

// Adds independent Gaussian noise shaped by the histogram to every receiver/band buffer.
void addTailNoise(const Scene& sc, const TailHist& hist, int seed, BandBuf& buf);

// Sums the bands through a linear-phase complementary filterbank (delay-compensated). Output length = buffer length.
std::vector<std::vector<float>> combineBands(const BandBuf& buf, double sr);

}  // namespace verb
