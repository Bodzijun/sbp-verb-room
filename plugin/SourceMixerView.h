#pragma once
#include "PluginProcessor.h"
#include "PluginLookAndFeel.h"

// Message-thread controls; the meters read the same atomic input peaks as the map.
class SourceMixerView : public juce::Component {
    class Strip : public juce::Component {
    public:
        Strip(VerbProcessor& p, int i) : proc(p), index(i) {
            for (auto* c : std::initializer_list<juce::Component*>{&name, &format, &input, &gain, &spread, &pattern, &sum, &solo, &mute}) addAndMakeVisible(*c);
            name.setSelectAllWhenFocused(true);
            name.onReturnKey = [this] { commitName(); name.giveAwayKeyboardFocus(); };
            name.onFocusLost = [this] { commitName(); };
            for (int k = 0; k < (int)verbp::sourceFormats().size(); ++k) format.addItem(verbp::sourceFormats()[k].name, k + 1);
            for (int k = 1; k <= verbp::kMaxInputChannels; ++k) input.addItem("Input " + juce::String(k), k);
            format.onChange = [this] { edit([this](auto& c) { c.format = format.getSelectedId() - 1; }); };
            input.onChange = [this] { edit([this](auto& c) { c.firstIn = input.getSelectedId() - 1; }); };
            sum.setButtonText("Sum to mono");
            sum.onClick = [this] { edit([this](auto& c) { c.sum = sum.getToggleState(); }); };
            for (auto* button : {&solo, &mute}) button->setClickingTogglesState(true);
            solo.setName("Source " + juce::String(index + 1) + " solo");
            mute.setName("Source " + juce::String(index + 1) + " mute");
            solo.getProperties().set("style", "sourceSolo"); mute.getProperties().set("style", "sourceMute");
            solo.setTooltip("Solo this source. Several sources can be soloed together; existing reverb tails decay naturally.");
            mute.setTooltip("Mute new input from this source. Existing reverb tails decay naturally. Mute takes priority over Solo.");
            soloAttachment = std::make_unique<juce::ButtonParameterAttachment>(*proc.apvts.getParameter(verbp::sourceSwitchId(index, true)), solo);
            muteAttachment = std::make_unique<juce::ButtonParameterAttachment>(*proc.apvts.getParameter(verbp::sourceSwitchId(index, false)), mute);
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
            name.setTooltip(c.name);
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
            const bool compact = getWidth() < 120;
            auto a = getLocalBounds().reduced(compact ? 8 : 12);
            sum.setButtonText(compact ? "Sum" : "Sum to mono");
            for (int k = 1; k <= verbp::kMaxInputChannels; ++k) input.changeItemText(k, compact ? juce::String(k) : "Input " + juce::String(k));
            gain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, std::min(84, a.getWidth()), 24);
            for (auto* s : {&spread, &pattern}) s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, std::min(100, a.getWidth()), 20);
            a.removeFromTop(22); name.setBounds(a.removeFromTop(26)); a.removeFromTop(10);
            format.setBounds(a.removeFromTop(26)); a.removeFromTop(6);
            input.setBounds(a.removeFromTop(26));
            a.removeFromTop(8); inputCaption = a.removeFromTop(18);
            a.removeFromTop(10); sum.setBounds(a.removeFromTop(24)); a.removeFromTop(8);
            auto buttons = a.removeFromTop(26);
            solo.setBounds(buttons.removeFromLeft((buttons.getWidth() - 4) / 2)); buttons.removeFromLeft(4); mute.setBounds(buttons);
            a.removeFromTop(8);
            auto bottom = a.removeFromBottom(126);
            bottom.removeFromTop(18); spread.setBounds(bottom.removeFromTop(42));
            bottom.removeFromTop(24); pattern.setBounds(bottom.removeFromTop(42));
            meter = a.removeFromRight(compact ? 8 : 12).withTrimmedBottom(28).toFloat();
            a.removeFromRight(compact ? 4 : 6); gain.setBounds(a);
        }
        void paint(juce::Graphics& g) override {
            g.setColour(skin::canvas.brighter(0.05f)); g.fillRoundedRectangle(getLocalBounds().reduced(3).toFloat(), 6);
            const float hue = std::fmod(164.f / 360.f + 0.137f * index, 1.f);
            auto colour = juce::Colour::fromHSV(hue, 0.6f, 0.92f, 1.f);
            const int pad = getWidth() < 120 ? 8 : 12;
            g.setColour(colour); g.fillRect(pad, 12, getWidth() - 2 * pad, 3);
            g.setFont(12.f); g.drawText((getWidth() < 120 ? "SRC " : "SOURCE ") + juce::String(index + 1), pad, 17, getWidth() - 2 * pad, 18, juce::Justification::centredLeft);
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
            g.drawText(getWidth() < 120 ? "Pattern" : "Directivity", pattern.getX(), pattern.getY() - 18, pattern.getWidth(), 18, juce::Justification::centredLeft);
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
        juce::TextButton solo{"S"}, mute{"M"};
        std::unique_ptr<juce::SliderParameterAttachment> gainAttachment;
        std::unique_ptr<juce::ButtonParameterAttachment> soloAttachment, muteAttachment;
    };
public:
    explicit SourceMixerView(VerbProcessor& p) : proc(p) {
        addAndMakeVisible(viewport);
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(false, true);
        for (auto* button : {&compactButton, &normalButton}) {
            addAndMakeVisible(*button); button->setClickingTogglesState(true);
        }
        compactButton.setName("Compact source strips"); normalButton.setName("Normal source strips");
        compactButton.onClick = [this] { proc.setMixerCompactEnabled(true); refreshLayout(); };
        normalButton.onClick = [this] { proc.setMixerCompactEnabled(false); refreshLayout(); };
        refreshLayout();
    }
    void refresh() {
        const auto cfg = proc.getSources();
        if (compact != proc.mixerCompactEnabled()) refreshLayout();
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
        normalButton.setBounds(getWidth() - 82, 8, 70, 26);
        compactButton.setBounds(getWidth() - 162, 8, 76, 26);
        viewport.setBounds(getLocalBounds().withTrimmedTop(44).withTrimmedBottom(26));
        const int height = std::max(450, viewport.getHeight() - viewport.getScrollBarThickness());
        const int width = compact ? 92 : 160;
        content.setSize(std::max(viewport.getWidth(), (int)strips.size() * width), height);
        for (size_t i = 0; i < strips.size(); ++i) strips[i]->setBounds((int)i * width, 0, width, height);
        viewport.setScrollBarsShown(height > viewport.getHeight(), true);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(skin::canvas); g.setColour(skin::text); g.setFont(16.f);
        g.drawText("Source mixer", 14, 8, getWidth() - 188, 26, juce::Justification::centredLeft);
        g.setColour(skin::textDim); g.setFont(11.f);
        g.drawText("Meters: input peaks before source gain. Scroll to see more sources.", 14, getHeight() - 22, getWidth() - 28, 18, juce::Justification::centredLeft);
    }
private:
    void refreshLayout() {
        compact = proc.mixerCompactEnabled();
        compactButton.setToggleState(compact, juce::dontSendNotification);
        normalButton.setToggleState(!compact, juce::dontSendNotification);
        resized();
    }
    VerbProcessor& proc;
    bool compact = true;
    juce::TextButton compactButton{"Compact"}, normalButton{"Normal"};
    juce::Component content;
    juce::Viewport viewport;
    std::vector<std::unique_ptr<Strip>> strips;
};
