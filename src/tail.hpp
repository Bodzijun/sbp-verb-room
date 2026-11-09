// Late-field model: ray-traced energy histogram -> decorrelated band noise -> complementary filterbank.
#pragma once
#include <array>
#include <vector>

#include "scene.hpp"

namespace verb {

using BandBuf = std::vector<std::array<std::vector<double>, NB>>;  // [receiver][band][sample]

struct TailHist {
    size_t nBins = 0;
    double binDur = 0.001;
    std::vector<std::array<std::vector<double>, NB>> h;  // [receiver][band][bin]: IR energy per bin
    std::vector<std::array<std::vector<double>, NB>> h2;  // [receiver][band][bin]: sum of the squared contributions (to tell a few strong rays from many)
    std::vector<std::array<size_t, NB>> reliableEnd;      // [receiver][band]: first bin from which the histogram rests on too few rays and is replaced by the extrapolated decay
};

// Traces `rays` rays from `src`; only energy that has reflected more than `order` times is recorded
// (lower orders are covered by the deterministic image-source part).
// finalize=false retains raw first/second moments so independent traces can be
// pooled before estimating confidence and decay. Finalize a histogram only once.
TailHist traceTail(const Scene& sc, const Transducer& src, const std::vector<Transducer>& rcv, int order, int rays, int seed, bool finalize = true);
void finalizeTail(TailHist& hist);

// Immutable UI snapshot, generated off the audio thread from the modeled tail.
struct TailDiagnostics {
    static constexpr size_t points = 256;
    std::array<std::array<float, points>, NB> decayDb{};
    std::array<double, NB> rt60{}, reliableSeconds{};
    std::array<bool, NB> active{};
    double duration = 0;
    int raysUsed = 0, rayLimit = 0, baseRays = 0;
    bool shared = false, adaptive = false, needsMoreRays = false;
};

// Is a significant part of the energy already relying on a sparse population?
bool tailNeedsMoreRays(const TailHist& raw, const TailHist& finalized);
TailHist traceTailAdaptive(const Scene&, const Transducer&, const std::vector<Transducer>&,
                           int order, int baseRays, int maxRays, int seed, int& raysUsed, bool* needsMore = nullptr);
TailDiagnostics analyzeTail(const TailHist& finalized);

// How long the late tail has to be so that nothing audible is cut off: the time (a quick trace of the room, mid bands) at
// which the remaining energy has fallen 80 dB, plus a margin for the final fade. The scene's own length is the minimum; at most 20 s.
double effectiveTailSeconds(const Scene& sc, int erOrder);

// Fades the last part of every channel of a tail IR to silence (a raised cosine), so that a tail that is still audible at the
// end of its length ends smoothly instead of being cut.
void fadeTailEnd(std::vector<std::vector<float>>& ir, size_t n, double sr);

// Adds independent Gaussian noise shaped by the histogram to every receiver/band buffer.
void addTailNoise(const Scene& sc, const TailHist& hist, int seed, BandBuf& buf);

// Sums the bands through a linear-phase complementary filterbank (delay-compensated). Output length = buffer length.
std::vector<std::vector<float>> combineBands(const BandBuf& buf, double sr);

}  // namespace verb
