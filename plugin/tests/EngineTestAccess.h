#pragma once
#include "../PluginProcessor.h"

struct E9bAccess {
    static std::shared_ptr<verb::Engine> engine(VerbProcessor& p) {
        juce::SpinLock::ScopedLockType lock(p.engineLock);
        return p.bundle ? p.bundle->engine : nullptr;
    }
};
