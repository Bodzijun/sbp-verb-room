#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <immintrin.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <vector>

namespace verb::bake_diag {
inline bool enabled() noexcept {
    static const bool on = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"VERB_BAKE_LOG", value, 2) == 1 && value[0] == L'1';
    }();
    return on;
}

struct FpState {
    unsigned mxcsr = 0;
    unsigned x87 = 0;
    static FpState read() noexcept {
        alignas(16) unsigned char area[512];
        _fxsave(area);
        unsigned short cw;
        std::memcpy(&cw, area, sizeof cw);
        return {_mm_getcsr(), cw};
    }
};

struct ThreadStart {
    unsigned index = 0;
    DWORD id = 0;
    FpState fp;
    FpState creatorFp;
    DWORD creatorId = 0;
};
inline thread_local ThreadStart workerStart;

inline constexpr unsigned ftzDaz = 0x8040;

class ScopedDenormalMode {
    const int mode;
    const unsigned before;
public:
    explicit ScopedDenormalMode(int setting) noexcept : mode(setting), before(mode < 0 ? 0 : _mm_getcsr()) {
        if (mode >= 0) _mm_setcsr(mode ? before | ftzDaz : before & ~ftzDaz);
    }
    ~ScopedDenormalMode() { if (mode >= 0) _mm_setcsr(before); }
};

struct ThreadCounters {
    uint64_t user = 0, kernel = 0, cycles = 0;
    bool timesOk = false, cyclesOk = false;
    static ThreadCounters read() noexcept {
        ThreadCounters result;
        FILETIME created{}, exited{}, kernel{}, user{};
        const auto thread = GetCurrentThread();
        result.timesOk = GetThreadTimes(thread, &created, &exited, &kernel, &user) != 0;
        const auto ticks = [](FILETIME time) {
            return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        result.user = ticks(user);
        result.kernel = ticks(kernel);
        result.cyclesOk = QueryThreadCycleTime(thread, &result.cycles) != 0;
        return result;
    }
};

struct ProcessSample {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    PROCESS_POWER_THROTTLING_STATE qos{};
    DWORD priority = 0;
    DWORD_PTR affinity = 0;
    bool memoryOk = false, qosOk = false;
    static ProcessSample read() noexcept {
        ProcessSample sample;
        const auto process = GetCurrentProcess();
        sample.memoryOk = K32GetProcessMemoryInfo(process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&sample.memory), sizeof sample.memory) != 0;
        sample.qos.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        sample.qosOk = GetProcessInformation(process, ProcessPowerThrottling, &sample.qos, sizeof sample.qos) != 0;
        sample.priority = GetPriorityClass(process);
        DWORD_PTR system = 0;
        GetProcessAffinityMask(process, &sample.affinity, &system);
        return sample;
    }
};

struct Worker {
    ThreadStart thread;
    FpState first, last;
    unsigned mxcsrControlsChanged = 0, x87Changed = 0;
    int priority = 0;
    THREAD_POWER_THROTTLING_STATE qos{};
    bool qosOk = false;
    unsigned jobs = 0, completed = 0, missingTimes = 0, missingCycles = 0;
    // GetCurrentProcessorNumber returns the processor number within the current group.
    // On the target (one group), 0-15 are P and 16-27 are E logical CPUs.
    std::array<unsigned, 64> cpu{};
    unsigned otherCpu = 0;
    uint64_t user = 0, kernel = 0, cycles = 0;

    void checkFp(FpState state) noexcept {
        // Ignore the six sticky exception flags for the change counter.
        mxcsrControlsChanged += ((first.mxcsr ^ state.mxcsr) & ~0x3fu) != 0;
        x87Changed += first.x87 != state.x87;
    }
};

class Bake {
    const ProcessSample before = ProcessSample::read();
    std::mutex mutex;
    std::vector<Worker> workers;
public:
    explicit Bake(unsigned maxWorkers) : workers(maxWorkers) {}

    class Job {
        Bake* bake;
        ThreadCounters before{};
    public:
        explicit Job(Bake* owner) noexcept : bake(owner) {
            if (!bake || workerStart.index >= bake->workers.size()) { bake = nullptr; return; }
            const auto fp = FpState::read();
            const auto cpu = GetCurrentProcessorNumber();
            const auto group = GetThreadGroupAffinityGroup();
            {
                const std::lock_guard<std::mutex> lock(bake->mutex);
                auto& row = bake->workers[workerStart.index];
                if (row.jobs++ == 0) { row.thread = workerStart; row.first = fp; row.last = fp; }
                row.checkFp(fp);
                if (group == 0 && cpu < row.cpu.size()) ++row.cpu[cpu];
                else ++row.otherCpu;
            }
            before = ThreadCounters::read();
        }
        Job(const Job&) = delete;
        Job& operator=(const Job&) = delete;
        ~Job() {
            if (!bake) return;
            const auto end = ThreadCounters::read();
            const auto fp = FpState::read();
            const auto thread = GetCurrentThread();
            const auto priority = GetThreadPriority(thread);
            THREAD_POWER_THROTTLING_STATE qos{};
            qos.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
            const bool qosOk = GetThreadInformation(thread, ThreadPowerThrottling, &qos, sizeof qos) != 0;
            const std::lock_guard<std::mutex> lock(bake->mutex);
            auto& row = bake->workers[workerStart.index];
            ++row.completed;
            row.last = fp;
            row.checkFp(fp);
            row.priority = priority;
            row.qos = qos;
            row.qosOk = qosOk;
            if (before.timesOk && end.timesOk) {
                row.user += end.user - before.user;
                row.kernel += end.kernel - before.kernel;
            } else ++row.missingTimes;
            if (before.cyclesOk && end.cyclesOk) row.cycles += end.cycles - before.cycles;
            else ++row.missingCycles;
        }
    private:
        static WORD GetThreadGroupAffinityGroup() noexcept {
            // All benchmark CPUs are in group 0. Also detect unsupported group mappings.
            GROUP_AFFINITY affinity{};
            return GetThreadGroupAffinity(GetCurrentThread(), &affinity) ? affinity.Group : 0xffff;
        }
    };

    std::string describe() {
        const auto after = ProcessSample::read();
        const std::lock_guard<std::mutex> lock(mutex);
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "diag_process faultBefore=" << before.memory.PageFaultCount
            << " faultAfter=" << after.memory.PageFaultCount
            << " wsBefore=" << before.memory.WorkingSetSize << " wsAfter=" << after.memory.WorkingSetSize
            << " privateBefore=" << before.memory.PrivateUsage << " privateAfter=" << after.memory.PrivateUsage
            << " memoryBeforeOk=" << before.memoryOk << " memoryAfterOk=" << after.memoryOk
            << " priorityBefore=0x" << std::hex << before.priority << " priorityAfter=0x" << after.priority
            << " affinity=0x" << after.affinity << std::dec
            << " qosBeforeOk=" << before.qosOk << " qosAfterOk=" << after.qosOk
            << " ctlBefore=" << before.qos.ControlMask << " stateBefore=" << before.qos.StateMask
            << " ctlAfter=" << after.qos.ControlMask << " stateAfter=" << after.qos.StateMask;
        for (const auto& row : workers) {
            if (row.jobs == 0) continue;
            out << "\ndiag_worker index=" << row.thread.index << " tid=" << row.thread.id
                << " creatorTid=" << row.thread.creatorId
                << " creatorMxcsr=0x" << std::hex << row.thread.creatorFp.mxcsr << " creatorX87=0x" << row.thread.creatorFp.x87
                << " startMxcsr=0x" << row.thread.fp.mxcsr << " startX87=0x" << row.thread.fp.x87
                << " firstMxcsr=0x" << row.first.mxcsr << " firstX87=0x" << row.first.x87
                << " endMxcsr=0x" << row.last.mxcsr << " endX87=0x" << row.last.x87 << std::dec
                << " mxcsrControlChanges=" << row.mxcsrControlsChanged << " x87Changes=" << row.x87Changed
                << " jobs=" << row.jobs << " completed=" << row.completed << " priority=" << row.priority
                << " qosOk=" << row.qosOk << " ctl=" << row.qos.ControlMask << " state=" << row.qos.StateMask
                << " missingTimes=" << row.missingTimes << " missingCycles=" << row.missingCycles
                << " userS=" << std::fixed << std::setprecision(7) << double(row.user) * 1e-7
                << " kernelS=" << double(row.kernel) * 1e-7 << " cycles=" << row.cycles << " cpu=";
            const char* sep = "";
            for (size_t i = 0; i < row.cpu.size(); ++i) if (row.cpu[i]) {
                out << sep << i << ':' << row.cpu[i];
                sep = ",";
            }
            out << " otherCpu=" << row.otherCpu;
        }
        return out.str();
    }
};
}
#endif
