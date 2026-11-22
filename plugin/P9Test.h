#pragma once
// Opt-in Flate bake benchmark. Use a fresh process per run.
#include "tests/EngineTestAccess.h"
#include <algorithm>
#include <cstdio>
#include "P9cTestControls.h"

#if JUCE_WINDOWS
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace p9 {
using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static bool waitFor(const std::function<bool()>& condition, double timeoutMs) {
    const auto start = Clock::now();
    do {
        if (condition()) return true;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    } while (msSince(start) < timeoutMs);
    return condition();
}

static bool waitForCurrentEngine(VerbProcessor& p) {
    const auto current = [&] { return !p.isRoomLoadPending() && p.isWebTailCurrent(); };
    return waitFor([&] { return p.getLoadState() == 3 || current(); }, 120000) && current();
}

#if JUCE_WINDOWS
static double cpuSeconds(HANDLE handle, bool thread) {
    FILETIME created{}, exited{}, kernel{}, user{};
    const BOOL ok = thread ? GetThreadTimes(handle, &created, &exited, &kernel, &user)
                           : GetProcessTimes(handle, &created, &exited, &kernel, &user);
    const auto ticks = [](FILETIME time) {
        return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return ok ? double(ticks(kernel) + ticks(user)) * 1e-7 : 0.;
}

struct ThreadSample { DWORD id; int priority; double cpu; };

static std::vector<ThreadSample> threads() {
    std::vector<ThreadSample> result;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
        if (auto thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID)) {
            result.push_back({entry.th32ThreadID, GetThreadPriority(thread), cpuSeconds(thread, true)});
            CloseHandle(thread);
        }
    }
    CloseHandle(snapshot);
    return result;
}
#endif

struct Audio {
    std::atomic<bool> stop{false};
    std::thread thread;
    uint64_t blocks = 0, missed = 0;

    Audio(VerbProcessor& p, int channels, bool enabled) {
        if (!enabled) return;
        thread = std::thread([this, &p, channels] {
            juce::AudioBuffer<float> buffer(channels, 256);
            juce::MidiBuffer midi;
            constexpr auto period = std::chrono::nanoseconds(5333333);
            auto next = Clock::now();
#if JUCE_WINDOWS
            // High-resolution waitable timer. Fall back on older Windows.
            const auto timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
#endif
            while (!stop.load()) {
                buffer.clear();
                p.processBlock(buffer, midi);
                ++blocks;
                next += period;
                const auto now = Clock::now();
                if (now > next) {
                    missed += 1 + (now - next) / period;
                    next += (1 + (now - next) / period) * period;
                }
#if JUCE_WINDOWS
                const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(next - now).count() / 100;
                LARGE_INTEGER due{};
                due.QuadPart = -std::max<int64_t>(1, ticks);
                if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)
                    && WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) continue;
#endif
                std::this_thread::sleep_until(next);
            }
#if JUCE_WINDOWS
            if (timer) CloseHandle(timer);
#endif
        });
    }

    void finish() { stop = true; if (thread.joinable()) thread.join(); }
    ~Audio() { finish(); }
};

using Editor = juce::AudioProcessorEditor;

static int run(const std::function<std::unique_ptr<Editor>(VerbProcessor&)>& attach = {},
               const std::function<bool(Editor&)>& trigger = {},
               const std::function<bool(Editor&)>& observe = {}) {
    if (!configureP9cTest()) return 1;
    const bool closeEditor = juce::SystemStats::getEnvironmentVariable("VERB_P9C_CLOSE_EDITOR", {}) == "1";
    const auto layout = juce::SystemStats::getEnvironmentVariable("VERB_P9_LAYOUT", "preset");
    const auto audio = juce::SystemStats::getEnvironmentVariable("VERB_P9_AUDIO", "1");
    const unsigned workers = (unsigned)juce::SystemStats::getEnvironmentVariable("VERB_P9_WORKERS", "0").getIntValue();
    if ((layout != "preset" && layout != "lcr")
        || (audio != "0" && audio != "1") || workers > 64 || (workers && trigger)) {
        std::printf("P9 invalid scenario\n");
        return 1;
    }
    VerbProcessor p;
    const int channels = layout == "lcr" ? 3 : 2;
    p.setPlayConfigDetails(2, channels, 48000, 256);
    p.prepareToPlay(48000, 256);
    // Let both scenarios consume the same pending convolution handovers before the bake.
    Audio warmup(p, channels, true);
    if (!waitForCurrentEngine(p)) { std::printf("P9 initial engine unavailable: %s\n", p.getStatus().toRawUTF8()); return 1; }
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile("test/Flate.verbroom");
    juce::String error;
    const auto loadStart = Clock::now();
    if (!p.loadRoomPresetText(file.loadFileAsString(), error) || !waitForCurrentEngine(p)) {
        std::printf("P9 preset unavailable: %s, %s\n", error.toRawUTF8(), p.getStatus().toRawUTF8());
        return 1;
    }
    const double loadMs = msSince(loadStart);
    if (layout == "lcr") {
        auto output = p.getOutputCfg();
        output.layout = (int)verb::Layout::LCR;
        p.setOutputCfg(output);
        if (!waitForCurrentEngine(p) || p.numOutputChannels() != 3) { std::printf("P9 LCR unavailable\n"); return 1; }
    }
    warmup.finish();
    auto engine = E9bAccess::engine(p);
    if (!engine || p.pathBakePercent() != 0) { std::printf("P9 expected a fresh unbaked engine\n"); return 1; }
    Audio callbacks(p, channels, audio == "1");
    auto editor = attach ? attach(p) : nullptr;
    if (attach && !editor) return 1;
    const auto quality = p.getTailSettings();
    auto value = [&](const char* id) { return p.apvts.getRawParameterValue(id)->load(); };
    const auto source = p.getSources().at(0);
    std::printf("P9_CONFIG loadMs=%.1f layout=%d out=%d sourceFormat=%d points=%zu mode=%.0f early=%.4f tail=%.4f baseRays=%d adaptive=%d audio=%s editor=%d override=%u\n",
        loadMs, p.effectiveLayout(), p.numOutputChannels(), source.format,
        verbp::buildPoints(p.getSources()).size(), value("mode"), value("early"), value("tail"),
        quality.rays, (int)quality.adaptive, audio.toRawUTF8(), (int)(bool)attach, workers);
#if JUCE_WINDOWS
    DWORD_PTR affinity = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &affinity, &system);
    std::printf("P9_PROCESS affinity=%llx system=%llx priorityClass=%lu\n",
        (unsigned long long)affinity, (unsigned long long)system, GetPriorityClass(GetCurrentProcess()));
    const auto initialThreads = threads();
    const auto initialCpu = cpuSeconds(GetCurrentProcess(), false);
#endif
    const auto previous = engine->bakeStats();
    const auto start = Clock::now();
    if (workers) {
        // Comparison only: the same prepared engine and paths, with its test worker-count override.
        engine->requestPathBake(workers);
    } else if (!(editor && trigger ? trigger(*editor) : p.startPathBake(verbp::kListener))) {
        std::printf("P9 bake refused\n");
        return 1;
    }
    if (closeEditor && editor) {
        if (!waitFor([&] { return engine->bakeStats().state == verb::Engine::BakeState::Baking; }, 5000)) {
            std::printf("P9c editor-close control: Bake path was not dispatched\n");
            return 1;
        }
        editor.reset();
        std::printf("P9c editor closed %.1f ms after trigger\n", msSince(start));
    }

    double engineDone = -1., firstStations = -1.;
    int lastPercent = -1, lastWait = -1;
    const auto completed = waitFor([&] {
        const auto stats = engine->bakeStats();
        const double now = msSince(start);
        if (stats.percent == 100 && engineDone < 0.) engineDone = now;
        if (stats.stations > 0 && firstStations < 0.) firstStations = now;
        if (stats.percent / 10 != lastPercent || stats.waitReason != lastWait) {
            lastPercent = stats.percent / 10;
            lastWait = stats.waitReason;
            std::printf("P9_PROGRESS ms=%.1f percent=%d processor=%d state=%d wait=%d stations=%llu\n",
                now, stats.percent, p.pathBakePercent(), (int)stats.state, stats.waitReason,
                (unsigned long long)stats.stations);
        }
        return p.pathBakePercent() == 100;
    }, 300000);
    const double processorDone = msSince(start);
#if JUCE_WINDOWS
    const auto finalCpu = cpuSeconds(GetCurrentProcess(), false);
    const auto finalThreads = threads(); // sample while pool and audio thread still exist
#endif
    const bool observed = !(editor && observe && completed) || observe(*editor);
    callbacks.finish();
    const auto stats = engine->bakeStats();
    const auto trace = engine->bakeTrace();
    const auto tables = engine->bakedTablesSnapshot();
    std::printf("P9_RESULT complete=%d wallMs=%.1f engineDoneMs=%.1f engineMs=%.1f firstStationMs=%.1f workers=%d activeLimit=%d resets=%llu cancelled=%llu audioBlocks=%llu audioMissed=%llu hash=%016llx discontinuities=%d\n",
        (int)completed, processorDone, engineDone, stats.milliseconds, firstStations,
        trace.workers, trace.activeLimit, (unsigned long long)(stats.resets - previous.resets),
        (unsigned long long)(stats.cancelled - previous.cancelled), (unsigned long long)callbacks.blocks,
        (unsigned long long)callbacks.missed, (unsigned long long)stats.hash, stats.discontinuities);
    if (!completed || stats.percent != 100 || tables.empty() || E9bAccess::engine(p) != engine) return 1;
    for (const auto& table : tables)
        std::printf("P9_TABLE group=%d sources=%d receivers=%d nodes=%zu estimator=%d selectedRays=%d\n",
            table->path.group, table->sources, table->receivers, table->u.size(), table->estimator, table->selectedRays);
    double lastInitial = 0., lastStation = 0.;
    unsigned initialJobs = 0, validationJobs = 0, retries = 0;
    for (const auto& station : trace.stations) {
        if (station.kind == 0) { ++initialJobs; lastInitial = std::max(lastInitial, station.doneMs); }
        else ++validationJobs;
        lastStation = std::max(lastStation, station.doneMs);
        retries += station.retry;
        std::printf("P9_STATION group=%d u=%.8f kind=%d rays=%d queuedMs=%.1f doneMs=%.1f activeAtQueue=%d retry=%d\n",
            station.group, station.u, station.kind, station.rays, station.queuedMs, station.doneMs,
            station.activeAtQueue, (int)station.retry);
    }
    std::printf("P9_PHASE initialJobs=%u validationJobs=%u retries=%u lastInitialMs=%.1f lastStationMs=%.1f maxStepGapMs=%.1f waitSteps=%llu\n",
        initialJobs, validationJobs, retries, lastInitial, lastStation, trace.maxStepGapMs,
        (unsigned long long)trace.waitSteps);
#if JUCE_WINDOWS
    double poolCpu = 0.;
    int poolActive = 0;
    for (const auto& end : finalThreads) {
        const auto begin = std::find_if(initialThreads.begin(), initialThreads.end(),
            [&](const ThreadSample& sample) { return sample.id == end.id; });
        const auto delta = end.cpu - (begin == initialThreads.end() ? 0. : begin->cpu);
        if (delta <= 0.001) continue;
        std::printf("P9_THREAD id=%lu priority=%d cpuSeconds=%.3f\n", end.id, end.priority, delta);
        if (end.priority == THREAD_PRIORITY_LOWEST) { poolCpu += delta; ++poolActive; }
    }
    std::printf("P9_CPU processSeconds=%.3f lowestPrioritySeconds=%.3f lowestPriorityActive=%d\n",
        finalCpu - initialCpu, poolCpu, poolActive);
#endif
    const auto saveStart = Clock::now();
    juce::MemoryBlock state;
    p.getStateInformation(state);
    std::printf("P9_SAVE stateMs=%.2f bytes=%zu\n", msSince(saveStart), state.getSize());
    p.releaseResources();
    return observed ? 0 : 1;
}
} // namespace p9

static int testP9() { return p9::run(); }
