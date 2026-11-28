#pragma once
// V04e: minimal trigger-zone list for the classic (no WebView) editor. Read-only geometry; per zone an Enabled and a
// Visible checkbox and a Delete button. Edits go through VerbProcessor::setTriggerZones (same validation as the
// WebView commands). Portable JUCE only.
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>
#include "TriggerZones.h"

class ClassicZoneList : public juce::Component {
public:
    std::function<std::vector<verbp::TriggerZone>()> getZones;
    std::function<bool(const std::vector<verbp::TriggerZone>&)> setZones;

    ClassicZoneList() {
        title.setText("TRIGGER ZONES", juce::dontSendNotification);
        title.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        addAndMakeVisible(title);
        empty.setText("No zones (draw them in the WebView editor)", juce::dontSendNotification);
        empty.setFont(juce::FontOptions(12.0f));
        addAndMakeVisible(empty);
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport);
    }

    // Rebuilds the rows when the processor's zone list changed (cheap compare; called from the editor's timer).
    void refresh() {
        if (!getZones) return;
        auto z = getZones();
        if (built && z == shown) return;
        shown = std::move(z);
        built = true;
        rows.clear();
        for (size_t i = 0; i < shown.size(); ++i) {
            auto r = std::make_unique<Row>();
            const auto& zone = shown[i];
            const auto box = zone.outline();
            double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
            for (const auto& p : box) { x0 = std::min(x0, p.first); x1 = std::max(x1, p.first); y0 = std::min(y0, p.second); y1 = std::max(y1, p.second); }
            const juce::String name = zone.name.empty() ? juce::String("Zone ") + juce::String(zone.id) : juce::String(zone.name);
            r->name.setText(name, juce::dontSendNotification);
            r->name.setTooltip(juce::String(zone.polygon ? "Polygon, " : "Box, ") + juce::String(x0, 2) + ".." + juce::String(x1, 2) + " x " +
                               juce::String(y0, 2) + ".." + juce::String(y1, 2) + " m, z " + juce::String(zone.zMin, 2) + ".." + juce::String(zone.zMax, 2) + " m");
            r->name.setFont(juce::FontOptions(12.0f));
            r->enabled.setToggleState(zone.enabled, juce::dontSendNotification);
            r->visible.setToggleState(zone.visible, juce::dontSendNotification);
            const int id = zone.id;
            r->enabled.onClick = [this, id, b = &r->enabled] { edit(id, [on = b->getToggleState()](verbp::TriggerZone& t) { t.enabled = on; }); };
            r->visible.onClick = [this, id, b = &r->visible] { edit(id, [on = b->getToggleState()](verbp::TriggerZone& t) { t.visible = on; }); };
            r->del.onClick = [this, id] { removeZone(id); };
            for (auto* c : std::initializer_list<juce::Component*>{&r->name, &r->enabled, &r->visible, &r->del}) content.addAndMakeVisible(c);
            rows.push_back(std::move(r));
        }
        empty.setVisible(rows.empty());
        resized();
    }

    int numRows() const { return (int)rows.size(); }
    // Test hooks (classic editor test).
    void testToggle(int row, bool visibleBox) { if (row >= 0 && row < numRows()) { auto& b = visibleBox ? rows[(size_t)row]->visible : rows[(size_t)row]->enabled; b.setToggleState(!b.getToggleState(), juce::sendNotificationSync); } }
    void testDelete(int row) { if (row >= 0 && row < numRows()) { auto& b = rows[(size_t)row]->del; if (b.onClick) b.onClick(); } }

    void resized() override {
        auto a = getLocalBounds();
        title.setBounds(a.removeFromTop(18));
        empty.setBounds(a.removeFromTop(20));
        if (rows.empty()) { viewport.setBounds({}); return; }
        viewport.setBounds(a);
        const int w = juce::jmax(160, a.getWidth() - viewport.getScrollBarThickness());
        content.setSize(w, (int)rows.size() * 22);
        int y = 0;
        for (auto& r : rows) {
            juce::Rectangle<int> rr(0, y, w, 22);
            r->del.setBounds(rr.removeFromRight(54).reduced(1));
            r->visible.setBounds(rr.removeFromRight(62));
            r->enabled.setBounds(rr.removeFromRight(46));
            r->name.setBounds(rr);
            y += 22;
        }
    }

private:
    struct Row {
        juce::Label name;
        juce::ToggleButton enabled{"On"}, visible{"Show"};
        juce::TextButton del{"Delete"};
    };
    void edit(int id, const std::function<void(verbp::TriggerZone&)>& f) {
        if (!getZones || !setZones) return;
        auto z = getZones();
        for (auto& t : z) if (t.id == id) f(t);
        setZones(z);
        juce::Component::SafePointer<ClassicZoneList> self(this);
        juce::MessageManager::callAsync([self] { if (self) self->refresh(); });
    }
    void removeZone(int id) {
        if (!getZones || !setZones) return;
        auto z = getZones();
        z.erase(std::remove_if(z.begin(), z.end(), [id](const verbp::TriggerZone& t) { return t.id == id; }), z.end());
        setZones(z);
        // rows own the button being clicked: rebuild after the click handler returns
        juce::Component::SafePointer<ClassicZoneList> self(this);
        juce::MessageManager::callAsync([self] { if (self) self->refresh(); });
    }

    juce::Label title, empty;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<verbp::TriggerZone> shown;
    bool built = false;
};
