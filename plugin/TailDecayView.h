#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "tail.hpp"
#include "PluginLookAndFeel.h"

// Immutable model snapshot; hiding a curve never changes the sound.
class TailDecayView : public juce::Component {
public:
    TailDecayView() {
        const char* names[] = {"125 Hz", "250 Hz", "500 Hz", "1 kHz", "2 kHz", "4 kHz", "8 kHz"};
        for (int b = 0; b < verb::NB; ++b) {
            bands[b].setButtonText(names[b]);
            bands[b].setToggleState(true, juce::dontSendNotification);
            bands[b].setColour(juce::ToggleButton::textColourId, colour(b));
            bands[b].setTooltip("Show or hide this band. The graph is modeled late-field energy, averaged over sources and receivers.");
            bands[b].onClick = [this] { repaint(); };
            addAndMakeVisible(bands[b]);
        }
    }

    void setData(std::shared_ptr<const verb::TailDiagnostics> d) {
        if (data == d) return;
        data = std::move(d);
        for (int b = 0; b < verb::NB; ++b) bands[b].setEnabled(data && data->active[b]);
        repaint();
    }

    void resized() override {
        const int columns = getWidth() >= 740 ? 7 : 4;
        const int cell = std::max(1, (getWidth() - 28) / columns);
        for (int b = 0; b < verb::NB; ++b)
            bands[b].setBounds(14 + (b % columns) * cell, 34 + (b / columns) * 40, cell - 4, 20);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(skin::canvas);
        g.setColour(skin::text);
        g.setFont(juce::Font(juce::FontOptions(16.f, juce::Font::bold)));
        g.drawText("Tail decay by octave band", 14, 8, getWidth() - 28, 22, juce::Justification::centredLeft);
        g.setFont(juce::Font(juce::FontOptions(11.f)));
        for (int b = 0; b < verb::NB; ++b) {
            g.setColour(colour(b).withAlpha(bands[b].getToggleState() ? 1.f : 0.35f));
            const auto text = data && data->active[b] && data->rt60[b] > 0 ? "RT60 " + juce::String(data->rt60[b], 2) + " s" : "RT60 --";
            juce::String detail = "Result RT60 / Physical RT60: " + (data ? juce::String(data->rt60[b],2) + " / " + juce::String(data->physicalRT60[b],2) + " s" : "--");
            if(data) {
                static const char* status[]={"applied","unity: no measurable decay","unity: below bin resolution","limited to 20 s","unity: fit rejected"};
                bool mixed=false;
                for(size_t s=1;s<data->sourceReshapeStatus.size();++s)mixed|=data->sourceReshapeStatus[s][b]!=data->sourceReshapeStatus[0][b]
                    ||std::abs(data->sourceEffectiveMultipliers[s][b]-data->sourceEffectiveMultipliers[0][b])>1e-9;
                detail += "; effective decay x" + juce::String(data->effectiveMultipliers[b],3) + " (" + (mixed?"per-source limits vary":status[std::clamp(data->reshapeStatus[b],0,4)]) + ")";
                for(size_t s=0;s<data->sourceReshapeStatus.size();++s)if(data->sourceReshapeStatus[s][b]!=0)
                    detail += "; source " + juce::String((int)s+1) + ": " + status[std::clamp(data->sourceReshapeStatus[s][b],0,4)];
            }
            bands[b].setTooltip(detail);
            g.drawText(text, bands[b].getBounds().translated(4, 19), juce::Justification::centredLeft);
        }
        const int legendHeight = getWidth() >= 740 ? 78 : 118;
        const auto plot = getLocalBounds().reduced(14).withTrimmedLeft(32).withTrimmedTop(legendHeight).withTrimmedBottom(38).toFloat();
        if (plot.getWidth() <= 0 || plot.getHeight() <= 0) return;
        for (int db = 0; db >= -80; db -= 20) {
            const float y = plot.getY() - db * plot.getHeight() / 80.f;
            g.setColour(skin::gridLine); g.drawHorizontalLine((int)y, plot.getX(), plot.getRight());
            g.setColour(skin::textDim); g.drawText(juce::String(db), (int)plot.getX() - 32, (int)y - 7, 27, 14, juce::Justification::centredRight);
        }
        for (int k = 0; k <= 4; ++k) {
            const float x = plot.getX() + k * plot.getWidth() / 4;
            g.setColour(skin::gridLine); g.drawVerticalLine((int)x, plot.getY(), plot.getBottom());
            if (data) {
                g.setColour(skin::textDim);
                g.drawText(juce::String(k * data->duration / 4, 1) + " s", (int)x - 24, (int)plot.getBottom() + 3, 48, 16, juce::Justification::centred);
            }
        }
        if (data) {
            g.saveState(); g.reduceClipRegion(plot.toNearestInt());
            for (int b = 0; b < verb::NB; ++b) {
                if (!data->active[b] || !bands[b].getToggleState()) continue;
                juce::Path traced, estimated, physical;
                for (size_t i = 0; i < verb::TailDiagnostics::points; ++i) {
                    const float f = (float)i / (verb::TailDiagnostics::points - 1);
                    const float x = plot.getX() + f * plot.getWidth();
                    const float y = plot.getY() - data->decayDb[b][i] * plot.getHeight() / 80.f;
                    const float py = plot.getY() - data->physicalDecayDb[b][i] * plot.getHeight() / 80.f;
                    if (i==0) physical.startNewSubPath(x,py); else physical.lineTo(x,py);
                    if (i == 0) { traced.startNewSubPath(x, y); estimated.startNewSubPath(x, y); }
                    if (f * data->duration <= data->reliableSeconds[b]) { traced.lineTo(x, y); estimated.startNewSubPath(x, y); }
                    else estimated.lineTo(x, y);
                }
                g.setColour(colour(b).withAlpha(.25f)); g.strokePath(physical,juce::PathStrokeType(1.f));
                g.setColour(colour(b)); g.strokePath(traced, juce::PathStrokeType(1.7f));
                juce::Path dashed; const float pattern[] = {4.f, 3.f};
                juce::PathStrokeType(1.5f).createDashedStroke(dashed, estimated, pattern, 2);
                g.fillPath(dashed);
            }
            g.restoreState();
        } else {
            g.setColour(skin::textDim);
            g.drawText("The decay graph appears when the room is ready.", plot.toNearestInt(), juce::Justification::centred);
        }
        g.setColour(skin::textDim);
        g.drawText("Energy (dB) | Solid: result; dashed: extrapolated; faint: physical. RT60: T20.",
                   14, getHeight() - 20, getWidth() - 28, 16, juce::Justification::centredLeft);
    }

private:
    static juce::Colour colour(int b) {
        const juce::uint32 colours[] = {0xff67d6b0, 0xff8cbdff, 0xffc7a5ff, 0xffffb872, 0xffff859c, 0xffd5db7b, 0xff6edce1};
        return juce::Colour(colours[b]);
    }
    std::array<juce::ToggleButton, verb::NB> bands;
    std::shared_ptr<const verb::TailDiagnostics> data;
};
