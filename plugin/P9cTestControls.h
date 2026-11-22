#pragma once
#include "bake_pool.hpp"

// Called only by standalone bake benchmarks before constructing the processor.
inline bool configureP9cTest() {
#if JUCE_WINDOWS
    const auto mode = [](const char* name) {
        const auto value = juce::SystemStats::getEnvironmentVariable(name, {});
        if (value.isEmpty()) return -1;
        if (value == "0") return 0;
        if (value == "1") return 1;
        return -2;
    };
    const int creator = mode("VERB_P9C_CREATOR_FTZ");
    const int station = mode("VERB_P9C_STATION_FTZ");
    if (creator == -2 || station == -2) {
        std::printf("P9c invalid FTZ control (use 0 or 1)\n");
        return false;
    }
    if (juce::SystemStats::getEnvironmentVariable("VERB_P9C_HIGH", {}) == "1"
        && !SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
        std::printf("P9c could not set High priority: %lu\n", GetLastError());
        return false;
    }
    verb::BakePool::testCreatorDenormalMode = creator;
    verb::BakePool::testStationDenormalMode = station;
#endif
    return true;
}
