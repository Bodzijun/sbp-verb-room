#pragma once
#include <set>
#include "PluginLookAndFeel.h"
#include "SceneObjects.h"

class SceneOutliner : public juce::Component, private juce::ListBoxModel {
public:
    std::function<void(std::vector<verbp::SurfaceRef>, bool)> onSelect;
    std::function<void(std::vector<verbp::SurfaceRef>, int, bool)> onFlag; // 0 visibility, 1 locking
    std::function<void(juce::String)> onLink;
    std::function<void()> onUnlink;
    std::function<void(std::vector<verbp::SurfaceRef>, juce::String, bool)> onRename;

    SceneOutliner() {
        addAndMakeVisible(list); list.setModel(this); list.setRowHeight(26);
        list.setColour(juce::ListBox::backgroundColourId, skin::panel);
        list.setTooltip("H hides a surface on the map only; it remains acoustically active. L locks editing. Shift/Ctrl adds to selection; Alt selects an individual linked member.");
        for (auto* b : {&link, &unlink}) addAndMakeVisible(*b);
        link.onClick = [this] { askName("Link surfaces", "Construction", [this](juce::String name) { if (onLink) onLink(name); }); };
        unlink.onClick = [this] { if (onUnlink) onUnlink(); };
        link.setTooltip("Link selected walls and zones into a construction. Drag any member to move the whole group.");
        unlink.setTooltip("Unlink the selected surfaces without changing their geometry.");
    }
    void refresh(const verb::Scene& scene, const std::vector<verbp::SurfaceRef>& selected) {
        selection = selected;
        rows.clear();
        std::set<std::string> groups;
        const auto refs = verbp::surfaceRefs(scene);
        auto item = [&](verbp::SurfaceRef ref, bool child) {
            const auto* tag = verbp::surfaceTag(scene, ref);
            juce::String name = tag->name;
            if (name.isEmpty()) {
                const char* names[] = {"", "Wall", "Floor", "Ceiling", "Floor zone", "Ceiling zone"};
                name = names[ref.kind];
                if (ref.kind == 1 || ref.kind >= 4) name += " " + juce::String(ref.index + 1);
            }
            rows.push_back({name, {ref}, false, child, tag->hidden, tag->locked});
        };
        for (auto ref : refs) {
            const auto* tag = verbp::surfaceTag(scene, ref);
            if (tag->group.empty()) { item(ref, false); continue; }
            if (!groups.insert(tag->group).second) continue;
            Row row; row.name = tag->group; row.group = true; row.hidden = row.locked = true;
            for (auto member : refs) {
                const auto* mt = verbp::surfaceTag(scene, member);
                if (mt->group != tag->group) continue;
                row.refs.push_back(member); row.hidden &= mt->hidden; row.locked &= mt->locked;
            }
            rows.push_back(row);
            if (!collapsed.count(tag->group)) for (auto member : row.refs) item(member, true);
        }
        int movable = 0;
        for (auto ref : selection) movable += ref.kind == 1 || ref.kind >= 4;
        link.setEnabled(movable >= 2); unlink.setEnabled(!selection.empty());
        list.updateContent(); list.repaint();
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(skin::panel); g.setColour(skin::text); g.setFont(juce::Font(juce::FontOptions(12.f)));
        g.drawText("SCENE OUTLINER", 10, 4, getWidth() - 20, 20, juce::Justification::centredLeft);
        g.setColour(skin::textDim); g.setFont(juce::Font(juce::FontOptions(10.f)));
        g.drawText("H: hide map   L: lock   Double-click: rename", 8, getHeight() - 20, getWidth() - 16, 16, juce::Justification::centredLeft);
    }
    void resized() override {
        auto b = getLocalBounds().reduced(6); b.removeFromTop(23); b.removeFromBottom(18);
        auto buttons = b.removeFromBottom(28); link.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(2)); unlink.setBounds(buttons.reduced(2));
        list.setBounds(b.reduced(0, 3));
    }
private:
    struct Row {
        juce::String name;
        std::vector<verbp::SurfaceRef> refs;
        bool group = false, child = false, hidden = false, locked = false;
    };
    juce::ListBox list;
    juce::TextButton link{"Link"}, unlink{"Unlink"};
    std::vector<Row> rows;
    std::vector<verbp::SurfaceRef> selection;
    std::set<std::string> collapsed;
    int getNumRows() override { return (int)rows.size(); }
    void paintListBoxItem(int index, juce::Graphics& g, int width, int height, bool) override {
        if (index < 0 || index >= (int)rows.size()) return;
        const auto& r = rows[(size_t)index];
        bool selected = !r.refs.empty();
        for (auto ref : r.refs) selected &= std::find(selection.begin(), selection.end(), ref) != selection.end();
        if (selected) { g.setColour(skin::accent.withAlpha(0.18f)); g.fillRect(0, 0, width, height); }
        g.setColour(r.hidden ? skin::textDim : skin::text); g.setFont(juce::Font(juce::FontOptions(12.f)));
        const int left = r.child ? 25 : 7;
        if (r.group) g.drawText(collapsed.count(r.name.toStdString()) ? "+" : "-", 3, 0, 14, height, juce::Justification::centred);
        g.drawText(r.name + (r.group ? " (" + juce::String((int)r.refs.size()) + ")" : ""), r.group ? 20 : left, 0,
                   width - (r.group ? 20 : left) - 48, height, juce::Justification::centredLeft, true);
        for (int k = 0; k < 2; ++k) {
            const bool active = k == 0 ? r.hidden : r.locked;
            g.setColour(active ? skin::gold : skin::textDim.withAlpha(0.5f));
            g.drawText(k == 0 ? "H" : "L", width - 46 + k * 23, 0, 23, height, juce::Justification::centred);
        }
    }
    void listBoxItemClicked(int index, const juce::MouseEvent& e) override {
        if (index < 0 || index >= (int)rows.size()) return;
        const auto row = rows[(size_t)index];
        const int width = list.getVisibleRowWidth();
        if (e.position.x >= width - 46) {
            const int flag = e.position.x >= width - 23 ? 1 : 0;
            if (onFlag) onFlag(row.refs, flag, !(flag ? row.locked : row.hidden));
            return;
        }
        if (row.group && e.position.x < 19) {
            const auto name = row.name.toStdString();
            if (collapsed.count(name)) collapsed.erase(name); else collapsed.insert(name);
            if (onSelect) onSelect(selection, false);  // refreshes the flattened tree
            return;
        }
        auto refs = e.mods.isShiftDown() || e.mods.isCommandDown() ? selection : std::vector<verbp::SurfaceRef>();
        for (auto ref : row.refs) {
            auto found = std::find(refs.begin(), refs.end(), ref);
            if (found == refs.end()) refs.push_back(ref);
            else if (e.mods.isShiftDown() || e.mods.isCommandDown()) refs.erase(found);
        }
        if (onSelect) onSelect(refs, !e.mods.isAltDown());
    }
    void listBoxItemDoubleClicked(int index, const juce::MouseEvent& event) override {
        if (index < 0 || index >= (int)rows.size()) return;
        const auto row = rows[(size_t)index];
        if (event.position.x >= list.getVisibleRowWidth() - 46 || (row.group && event.position.x < 19)) return;
        askName(row.group ? "Rename construction" : "Rename surface", row.name,
            [this, row](juce::String name) { if (onRename) onRename(row.refs, name, row.group); });
    }
    void askName(const juce::String& title, const juce::String& value, std::function<void(juce::String)> done) {
        auto* w = new juce::AlertWindow(title, "Name:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", value); w->getTextEditor("name")->setInputRestrictions(128);
        w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey)); w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<SceneOutliner> self(this);
        w->enterModalState(true, juce::ModalCallbackFunction::create([self, w, done](int result) {
            if (self == nullptr || result != 1) return;
            const auto name = w->getTextEditorContents("name").trim(); if (name.isNotEmpty()) done(name);
        }), true);
    }
};
