#include "energy.hpp"
#include "tail.hpp"
#include "rt_engine.hpp"
#include "../plugin/RoomLibrary.h"
#include "bake_pool.hpp"
#include <chrono>
#include <cstdio>
#include <stdexcept>

static int failures = 0;
static void check(bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }
int main() {
    verb::Scene free; verb::Material absorber; absorber.alpha.fill(1); free.mats = {absorber}; free.floorMat = 0; free.ceilMat = -1; free.diffraction = false;
    verb::Transducer source; source.pos = {0, 0, 1.2}; free.sources = {source};
    const auto one = verb::earlyFieldEnergy(free, source, {1, 0, 1.2});
    const auto two = verb::earlyFieldEnergy(free, source, {2, 0, 1.2});
    check(std::fabs(10 * std::log10(two.direct[0] / one.direct[0]) + 6.0206) < .05, "direct energy follows inverse-square distance");
    source.gainDb = -6; const auto quiet = verb::earlyFieldEnergy(free, source, {1, 0, 1.2});
    check(std::fabs(10 * std::log10(quiet.direct[3] / one.direct[3]) + 6) < 1e-8, "source gain scales energy in dB");
    source = free.sources[0]; source.pattern = .5; const auto front = verb::earlyFieldEnergy(free, source, {1, 0, 1.2}), back = verb::earlyFieldEnergy(free, source, {-1, 0, 1.2});
    check(front.direct[3] > .5 && back.direct[3] < 1e-20, "source directivity is preserved");
    free.walls.push_back({.5, -5, .5, 5, 0});
    check(verb::earlyFieldEnergy(free, free.sources[0], {1, 0, 1.2}).direct[3] == 0, "opaque wall blocks direct energy");
    free.mats[0].alpha.fill(0); free.mats[0].tau.fill(.01);
    const auto through = verb::earlyFieldEnergy(free, free.sources[0], {1, 0, 1.2});
    check(std::fabs(10 * std::log10(through.direct[3] / one.direct[3]) + 20) < 1e-5, "wall transmission uses energy rather than amplitude loss");
    verb::Scene room; verb::Material material; material.alpha.fill(.35); material.scatter = .2;
    room.mats = {material}; room.floorMat = room.ceilMat = 0; room.height = 4; room.length = .7; room.detectRadius = .5;
    room.walls = {{0, 0, 8, 0, 0}, {8, 0, 8, 10, 0}, {8, 10, 0, 10, 0}, {0, 10, 0, 0, 0}};
    source = {}; source.pos = {2, 2, 1.2}; room.sources = {source};
    std::vector<verb::Transducer> receivers(12);
    for (int i = 0; i < 12; ++i) receivers[i].pos = {1. + i % 4 * 2, 2. + i / 4 * 3, .8 + (i % 3) * .7};
    const auto hist = verb::traceTail(room, source, receivers, 1, 2500, 27, false);
    const auto total = verb::traceTailEnergy(room, source, receivers, 1, 2500, 27);
    double error = 0, peak = 0;
    for (size_t r = 0; r < receivers.size(); ++r) for (int b = 0; b < verb::NB; ++b) {
        double sum = 0; for (double e : hist.h[r][b]) sum += e;
        error = std::max(error, std::fabs(sum - total[r][b])); peak = std::max(peak, sum);
    }
    check(peak > 0 && error / peak < 1e-12, "accelerated integrated tracing matches the original histogram over varied probe heights");
    std::shared_ptr<const verb::Scene> held;
    {
        auto modeled = room; modeled.roughness = .4; modeled.clutter.count = 2;
        verb::EngineConfig config; config.erOrder = 1; config.tailRays = 1000; config.tailThread = false;
        modeled.receivers = {receivers[0]}; verb::Engine engine(modeled, config);
        held = engine.sceneSnapshot();
        check(held && held->walls.size() == room.walls.size() + 2 && held->mats.size() > room.mats.size(),
            "engine exposes its actual roughened geometry and seeded clutter");
        modeled.height = 5; check(engine.setScene(modeled), "snapshot scene update is accepted");
        check(engine.sceneSnapshot()->height == 5 && held->height == 4, "published geometry snapshots remain immutable across updates");
    }
    check(held && held->height == 4, "geometry snapshot safely outlives its engine");
    verb::EnergyMapOptions options; options.resolution = 8; options.rays = 2500; options.duration = .7;
    const auto map = verb::analyzeEnergyMap(room, options);
    bool finite = true; double late = 0;
    for (const auto& c : map.cells) for (int b = 0; b < verb::NB; ++b) { finite &= std::isfinite(c.direct[b] + c.early[b] + c.tail[b]); late += c.tail[b]; }
    check(finite && late > 0 && map.raysUsed == 2500 && map.cells.size() <= 64, "map computes finite direct/early/tail energy with a bounded grid and ray budget");
    auto quietRoom = room; quietRoom.sources[0].gainDb = -6; auto tailOptions = options; tailOptions.components = 4; tailOptions.tailDb = 3;
    const auto quietMap = verb::analyzeEnergyMap(quietRoom, tailOptions);
    std::vector<verb::Transducer> mapProbes;
    for (int y = 0; y < quietMap.ny; ++y) for (int x = 0; x < quietMap.nx; ++x) { verb::Transducer t; t.pos = quietMap.position(x, y); mapProbes.push_back(t); }
    const auto rawQuiet = verb::traceTailEnergy(quietRoom, quietRoom.sources[0], mapProbes, 1, 2500, quietRoom.seed);
    check(std::fabs(10 * std::log10(quietMap.cells[0].tail[3] / rawQuiet[0][3]) - 3) < 1e-8,
        "map applies source gain once and the configured tail level once");
    options.components = 1; const auto directOnly = verb::analyzeEnergyMap(room, options);
    check(directOnly.raysUsed == 0 && directOnly.cells[0].tail[3] == 0 && directOnly.cells[0].early[3] == 0, "direct-only analysis skips all tail tracing and reflected energy");
    auto fineOptions = options; fineOptions.resolution = 96;
    const auto fine = verb::analyzeEnergyMap(room, fineOptions);
    check(std::max(fine.nx, fine.ny) == 96 && fine.cells.size() <= 96 * 96
        && fine.dx < directOnly.dx / 10 && fine.dy < directOnly.dy / 10,
        "fine analysis samples a genuine 96-cell grid within bounded memory");
    const auto pos = fine.position(0, 0);
    const auto expected = verb::earlyFieldEnergy(room, source, pos);
    check(std::fabs(fine.cells[0].direct[3] - expected.direct[3]) < 1e-12,
        "fine cells use their own physical probe positions");
    auto raised = room; verb::Zone platform; platform.z = 2; platform.mat = 0; platform.poly = {{0,0},{8,0},{8,10},{0,10}}; raised.floorZones = {platform};
    bool rejected = false; try { verb::analyzeEnergyMap(raised, options); } catch (const std::exception&) { rejected = true; }
    check(rejected, "probes below a raised floor are rejected");
    options.components = 7; bool cancelled = false; int polls = 0;
    const auto started = std::chrono::steady_clock::now();
    try { verb::analyzeEnergyMap(room, options, [&] { return ++polls > 20; }); } catch (const std::exception& e) { cancelled = std::string(e.what()) == "Analysis cancelled"; }
    check(cancelled && std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() < 1., "analysis supports prompt cancellation during tracing");
    // Audience floor changes specular reflection at the MIRROR point, not at
    // the receiver. Its projected rectangle is a genuine energy discontinuity.
    auto pool = std::make_shared<verb::BakePool>(3);
    for (const auto& builtin : verbp::builtinRooms()) {
        if (builtin.name != "Cathedral (long tail)" && builtin.name != "Concert hall (audience zone)") continue;
        std::istringstream input(builtin.text); auto s = verb::parseScene(input, builtin.name);
        verb::EnergyMapOptions o; o.components = 3; o.earlyDb = 0;
        auto serial = verb::analyzeEnergyMap(s, o);
        auto parallel = verb::analyzeEnergyMap(s, o, {}, {}, {pool,3});
        auto plain = s; plain.floorZones.clear();
        auto floor = s; floor.walls.clear(); floor.ceilMat = -1; floor.diffraction = false;
        auto plainFloor = floor; plainFloor.floorZones.clear();
        bool exact = true, geometry = true; int zoneCells = 0, bareCells = 0;
        for (int y=0;y<serial.ny;++y) for(int x=0;x<serial.nx;++x) {
            auto i=(size_t)y*serial.nx+x; auto rc=serial.position(x,y); auto src=s.sources[0];
            const auto fp=src.pos+(rc-src.pos)*(src.pos.z/(src.pos.z+rc.z));
            const bool audience=verb::pointInZone(s.floorZones[0],fp.x,fp.y);
            audience ? ++zoneCells : ++bareCells;
            const auto a=verb::earlyFieldEnergy(plain,src,rc,o.earlyDb);
            const auto f=verb::earlyFieldEnergy(floor,src,rc,o.earlyDb);
            const auto p=verb::earlyFieldEnergy(plainFloor,src,rc,o.earlyDb);
            const auto& mat=s.mats[(size_t)(audience?s.floorZones[0].mat:s.floorMat)];
            const auto& base=s.mats[(size_t)s.floorMat];
            for(int b=0;b<verb::NB;++b) {
                exact &= serial.cells[i].early[b]==parallel.cells[i].early[b] && serial.cells[i].direct[b]==parallel.cells[i].direct[b];
                const double expected=verb::reflEnergy(mat,b)*(1-verb::scatterAt(mat,b))/(verb::reflEnergy(base,b)*(1-verb::scatterAt(base,b)));
                geometry &= std::abs(f.early[b]/p.early[b]-expected)<1e-6;
                geometry &= std::abs((serial.cells[i].early[b]-a.early[b])-(f.early[b]-p.early[b]))<1e-12;
            }
        }
        std::printf("Audience projection %s zone_cells%d bare_cells%d\n",builtin.name.c_str(),zoneCells,bareCells);
        check(exact,"large-room early map is identical across worker chunks");
        check(geometry && zoneCells>0 && bareCells>0,"every large-room early material step equals the analytic floor mirror-point projection in all7 bands");
    }
    return failures ? 1 : 0;
}
