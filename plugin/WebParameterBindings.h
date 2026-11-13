#pragma once
#include "PluginProcessor.h"
#include "WebJson.h"
namespace verbweb {
// Message-thread polling avoids 243 audio-thread callbacks, allocation and AsyncUpdater posts.
class WebParameterBindings {
public:
    explicit WebParameterBindings(VerbProcessor&);
    ~WebParameterBindings();
    juce::var descriptors() const;
    juce::var snapshot(bool tailCurrent) const;
    bool changed();
    juce::var apply(const juce::var& batch);
    void endAll();
private:
    struct Binding { juce::String id; juce::RangedAudioParameter* parameter; std::atomic<float>* raw; float last = -999999.f; bool gesture = false; bool staleEnded = false; };
    VerbProcessor& proc;
    std::vector<Binding> bindings;
};
}
