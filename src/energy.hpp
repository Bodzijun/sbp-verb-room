#pragma once
#include "scene.hpp"
#include <functional>

namespace verb {
using BandEnergy = std::array<double, NB>;
struct FieldEnergy { BandEnergy direct{}, early{}, tail{}; };
struct EnergyMapOptions {
    int resolution = 24;       // longest dimension, 8..96
    int rays = 48000;          // total budget across spatial source points
    double height = 1.2, duration = 20, earlyDb = -9, tailDb = 0;
    int components = 7;        // direct=1, early=2, tail=4
};
struct EnergyMap {
    Scene scene;
    EnergyMapOptions options;
    int nx = 0, ny = 0, raysUsed = 0;
    double x0 = 0, y0 = 0, dx = 1, dy = 1;
    std::vector<FieldEnergy> cells;
    std::vector<bool> valid;
    Vec3 position(int x, int y) const { return {x0 + (x + .5) * dx, y0 + (y + .5) * dy, options.height}; }
};
// Incoherent, time-integrated band energy for unit excitation at every source
// point. Omni probes; source gains/directivity and the first-order engine's
// transmission/diffraction rules apply. Not a waveform/SPL/modal simulation.
FieldEnergy earlyFieldEnergy(const Scene&, const Transducer&, Vec3 receiver, double earlyDb = -9);
EnergyMap analyzeEnergyMap(const Scene&, EnergyMapOptions,
    const std::function<bool()>& cancelled = {}, const std::function<void(double)>& progress = {});
}
