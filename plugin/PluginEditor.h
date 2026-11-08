#pragma once
#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "PluginLookAndFeel.h"
#include "PluginProcessor.h"
#include "RoomLibrary.h"
#include "TailDecayView.h"

// Room editor. Canvas: drag sources/listener; Move tool: drag wall endpoints (shared corners move together) or
// whole segments; Add-wall tool: drag out a new segment (snaps to corners and a 10 cm grid, Alt = free);
// Delete / Backspace removes the selected segment; Ctrl+Z undoes. Every segment owns its surface: absorption in
// 7 octave bands, scattering (LF/HF) and transmission loss (LF/MF/HF) or a preset (Concrete ... Open doorway).
class VerbEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit VerbEditor(VerbProcessor&);
    ~VerbEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    // View: auto-fit follows walls and objects; the free view keeps its own centre and zoom (wheel, drag empty space).
    void setAutoView(bool on);
    bool isAutoView() const { return autoFit; }
    void zoomView(double factor);  // about the middle of the map (the wheel zooms about the pointer)
    void selectWall(int index) { selectSurface(SelKind::Wall, index); }  // select a segment and show its surface
    void clearEverything() { clearRoom(); }  // test hook (the button asks first)
    // Multi-selection hooks (also used by the tests).
    void selectWalls(const std::vector<int>& w) {
        if (w.empty()) return;
        selWalls = w;
        selKind = SelKind::Wall;
        selWall = w.back();
        showTab(1);
        refreshSurface();
        resized();
        repaint();
    }
    void applyPresetToSelection(const juce::String& name) {
        const verb::Material m = verb::presetByName(name.toStdString());
        editSurface([m](verb::Material& x) { x = m; });
    }
    // Room library and export (also used by the tests).
    int numBuiltinRooms() const { return (int)builtins.size(); }
    void chooseBuiltinRoom(int i) {
        if (i < 0 || i >= (int)builtins.size()) return;
        const auto& r = builtins[(size_t)i];
        if (r.setup.empty()) { loadRoomText(juce::String(r.text), juce::String(r.name) + ".txt"); return; }
        undoStack.clear();  // a room with its own sources (the car): the plugin's sources are set up with it
        selKind = SelKind::Object;
        selWall = -1;
        selWalls.clear();
        zonePts.clear();
        proc.loadRoomWithSetup(juce::String(r.text), juce::String(r.name) + ".txt", r.setup, r.micPattern);
    }
    void showPlane(int p) { setPlane(p == 1 ? Plane::Front : p == 2 ? Plane::Side : p == 3 ? Plane::Iso : Plane::Top); }  // 0 plan, 1 front, 2 side, 3 3D
    void selectZone(bool ceiling, int index) {
        zoneLayerCeiling = ceiling;
        selectSurface(ceiling ? SelKind::CeilZone : SelKind::FloorZone, index);
    }
    double testViewPsi() const { return viewPsi; }
    int testPickCount(double wx, double wy) { const auto sc = proc.getDisplayScene(); return (int)pickCandidates(sc, view.toScreen(wx, wy)).size(); }  // surfaces under a world point
    void testSetLayer(int layer, int mode) { layerMode[layer] = mode; updateLayerButtons(); }
    void showPage(int tabIndex, int object) {                            // 0 Objects, 1 Surfaces, 2 Mix, 3 Space, 4 Decay
        if (object >= 0) selectObject(object);
        showTab(tabIndex);
        resized();
    }

private:
    enum class Tool { Move, BoxSelect, AddWall, AddFloorZone, AddCeilZone };
    enum class SelKind { Object, Wall, Floor, Ceiling, FloorZone, CeilZone };  // zones: selWall holds the zone index
    enum class DragMode { None, Object, Vertex, Wall, NewWall, FreePoint, ZoneVertex, ZoneMove, WallTop, WallBottom, Box };
    // Which view the map shows: plan (x, y) from above, an elevation (x, z) from the front / (y, z) from the side, or 3D.
    enum class Plane { Top, Front, Side, Iso };
    // The map's frame (u across, v up) is the world (x, y), or, with `rotated`, the world turned so that +x points UP and
    // +y points left: a listener at yaw 0 then faces up and its left/right are the screen's left/right. minX/minY are
    // the frame's lower-left corner.
    struct View {
        double minX = 0, minY = 0, scale = 1;
        juce::Rectangle<float> area;
        bool rotated = false;
        double psi = 0;  // with `rotated`: the world heading (radians) that points up on the screen (the listener's yaw)
        // frame of the map: v = along the up heading, u = to its right (psi = 0: v = x, u = -y)
        void frame(double x, double y, double& u, double& v) const {
            if (!rotated) { u = x; v = y; return; }
            const double s = std::sin(psi), c = std::cos(psi);
            u = x * s - y * c;
            v = x * c + y * s;
        }
        juce::Point<float> toScreen(double x, double y) const {
            double u, v;
            frame(x, y, u, v);
            return {(float)(area.getX() + (u - minX) * scale), (float)(area.getBottom() - (v - minY) * scale)};
        }
        void toWorld(juce::Point<float> p, double& x, double& y) const {
            const double u = minX + (p.x - area.getX()) / scale, v = minY + (area.getBottom() - p.y) / scale;
            if (!rotated) { x = u; y = v; return; }
            const double s = std::sin(psi), c = std::cos(psi);
            x = u * s + v * c;
            y = -u * c + v * s;
        }
        float uToX(double u) const { return (float)(area.getX() + (u - minX) * scale); }
        float vToY(double v) const { return (float)(area.getBottom() - (v - minY) * scale); }
        // unit vector on the screen of a world heading (radians, counter-clockwise from +x)
        juce::Point<float> dir(double a) const {
            if (!rotated) return {(float)std::cos(a), (float)-std::sin(a)};
            return {(float)std::sin(psi - a), (float)-std::cos(a - psi)};
        }
    };
    bool faceUp = false;     // plan orientation: the listener's front points up, whatever its yaw (left/right on the screen = the listener's)
    double viewPsi = 0;     // the heading used for that rotation: taken when a room is loaded or "Listener up" is switched on, then kept, so turning the listener does not spin the map
    uint64_t viewPsiSerial = ~0ull;
    bool relatchView = false;
    struct Row {
        juce::Label label;
        juce::Slider slider;
    };

    void paintInputMeters(juce::Graphics& g);
    View makeView(const verb::Scene&, const double* listener) const;
    void timerCallback() override;
    void selectObject(int obj);
    void selectSurface(SelKind kind, int wall);
    void showTab(int tab);
    void updateSliderRanges();
    void syncSliders();
    void refreshSurface();
    void chooseScene();
    void setupRow(Row& r, const juce::String& text, double lo, double hi, double step, const juce::String& suffix);
    void editSurface(std::function<void(verb::Material&)> fn);
    void pushUndo();
    void undo();
    void deleteSelectedWall();
    juce::Point<double> snapWorld(const juce::MouseEvent&, const verb::Scene&, bool excludeDragged) const;
    double paramValue(int obj, int k) const;
    void setParam(const juce::String& id, double v);

    VerbLookAndFeel laf;  // first member: destroyed after every component that uses it
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> headers[5];  // section headers per tab, drawn in paint()
    VerbProcessor& proc;
    // header
    juce::TextButton loadButton{"Load scene..."};  // (not shown: the room box has "Load from file...")
    juce::ComboBox roomBox;                         // room library: built-in rooms, the user's saved rooms, load from file
    juce::TextButton saveRoomBtn{"Save room"}, exportBtn{"Export IR"};
    std::vector<verbp::BuiltinRoom> builtins;
    std::vector<juce::File> userRooms;
    bool exportShown = false;
    static juce::File libraryDir();
    void refreshRoomBox();
    void onRoomChosen();
    void saveRoomDialog();
    void exportDialog();
    void loadRoomText(const juce::String& text, const juce::String& name);
    juce::Label statusLabel;
    juce::TextButton tabObjects{"Objects"}, tabSurfaces{"Surfaces"}, tabMix{"Mix"}, tabSpace{"Space"};
    juce::TextButton tabDecay{"Decay"};
    TailDecayView decayView;
    juce::ComboBox qualityBox;
    juce::Label qualityLabel, qualityInfo;
    Row rayRow;
    juce::ToggleButton adaptiveTail{"Adaptive rays (up to 4x base)"};
    void refreshTailSettings();
    juce::ToggleButton soloDirect{"Direct"}, soloEarly{"Early"}, soloTail{"Tail"};
    juce::Label soloLabel;
    std::unique_ptr<juce::ButtonParameterAttachment> soloAttachments[3];
    // canvas toolbar
    IconButton toolMove{IconButton::Icon::Move}, toolBox{IconButton::Icon::Box}, toolAdd{IconButton::Icon::Wall}, toolFloorZone{IconButton::Icon::FloorZone},
        toolCeilZone{IconButton::Icon::CeilZone}, toolDelete{IconButton::Icon::Delete}, toolUndo{IconButton::Icon::Undo}, toolClear{IconButton::Icon::Clear};
    void clearRoom();  // removes every wall, zone and clutter object (one undo step)
    juce::Slider mixSlider, tailSlider;  // dry/wet, tail level
    juce::Label mixLabel, tailLabel;
    std::unique_ptr<juce::SliderParameterAttachment> mixAttachment, tailAttachment;
    juce::TextButton viewTop{"Top"}, viewFront{"Front"}, viewSide{"Side"}, viewIso{"3D"};
    juce::TooltipWindow tooltipWindow{this, 600};
    juce::Rectangle<int> toolStrip;
    // multi-selection of walls (the primary one is selWall) and the box-select rubber band
    std::vector<int> selWalls;
    juce::Point<float> boxStart, boxEnd;
    bool wallSelected(int i) const { return selKind == SelKind::Wall && std::find(selWalls.begin(), selWalls.end(), i) != selWalls.end(); }
    // 3D view
    double camYaw = -0.75, camPitch = 0.5, camZoom = 1.0;
    juce::Point<float> orbitStart;
    double orbitYaw0 = 0, orbitPitch0 = 0;
    bool orbiting = false;
    void paint3D(juce::Graphics&, const verb::Scene&, const double* lis);
    Plane plane = Plane::Top;
    void setPlane(Plane p);
    bool zoneLayerCeiling = false;               // which layer a click on the plan selects when zones overlap
    std::vector<juce::Point<double>> zonePts;    // corners of the zone being drawn
    juce::Point<float> hoverPos;
    int zoneVertex = -1;                         // dragged corner
    int dragParamA = 0, dragParamB = 1;          // the two parameters an object drag moves (x,y on the plan; x|y,z in an elevation)
    std::vector<std::pair<double, double>> zoneOrig;
    void finishZone();
    void paintElevation(juce::Graphics&, const verb::Scene&, const double* lis);
    void mouseDownElevation(const juce::MouseEvent&);
    void mouseMove(const juce::MouseEvent&) override;
    bool inPlan() const { return plane == Plane::Top; }
    // Layers of the plan: walls, floor zones, ceiling zones and plates (zones with a height). Each is shown, locked
    // (drawn, but clicks pass through it) or hidden (not drawn, not picked).
    enum Layer { LayerWalls = 0, LayerFloors, LayerCeilings, LayerPlates };
    int layerMode[4] = {0, 0, 0, 0};  // 0 shown, 1 locked, 2 hidden
    juce::TextButton layerBtn[4];
    static int zoneLayer(const verb::Zone& z, bool ceiling) { return z.z >= 0 ? (int)LayerPlates : (ceiling ? (int)LayerCeilings : (int)LayerFloors); }
    void updateLayerButtons();
    // Everything under a point of the plan that can be picked, the top one first; a repeated click steps down the list.
    struct Pick { SelKind kind; int idx; };
    std::vector<Pick> pickCandidates(const verb::Scene& sc, juce::Point<float> pos);
    std::vector<Pick> pickList;
    int pickCur = 0;
    bool pickPending = false, pickWasSelected = false;
    juce::Point<float> pickDownPos;
    double horiz(double x, double y) const { return plane == Plane::Front ? x : y; }  // map-horizontal coordinate in the current plane
    int horizParam() const { return plane == Plane::Front ? 0 : 1; }
    juce::ToggleButton autoViewBtn{"Auto view"};
    bool autoFit = true;               // view follows walls and objects
    double vCx = 0, vCy = 0, vScale = 20;  // free view: centre (metres) and pixels per metre
    bool panning = false, panPending = false;
    juce::Point<float> panStart;
    double panCx0 = 0, panCy0 = 0;
    // objects tab: source list and setup
    juce::ComboBox sourceBox;
    juce::TextButton addSourceBtn{"+ Source"}, removeSourceBtn{"- Source"};
    juce::TextEditor nameEdit;
    juce::Label nameLabel, formatLabel, firstInLabel, chLabel, infoLabel, srcGainLabel;
    juce::ComboBox formatBox, firstInBox, chBox;
    juce::ToggleButton sumToggle{"Sum channels into one point"};
    Row spreadRow, patternRow;
    juce::Slider srcGainSlider;
    std::unique_ptr<juce::SliderParameterAttachment> srcGainAttachment;
    std::vector<verbp::SourceCfg> cfgCache;
    juce::ToggleButton freeToggle{"Free channel points (drag each one on the map)"};
    // surfaces tab: class, scale
    juce::ComboBox classBox;
    juce::Label classLabel;
    Row scaleRow;
    Row zoneZRow;  // the selected floor/ceiling zone: 0 = a material patch, above 0 = a plate at this height
    Row wallBotRow, wallTopRow;  // the selected segment's vertical extent
    Row hMinRow, hMaxRow;        // clutter object heights (Space tab)
    juce::TextButton scaleRoomBtn{"Scale room"};
    juce::ToggleButton scaleObjects{"Room scale also moves sources, listener, height"};
    // space tab: air and clutter
    Row tempRow, humRow, cntRow, minLenRow, maxLenRow, tlMinRow, tlMaxRow;
    juce::Slider seedSlider;
    juce::Label seedLabel, airInfo, clutterInfo;
    juce::TextButton randomizeBtn{"Randomize"};
    juce::ToggleButton showClutter{"Show clutter on the map"};
    juce::ToggleButton showMeters{"Show input channel meters (map, bottom)"};
    juce::Label versionLabel;  // the running version and build time (Space tab)
    juce::ToggleButton diffToggle{"Edge diffraction (sound bends round wall ends)"};
    void refreshSpace();
    void editClutter(std::function<void(verb::ClutterCfg&)> fn);
    // interaction state
    bool rightDragging = false;
    int freeSrc = -1, freeCh = -1;  // channel point being dragged (free placement)
    // mix tab: outputs
    juce::ToggleButton showMic{"Show the microphone array to scale"};
    juce::ComboBox layoutBox, micBox;  // output layout; microphone pattern (omni ... figure-8, human ears)
    juce::Label micLabel;
    juce::Label layoutLabel, outInfoLabel;
    Row radiusRow;
    void refreshSources();
    void refreshOutputs();
    void updateObjectsVisibility();
    bool uiIsSource = false, uiMulti = false, uiRing = false, uiSpread = false;  // what the selected object shows
    void editSource(std::function<void(verbp::SourceCfg&)> fn);
    juce::Label selLabel;
    std::array<juce::Slider, 4> sliders;
    std::array<juce::Label, 4> sliderLabels;
    bool updatingUI = false;
    // surfaces tab
    juce::Label surfTitle, presetLabel;
    juce::ComboBox presetBox;
    juce::TextButton selFloor{"Floor"}, selCeiling{"Ceiling"};
    juce::ToggleButton openSky{"Open sky (no ceiling)"};
    std::array<Row, 7> absRows;
    std::array<Row, 2> scRows;
    std::array<Row, 3> tlRows;
    Row heightRow, passRow, roughRow;
    // mix tab
    juce::Slider gainSlider, earlySlider, diffSlider, dopplerSlider;
    juce::Label gainLabel, earlyLabel, diffLabel, dopplerLabel, inputModeLabel;
    juce::ComboBox inputModeBox;
    std::unique_ptr<juce::SliderParameterAttachment> gainAttachment, earlyAttachment, diffAttachment, dopplerAttachment;
    std::unique_ptr<juce::ComboBoxParameterAttachment> inputModeAttachment;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> canvas;
    Tool tool = Tool::Move;
    SelKind selKind = SelKind::Object;
    int selected = 0;  // selected object (source index / kListener)
    int selWall = -1;
    int tab = 0;
    DragMode dragMode = DragMode::None;
    int dragging = -1;
    std::vector<std::pair<int, int>> dragEnds;  // (wall, 0=a/1=b) endpoints moved together
    double dragStartX = 0, dragStartY = 0;
    std::array<double, 4> dragOrig{};           // original wall endpoints for whole-wall drags
    juce::Point<double> addStart, addEnd;
    bool lockView = false;
    juce::String lastStatus;
    int lastState = -1;
    uint64_t lastSerial = ~0ull;
    std::vector<verb::Scene> undoStack;
    View view;
};
