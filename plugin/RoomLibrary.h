// Built-in starter rooms for the room library. Each is a verb::Scene built from the surface presets and written as
// scene text, so it loads exactly like a room file (walls, floor/ceiling, zones, air, clutter, source and listener).
#pragma once
#include <array>
#include <sstream>
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

// Turns a whole room about the origin so that the listener looks UP the plan (+y) at the sources, which are then above it:
// the left of the listener is the left of the picture. `deg` < 1000: turn by exactly that angle; otherwise the angle is taken
// from the listener to the centre of the sources. With `shift` the room is moved so that its walls start at 0, 0.
inline void listenerUp(verb::Scene& s, double deg = 1e9, bool shift = true) {
    if (deg > 1000.0) {
        if (!s.hasListener || s.sources.empty()) return;
        double cx = 0, cy = 0;
        for (auto& t : s.sources) { cx += t.pos.x; cy += t.pos.y; }
        cx /= (double)s.sources.size();
        cy /= (double)s.sources.size();
        if (std::hypot(cx - s.listenerPos.x, cy - s.listenerPos.y) < 1e-6) return;
        deg = 90.0 - std::atan2(cy - s.listenerPos.y, cx - s.listenerPos.x) * 180.0 / verb::kPi;
    }
    const double a = deg * verb::kPi / 180.0, c = std::cos(a), sn = std::sin(a);
    auto rot = [&](double& x, double& y) { const double nx = c * x - sn * y, ny = sn * x + c * y; x = nx; y = ny; };
    for (auto& w : s.walls) { rot(w.ax, w.ay); rot(w.bx, w.by); }
    for (auto* zl : {&s.floorZones, &s.ceilZones})
        for (auto& z : *zl)
            for (auto& p : z.poly) rot(p.first, p.second);
    for (auto* tl : {&s.sources, &s.receivers})
        for (auto& t : *tl) { rot(t.pos.x, t.pos.y); t.azimuthDeg += deg; }
    rot(s.listenerPos.x, s.listenerPos.y);
    s.listenerYaw += deg;
    while (s.listenerYaw > 180.0) s.listenerYaw -= 360.0;
    while (s.listenerYaw <= -180.0) s.listenerYaw += 360.0;
    for (auto* tl : {&s.sources, &s.receivers})
        for (auto& t : *tl) {
            while (t.azimuthDeg > 180.0) t.azimuthDeg -= 360.0;
            while (t.azimuthDeg <= -180.0) t.azimuthDeg += 360.0;
        }
    if (!shift || s.walls.empty()) return;
    double x0 = 1e18, y0 = 1e18;
    for (auto& w : s.walls) { x0 = std::min({x0, w.ax, w.bx}); y0 = std::min({y0, w.ay, w.by}); }
    for (auto& w : s.walls) { w.ax -= x0; w.bx -= x0; w.ay -= y0; w.by -= y0; }
    for (auto* zl : {&s.floorZones, &s.ceilZones})
        for (auto& z : *zl)
            for (auto& p : z.poly) { p.first -= x0; p.second -= y0; }
    for (auto* tl : {&s.sources, &s.receivers})
        for (auto& t : *tl) { t.pos.x -= x0; t.pos.y -= y0; }
    s.listenerPos.x -= x0;
    s.listenerPos.y -= y0;
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
    detail::listenerUp(s, 90.0, false);  // the car drives up the plan: the driver is at the bottom, the engine at the top
    verb::sanitizeScene(s);
    verb::compactMaterials(s);
    BuiltinRoom r;
    r.name = withEngineSim ? "MotorSynth + EngineSim (21 sources)" : "MotorSynth vehicle (15 stems)";
    r.text = verb::serializeScene(s);
    r.setup = setup;
    r.micPattern = (int)verb::MicPattern::HumanEars;
    return r;
}

// ---- concert halls with an orchestra on risers, for orchestration ----
// Frame while building: x across (to the right as the audience sees it), y from the front edge of the stage (y = 0) towards the
// back wall (+y); the audience is at y < 0. The listener looks at the stage (+y), so with the plan as drawn the orchestra is
// ABOVE the listener. Every instrument group is one mono source (input channel 1, 2, 3 ... in the order of the list), at the
// place and height it really has: the strings on the stage, the woodwinds on the first riser, the brass on the second, the
// timpani and percussion on the third. The names say what each group is.
struct Player {
    const char* name;
    double x, y;  // metres: across, depth from the front edge of the stage
    int riser;    // 0 stage floor, 1..3 riser steps
    double up;    // height of the instrument above the floor it stands on
};

struct HallSpec {
    std::string name;
    double width, audienceDepth, stageDepth, height;
    const char* wall;
    const char* backWall;                     // the wall behind the audience
    double stageH, riserStep;                 // stage above the audience floor, height of each riser step
    double riserY[3];                         // depth (from the front edge) where riser 1, 2, 3 start
    double zoneX0, zoneX1, zoneY0, zoneY1;   // the seated audience, same frame
    double lisX, lisY, lisZ;                  // the listener
    double canopyZ;                           // reflector over the stage (0 = none)
    double length;
};

inline BuiltinRoom orchestraRoom(const HallSpec& h, const std::vector<Player>& players) {
    verb::Scene s;
    s.sr = 48000;
    s.length = h.length;
    s.height = h.height;
    s.order = 2;
    s.mats = verb::surfacePresets();
    s.floorMat = verb::presetIndexByName("Parquet_floor");
    s.ceilMat = verb::presetIndexByName("Plaster");
    const int wall = verb::presetIndexByName(h.wall), back = verb::presetIndexByName(h.backWall);
    const double x0 = -h.width / 2, x1 = h.width / 2, y0 = -h.audienceDepth, y1 = h.stageDepth;
    s.walls = {verb::Wall{x0, y0, x1, y0, back}, verb::Wall{x1, y0, x1, y1, wall}, verb::Wall{x1, y1, x0, y1, wall}, verb::Wall{x0, y1, x0, y0, wall}};
    auto plate = [&](double ax, double ay, double bx, double by, double z, const char* mat, bool ceiling) {
        verb::Zone zn;
        zn.mat = verb::presetIndexByName(mat);
        zn.z = z;
        zn.poly = {{ax, ay}, {bx, ay}, {bx, by}, {ax, by}};
        (ceiling ? s.ceilZones : s.floorZones).push_back(zn);
    };
    detail::clutter(s, 0);
    {   // the seated audience on the main floor
        verb::Zone a;
        a.mat = verb::presetIndexByName("Audience_seated");
        a.poly = {{h.zoneX0, h.zoneY0}, {h.zoneX1, h.zoneY0}, {h.zoneX1, h.zoneY1}, {h.zoneX0, h.zoneY1}};
        s.floorZones.push_back(a);
    }
    plate(x0, 0.0, x1, y1, h.stageH, "Parquet_floor", false);  // the stage
    for (int r = 0; r < 3; ++r)                                // the risers, each one on top of the one before
        if (h.riserY[r] > 0) plate(x0 + 1.0, h.riserY[r], x1 - 1.0, y1 - 0.2, h.stageH + h.riserStep * (r + 1), "Parquet_floor", false);
    if (h.canopyZ > 0) plate(x0 + 1.5, 0.5, x1 - 1.5, y1 - 0.3, h.canopyZ, "Wood_panel", true);  // reflector over the orchestra
    std::vector<SourceCfg> setup;
    for (size_t i = 0; i < players.size(); ++i) {
        const Player& pl = players[i];
        const double floorZ = h.stageH + (pl.riser > 0 ? h.riserStep * pl.riser : 0.0);
        verb::Transducer t;
        t.name = pl.name;
        t.pos = {pl.x, pl.y, floorZ + pl.up};
        t.azimuthDeg = -90;  // towards the audience
        t.pattern = 0.3;
        s.sources.push_back(t);
        SourceCfg c;
        c.name = pl.name;
        c.format = 0;
        c.channels = 1;
        c.firstIn = (int)i;
        c.spread = 0.0;
        c.pattern = 0.3;
        setup.push_back(c);
    }
    for (int side = 0; side < 2; ++side) {
        verb::Transducer r;
        r.name = side ? "R" : "L";
        r.pos = {h.lisX, h.lisY, h.lisZ};
        r.azimuthDeg = side ? -55 : 55;
        r.pattern = 0.5;
        s.receivers.push_back(r);
    }
    s.hasListener = true;
    s.listenerPos = {h.lisX, h.lisY, h.lisZ};
    s.listenerYaw = 90;  // facing the orchestra
    detail::listenerUp(s, 0.0, true);  // already facing up the plan; moves the room so that its corner is at 0, 0
    verb::sanitizeScene(s);
    verb::compactMaterials(s);
    BuiltinRoom r;
    r.name = h.name;
    r.text = verb::serializeScene(s);
    r.setup = setup;
    r.micPattern = (int)verb::MicPattern::LayoutDefault;
    return r;
}

// Seating of a symphony orchestra, as the audience sees it (left to right): first violins, second violins, violas, cellos with the
// double basses behind them on the right; the woodwinds in the middle on the first riser, the brass behind them on the second,
// the timpani and percussion at the back on the third.
inline std::vector<Player> symphonyPlayers() {
    return {
        {"Violins I (strings)", -5.5, 2.4, 0, 1.35},
        {"Violins II (strings)", -1.8, 2.9, 0, 1.35},
        {"Violas (strings)", 2.0, 2.9, 0, 1.30},
        {"Cellos (strings)", 5.6, 2.8, 0, 0.90},
        {"Basses (strings)", 8.0, 5.3, 0, 1.20},
        {"Harp", -8.2, 4.9, 0, 1.00},
        {"Flutes (wood)", -2.4, 6.2, 1, 1.45},
        {"Oboes (wood)", -0.8, 6.3, 1, 1.35},
        {"Clarinets (wood)", 0.9, 6.3, 1, 1.25},
        {"Bassoons (wood)", 2.6, 6.2, 1, 1.30},
        {"Horns (brass)", -4.2, 8.6, 2, 1.20},
        {"Trumpets (brass)", -0.9, 8.7, 2, 1.45},
        {"Trombones (brass)", 2.6, 8.8, 2, 1.35},
        {"Tuba (brass)", 5.3, 8.9, 2, 1.10},
        {"Timpani (drums)", -3.6, 10.7, 3, 0.95},
        {"Percussion", 3.0, 10.8, 3, 1.20},
        {"Soloist", 0.9, 1.0, 0, 1.40},
    };
}

// A chamber orchestra (about 25 players): smaller stage, one riser for the winds and brass, a keyboard in front for the continuo.
inline std::vector<Player> chamberPlayers() {
    return {
        {"Violins I (strings)", -3.0, 1.9, 0, 1.35},
        {"Violins II (strings)", -0.9, 2.3, 0, 1.35},
        {"Violas (strings)", 1.2, 2.3, 0, 1.30},
        {"Cellos (strings)", 3.0, 2.0, 0, 0.90},
        {"Basses (strings)", 4.3, 3.7, 0, 1.20},
        {"Flute (wood)", -1.8, 4.2, 1, 1.45},
        {"Oboe (wood)", -0.6, 4.3, 1, 1.35},
        {"Clarinet (wood)", 0.6, 4.3, 1, 1.25},
        {"Bassoon (wood)", 1.8, 4.2, 1, 1.30},
        {"Horns (brass)", -2.8, 5.4, 1, 1.20},
        {"Trumpets (brass)", 1.4, 5.4, 1, 1.45},
        {"Timpani (drums)", 3.5, 5.4, 1, 0.95},
        {"Harpsichord / piano", -4.6, 1.0, 0, 1.15},
        {"Soloist", 0.0, 0.9, 0, 1.40},
    };
}

inline BuiltinRoom symphonyHall(bool conductor) {
    HallSpec h;
    h.name = conductor ? "Symphony hall - conductor's podium (orchestra on risers)" : "Symphony hall - audience, row 12 (orchestra on risers)";
    h.width = 22.0; h.audienceDepth = 34.0; h.stageDepth = 12.0; h.height = 17.0;
    h.wall = "Plywood_thin"; h.backWall = "Velvet_drapes";
    h.stageH = 1.1; h.riserStep = 0.4;
    h.riserY[0] = 5.2; h.riserY[1] = 7.6; h.riserY[2] = 9.9;
    h.zoneX0 = -10.0; h.zoneX1 = 10.0; h.zoneY0 = -33.0; h.zoneY1 = -2.0;
    h.canopyZ = 12.0;
    h.length = 2.5;
    if (conductor) { h.lisX = 0.0; h.lisY = 0.8; h.lisZ = h.stageH + 0.3 + 1.6; }  // the podium is 0.3 m high
    else { h.lisX = 0.0; h.lisY = -14.0; h.lisZ = 1.15; }
    return orchestraRoom(h, symphonyPlayers());
}

inline BuiltinRoom chamberHall() {
    HallSpec h;
    h.name = "Chamber hall - audience, row 6 (chamber orchestra)";
    h.width = 13.0; h.audienceDepth = 15.0; h.stageDepth = 6.5; h.height = 9.0;
    h.wall = "Wood_panel"; h.backWall = "Velvet_drapes";
    h.stageH = 0.8; h.riserStep = 0.35;
    h.riserY[0] = 3.6; h.riserY[1] = 0.0; h.riserY[2] = 0.0;
    h.zoneX0 = -5.5; h.zoneX1 = 5.5; h.zoneY0 = -13.5; h.zoneY1 = -1.5;
    h.canopyZ = 6.0;
    h.length = 1.8;
    h.lisX = 0.0; h.lisY = -6.5; h.lisZ = 1.15;
    return orchestraRoom(h, chamberPlayers());
}

// A complete VSL Studio template: stereo pairs are averaged into independent
// point sources, leaving the hall responsible for positioning and room sound.
inline std::vector<Player> vslGrandPlayers() {
    return {
        {"Violins I", -7.8, 2.3, 0, 1.35}, {"Violins II", -3.8, 3.0, 0, 1.35},
        {"Violas", 0.8, 3.2, 0, 1.30}, {"Cellos", 5.2, 2.6, 0, 0.90}, {"Double basses", 9.8, 5.4, 0, 1.20},
        {"Solo violin", 0.0, 1.2, 0, 1.40}, {"Harp I", -10.8, 4.1, 0, 1.00}, {"Harp II", -11.5, 5.4, 0, 1.00},
        {"Piccolo", -5.4, 8.0, 1, 1.45}, {"Flute I", -5.5, 6.8, 1, 1.45}, {"Flute II", -3.8, 6.8, 1, 1.45},
        {"Oboe I", -2.0, 6.8, 1, 1.35}, {"Oboe II", -0.3, 6.8, 1, 1.35}, {"English horn", 1.4, 7.6, 1, 1.30},
        {"Clarinet I", 0.0, 8.2, 1, 1.25}, {"Clarinet II", 1.7, 8.2, 1, 1.25}, {"Bass clarinet", 3.4, 8.2, 1, 1.25},
        {"Bassoon I", 3.2, 6.8, 1, 1.30}, {"Bassoon II", 4.8, 6.8, 1, 1.30}, {"Contrabassoon", 5.2, 8.3, 1, 1.30},
        {"Horns I-II", -7.3, 9.4, 2, 1.20}, {"Horns III-IV", -4.9, 9.6, 2, 1.20},
        {"Trumpets I-II", -1.9, 9.7, 2, 1.45}, {"Trumpet III", 0.2, 9.7, 2, 1.45},
        {"Tenor trombones", 3.0, 9.9, 2, 1.35}, {"Bass trombone", 5.5, 10.0, 2, 1.35}, {"Tuba", 7.8, 10.1, 2, 1.10},
        {"Timpani", -4.0, 12.4, 3, 0.95}, {"Percussion - drums", 0.3, 12.6, 3, 1.00},
        {"Percussion - metals", 4.5, 12.5, 3, 1.30}, {"Mallets", 8.2, 12.3, 3, 1.00}, {"Piano / celesta", -10.5, 2.3, 0, 1.10},
    };
}

inline BuiltinRoom vslGrandHall(bool conductor) {
    HallSpec h;
    h.name = conductor ? "VSL Grand hall - conductor (32 stereo groups)" : "VSL Grand hall - audience (32 stereo groups)";
    h.width = 28.0; h.audienceDepth = 34.0; h.stageDepth = 14.0; h.height = 18.5;
    h.wall = "Wood_panel"; h.backWall = "Velvet_drapes";
    h.stageH = 1.0; h.riserStep = 0.35;
    h.riserY[0] = 6.3; h.riserY[1] = 8.9; h.riserY[2] = 11.7;
    h.zoneX0 = -10.0; h.zoneX1 = 10.0; h.zoneY0 = -27.5; h.zoneY1 = -2.0;
    h.canopyZ = 11.5; h.length = 3.2;
    h.lisX = 0.0; h.lisY = conductor ? 0.2 : -14.0; h.lisZ = conductor ? 2.9 : 1.2;
    auto room = orchestraRoom(h, vslGrandPlayers());
    std::istringstream in(room.text);
    auto sc = verb::parseScene(in, room.name);
    // orchestraRoom shifted the construction frame by (+14, +34).
    auto point = [](double x, double y) { return std::make_pair(x + 14.0, y + 34.0); };
    verb::Material oak = verb::presetByName("Wood_panel");
    oak.name = "Faceted_oak_wall"; oak.tau.fill(0); oak.scatter = 0.30; oak.scatterHf = 0.75;
    const int side = (int)sc.mats.size(); sc.mats.push_back(oak);
    auto rear = verb::presetByName("Velvet_drapes"); rear.name = "Rear_absorbing_wall"; rear.tau.fill(0);
    const int back = (int)sc.mats.size(); sc.mats.push_back(rear);
    const std::vector<std::pair<double, double>> outline = {
        {-8.0, -34.0}, {7.2, -34.0}, {13.4, -29.0}, {16.0, -21.0}, {15.2, -12.0}, {13.8, -4.0},
        {14.0, 0.0}, {14.0, 14.0}, {9.0, 17.0}, {-10.0, 17.0}, {-14.0, 14.0}, {-14.0, 0.0},
        {-15.0, -6.0}, {-17.0, -15.0}, {-16.0, -24.0}, {-12.5, -30.0},
    };
    sc.walls.clear();
    for (size_t i = 0; i < outline.size(); ++i) {
        const auto a = point(outline[i].first, outline[i].second);
        const auto b = point(outline[(i + 1) % outline.size()].first, outline[(i + 1) % outline.size()].second);
        // Include the right bevel's endpoint at y=-29: both rear bevels and the back wall absorb.
        const bool rearFace = outline[i].second <= -29 && outline[(i + 1) % outline.size()].second <= -29;
        sc.walls.push_back(verb::Wall{a.first, a.second, b.first, b.second, rearFace ? back : side});
    }
    auto zone = [&](std::initializer_list<std::pair<double, double>> vertices, double z, const char* material, bool ceiling) {
        verb::Zone a; a.z = z; a.mat = (int)sc.mats.size(); sc.mats.push_back(verb::presetByName(material));
        for (const auto& p : vertices) a.poly.push_back(point(p.first, p.second));
        (ceiling ? sc.ceilZones : sc.floorZones).push_back(a);
    };
    zone({{-8, -32.8}, {6.8, -32.8}, {10, -28}, {-10, -28}}, -1, "Audience_seated", false);
    zone({{-14.2, -22}, {-11.5, -24}, {-10.8, -9}, {-14.5, -10}}, 3.6, "Audience_seated", false);
    zone({{11.2, -23}, {14.3, -21}, {13.4, -7}, {10.8, -9}}, 4.2, "Audience_seated", false);
    zone({{-8, -9}, {8, -8}, {7, -2}, {-7, -2}}, 10.6, "Wood_panel", true);
    for (size_t i = 0; i < room.setup.size(); ++i) {
        auto& c = room.setup[i]; c.format = 1; c.channels = 2; c.firstIn = 2 * (int)i; c.sum = true;
    }
    detail::listenerUp(sc, 0.0, true);
    verb::sanitizeScene(sc); verb::compactMaterials(sc);
    room.text = verb::serializeScene(sc);
    return room;
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
        // stone walls and a plaster vault, a marble floor with the nave full of pews: a decay of about 7-8 s (all polished marble gives 25 s+)
        auto s = box(40.0, 16.0, 22.0, "Brick", "Stone_marble", "Plaster", 5.0, 8.0, 1.6, 20.0, 8.0, 1.5);
        s.length = 6.0;
        s.floorZones.push_back(rect(12.0, 4.5, 34.0, 13.0, "Audience_seated", s));
        v.push_back({"Cathedral (long tail)", s});
    }
    {
        auto s = box(20.0, 20.0, 8.0, "Brick", "Asphalt", nullptr, 6.0, 10.0, 1.6, 14.0, 10.0, 1.5);
        s.length = 1.5;
        v.push_back({"Courtyard (open sky)", s});
    }
    std::vector<BuiltinRoom> out;
    for (auto& e : v) {
        detail::listenerUp(e.second);  // the listener at the bottom, looking up at the sources
        verb::sanitizeScene(e.second);
        verb::compactMaterials(e.second);
        BuiltinRoom r;
        r.name = e.first;
        r.text = verb::serializeScene(e.second);
        out.push_back(r);
    }
    out.push_back(carRoom(false));
    out.push_back(carRoom(true));
    out.push_back(chamberHall());
    out.push_back(symphonyHall(false));
    out.push_back(symphonyHall(true));
    out.push_back(vslGrandHall(false));
    out.push_back(vslGrandHall(true));
    return out;
}

}  // namespace verbp
