#pragma once

#include "baked_path.hpp"
#include "tail.hpp"

namespace verb {

struct Geo;
using MotionEnvelope = std::array<std::array<float, NB>, kBakeWinCount>;
inline constexpr int kMaxMotionChannels = 16;

// Two reciprocal slices of the late field, referenced to the scene anchors.
// Construction and tracing are worker-only. Queries only read immutable data.
class MotionGrid {
public:
    MotionGrid(const Geo&, Vec3 source, Vec3 listener, int order, int rays,
               const TailExecution&, const std::vector<Transducer>& receivers = {});
    MotionEnvelope sourceField(Vec3) const noexcept;
    MotionEnvelope listenerField(Vec3) const noexcept;
    MotionEnvelope sourceFieldAndChannels(Vec3, MotionEnvelope* outCh, int maxCh) const noexcept;
    MotionEnvelope listenerFieldAndChannels(Vec3, MotionEnvelope* outCh, int maxCh) const noexcept;
    void applyCloseness(MotionEnvelope&, const Transducer& source, Vec3 listener,
                        MotionEnvelope* chEnvelopes = nullptr, int numCh = 0) const noexcept;
    size_t cells() const noexcept { return cells_.size(); }
    int channels() const noexcept { return channels_; }
    int numChannels() const noexcept { return channels_; }
    size_t memoryBytes() const noexcept {
        return sizeof(*this) + cells_.size() * sizeof(Cell) +
               (sourceCh_.size() + listenerCh_.size()) * sizeof(MotionEnvelope) +
               walls_.size() * sizeof(Wall);
    }

private:
    struct Cell {
        Vec3 position{};
        bool interior = false;
        MotionEnvelope source{}, listener{};
    };
    MotionEnvelope lookup(Vec3, bool source, MotionEnvelope* outCh = nullptr, int maxCh = 0) const noexcept;
    bool blocked(Vec3, Vec3) const noexcept;

    int nx_ = 0, ny_ = 0, layers_ = 1, channels_ = 1;
    double x0_ = 0., y0_ = 0., dx_ = 1., dy_ = 1.;
    std::array<double, 2> heights_{};
    std::vector<Cell> cells_;
    std::vector<MotionEnvelope> sourceCh_, listenerCh_; // [cellIndex * channels_ + r]
    std::vector<Wall> walls_;
};

} // namespace verb
