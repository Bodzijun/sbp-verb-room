#pragma once
#include "PluginProcessor.h"
#include "PluginLookAndFeel.h"
#include "../src/energy.hpp"
#include <atomic>
#include <mutex>
#include <thread>

class EnergyMapView : public juce::Component {
public:
    explicit EnergyMapView(VerbProcessor&);
    ~EnergyMapView() override;
    juce::Component& controlsPanel() { return controls; }
    void layoutControls();
    void update();
    void startAnalysis();
    void cancelAnalysis();
    void autoContrast();
    bool isBusy() const { return working.load(); }
    bool isStale() const { return stale; }
    std::shared_ptr<const verb::EnergyMap> result() const { return data; }
    void paint(juce::Graphics&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
private:
    verb::EnergyMapOptions options() const;
    juce::String signature(const verb::Scene&, const verb::EnergyMapOptions&) const;
    void setControlsEnabled(bool);
    void refreshSources();
    double cellEnergy(const verb::FieldEnergy&) const;
    juce::Rectangle<float> plotBounds() const;
    static juce::Colour heat(float);
    VerbProcessor& proc;
    juce::Component controls;
    juce::Label title, sourceLabel, bandLabel, heightLabel, qualityLabel, gridLabel, timeLabel, rangeLabel, status, explanation;
    juce::ComboBox sources, bands, quality, grid;
    juce::Slider height, duration, range;
    juce::ToggleButton direct{"Direct"}, early{"Early"}, tail{"Tail"};
    juce::TextButton analyze{"Analyze"}, cancel{"Cancel"}, autoScale{"Auto"};
    std::thread worker;
    std::atomic<bool> working{false}, stop{false};
    std::atomic<double> progress{0};
    std::mutex completionMutex;
    std::shared_ptr<const verb::EnergyMap> pending, data;
    juce::String pendingError, submittedSignature, resultSignature;
    bool stale = false;
    int hover = -1;
};
