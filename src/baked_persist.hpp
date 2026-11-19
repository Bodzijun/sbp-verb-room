#pragma once
// P4 (owner decision 2026-11-18, overrides the E9 "tables are session-only" design): a baked table is tied
// to the CONTENT it was computed for, is saved with the project/preset and is re-installed after Undo.
// The session revision counters (geometry/quality/objRevisions/request/job) stay as the cheap audio-thread
// validity check; this file adds the cross-session identity (content signature) and a compact, strictly
// validated binary form. Nothing here runs on the audio thread.
//
// Signature INCLUDES: modeled scene acoustics (walls, materials, zones, height, seed/clutter/roughness,
// temperature/humidity, rendered length, sample rate), absorption scale, bake quality settings
// (erOrder/tailRays/adaptiveTail/shared), receiver layout relative to the listener anchor, spatial taps,
// creative decay multipliers (conservative: today a decay change invalidates via qualitySerial), source
// count/receiver count, per-source directivity profile content + amount + aim + pattern, the raw poses of
// the objects the table treats as static (listener included when it does not move), the full path content
// (poses, arc lengths, timing keys, flags, group/object, laps, uIndexed), the estimator version and this
// persistence format version.
// Signature EXCLUDES: names (scene/material/transducer/profile), editor tags (hidden/locked/group), layer
// modes, material colour class, UI-only state, output gain/mix/solo, the construction-time source poses
// (the bake traces the LIVE poses, hashed separately) and every session-local counter (geomSerial,
// qualitySerial, bakeObjectRevision, bakeRevision, bakeAction, recorded-pass generation).
#include <cstdint>
#include <cstring>
#include <vector>
#include "baked_path.hpp"

namespace verb {

inline constexpr uint32_t kBakePersistVersion = 2;
// Hard cap for the serialised (binary, before base64) size of ALL tables of one project state. Measured
// payloads are far below it (see out/P4/REPORT.uk.md); a state above the cap keeps the old behaviour and
// the plugin says "bake not stored: too large".
inline constexpr size_t kBakePersistMaxBytes = 4u * 1024u * 1024u;
// In-memory undo cache: the last few published tables, keyed by content signature (Engine::Impl).
inline constexpr size_t kBakeCacheCap = 4;

inline uint64_t bakeFnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    auto p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
inline uint64_t bakeMixU64(uint64_t v, uint64_t h) { return bakeFnv1a(&v, sizeof v, h); }
inline uint64_t bakeMixI32(int32_t v, uint64_t h) { return bakeFnv1a(&v, sizeof v, h); }
inline uint64_t bakeMixU8(uint8_t v, uint64_t h) { return bakeFnv1a(&v, sizeof v, h); }
inline uint64_t bakeMixF64(double v, uint64_t h) { static_assert(sizeof(double) == 8, "IEEE754 double"); return bakeFnv1a(&v, sizeof v, h); }
inline uint64_t bakeMixStr(const std::string& s, uint64_t h) { h = bakeMixU64(s.size(), h); return bakeFnv1a(s.data(), s.size(), h); }

// Acoustic content of a directivity profile: the SH coefficients that shape the traced energy. Identity
// metadata (id/name/category/license/attribution) does not change the sound and is excluded.
inline uint64_t bakeDirectivityProfileHash(const DirectivityProfile* p) {
    if (!p) return 0;
    uint64_t h = bakeMixI32(p->order, 1469598103934665603ull);
    h = bakeMixF64(p->minGain, h);
    for (const auto& c : p->coeffs) {
        h = bakeMixU64(c.size(), h);
        h = bakeFnv1a(c.data(), c.size() * sizeof(float), h);
    }
    return h;
}
inline uint64_t bakeDirectivityStateHash(const DirectivityState& st) {
    uint64_t h = bakeMixF64(st.amount, 1469598103934665603ull);
    h = bakeMixU8(st.fixedPattern ? 1 : 0, h);
    h = bakeMixF64(st.pattern, h);
    h = bakeMixF64(st.aimAzDeg, h); h = bakeMixF64(st.aimElDeg, h); h = bakeMixF64(st.aimRollDeg, h);
    return bakeMixU64(bakeDirectivityProfileHash(st.profile.get()), h);
}

// Path content without the session-local recorded-pass generation counter.
inline uint64_t bakePathContentHash(const BakedPath& p) {
    uint64_t h = bakeMixI32(p.object, 1469598103934665603ull);
    h = bakeMixI32(p.group, h);
    h = bakeMixU8(p.closed ? 1 : 0, h); h = bakeMixU8(p.recorded ? 1 : 0, h); h = bakeMixU8(p.uIndexed ? 1 : 0, h);
    h = bakeMixF64(p.startU, h); h = bakeMixF64(p.endU, h);
    h = bakeMixI32(p.laps, h);
    h = bakeMixU64(p.timingSig, h);
    h = bakeMixU64(p.poses.size(), h);
    for (const auto& q : p.poses) for (double v : q) h = bakeMixF64(v, h);
    h = bakeMixU64(p.distance.size(), h);
    for (double d : p.distance) h = bakeMixF64(d, h);
    h = bakeMixU64(p.timingKeys.size(), h);
    for (const auto& k : p.timingKeys) { h = bakeMixF64(k.t, h); h = bakeMixF64(k.f, h); h = bakeMixU8(k.easing, h); }
    return h;
}

// Acoustic content of the MODELLED scene (clutter/roughness/absorption already applied by World). Receivers
// are hashed in the listener-anchor frame (offset rotated by -listenerYaw, azimuth minus listenerYaw): the
// bake resolves them against the LIVE listener pose, so the construction anchor itself must not matter.
// Construction-time source poses are excluded: the bake traces live poses (hashed by the group signature).
inline uint64_t bakeSceneContentHash(const Scene& s) {
    uint64_t h = 1469598103934665603ull;
    h = bakeMixF64(s.sr, h); h = bakeMixF64(s.height, h);
    // s.length is NOT hashed: it is the engine's auto tail length (settled at the current pose, differs between
    // sessions for identical content: 1.43 s vs 1.49 s seen in P4 tests) and only guarantees the tail is not cut.
    h = bakeMixF64(s.tempC, h); h = bakeMixF64(s.humidity, h);
    h = bakeMixU8(s.diffraction ? 1 : 0, h);
    h = bakeMixF64(s.roughness, h); h = bakeMixI32(s.roughSeed, h);
    h = bakeMixI32(s.seed, h);
    h = bakeMixI32(s.clutter.count, h); h = bakeMixF64(s.clutter.minLen, h); h = bakeMixF64(s.clutter.maxLen, h);
    h = bakeMixF64(s.clutter.tlMin, h); h = bakeMixF64(s.clutter.tlMax, h); h = bakeMixF64(s.clutter.hMin, h);
    h = bakeMixF64(s.clutter.hMax, h); h = bakeMixF64(s.clutter.minDist, h); h = bakeMixI32(s.clutter.seed, h);
    h = bakeMixU8(s.hasListener ? 1 : 0, h);
    h = bakeMixI32(s.floorMat, h); h = bakeMixI32(s.ceilMat, h); h = bakeMixU8(s.ceilingInfinite ? 1 : 0, h);
    h = bakeMixU64(s.mats.size(), h);
    for (const auto& m : s.mats) {
        for (double a : m.alpha) h = bakeMixF64(a, h);
        for (double t : m.tau) h = bakeMixF64(t, h);
        h = bakeMixF64(m.scatter, h); h = bakeMixF64(m.scatterHf, h);
    }
    h = bakeMixU64(s.walls.size(), h);
    for (const auto& w : s.walls) {
        h = bakeMixF64(w.ax, h); h = bakeMixF64(w.ay, h); h = bakeMixF64(w.bx, h); h = bakeMixF64(w.by, h);
        h = bakeMixI32(w.mat, h); h = bakeMixF64(w.z0, h); h = bakeMixF64(w.z1, h);
    }
    auto zones = [&](const std::vector<Zone>& zs) {
        h = bakeMixU64(zs.size(), h);
        for (const auto& z : zs) {
            h = bakeMixI32(z.mat, h); h = bakeMixF64(z.z, h);
            h = bakeMixU64(z.poly.size(), h);
            for (const auto& q : z.poly) { h = bakeMixF64(q.first, h); h = bakeMixF64(q.second, h); }
        }
    };
    zones(s.floorZones); zones(s.ceilZones);
    const double cy = std::cos(-s.listenerYaw * kPi / 180.), sy = std::sin(-s.listenerYaw * kPi / 180.);
    h = bakeMixU64(s.receivers.size(), h);
    for (const auto& r : s.receivers) {
        const double dx = r.pos.x - s.listenerPos.x, dy = r.pos.y - s.listenerPos.y;
        h = bakeMixF64(dx * cy - dy * sy, h); h = bakeMixF64(dx * sy + dy * cy, h); h = bakeMixF64(r.pos.z - s.listenerPos.z, h);
        h = bakeMixF64(r.azimuthDeg - s.listenerYaw, h); h = bakeMixF64(r.elevationDeg, h);
        h = bakeMixF64(r.pattern, h); h = bakeMixF64(r.patternHf, h); h = bakeMixF64(r.gainDb, h);
        h = bakeMixI32(r.shIndex, h);
        h = bakeMixU64(bakeDirectivityProfileHash(r.profile()), h);
    }
    return h;
}

// ---- compact binary form (little-endian, fixed layout, per-table FNV-1a checksum) ----------------------
// Header: "VRBK" u32 version u32 tableCount. Per table: contentSig u64, estimator i32, sources/receivers/
// discontinuities/selectedRays i32, movesListener u8, path, movingPaths, groupSources, staticPose, patterns,
// u, energy, noiseDb, sumNoiseDb, residualSegments, checksum u64. NOT stored (session-local or derivable):
// objRevisions, request/geometry/quality/job, amplitude (= sqrt(energy)), joint curve (= buildJointCurve).
struct BakeByteWriter {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void i32(int32_t v) { u32((uint32_t)v); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void f64(double v) { uint64_t bits; std::memcpy(&bits, &v, 8); u64(bits); }  // IEEE754, x86/ARM little-endian
    void f64vec(const std::vector<double>& v) { u32((uint32_t)v.size()); for (double x : v) f64(x); }
    void path(const BakedPath& p) {
        i32(p.object); i32(p.group);
        u8(p.closed ? 1 : 0); u8(p.recorded ? 1 : 0); u8(p.uIndexed ? 1 : 0);
        f64(p.startU); f64(p.endU); i32(p.laps); u64(p.timingSig);
        u64(0);  // recording generation is session-local: never persisted
        u32((uint32_t)p.poses.size()); for (const auto& q : p.poses) for (double v : q) f64(v);
        f64vec(p.distance);
        u32((uint32_t)p.timingKeys.size());
        for (const auto& k : p.timingKeys) { f64(k.t); f64(k.f); u8(k.easing); }
    }
};
struct BakeByteReader {
    const uint8_t* p; size_t n, pos = 0; bool ok = true;
    uint8_t u8() { if (pos + 1 > n) { ok = false; return 0; } return p[pos++]; }
    uint32_t u32() { if (pos + 4 > n) { ok = false; return 0; } uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= (uint32_t)p[pos++] << (8 * i); return v; }
    int32_t i32() { return (int32_t)u32(); }
    uint64_t u64() { if (pos + 8 > n) { ok = false; return 0; } uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= (uint64_t)p[pos++] << (8 * i); return v; }
    double f64() { uint64_t bits = u64(); double v; std::memcpy(&v, &bits, 8); return v; }
    bool f64vec(std::vector<double>& v, size_t cap) {
        const uint32_t cnt = u32(); if (!ok || cnt > cap) { ok = false; return false; }
        v.resize(cnt); for (auto& x : v) { x = f64(); if (!std::isfinite(x)) ok = false; }
        return ok;
    }
    bool path(BakedPath& out) {
        out.object = i32(); out.group = i32();
        out.closed = u8() != 0; out.recorded = u8() != 0; out.uIndexed = u8() != 0;
        out.startU = f64(); out.endU = f64(); out.laps = i32(); out.timingSig = u64();
        out.recording = u64();
        const uint32_t np = u32(); if (!ok || np > 8193) { ok = false; return false; }
        out.poses.resize(np);
        for (auto& q : out.poses) for (double& v : q) { v = f64(); if (!std::isfinite(v)) ok = false; }
        if (!f64vec(out.distance, 8193)) return false;
        const uint32_t nt = u32(); if (!ok || nt > 8193) { ok = false; return false; }
        out.timingKeys.resize(nt);
        for (auto& k : out.timingKeys) { k.t = f64(); k.f = f64(); k.easing = u8(); if (!std::isfinite(k.t) || !std::isfinite(k.f) || k.easing > 3) ok = false; }
        return ok;
    }
};

inline std::vector<uint8_t> bakeSerializeTables(const std::vector<BakedTable>& tables) {
    BakeByteWriter w;
    w.b.push_back('V'); w.b.push_back('R'); w.b.push_back('B'); w.b.push_back('K');
    w.u32(kBakePersistVersion); w.u32((uint32_t)tables.size());
    for (const auto& t : tables) {
        const size_t start = w.b.size();
        w.u64(t.contentSig); w.i32(t.estimator);
        w.i32(t.sources); w.i32(t.receivers); w.i32(t.discontinuities); w.i32(t.selectedRays);
        w.u8(t.movesListener ? 1 : 0);
        w.path(t.path);
        w.u32((uint32_t)t.movingPaths.size()); for (const auto& p : t.movingPaths) w.path(p);
        w.u32((uint32_t)t.groupSources.size()); for (int s : t.groupSources) w.i32(s);
        w.f64vec(t.staticPose); w.f64vec(t.patterns);
        w.f64vec(t.u); w.f64vec(t.energy); w.f64vec(t.noiseDb); w.f64vec(t.sumNoiseDb);
        w.f64vec(t.winEnergy); w.f64vec(t.winEss);
        w.u32((uint32_t)t.residualSegments.size());
        for (const auto& r : t.residualSegments) { w.f64(r[0]); w.f64(r[1]); w.f64(r[2]); }
        w.u64(bakeFnv1a(w.b.data() + start, w.b.size() - start));
    }
    return std::move(w.b);
}

// Strict: any structural violation, checksum mismatch, non-finite value or oversize blob returns false and
// leaves `out` empty. A structurally valid table with a foreign estimator/signature still deserialises; the
// engine's install step is what rejects it (state notBaked, never a crash, never playback).
inline bool bakeDeserializeTables(const uint8_t* data, size_t size, std::vector<BakedTable>& out) {
    out.clear();
    if (!data || size < 12 || size > kBakePersistMaxBytes) return false;
    if (data[0] != 'V' || data[1] != 'R' || data[2] != 'B' || data[3] != 'K') return false;
    BakeByteReader r{data, size};
    r.pos = 4;  // magic consumed above
    if (r.u32() != kBakePersistVersion) return false;
    const uint32_t count = r.u32();
    if (!r.ok || count == 0 || count > 256) return false;
    std::vector<BakedTable> tables;
    tables.reserve(count);
    for (uint32_t ti = 0; ti < count && r.ok; ++ti) {
        const size_t start = r.pos;
        BakedTable t;
        t.contentSig = r.u64(); t.estimator = r.i32();
        t.sources = r.i32(); t.receivers = r.i32(); t.discontinuities = r.i32(); t.selectedRays = r.i32();
        t.movesListener = r.u8() != 0;
        if (!r.ok || t.sources < 0 || t.sources > 1024 || t.receivers < 0 || t.receivers > 1024 ||
            t.discontinuities < 0 || t.selectedRays < 0) { r.ok = false; break; }
        if (!r.path(t.path)) break;
        const uint32_t nmp = r.u32(); if (!r.ok || nmp > 256) { r.ok = false; break; }
        t.movingPaths.resize(nmp);
        for (auto& p : t.movingPaths) if (!r.path(p)) break;
        if (!r.ok) break;
        const uint32_t ngs = r.u32(); if (!r.ok || ngs > 1024) { r.ok = false; break; }
        t.groupSources.resize(ngs);
        for (int& s : t.groupSources) { s = r.i32(); if (s < -1 || s > 1023) r.ok = false; }
        if (!r.f64vec(t.staticPose, 4u * 1025u)) break;
        if (!r.f64vec(t.patterns, 1024)) break;
        if (!r.f64vec(t.u, 65536)) break;
        if (!r.f64vec(t.energy, 65536u * 1024u)) break;
        if (!r.f64vec(t.noiseDb, 65536u * 1024u)) break;
        if (!r.f64vec(t.sumNoiseDb, 65536u * 1024u)) break;
        if (!r.f64vec(t.winEnergy, 65536u * 1024u * (size_t)kBakeWinCount)) break;
        if (!r.f64vec(t.winEss, 65536u * 1024u * (size_t)kBakeWinCount)) break;
        const uint32_t nrs = r.u32(); if (!r.ok || nrs > 65536) { r.ok = false; break; }
        t.residualSegments.resize(nrs);
        for (auto& seg : t.residualSegments) for (double& v : seg) { v = r.f64(); if (!std::isfinite(v)) r.ok = false; }
        if (!r.ok) break;
        const uint64_t stored = r.u64();
        if (!r.ok || stored != bakeFnv1a(data + start, r.pos - start - 8)) { r.ok = false; break; }
        // Structural payload invariants (the audio lookup indexes energy with stride sources*receivers*NB).
        const uint64_t stride = (uint64_t)t.sources * (uint64_t)t.receivers * (uint64_t)NB;
        const uint64_t wStride = (uint64_t)kBakeWinCount * stride;
        if (t.u.size() < 2 || t.energy.size() != t.u.size() * stride || t.noiseDb.size() != t.energy.size() ||
            t.sumNoiseDb.size() != t.u.size() * (uint64_t)t.receivers * NB ||
            t.winEnergy.size() != t.u.size() * wStride || t.winEss.size() != t.winEnergy.size() ||
            t.groupSources.size() != (size_t)t.sources || t.movingPaths.empty() ||
            t.staticPose.size() % 4 != 0 || t.patterns.size() > 1024) { r.ok = false; break; }
        for (double e : t.energy) if (!(e >= 0)) { r.ok = false; break; }
        for (double e : t.winEnergy) if (!(e >= 0)) { r.ok = false; break; }
        for (double e : t.winEss) if (!(e >= 0)) { r.ok = false; break; }
        if (!r.ok) break;
        for (size_t j = 1; j < t.u.size(); ++j) if (!(t.u[j] > t.u[j - 1])) { r.ok = false; break; }
        if (!r.ok) break;
        // Derivables rebuilt exactly as the worker builds them: amplitude = sqrt(energy); the joint curve
        // only exists for declared timing/uIndexed paths (same condition as the bake job setup).
        t.amplitude.resize(t.energy.size());
        for (size_t j = 0; j < t.energy.size(); ++j) t.amplitude[j] = std::sqrt(t.energy[j]);
        t.winAmplitude.resize(t.winEnergy.size());
        for (size_t j = 0; j < t.winEnergy.size(); ++j) t.winAmplitude[j] = std::sqrt(t.winEnergy[j]);
        bool useJointCurve = false;
        for (const auto& mp : t.movingPaths) if (!mp.recorded && (mp.hasTiming() || mp.uIndexed)) { useJointCurve = true; break; }
        if (useJointCurve) t.buildJointCurve();
        t.objRevisions.clear();
        t.request = t.geometry = t.quality = t.job = 0;
        tables.push_back(std::move(t));
    }
    if (!r.ok || r.pos != size || tables.size() != count) return false;
    out = std::move(tables);
    return true;
}

}  // namespace verb