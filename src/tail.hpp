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
};

// Traces `rays` rays from `src`; only energy that has reflected more than `order` times is recorded
// (lower orders are covered by the deterministic image-source part).
TailHist traceTail(const Scene& sc, const Transducer& src, const std::vector<Transducer>& rcv, int order, int rays, int seed);

// Adds independent Gaussian noise shaped by the histogram to every receiver/band buffer.
void addTailNoise(const Scene& sc, const TailHist& hist, int seed, BandBuf& buf);

// Sums the bands through a linear-phase complementary filterbank (delay-compensated). Output length = buffer length.
std::vector<std::vector<float>> combineBands(const BandBuf& buf, double sr);

}  // namespace verb
