#pragma once
#include "dsp.hpp"
#include "scene.hpp"

namespace verb {

// Renders one multichannel IR (one channel per receiver) for the given source.
Wav renderIR(const Scene& s, int sourceIdx, bool verbose);

// Reverberation time from backward-integrated energy (extrapolated from -5..-25 dB). Returns <0 on failure.
double estimateRT60(const std::vector<float>& ir, double sr);

}  // namespace verb
