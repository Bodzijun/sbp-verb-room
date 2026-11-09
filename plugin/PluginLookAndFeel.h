// Visual identity of the editor: flat, tonal, hairline. No outlines on controls, no bevels, no drop shadows:
// depth comes only from surface lightness (window < panel < field < hover), emphasis from one accent.
//   accent  hsl(164, 61%, 42%)  teal-green: active state, value fills, the first source
//   rose    hsl(344, 61%, 58%)  complement of the accent: the listener
//   gold    hsl( 44, 85%, 58%)  triad: selection, handles
//   violet  hsl(284, 50%, 66%)  triad: ceiling and secondary information
// Control styles are chosen per component with the "style" property: "tab", "ghost" (transparent until hovered),
// "primary" (accent fill); anything else is a tonal field button.
#pragma once
#include <juce_audio_utils/juce_audio_utils.h>

namespace skin {
inline juce::Colour hsl(float h, float s, float l, float a = 1.f) { return juce::Colour::fromHSL(h / 360.f, s, l, a); }

const juce::Colour bg = hsl(210, 0.18f, 0.065f);       // window
const juce::Colour panel = hsl(210, 0.15f, 0.095f);    // the control panel
const juce::Colour field = hsl(210, 0.14f, 0.135f);    // buttons, inputs, menus
const juce::Colour fieldHi = hsl(210, 0.14f, 0.185f);  // hovered
const juce::Colour canvas = hsl(210, 0.20f, 0.085f);   // the map
const juce::Colour gridLine = hsl(210, 0.14f, 0.135f);
const juce::Colour line = hsl(210, 0.12f, 0.20f);      // hairlines and slider tracks
const juce::Colour text = hsl(200, 0.10f, 0.90f);
const juce::Colour textMid = hsl(200, 0.08f, 0.70f);
const juce::Colour textDim = hsl(205, 0.07f, 0.50f);
const juce::Colour accent = hsl(164, 0.61f, 0.42f);
const juce::Colour accentHi = hsl(164, 0.66f, 0.55f);
const juce::Colour rose = hsl(344, 0.61f, 0.58f);
const juce::Colour gold = hsl(44, 0.85f, 0.58f);
const juce::Colour violet = hsl(284, 0.50f, 0.66f);
const juce::Colour warn = hsl(8, 0.70f, 0.62f);
}  // namespace skin

// A tool button that draws its own line icon (tools sit in a vertical strip on the left of the map, like a 3D editor).
class IconButton : public juce::Button {
public:
    enum class Icon { Move, Wall, FloorZone, CeilZone, Box, Delete, Undo, Clear };
    explicit IconButton(Icon i) : juce::Button({}), icon(i) {}

    void paintButton(juce::Graphics& g, bool hover, bool down) override {
        auto r = getLocalBounds().toFloat().reduced(1.f);
        const bool on = getToggleState();
        g.setColour(on ? skin::accent.withAlpha(0.22f) : (hover || down ? skin::fieldHi : juce::Colours::transparentBlack));
        g.fillRoundedRectangle(r, 6.f);
        const juce::Colour c = !isEnabled() ? skin::textDim.withAlpha(0.5f) : on ? skin::accentHi : (hover ? skin::text : skin::textMid);
        g.setColour(c);
        const auto b = r.withSizeKeepingCentre(18.f, 18.f);
        const float x = b.getX(), y = b.getY();
        juce::Path p;
        const juce::PathStrokeType st(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        switch (icon) {
            case Icon::Move:  // arrow cursor
                p.startNewSubPath(x + 3.5f, y + 2.f);
                p.lineTo(x + 3.5f, y + 15.f);
                p.lineTo(x + 7.f, y + 11.8f);
                p.lineTo(x + 10.f, y + 17.f);
                p.lineTo(x + 12.f, y + 16.f);
                p.lineTo(x + 9.2f, y + 11.f);
                p.lineTo(x + 14.f, y + 10.6f);
                p.closeSubPath();
                g.strokePath(p, st);
                break;
            case Icon::Wall:  // a segment with its two ends
                g.drawLine(x + 3.5f, y + 14.5f, x + 14.5f, y + 3.5f, 1.5f);
                g.fillEllipse(x + 1.5f, y + 12.5f, 4.f, 4.f);
                g.fillEllipse(x + 12.5f, y + 1.5f, 4.f, 4.f);
                break;
            case Icon::FloorZone:  // a filled region on the ground
            case Icon::CeilZone: {
                p.startNewSubPath(x + 2.5f, y + 12.5f);
                p.lineTo(x + 6.f, y + 4.5f);
                p.lineTo(x + 14.5f, y + 3.5f);
                p.lineTo(x + 15.5f, y + 13.5f);
                p.closeSubPath();
                if (icon == Icon::FloorZone) {
                    g.setColour(c.withAlpha(0.28f));
                    g.fillPath(p);
                    g.setColour(c);
                    g.strokePath(p, st);
                } else {
                    const float dash[2] = {2.6f, 2.2f};
                    juce::Path d;
                    juce::PathStrokeType(1.5f).createDashedStroke(d, p, dash, 2);
                    g.fillPath(d);
                }
                break;
            }
            case Icon::Box: {  // marquee
                const float dash[2] = {2.4f, 2.f};
                p.addRectangle(x + 2.f, y + 3.f, 14.f, 12.f);
                juce::Path d;
                juce::PathStrokeType(1.4f).createDashedStroke(d, p, dash, 2);
                g.fillPath(d);
                break;
            }
            case Icon::Delete:  // bin
                p.startNewSubPath(x + 3.f, y + 5.f);
                p.lineTo(x + 15.f, y + 5.f);
                p.startNewSubPath(x + 7.f, y + 5.f);
                p.lineTo(x + 7.f, y + 3.f);
                p.lineTo(x + 11.f, y + 3.f);
                p.lineTo(x + 11.f, y + 5.f);
                p.startNewSubPath(x + 4.5f, y + 5.f);
                p.lineTo(x + 5.5f, y + 15.5f);
                p.lineTo(x + 12.5f, y + 15.5f);
                p.lineTo(x + 13.5f, y + 5.f);
                p.startNewSubPath(x + 9.f, y + 7.5f);
                p.lineTo(x + 9.f, y + 13.f);
                g.strokePath(p, st);
                break;
            case Icon::Clear:  // a blank page with a folded corner: start from nothing
                p.startNewSubPath(x + 4.f, y + 2.f);
                p.lineTo(x + 11.f, y + 2.f);
                p.lineTo(x + 15.f, y + 6.f);
                p.lineTo(x + 15.f, y + 16.f);
                p.lineTo(x + 4.f, y + 16.f);
                p.closeSubPath();
                p.startNewSubPath(x + 11.f, y + 2.f);
                p.lineTo(x + 11.f, y + 6.f);
                p.lineTo(x + 15.f, y + 6.f);
                g.strokePath(p, st);
                break;
            case Icon::Undo:
                p.startNewSubPath(x + 3.f, y + 4.f);
                p.lineTo(x + 3.f, y + 8.5f);
                p.lineTo(x + 7.5f, y + 8.5f);
                p.addCentredArc(x + 9.5f, y + 10.5f, 5.5f, 5.5f, 0.f, -2.1f, 2.0f, true);
                g.strokePath(p, st);
                break;
        }
    }

    Icon icon;
};

class VerbLookAndFeel : public juce::LookAndFeel_V4 {
public:
    VerbLookAndFeel() {
        using namespace skin;
        setColour(juce::ResizableWindow::backgroundColourId, bg);
        setColour(juce::Label::textColourId, textMid);
        setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
        setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxTextColourId, text);
        setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxHighlightColourId, accent.withAlpha(0.45f));
        setColour(juce::TextButton::buttonColourId, field);
        setColour(juce::TextButton::buttonOnColourId, accent);
        setColour(juce::TextButton::textColourOffId, text);
        setColour(juce::TextButton::textColourOnId, text);
        setColour(juce::ToggleButton::textColourId, textMid);
        setColour(juce::ComboBox::backgroundColourId, field);
        setColour(juce::ComboBox::textColourId, text);
        setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour(juce::ComboBox::arrowColourId, textDim);
        setColour(juce::PopupMenu::backgroundColourId, field);
        setColour(juce::PopupMenu::textColourId, text);
        setColour(juce::PopupMenu::headerTextColourId, textDim);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha(0.28f));
        setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour(juce::TextEditor::backgroundColourId, field);
        setColour(juce::TextEditor::textColourId, text);
        setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        setColour(juce::TextEditor::focusedOutlineColourId, accent);
        setColour(juce::TextEditor::highlightColourId, accent.withAlpha(0.4f));
        setColour(juce::CaretComponent::caretColourId, accentHi);
        setColour(juce::AlertWindow::backgroundColourId, panel);
        setColour(juce::AlertWindow::textColourId, text);
        setColour(juce::AlertWindow::outlineColourId, line);
        setColour(juce::TooltipWindow::backgroundColourId, fieldHi);
        setColour(juce::TooltipWindow::textColourId, text);
        setColour(juce::TooltipWindow::outlineColourId, juce::Colours::transparentBlack);
    }

    static juce::Font font(float size, bool bold = false) { return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain)); }
    juce::Font getLabelFont(juce::Label&) override { return font(12.5f); }
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return font(12.5f); }
    juce::Font getComboBoxFont(juce::ComboBox&) override { return font(12.5f); }
    juce::Font getPopupMenuFont() override { return font(13.f); }

    static juce::String styleOf(const juce::Component& c) { return c.getProperties().getWithDefault("style", "").toString(); }

    // ---- buttons ----
    void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool hover, bool down) override {
        using namespace skin;
        const auto st = styleOf(b);
        auto r = b.getLocalBounds().toFloat();
        const bool on = b.getToggleState();
        if (st == "tab") {  // text with an underline: the active tab carries the accent
            g.setColour(line);
            g.fillRect(r.getX(), r.getBottom() - 1.f, r.getWidth(), 1.f);
            if (on) {
                g.setColour(accent);
                g.fillRect(r.getX() + 4.f, r.getBottom() - 2.f, r.getWidth() - 8.f, 2.f);
            } else if (hover) {
                g.setColour(textDim.withAlpha(0.6f));
                g.fillRect(r.getX() + 4.f, r.getBottom() - 2.f, r.getWidth() - 8.f, 2.f);
            }
            return;
        }
        r = r.reduced(0.5f);
        juce::Colour fill;
        if (st == "primary") fill = (hover ? accentHi : accent).withMultipliedBrightness(down ? 0.85f : 1.f);
        else if (st == "ghost") fill = on ? accent.withAlpha(0.20f) : (hover ? fieldHi : juce::Colours::transparentBlack);
        else fill = on ? accent.withAlpha(0.24f) : (hover ? fieldHi : field);
        if (down && st != "primary") fill = fill.brighter(0.08f);
        g.setColour(fill);
        g.fillRoundedRectangle(r, 4.f);
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& b, bool hover, bool) override {
        using namespace skin;
        const auto st = styleOf(b);
        const bool on = b.getToggleState();
        juce::Colour c = text;
        if (st == "tab") c = on ? text : (hover ? textMid : textDim);
        else if (st == "primary") c = juce::Colours::white;
        else if (on) c = accentHi;
        else if (!b.isEnabled()) c = textDim;
        g.setColour(c);
        g.setFont(font(12.5f, st == "tab" && on));
        g.drawFittedText(b.getButtonText(), b.getLocalBounds().reduced(4, 0), juce::Justification::centred, 1);
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool hover, bool) override {
        using namespace skin;
        auto r = b.getLocalBounds().toFloat();
        const float tw = 22.f, th = 12.f, ty = r.getCentreY() - th / 2;
        juce::Rectangle<float> track(r.getX() + 1, ty, tw, th);
        const bool on = b.getToggleState();
        g.setColour(on ? accent : (hover ? line.brighter(0.15f) : line));
        g.fillRoundedRectangle(track, th / 2);
        g.setColour(on ? juce::Colours::white : textMid);
        const float kr = 4.2f, kx = on ? track.getRight() - th / 2 : track.getX() + th / 2;
        g.fillEllipse(kx - kr, track.getCentreY() - kr, 2 * kr, 2 * kr);
        g.setColour(b.isEnabled() ? (on ? text : textMid) : textDim);
        g.setFont(font(12.5f));
        const int tx = (int)(track.getRight() + 9);
        g.drawFittedText(b.getButtonText(), tx, 0, b.getWidth() - tx, b.getHeight(), juce::Justification::centredLeft, 2);
    }

    // ---- fields ----
    void drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override {
        using namespace skin;
        auto r = juce::Rectangle<float>(0.f, 0.f, (float)w, (float)h);
        g.setColour(box.isEnabled() ? (box.isMouseOver() ? fieldHi : field) : panel);
        g.fillRoundedRectangle(r, 4.f);
        juce::Path chevron;
        const float cx = (float)w - 13.f, cy = (float)h / 2.f;
        chevron.startNewSubPath(cx - 3.5f, cy - 1.5f);
        chevron.lineTo(cx, cy + 2.f);
        chevron.lineTo(cx + 3.5f, cy - 1.5f);
        g.setColour(box.isEnabled() ? textMid : textDim);
        g.strokePath(chevron, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override {
        label.setBounds(8, 0, box.getWidth() - 28, box.getHeight());
        label.setFont(getComboBoxFont(box));
    }
    void fillTextEditorBackground(juce::Graphics& g, int w, int h, juce::TextEditor& e) override {
        g.setColour(e.findColour(juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle(0.f, 0.f, (float)w, (float)h, 4.f);
    }
    void drawTextEditorOutline(juce::Graphics& g, int w, int h, juce::TextEditor& e) override {
        if (!e.hasKeyboardFocus(true) || e.isReadOnly()) return;
        g.setColour(skin::accent);
        g.drawRoundedRectangle(0.5f, 0.5f, (float)w - 1.f, (float)h - 1.f, 4.f, 1.f);
    }
    juce::Label* createSliderTextBox(juce::Slider& s) override {
        auto* l = LookAndFeel_V4::createSliderTextBox(s);
        l->setJustificationType(juce::Justification::centredRight);
        l->setColour(juce::Label::outlineWhenEditingColourId, skin::accent);
        l->setColour(juce::Label::backgroundWhenEditingColourId, skin::field);
        l->setFont(font(12.5f));
        return l;
    }

    // ---- popup menus ----
    void drawPopupMenuBackground(juce::Graphics& g, int w, int h) override {
        g.fillAll(skin::bg);
        g.setColour(skin::field);
        g.fillRoundedRectangle(0.f, 0.f, (float)w, (float)h, 6.f);
        g.setColour(skin::line);
        g.drawRoundedRectangle(0.5f, 0.5f, (float)w - 1.f, (float)h - 1.f, 6.f, 1.f);
    }

    // ---- sliders: a hairline track, an accent fill and a small round handle that grows on hover ----
    int getSliderThumbRadius(juce::Slider&) override { return 6; }
    void drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, const juce::Slider::SliderStyle style, juce::Slider& s) override {
        using namespace skin;
        if (style == juce::Slider::LinearVertical && (bool)s.getProperties().getWithDefault("sourceFader", false)) {
            const float cx = x + w * 0.5f;
            const float trackWidth = juce::jlimit(16.f, 28.f, w * 0.24f);
            const float top = (float)y + 6.f, bottom = (float)(y + h) - 6.f;
            const float p = juce::jlimit(top, bottom, pos);
            const bool active = s.isMouseOverOrDragging();
            g.setColour(line);
            g.fillRoundedRectangle(cx - trackWidth * 0.5f, top, trackWidth, bottom - top, 5.f);
            g.setColour(accent.withAlpha(active ? 0.8f : 0.55f));
            g.fillRoundedRectangle(cx - trackWidth * 0.5f, p, trackWidth, bottom - p, 5.f);
            g.setColour(active ? accentHi : accent);
            g.fillRoundedRectangle(cx - trackWidth * 0.5f - 5.f, p - 5.f, trackWidth + 10.f, 10.f, 3.f);
            return;
        }
        if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearBar) {
            LookAndFeel_V4::drawLinearSlider(g, x, y, w, h, pos, 0, 0, style, s);
            return;
        }
        const float cy = y + h / 2.f, left = (float)x + 6.f, right = (float)(x + w) - 6.f;
        const float p = juce::jlimit(left, right, pos);
        const bool en = s.isEnabled(), act = s.isMouseOverOrDragging();
        g.setColour(line.withAlpha(en ? 1.f : 0.5f));
        g.fillRoundedRectangle(left, cy - 1.f, right - left, 2.f, 1.f);
        const juce::Colour c = en ? accent : textDim;
        g.setColour(c);
        g.fillRoundedRectangle(left, cy - 1.f, p - left, 2.f, 1.f);
        const float r = act ? 5.5f : 4.f;
        g.setColour(en ? (act ? accentHi : c) : textDim);
        g.fillEllipse(p - r, cy - r, 2 * r, 2 * r);
    }
};
