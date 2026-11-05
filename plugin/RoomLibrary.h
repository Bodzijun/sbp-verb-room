// Built-in starter rooms for the room library. Each is a verb::Scene built from the surface presets and written as
// scene text, so it loads exactly like a room file (walls, floor/ceiling, zones, air, clutter, source and listener).
#pragma once
#include <array>
#include <string>
#include <vector>

#include "SourceModel.h"
#include "layouts.hpp"
#include "scene.hpp"

namespace verbp {

struct BuiltinRoom {
    std::string name;
    std::string text;
    // Optional setup that comes with the room: the sources of the plugin (formats, input channels, spread) in the same order
    // as the `source` lines of the text (so the text gives their places), and the listener's microphone pattern (-1 = keep).
    std::vector<SourceCfg> setup;
    int micPattern = -1;
};

namespace detail {
inline verb::Scene box(double w, double d, double h, const char* wall, const char* floor, const char* ceiling /* nullptr = open sky */,
                       double srcX, double srcY, double srcZ, double lisX, double lisY, double lisZ) {
    verb::Scene s;
    s.sr = 48000;
    s.length = 2.0;
    s.height = h;
    s.order = 2;
    s.mats = verb::surfacePresets();
    s.floorMat = verb::presetIndexByName(floor);
    s.ceilMat = ceiling ? verb::presetIndexByName(ceiling) : -1;
    const int m = verb::presetIndexByName(wall);
    s.walls = {verb::Wall{0, 0, w, 0, m}, verb::Wall{w, 0, w, d, m}, verb::Wall{w, d, 0, d, m}, verb::Wall{0, d, 0, 0, m}};
    verb::Transducer src;
    src.name = "Source";
    src.pos = {srcX, srcY, srcZ};
    src.azimuthDeg = std::atan2(lisY - srcY, lisX - srcX) * 180.0 / verb::kPi;
    s.sources.push_back(src);
    for (int side = 0; side < 2; ++side) {  // ORTF-like pair; the plugin builds its own microphones from the output layout
        verb::Transducer r;
        r.name = side ? "R" : "L";
        r.pos = {lisX, lisY, lisZ};
        r.azimuthDeg = side ? -55 : 55;
        r.pattern = 0.5;
        s.receivers.push_back(r);
    }
    s.hasListener = true;
    s.listenerPos = {lisX, lisY, lisZ};
    s.listenerYaw = std::atan2(srcY - lisY, srcX - lisX) * 180.0 / verb::kPi;
    return s;
}

inline void clutter(verb::Scene& s, int count, double hMax = 1.6, int seed = 7) {
    s.clutter.count = count;
    s.clutter.hMax = hMax;
    s.clutter.seed = seed;
}

inline verb::Zone rect(double x0, double y0, double x1, double y1, const char* mat, verb::Scene& s) {
    verb::Zone z;
    z.mat = verb::presetIndexByName(mat);
    z.poly = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    (void)s;
    return z;
}
}  // namespace detail

// ---- the interior of a car, with a place for every stem of SBP EngineSim Stems / MotorSynth2 ----
// Car frame: x forward, y left, z up from the cabin floor; the driver (left-hand drive) sits at x = 0. The cabin is a closed box
// whose panels let sound through (firewall 22-40 dB, doors 20-34 dB, glass), the engine bay is in front of the firewall, the
// tailpipes behind the rear partition. The 30 stem channels: Main, Exhaust A/B, Intake, Engine block, Valvetrain, Starter (stereo
// pairs, channels 1-14) and 16 mono cylinders (channels 15-30), see stem_layout.h of MotorSynth2.
// MotorSynth2 routes every stem as a stereo pair with L == R, so each source below is one mono source on the left
// channel of its pair (or, for the wide ones, a stereo source that places the two copies apart).
// withEngineSim: the 6 dry EngineSim stems follow on inputs 31-42 (the "whole car + EngineSim" scheme).
inline BuiltinRoom carRoom(bool withEngineSim) {
    verb::Scene s;
    s.sr = 48000;
    s.length = 1.0;
    s.height = 1.2;
    s.order = 2;
    s.mats = verb::surfacePresets();
    auto withTl = [&](const char* base, const char* name, std::array<double, verb::NB> tl) {
        verb::Material m = verb::presetByName(base);
        m.name = name;
        for (int b = 0; b < verb::NB; ++b) m.tau[(size_t)b] = verb::tlToTau(tl[(size_t)b]);
        s.mats.push_back(m);
        return (int)s.mats.size() - 1;
    };
    const int firewall = withTl("Metal", "Firewall", {22, 26, 30, 34, 38, 40, 40});
    const int door = withTl("Metal_door", "Car_door", {20, 24, 28, 30, 32, 34, 34});
    const int rear = withTl("Upholstery_sofa", "Rear_partition", {12, 16, 20, 24, 28, 30, 30});
    const int glass = verb::presetIndexByName("Glass_window");
    s.floorMat = verb::presetIndexByName("Carpet_thin");
    s.ceilMat = verb::presetIndexByName("Ceiling_tiles");  // the headliner
    const double xF = 0.9, xR = -1.5, yW = 0.8, zS = 0.8, zT = 1e9;
    s.walls = {verb::Wall{xF, -yW, xF, yW, firewall, 0.0, 0.80},   verb::Wall{xF, -yW, xF, yW, glass, 0.80, zT},   // firewall + windscreen
               verb::Wall{xF, yW, xR, yW, door, 0.0, zS},          verb::Wall{xF, yW, xR, yW, glass, zS, zT},       // left door + window
               verb::Wall{xR, yW, xR, -yW, rear, 0.0, 0.75},       verb::Wall{xR, yW, xR, -yW, glass, 0.75, zT},    // rear partition + rear window
               verb::Wall{xR, -yW, xF, -yW, door, 0.0, zS},        verb::Wall{xR, -yW, xF, -yW, glass, zS, zT}};    // right door + window
    {   // the engine bay: a box under a low bonnet (a plate at 0.8 m) with fenders and a grille, joined to the cabin
        // only through the firewall; the cabin roof is open over it so that the bonnet is the bay's ceiling
        const int fender = withTl("Metal", "Fender", {16, 20, 24, 26, 28, 30, 30});
        const int bonnet = withTl("Metal", "Bonnet", {20, 24, 28, 30, 32, 34, 34});
        const int grille = withTl("Metal", "Grille", {6, 8, 10, 12, 14, 16, 16});  // radiator and openings: leaky
        const double xB = 2.1, zB = 0.80;
        s.walls.push_back(verb::Wall{xF, yW, xB, yW, fender, 0.0, zB});
        s.walls.push_back(verb::Wall{xB, yW, xB, -yW, grille, 0.0, zB});
        s.walls.push_back(verb::Wall{xB, -yW, xF, -yW, fender, 0.0, zB});
        verb::Zone open;
        open.mat = -1;
        open.poly = {{xF, -yW}, {xB, -yW}, {xB, yW}, {xF, yW}};
        s.ceilZones.push_back(open);
        verb::Zone hood = open;
        hood.mat = bonnet;
        hood.z = zB;
        s.ceilZones.push_back(hood);
        verb::Zone under = open;
        under.mat = verb::presetIndexByName("Metal");
        s.floorZones.push_back(under);  // the undertray
    }
    {   // the rear bench: absorbing
        verb::Zone z;
        z.mat = verb::presetIndexByName("Upholstery_sofa");
        z.poly = {{xR, -yW}, {-0.85, -yW}, {-0.85, yW}, {xR, yW}};
        s.floorZones.push_back(z);
    }
    auto addSource = [&](const std::string& name, double x, double y, double z) {
        verb::Transducer t;
        t.name = name;
        t.pos = {x, y, z};
        t.azimuthDeg = 0;  // all stems are omnidirectional and face forward: L/R of a stereo stem is the car's left/right
        t.pattern = 0;
        s.sources.push_back(t);
    };
    std::vector<SourceCfg> setup;
    auto stem = [&](const std::string& name, int firstIn, int channels, double x, double y, double z, double spread) {
        SourceCfg c;
        c.name = name;
        c.format = channels == 2 ? 1 : 0;  // Stereo / Mono
        c.channels = channels;
        c.firstIn = firstIn;
        c.spread = spread;
        c.pattern = 0.0;
        setup.push_back(c);
        addSource(name, x, y, z);
    };
    auto wide = [&](const std::string& name, int pair, double x, double y, double z, double spread) {
        SourceCfg c;  // stereo pair: the two copies sit at +-0.5*spread across the car, 0.87*spread ahead of (x, y)
        c.name = name;
        c.format = 1;
        c.channels = 2;
        c.firstIn = (pair - 1) * 2;  // MotorSynth stem `pair` arrives on Room inputs 2*pair-1 and 2*pair
        c.spread = spread;
        c.pattern = 0.0;
        setup.push_back(c);
        addSource(name, x, y, z);
    };
    auto mono = [&](const std::string& name, int pair, double x, double y, double z) { stem(name, (pair - 1) * 2, 1, x, y, z, 0.0); };
    mono("Exhaust", 1, -1.8, 0.35, 0.30);        // tailpipe, just behind the rear partition
    mono("Engine", 2, 1.6, 0.0, 0.50);           // engine bay
    mono("Mechanical", 3, 1.6, 0.0, 0.70);
    mono("Gearbox", 4, 0.6, 0.0, 0.35);          // the tunnel
    mono("Intake", 5, 1.9, 0.30, 0.65);
    mono("Turbo BOV", 6, 1.7, -0.35, 0.60);
    wide("Tires", 7, 0.0, 0.0, 0.30, 1.65);     // front axle: two copies at x=+1.35, y=+-0.78 (outside the cabin)
    mono("Chassis", 8, 0.0, 0.0, 0.25);
    wide("Brakes", 9, 0.0, 0.0, 0.30, 1.65);
    mono("Backfire", 10, -1.85, 0.35, 0.30);
    wide("Wind", 11, -0.9, 0.0, 1.05, 1.9);     // the mirrors
    mono("Cabin", 12, 0.0, 0.0, 1.00);           // already the cabin sound, next to the driver
    mono("Accessories", 13, 0.65, 0.20, 0.85);  // the dashboard, inside the cabin in front of the driver
    mono("Electric", 14, 1.2, 0.0, 0.50);
    wide("Environment", 15, -2.5, 0.0, 1.50, 6.0);  // diffuse atmosphere: two copies 3 m either side, outside the body
    if (withEngineSim) {
        mono("EngineSim Exhaust A", 16, -1.8, 0.35, 0.30);
        mono("EngineSim Exhaust B", 17, -1.8, -0.35, 0.30);
        mono("EngineSim Intake", 18, 1.9, 0.30, 0.65);
        mono("EngineSim Block", 19, 1.6, 0.0, 0.50);
        mono("EngineSim Valvetrain", 20, 1.6, 0.0, 0.72);
        mono("EngineSim Starter", 21, 1.3, 0.30, 0.30);
    }
    verb::Transducer ear;
    for (int side = 0; side < 2; ++side) {
        ear.name = side ? "R" : "L";
        ear.pos = {0.0, 0.38, 1.0};  // the driver's head
        ear.azimuthDeg = side ? -90 : 90;
        ear.pattern = 0.5;
        s.receivers.push_back(ear);
    }
    s.hasListener = true;
    s.listenerPos = {0.0, 0.38, 1.0};
    s.listenerYaw = 0;
    verb::sanitizeScene(s);
    verb::compactMaterials(s);
    BuiltinRoom r;
    r.name = withEngineSim ? "MotorSynth + EngineSim (21 sources)" : "MotorSynth vehicle (15 stems)";
    r.text = verb::serializeScene(s);
    r.setup = setup;
    r.micPattern = (int)verb::MicPattern::HumanEars;
    return r;
}

inline std::vector<BuiltinRoom> builtinRooms() {
    using namespace detail;
    std::vector<std::pair<std::string, verb::Scene>> v;
    {
        auto s = box(2.0, 1.6, 2.3, "Acoustic_panel", "Carpet_thin", "Acoustic_panel", 1.0, 0.8, 1.5, 1.0, 0.8, 1.5);
        s.sources[0].pos = {0.9, 0.8, 1.5};
        s.listenerPos = {1.2, 0.8, 1.5};
        for (auto& r : s.receivers) r.pos = s.listenerPos;
        s.clutter.count = 0;
        v.push_back({"Vocal booth (dead)", s});
    }
    {
        auto s = box(4.2, 3.5, 2.5, "Plaster", "Carpet", "Plaster", 1.2, 1.7, 1.4, 3.0, 1.7, 1.2);
        clutter(s, 8, 1.4, 11);
        v.push_back({"Bedroom", s});
    }
    {
        auto s = box(6.0, 4.5, 2.6, "Plaster", "Parquet_floor", "Plaster", 1.5, 2.2, 1.4, 4.2, 2.2, 1.2);
        clutter(s, 14, 1.6, 5);
        v.push_back({"Living room", s});
    }
    {
        auto s = box(2.2, 2.6, 2.4, "Ceramic_tiles", "Ceramic_tiles", "Plaster", 0.8, 1.3, 1.6, 1.6, 1.3, 1.4);
        v.push_back({"Bathroom (tiles)", s});
    }
    {
        auto s = box(8.0, 6.0, 3.5, "Wood_panel", "Parquet_floor", "Ceiling_tiles", 2.5, 3.0, 1.4, 5.5, 3.0, 1.5);
        clutter(s, 6, 1.6, 3);
        v.push_back({"Studio live room", s});
    }
    {
        auto s = box(14.0, 2.0, 2.7, "Plaster", "Ceramic_tiles", "Plaster", 1.5, 1.0, 1.5, 11.0, 1.0, 1.5);
        v.push_back({"Corridor", s});
    }
    {
        auto s = box(6.0, 5.0, 2.4, "Concrete", "Concrete", "Concrete", 1.5, 2.5, 1.5, 4.2, 2.5, 1.4);
        v.push_back({"Garage (concrete)", s});
    }
    {
        auto s = box(28.0, 18.0, 12.0, "Plaster", "Parquet_floor", "Plaster", 3.0, 9.0, 1.6, 16.0, 9.0, 1.2);
        s.length = 3.0;
        s.floorZones.push_back(rect(10.0, 2.0, 26.0, 16.0, "Audience_seated", s));  // the seated audience
        v.push_back({"Concert hall (audience zone)", s});
    }
    {
        auto s = box(40.0, 16.0, 22.0, "Stone_marble", "Stone_marble", "Stone_marble", 5.0, 8.0, 1.6, 20.0, 8.0, 1.5);
        s.length = 4.0;
        v.push_back({"Cathedral (long tail)", s});
    }
    {
        auto s = box(20.0, 20.0, 8.0, "Brick", "Asphalt", nullptr, 6.0, 10.0, 1.6, 14.0, 10.0, 1.5);
        s.length = 1.5;
        v.push_back({"Courtyard (open sky)", s});
    }
    std::vector<BuiltinRoom> out;
    for (auto& e : v) {
        verb::sanitizeScene(e.second);
        verb::compactMaterials(e.second);
        BuiltinRoom r;
        r.name = e.first;
        r.text = verb::serializeScene(e.second);
        out.push_back(r);
    }
    out.push_back(carRoom(false));
    out.push_back(carRoom(true));
    return out;
}

}  // namespace verbp
