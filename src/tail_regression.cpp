// Reproductions for the 0.1.3 forum report. No JUCE/host required.
#include <cstdio>
#include <filesystem>
#include <sstream>
#include "../plugin/RoomLibrary.h"
#include "rt_engine.hpp"
#include "dsp.hpp"
#include "tail.hpp"

using namespace verb;
static int failures = 0;
static void check(bool ok, const char* name) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

static Scene hall(int which, int seed, double sr, bool faceStage = false) {
    const auto room = which == 0 ? verbp::chamberHall() : verbp::symphonyHall(which >= 2);
    std::istringstream is(room.text);
    Scene sc = parseScene(is, room.name);
    sc.seed = seed; sc.sr = sr;
    if (which == 3) sc.listenerPos.y -= 3.0; // conductor, three metres towards the audience
    // The live plugin builds this array, then applies the listener's automatable
    // yaw. Test both its initial orientation and the settled preset orientation.
    sc.receivers = makeReceiverArray(Layout::StereoORTF, sc.listenerPos, 0.3, 2);
    if (faceStage) {
        const double a = sc.listenerYaw * kPi / 180.0;
        for (auto& r : sc.receivers) {
            const Vec3 d = r.pos - sc.listenerPos;
            r.pos = sc.listenerPos + Vec3{d.x * std::cos(a) - d.y * std::sin(a), d.x * std::sin(a) + d.y * std::cos(a), d.z};
            r.azimuthDeg += sc.listenerYaw;
        }
    } else sc.listenerYaw = 0;
    return sc;
}

struct Run { Wav wav; double length; };
static Run impulse(const Scene& sc, bool shared, int source, size_t onset = 0, int chunk = 512) {
    EngineConfig cfg;
    cfg.erOrder = 1; cfg.tailThread = false; cfg.sharedTail = shared;
    cfg.tailRays = shared ? 48000 : 40000;
    cfg.maxEarlyDelaySec = shared ? 0.4 : 0.75;
    Engine eng(sc, cfg);
    const size_t total = onset + (size_t)((eng.tailSeconds() + 0.5) * sc.sr);
    Run run; run.length = eng.tailSeconds(); run.wav.sr = (int)sc.sr;
    run.wav.ch.assign(sc.receivers.size(), std::vector<float>(total));
    std::vector<float> input(chunk), zero(chunk);
    std::vector<const float*> ip(sc.sources.size(), zero.data());
    std::vector<float*> op(sc.receivers.size());
    for (size_t pos = 0; pos < total;) {
        const int n = (int)std::min<size_t>(chunk, total - pos);
        std::fill(input.begin(), input.end(), 0.f);
        if (onset >= pos && onset < pos + n) input[onset - pos] = 1.f;
        ip[source] = input.data();
        for (size_t r = 0; r < op.size(); ++r) op[r] = run.wav.ch[r].data() + pos;
        eng.process(ip.data(), op.data(), n);
        pos += n;
    }
    return run;
}

static std::vector<double> envelope(const Wav& wav) {
    const size_t hop = (size_t)(0.05 * wav.sr);
    std::vector<double> db;
    for (size_t k = 0; k + hop <= wav.ch[0].size(); k += hop) {
        double e = 0;
        for (const auto& ch : wav.ch)
            for (size_t i = k; i < k + hop; ++i) e += (double)ch[i] * ch[i];
        db.push_back(10 * std::log10(e / (hop * wav.ch.size()) + 1e-30));
    }
    return db;
}

// Count re-growth even after a hole in the envelope. Ignoring everything after the
// first quiet window would hide exactly the disappearing/reappearing tail bug.
static double regrowth(const std::vector<double>& db, double start = 0.3) {
    double low = 1e9, rise = 0;
    const double floor = *std::max_element(db.begin(), db.end()) - 80;
    for (size_t k = (size_t)std::lround(start / 0.05); k < db.size(); ++k) {
        const double v = std::max(db[k], floor);
        rise = std::max(rise, v - low);
        low = std::min(low, v);
    }
    return rise;
}

static void save(const Run& run, const std::string& stem) {
    writeWav(stem + "-impulse.wav", run.wav);
    std::ofstream csv(stem + "-envelope.csv");
    csv << "seconds,db\n";
    const auto db = envelope(run.wav);
    for (size_t k = 0; k < db.size(); ++k) csv << (k + 0.5) * 0.05 << ',' << db[k] << '\n';
    // Same clean 1046.5 Hz burst as the forum test; preserve absolute gains for A/B.
    std::vector<float> tone((size_t)(0.35 * run.wav.sr));
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = (float)(0.3 * std::sin(2 * kPi * 1046.5 * i / run.wav.sr));
    Wav wet; wet.sr = run.wav.sr;
    for (const auto& ch : run.wav.ch) wet.ch.push_back(convolve(ch, tone));
    writeWav(stem + "-tone.wav", wet);
}

static void syntheticDecay() {
    TailHist h;
    h.nBins = 2002; h.binDur = 0.001; h.h.resize(1); h.h2.resize(1);
    for (int b = 0; b < NB; ++b) {
        h.h[0][b].assign(h.nBins, 0.0);
        h.h2[0][b].assign(h.nBins, 0.0);
    }
    for (size_t k = 40; k < h.nBins; ++k) {
        const double truth = std::pow(10.0, -0.006 * (k - 40)); // -60 dB/s
        if (k < 220) {
            h.h[0][3][k] = truth;
            h.h2[0][3][k] = truth * truth / 20;
        } else if (k % 180 == 0) {
            h.h[0][3][k] = 180 * truth;
            h.h2[0][3][k] = std::pow(180 * truth, 2);
        }
    }
    finalizeTail(h);
    double rise = 0, low = 1e9;
    for (size_t k = 400; k < h.nBins; ++k) {
        const double db = 10 * std::log10(h.h[0][3][k] + 1e-30);
        rise = std::max(rise, db - low); low = std::min(low, db);
    }
    const double error = 10 * std::log10(h.h[0][3][1000]) + 0.06 * 960;
    std::printf("Synthetic short decay: reliable end %zu ms, growth %.2f dB, level error %.2f dB\n", h.reliableEnd[0][3], rise, error);
    check(h.reliableEnd[0][3] < 400 && rise < 1.0, "sparse decay before 400 ms is repaired, including later isolated survivors");
    check(std::abs(error) < 4, "short-tail correction preserves the known decay slope and level");
    bool silent = true;
    for (int b : {0, 1, 2, 4, 5, 6}) for (double v : h.h[0][b]) silent &= v == 0;
    check(silent, "empty bands stay silent");

    // An open, absorbing space can lose its rays while the short initial field
    // is still building. A near-flat fitted slope must not invent tail energy.
    std::fill(h.h[0][3].begin(), h.h[0][3].end(), 0.0);
    std::fill(h.h2[0][3].begin(), h.h2[0][3].end(), 0.0);
    double before = 0;
    for (size_t k = 40; k < 220; ++k) {
        const double e = std::pow(10.0, 0.002 * (k - 40));
        h.h[0][3][k] = e; h.h2[0][3][k] = e * e / 20;
        before += e;
    }
    finalizeTail(h);
    double after = 0;
    for (double e : h.h[0][3]) after += e;
    std::printf("Uncertain rising fit: energy ratio %.4f\n", after / before);
    check(after <= before * 1.02, "an uncertain fit cannot create extra reverberant energy");
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string dir = argc > 1 ? argv[1] : "";
    if (!dir.empty()) std::filesystem::create_directories(dir);
    if (std::getenv("VERB_VSL_TAIL_ONLY")) {
        for (bool conductor : {false, true}) {
            const auto room = verbp::vslGrandHall(conductor);
            std::istringstream in(room.text); auto sc = parseScene(in, room.name);
            sc.sr = 44100; sc.receivers = makeReceiverArray(Layout::StereoORTF, sc.listenerPos, 0.1, 2);
            const double angle = sc.listenerYaw * kPi / 180.0;
            for (auto& receiver : sc.receivers) {
                const auto d = receiver.pos - sc.listenerPos;
                receiver.pos = sc.listenerPos + Vec3{d.x * std::cos(angle) - d.y * std::sin(angle), d.x * std::sin(angle) + d.y * std::cos(angle), d.z};
                receiver.azimuthDeg += sc.listenerYaw;
            }
            for (int source : {0, 31}) {
                const auto run = impulse(sc, true, source);
                const auto db = envelope(run.wav);
                const double growth = regrowth(db, 0.4);
                bool finite = true; for (const auto& channel : run.wav.ch) for (float sample : channel) finite &= std::isfinite(sample);
                std::printf("VSL %s, source %d: %.2f s, late regrowth %.2f dB\n", conductor ? "conductor" : "audience", source + 1, run.length, growth);
                check(finite && growth < 3.0, "VSL late tail stays finite without phantom regrowth");
                if (!dir.empty() && source == 0) save(run, dir + (conductor ? "/vsl-conductor" : "/vsl-audience"));
            }
        }
        return failures ? 1 : 0;
    }
    if (std::getenv("VERB_CATHEDRAL_ONLY")) {
        const auto rooms = verbp::builtinRooms();
        const auto it = std::find_if(rooms.begin(), rooms.end(), [](const auto& r) { return r.name == "Cathedral (long tail)"; });
        if (it == rooms.end()) return 1;
        std::istringstream in(it->text);
        Scene sc = parseScene(in, it->name);
        sc.sr = 44100;
        sc.receivers = makeReceiverArray(Layout::StereoORTF, sc.listenerPos, 0.1, 2);
        while (sc.sources.size() < 8) sc.sources.push_back(sc.sources[0]);
        const auto run = impulse(sc, true, 0);
        std::printf("Cathedral shared: %.3f s\n", run.length);
        const auto levels = envelope(run.wav);
        const double peak = *std::max_element(levels.begin(), levels.end());
        const size_t beforeFade = (size_t)std::max(0.0, std::floor((run.length - 0.65) / 0.05));
        const double fadeStart = levels[std::min(beforeFade, levels.size() - 1)] - peak;
        std::printf("  level before final fade: %.1f dB relative to peak\n", fadeStart);
        check(run.length > 10.0, "long cathedral is not truncated at ten seconds");
        check(fadeStart < -75.0, "final fade starts after cathedral tail becomes quiet");
        if (!dir.empty()) save(run, dir + "/cathedral");
        return failures ? 1 : 0;
    }
    const bool quick = argc > 2 && std::string(argv[2]) == "quick";
    const char* names[] = {"chamber", "audience", "conductor", "conductor-further"};
    try {
        syntheticDecay();
        for (int which = 0; which < 4; ++which)
            for (int seed = 1; seed <= (quick ? 1 : 3); ++seed) {
                const Scene sc = hall(which, seed, 44100);
                const auto run = impulse(sc, true, 0);
                const double rise = regrowth(envelope(run.wav));
                std::printf("%s seed %d: length %.3f s, late regrowth %.2f dB\n", names[which], seed, run.length, rise);
                check(rise < 3.0, "shared hall has no phantom late energy burst (>3 dB)");
                if (!dir.empty() && seed == 1) save(run, dir + "/" + names[which]);
                if (which == 0 && seed == 1) {
                    const size_t offset = 3 * 44100 + 37;
                    const auto shifted = impulse(sc, true, 0, offset, 1000);
                    double error = 0, peak = 0, pre = 0, post = 0;
                    for (size_t r = 0; r < run.wav.ch.size(); ++r) {
                        const auto& a = run.wav.ch[r]; const auto& b = shifted.wav.ch[r];
                        for (size_t i = 0; i < a.size(); ++i) {
                            peak = std::max(peak, (double)std::abs(a[i]));
                            error = std::max(error, (double)std::abs(a[i] - b[offset + i]));
                            if (i > (run.length + 0.1) * sc.sr) post = std::max(post, (double)std::abs(a[i]));
                        }
                        for (size_t i = 0; i < offset; ++i) pre = std::max(pre, (double)std::abs(b[i]));
                    }
                    check(error < peak * 1e-5, "non-aligned onset and host block size preserve the impulse response");
                    check(pre == 0 && post < peak * 1e-7, "silence before input and after the tail; no buffer repetition");
                }
            }
        if (!quick) {
            for (int which = 0; which < 4; ++which)
                for (int seed = 1; seed <= 3; ++seed) {
                    const auto run = impulse(hall(which, seed, 48000, true), true, 1, 0, 257);
                    const double rise = regrowth(envelope(run.wav));
                    std::printf("Preset yaw, %s seed %d: late growth %.2f dB\n", names[which], seed, rise);
                    check(rise < 3, "settled preset orientation, second violin source, 48 kHz");
                }
            for (double sr : {44100.0, 48000.0, 96000.0}) {
                Scene sc = hall(0, 1, sr);
                sc.sources.resize(1);
                const auto run = impulse(sc, false, 0, 0, 317);
                const double rise = regrowth(envelope(run.wav));
                std::printf("Single-source chamber %.0f Hz: length %.3f s, growth %.2f dB\n", sr, run.length, rise);
                check(rise < 3, "single-source tail remains smooth at supported sample rates");
                if (sr != 44100) {
                    const auto multi = impulse(hall(0, 1, sr), true, 13, 0, 73);
                    check(regrowth(envelope(multi.wav)) < 3, "shared tail: last source and short host blocks at 48/96 kHz");
                }
            }
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
