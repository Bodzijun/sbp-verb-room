#pragma once
#include "../PluginProcessor.h"

struct E9bAccess {
    static std::shared_ptr<verb::Engine> engine(VerbProcessor& p) {
        juce::SpinLock::ScopedLockType lock(p.engineLock);
        return p.bundle ? p.bundle->engine : nullptr;
    }
    static std::array<double, 3> hermiteVel(const VerbProcessor& p, int o) {
        return {p.hVel[(size_t)o][0], p.hVel[(size_t)o][1], p.hVel[(size_t)o][2]};
    }
    static std::array<double, 3> hermiteEnd(const VerbProcessor& p, int o) {
        return {p.hPEnd[(size_t)o][0], p.hPEnd[(size_t)o][1], p.hPEnd[(size_t)o][2]};
    }
};
