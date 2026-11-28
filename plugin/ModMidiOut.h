#pragma once
// V04e: optional MIDI out of a modulation route (notes / CC to the host's MIDI output).
// Header-only, no JUCE dependency: the generator writes into a caller-provided sink, so the same code runs in the
// plugin (juce::MidiBuffer) and in tests. Audio thread: fixed-size state, no allocation, no locks.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace verbp {

enum class ModMidiMode : uint8_t {
    Off = 0,
    Gate = 1,     // note-on when the smoothed route output rises to >= threshold, note-off when it falls below threshold - hysteresis
    Trigger = 2,  // short note (lengthMs) on every upward crossing (one-shot samples)
    CC = 3,       // controller value lo..hi from the route output, sent only when the integer value changes
    Index = 4     // keyswitch: driver value rounded to an integer i; i >= 1 holds note number + i - 1, i = 0 holds nothing
};
inline constexpr ModMidiMode kLastModMidiMode = ModMidiMode::Index;

inline const char* modMidiModeName(ModMidiMode m) noexcept {
    switch (m) {
    case ModMidiMode::Gate: return "gate";
    case ModMidiMode::Trigger: return "trigger";
    case ModMidiMode::CC: return "cc";
    case ModMidiMode::Index: return "index";
    default: return "off";
    }
}
inline bool parseModMidiMode(std::string_view s, ModMidiMode& out) noexcept {
    if (s == "off") { out = ModMidiMode::Off; return true; }
    if (s == "gate") { out = ModMidiMode::Gate; return true; }
    if (s == "trigger") { out = ModMidiMode::Trigger; return true; }
    if (s == "cc") { out = ModMidiMode::CC; return true; }
    if (s == "index") { out = ModMidiMode::Index; return true; }
    return false;
}

struct ModMidiOut {
    ModMidiMode mode = ModMidiMode::Off;
    int channel = 1;           // 1..16
    int number = 60;           // note (gate/trigger/index base) or controller number, 0..127
    int velocity = 100;        // 1..127 fixed note-on velocity
    bool velocityFromValue = false; // note-on velocity = route output 0..1 -> 1..127
    double threshold = 0.5;    // 0..1 of the route's normalized output
    double hysteresis = 0.05;  // 0..0.5 below the threshold before the gate re-arms
    double lengthMs = 100.0;   // trigger note length, 1..5000 ms
    int ccLo = 0;              // CC value at route output 0
    int ccHi = 127;            // CC value at route output 1
    bool program = false;      // index mode: also send program change (program = index, 0..127)

    bool enabled() const noexcept { return mode != ModMidiMode::Off; }
    bool operator==(const ModMidiOut& b) const noexcept {
        if (mode != b.mode) return false;
        if (mode == ModMidiMode::Off) return true;  // an Off block is identical whatever its stale fields
        return channel == b.channel && number == b.number && velocity == b.velocity && velocityFromValue == b.velocityFromValue &&
               threshold == b.threshold && hysteresis == b.hysteresis && lengthMs == b.lengthMs && ccLo == b.ccLo &&
               ccHi == b.ccHi && program == b.program;
    }
    bool operator!=(const ModMidiOut& b) const noexcept { return !(*this == b); }
    // Fields whose change must release what the route holds (identity of the sounding note).
    bool sameIdentity(const ModMidiOut& b) const noexcept {
        return mode == b.mode && (mode == ModMidiMode::Off || (channel == b.channel && number == b.number));
    }
};

inline bool validModMidiOut(const ModMidiOut& m) noexcept {
    if ((uint8_t)m.mode > (uint8_t)kLastModMidiMode) return false;
    if (m.mode == ModMidiMode::Off) return true;
    if (m.channel < 1 || m.channel > 16 || m.number < 0 || m.number > 127 || m.velocity < 1 || m.velocity > 127) return false;
    if (!std::isfinite(m.threshold) || m.threshold < 0.0 || m.threshold > 1.0) return false;
    if (!std::isfinite(m.hysteresis) || m.hysteresis < 0.0 || m.hysteresis > 0.5) return false;
    if (!std::isfinite(m.lengthMs) || m.lengthMs < 1.0 || m.lengthMs > 5000.0) return false;
    if (m.ccLo < 0 || m.ccLo > 127 || m.ccHi < 0 || m.ccHi > 127) return false;
    return true;
}

inline void sanitizeModMidiOut(ModMidiOut& m) noexcept {
    if ((uint8_t)m.mode > (uint8_t)kLastModMidiMode) m.mode = ModMidiMode::Off;
    m.channel = std::clamp(m.channel, 1, 16);
    m.number = std::clamp(m.number, 0, 127);
    m.velocity = std::clamp(m.velocity, 1, 127);
    m.threshold = std::clamp(std::isfinite(m.threshold) ? m.threshold : 0.5, 0.0, 1.0);
    m.hysteresis = std::clamp(std::isfinite(m.hysteresis) ? m.hysteresis : 0.05, 0.0, 0.5);
    m.lengthMs = std::clamp(std::isfinite(m.lengthMs) ? m.lengthMs : 100.0, 1.0, 5000.0);
    m.ccLo = std::clamp(m.ccLo, 0, 127);
    m.ccHi = std::clamp(m.ccHi, 0, 127);
}

// One MIDI message (status, data1, data2; size 2 for program change) at a sample offset within the host block.
struct ModMidiEvent {
    int sample = 0;
    uint8_t status = 0, d1 = 0, d2 = 0;
    uint8_t size = 3;
};

// Input of the generator for one route in one control period.
struct ModMidiRouteInput {
    const ModMidiOut* cfg = nullptr;   // nullptr / Off: route has no MIDI out
    bool active = false;               // route enabled, valid, Creative mode, transport running
    double norm = 0.0;                 // smoothed normalized curve output 0..1
    double raw = 0.0;                  // raw driver value (index mode)
    uint32_t identity = 0;             // object/driver/zone/enabled hash: a change releases the route's note
};

// Generator state. All arrays are fixed-size; nothing allocates.
template <int MaxRoutes>
class ModMidiGenerator {
public:
    static constexpr int kMaxEventsPerBlock = 192;  // hard cap per host block (except the release-all path)

    struct RouteState {
        bool configured = false;  // previous period had a MIDI config (for identity diffing)
        ModMidiOut cfg{};
        uint32_t identity = 0;
        bool gateOn = false;      // gate/trigger: latched above threshold
        bool primed = false;      // trigger: first active period only latches the level (no one-shot for "already inside")
        int heldCh = -1;          // channel 0..15 of the note this route holds, -1 = none
        int heldNote = 0;
        int64_t offDue = 0;       // trigger: samples until note-off, counted from the end of the last period
        int lastCc = -1;
        int lastIndex = 0;
        uint8_t flash = 0;
    };

    std::array<RouteState, MaxRoutes> routes{};
    std::array<std::array<uint8_t, 128>, 16> refCount{};   // how many routes hold (channel, note)
    std::array<std::array<int16_t, 128>, 16> lastCcSent{}; // -1 = unknown (dedup across routes)
    // UI telemetry: last message sent per route (status | d1 << 8 | d2 << 16 | 1 << 24 when valid) + counter.
    std::array<uint32_t, MaxRoutes> lastMsg{};
    std::array<uint32_t, MaxRoutes> sendCount{};
    uint64_t totalEvents = 0;
    uint64_t droppedEvents = 0;
    int eventsThisBlock = 0;

    ModMidiGenerator() noexcept { hardReset(); }

    void hardReset() noexcept {
        for (auto& r : routes) r = RouteState{};
        for (auto& ch : refCount) ch.fill(0);
        for (auto& ch : lastCcSent) ch.fill(-1);
        lastMsg.fill(0);
        sendCount.fill(0);
        eventsThisBlock = 0;
    }

    void beginBlock() noexcept { eventsThisBlock = 0; }

    bool anyHeld() const noexcept {
        for (const auto& r : routes) if (r.heldCh >= 0) return true;
        return false;
    }

    // Release every note held by any route (note-off per used channel/note, never CC 123) and re-arm all routes.
    // CC memory is forgotten so the next active period re-sends the current value.
    template <typename Sink>
    int releaseAll(Sink&& sink, int sample) noexcept {
        int sent = 0;
        for (int c = 0; c < 16; ++c)
            for (int n = 0; n < 128; ++n)
                if (refCount[(size_t)c][(size_t)n] > 0) {
                    refCount[(size_t)c][(size_t)n] = 0;
                    sink(ModMidiEvent{sample, (uint8_t)(0x80 | c), (uint8_t)n, 0, 3});
                    ++sent; ++totalEvents;
                }
        for (int i = 0; i < MaxRoutes; ++i) {
            auto& r = routes[(size_t)i];
            r.gateOn = false;
            r.primed = false;
            r.heldCh = -1;
            r.offDue = 0;
            r.lastCc = -1;
            r.lastIndex = 0;
        }
        for (auto& ch : lastCcSent) ch.fill(-1);
        return sent;
    }

    // One control period. `periodStart` is the block offset of the period's first sample, `len` its length; events
    // are stamped at the period's last sample (the pose the route output was computed from). Returns events written.
    template <typename Sink>
    int process(const ModMidiRouteInput* in, int count, int periodStart, int len, double sampleRate, Sink&& sink) noexcept {
        int written = 0;
        const int stamp = periodStart + std::max(1, len) - 1;
        auto emit = [&](int routeIdx, int sample, uint8_t status, uint8_t d1, uint8_t d2, uint8_t size) noexcept -> bool {
            if (eventsThisBlock >= kMaxEventsPerBlock) { ++droppedEvents; return false; }
            sink(ModMidiEvent{sample, status, d1, d2, size});
            ++eventsThisBlock; ++written; ++totalEvents;
            if (routeIdx >= 0) {
                lastMsg[(size_t)routeIdx] = (uint32_t)status | ((uint32_t)d1 << 8) | ((uint32_t)d2 << 16) | (1u << 24);
                ++sendCount[(size_t)routeIdx];
            }
            return true;
        };
        auto noteOn = [&](int i, int ch, int note, int vel, int sample) noexcept {
            auto& rc = refCount[(size_t)ch][(size_t)note];
            if (rc == 0) {
                if (!emit(i, sample, (uint8_t)(0x90 | ch), (uint8_t)note, (uint8_t)std::clamp(vel, 1, 127), 3)) return false;
            }
            rc = (uint8_t)std::min(255, rc + 1);
            routes[(size_t)i].heldCh = ch;
            routes[(size_t)i].heldNote = note;
            return true;
        };
        auto noteOff = [&](int i, int sample) noexcept {
            auto& r = routes[(size_t)i];
            if (r.heldCh < 0) return;
            auto& rc = refCount[(size_t)r.heldCh][(size_t)r.heldNote];
            if (rc > 0) {
                --rc;
                if (rc == 0) emit(i, sample, (uint8_t)(0x80 | r.heldCh), (uint8_t)r.heldNote, 0, 3);
            }
            r.heldCh = -1;
            r.offDue = 0;
        };

        const int n = std::clamp(count, 0, MaxRoutes);
        for (int i = 0; i < MaxRoutes; ++i) {
            auto& r = routes[(size_t)i];
            const bool has = i < n && in[i].cfg && in[i].cfg->enabled();
            // Route removed / MIDI switched off / identity changed: release what it holds first.
            if (r.configured && (!has || !r.cfg.sameIdentity(*in[i].cfg) || r.identity != in[i].identity)) {
                noteOff(i, periodStart);
                r = RouteState{};
                r.lastIndex = 0;
            }
            if (!has) { r.configured = false; continue; }
            const ModMidiOut& c = *in[i].cfg;
            r.configured = true;
            r.cfg = c;
            r.identity = in[i].identity;
            const int ch = c.channel - 1;

            // Trigger note-offs due inside this period happen whether or not the route is still active.
            if (r.heldCh >= 0 && c.mode == ModMidiMode::Trigger) {
                if (r.offDue <= len) {
                    noteOff(i, periodStart + (int)std::max<int64_t>(0, r.offDue - 1));
                } else {
                    r.offDue -= len;
                }
            }
            if (!in[i].active) {
                // Inactive (Physical, disabled, transport stopped): release the note, keep CC silent, re-arm.
                if (r.heldCh >= 0) noteOff(i, periodStart);
                r.gateOn = false;
                r.primed = false;
                r.lastIndex = 0;
                r.lastCc = -1;
                continue;
            }
            const double v = std::clamp(std::isfinite(in[i].norm) ? in[i].norm : 0.0, 0.0, 1.0);
            const int vel = c.velocityFromValue ? (int)std::lround(1.0 + v * 126.0) : c.velocity;
            switch (c.mode) {
            case ModMidiMode::Gate:
            case ModMidiMode::Trigger: {
                if (c.mode == ModMidiMode::Trigger && !r.primed) {
                    // After a reset (start, seek, state load, re-activation) a trigger only learns the current level:
                    // a one-shot fires on an observed upward crossing, so seek == continuous playback.
                    r.primed = true;
                    r.gateOn = v >= c.threshold;
                    break;
                }
                r.primed = true;
                const bool rising = !r.gateOn && v >= c.threshold;
                const bool falling = r.gateOn && v < c.threshold - c.hysteresis;
                if (rising) {
                    r.gateOn = true;
                    if (c.mode == ModMidiMode::Gate) {
                        if (r.heldCh < 0) noteOn(i, ch, c.number, vel, stamp);
                    } else {
                        // Retrigger rule: our own previous one-shot still sounding and nobody else holds the note ->
                        // note-off then note-on at the same sample (never two note-ons in a row).
                        if (r.heldCh >= 0) noteOff(i, stamp);
                        if (noteOn(i, ch, c.number, vel, stamp))
                            r.offDue = std::max<int64_t>(1, (int64_t)std::llround(c.lengthMs * 0.001 * sampleRate));
                    }
                } else if (falling) {
                    r.gateOn = false;
                    if (c.mode == ModMidiMode::Gate) noteOff(i, stamp);
                }
                break;
            }
            case ModMidiMode::CC: {
                const int value = std::clamp((int)std::lround(c.ccLo + v * (double)(c.ccHi - c.ccLo)), 0, 127);
                if (value != r.lastCc) {
                    r.lastCc = value;
                    auto& last = lastCcSent[(size_t)ch][(size_t)c.number];
                    if (last != value && emit(i, stamp, (uint8_t)(0xB0 | ch), (uint8_t)c.number, (uint8_t)value, 3))
                        last = (int16_t)value;
                }
                break;
            }
            case ModMidiMode::Index: {
                const double rawV = std::isfinite(in[i].raw) ? in[i].raw : 0.0;
                const int idx = std::clamp((int)std::lround(rawV), 0, 128);
                if (idx != r.lastIndex) {
                    r.lastIndex = idx;
                    if (r.heldCh >= 0) noteOff(i, stamp);
                    const int note = c.number + idx - 1;
                    if (idx >= 1 && note <= 127) noteOn(i, ch, note, vel, stamp);
                    if (c.program) emit(i, stamp, (uint8_t)(0xC0 | ch), (uint8_t)std::min(idx, 127), 0, 2);
                }
                break;
            }
            case ModMidiMode::Off:
                break;
            }
        }
        return written;
    }
};

} // namespace verbp
