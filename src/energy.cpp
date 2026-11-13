#include "energy.hpp"
#include "geometry.hpp"
#include "dsp.hpp"
#include "tail.hpp"
#include <limits>
#include <stdexcept>

namespace verb {
FieldEnergy earlyFieldEnergy(const Scene& sc, const Transducer& src, Vec3 rc, double earlyDb) {
    const Geo g(sc);
    const double c = speedOfSound(sc.tempC);
    FieldEnergy result;
    auto deposit = [&](BandEnergy& dest, double length, double direction, const float* amp, double level) {
        if (length / c >= .75) return; // the plug-in's early delay horizon
        const double spread = direction / std::max(.05, length);
        for (int b = 0; b < NB; ++b)
            dest[b] += level * spread * spread * amp[b] * amp[b]
                * std::pow(10., -airAbsDbPerM(kBandHz[b], sc.tempC, sc.humidity) * length / 10.);
    };
    float direct[NB]; std::fill_n(direct, NB, 1.f);
    const bool clear = transmitLeg(g, src.pos, rc, -1, -1, direct);
    const bool screened = direct[3] < .5f;
    if (clear) {
        if (sc.diffraction) litEdgeAttenuation(g, src.pos, rc, c, direct);
        deposit(result.direct, norm(rc - src.pos), dirGain(src, unit(rc - src.pos)), direct, 1);
    }
    if (sc.diffraction && (!clear || screened)) {
        DiffPath path;
        bool wall=diffractedPath(g,src.pos,rc,c,path,clear);DiffPath plate;
        bool rim=singleEdgePath(g,src.pos,rc,c,plate,-1,true);
        if(rim && (!wall || plate.length<path.length))path=plate;
        if (wall || rim)
            deposit(result.direct, path.length, dirGain(src, unit(path.first - src.pos)), path.amp, 1);
    }
    if(g.separatedShell(src.pos,rc,false))return result;
    for (int s = 0; s < g.nSurf(); ++s) {
        if (s == g.ceilId() && !g.hasCeil) continue;
        const Vec3 image = mirror(g, src.pos, s);
        Vec3 point; float amp[NB]; std::fill_n(amp, NB, 1.f);
        if (!crossSurfaceSoft(g, rc, image, s, point, c, amp)
            || !softReflectionLeg(g, rc, point, s, -1, c, amp, 1, src.pos)
            || !softReflectionLeg(g, point, src.pos, s, -1, c, amp, 2)
            || !touchCross(g, rc, point, src.pos, amp)) continue;
        const auto* material = g.matAt(s, point.x, point.y);
        if (!material) continue;
        for (int b = 0; b < NB; ++b) amp[b] *= (float)std::sqrt(reflEnergy(*material, b) * (1 - scatterAt(*material, b)));
        deposit(result.early, norm(point - src.pos) + norm(rc - point), dirGain(src, unit(point - src.pos)), amp, std::pow(10., earlyDb / 10.));
    }
    return result;
}

EnergyMap analyzeEnergyMap(const Scene& input, EnergyMapOptions opt, const std::function<bool()>& cancelled,
    const std::function<void(double)>& progress) {
    auto checkCancel = [&] { if (cancelled && cancelled()) throw std::runtime_error("Analysis cancelled"); };
    if (input.sources.empty()) throw std::runtime_error("No audible spatial sources selected");
    if (!std::isfinite(opt.height) || opt.height <= 0 || opt.height > 30 || !std::isfinite(opt.duration))
        throw std::runtime_error("Invalid analysis height or duration");
    opt.resolution = std::clamp(opt.resolution, 8, 96); opt.rays = std::clamp(opt.rays, 1000, 384000);
    opt.duration = std::clamp(opt.duration, .1, 20.); opt.components &= 7;
    if (!opt.components) throw std::runtime_error("Select Direct, Early or Tail");
    EnergyMap out; out.options = opt;
    out.scene = withClutter(withRoughness(input), 128); out.scene.length = opt.duration;
    const auto& sc = out.scene;
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    auto include = [&](double x, double y) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); };
    for (const auto& w : sc.walls) { include(w.ax, w.ay); include(w.bx, w.by); }
    if (sc.walls.empty()) for (const auto& s : sc.sources) { include(s.pos.x - 4, s.pos.y - 4); include(s.pos.x + 4, s.pos.y + 4); }
    if (x1 - x0 < 1) { x0 -= .5; x1 += .5; } if (y1 - y0 < 1) { y0 -= .5; y1 += .5; }
    const double longest = std::max(x1 - x0, y1 - y0);
    out.nx = std::max(2, (int)std::ceil(opt.resolution * (x1 - x0) / longest));
    out.ny = std::max(2, (int)std::ceil(opt.resolution * (y1 - y0) / longest));
    out.x0 = x0; out.y0 = y0; out.dx = (x1 - x0) / out.nx; out.dy = (y1 - y0) / out.ny;
    out.cells.resize((size_t)out.nx * out.ny); out.valid.assign(out.cells.size(), true);
    std::vector<Transducer> probes;
    std::vector<size_t> indices;
    for (int y = 0; y < out.ny; ++y) for (int x = 0; x < out.nx; ++x) {
        const size_t i = (size_t)y * out.nx + x; const Vec3 pos = out.position(x, y);
        bool valid = !(pos.z >= sc.height && ceilMatAt(sc, pos.x, pos.y) >= 0);
        for (const auto& z : sc.floorZones) if (z.z >= 0 && pos.z <= z.z && pointInZone(z, pos.x, pos.y)) valid = false;
        out.valid[i] = valid;
        if (valid) { Transducer t; t.pos = pos; probes.push_back(t); indices.push_back(i); }
    }
    if (probes.empty()) throw std::runtime_error("No valid map cells at this height");
    const int sources = (int)sc.sources.size();
    for (int s = 0; s < sources; ++s) {
        checkCancel(); const auto& src = sc.sources[s];
        if (opt.components & 3) for (size_t r = 0; r < probes.size(); ++r) {
            if (r % 8 == 0) checkCancel();
            const auto e = earlyFieldEnergy(sc, src, probes[r].pos, opt.earlyDb);
            auto& cell = out.cells[indices[r]];
            for (int b = 0; b < NB; ++b) {
                if (opt.components & 1) cell.direct[b] += e.direct[b];
                if (opt.components & 2) cell.early[b] += e.early[b];
            }
        }
        if (opt.components & 4) {
            const int rays = opt.rays / sources + (s < opt.rays % sources ? 1 : 0);
            const auto e = traceTailEnergy(sc, src, probes, 1, rays, sc.seed + s * 977, cancelled,
                [&](double f) { if (progress) progress((s + .1 + .9 * f) / sources); });
            const double gain = std::pow(10., opt.tailDb / 10.); // source gain is already in dirGain()
            for (size_t r = 0; r < probes.size(); ++r) for (int b = 0; b < NB; ++b) out.cells[indices[r]].tail[b] += gain * e[r][b];
            out.raysUsed += rays;
        }
        if (progress) progress((s + 1.) / sources);
    }
    checkCancel(); return out;
}
}
