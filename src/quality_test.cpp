#include <cstdio>
#include <sstream>
#include <thread>
#include <chrono>
#include "../plugin/RoomLibrary.h"
#include "rt_engine.hpp"

using namespace verb;
static int fails = 0;
static void check(bool ok, const char* message) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message); if (!ok) ++fails; }

static void testDecayAnalysis() {
    TailHist h; h.nBins = 4001; h.h.resize(2); h.reliableEnd.resize(2);
    for (auto& r : h.reliableEnd) r.fill(700);
    for (int r = 0; r < 2; ++r) for (int b = 0; b < NB; ++b) {
        h.h[r][b].assign(h.nBins, 0);
        if (b == NB - 1) continue;
        const double rt = 0.4 + 0.15 * b;
        for (size_t k = 0; k < h.nBins; ++k) h.h[r][b][k] = (r + 1) * std::pow(10.0, -6.0 * k * h.binDur / rt);
    }
    const auto d = analyzeTail(h);
    bool correct = true, monotonic = true;
    for (int b = 0; b < NB - 1; ++b) {
        correct &= std::abs(d.rt60[b] - (0.4 + 0.15 * b)) < 0.005;
        correct &= d.active[b] && std::abs(d.reliableSeconds[b] - 0.6) < 1e-9;
        for (size_t i = 1; i < TailDiagnostics::points; ++i)
            monotonic &= std::isfinite(d.decayDb[b][i]) && d.decayDb[b][i] <= d.decayDb[b][i - 1];
    }
    check(correct, "T20-derived RT60 matches known exponential decay in six bands");
    check(monotonic && !d.active[NB - 1] && d.rt60[NB - 1] < 0, "EDC is finite and monotonic; silent bands have no RT60");
    auto final = h;
    for (auto& r : final.reliableEnd) r.fill(20);
    check(tailNeedsMoreRays(h, final), "significant energy beyond confidence boundary requests refinement");
    for (auto& r : final.reliableEnd) r.fill(3500);
    check(!tailNeedsMoreRays(h, final), "inaudibly weak residual does not waste more rays");
}

static std::vector<float> render(const Scene& sc, int mask, int chunk, bool switching = false) {
    EngineConfig cfg; cfg.erOrder = 1; cfg.tailRays = 4000; cfg.tailThread = false;
    Engine e(sc, cfg); e.setSoloMask(mask);
    const size_t total = 48000;
    std::vector<float> result(total), input(chunk), zero(chunk), right(chunk);
    for (size_t pos = 0; pos < total;) {
        if (switching && pos == 8192) e.setSoloMask(2);
        if (switching && pos == 16384) e.setSoloMask(0);
        size_t next = total;
        if (switching && pos < 8192) next = 8192;
        else if (switching && pos < 16384) next = 16384;
        const int n = (int)std::min<size_t>(chunk, next - pos);
        for (int i = 0; i < n; ++i) {
            const size_t t = pos + i;
            input[i] = t >= 4096 ? (float)(0.1 * std::sin(2 * kPi * 523.25 * t / sc.sr)) : 0.f;
        }
        const float* ip[] = {input.data(), zero.data()};
        float* op[] = {result.data() + pos, right.data()};
        e.process(ip, op, n); pos += n;
    }
    return result;
}

static void testSolo() {
    auto sc = loadScene("scenes/demo_box.txt"); sc.sources.resize(1);
    const auto all = render(sc, 0, 512), direct = render(sc, 1, 512), early = render(sc, 2, 512), tail = render(sc, 4, 512);
    double peak = 0, error = 0, rampError = 0;
    const auto switched = render(sc, 0, 512, true), irregular = render(sc, 0, 37, true);
    bool same = true;
    for (size_t i = 4096; i < all.size(); ++i) {
        peak = std::max(peak, (double)std::abs(all[i]));
        error = std::max(error, (double)std::abs(all[i] - direct[i] - early[i] - tail[i]));
        double gain = i < 8192 ? 1.0 : i < 16384 ? std::max(0.0, 1.0 - (i - 8192 + 1) / 480.0) : std::min(1.0, (i - 16384 + 1) / 480.0);
        const double expected = early[i] + gain * (direct[i] + tail[i]);
        rampError = std::max(rampError, std::abs(switched[i] - expected));
        same &= std::abs(switched[i] - irregular[i]) < 1e-6;
    }
    check(peak > 0 && error < peak * 1e-5, "Direct + Early + Tail reconstruct the normal mix");
    check(rampError < peak * 2e-5, "Solo switches use the expected continuous 10 ms ramp");
    check(same, "Solo automation is independent of host block size");
}

static void testAdaptive() {
    const auto room = verbp::chamberHall(); std::istringstream is(room.text); auto sc = parseScene(is, room.name);
    EngineConfig cfg; cfg.erOrder = 1; cfg.tailThread = false; cfg.sharedTail = true; cfg.tailRays = 24000; cfg.adaptiveTail = true;
    Engine e(sc, cfg); auto d = e.tailDiagnostics();
    const int base = std::max(1500, cfg.tailRays / (int)sc.sources.size()) * (int)sc.sources.size();
    std::printf("Adaptive chamber: %d base, %d used, %d limit\n", base, d ? d->raysUsed : 0, d ? d->rayLimit : 0);
    check(d && d->adaptive && d->shared && d->raysUsed >= base && d->raysUsed <= 4 * base, "adaptive shared trace stays within its budget and publishes diagnostics");
    cfg.adaptiveTail = false; Engine fixed(sc, cfg); auto f = fixed.tailDiagnostics();
    check(f && !f->adaptive && f->raysUsed == base && f->rayLimit == base, "manual budget performs no automatic refinement");
    check(d && d->raysUsed > base, "sparse orchestra statistics actually trigger additional rays");

    cfg.tailThread = true; cfg.tailRefreshMs = 20;
    Engine live(sc, cfg); live.setTailQuality(48000, true);
    std::vector<float> zeros(256), left(256), right(256);
    std::vector<const float*> in(sc.sources.size(), zeros.data());
    float* out[] = {left.data(), right.data()};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool updated = false, finite = true;
    while (std::chrono::steady_clock::now() < deadline) {
        live.process(in.data(), out, 256);
        for (float v : left) finite &= std::isfinite(v);
        auto current = live.tailDiagnostics();
        if (current && current->baseRays == 48000 && current->adaptive && live.tailRefreshCount() >= 2) { updated = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(updated && finite && live.tailRefreshCount() >= 2, "shared-tail worker applies live quality changes while the audio thread runs");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try { testDecayAnalysis(); testSolo(); testAdaptive(); }
    catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
    std::printf("%d quality check(s) failed\n", fails); return fails ? 1 : 0;
}
