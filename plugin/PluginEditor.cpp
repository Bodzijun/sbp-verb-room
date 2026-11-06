#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

namespace {
const juce::Colour kBg = skin::bg, kGrid = skin::canvas, kWall = skin::text, kSrc = skin::accent, kLis = skin::rose, kSel = skin::gold;
constexpr int kPanelW = 350;
constexpr int kMaxWalls = 128;

double distPointSeg(juce::Point<float> p, juce::Point<float> a, juce::Point<float> b) {
    const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.f;
    t = std::min(1.f, std::max(0.f, t));
    return p.getDistanceFrom({a.x + t * dx, a.y + t * dy});
}

double meanOf(const std::array<double, verb::NB>& v) {
    double s = 0;
    for (double x : v) s += x;
    return s / verb::NB;
}

// Material index owned by exactly this surface (a shared material is cloned first, so one segment = one surface).
int ownMaterialFor(verb::Scene& s, int kind /*1 wall, 2 floor, 3 ceiling, 4 floor zone, 5 ceiling zone*/, int wall) {
    int* idx = nullptr;
    if (kind == 1 && wall >= 0 && wall < (int)s.walls.size()) idx = &s.walls[(size_t)wall].mat;
    else if (kind == 2) idx = &s.floorMat;
    else if (kind == 3) idx = &s.ceilMat;
    else if (kind == 4 && wall >= 0 && wall < (int)s.floorZones.size()) idx = &s.floorZones[(size_t)wall].mat;
    else if (kind == 5 && wall >= 0 && wall < (int)s.ceilZones.size()) idx = &s.ceilZones[(size_t)wall].mat;
    if (!idx) return -1;
    if (*idx < 0) {  // open ceiling being edited: give it a default surface
        verb::Material m = verb::surfacePresets()[2];  // Plaster
        s.mats.push_back(m);
        *idx = (int)s.mats.size() - 1;
        return *idx;
    }
    int users = 0;
    for (auto& w : s.walls) users += w.mat == *idx;
    users += s.floorMat == *idx;
    users += s.ceilMat == *idx;
    for (auto& z : s.floorZones) users += z.mat == *idx;
    for (auto& z : s.ceilZones) users += z.mat == *idx;
    if (users > 1) {
        verb::Material m = s.mats[(size_t)*idx];
        s.mats.push_back(m);
        *idx = (int)s.mats.size() - 1;
    }
    return *idx;
}

double tlFromTau(double tau) { return tau <= 1e-6 ? 60.0 : std::min(60.0, verb::tauToTl(tau)); }

// One colour per source (listener stays red).
juce::Colour srcColour(int i) {  // starts at the accent (hue 164); hues near the listener's rose are skipped
    float h = std::fmod(164.f / 360.f + 0.137f * (float)i, 1.f);
    if (h > 0.90f && h < 0.995f) h += 0.1f;
    return juce::Colour::fromHSV(std::fmod(h, 1.f), 0.60f, 0.92f, 1.f);
}
}  // namespace

VerbEditor::VerbEditor(VerbProcessor& p) : AudioProcessorEditor(&p), proc(p) {
    setLookAndFeel(&laf);
    setSize(1160, 760);
    setResizable(true, true);
    setResizeLimits(900, 600, 2600, 1800);
    setWantsKeyboardFocus(true);

    // ---- header ----
    builtins = verbp::builtinRooms();
    addAndMakeVisible(roomBox);
    addAndMakeVisible(saveRoomBtn);
    addAndMakeVisible(exportBtn);
    roomBox.setTextWhenNothingSelected("Room library ...");
    roomBox.onChange = [this] { onRoomChosen(); };
    saveRoomBtn.setTooltip("Save the room (walls, surfaces, zones, air, clutter, roughness) with the current source and listener positions to your library");
    exportBtn.setTooltip("Render the impulse responses of the current scene (one WAV per source point, one channel per output) to a folder");
    saveRoomBtn.onClick = [this] { saveRoomDialog(); };
    exportBtn.onClick = [this] { exportDialog(); };
    refreshRoomBox();
    for (auto* b : {&tabObjects, &tabSurfaces, &tabMix, &tabSpace}) b->getProperties().set("style", "tab");
    saveRoomBtn.getProperties().set("style", "ghost");
    exportBtn.getProperties().set("style", "primary");
    addAndMakeVisible(statusLabel);
    statusLabel.setJustificationType(juce::Justification::topLeft);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    for (auto* b : {&tabObjects, &tabSurfaces, &tabMix, &tabSpace}) {
        addAndMakeVisible(*b);
        b->setClickingTogglesState(true);
        b->setRadioGroupId(201);
    }
    tabObjects.onClick = [this] { showTab(0); };
    tabSurfaces.onClick = [this] { showTab(1); };
    tabMix.onClick = [this] { showTab(2); };
    tabSpace.onClick = [this] { showTab(3); };

    // ---- canvas toolbar ----
    // tools: a vertical strip of icon buttons on the left of the map; views: a slim switcher above the map
    for (auto* b : {&toolMove, &toolBox, &toolAdd, &toolFloorZone, &toolCeilZone, &toolDelete, &toolUndo, &toolClear}) addAndMakeVisible(*b);
    toolClear.setTooltip("Clear everything: remove all walls, zones and clutter and start a room of your own (undo brings it back)");
    toolClear.onClick = [this] {
        juce::Component::SafePointer<VerbEditor> self(this);
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                         .withIconType(juce::MessageBoxIconType::NoIcon)
                                         .withTitle("Clear everything")
                                         .withMessage("Remove all walls, floor and ceiling zones and clutter? Sources, listener and air stay. You can undo this.")
                                         .withButton("Clear all")
                                         .withButton("Cancel"),
                                     [self](int result) {
                                         if (result == 1 && self != nullptr) self->clearRoom();
                                     });
    };
    for (auto* b : {&viewTop, &viewFront, &viewSide, &viewIso}) { addAndMakeVisible(*b); b->getProperties().set("style", "ghost"); b->setClickingTogglesState(true); b->setRadioGroupId(102); }
    addAndMakeVisible(autoViewBtn);
    autoViewBtn.setToggleState(true, juce::dontSendNotification);
    autoViewBtn.onClick = [this] { setAutoView(autoViewBtn.getToggleState()); };
    for (auto* b : {&toolMove, &toolBox, &toolAdd, &toolFloorZone, &toolCeilZone}) { b->setClickingTogglesState(true); b->setRadioGroupId(101); }
    viewTop.setToggleState(true, juce::dontSendNotification);
    viewTop.setTooltip("Plan view from above (x, y)");
    viewFront.setTooltip("Elevation from the front (x, height): wall heights, floor and ceiling, source and listener heights");
    viewSide.setTooltip("Elevation from the side (y, height)");
    viewIso.setTooltip("3D view: drag to orbit, wheel to zoom, double-click to reset");
    viewTop.onClick = [this] { setPlane(Plane::Top); };
    viewFront.onClick = [this] { setPlane(Plane::Front); };
    viewSide.onClick = [this] { setPlane(Plane::Side); };
    viewIso.onClick = [this] { setPlane(Plane::Iso); };
    toolMove.setToggleState(true, juce::dontSendNotification);
    toolMove.setTooltip("Select and move: click a wall, zone or object, drag corners; Shift+click adds walls to the selection");
    toolBox.setTooltip("Box select: drag a rectangle to select several walls at once (Shift adds), then edit their surfaces together");
    toolAdd.setTooltip("Draw walls: drag out a segment (snaps to corners and a 10 cm grid, Alt = free)");
    toolFloorZone.setTooltip("Floor zone: click the corners, double-click or Enter closes it, Esc cancels");
    toolCeilZone.setTooltip("Ceiling zone (its own material, or an opening): click the corners, double-click or Enter closes it");
    toolDelete.setTooltip("Delete the selected walls or zone (Del)");
    toolUndo.setTooltip("Undo (Ctrl+Z)");
    toolMove.onClick = [this] { tool = Tool::Move; zonePts.clear(); };
    toolBox.onClick = [this] { tool = Tool::BoxSelect; zonePts.clear(); if (!inPlan()) setPlane(Plane::Top); };
    toolAdd.onClick = [this] { tool = Tool::AddWall; zonePts.clear(); if (!inPlan()) setPlane(Plane::Top); };
    toolFloorZone.onClick = [this] { tool = Tool::AddFloorZone; zoneLayerCeiling = false; zonePts.clear(); if (!inPlan()) setPlane(Plane::Top); };
    toolCeilZone.onClick = [this] { tool = Tool::AddCeilZone; zoneLayerCeiling = true; zonePts.clear(); if (!inPlan()) setPlane(Plane::Top); };
    toolDelete.onClick = [this] { deleteSelectedWall(); };
    toolUndo.onClick = [this] { undo(); };

    // ---- objects tab ----
    addAndMakeVisible(selLabel);
    selLabel.setFont(juce::FontOptions(16.f, juce::Font::bold));
    static const char* names[4] = {"X (m)", "Y (m)", "Z (m)", "Azimuth"};
    for (int k = 0; k < 4; ++k) {
        addAndMakeVisible(sliders[(size_t)k]);
        addAndMakeVisible(sliderLabels[(size_t)k]);
        sliderLabels[(size_t)k].setText(names[k], juce::dontSendNotification);
        sliders[(size_t)k].setSliderStyle(juce::Slider::LinearHorizontal);
        sliders[(size_t)k].setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
        sliders[(size_t)k].setNumDecimalPlacesToDisplay(2);
        // plain sliders (not attachments): the range follows the room, the parameter keeps its fixed range
        sliders[(size_t)k].onValueChange = [this, k] {
            if (!updatingUI) setParam(verbp::paramId(selected, k), sliders[(size_t)k].getValue());
        };
        sliders[(size_t)k].onDragStart = [this, k] {
            if (auto* pr = proc.apvts.getParameter(verbp::paramId(selected, k))) pr->beginChangeGesture();
        };
        sliders[(size_t)k].onDragEnd = [this, k] {
            if (auto* pr = proc.apvts.getParameter(verbp::paramId(selected, k))) pr->endChangeGesture();
        };
    }

    // ---- sources (objects tab) ----
    for (auto* c : std::initializer_list<juce::Component*>{&sourceBox, &addSourceBtn, &removeSourceBtn, &nameEdit, &nameLabel, &formatLabel, &firstInLabel,
                                                           &chLabel, &infoLabel, &srcGainLabel, &formatBox, &firstInBox, &chBox, &sumToggle, &srcGainSlider})
        addAndMakeVisible(*c);
    setupRow(spreadRow, "Spread radius", 0, 10, 0.05, " m");
    setupRow(patternRow, "Directivity", 0, 1, 0.01, "");  // 0 omni, 0.5 cardioid, 1 figure-8
    patternRow.label.setTooltip("0 = omni, 0.5 = cardioid, 1 = figure-8; the source faces its azimuth");
    patternRow.slider.onValueChange = [this] {
        if (updatingUI || !uiIsSource) return;
        proc.setSourcePattern(selected, patternRow.slider.getValue());
        if (selected < (int)cfgCache.size()) cfgCache[(size_t)selected].pattern = patternRow.slider.getValue();
    };
    nameLabel.setText("Name", juce::dontSendNotification);
    formatLabel.setText("Format", juce::dontSendNotification);
    firstInLabel.setText("First input channel", juce::dontSendNotification);
    chLabel.setText("Channels", juce::dontSendNotification);
    srcGainLabel.setText("Source gain (dB)", juce::dontSendNotification);
    infoLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    infoLabel.setJustificationType(juce::Justification::topLeft);
    for (size_t i = 0; i < verbp::sourceFormats().size(); ++i) formatBox.addItem(verbp::sourceFormats()[i].name, (int)i + 1);
    for (int ch = 1; ch <= verbp::kMaxInputChannels; ++ch) firstInBox.addItem(juce::String(ch), ch);
    for (int n = 2; n <= 16; ++n) chBox.addItem(juce::String(n), n);
    srcGainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    srcGainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    sourceBox.onChange = [this] {
        if (updatingUI) return;
        const int id = sourceBox.getSelectedId();
        selectObject(id == 1000 ? verbp::kListener : id - 1);
    };
    addSourceBtn.onClick = [this] {
        const int i = proc.addSource();
        if (i >= 0) { cfgCache = proc.getSources(); selectObject(i); }
    };
    removeSourceBtn.onClick = [this] {
        if (selected == verbp::kListener) return;
        const int idx = selected;
        proc.removeSource(idx);
        cfgCache = proc.getSources();
        selectObject(std::max(0, std::min(idx, (int)cfgCache.size() - 1)));
    };
    nameEdit.onReturnKey = nameEdit.onFocusLost = [this] {
        const std::string t = nameEdit.getText().toStdString();
        if (!uiIsSource || selected >= (int)cfgCache.size() || cfgCache[(size_t)selected].name == t) return;
        editSource([t](verbp::SourceCfg& c) { c.name = t; });
    };
    formatBox.onChange = [this] {
        if (updatingUI) return;
        const int f = formatBox.getSelectedId() - 1;
        editSource([f](verbp::SourceCfg& c) {
            c.format = f;
            const int fixed = verbp::sourceFormats()[(size_t)f].channels;
            c.channels = fixed > 0 ? fixed : std::max(2, c.channels);
            if (c.effectiveChannels() == 1) { c.sum = false; c.freePoints = false; }
            if (c.freePoints) verbp::freezeLayout(c);
        });
    };
    chBox.onChange = [this] {
        if (updatingUI) return;
        const int n = chBox.getSelectedId();
        editSource([n](verbp::SourceCfg& c) { c.channels = n; });
    };
    firstInBox.onChange = [this] {
        if (updatingUI) return;
        const int ch = firstInBox.getSelectedId() - 1;
        editSource([ch](verbp::SourceCfg& c) { c.firstIn = ch; });
    };
    sumToggle.onClick = [this] {
        const bool on = sumToggle.getToggleState();
        editSource([on](verbp::SourceCfg& c) { c.sum = on; });
    };
    spreadRow.slider.onValueChange = [this] {
        if (updatingUI || !uiIsSource) return;
        proc.setSourceSpread(selected, spreadRow.slider.getValue());
        if (selected < (int)cfgCache.size()) cfgCache[(size_t)selected].spread = spreadRow.slider.getValue();
    };

    // ---- surfaces tab ----
    addAndMakeVisible(surfTitle);
    surfTitle.setFont(juce::FontOptions(15.f, juce::Font::bold));
    surfTitle.setColour(juce::Label::textColourId, skin::text);
    addAndMakeVisible(selFloor);
    addAndMakeVisible(selCeiling);
    selFloor.onClick = [this] { selectSurface(SelKind::Floor, -1); };
    selCeiling.onClick = [this] { selectSurface(SelKind::Ceiling, -1); };
    addAndMakeVisible(presetBox);
    addAndMakeVisible(presetLabel);
    presetLabel.setText("Preset (copies into this surface)", juce::dontSendNotification);
    {   // presets grouped by class
        const auto& pr = verb::surfacePresets();
        const auto& cl = verb::surfaceClasses();
        for (int c = 0; c < verb::SC_Custom; ++c) {
            presetBox.addSectionHeading(cl[(size_t)c].name);
            for (size_t i = 0; i < pr.size(); ++i)
                if (pr[i].cls == c) presetBox.addItem(juce::String(pr[i].name).replaceCharacter('_', ' '), (int)i + 1);
        }
        presetBox.addSeparator();
        presetBox.addItem("Custom", 1000);
    }
    presetBox.onChange = [this] {
        if (updatingUI) return;
        const int id = presetBox.getSelectedId();
        const auto& pr = verb::surfacePresets();
        if (id < 1 || id > (int)pr.size()) return;
        pushUndo();
        const verb::Material preset = pr[(size_t)id - 1];
        editSurface([preset](verb::Material& m) { m = preset; });
    };
    addAndMakeVisible(openSky);
    openSky.onClick = [this] {
        if (updatingUI) return;
        pushUndo();
        const bool open = openSky.getToggleState();
        const bool zone = selKind == SelKind::CeilZone;
        const int zi = selWall;
        proc.editScene([open, zone, zi](verb::Scene& s) {
            int* mat = &s.ceilMat;
            if (zone) {
                if (zi < 0 || zi >= (int)s.ceilZones.size()) return;
                mat = &s.ceilZones[(size_t)zi].mat;
            }
            if (open) *mat = -1;
            else if (*mat < 0) { s.mats.push_back(verb::surfacePresets()[2]); *mat = (int)s.mats.size() - 1; }
        });
    };
    static const char* bandNames[7] = {"125 Hz", "250 Hz", "500 Hz", "1 kHz", "2 kHz", "4 kHz", "8 kHz"};
    for (int b = 0; b < 7; ++b) {
        setupRow(absRows[(size_t)b], juce::String("Absorb ") + bandNames[b], 0, 100, 1, " %");
        absRows[(size_t)b].slider.onValueChange = [this, b] {
            if (updatingUI) return;
            const double v = absRows[(size_t)b].slider.getValue() / 100.0;
            editSurface([b, v](verb::Material& m) { m.name = "Custom"; m.alpha[(size_t)b] = v; });
        };
    }
    setupRow(scRows[0], "Scatter LF", 0, 90, 1, " %");
    setupRow(scRows[1], "Scatter HF", 0, 90, 1, " %");
    scRows[0].slider.onValueChange = [this] {
        if (updatingUI) return;
        const double v = scRows[0].slider.getValue() / 100.0;
        editSurface([v](verb::Material& m) { m.name = "Custom"; if (m.scatterHf < 0) m.scatterHf = m.scatter; m.scatter = v; });
    };
    scRows[1].slider.onValueChange = [this] {
        if (updatingUI) return;
        const double v = scRows[1].slider.getValue() / 100.0;
        editSurface([v](verb::Material& m) { m.name = "Custom"; m.scatterHf = v; });
    };
    // one master control for how much sound gets through this surface (0 % = opaque, 100 % = open); the three
    // band rows below stay available for a frequency-dependent loss
    setupRow(passRow, "Sound passes", 0, 100, 1, " %");
    passRow.slider.onValueChange = [this] {
        if (updatingUI) return;
        const double p = passRow.slider.getValue() / 100.0;
        editSurface([p](verb::Material& m) {
            m.name = "Custom";
            for (int b = 0; b < 7; ++b) m.tau[(size_t)b] = p <= 0.005 ? 0.0 : verb::tlToTau((1.0 - p) * 60.0);
        });
    };
    static const char* tlNames[3] = {"Pass-through LF", "Pass-through MF", "Pass-through HF"};
    static const int tlFirst[3] = {0, 2, 4}, tlLast[3] = {1, 3, 6};
    for (int i = 0; i < 3; ++i) {
        setupRow(tlRows[(size_t)i], tlNames[i], 0, 60, 0.5, "");
        tlRows[(size_t)i].slider.textFromValueFunction = [](double v) {  // 0 dB = open, maximum = no sound gets through
            return v >= 59.9 ? juce::String("opaque") : juce::String(v, 1) + " dB TL";
        };
        tlRows[(size_t)i].slider.valueFromTextFunction = [](const juce::String& t) { return t.containsIgnoreCase("opaque") ? 60.0 : t.getDoubleValue(); };
        tlRows[(size_t)i].slider.onValueChange = [this, i] {
            if (updatingUI) return;
            const double v = tlRows[(size_t)i].slider.getValue();
            editSurface([i, v](verb::Material& m) {
                m.name = "Custom";
                for (int b = tlFirst[i]; b <= tlLast[i]; ++b) m.tau[(size_t)b] = v >= 59.9 ? 0.0 : verb::tlToTau(v);  // 60 dB = opaque
            });
        };
    }
    setupRow(heightRow, "Ceiling height", 1, 30, 0.05, " m");
    heightRow.slider.onValueChange = [this] {
        if (updatingUI) return;
        const double v = heightRow.slider.getValue();
        proc.editScene([v](verb::Scene& s) { s.height = v; });
    };
    // one undo step per slider gesture
    for (auto& r : absRows) r.slider.onDragStart = [this] { pushUndo(); };
    for (auto& r : scRows) r.slider.onDragStart = [this] { pushUndo(); };
    for (auto& r : tlRows) r.slider.onDragStart = [this] { pushUndo(); };
    passRow.slider.onDragStart = [this] { pushUndo(); };
    heightRow.slider.onDragStart = [this] { pushUndo(); };

    // ---- free channel points ----
    addAndMakeVisible(freeToggle);
    freeToggle.onClick = [this] {
        const bool on = freeToggle.getToggleState();
        editSource([on](verbp::SourceCfg& c) {
            c.freePoints = on;
            if (on) verbp::freezeLayout(c);  // start from the current circle: nothing jumps
        });
    };

    // ---- surfaces: class and scale ----
    for (auto* c : std::initializer_list<juce::Component*>{&classBox, &classLabel, &scaleRoomBtn, &scaleObjects}) addAndMakeVisible(*c);
    classLabel.setText("Class (colour on the map)", juce::dontSendNotification);
    for (int c = 0; c <= verb::SC_Custom; ++c) classBox.addItem(verb::surfaceClasses()[(size_t)c].name, c + 1);
    classBox.onChange = [this] {
        if (updatingUI) return;
        const int cls = classBox.getSelectedId() - 1;
        pushUndo();
        editSurface([cls](verb::Material& m) { m.cls = cls; });
    };
    setupRow(wallBotRow, "Wall bottom", 0, 30, 0.05, " m");
    setupRow(wallTopRow, "Wall top", 0.1, 30, 0.05, "");
    wallTopRow.slider.textFromValueFunction = [this](double v) {  // the top at the ceiling means a full-height wall
        return (v >= wallTopRow.slider.getMaximum() - 1e-6 ? juce::String("full height") : juce::String(v, 2) + " m");
    };
    wallTopRow.slider.valueFromTextFunction = [this](const juce::String& t) { return t.containsIgnoreCase("full") ? wallTopRow.slider.getMaximum() : t.getDoubleValue(); };
    wallBotRow.slider.onValueChange = [this] {
        if (updatingUI || selKind != SelKind::Wall) return;
        std::vector<int> wis = selWalls;
        if (wis.empty()) wis.push_back(selWall);
        const double v = wallBotRow.slider.getValue();
        proc.editScene([wis, v](verb::Scene& s) {
            for (int wi : wis) {
                if (wi < 0 || wi >= (int)s.walls.size()) continue;
                auto& w = s.walls[(size_t)wi];
                w.z0 = std::max(0.0, std::min(v, std::min(w.z1, s.height) - 0.05));
            }
        });
    };
    wallTopRow.slider.onValueChange = [this] {
        if (updatingUI || selKind != SelKind::Wall) return;
        std::vector<int> wis = selWalls;
        if (wis.empty()) wis.push_back(selWall);
        const double v = wallTopRow.slider.getValue(), vmax = wallTopRow.slider.getMaximum();
        proc.editScene([wis, v, vmax](verb::Scene& s) {
            for (int wi : wis) {
                if (wi < 0 || wi >= (int)s.walls.size()) continue;
                auto& w = s.walls[(size_t)wi];
                w.z1 = v >= vmax - 1e-6 ? 1e9 : std::max(v, w.z0 + 0.05);
            }
        });
    };
    setupRow(zoneZRow, "Plate height", 0, 30, 0.05, "");
    zoneZRow.slider.textFromValueFunction = [](double v) { return v <= 1e-6 ? juce::String("none (patch)") : juce::String(v, 2) + " m"; };
    zoneZRow.slider.valueFromTextFunction = [](const juce::String& t) { return t.containsIgnoreCase("none") ? 0.0 : t.getDoubleValue(); };
    zoneZRow.slider.onValueChange = [this] {
        if (updatingUI || (selKind != SelKind::FloorZone && selKind != SelKind::CeilZone)) return;
        const bool ceiling = selKind == SelKind::CeilZone;
        const int zi = selWall;
        const double v = zoneZRow.slider.getValue();
        proc.editScene([ceiling, zi, v](verb::Scene& s) {
            auto& list = ceiling ? s.ceilZones : s.floorZones;
            if (zi < 0 || zi >= (int)list.size()) return;
            list[(size_t)zi].z = v <= 1e-6 ? -1.0 : v;
        });
    };
    zoneZRow.slider.onDragStart = [this] { pushUndo(); };
    wallBotRow.slider.onDragStart = [this] { pushUndo(); };
    wallTopRow.slider.onDragStart = [this] { pushUndo(); };
    setupRow(scaleRow, "Scale factor", 0.25, 4.0, 0.01, " x");
    scaleRow.slider.setValue(1.0, juce::dontSendNotification);
    scaleRow.slider.setSkewFactorFromMidPoint(1.0);
    scaleObjects.setToggleState(true, juce::dontSendNotification);
    scaleRoomBtn.onClick = [this] {
        pushUndo();
        proc.scaleRoom(scaleRow.slider.getValue(), scaleObjects.getToggleState());
        updatingUI = true; scaleRow.slider.setValue(1.0, juce::dontSendNotification); updatingUI = false;
    };

    // ---- space tab: air and clutter ----
    for (auto* c : std::initializer_list<juce::Component*>{&seedSlider, &seedLabel, &airInfo, &clutterInfo, &randomizeBtn, &showClutter, &showMeters, &versionLabel}) addAndMakeVisible(*c);
    setupRow(tempRow, "Temperature", -20, 45, 0.5, " C");
    setupRow(humRow, "Humidity", 5, 100, 1, " %");
    setupRow(cntRow, "Clutter objects", 0, 80, 1, "");
    setupRow(minLenRow, "Min length", 0.1, 3.0, 0.05, " m");
    setupRow(maxLenRow, "Max length", 0.2, 5.0, 0.05, " m");
    setupRow(tlMinRow, "Min pass loss", 0, 30, 0.5, " dB");
    setupRow(tlMaxRow, "Max pass loss", 0, 40, 0.5, " dB");
    setupRow(hMinRow, "Object min height", 0.1, 3.0, 0.05, " m");
    setupRow(hMaxRow, "Object max height", 0.2, 4.0, 0.05, " m");
    hMinRow.slider.onValueChange = [this] { if (!updatingUI) { const double v = hMinRow.slider.getValue(); editClutter([v](verb::ClutterCfg& c) { c.hMin = v; c.hMax = std::max(c.hMax, v); }); } };
    hMaxRow.slider.onValueChange = [this] { if (!updatingUI) { const double v = hMaxRow.slider.getValue(); editClutter([v](verb::ClutterCfg& c) { c.hMax = v; c.hMin = std::min(c.hMin, v); }); } };
    seedLabel.setText("Seed", juce::dontSendNotification);
    seedSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    seedSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 78, 20);
    seedSlider.setRange(1, 9999, 1);
    airInfo.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    clutterInfo.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    clutterInfo.setJustificationType(juce::Justification::topLeft);
    clutterInfo.setText("Clutter = seeded random half-obstacles (furniture, objects) that absorb a little, let part of the sound through and scatter strongly: "
                        "they fill the space so it does not answer like an empty box.", juce::dontSendNotification);
    tempRow.slider.onValueChange = [this] { if (!updatingUI) proc.editScene([v = tempRow.slider.getValue()](verb::Scene& s) { s.tempC = v; }); };
    humRow.slider.onValueChange = [this] { if (!updatingUI) proc.editScene([v = humRow.slider.getValue()](verb::Scene& s) { s.humidity = v; }); };
    cntRow.slider.onValueChange = [this] { if (!updatingUI) { const int v = (int)cntRow.slider.getValue(); editClutter([v](verb::ClutterCfg& c) { c.count = v; }); } };
    minLenRow.slider.onValueChange = [this] { if (!updatingUI) { const double v = minLenRow.slider.getValue(); editClutter([v](verb::ClutterCfg& c) { c.minLen = v; c.maxLen = std::max(c.maxLen, v); }); } };
    maxLenRow.slider.onValueChange = [this] { if (!updatingUI) { const double v = maxLenRow.slider.getValue(); editClutter([v](verb::ClutterCfg& c) { c.maxLen = v; c.minLen = std::min(c.minLen, v); }); } };
    tlMinRow.slider.onValueChange = [this] { if (!updatingUI) { const double v = tlMinRow.slider.getValue(); editClutter([v](verb::ClutterCfg& c) { c.tlMin = v; c.tlMax = std::max(c.tlMax, v); }); } };
    tlMaxRow.slider.onValueChange = [this] { if (!updatingUI) { const double v = tlMaxRow.slider.getValue(); editClutter([v](verb::ClutterCfg& c) { c.tlMax = v; c.tlMin = std::min(c.tlMin, v); }); } };
    // the seed drives both the clutter layout and the roughness of the surfaces
    seedSlider.onValueChange = [this] { if (!updatingUI) { const int v = (int)seedSlider.getValue(); proc.editScene([v](verb::Scene& s) { s.clutter.seed = v; s.roughSeed = v; }); } };
    randomizeBtn.onClick = [this] {
        const int v = 1 + (int)(juce::Random::getSystemRandom().nextInt(9999));
        proc.editScene([v](verb::Scene& s) { s.clutter.seed = v; s.roughSeed = v; });
    };
    setupRow(roughRow, "Roughness", 0, 100, 1, " %");
    roughRow.slider.onValueChange = [this] { if (!updatingUI) proc.editScene([v = roughRow.slider.getValue() / 100.0](verb::Scene& s) { s.roughness = v; }); };
    addAndMakeVisible(diffToggle);
    diffToggle.onClick = [this] { if (!updatingUI) proc.editScene([on = diffToggle.getToggleState()](verb::Scene& s) { s.diffraction = on; }); };
    showClutter.setToggleState(true, juce::dontSendNotification);
    showClutter.onClick = [this] { repaint(); };
    {
        static const char* names[4] = {"Walls", "Floors", "Ceilings", "Plates"};
        for (int i = 0; i < 4; ++i) {
            layerBtn[i].setButtonText(names[i]);
            layerBtn[i].getProperties().set("style", "ghost");
            layerBtn[i].setTooltip("Click: shown -> locked (clicks pass through it) -> hidden");
            layerBtn[i].onClick = [this, i] {
                layerMode[i] = (layerMode[i] + 1) % 3;
                if (layerMode[i] != 0) {  // a selection in a layer that is now locked or hidden is dropped
                    const bool wall = selKind == SelKind::Wall && i == LayerWalls;
                    bool zone = false;
                    if (selKind == SelKind::FloorZone || selKind == SelKind::CeilZone) {
                        const auto sc = proc.getDisplayScene();
                        const auto& zl = selKind == SelKind::CeilZone ? sc.ceilZones : sc.floorZones;
                        if (selWall >= 0 && selWall < (int)zl.size()) zone = zoneLayer(zl[(size_t)selWall], selKind == SelKind::CeilZone) == i;
                    }
                    if (wall || zone) { selKind = SelKind::Object; selWall = -1; selWalls.clear(); refreshSurface(); }
                }
                updateLayerButtons();
                resized();
                repaint();
            };
            addAndMakeVisible(layerBtn[i]);
        }
        updateLayerButtons();
    }
    {
      #ifdef JucePlugin_VersionString
        const juce::String ver = JucePlugin_VersionString;
      #else
        const juce::String ver = "dev";
      #endif
        versionLabel.setText("Verb Room " + ver + ", built " + juce::String(__DATE__) + " " + juce::String(__TIME__), juce::dontSendNotification);
        versionLabel.setColour(juce::Label::textColourId, skin::textDim);
        versionLabel.setFont(juce::Font(juce::FontOptions(11.f)));
    }
    showMeters.setToggleState(proc.inputMetersEnabled(), juce::dontSendNotification);
    showMeters.onClick = [this] { proc.setInputMetersEnabled(showMeters.getToggleState()); repaint(); };
    showMic.onClick = [this] { repaint(); };
    addAndMakeVisible(showMic);

    // ---- mix tab ----
    for (auto* c : std::initializer_list<juce::Component*>{&gainSlider, &earlySlider, &diffSlider, &dopplerSlider, &mixSlider, &tailSlider, &gainLabel, &earlyLabel, &diffLabel,
                                                           &dopplerLabel, &mixLabel, &tailLabel, &layoutBox, &layoutLabel, &micBox, &micLabel, &outInfoLabel})
        addAndMakeVisible(*c);
    for (auto* s : {&gainSlider, &earlySlider, &diffSlider, &dopplerSlider, &mixSlider, &tailSlider}) {
        s->setSliderStyle(juce::Slider::LinearHorizontal);
        s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    }
    gainLabel.setText("Output gain (dB)", juce::dontSendNotification);
    earlyLabel.setText("Early reflections (dB)  - lower = less comb/flanger", juce::dontSendNotification);
    diffLabel.setText("Early diffusion  - higher = less flanger/phaser", juce::dontSendNotification);
    mixLabel.setText("Dry / wet mix  - 0 % = input only, 100 % = room only", juce::dontSendNotification);
    mixAttachment = std::make_unique<juce::SliderParameterAttachment>(*proc.apvts.getParameter("mix"), mixSlider);
    mixSlider.textFromValueFunction = [](double v) { return juce::String((int)std::lround(v)) + " %"; };
    mixSlider.valueFromTextFunction = [](const juce::String& t) { return t.getDoubleValue(); };
    mixSlider.updateText();
    tailLabel.setText("Tail level (dB)  - the late reverberation alone; the direct sound and early reflections stay", juce::dontSendNotification);
    tailAttachment = std::make_unique<juce::SliderParameterAttachment>(*proc.apvts.getParameter("tail"), tailSlider);
    tailSlider.textFromValueFunction = [](double v) { return (v > 0 ? "+" : "") + juce::String(v, 1) + " dB"; };
    tailSlider.valueFromTextFunction = [](const juce::String& t) { return t.getDoubleValue(); };
    tailSlider.updateText();
    dopplerLabel.setText("Doppler strength  - 1 = physical, 0 = no pitch shift", juce::dontSendNotification);
    dopplerAttachment = std::make_unique<juce::SliderParameterAttachment>(*proc.apvts.getParameter("doppler"), dopplerSlider);
    gainAttachment = std::make_unique<juce::SliderParameterAttachment>(*proc.apvts.getParameter("gain"), gainSlider);
    earlyAttachment = std::make_unique<juce::SliderParameterAttachment>(*proc.apvts.getParameter("early"), earlySlider);
    diffAttachment = std::make_unique<juce::SliderParameterAttachment>(*proc.apvts.getParameter("diffusion"), diffSlider);
    setupRow(radiusRow, "Mic radius", 0, 1, 0.0025, " m");
    layoutLabel.setText("Output layout (virtual microphone array)", juce::dontSendNotification);
    outInfoLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    outInfoLabel.setJustificationType(juce::Justification::topLeft);
    micLabel.setText("Microphone pattern (sensitivity of the listener)", juce::dontSendNotification);
    micBox.setTooltip("The shape of the virtual microphones: omni, cardioid family, figure-8, or a head model (human ears: omni at low frequencies, "
                      "a head shadow that grows with frequency; with a stereo output the capsules become ears 17.5 cm apart facing sideways)");
    micBox.onChange = [this] {
        if (updatingUI) return;
        verbp::OutputCfg o = proc.getOutputCfg();
        const int prev = o.micPattern;
        o.micPattern = micBox.getSelectedId() - 1;
        // the ears sit at the head's half-width: the radius follows the preset (and returns to the default when it is left)
        const int ears = (int)verb::MicPattern::HumanEars;
        if (o.micPattern == ears && prev != ears) o.radius = 0.0875;
        else if (prev == ears && o.micPattern != ears && std::fabs(o.radius - 0.0875) < 1e-6) o.radius = 0.1;
        proc.setOutputCfg(o);  // the engine is rebuilt in the background
    };
    layoutBox.onChange = [this] {
        if (updatingUI) return;
        const int id = layoutBox.getSelectedId();
        verbp::OutputCfg o = proc.getOutputCfg();
        o.layout = id == 1000 ? -1 : id - 1;
        proc.setOutputCfg(o);  // the engine is rebuilt in the background
    };
    auto commitRadius = [this] {
        if (updatingUI) return;
        verbp::OutputCfg o = proc.getOutputCfg();
        o.radius = radiusRow.slider.getValue();
        proc.setOutputCfg(o);
    };
    radiusRow.slider.onDragEnd = commitRadius;
    radiusRow.slider.onValueChange = [this, commitRadius] { if (!radiusRow.slider.isMouseButtonDown()) commitRadius(); };

    tabObjects.setToggleState(true, juce::dontSendNotification);
    selectObject(0);
    showTab(0);
    refreshOutputs();
    refreshSpace();
    startTimerHz(10);
}

VerbEditor::~VerbEditor() {
    stopTimer();
    setLookAndFeel(nullptr);
}

void VerbEditor::setupRow(Row& r, const juce::String& text, double lo, double hi, double step, const juce::String& suffix) {
    addAndMakeVisible(r.label);
    addAndMakeVisible(r.slider);
    r.label.setText(text, juce::dontSendNotification);
    r.slider.setSliderStyle(juce::Slider::LinearHorizontal);
    r.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 78, 20);
    r.slider.setRange(lo, hi, step);
    r.slider.setTextValueSuffix(suffix);
    r.slider.setNumDecimalPlacesToDisplay(step < 1 ? 2 : 0);
}

void VerbEditor::showTab(int t) {
    tab = t;
    tabObjects.setToggleState(t == 0, juce::dontSendNotification);
    tabSurfaces.setToggleState(t == 1, juce::dontSendNotification);
    tabMix.setToggleState(t == 2, juce::dontSendNotification);
    tabSpace.setToggleState(t == 3, juce::dontSendNotification);
    const bool s = t == 1, m = t == 2, sp = t == 3;
    updateObjectsVisibility();
    for (auto* c : std::initializer_list<juce::Component*>{&surfTitle, &selFloor, &selCeiling, &presetBox, &presetLabel, &heightRow.label, &heightRow.slider})
        c->setVisible(s);
    openSky.setVisible(s && (selKind == SelKind::Ceiling || selKind == SelKind::CeilZone));
    for (auto& r : absRows) { r.label.setVisible(s); r.slider.setVisible(s); }
    for (auto& r : scRows) { r.label.setVisible(s); r.slider.setVisible(s); }
    for (auto& r : tlRows) { r.label.setVisible(s); r.slider.setVisible(s); }
    passRow.label.setVisible(s); passRow.slider.setVisible(s);
    roughRow.label.setVisible(sp); roughRow.slider.setVisible(sp);
    diffToggle.setVisible(sp);
    for (auto* c : std::initializer_list<juce::Component*>{&gainSlider, &earlySlider, &diffSlider, &dopplerSlider, &mixSlider, &tailSlider, &gainLabel, &earlyLabel, &diffLabel,
                                                           &dopplerLabel, &mixLabel, &tailLabel})
        c->setVisible(m);
    for (auto* c : std::initializer_list<juce::Component*>{&classBox, &classLabel, &scaleRoomBtn, &scaleObjects, &scaleRow.label, &scaleRow.slider,
                                                           &wallBotRow.label, &wallBotRow.slider, &wallTopRow.label, &wallTopRow.slider, &zoneZRow.label, &zoneZRow.slider})
        c->setVisible(s);
    for (auto* c : std::initializer_list<juce::Component*>{&tempRow.label, &tempRow.slider, &humRow.label, &humRow.slider, &cntRow.label, &cntRow.slider,
                                                           &minLenRow.label, &minLenRow.slider, &maxLenRow.label, &maxLenRow.slider, &tlMinRow.label, &tlMinRow.slider,
                                                           &tlMaxRow.label, &tlMaxRow.slider, &hMinRow.label, &hMinRow.slider, &hMaxRow.label, &hMaxRow.slider, &seedSlider, &seedLabel, &airInfo, &clutterInfo, &randomizeBtn, &showClutter, &showMeters, &versionLabel})
        c->setVisible(sp);
}

void VerbEditor::updateObjectsVisibility() {
    const bool o = tab == 0;
    selLabel.setVisible(o);
    for (auto& x : sliders) x.setVisible(o);
    for (auto& x : sliderLabels) x.setVisible(o);
    for (auto* c : std::initializer_list<juce::Component*>{&sourceBox, &addSourceBtn, &removeSourceBtn}) c->setVisible(o);
    const bool src = o && uiIsSource;
    for (auto* c : std::initializer_list<juce::Component*>{&nameEdit, &nameLabel, &formatLabel, &formatBox, &firstInLabel, &firstInBox, &srcGainLabel, &srcGainSlider, &infoLabel})
        c->setVisible(src);
    // the listener owns the output configuration: layout, microphone radius, what the track carries
    const bool lis = o && selected == verbp::kListener;
    for (auto* c : std::initializer_list<juce::Component*>{&layoutBox, &layoutLabel, &micBox, &micLabel, &outInfoLabel, &radiusRow.label, &radiusRow.slider, &showMic}) c->setVisible(lis);
    chBox.setVisible(src && uiRing);
    chLabel.setVisible(src && uiRing);
    sumToggle.setVisible(src && uiMulti);
    freeToggle.setVisible(src && uiMulti && uiSpread);
    spreadRow.label.setVisible(src && uiMulti && uiSpread);
    spreadRow.slider.setVisible(src && uiMulti && uiSpread);
    patternRow.label.setVisible(src);
    patternRow.slider.setVisible(src);
}

void VerbEditor::refreshSources() {
    cfgCache = proc.getSources();
    if (selected != verbp::kListener && selected >= (int)cfgCache.size()) selected = 0;
    updatingUI = true;
    sourceBox.clear(juce::dontSendNotification);
    for (size_t i = 0; i < cfgCache.size(); ++i) sourceBox.addItem(juce::String((int)i + 1) + ": " + juce::String(cfgCache[i].name), (int)i + 1);
    sourceBox.addItem("Listener", 1000);
    sourceBox.setSelectedId(selected == verbp::kListener ? 1000 : selected + 1, juce::dontSendNotification);
    uiIsSource = selected != verbp::kListener && selected < (int)cfgCache.size();
    if (uiIsSource) {
        const verbp::SourceCfg& c = cfgCache[(size_t)selected];
        const int n = c.effectiveChannels();
        uiMulti = n > 1;
        uiRing = verbp::sourceFormats()[(size_t)c.format].channels == 0;
        uiSpread = !c.sum;
        nameEdit.setText(juce::String(c.name), false);
        selLabel.setText("Source " + juce::String(selected + 1) + ": " + juce::String(c.name), juce::dontSendNotification);
        formatBox.setSelectedId(c.format + 1, juce::dontSendNotification);
        chBox.setSelectedId(std::max(2, c.channels), juce::dontSendNotification);
        firstInBox.setSelectedId(c.firstIn + 1, juce::dontSendNotification);
        sumToggle.setToggleState(c.sum, juce::dontSendNotification);
        freeToggle.setToggleState(c.freePoints, juce::dontSendNotification);
        spreadRow.slider.setValue(c.spread, juce::dontSendNotification);
        patternRow.slider.setValue(c.pattern, juce::dontSendNotification);
        const int lfe = c.lfeChannel();
        int points = (n == 1 || c.sum) ? 1 : n - (lfe >= 0 ? 1 : 0);
        infoLabel.setText("Inputs " + juce::String(c.firstIn + 1) + "-" + juce::String(c.firstIn + n) + " -> " + juce::String(points) + " point" + (points == 1 ? "" : "s") +
                              (lfe >= 0 ? ", LFE (input " + juce::String(c.firstIn + lfe + 1) + ") goes straight to the LFE output" : ""),
                          juce::dontSendNotification);
        srcGainAttachment.reset();
        srcGainAttachment = std::make_unique<juce::SliderParameterAttachment>(*proc.apvts.getParameter(verbp::paramId(selected, 4)), srcGainSlider);
    } else {
        uiMulti = uiRing = uiSpread = false;
        srcGainAttachment.reset();
    }
    updatingUI = false;
    updateObjectsVisibility();
    resized();
}

void VerbEditor::editClutter(std::function<void(verb::ClutterCfg&)> fn) {
    proc.editScene([fn](verb::Scene& s) { fn(s.clutter); });
}

void VerbEditor::refreshSpace() {
    const verb::Scene sc = proc.getDisplayScene();
    updatingUI = true;
    tempRow.slider.setValue(sc.tempC, juce::dontSendNotification);
    humRow.slider.setValue(sc.humidity, juce::dontSendNotification);
    cntRow.slider.setValue(sc.clutter.count, juce::dontSendNotification);
    minLenRow.slider.setValue(sc.clutter.minLen, juce::dontSendNotification);
    maxLenRow.slider.setValue(sc.clutter.maxLen, juce::dontSendNotification);
    tlMinRow.slider.setValue(sc.clutter.tlMin, juce::dontSendNotification);
    tlMaxRow.slider.setValue(sc.clutter.tlMax, juce::dontSendNotification);
    hMinRow.slider.setValue(sc.clutter.hMin, juce::dontSendNotification);
    hMaxRow.slider.setValue(sc.clutter.hMax, juce::dontSendNotification);
    seedSlider.setValue(sc.clutter.seed, juce::dontSendNotification);
    roughRow.slider.setValue(sc.roughness * 100.0, juce::dontSendNotification);
    diffToggle.setToggleState(sc.diffraction, juce::dontSendNotification);
    airInfo.setText("Speed of sound " + juce::String(verb::speedOfSound(sc.tempC), 1) + " m/s | air loss at 8 kHz " +
                        juce::String(verb::airAbsDbPerM(8000, sc.tempC, sc.humidity), 3) + " dB/m (" + juce::String(verb::airAbsDbPerM(8000, sc.tempC, sc.humidity) * 20, 1) + " dB over 20 m)",
                    juce::dontSendNotification);
    updatingUI = false;
}

void VerbEditor::editSource(std::function<void(verbp::SourceCfg&)> fn) {
    if (!uiIsSource || selected >= (int)cfgCache.size()) return;
    auto v = proc.getSources();
    if (selected >= (int)v.size()) return;
    fn(v[(size_t)selected]);
    proc.setSources(v);  // structural: the engine is rebuilt in the background
    cfgCache = v;
    refreshSources();
}

void VerbEditor::refreshOutputs() {
    const verbp::OutputCfg o = proc.getOutputCfg();
    updatingUI = true;
    layoutBox.clear(juce::dontSendNotification);
    layoutBox.addItem("Match the track's channel count (auto)", 1000);
    const auto& defs = verb::layoutDefs();
    for (size_t i = 0; i < defs.size(); ++i)
        layoutBox.addItem(juce::String(defs[i].name) + (defs[i].channels > 0 ? "  (" + juce::String(defs[i].channels) + " ch)" : ""), (int)defs[i].id + 1);
    layoutBox.setSelectedId(o.layout < 0 ? 1000 : o.layout + 1, juce::dontSendNotification);
    radiusRow.slider.setValue(o.radius, juce::dontSendNotification);
    micBox.clear(juce::dontSendNotification);
    for (const auto& d : verb::micPatternDefs()) micBox.addItem(d.name, (int)d.id + 1);
    micBox.setSelectedId(std::min(std::max(o.micPattern, 0), (int)verb::MicPattern::HumanEars) + 1, juce::dontSendNotification);
    juce::String lname = "?";
    for (auto& d : defs) if ((int)d.id == proc.effectiveLayout()) lname = d.name;
    const int rendered = proc.numOutputChannels(), trackOut = proc.hostOutputChannels();
    juce::String info = "Output: " + lname + ", " + juce::String(rendered) + " channel(s) -> track channels 1-" + juce::String(rendered) + ".\n" +
                        "The track has " + juce::String(proc.hostInputChannels()) + " in / " + juce::String(trackOut) +
                        " out: the inputs only carry the sources and do not decide the output. " + juce::String(proc.numPoints()) + " source point(s).";
    if (o.layout >= 0 && verb::layoutDefs()[(size_t)std::min((size_t)o.layout, verb::layoutDefs().size() - 1)].channels > trackOut)
        info += "\nThe layout has more channels than the track can carry: only the first " + juce::String(trackOut) + " are rendered.";
    outInfoLabel.setText(info, juce::dontSendNotification);
    updatingUI = false;
}

void VerbEditor::selectObject(int obj) {
    selKind = SelKind::Object;
    selected = obj;
    const bool lis = obj == verbp::kListener;
    sliderLabels[3].setText(lis ? "Yaw" : "Azimuth", juce::dontSendNotification);
    selLabel.setText(lis ? "Listener" : "Source " + juce::String(obj + 1), juce::dontSendNotification);
    updateSliderRanges();
    syncSliders();
    refreshSurface();
    refreshSources();
    repaint();
}

void VerbEditor::selectSurface(SelKind kind, int wall) {
    selKind = kind;
    selWall = wall;
    selWalls.clear();
    if (kind == SelKind::Wall && wall >= 0) selWalls.push_back(wall);
    showTab(1);
    refreshSurface();
    resized();
    repaint();
}

double VerbEditor::paramValue(int obj, int k) const { return proc.apvts.getRawParameterValue(verbp::paramId(obj, k))->load(); }

void VerbEditor::setParam(const juce::String& id, double v) {
    if (auto* p = proc.apvts.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1((float)v));
}

// ---------- room library / IR export ----------
juce::File VerbEditor::libraryDir() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("SBP").getChildFile("Verb Room").getChildFile("Rooms");
}

void VerbEditor::refreshRoomBox() {
    roomBox.clear(juce::dontSendNotification);
    roomBox.addSectionHeading("Built-in rooms");
    for (size_t i = 0; i < builtins.size(); ++i) roomBox.addItem(juce::String(builtins[i].name), (int)i + 1);
    userRooms.clear();
    const auto dir = libraryDir();
    if (dir.isDirectory()) {
        for (auto& f : dir.findChildFiles(juce::File::findFiles, false, "*.txt")) userRooms.push_back(f);
        std::sort(userRooms.begin(), userRooms.end(), [](const juce::File& a, const juce::File& b) { return a.getFileName().compareIgnoreCase(b.getFileName()) < 0; });
    }
    if (!userRooms.empty()) {
        roomBox.addSectionHeading("My rooms");
        for (size_t i = 0; i < userRooms.size(); ++i) roomBox.addItem(userRooms[i].getFileNameWithoutExtension(), 100 + (int)i);
    }
    roomBox.addSeparator();
    roomBox.addItem("Load from file ...", 1000);
    roomBox.addItem("Open my library folder", 1001);
    roomBox.setSelectedId(0, juce::dontSendNotification);
}

void VerbEditor::loadRoomText(const juce::String& text, const juce::String& name) {
    undoStack.clear();
    selKind = SelKind::Object;
    selWall = -1;
    zonePts.clear();
    proc.loadSceneText(text, name, true);
}

void VerbEditor::onRoomChosen() {
    const int id = roomBox.getSelectedId();
    roomBox.setSelectedId(0, juce::dontSendNotification);  // it is a menu, not a state: the same room can be chosen again
    if (id >= 1 && id <= (int)builtins.size()) {
        chooseBuiltinRoom(id - 1);
    } else if (id >= 100 && id < 100 + (int)userRooms.size()) {
        const auto f = userRooms[(size_t)id - 100];
        if (f.existsAsFile()) loadRoomText(f.loadFileAsString(), f.getFileName());
    } else if (id == 1000) {
        chooseScene();
    } else if (id == 1001) {
        auto dir = libraryDir();
        dir.createDirectory();
        dir.revealToUser();
    }
}

void VerbEditor::saveRoomDialog() {
    auto* w = new juce::AlertWindow("Save room", "Name of the room in your library (" + libraryDir().getFullPathName() + "):", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", juce::File(proc.getSceneName()).getFileNameWithoutExtension());
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<VerbEditor> self(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([self, w](int result) {
                           if (result != 1 || self == nullptr) return;
                           const auto name = juce::File::createLegalFileName(w->getTextEditorContents("name").trim());
                           if (name.isEmpty()) return;
                           auto dir = libraryDir();
                           if (!dir.createDirectory()) return;
                           auto f = dir.getChildFile(name + ".txt");
                           f.replaceWithText(self->proc.getRoomText());
                           self->refreshRoomBox();
                       }),
                       true);
}

void VerbEditor::exportDialog() {
    if (proc.getExportState() == 1) return;
    chooser = std::make_unique<juce::FileChooser>("Folder for the impulse responses", libraryDir().getParentDirectory().getChildFile("IR"));
    juce::Component::SafePointer<VerbEditor> self(this);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectDirectories, [self](const juce::FileChooser& fc) {
        if (self == nullptr) return;
        const auto dir = fc.getResult();
        if (dir == juce::File()) return;
        self->exportShown = true;
        self->proc.startExportIR(dir);
    });
}

void VerbEditor::chooseScene() {
    chooser = std::make_unique<juce::FileChooser>("Choose a room scene", juce::File(), "*.txt");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
        auto f = fc.getResult();
        if (f.existsAsFile()) {
            undoStack.clear();
            selKind = SelKind::Object;
            selWall = -1;
            proc.loadSceneText(f.loadFileAsString(), f.getFileName(), true);
        }
    });
}

// ---------- objects tab ----------
void VerbEditor::updateSliderRanges() {
    double b[4];
    const verb::Scene dsc = proc.getDisplayScene();
    const double height = dsc.ceilMat < 0 ? 30.0 : dsc.height;  // an open sky has no ceiling to stay under
    const bool bounded = proc.getBounds(b);
    updatingUI = true;
    // the range always contains the object's current value, so a position dragged far away still shows correctly
    const double vx = paramValue(selected, 0), vy = paramValue(selected, 1);
    sliders[0].setRange(std::min(bounded ? b[0] : -50.0, vx - 1.0), std::max(bounded ? b[1] : 50.0, vx + 1.0), 0.001);
    sliders[1].setRange(std::min(bounded ? b[2] : -50.0, vy - 1.0), std::max(bounded ? b[3] : 50.0, vy + 1.0), 0.001);
    sliders[2].setRange(0.0, std::max(0.1, height), 0.001);
    sliders[3].setRange(-180.0, 180.0, 0.1);
    updatingUI = false;
}

void VerbEditor::syncSliders() {
    const double vx = paramValue(selected, 0), vy = paramValue(selected, 1);  // dragged/automated beyond the range?
    if (vx < sliders[0].getMinimum() || vx > sliders[0].getMaximum() || vy < sliders[1].getMinimum() || vy > sliders[1].getMaximum()) updateSliderRanges();
    updatingUI = true;
    for (int k = 0; k < 4; ++k)
        if (!sliders[(size_t)k].isMouseButtonDown()) sliders[(size_t)k].setValue(paramValue(selected, k), juce::dontSendNotification);
    updatingUI = false;
}

// ---------- surfaces tab ----------
void VerbEditor::refreshSurface() {
    verb::Scene sc = proc.getDisplayScene();
    int mi = -1;
    juce::String title = "Select a wall on the map, or:";
    if (selKind == SelKind::Wall && selWall >= 0 && selWall < (int)sc.walls.size()) {
        const auto& w = sc.walls[(size_t)selWall];
        mi = w.mat;
        title = "Wall " + juce::String(selWall + 1) + "  (" + juce::String(std::hypot(w.bx - w.ax, w.by - w.ay), 2) + " m)";
        if (selWalls.size() > 1) title = juce::String((int)selWalls.size()) + " walls selected  (edits apply to all)";
    } else if (selKind == SelKind::Floor) {
        mi = sc.floorMat;
        title = "Floor";
    } else if (selKind == SelKind::Ceiling) {
        mi = sc.ceilMat;
        title = sc.ceilMat < 0 ? "Ceiling (open sky)" : "Ceiling";
    } else if (selKind == SelKind::FloorZone && selWall >= 0 && selWall < (int)sc.floorZones.size()) {
        mi = sc.floorZones[(size_t)selWall].mat;
        title = "Floor zone " + juce::String(selWall + 1);
    } else if (selKind == SelKind::CeilZone && selWall >= 0 && selWall < (int)sc.ceilZones.size()) {
        mi = sc.ceilZones[(size_t)selWall].mat;
        title = "Ceiling zone " + juce::String(selWall + 1) + (mi < 0 ? " (opening)" : "");
    } else if (selKind == SelKind::Wall || selKind == SelKind::FloorZone || selKind == SelKind::CeilZone) {
        selKind = SelKind::Object;
    }
    surfTitle.setText(title, juce::dontSendNotification);
    const bool zoneCeil = selKind == SelKind::CeilZone;
    openSky.setButtonText(zoneCeil ? "Opening (no ceiling here)" : "Open sky (no ceiling)");
    openSky.setVisible(tab == 1 && (selKind == SelKind::Ceiling || zoneCeil));
    updatingUI = true;
    openSky.setToggleState(zoneCeil ? mi < 0 : sc.ceilMat < 0, juce::dontSendNotification);
    heightRow.slider.setValue(sc.height, juce::dontSendNotification);
    const bool isWall = selKind == SelKind::Wall && selWall >= 0 && selWall < (int)sc.walls.size();
    wallBotRow.slider.setEnabled(isWall);
    wallTopRow.slider.setEnabled(isWall);
    {
        const bool isZone = (selKind == SelKind::FloorZone && selWall >= 0 && selWall < (int)sc.floorZones.size()) ||
                            (selKind == SelKind::CeilZone && selWall >= 0 && selWall < (int)sc.ceilZones.size());
        zoneZRow.slider.setEnabled(isZone && !(selKind == SelKind::CeilZone && sc.ceilZones[(size_t)selWall].mat < 0));
        const double z = !isZone ? -1.0 : (selKind == SelKind::FloorZone ? sc.floorZones[(size_t)selWall].z : sc.ceilZones[(size_t)selWall].z);
        zoneZRow.slider.setValue(z < 0 ? 0.0 : z, juce::dontSendNotification);
    }
    wallBotRow.slider.setRange(0.0, std::max(0.1, sc.height), 0.05);
    wallTopRow.slider.setRange(0.1, std::max(0.2, sc.height), 0.05);
    if (isWall) {
        const auto& w = sc.walls[(size_t)selWall];
        wallBotRow.slider.setValue(std::min(w.z0, sc.height), juce::dontSendNotification);
        wallTopRow.slider.setValue(w.z1 >= sc.height ? sc.height : w.z1, juce::dontSendNotification);  // the maximum = full height
    }
    const bool on = mi >= 0 && mi < (int)sc.mats.size();
    for (auto& r : absRows) r.slider.setEnabled(on);
    for (auto& r : scRows) r.slider.setEnabled(on);
    for (auto& r : tlRows) r.slider.setEnabled(on);
    passRow.slider.setEnabled(on);
    presetBox.setEnabled(on);
    classBox.setEnabled(on);
    if (on) {
        const verb::Material& m = sc.mats[(size_t)mi];
        for (int b = 0; b < 7; ++b) absRows[(size_t)b].slider.setValue(m.alpha[(size_t)b] * 100.0, juce::dontSendNotification);
        scRows[0].slider.setValue(m.scatter * 100.0, juce::dontSendNotification);
        scRows[1].slider.setValue((m.scatterHf < 0 ? m.scatter : m.scatterHf) * 100.0, juce::dontSendNotification);
        {
            double mt = 0;
            for (int b = 0; b < 7; ++b) mt += m.tau[(size_t)b] / 7.0;
            passRow.slider.setValue(std::max(0.0, 100.0 * (1.0 - tlFromTau(mt) / 60.0)), juce::dontSendNotification);
        }
        tlRows[0].slider.setValue(tlFromTau((m.tau[0] + m.tau[1]) / 2), juce::dontSendNotification);
        tlRows[1].slider.setValue(tlFromTau((m.tau[2] + m.tau[3]) / 2), juce::dontSendNotification);
        tlRows[2].slider.setValue(tlFromTau((m.tau[4] + m.tau[5] + m.tau[6]) / 3), juce::dontSendNotification);
        int id = 1000;
        const auto& pr = verb::surfacePresets();
        for (size_t i = 0; i < pr.size(); ++i)
            if (pr[i].name == m.name) id = (int)i + 1;
        presetBox.setSelectedId(id, juce::dontSendNotification);
        classBox.setSelectedId(std::min(std::max(m.cls, 0), (int)verb::SC_Custom) + 1, juce::dontSendNotification);
    } else {
        presetBox.setSelectedId(0, juce::dontSendNotification);
    }
    updatingUI = false;
}

void VerbEditor::editSurface(std::function<void(verb::Material&)> fn) {
    const int kind = selKind == SelKind::Wall ? 1 : selKind == SelKind::Floor ? 2 : selKind == SelKind::Ceiling ? 3
                     : selKind == SelKind::FloorZone ? 4 : selKind == SelKind::CeilZone ? 5 : 0;
    const int wall = selWall;
    if (kind == 0) return;
    // every selected wall takes the change (each keeps its own material, only the edited property is set)
    std::vector<int> targets{wall};
    if (kind == 1 && !selWalls.empty()) targets = selWalls;
    proc.editScene([kind, targets, fn](verb::Scene& s) {
        for (int w : targets) {
            const int idx = ownMaterialFor(s, kind, w);
            if (idx < 0) continue;
            verb::Material& m = s.mats[(size_t)idx];
            fn(m);
            for (int b = 0; b < verb::NB; ++b) {  // energy bookkeeping: absorbed + transmitted <= 1
                m.alpha[(size_t)b] = std::min(1.0, std::max(0.0, m.alpha[(size_t)b]));
                m.tau[(size_t)b] = std::min(1.0, std::max(0.0, m.tau[(size_t)b]));
                if (m.alpha[(size_t)b] + m.tau[(size_t)b] > 1.0) m.alpha[(size_t)b] = 1.0 - m.tau[(size_t)b];
            }
        }
    });
}

void VerbEditor::pushUndo() {
    undoStack.push_back(proc.getDisplayScene());
    if (undoStack.size() > 60) undoStack.erase(undoStack.begin());
}

void VerbEditor::undo() {
    if (undoStack.empty()) return;
    verb::Scene snap = undoStack.back();
    undoStack.pop_back();
    proc.editScene([snap](verb::Scene& s) { s = snap; });
    selWalls.erase(std::remove_if(selWalls.begin(), selWalls.end(), [&](int i) { return i >= (int)snap.walls.size(); }), selWalls.end());
    if ((selKind == SelKind::Wall && selWall >= (int)snap.walls.size()) || (selKind == SelKind::FloorZone && selWall >= (int)snap.floorZones.size()) ||
        (selKind == SelKind::CeilZone && selWall >= (int)snap.ceilZones.size())) { selKind = SelKind::Object; selWall = -1; }
    refreshSurface();
    repaint();
}

void VerbEditor::deleteSelectedWall() {
    if (selWall < 0 || (selKind != SelKind::Wall && selKind != SelKind::FloorZone && selKind != SelKind::CeilZone)) return;
    pushUndo();
    const int w = selWall;
    const SelKind kind = selKind;
    std::vector<int> ws = selWalls;
    if (ws.empty()) ws.push_back(w);
    std::sort(ws.begin(), ws.end(), std::greater<int>());  // from the back, so the indices stay valid
    proc.editScene([w, kind, ws](verb::Scene& s) {
        auto& list = kind == SelKind::FloorZone ? s.floorZones : s.ceilZones;
        if (kind == SelKind::Wall) {
            for (int i : ws)
                if (i >= 0 && i < (int)s.walls.size()) s.walls.erase(s.walls.begin() + i);
        } else if (w >= 0 && w < (int)list.size()) {
            list.erase(list.begin() + w);
        }
    });
    selKind = SelKind::Object;
    selWall = -1;
    selWalls.clear();
    refreshSurface();
    repaint();
}

void VerbEditor::clearRoom() {
    pushUndo();
    proc.editScene([](verb::Scene& s) {
        s.walls.clear();
        s.floorZones.clear();
        s.ceilZones.clear();
        s.clutter.count = 0;
        s.roughness = 0;
    });
    selKind = SelKind::Object;
    selWall = -1;
    selWalls.clear();
    zonePts.clear();
    refreshSurface();
    refreshSpace();
    repaint();
}

bool VerbEditor::keyPressed(const juce::KeyPress& key) {
    if (!zonePts.empty()) {  // drawing a zone: Enter closes it, Escape drops it
        if (key == juce::KeyPress::returnKey) { finishZone(); return true; }
        if (key == juce::KeyPress::escapeKey) { zonePts.clear(); repaint(); return true; }
    }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) { deleteSelectedWall(); return true; }
    if (key.getModifiers().isCommandDown() && (key.getTextCharacter() == 'z' || key.getTextCharacter() == 'Z')) { undo(); return true; }
    return false;
}

// ---------- timer / view ----------
void VerbEditor::timerCallback() {
    auto s = proc.getStatus();
    const int st = proc.getLoadState();
    const uint64_t ser = proc.getSceneSerial();
    bool exporting = false;
    if (exportShown) {  // the export's progress / result replaces the scene status until the scene status changes again
        const int es = proc.getExportState();
        statusLabel.setText(proc.getExportMessage(), juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, es == 3 ? juce::Colours::salmon : es == 2 ? juce::Colours::lightgreen : juce::Colours::khaki);
        if (es == 1) exporting = true;
        else { exportShown = false; lastStatus = s; lastState = st; }  // finished: the result stays until the next change
    }
    if (!exporting && (s != lastStatus || st != lastState)) {
        lastStatus = s;
        lastState = st;
        statusLabel.setText(s, juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, st == 3 ? juce::Colours::salmon : st == 1 ? juce::Colours::khaki : juce::Colours::lightgrey);
    }
    if (ser != lastSerial) {
        lastSerial = ser;
        updateSliderRanges();
        refreshSurface();
        refreshSources();
        refreshOutputs();
        refreshSpace();
    }
    syncSliders();  // follow host automation
    repaint();
}

VerbEditor::View VerbEditor::makeView(const verb::Scene& sc, const double* lis) const {
    if (!autoFit) {  // free view: its own centre and zoom
        View fv;
        fv.area = canvas.reduced(14).toFloat();
        fv.scale = vScale;
        fv.minX = vCx - fv.area.getWidth() / (2 * vScale);
        fv.minY = vCy - fv.area.getHeight() / (2 * vScale);
        fv.rotated = faceUp && inPlan();
        fv.psi = viewPsi;
        return fv;
    }
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    const bool rot = faceUp && inPlan();
    View frameView;  // only for the frame conversion of the extents
    frameView.rotated = rot;
    frameView.psi = viewPsi;
    // extents in the map's frame (turned so that the listener's heading is up)
    auto add = [&](double x, double y) {
        double u, w;
        frameView.frame(x, y, u, w);
        x0 = std::min(x0, u); y0 = std::min(y0, w); x1 = std::max(x1, u); y1 = std::max(y1, w);
    };
    if (!inPlan()) {  // elevation: horizontal extent of everything, height from the floor to the ceiling
        auto addU = [&](double x, double y) { const double u = horiz(x, y); x0 = std::min(x0, u); x1 = std::max(x1, u); };
        for (auto& w : sc.walls) { addU(w.ax, w.ay); addU(w.bx, w.by); }
        for (auto* zl : {&sc.floorZones, &sc.ceilZones})
            for (auto& z : *zl) for (auto& p : z.poly) addU(p.first, p.second);
        for (int s = 0; s < (int)cfgCache.size(); ++s) addU(paramValue(s, 0), paramValue(s, 1));
        addU(lis[0], lis[1]);
        if (x1 < x0) { x0 = -5; x1 = 5; }
        x0 -= 1.5; x1 += 1.5;
        y0 = -0.6; y1 = std::max(sc.height, 1.0) + 0.6;
        View v;
        v.area = canvas.reduced(14).toFloat();
        v.scale = std::min(v.area.getWidth() / (x1 - x0), v.area.getHeight() / (y1 - y0));
        v.minX = x0 - (v.area.getWidth() / v.scale - (x1 - x0)) / 2;
        v.minY = y0 - (v.area.getHeight() / v.scale - (y1 - y0)) / 2;
        return v;
    }
    for (auto& w : sc.walls) { add(w.ax, w.ay); add(w.bx, w.by); }
    for (auto* zl : {&sc.floorZones, &sc.ceilZones})
        for (auto& z : *zl) for (auto& p : z.poly) add(p.first, p.second);
    for (int s = 0; s < (int)cfgCache.size(); ++s) add(paramValue(s, 0), paramValue(s, 1));
    add(lis[0], lis[1]);
    if (x1 < x0) { x0 = y0 = -5; x1 = y1 = 5; }
    const double pad = 1.5;
    x0 -= pad; y0 -= pad; x1 += pad; y1 += pad;
    View v;
    v.rotated = rot;
    v.psi = viewPsi;
    v.area = canvas.reduced(14).toFloat();
    v.scale = std::min(v.area.getWidth() / (x1 - x0), v.area.getHeight() / (y1 - y0));
    v.minX = x0 - (v.area.getWidth() / v.scale - (x1 - x0)) / 2;
    v.minY = y0 - (v.area.getHeight() / v.scale - (y1 - y0)) / 2;
    return v;
}

void VerbEditor::paint(juce::Graphics& g) {
    g.fillAll(kBg);
    g.setColour(skin::panel);  // the tool strip and the right-hand control panel sit on their own surfaces
    g.fillRect(getLocalBounds().withLeft(canvas.getRight() + 8).toFloat());
    g.fillRect(toolStrip.withTop(0).withBottom(getHeight()).toFloat());
    {   // section headers: a small spaced caption and a hairline after it
        const juce::Font hf = juce::Font(juce::FontOptions(10.5f, juce::Font::bold)).withExtraKerningFactor(0.14f);
        for (auto& h : headers[std::min(std::max(tab, 0), 3)]) {
            const juce::String t = h.second == "@SEL" ? selLabel.getText().toUpperCase() : h.second;
            juce::GlyphArrangement ga;
            ga.addLineOfText(hf, t, 0.f, 0.f);
            const float w = ga.getBoundingBox(0, -1, true).getWidth();
            g.setColour(skin::textDim);
            g.setFont(hf);
            g.drawText(t, h.first.withWidth((int)w + 6), juce::Justification::centredLeft);
            g.setColour(skin::line);
            g.fillRect((float)h.first.getX() + w + 12.f, (float)h.first.getCentreY(), (float)h.first.getWidth() - w - 12.f, 1.f);
        }
        g.setFont(juce::Font(juce::FontOptions(12.5f)));  // the header font has letter-spacing: never let it leak into the map's text
    }
    g.setColour(kGrid);
    g.fillRect(canvas.toFloat());
    double home[4];
    verb::Scene sc = proc.getDisplayScene(home);
    double lis[4];
    for (int k = 0; k < 4; ++k) lis[k] = paramValue(verbp::kListener, k);
    if (viewPsiSerial != proc.getLoadSerial() || relatchView) {  // the listener's heading is up when a room loads; then the map holds still while the listener turns
        viewPsi = lis[3] * verb::kPi / 180.0;
        viewPsiSerial = proc.getLoadSerial();
        relatchView = false;
    }
    if (!lockView) view = makeView(sc, lis);
    if (autoFit) {  // remember the fitted view: the free view starts from it
        vCx = view.minX + view.area.getWidth() / (2 * view.scale);
        vCy = view.minY + view.area.getHeight() / (2 * view.scale);
        vScale = view.scale;
    }

    g.saveState();
    g.reduceClipRegion(canvas);
    // grid: the spacing adapts to the zoom (about 24 px or more between lines)
    {
        static const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500};
        double step = 500;
        for (double st : steps)
            if (st * view.scale >= 24.0) { step = st; break; }
        g.setColour(skin::gridLine);
        for (double u = std::floor(view.minX / step) * step; view.uToX(u) < view.area.getRight(); u += step) {  // in the map's own frame
            const float sx = view.uToX(u);
            if (sx >= view.area.getX()) g.drawVerticalLine((int)sx, view.area.getY(), view.area.getBottom());
        }
        for (double w = std::floor(view.minY / step) * step; view.vToY(w) > view.area.getY(); w += step) {
            const float sy = view.vToY(w);
            if (sy <= view.area.getBottom()) g.drawHorizontalLine((int)sy, view.area.getX(), view.area.getRight());
        }
        g.setColour(juce::Colours::grey);
        g.setFont(11.f);
        g.drawText(juce::String(step, step < 1 ? 1 : 0) + " m grid", (int)view.area.getRight() - 90, (int)view.area.getY() + 2, 88, 14, juce::Justification::right);
    }

    if (plane == Plane::Iso) {
        paint3D(g, sc, lis);
        g.restoreState();
        g.setColour(skin::textDim);
        g.setFont(juce::Font(juce::FontOptions(11.5f)));
        g.drawText("3D view  |  drag: orbit  |  wheel: zoom  |  double-click: reset the camera  |  edit in Top, Front or Side",
                   canvas.getX() + 8, canvas.getBottom() - 20, canvas.getWidth() - 16, 16, juce::Justification::left);
        return;
    }
    if (!inPlan()) {
        paintElevation(g, sc, lis);
        g.restoreState();
        g.setColour(juce::Colours::grey);
        g.setFont(12.f);
        g.drawText("Height " + juce::String(sc.height, 1) + " m | drag sources/listener to change position and height | drag a wall's top/bottom edge | click the floor or ceiling line | wheel: zoom",
                   canvas.getX() + 8, canvas.getBottom() - 20, canvas.getWidth() - 16, 16, juce::Justification::left);
        return;
    }
    // floor and ceiling zones: tinted regions (floor solid, ceiling dashed outline; the layer of the selected tool/zone is stronger)
    for (int pass = 0; pass < 2; ++pass) {
        const bool ceiling = pass == 1;
        const auto& zl = ceiling ? sc.ceilZones : sc.floorZones;
        const bool active = ceiling == zoneLayerCeiling;
        for (size_t i = 0; i < zl.size(); ++i) {
            if (zl[i].poly.size() < 3 || layerMode[zoneLayer(zl[i], ceiling)] == 2) continue;
            juce::Path path;
            juce::Point<float> centre;
            for (size_t k = 0; k < zl[i].poly.size(); ++k) {
                const auto p = view.toScreen(zl[i].poly[k].first, zl[i].poly[k].second);
                if (k == 0) path.startNewSubPath(p); else path.lineTo(p);
                centre += p;
            }
            path.closeSubPath();
            centre /= (float)zl[i].poly.size();
            const int mat = zl[i].mat;
            const juce::Colour c = mat >= 0 && mat < (int)sc.mats.size()
                                       ? juce::Colour(verb::surfaceClasses()[(size_t)std::min(std::max(sc.mats[(size_t)mat].cls, 0), (int)verb::SC_Custom)].argb)
                                       : juce::Colours::black;
            const bool sel = selKind == (ceiling ? SelKind::CeilZone : SelKind::FloorZone) && selWall == (int)i;
            g.setColour(c.withAlpha(mat < 0 ? 0.35f : (active ? 0.24f : 0.09f)));
            g.fillPath(path);
            g.setColour(sel ? kSel : c.withAlpha(active ? 0.9f : 0.4f));
            if (ceiling) {
                const float dash[2] = {6.f, 4.f};
                juce::Path dashed;
                juce::PathStrokeType(sel ? 1.8f : 1.f).createDashedStroke(dashed, path, dash, 2);
                g.fillPath(dashed);
            } else {
                g.strokePath(path, juce::PathStrokeType(sel ? 1.8f : 1.f));
            }
            if (active || sel) {
                g.setColour(sel ? kSel : juce::Colours::lightgrey.withAlpha(0.8f));
                g.setFont(11.f);
                g.drawText(juce::String(ceiling ? "ceiling " : "floor ") + (mat < 0 ? juce::String("opening") : juce::String(sc.mats[(size_t)mat].name).replaceCharacter('_', ' ')) +
                               (zl[i].z >= 0 ? " @ " + juce::String(zl[i].z, 2) + " m" : juce::String()),
                           (int)centre.x - 60, (int)centre.y - 7, 120, 14, juce::Justification::centred);
            }
            if (sel) {
                g.setColour(kSel);
                for (auto& p : zl[i].poly) { const auto sp = view.toScreen(p.first, p.second); g.fillEllipse(sp.x - 5, sp.y - 5, 10, 10); }
            }
        }
    }
    if (!zonePts.empty()) {  // the zone being drawn
        g.setColour(kSel.withAlpha(0.9f));
        for (size_t k = 0; k + 1 < zonePts.size(); ++k)
            g.drawLine({view.toScreen(zonePts[k].x, zonePts[k].y), view.toScreen(zonePts[k + 1].x, zonePts[k + 1].y)}, 2.f);
        const auto last = view.toScreen(zonePts.back().x, zonePts.back().y);
        g.setColour(kSel.withAlpha(0.45f));
        g.drawLine({last, hoverPos}, 1.5f);
        if (zonePts.size() >= 3) g.drawLine({hoverPos, view.toScreen(zonePts[0].x, zonePts[0].y)}, 1.f);
        g.setColour(kSel);
        for (auto& p : zonePts) { const auto sp = view.toScreen(p.x, p.y); g.fillEllipse(sp.x - 4, sp.y - 4, 8, 8); }
    }
    // walls: warm tint = absorbent, dashed blue = lets sound through (doorway, window, curtain, partition)
    for (size_t i = 0; i < sc.walls.size() && layerMode[LayerWalls] != 2; ++i) {
        const auto& w = sc.walls[i];
        double t = 0;
        juce::Colour classColour = kWall;
        if (w.mat >= 0 && w.mat < (int)sc.mats.size()) {
            t = meanOf(sc.mats[(size_t)w.mat].tau);
            classColour = juce::Colour(verb::surfaceClasses()[(size_t)std::min(std::max(sc.mats[(size_t)w.mat].cls, 0), (int)verb::SC_Custom)].argb);
        }
        const bool sel = wallSelected((int)i);
        const auto p0 = view.toScreen(w.ax, w.ay), p1 = view.toScreen(w.bx, w.by);
        juce::Colour c = classColour;  // the colour tells the class of the surface (see the Surfaces tab)
        if (sel) c = kSel;
        if (t > 0.02) {  // dashed = lets sound through (the more it passes, the fainter)
            const float dash[2] = {9.f, 6.f};
            g.setColour(sel ? kSel : classColour.withAlpha(0.45f + 0.55f * (float)(1.0 - std::min(1.0, t))));
            g.drawDashedLine(juce::Line<float>(p0, p1), dash, 2, sel ? 2.4f : 1.5f);
        } else {
            const bool partial = w.z0 > 0.001 || w.z1 < sc.height - 1e-6;  // not floor to ceiling: sound passes over/under it
            g.setColour(partial && !sel ? c.withAlpha(0.6f) : c);
            g.drawLine({p0, p1}, sel ? 2.4f : (partial ? 1.f : 1.6f));
        }
        if (sel) {
            g.setColour(kSel);
            g.fillEllipse(p0.x - 5, p0.y - 5, 10, 10);
            g.fillEllipse(p1.x - 5, p1.y - 5, 10, 10);
            if (w.z0 > 0.001 || w.z1 < sc.height - 1e-6) {  // vertical extent of a partial wall
                g.setColour(kSel);
                g.setFont(12.f);
                g.drawText(juce::String(w.z0, 2) + " - " + juce::String(std::min(w.z1, sc.height), 2) + " m", (int)((p0.x + p1.x) / 2) - 40, (int)((p0.y + p1.y) / 2) - 18, 80, 14, juce::Justification::centred);
            }
        }
    }
    if (dragMode == DragMode::Box) {  // the rubber band of the box-select tool
        const auto rb = juce::Rectangle<float>(boxStart, boxEnd);
        g.setColour(skin::accent.withAlpha(0.10f));
        g.fillRect(rb);
        g.setColour(skin::accentHi.withAlpha(0.8f));
        g.drawRect(rb, 1.f);
    }
    if (dragMode == DragMode::NewWall) {
        g.setColour(kSel.withAlpha(0.8f));
        g.drawLine({view.toScreen(addStart.x, addStart.y), view.toScreen(addEnd.x, addEnd.y)}, 2.f);
    }
    if (showClutter.getToggleState() && sc.clutter.count > 0) {  // the seeded half-obstacles that fill the space
        const verb::Scene eff = verb::withClutter(sc, 128);
        g.setColour(juce::Colour(verb::surfaceClasses()[verb::SC_Furniture].argb).withAlpha(0.55f));
        for (size_t i = sc.walls.size(); i < eff.walls.size(); ++i)
            g.drawLine({view.toScreen(eff.walls[i].ax, eff.walls[i].ay), view.toScreen(eff.walls[i].bx, eff.walls[i].by)}, 1.6f);
    }

    // the output layout's virtual microphones around the listener (schematic, or to scale when asked)
    {
        (void)home;
        const double ly = lis[3] * verb::kPi / 180.0, c2 = std::cos(ly), s2 = std::sin(ly);
        const bool toScale = showMic.getToggleState();
        const double micR = toScale ? std::max(0.005, proc.getOutputCfg().radius) : 0.45;
        if (toScale) {
            const auto cp = view.toScreen(lis[0], lis[1]);
            const float rpx = (float)(micR * view.scale);
            g.setColour(kLis.withAlpha(0.35f));
            g.drawEllipse(cp.x - rpx, cp.y - rpx, 2 * rpx, 2 * rpx, 1.f);
        }
        auto arr = verb::makeReceiverArray((verb::Layout)proc.effectiveLayout(), {0, 0, 0}, micR, std::max(1, proc.numOutputChannels()));
        verb::applyMicPattern(arr, (verb::MicPattern)proc.getOutputCfg().micPattern, proc.getOutputCfg().radius);  // human ears: the capsules move to the ears
        for (auto& r : arr) {
            if (r.gainDb < -100) continue;  // LFE placeholder
            const auto p = view.toScreen(lis[0] + r.pos.x * c2 - r.pos.y * s2, lis[1] + r.pos.x * s2 + r.pos.y * c2);
            const double az = (r.azimuthDeg + lis[3]) * verb::kPi / 180.0;
            g.setColour((r.elevationDeg > 0 ? skin::gold : kLis).withAlpha(0.6f));
            const auto dv = view.dir(az);
            g.drawLine({p, {p.x + 14.f * dv.x, p.y + 14.f * dv.y}}, 1.5f);
            g.fillEllipse(p.x - 2.5f, p.y - 2.5f, 5.f, 5.f);
        }
    }
    auto drawObj = [&](int obj, juce::Colour c, const juce::String& label, double az) {
        const auto p = view.toScreen(paramValue(obj, 0), paramValue(obj, 1));
        const bool sel = selKind == SelKind::Object && obj == selected;
        const float r = sel ? 6.f : 5.f;
        g.setColour(c.withAlpha(sel ? 0.22f : 0.14f));
        g.fillEllipse(p.x - r * 2.2f, p.y - r * 2.2f, r * 4.4f, r * 4.4f);
        g.setColour(c);
        g.fillEllipse(p.x - r, p.y - r, 2 * r, 2 * r);
        if (sel) { g.setColour(c.withAlpha(0.9f)); g.drawEllipse(p.x - r - 3, p.y - r - 3, 2 * r + 6, 2 * r + 6, 1.f); }
        const double a = az * verb::kPi / 180.0;
        g.setColour(c);
        const auto dv = view.dir(a);
        g.drawLine({{p.x + (r + 1.f) * dv.x, p.y + (r + 1.f) * dv.y}, {p.x + 22.f * dv.x, p.y + 22.f * dv.y}}, 1.2f);
        g.setColour(sel ? skin::text : skin::textMid);
        g.setFont(juce::Font(juce::FontOptions(12.f)));
        g.drawText(label, (int)p.x - 40, (int)p.y + 11, 80, 15, juce::Justification::centred);
    };
    for (int s = 0; s < (int)cfgCache.size(); ++s) {
        const verbp::SourceCfg& c = cfgCache[(size_t)s];
        const double az = paramValue(s, 3), x = paramValue(s, 0), y = paramValue(s, 1);
        const juce::Colour col = srcColour(s);  // one colour per source: its centre and all its channel points
        if (c.effectiveChannels() > 1 && !c.sum) {  // the channels of a multichannel source
            const double ca = std::cos(az * verb::kPi / 180.0), sa = std::sin(az * verb::kPi / 180.0);
            for (int ch = 0; ch < c.effectiveChannels(); ++ch) {
                if (ch == c.lfeChannel()) continue;
                float ox, oy;
                if (c.freePoints && ch < (int)c.ox.size() && ch < (int)c.oy.size()) { ox = c.ox[(size_t)ch]; oy = c.oy[(size_t)ch]; }
                else verbp::layoutOffset(c, ch, ox, oy);
                const auto p = view.toScreen(x + ca * ox - sa * oy, y + sa * ox + ca * oy);
                const auto cp = view.toScreen(x, y);
                g.setColour(col.withAlpha(0.35f));
                g.drawLine({cp, p}, 1.f);
                g.setColour(col);
                g.fillEllipse(p.x - 3.5f, p.y - 3.5f, 7.f, 7.f);
                if (c.freePoints) {  // draggable: show the channel number
                    g.setColour(juce::Colours::black.withAlpha(0.8f));
                    g.setFont(10.f);
                    g.drawText(juce::String(ch + 1), (int)p.x - 6, (int)p.y - 6, 12, 12, juce::Justification::centred);
                }
            }
        }
        drawObj(s, col, juce::String(c.name), az);
    }
    drawObj(verbp::kListener, kLis, "Listener", lis[3]);
    g.restoreState();

    g.setColour(skin::textDim);
    g.setFont(juce::Font(juce::FontOptions(11.5f)));
    g.drawText("Height " + juce::String(sc.height, 1) + " m  |  " + juce::String((int)sc.walls.size()) + " walls  |  dashed = sound passes through  |  Shift+click: add walls to the selection  |  wheel: zoom  |  drag empty space: pan  |  click again: next surface under the cursor  |  right click: aim",
               canvas.getX() + 8, canvas.getBottom() - 20, canvas.getWidth() - 16, 16, juce::Justification::left);
    paintInputMeters(g);
}

// One bar per input channel of the host (numbered as the host numbers them, from 1), coloured like the source that
// reads the channel, so a wrong send / pin mapping is visible at a glance.
void VerbEditor::paintInputMeters(juce::Graphics& g) {
    const int nIn = std::min(proc.hostInputChannels(), verbp::kMaxInputChannels);
    if (!proc.inputMetersEnabled() || nIn <= 2 || canvas.getWidth() < 200) return;
    std::vector<int> owner((size_t)nIn, -1);
    for (int s = 0; s < (int)cfgCache.size(); ++s) {
        const auto& c = cfgCache[(size_t)s];
        for (int q = 0; q < c.effectiveChannels(); ++q)
            if (c.firstIn + q < nIn) owner[(size_t)(c.firstIn + q)] = s;
    }
    const float x0 = (float)canvas.getX() + 8.f, w = (float)canvas.getWidth() - 16.f, y1 = (float)canvas.getBottom() - 34.f, h = 30.f;
    const float bw = w / (float)nIn;
    g.setFont(juce::Font(juce::FontOptions(8.5f)));
    for (int ch = 0; ch < nIn; ++ch) {
        const float pk = proc.inputPeak(ch);
        const float db = pk > 1e-5f ? 20.f * std::log10(pk) : -100.f;
        const float lvl = juce::jlimit(0.f, 1.f, (db + 60.f) / 60.f);
        const juce::Colour col = owner[(size_t)ch] >= 0 ? srcColour(owner[(size_t)ch]) : juce::Colours::grey;
        const float bx = x0 + bw * (float)ch;
        g.setColour(col.withAlpha(0.10f));
        g.fillRect(bx + 1.f, y1 - h, bw - 2.f, h);
        g.setColour(col.withAlpha(owner[(size_t)ch] >= 0 ? 0.9f : 0.5f));
        g.fillRect(bx + 1.f, y1 - h * lvl, bw - 2.f, h * lvl);
        g.setColour(skin::textDim);
        if (bw >= 14.f || ch % 2 == 0) g.drawText(juce::String(ch + 1), (int)bx - 2, (int)y1 + 1, (int)bw + 4, 10, juce::Justification::centred);
    }
}

void VerbEditor::resized() {
    for (auto& h : headers) h.clear();
    // a section header: a small caption with a hairline after it; the text "@SEL" is the selected object's name
    auto header = [&](int page, juce::Rectangle<int>& a, const juce::String& t, int gap = 8) {
        a.removeFromTop(gap);
        headers[page].push_back({a.removeFromTop(14), t});
        a.removeFromTop(4);
    };
    auto b = getLocalBounds();
    auto panel = b.removeFromRight(kPanelW).reduced(16, 14);
    auto strip = b.removeFromLeft(54).reduced(7, 10);  // the tool strip, like a 3D editor
    auto viewBar = b.removeFromTop(36).reduced(8, 5);
    canvas = b.reduced(8);
    toolStrip = juce::Rectangle<int>(0, 0, 54, getHeight());
    {
        int lx = canvas.getX() + 8;
        for (int i = 0; i < 4; ++i) {
            const int w = layerMode[i] == 0 ? 74 : 112;
            layerBtn[i].setBounds(lx, canvas.getY() + 6, w, 22);
            lx += w + 4;
        }
    }
    auto place = [&](std::initializer_list<juce::Component*> group) {
        for (auto* t : group) { t->setBounds(strip.removeFromTop(40).reduced(0, 2)); }
        strip.removeFromTop(10);
    };
    place({&toolMove, &toolBox});
    place({&toolAdd, &toolFloorZone, &toolCeilZone});
    place({&toolDelete, &toolUndo});
    place({&toolClear});
    autoViewBtn.setBounds(viewBar.removeFromRight(100));
    for (auto* t : {&viewTop, &viewFront, &viewSide, &viewIso}) { t->setBounds(viewBar.removeFromLeft(54)); viewBar.removeFromLeft(2); }

    {
        auto row = panel.removeFromTop(28);
        exportBtn.setBounds(row.removeFromRight(70));
        row.removeFromRight(4);
        saveRoomBtn.setBounds(row.removeFromRight(84));
        row.removeFromRight(4);
        roomBox.setBounds(row);
    }
    panel.removeFromTop(6);
    statusLabel.setBounds(panel.removeFromTop(34));
    panel.removeFromTop(2);
    {
        auto row = panel.removeFromTop(28);
        const int w = row.getWidth() / 4;
        tabObjects.setBounds(row.removeFromLeft(w));
        tabSurfaces.setBounds(row.removeFromLeft(w));
        tabMix.setBounds(row.removeFromLeft(w));
        tabSpace.setBounds(row);
    }
    panel.removeFromTop(8);

    // objects: source list + setup, then the position sliders
    {
        auto a = panel;
        auto top = a.removeFromTop(26);
        removeSourceBtn.setBounds(top.removeFromRight(72));
        top.removeFromRight(4);
        addSourceBtn.setBounds(top.removeFromRight(72));
        top.removeFromRight(4);
        sourceBox.setBounds(top);
        const bool src = uiIsSource;
        if (src) header(0, a, "SETUP", 8);
        else a.removeFromTop(6);
        auto labeled = [&](juce::Label& l, juce::Component& c, int h = 24, int lw = 120) {
            auto r = src ? a.removeFromTop(h) : juce::Rectangle<int>();  // a listener has no source rows: no gap
            l.setBounds(src ? r.removeFromLeft(lw) : juce::Rectangle<int>());
            c.setBounds(src ? r : juce::Rectangle<int>());
            a.removeFromTop(src ? 3 : 0);
        };
        labeled(nameLabel, nameEdit);
        labeled(formatLabel, formatBox);
        if (uiRing) labeled(chLabel, chBox);
        else { chLabel.setBounds({}); chBox.setBounds({}); }
        labeled(firstInLabel, firstInBox);
        if (uiMulti) { auto r = a.removeFromTop(24); sumToggle.setBounds(src ? r : juce::Rectangle<int>()); a.removeFromTop(src ? 3 : 0); }
        else sumToggle.setBounds({});
        if (uiMulti && uiSpread) { auto r = a.removeFromTop(24); freeToggle.setBounds(src ? r : juce::Rectangle<int>()); a.removeFromTop(src ? 3 : 0); }
        else freeToggle.setBounds({});
        if (uiMulti && uiSpread) {
            auto r = a.removeFromTop(24);
            spreadRow.label.setBounds(src ? r.removeFromLeft(100) : juce::Rectangle<int>());
            spreadRow.slider.setBounds(src ? r : juce::Rectangle<int>());
            a.removeFromTop(3);
        } else { spreadRow.label.setBounds({}); spreadRow.slider.setBounds({}); }
        {
            auto r = src ? a.removeFromTop(24) : juce::Rectangle<int>();
            patternRow.label.setBounds(src ? r.removeFromLeft(100) : juce::Rectangle<int>());
            patternRow.slider.setBounds(src ? r : juce::Rectangle<int>());
            a.removeFromTop(src ? 3 : 0);
        }
        {
            auto r = src ? a.removeFromTop(24) : juce::Rectangle<int>();
            srcGainLabel.setBounds(src ? r.removeFromLeft(120) : juce::Rectangle<int>());
            srcGainSlider.setBounds(src ? r : juce::Rectangle<int>());
            a.removeFromTop(src ? 3 : 0);
        }
        infoLabel.setBounds(src ? a.removeFromTop(44) : juce::Rectangle<int>());
        header(0, a, "@SEL", src ? 6 : 2);
        selLabel.setBounds({});
        for (int k = 0; k < 4; ++k) {
            sliderLabels[(size_t)k].setBounds(a.removeFromTop(16));
            sliders[(size_t)k].setBounds(a.removeFromTop(24));
            a.removeFromTop(4);
        }
        if (selected == verbp::kListener) {  // output configuration of the listener
            header(0, a, "OUTPUT", 8);
            layoutLabel.setBounds(a.removeFromTop(16));
            layoutBox.setBounds(a.removeFromTop(26));
            a.removeFromTop(6);
            micLabel.setBounds(a.removeFromTop(16));
            micBox.setBounds(a.removeFromTop(26));
            a.removeFromTop(6);
            auto rr = a.removeFromTop(24);
            radiusRow.label.setBounds(rr.removeFromLeft(80));
            radiusRow.slider.setBounds(rr);
            a.removeFromTop(4);
            showMic.setBounds(a.removeFromTop(24));
            a.removeFromTop(6);
            outInfoLabel.setBounds(a.removeFromTop(110));
        } else {
            for (auto* c : std::initializer_list<juce::Component*>{&layoutBox, &layoutLabel, &micBox, &micLabel, &outInfoLabel, &radiusRow.label, &radiusRow.slider, &showMic}) c->setBounds({});
        }
    }
    // surfaces
    {
        auto a = panel;
        surfTitle.setBounds(a.removeFromTop(26));
        auto r = a.removeFromTop(26);
        selFloor.setBounds(r.removeFromLeft(r.getWidth() / 2 - 3));
        r.removeFromLeft(6);
        selCeiling.setBounds(r);
        a.removeFromTop(2);
        if (selKind == SelKind::Ceiling || selKind == SelKind::CeilZone) { openSky.setBounds(a.removeFromTop(24)); a.removeFromTop(2); }
        else openSky.setBounds({});
        header(1, a, "MATERIAL", 4);
        presetLabel.setBounds({});
        presetBox.setBounds(a.removeFromTop(24));
        a.removeFromTop(3);
        classLabel.setBounds({});
        classBox.setBounds(a.removeFromTop(24));
        auto layoutRow = [&](Row& row) {  // compact rows: the Surfaces page has the most controls
            auto rr = a.removeFromTop(20);
            row.label.setBounds(rr.removeFromLeft(112));
            row.slider.setBounds(rr);
        };
        header(1, a, "ABSORPTION");
        for (auto& row : absRows) layoutRow(row);
        header(1, a, "SCATTERING");
        for (auto& row : scRows) layoutRow(row);
        header(1, a, "TRANSMISSION");
        layoutRow(passRow);
        for (auto& row : tlRows) layoutRow(row);
        header(1, a, "EXTENT");
        if (selKind == SelKind::FloorZone || selKind == SelKind::CeilZone) {  // a zone has a plate height instead of a wall extent
            layoutRow(zoneZRow);
            wallBotRow.label.setBounds({}); wallBotRow.slider.setBounds({}); wallTopRow.label.setBounds({}); wallTopRow.slider.setBounds({});
        } else {
            layoutRow(wallBotRow);
            layoutRow(wallTopRow);
            zoneZRow.label.setBounds({}); zoneZRow.slider.setBounds({});
        }
        layoutRow(heightRow);
        header(1, a, "ROOM SCALE");
        {
            auto rr = a.removeFromTop(24);
            scaleRoomBtn.setBounds(rr.removeFromRight(84));
            rr.removeFromRight(6);
            scaleRow.label.setBounds(rr.removeFromLeft(100));
            scaleRow.slider.setBounds(rr);
        }
        a.removeFromTop(4);
        scaleObjects.setBounds(a.removeFromTop(24));
    }
    // space: air and clutter
    {
        auto a = panel;
        auto layoutRow = [&](Row& row) {
            auto rr = a.removeFromTop(22);
            row.label.setBounds(rr.removeFromLeft(112));
            row.slider.setBounds(rr);
            a.removeFromTop(1);
        };
        header(3, a, "AIR AND SURFACES", 2);
        layoutRow(tempRow);
        layoutRow(humRow);
        layoutRow(roughRow);
        diffToggle.setBounds(a.removeFromTop(24));
        a.removeFromTop(2);
        airInfo.setBounds(a.removeFromTop(34));
        header(3, a, "CLUTTER");
        layoutRow(cntRow);
        layoutRow(minLenRow);
        layoutRow(maxLenRow);
        layoutRow(tlMinRow);
        layoutRow(tlMaxRow);
        layoutRow(hMinRow);
        layoutRow(hMaxRow);
        {
            auto rr = a.removeFromTop(24);
            seedLabel.setBounds(rr.removeFromLeft(112));
            randomizeBtn.setBounds(rr.removeFromRight(86));
            rr.removeFromRight(4);
            seedSlider.setBounds(rr);
            a.removeFromTop(4);
        }
        showClutter.setBounds(a.removeFromTop(24));
        showMeters.setBounds(a.removeFromTop(24));
        a.removeFromTop(4);
        clutterInfo.setBounds(a.removeFromTop(70));
        versionLabel.setBounds(a.removeFromTop(18));
    }
    // mix
    {
        auto a = panel;
        header(2, a, "MIX", 2);
        for (auto* pr : {&mixLabel, &gainLabel, &tailLabel, &earlyLabel, &diffLabel, &dopplerLabel}) {
            pr->setBounds(a.removeFromTop(16));
            juce::Slider* s = pr == &mixLabel ? &mixSlider : pr == &gainLabel ? &gainSlider : pr == &tailLabel ? &tailSlider : pr == &earlyLabel ? &earlySlider : pr == &diffLabel ? &diffSlider : &dopplerSlider;
            s->setBounds(a.removeFromTop(24));
            a.removeFromTop(10);
        }
    }
}

// ---------- mouse ----------
juce::Point<double> VerbEditor::snapWorld(const juce::MouseEvent& e, const verb::Scene& sc, bool excludeDragged) const {
    double x, y;
    view.toWorld(e.position, x, y);
    float best = 10.f;
    bool found = false;
    juce::Point<double> res;
    for (size_t i = 0; i < sc.walls.size(); ++i)
        for (int end = 0; end < 2; ++end) {
            if (excludeDragged && std::find(dragEnds.begin(), dragEnds.end(), std::make_pair((int)i, end)) != dragEnds.end()) continue;
            const double px = end ? sc.walls[i].bx : sc.walls[i].ax, py = end ? sc.walls[i].by : sc.walls[i].ay;
            const float d = view.toScreen(px, py).getDistanceFrom(e.position);
            if (d < best) { best = d; res = {px, py}; found = true; }
        }
    if (found) return res;
    if (!e.mods.isAltDown()) { x = std::round(x * 10.0) / 10.0; y = std::round(y * 10.0) / 10.0; }
    return {x, y};
}

void VerbEditor::updateLayerButtons() {
    static const char* names[4] = {"Walls", "Floors", "Ceilings", "Plates"};
    for (int i = 0; i < 4; ++i) {
        layerBtn[i].setButtonText(juce::String(names[i]) + (layerMode[i] == 1 ? " (locked)" : layerMode[i] == 2 ? " (hidden)" : ""));
        layerBtn[i].setAlpha(layerMode[i] == 0 ? 1.f : (layerMode[i] == 1 ? 0.75f : 0.5f));
        layerBtn[i].setVisible(inPlan());
    }
}

std::vector<VerbEditor::Pick> VerbEditor::pickCandidates(const verb::Scene& sc, juce::Point<float> pos) {
    std::vector<Pick> out;
    if (layerMode[LayerWalls] == 0) {
        std::vector<std::pair<double, int>> near;
        for (size_t i = 0; i < sc.walls.size(); ++i) {
            const double d = distPointSeg(pos, view.toScreen(sc.walls[i].ax, sc.walls[i].ay), view.toScreen(sc.walls[i].bx, sc.walls[i].by));
            if (d < 7.0) near.push_back({d, (int)i});
        }
        std::sort(near.begin(), near.end());
        for (auto& n : near) out.push_back({SelKind::Wall, n.second});
    }
    double wx, wy;
    view.toWorld(pos, wx, wy);
    const int order[3] = {(int)LayerPlates, zoneLayerCeiling ? (int)LayerCeilings : (int)LayerFloors, zoneLayerCeiling ? (int)LayerFloors : (int)LayerCeilings};
    for (int layer : order) {
        if (layerMode[layer] != 0) continue;
        for (int pass = 0; pass < 2; ++pass) {
            const bool ceiling = pass == 1;
            const auto& zl = ceiling ? sc.ceilZones : sc.floorZones;
            for (int i = (int)zl.size() - 1; i >= 0; --i)
                if (zoneLayer(zl[(size_t)i], ceiling) == layer && zl[(size_t)i].poly.size() >= 3 && verb::pointInZone(zl[(size_t)i], wx, wy))
                    out.push_back({ceiling ? SelKind::CeilZone : SelKind::FloorZone, i});
        }
    }
    return out;
}

void VerbEditor::mouseDown(const juce::MouseEvent& e) {
    grabKeyboardFocus();
    if (!canvas.contains(e.getPosition())) return;
    if (e.mods.isMiddleButtonDown()) {  // middle button: pan the view, whatever is under the pointer
        panPending = true;
        panStart = e.position;
        panCx0 = view.minX + view.area.getWidth() / (2 * view.scale);
        panCy0 = view.minY + view.area.getHeight() / (2 * view.scale);
        return;
    }
    if (e.mods.isRightButtonDown()) {  // right click / drag: turn the selected object towards the pointer
        if (selKind == SelKind::Object && inPlan()) {
            rightDragging = true;
            if (auto* pr = proc.apvts.getParameter(verbp::paramId(selected, 3))) pr->beginChangeGesture();
            double x, y;
            view.toWorld(e.position, x, y);
            proc.orientObject(selected, x, y);
            repaint();
        }
        return;
    }
    verb::Scene sc = proc.getDisplayScene();
    dragMode = DragMode::None;
    if (plane == Plane::Iso) {  // 3D: drag orbits the camera
        orbiting = true;
        orbitStart = e.position;
        orbitYaw0 = camYaw;
        orbitPitch0 = camPitch;
        return;
    }
    if (!inPlan()) { mouseDownElevation(e); return; }

    if (tool == Tool::BoxSelect) {  // rubber-band selection of walls
        boxStart = boxEnd = e.position;
        dragMode = DragMode::Box;
        lockView = true;
        return;
    }
    if (tool == Tool::AddFloorZone || tool == Tool::AddCeilZone) {  // click the corners; the first corner closes the zone
        const auto p = snapWorld(e, sc, false);
        if (zonePts.size() >= 3 && view.toScreen(zonePts[0].x, zonePts[0].y).getDistanceFrom(e.position) < 10.f) { finishZone(); return; }
        zonePts.push_back(p);
        repaint();
        return;
    }
    if (tool == Tool::AddWall) {
        if ((int)sc.walls.size() >= kMaxWalls) return;
        addStart = addEnd = snapWorld(e, sc, false);
        dragMode = DragMode::NewWall;
        lockView = true;
        return;
    }

    // channel points of free-placement sources (each one is an independent, draggable point)
    {
        float bd = 11.f;
        int fs = -1, fc = -1;
        for (int s = 0; s < (int)cfgCache.size(); ++s) {
            const verbp::SourceCfg& c = cfgCache[(size_t)s];
            if (!c.freePoints || c.effectiveChannels() <= 1 || c.sum) continue;
            const double az = paramValue(s, 3) * verb::kPi / 180.0, ca = std::cos(az), sa = std::sin(az);
            for (int ch = 0; ch < c.effectiveChannels() && ch < (int)c.ox.size() && ch < (int)c.oy.size(); ++ch) {
                if (ch == c.lfeChannel()) continue;
                const auto p = view.toScreen(paramValue(s, 0) + ca * c.ox[(size_t)ch] - sa * c.oy[(size_t)ch], paramValue(s, 1) + sa * c.ox[(size_t)ch] + ca * c.oy[(size_t)ch]);
                const float d = p.getDistanceFrom(e.position);
                if (d < bd) { bd = d; fs = s; fc = ch; }
            }
        }
        if (fs >= 0) {
            dragMode = DragMode::FreePoint;
            freeSrc = fs;
            freeCh = fc;
            selectObject(fs);
            showTab(0);
            return;
        }
    }

    // sources and listener first
    int best = -1;
    float bestD = 18.f;
    auto test = [&](int obj) {
        const float d = view.toScreen(paramValue(obj, 0), paramValue(obj, 1)).getDistanceFrom(e.position);
        if (d < bestD) { bestD = d; best = obj; }
    };
    for (int s = 0; s < (int)cfgCache.size(); ++s) test(s);
    test(verbp::kListener);
    if (best >= 0) {
        dragMode = DragMode::Object;
        dragging = best;
        dragParamA = 0;
        dragParamB = 1;
        selectObject(best);
        showTab(0);
        for (int k : {dragParamA, dragParamB})
            if (auto* pr = proc.apvts.getParameter(verbp::paramId(best, k))) pr->beginChangeGesture();
        return;
    }

    // Shift+click on a wall: add it to / remove it from the selection (edit several walls at once)
    if (e.mods.isShiftDown() && layerMode[LayerWalls] == 0) {
        int hit = -1;
        double hd = 8.0;
        for (size_t i = 0; i < sc.walls.size(); ++i) {
            const double d = distPointSeg(e.position, view.toScreen(sc.walls[i].ax, sc.walls[i].ay), view.toScreen(sc.walls[i].bx, sc.walls[i].by));
            if (d < hd) { hd = d; hit = (int)i; }
        }
        if (hit >= 0) {
            if (selKind != SelKind::Wall) { selWalls.clear(); }
            auto it = std::find(selWalls.begin(), selWalls.end(), hit);
            if (it == selWalls.end()) selWalls.push_back(hit); else selWalls.erase(it);
            selKind = selWalls.empty() ? SelKind::Object : SelKind::Wall;
            selWall = selWalls.empty() ? -1 : selWalls.back();
            showTab(1);
            refreshSurface();
            resized();
            repaint();
            return;
        }
    }

    // corners of the selected zone
    if (selKind == SelKind::FloorZone || selKind == SelKind::CeilZone) {
        const auto& zl = selKind == SelKind::FloorZone ? sc.floorZones : sc.ceilZones;
        if (selWall >= 0 && selWall < (int)zl.size() && layerMode[zoneLayer(zl[(size_t)selWall], selKind == SelKind::CeilZone)] == 0) {
            const auto& poly = zl[(size_t)selWall].poly;
            for (size_t i = 0; i < poly.size(); ++i)
                if (view.toScreen(poly[i].first, poly[i].second).getDistanceFrom(e.position) < 11.f) {
                    pushUndo();
                    dragMode = DragMode::ZoneVertex;
                    zoneVertex = (int)i;
                    lockView = true;
                    return;
                }
        }
    }

    // wall corners
    int bw = -1, bend = 0;
    float bd = 11.f;
    for (size_t i = 0; i < sc.walls.size() && layerMode[LayerWalls] == 0; ++i)
        for (int end = 0; end < 2; ++end) {
            const float d = view.toScreen(end ? sc.walls[i].bx : sc.walls[i].ax, end ? sc.walls[i].by : sc.walls[i].ay).getDistanceFrom(e.position);
            if (d < bd) { bd = d; bw = (int)i; bend = end; }
        }
    if (bw >= 0) {
        const double cx = bend ? sc.walls[(size_t)bw].bx : sc.walls[(size_t)bw].ax, cy = bend ? sc.walls[(size_t)bw].by : sc.walls[(size_t)bw].ay;
        dragEnds.clear();
        for (size_t i = 0; i < sc.walls.size(); ++i)
            for (int end = 0; end < 2; ++end)
                if (std::hypot((end ? sc.walls[i].bx : sc.walls[i].ax) - cx, (end ? sc.walls[i].by : sc.walls[i].ay) - cy) < 1e-4) dragEnds.push_back({(int)i, end});
        pushUndo();
        dragMode = DragMode::Vertex;
        lockView = true;
        selectSurface(SelKind::Wall, bw);
        return;
    }

    // wall bodies and zones under the pointer. The first press selects the top one; a press on the selected one that
    // does not move (see mouseUp) steps to the next surface that lies under the same point.
    {
        const auto cands = pickCandidates(sc, e.position);
        pickPending = false;
        if (cands.empty()) {  // empty space: dragging pans the view (a plain click does nothing)
            panPending = true;
            panStart = e.position;
            panCx0 = view.minX + view.area.getWidth() / (2 * view.scale);
            panCy0 = view.minY + view.area.getHeight() / (2 * view.scale);
            return;
        }
        int cur = 0;
        bool wasSel = false;
        for (size_t i = 0; i < cands.size(); ++i) {
            const bool isSel = cands[i].kind == SelKind::Wall ? (selKind == SelKind::Wall && selWall == cands[i].idx) : (selKind == cands[i].kind && selWall == cands[i].idx);
            if (isSel) { cur = (int)i; wasSel = true; break; }
        }
        pickList = cands;
        pickCur = cur;
        pickWasSelected = wasSel;
        pickDownPos = e.position;
        pickPending = cands.size() > 1;
        const Pick pk = cands[(size_t)cur];
        if (pk.kind == SelKind::Wall) {
            pushUndo();
            dragMode = DragMode::Wall;
            lockView = true;
            view.toWorld(e.position, dragStartX, dragStartY);
            const auto& w = sc.walls[(size_t)pk.idx];
            dragOrig = {w.ax, w.ay, w.bx, w.by};
            selectSurface(SelKind::Wall, pk.idx);
        } else {
            const bool ceiling = pk.kind == SelKind::CeilZone;
            const auto& zl = ceiling ? sc.ceilZones : sc.floorZones;
            double wx, wy;
            view.toWorld(e.position, wx, wy);
            zoneLayerCeiling = ceiling;
            if (selKind == pk.kind && selWall == pk.idx) {  // a second press on the selected one moves it
                pushUndo();
                dragMode = DragMode::ZoneMove;
                lockView = true;
                dragStartX = wx;
                dragStartY = wy;
                zoneOrig = zl[(size_t)pk.idx].poly;
            } else {
                selectSurface(pk.kind, pk.idx);
            }
        }
    }
}

// ---------- zones, planes ----------
void VerbEditor::setPlane(Plane p) {
    plane = p;
    viewTop.setToggleState(p == Plane::Top, juce::dontSendNotification);
    viewFront.setToggleState(p == Plane::Front, juce::dontSendNotification);
    viewSide.setToggleState(p == Plane::Side, juce::dontSendNotification);
    viewIso.setToggleState(p == Plane::Iso, juce::dontSendNotification);
    if (p != Plane::Top) {  // drawing tools work on the plan only
        tool = Tool::Move;
        toolMove.setToggleState(true, juce::dontSendNotification);
        zonePts.clear();
    }
    dragMode = DragMode::None;
    lockView = false;
    setAutoView(true);
    updateLayerButtons();
    repaint();
}

void VerbEditor::mouseMove(const juce::MouseEvent& e) {
    hoverPos = e.position;
    if (!zonePts.empty()) repaint();
}

void VerbEditor::finishZone() {
    std::vector<juce::Point<double>> pts;
    for (auto& p : zonePts)  // a double-click adds the last corner twice
        if (pts.empty() || std::hypot(p.x - pts.back().x, p.y - pts.back().y) > 0.02) pts.push_back(p);
    const bool ceiling = tool == Tool::AddCeilZone;
    zonePts.clear();
    if (pts.size() < 3) { repaint(); return; }
    pushUndo();
    int newIndex = -1;
    proc.editScene([&](verb::Scene& s) {
        verb::Material m = verb::surfacePresets()[2];  // Plaster for a ceiling zone
        if (!ceiling)
            for (const auto& pr : verb::surfacePresets())
                if (juce::String(pr.name).containsIgnoreCase("carpet")) { m = pr; break; }  // a floor zone starts as carpet: easy to hear and see
        s.mats.push_back(m);
        verb::Zone z;
        z.mat = (int)s.mats.size() - 1;
        for (auto& p : pts) z.poly.push_back({p.x, p.y});
        auto& list = ceiling ? s.ceilZones : s.floorZones;
        list.push_back(z);
        newIndex = (int)list.size() - 1;
    });
    tool = Tool::Move;
    toolMove.setToggleState(true, juce::dontSendNotification);
    const auto sc = proc.getDisplayScene();
    const auto& list = ceiling ? sc.ceilZones : sc.floorZones;
    if (newIndex >= 0 && newIndex < (int)list.size()) selectSurface(ceiling ? SelKind::CeilZone : SelKind::FloorZone, newIndex);
    repaint();
}

void VerbEditor::mouseDownElevation(const juce::MouseEvent& e) {
    const verb::Scene sc = proc.getDisplayScene();
    const double H = sc.height;
    // sources and listener: drag changes the horizontal position and the height
    int best = -1;
    float bestD = 18.f;
    auto test = [&](int obj) {
        const float d = view.toScreen(horiz(paramValue(obj, 0), paramValue(obj, 1)), paramValue(obj, 2)).getDistanceFrom(e.position);
        if (d < bestD) { bestD = d; best = obj; }
    };
    for (int s = 0; s < (int)cfgCache.size(); ++s) test(s);
    test(verbp::kListener);
    if (best >= 0) {
        dragMode = DragMode::Object;
        dragging = best;
        dragParamA = horizParam();
        dragParamB = 2;
        selectObject(best);
        showTab(0);
        for (int k : {dragParamA, dragParamB})
            if (auto* pr = proc.apvts.getParameter(verbp::paramId(best, k))) pr->beginChangeGesture();
        return;
    }
    // wall rectangles: the top and bottom edges drag the wall's vertical extent, the body selects it
    int hit = -1, edge = 0;
    for (int i = (int)sc.walls.size() - 1; i >= 0 && hit < 0; --i) {
        const auto& w = sc.walls[(size_t)i];
        const double ua = horiz(w.ax, w.ay), ub = horiz(w.bx, w.by);
        const double zb = std::max(0.0, w.z0), zt = std::min(w.z1, H);
        const auto tl = view.toScreen(std::min(ua, ub), zt), br = view.toScreen(std::max(ua, ub), zb);
        juce::Rectangle<float> r(tl, br);
        r = r.withSizeKeepingCentre(std::max(r.getWidth(), 8.f), r.getHeight());
        if (!r.expanded(0.f, 6.f).contains(e.position)) continue;
        hit = i;
        if (std::fabs(e.position.y - r.getY()) < 6.f && zt < H + 1e-6) edge = 1;
        else if (std::fabs(e.position.y - r.getBottom()) < 6.f) edge = 2;
    }
    if (hit >= 0) {
        pushUndo();
        selectSurface(SelKind::Wall, hit);
        if (edge) { dragMode = edge == 1 ? DragMode::WallTop : DragMode::WallBottom; lockView = true; }
        return;
    }
    // floor / ceiling lines (and the zones lying on them)
    auto onLine = [&](double z) { return std::fabs(view.toScreen(0, z).y - e.position.y) < 7.f; };
    double u, zz;
    view.toWorld(e.position, u, zz);
    for (int pass = 0; pass < 2; ++pass) {
        const bool ceiling = pass == 0;
        if (!onLine(ceiling ? H : 0.0)) continue;
        const auto& zl = ceiling ? sc.ceilZones : sc.floorZones;
        for (int i = (int)zl.size() - 1; i >= 0; --i) {
            double lo = 1e18, hi = -1e18;
            for (auto& p : zl[(size_t)i].poly) { const double v = horiz(p.first, p.second); lo = std::min(lo, v); hi = std::max(hi, v); }
            if (u >= lo && u <= hi) { selectSurface(ceiling ? SelKind::CeilZone : SelKind::FloorZone, i); return; }
        }
        selectSurface(ceiling ? SelKind::Ceiling : SelKind::Floor, -1);
        return;
    }
    panPending = true;
    panStart = e.position;
    panCx0 = view.minX + view.area.getWidth() / (2 * view.scale);
    panCy0 = view.minY + view.area.getHeight() / (2 * view.scale);
}

void VerbEditor::paintElevation(juce::Graphics& g, const verb::Scene& sc, const double* lis) {
    (void)lis;
    const double H = sc.height;
    const auto classColour = [&](int mat) {
        return mat >= 0 && mat < (int)sc.mats.size() ? juce::Colour(verb::surfaceClasses()[(size_t)std::min(std::max(sc.mats[(size_t)mat].cls, 0), (int)verb::SC_Custom)].argb) : kWall;
    };
    const float x0 = view.area.getX(), x1 = view.area.getRight();
    // floor and ceiling
    const float yf = view.toScreen(0, 0).y, yc = view.toScreen(0, H).y;
    g.setColour(classColour(sc.floorMat).withAlpha(0.9f));
    g.drawLine(x0, yf, x1, yf, selKind == SelKind::Floor ? 2.4f : 1.4f);
    if (sc.ceilMat >= 0) {
        g.setColour(classColour(sc.ceilMat).withAlpha(0.9f));
        g.drawLine(x0, yc, x1, yc, selKind == SelKind::Ceiling ? 2.4f : 1.4f);
    } else {
        const float dash[2] = {4.f, 8.f};
        g.setColour(juce::Colours::grey);
        g.drawDashedLine(juce::Line<float>(x0, yc, x1, yc), dash, 2, 1.f);
    }
    if (selKind == SelKind::Floor) { g.setColour(kSel); g.drawLine(x0, yf, x1, yf, 2.4f); }
    if (selKind == SelKind::Ceiling) { g.setColour(kSel); g.drawLine(x0, yc, x1, yc, 2.4f); }
    // zones: a bar along the line over the zone's extent
    for (int pass = 0; pass < 2; ++pass) {
        const bool ceiling = pass == 1;
        const auto& zl = ceiling ? sc.ceilZones : sc.floorZones;
        for (size_t i = 0; i < zl.size(); ++i) {
            double lo = 1e18, hi = -1e18;
            for (auto& p : zl[i].poly) { const double v = horiz(p.first, p.second); lo = std::min(lo, v); hi = std::max(hi, v); }
            const float sx0 = view.toScreen(lo, 0).x, sx1 = view.toScreen(hi, 0).x, y = zl[i].z >= 0 ? view.toScreen(0, zl[i].z).y : (ceiling ? yc + 6.f : yf - 6.f);  // a plate sits at its own height
            const bool sel = selKind == (ceiling ? SelKind::CeilZone : SelKind::FloorZone) && selWall == (int)i;
            if (zl[i].mat < 0) {  // opening
                g.setColour(sel ? kSel : juce::Colours::black);
                g.fillRect(sx0, yc - 3.f, sx1 - sx0, 6.f);
                g.setColour(juce::Colours::grey);
                g.drawRect(sx0, yc - 3.f, sx1 - sx0, 6.f, 1.f);
            } else {
                g.setColour(sel ? kSel : classColour(zl[i].mat));
                g.fillRect(sx0, y - 2.5f, sx1 - sx0, 5.f);
            }
        }
    }
    // walls as rectangles over their horizontal extent and height range
    for (size_t i = 0; i < sc.walls.size(); ++i) {
        const auto& w = sc.walls[i];
        const double ua = horiz(w.ax, w.ay), ub = horiz(w.bx, w.by);
        const double zb = std::max(0.0, w.z0), zt = std::min(w.z1, H);
        const auto tl = view.toScreen(std::min(ua, ub), zt), br = view.toScreen(std::max(ua, ub), zb);
        juce::Rectangle<float> r(tl, br);
        r = r.withSizeKeepingCentre(std::max(r.getWidth(), 4.f), r.getHeight());
        const bool sel = wallSelected((int)i);
        const double t = w.mat >= 0 && w.mat < (int)sc.mats.size() ? meanOf(sc.mats[(size_t)w.mat].tau) : 0.0;
        const juce::Colour c = sel ? kSel : classColour(w.mat);
        g.setColour(c.withAlpha(t > 0.02 ? 0.035f : (sel ? 0.12f : 0.07f)));  // a light veil: the outline carries the shape
        g.fillRect(r);
        g.setColour(c.withAlpha(sel ? 1.f : 0.55f));
        g.drawRect(r, sel ? 1.8f : 1.f);
        if (sel) {
            g.setFont(12.f);
            g.drawText(juce::String(zb, 2) + " - " + juce::String(zt, 2) + " m", (int)r.getX() - 30, (int)r.getY() - 16, (int)r.getWidth() + 60, 14, juce::Justification::centred);
        }
    }
    // sources and listener at their heights
    auto drawObj = [&](int obj, juce::Colour c, const juce::String& label) {
        const auto p = view.toScreen(horiz(paramValue(obj, 0), paramValue(obj, 1)), paramValue(obj, 2));
        const bool sel = selKind == SelKind::Object && obj == selected;
        const float r = sel ? 6.f : 5.f;
        g.setColour(c.withAlpha(sel ? 0.22f : 0.14f));
        g.fillEllipse(p.x - r * 2.2f, p.y - r * 2.2f, r * 4.4f, r * 4.4f);
        g.setColour(c);
        g.fillEllipse(p.x - r, p.y - r, 2 * r, 2 * r);
        if (sel) { g.setColour(c.withAlpha(0.9f)); g.drawEllipse(p.x - r - 3, p.y - r - 3, 2 * r + 6, 2 * r + 6, 1.f); }
        g.setColour(c.withAlpha(0.35f));
        g.drawLine(p.x, p.y + r, p.x, yf, 1.f);  // drop line to the floor
        g.setColour(sel ? skin::text : skin::textMid);
        g.setFont(juce::Font(juce::FontOptions(12.f)));
        g.drawText(label + "  " + juce::String(paramValue(obj, 2), 2) + " m", (int)p.x - 70, (int)p.y + 11, 140, 15, juce::Justification::centred);
    };
    for (int s = 0; s < (int)cfgCache.size(); ++s) drawObj(s, srcColour(s), juce::String(cfgCache[(size_t)s].name));
    drawObj(verbp::kListener, kLis, "Listener");
    g.setColour(juce::Colours::grey);
    g.setFont(12.f);
    g.drawText(plane == Plane::Front ? "FRONT view: x across, height up" : "SIDE view: y across, height up", (int)x0 + 6, (int)view.area.getY() + 2, 260, 14, juce::Justification::left);
}

// A light 3D view: perspective orbit camera, walls as veils with their height range, zones on the floor and ceiling,
// sources and listener with drop lines. Read-only; the editing happens in the plan and the elevations.
void VerbEditor::paint3D(juce::Graphics& g, const verb::Scene& sc, const double* lis) {
    (void)lis;
    const double H = sc.height;
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    auto grow = [&](double x, double y) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); };
    for (auto& w : sc.walls) { grow(w.ax, w.ay); grow(w.bx, w.by); }
    for (auto* zl : {&sc.floorZones, &sc.ceilZones})
        for (auto& z : *zl) for (auto& p : z.poly) grow(p.first, p.second);
    for (int s = 0; s < (int)cfgCache.size(); ++s) grow(paramValue(s, 0), paramValue(s, 1));
    grow(paramValue(verbp::kListener, 0), paramValue(verbp::kListener, 1));
    if (x1 < x0) { x0 = y0 = -5; x1 = y1 = 5; }
    const double cx = 0.5 * (x0 + x1), cy = 0.5 * (y0 + y1), cz = 0.5 * H;
    const double radius = 0.5 * std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + H * H) + 0.5;
    const auto area = canvas.reduced(10).toFloat();
    const double sc0 = std::min(area.getWidth(), area.getHeight()) / (2.2 * radius) * camZoom;
    const double cyw = std::cos(camYaw), syw = std::sin(camYaw), cp = std::cos(camPitch), sp = std::sin(camPitch);
    const double D = 3.2 * radius;
    struct P2 { float x, y; double depth; };
    auto proj = [&](double x, double y, double z) {
        const double px = x - cx, py = y - cy, pz = z - cz;
        const double x1r = cyw * px - syw * py, y1r = syw * px + cyw * py;
        const double y2 = cp * y1r - sp * pz, z2 = sp * y1r + cp * pz;
        const double f = D / (D + y2);
        return P2{(float)(area.getCentreX() + x1r * sc0 * f), (float)(area.getCentreY() - z2 * sc0 * f), y2};
    };
    const auto classColour = [&](int mat) {
        return mat >= 0 && mat < (int)sc.mats.size() ? juce::Colour(verb::surfaceClasses()[(size_t)std::min(std::max(sc.mats[(size_t)mat].cls, 0), (int)verb::SC_Custom)].argb) : skin::textMid;
    };
    // floor: a faint slab with a 1 m grid, then the zones
    {
        const double gx0 = std::floor(x0) - 1, gx1 = std::ceil(x1) + 1, gy0 = std::floor(y0) - 1, gy1 = std::ceil(y1) + 1;
        juce::Path floorP;
        const auto a = proj(gx0, gy0, 0), b = proj(gx1, gy0, 0), c = proj(gx1, gy1, 0), d = proj(gx0, gy1, 0);
        floorP.startNewSubPath(a.x, a.y);
        floorP.lineTo(b.x, b.y);
        floorP.lineTo(c.x, c.y);
        floorP.lineTo(d.x, d.y);
        floorP.closeSubPath();
        g.setColour(classColour(sc.floorMat).withAlpha(0.05f));
        g.fillPath(floorP);
        g.setColour(skin::gridLine);
        const double step = (gx1 - gx0 > 60 || gy1 - gy0 > 60) ? 5.0 : 1.0;
        for (double x = gx0; x <= gx1 + 1e-6; x += step) { const auto p = proj(x, gy0, 0), q = proj(x, gy1, 0); g.drawLine(p.x, p.y, q.x, q.y, 0.8f); }
        for (double y = gy0; y <= gy1 + 1e-6; y += step) { const auto p = proj(gx0, y, 0), q = proj(gx1, y, 0); g.drawLine(p.x, p.y, q.x, q.y, 0.8f); }
    }
    auto drawZones = [&](const std::vector<verb::Zone>& zl, double z, bool ceiling) {
        for (size_t i = 0; i < zl.size(); ++i) {
            if (zl[i].poly.size() < 3) continue;
            juce::Path p;
            for (size_t k = 0; k < zl[i].poly.size(); ++k) {
                const auto q = proj(zl[i].poly[k].first, zl[i].poly[k].second, zl[i].z >= 0 ? zl[i].z : z);  // a plate has its own height
                if (k == 0) p.startNewSubPath(q.x, q.y); else p.lineTo(q.x, q.y);
            }
            p.closeSubPath();
            const bool sel = selKind == (ceiling ? SelKind::CeilZone : SelKind::FloorZone) && selWall == (int)i;
            const juce::Colour c = zl[i].mat < 0 ? skin::textDim : classColour(zl[i].mat);
            g.setColour(c.withAlpha(ceiling ? 0.06f : 0.16f));
            g.fillPath(p);
            g.setColour(sel ? kSel : c.withAlpha(0.8f));
            if (ceiling) {
                const float dash[2] = {5.f, 4.f};
                juce::Path dp;
                juce::PathStrokeType(1.f).createDashedStroke(dp, p, dash, 2);
                g.fillPath(dp);
            } else {
                g.strokePath(p, juce::PathStrokeType(sel ? 1.8f : 1.f));
            }
        }
    };
    drawZones(sc.floorZones, 0.0, false);
    // walls back to front
    struct WallQ { int i; double depth; };
    std::vector<WallQ> order;
    for (int i = 0; i < (int)sc.walls.size(); ++i) {
        const auto& w = sc.walls[(size_t)i];
        order.push_back({i, proj(0.5 * (w.ax + w.bx), 0.5 * (w.ay + w.by), 0.5 * H).depth});
    }
    std::sort(order.begin(), order.end(), [](const WallQ& a, const WallQ& b) { return a.depth > b.depth; });
    for (auto& o : order) {
        const auto& w = sc.walls[(size_t)o.i];
        const double zb = std::max(0.0, w.z0), zt = std::min(w.z1, H);
        const auto a = proj(w.ax, w.ay, zb), b = proj(w.bx, w.by, zb), c = proj(w.bx, w.by, zt), d = proj(w.ax, w.ay, zt);
        juce::Path q;
        q.startNewSubPath(a.x, a.y);
        q.lineTo(b.x, b.y);
        q.lineTo(c.x, c.y);
        q.lineTo(d.x, d.y);
        q.closeSubPath();
        const bool sel = wallSelected(o.i);
        const double t = w.mat >= 0 && w.mat < (int)sc.mats.size() ? meanOf(sc.mats[(size_t)w.mat].tau) : 0.0;
        const juce::Colour col = sel ? kSel : classColour(w.mat);
        g.setColour(col.withAlpha(t > 0.02 ? 0.04f : (sel ? 0.20f : 0.09f)));
        g.fillPath(q);
        g.setColour(col.withAlpha(sel ? 1.f : 0.6f));
        g.strokePath(q, juce::PathStrokeType(sel ? 1.8f : 1.f));
    }
    if (sc.ceilMat >= 0 || !sc.ceilZones.empty()) {  // the ceiling outline and its zones
        if (sc.ceilMat >= 0) {
            const auto a = proj(x0, y0, H), b = proj(x1, y0, H), c = proj(x1, y1, H), d = proj(x0, y1, H);
            juce::Path p;
            p.startNewSubPath(a.x, a.y);
            p.lineTo(b.x, b.y);
            p.lineTo(c.x, c.y);
            p.lineTo(d.x, d.y);
            p.closeSubPath();
            const float dash[2] = {3.f, 5.f};
            juce::Path dp;
            juce::PathStrokeType(1.f).createDashedStroke(dp, p, dash, 2);
            g.setColour(skin::violet.withAlpha(0.5f));
            g.fillPath(dp);
        }
        drawZones(sc.ceilZones, H, true);
    }
    // sources and listener
    auto drawObj = [&](int obj, juce::Colour c, const juce::String& label) {
        const double x = paramValue(obj, 0), y = paramValue(obj, 1), z = paramValue(obj, 2);
        const auto p = proj(x, y, z), f = proj(x, y, 0);
        const bool sel = selKind == SelKind::Object && obj == selected;
        g.setColour(c.withAlpha(0.35f));
        g.drawLine(p.x, p.y, f.x, f.y, 1.f);
        g.fillEllipse(f.x - 2.5f, f.y - 1.5f, 5.f, 3.f);
        const float r = sel ? 6.f : 5.f;
        g.setColour(c.withAlpha(0.2f));
        g.fillEllipse(p.x - r * 2.2f, p.y - r * 2.2f, r * 4.4f, r * 4.4f);
        g.setColour(c);
        g.fillEllipse(p.x - r, p.y - r, 2 * r, 2 * r);
        g.setColour(sel ? skin::text : skin::textMid);
        g.setFont(juce::Font(juce::FontOptions(12.f)));
        g.drawText(label, (int)p.x - 50, (int)p.y + 9, 100, 15, juce::Justification::centred);
    };
    for (int s = 0; s < (int)cfgCache.size(); ++s) drawObj(s, srcColour(s), juce::String(cfgCache[(size_t)s].name));
    drawObj(verbp::kListener, kLis, "Listener");
    g.setColour(skin::textDim);
    g.setFont(juce::Font(juce::FontOptions(11.5f)));
    g.drawText("3D", (int)area.getX() + 6, (int)area.getY() + 2, 60, 14, juce::Justification::left);
}

void VerbEditor::setAutoView(bool on) {
    autoFit = on;
    autoViewBtn.setToggleState(on, juce::dontSendNotification);
    repaint();
}

void VerbEditor::zoomView(double factor) {
    if (autoFit) setAutoView(false);
    vScale = std::min(400.0, std::max(0.5, vScale * factor));
    repaint();
}

void VerbEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    if (!canvas.contains(e.getPosition())) return;
    if (plane == Plane::Iso) {
        camZoom = std::min(8.0, std::max(0.25, camZoom * std::pow(1.2, wheel.deltaY * 10.0)));
        repaint();
        return;
    }
    if (autoFit) setAutoView(false);  // the wheel takes over the view; vCx/vCy/vScale hold the fitted one
    const double wx = vCx + (e.position.x - view.area.getCentreX()) / vScale;  // the point under the pointer stays put
    const double wy = vCy - (e.position.y - view.area.getCentreY()) / vScale;
    vScale = std::min(400.0, std::max(0.5, vScale * std::pow(1.25, wheel.deltaY * 10.0)));
    vCx = wx - (e.position.x - view.area.getCentreX()) / vScale;
    vCy = wy + (e.position.y - view.area.getCentreY()) / vScale;
    repaint();
}

void VerbEditor::mouseDoubleClick(const juce::MouseEvent& e) {
    if (plane == Plane::Iso) { camYaw = -0.75; camPitch = 0.5; camZoom = 1.0; repaint(); return; }
    if (!zonePts.empty()) { finishZone(); return; }  // double-click closes the zone being drawn
    if (canvas.contains(e.getPosition()) && !e.mods.isAnyModifierKeyDown()) setAutoView(true);  // back to following the objects
}

void VerbEditor::mouseDrag(const juce::MouseEvent& e) {
    if (orbiting) {
        camYaw = orbitYaw0 - (e.position.x - orbitStart.x) * 0.008;
        camPitch = std::min(1.5, std::max(-0.2, orbitPitch0 + (e.position.y - orbitStart.y) * 0.008));
        repaint();
        return;
    }
    if (dragMode == DragMode::Box) {
        boxEnd = e.position;
        repaint();
        return;
    }
    if (panPending) {
        const auto d = e.position - panStart;
        if (!panning && std::hypot(d.x, d.y) < 3.f) return;
        if (!panning) { panning = true; setAutoView(false); }
        vCx = panCx0 - d.x / vScale;
        vCy = panCy0 + d.y / vScale;
        repaint();
        return;
    }
    if (rightDragging) {
        double x, y;
        view.toWorld(e.position, x, y);
        proc.orientObject(selected, x, y);
        repaint();
        return;
    }
    if (dragMode == DragMode::FreePoint && freeSrc >= 0 && freeSrc < (int)cfgCache.size()) {
        double x, y;
        view.toWorld(e.position, x, y);
        const double az = paramValue(freeSrc, 3) * verb::kPi / 180.0, ca = std::cos(az), sa = std::sin(az);
        const double dx = x - paramValue(freeSrc, 0), dy = y - paramValue(freeSrc, 1);
        verbp::SourceCfg& c = cfgCache[(size_t)freeSrc];
        const float oz = freeCh < (int)c.oz.size() ? c.oz[(size_t)freeCh] : 0.f;
        const float ox = (float)(dx * ca + dy * sa), oy = (float)(-dx * sa + dy * ca);
        proc.setPointOffset(freeSrc, freeCh, ox, oy, oz);
        if (freeCh < (int)c.ox.size() && freeCh < (int)c.oy.size()) { c.ox[(size_t)freeCh] = ox; c.oy[(size_t)freeCh] = oy; }
        repaint();
        return;
    }
    if (dragMode == DragMode::Object && dragging >= 0) {
        double x, y;
        view.toWorld(e.position, x, y);  // free placement: inside, outside or beside the walls
        if (inPlan()) {
            setParam(verbp::paramId(dragging, 0), x);
            setParam(verbp::paramId(dragging, 1), y);
        } else {  // elevation: (x or y, height)
            setParam(verbp::paramId(dragging, dragParamA), x);
            setParam(verbp::paramId(dragging, 2), std::max(0.0, y));
        }
        repaint();
    } else if (dragMode == DragMode::WallTop || dragMode == DragMode::WallBottom) {
        double u, z;
        view.toWorld(e.position, u, z);
        z = std::round(z * 20.0) / 20.0;
        const int wi = selWall;
        const bool top = dragMode == DragMode::WallTop;
        proc.editScene([wi, z, top](verb::Scene& s) {
            if (wi < 0 || wi >= (int)s.walls.size()) return;
            auto& w = s.walls[(size_t)wi];
            const double zt = std::min(w.z1, s.height);
            if (top) w.z1 = z >= s.height - 0.025 ? 1e9 : std::max(w.z0 + 0.05, z);  // the ceiling = full height
            else w.z0 = std::min(std::max(0.0, z), zt - 0.05);
        });
        refreshSurface();
        repaint();
    } else if (dragMode == DragMode::ZoneVertex || dragMode == DragMode::ZoneMove) {
        verb::Scene sc = proc.getDisplayScene();
        const bool ceiling = selKind == SelKind::CeilZone;
        const int zi = selWall;
        if (dragMode == DragMode::ZoneVertex) {
            const auto p = snapWorld(e, sc, false);
            const int vi = zoneVertex;
            proc.editScene([p, vi, ceiling, zi](verb::Scene& s) {
                auto& zl = ceiling ? s.ceilZones : s.floorZones;
                if (zi < 0 || zi >= (int)zl.size() || vi < 0 || vi >= (int)zl[(size_t)zi].poly.size()) return;
                zl[(size_t)zi].poly[(size_t)vi] = {p.x, p.y};
            });
        } else {
            double x, y;
            view.toWorld(e.position, x, y);
            double dx = x - dragStartX, dy = y - dragStartY;
            if (!e.mods.isAltDown()) { dx = std::round(dx * 10.0) / 10.0; dy = std::round(dy * 10.0) / 10.0; }
            const auto orig = zoneOrig;
            proc.editScene([dx, dy, ceiling, zi, orig](verb::Scene& s) {
                auto& zl = ceiling ? s.ceilZones : s.floorZones;
                if (zi < 0 || zi >= (int)zl.size() || zl[(size_t)zi].poly.size() != orig.size()) return;
                for (size_t k = 0; k < orig.size(); ++k) zl[(size_t)zi].poly[k] = {orig[k].first + dx, orig[k].second + dy};
            });
        }
        repaint();
    } else if (dragMode == DragMode::NewWall) {
        verb::Scene sc = proc.getDisplayScene();
        addEnd = snapWorld(e, sc, false);
        repaint();
    } else if (dragMode == DragMode::Vertex) {
        verb::Scene sc = proc.getDisplayScene();
        const auto p = snapWorld(e, sc, true);
        const auto ends = dragEnds;
        proc.editScene([p, ends](verb::Scene& s) {
            for (auto& pr : ends) {
                if (pr.first < 0 || pr.first >= (int)s.walls.size()) continue;
                auto& w = s.walls[(size_t)pr.first];
                (pr.second ? w.bx : w.ax) = p.x;
                (pr.second ? w.by : w.ay) = p.y;
            }
        });
        repaint();
    } else if (dragMode == DragMode::Wall && selWall >= 0) {
        double x, y;
        view.toWorld(e.position, x, y);
        double dx = x - dragStartX, dy = y - dragStartY;
        if (!e.mods.isAltDown()) { dx = std::round(dx * 10.0) / 10.0; dy = std::round(dy * 10.0) / 10.0; }
        const int wi = selWall;
        const auto o = dragOrig;
        proc.editScene([wi, dx, dy, o](verb::Scene& s) {
            if (wi < 0 || wi >= (int)s.walls.size()) return;
            auto& w = s.walls[(size_t)wi];
            w.ax = o[0] + dx; w.ay = o[1] + dy; w.bx = o[2] + dx; w.by = o[3] + dy;
        });
        repaint();
    }
}

void VerbEditor::mouseUp(const juce::MouseEvent& e) {
    if (orbiting) { orbiting = false; return; }
    if (dragMode == DragMode::Box) {  // select every wall the rectangle touches
        const auto rect = juce::Rectangle<float>(boxStart, boxEnd);
        const verb::Scene sc = proc.getDisplayScene();
        if (!e.mods.isShiftDown() || selKind != SelKind::Wall) selWalls.clear();
        auto touches = [&](const verb::Wall& w) {
            const auto p0 = view.toScreen(w.ax, w.ay), p1 = view.toScreen(w.bx, w.by);
            if (rect.contains(p0) || rect.contains(p1)) return true;
            const juce::Line<float> seg(p0, p1);
            juce::Point<float> tmp;
            return seg.intersects(juce::Line<float>(rect.getTopLeft(), rect.getTopRight()), tmp) || seg.intersects(juce::Line<float>(rect.getTopRight(), rect.getBottomRight()), tmp) ||
                   seg.intersects(juce::Line<float>(rect.getBottomRight(), rect.getBottomLeft()), tmp) || seg.intersects(juce::Line<float>(rect.getBottomLeft(), rect.getTopLeft()), tmp);
        };
        for (int i = 0; i < (int)sc.walls.size(); ++i)
            if (touches(sc.walls[(size_t)i]) && std::find(selWalls.begin(), selWalls.end(), i) == selWalls.end()) selWalls.push_back(i);
        dragMode = DragMode::None;
        lockView = false;
        if (!selWalls.empty()) {
            selKind = SelKind::Wall;
            selWall = selWalls.back();
            showTab(1);
        } else {
            selKind = SelKind::Object;
            selWall = -1;
        }
        refreshSurface();
        resized();
        repaint();
        return;
    }
    if (panPending) {
        panPending = panning = false;
        return;
    }
    if (rightDragging) {
        if (auto* pr = proc.apvts.getParameter(verbp::paramId(selected, 3))) pr->endChangeGesture();
        rightDragging = false;
        return;
    }
    if (pickPending && pickWasSelected && pickList.size() > 1 && e.position.getDistanceFrom(pickDownPos) < 4.f) {
        // a click on the selected surface that did not move: pick the next one under the same point
        pickCur = (pickCur + 1) % (int)pickList.size();
        const Pick pk = pickList[(size_t)pickCur];
        pickPending = false;
        dragMode = DragMode::None;
        lockView = false;
        if (pk.kind != SelKind::Wall) zoneLayerCeiling = pk.kind == SelKind::CeilZone;
        selectSurface(pk.kind, pk.idx);
        return;
    }
    pickPending = false;
    if (dragMode == DragMode::FreePoint) {
        freeSrc = freeCh = -1;
        refreshSources();
    } else if (dragMode == DragMode::Object && dragging >= 0) {
        for (int k : {dragParamA, dragParamB})
            if (auto* pr = proc.apvts.getParameter(verbp::paramId(dragging, k))) pr->endChangeGesture();
        dragging = -1;
    } else if (dragMode == DragMode::NewWall) {
        if (std::hypot(addEnd.x - addStart.x, addEnd.y - addStart.y) > 0.1) {
            pushUndo();
            const auto a = addStart, b2 = addEnd;
            const SelKind kind = selKind;
            const int sw = selWall;
            int newIndex = -1;
            proc.editScene([&](verb::Scene& s) {
                verb::Material m = verb::surfacePresets()[2];  // Plaster unless a wall is selected: then copy its surface
                if (kind == SelKind::Wall && sw >= 0 && sw < (int)s.walls.size()) m = s.mats[(size_t)s.walls[(size_t)sw].mat];
                s.mats.push_back(m);
                s.walls.push_back(verb::Wall{a.x, a.y, b2.x, b2.y, (int)s.mats.size() - 1});
                newIndex = (int)s.walls.size() - 1;
            });
            if (newIndex >= 0 && newIndex < (int)proc.getDisplayScene().walls.size()) selectSurface(SelKind::Wall, newIndex);
        }
    }
    dragMode = DragMode::None;
    lockView = false;
    zoneVertex = -1;
    dragEnds.clear();
    repaint();
}
