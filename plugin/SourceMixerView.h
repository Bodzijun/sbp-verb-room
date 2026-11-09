#pragma once
#include "PluginProcessor.h"
#include "PluginLookAndFeel.h"

// Message-thread controls; the meters read the same atomic input peaks as the map.
class SourceMixerView : public juce::Component {
    class Strip : public juce::Component {
    public:
        Strip(VerbProcessor& p, int i) : proc(p), index(i) {
            for (auto* c : std::initializer_list<juce::Component*>{&name, &format, &input, &gain, &spread, &pattern, &sum}) addAndMakeVisible(*c);
            name.setSelectAllWhenFocused(true);
            name.onReturnKey = [this] { commitName(); name.giveAwayKeyboardFocus(); };
            name.onFocusLost = [this] { commitName(); };
            for (int k = 0; k < (int)verbp::sourceFormats().size(); ++k) format.addItem(verbp::sourceFormats()[k].name, k + 1);
            for (int k = 1; k <= verbp::kMaxInputChannels; ++k) input.addItem("Input " + juce::String(k), k);
            format.onChange = [this] { edit([this](auto& c) { c.format = format.getSelectedId() - 1; }); };
            input.onChange = [this] { edit([this](auto& c) { c.firstIn = input.getSelectedId() - 1; }); };
            sum.setButtonText("Sum to mono");
            sum.onClick = [this] { edit([this](auto& c) { c.sum = sum.getToggleState(); }); };
            gain.setSliderStyle(juce::Slider::LinearVertical);
            gain.getProperties().set("sourceFader", true);
            gain.setName("Source " + juce::String(index + 1) + " gain");
            gain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 84, 24);
            gain.setTextValueSuffix(" dB");
            gainAttachment = std::make_unique<juce::SliderParameterAttachment>(*proc.apvts.getParameter(verbp::paramId(index, 4)), gain);
            for (auto* s : {&gain, &spread, &pattern}) {
                s->setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
                s->setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            }
            for (auto* s : {&spread, &pattern}) {
                s->setSliderStyle(juce::Slider::LinearHorizontal);
                s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 100, 20);
            }
            spread.setRange(0, 20, 0.01); spread.setTextValueSuffix(" m");
            pattern.setRange(0, 1, 0.01);
            spread.onValueChange = [this] { if (!syncing) proc.setSourceSpread(index, spread.getValue()); };
            pattern.onValueChange = [this] { if (!syncing) proc.setSourcePattern(index, pattern.getValue()); };
            input.setTooltip("First host input channel used by this source.");
            gain.setTooltip("Source gain, before room processing. Follows host automation.");
            pattern.setTooltip("Directivity: 0 omni, 0.5 cardioid, 1 figure-8.");
        }
        void sync(const verbp::SourceCfg& c) {
            syncing = true;
            if (!name.hasKeyboardFocus(true)) name.setText(c.name, false);
            format.setSelectedId(c.format + 1, juce::dontSendNotification);
            input.setSelectedId(c.firstIn + 1, juce::dontSendNotification);
            sum.setToggleState(c.sum, juce::dontSendNotification);
            sum.setEnabled(c.effectiveChannels() > 1);
            if (!spread.isMouseButtonDown()) spread.setValue(c.spread, juce::dontSendNotification);
            if (!pattern.isMouseButtonDown()) pattern.setValue(c.pattern, juce::dontSendNotification);
            spread.setEnabled(c.effectiveChannels() > 1 && !c.sum && !c.freePoints);
            first = c.firstIn; channels = c.effectiveChannels();
            float pk = 0;
            for (int k = 0; k < channels; ++k) pk = std::max(pk, proc.inputPeak(first + k));
            peak = std::max(pk, peak * 0.88f);
            syncing = false;
            repaint();
        }
        void resized() override {
            auto a = getLocalBounds().reduced(12);
            a.removeFromTop(22); name.setBounds(a.removeFromTop(26)); a.removeFromTop(10);
            format.setBounds(a.removeFromTop(26)); a.removeFromTop(6);
            input.setBounds(a.removeFromTop(26));
            a.removeFromTop(8); inputCaption = a.removeFromTop(18);
            a.removeFromTop(10); sum.setBounds(a.removeFromTop(24)); a.removeFromTop(8);
            auto bottom = a.removeFromBottom(126);
            bottom.removeFromTop(18); spread.setBounds(bottom.removeFromTop(42));
            bottom.removeFromTop(24); pattern.setBounds(bottom.removeFromTop(42));
            meter = a.removeFromRight(18).withTrimmedBottom(28).toFloat();
            a.removeFromRight(8); gain.setBounds(a);
        }
        void paint(juce::Graphics& g) override {
            g.setColour(skin::canvas.brighter(0.05f)); g.fillRoundedRectangle(getLocalBounds().reduced(3).toFloat(), 6);
            const float hue = std::fmod(164.f / 360.f + 0.137f * index, 1.f);
            auto colour = juce::Colour::fromHSV(hue, 0.6f, 0.92f, 1.f);
            g.setColour(colour); g.fillRect(12, 12, getWidth() - 24, 3);
            g.setFont(12.f); g.drawText("SOURCE " + juce::String(index + 1), 12, 17, getWidth() - 24, 18, juce::Justification::centredLeft);
            const bool missing = first + channels > proc.hostInputChannels();
            g.setColour(missing ? juce::Colours::orange : skin::textDim);
            g.drawText("Inputs " + juce::String(first + 1) + (channels > 1 ? "-" + juce::String(first + channels) : "") + (missing ? " !" : ""), inputCaption, juce::Justification::centredLeft);
            g.setColour(skin::gridLine); g.fillRect(meter);
            const float db = juce::Decibels::gainToDecibels(peak, -60.f);
            const float level = juce::jlimit(0.f, 1.f, (db + 60.f) / 60.f);
            g.setColour(peak >= 1.f ? juce::Colours::orangered : colour);
            g.fillRect(meter.withTrimmedTop(meter.getHeight() * (1.f - level)));
            g.setColour(skin::textDim);
            g.drawText("Spread", spread.getX(), spread.getY() - 18, spread.getWidth(), 18, juce::Justification::centredLeft);
            g.drawText("Directivity", pattern.getX(), pattern.getY() - 18, pattern.getWidth(), 18, juce::Justification::centredLeft);
        }
    private:
        template <typename Fn> void edit(Fn fn) {
            if (syncing) return;
            auto c = proc.getSources();
            if (index >= (int)c.size()) return;
            fn(c[(size_t)index]); proc.setSources(c);
        }
        void commitName() {
            const auto c = proc.getSources();
            if (index < (int)c.size() && c[(size_t)index].name != name.getText().toStdString())
                edit([this](auto& item) { item.name = name.getText().toStdString(); });
        }
        VerbProcessor& proc;
        int index, first = 0, channels = 1;
        bool syncing = false;
        float peak = 0;
        juce::Rectangle<float> meter;
        juce::Rectangle<int> inputCaption;
        juce::TextEditor name;
        juce::ComboBox format, input;
        juce::Slider gain, spread, pattern;
        juce::ToggleButton sum;
        std::unique_ptr<juce::SliderParameterAttachment> gainAttachment;
    };
public:
    explicit SourceMixerView(VerbProcessor& p) : proc(p) {
        addAndMakeVisible(viewport);
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(false, true);
    }
    void refresh() {
        const auto cfg = proc.getSources();
        if (strips.size() != cfg.size()) {
            strips.clear();
            for (int i = 0; i < (int)cfg.size(); ++i) {
                strips.push_back(std::make_unique<Strip>(proc, i));
                content.addAndMakeVisible(*strips.back());
            }
            resized();
        }
        for (size_t i = 0; i < cfg.size(); ++i) strips[i]->sync(cfg[i]);
    }
    void resized() override {
        viewport.setBounds(getLocalBounds().withTrimmedTop(44).withTrimmedBottom(26));
        const int height = std::max(410, viewport.getHeight() - viewport.getScrollBarThickness());
        content.setSize(std::max(viewport.getWidth(), (int)strips.size() * 180), height);
        for (size_t i = 0; i < strips.size(); ++i) strips[i]->setBounds((int)i * 180, 0, 180, height);
        viewport.setScrollBarsShown(height > viewport.getHeight(), true);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(skin::canvas); g.setColour(skin::text); g.setFont(16.f);
        g.drawText("Source mixer", 14, 8, getWidth() - 28, 26, juce::Justification::centredLeft);
        g.setColour(skin::textDim); g.setFont(11.f);
        g.drawText("Meters: input peaks before source gain. Scroll to see more sources.", 14, getHeight() - 22, getWidth() - 28, 18, juce::Justification::centredLeft);
    }
private:
    VerbProcessor& proc;
    juce::Component content;
    juce::Viewport viewport;
    std::vector<std::unique_ptr<Strip>> strips;
};
