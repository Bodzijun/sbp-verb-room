#include "EnergyMapView.h"

EnergyMapView::EnergyMapView(VerbProcessor& p) : proc(p) {
    auto label = [&](juce::Label& l, const char* text) {
        controls.addAndMakeVisible(l); l.setText(text, juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, skin::textDim); l.setFont(juce::Font(juce::FontOptions(12.f)));
    };
    label(title, "ENERGY COVERAGE"); label(sourceLabel, "Excitation"); label(bandLabel, "Octave band");
    label(heightLabel, "Probe height (m)"); label(qualityLabel, "Total tail rays"); label(gridLabel, "Map resolution");
    label(timeLabel, "Tail integration (s)"); label(rangeLabel, "Colour range (dB)"); label(status, "Ready to analyze");
    label(explanation, "Unit excitation per spatial point; source gain and Solo/Mute apply. Omni probes. Energy sum across independent paths/sources. Hover for values. Colours are relative to the map peak; this is not calibrated SPL.");
    explanation.setJustificationType(juce::Justification::topLeft);
    for (auto* c : std::initializer_list<juce::Component*>{&sources, &bands, &quality, &grid, &height, &duration, &range, &direct, &early, &tail, &analyze, &cancel, &autoScale}) controls.addAndMakeVisible(*c);
    sources.setTooltip("All audible source groups or one group. Current source gain, Solo/Mute and spatial points are respected. LFE is excluded.");
    bands.addItem("Broadband (equal band energy)", 1);
    for (int b = 0; b < verb::NB; ++b) bands.addItem(juce::String((int)verb::kBandHz[b]) + " Hz", b + 2);
    bands.setSelectedId(1, juce::dontSendNotification);
    quality.addItem("Preview - 12,000", 1); quality.addItem("Mix - 48,000", 2); quality.addItem("Fine - 192,000", 3); quality.setSelectedId(2, juce::dontSendNotification);
    for (int cells : {16, 24, 32, 48, 64, 96}) grid.addItem(juce::String(cells) + " cells (long side)", cells);
    grid.setSelectedId(24, juce::dontSendNotification);
    grid.setTooltip("More cells give a finer map and increase analysis time. Tail detail also depends on ray count and the tracer's receiver-sphere radius. Audio processing is unchanged.");
    auto slider = [&](juce::Slider& s, double lo, double hi, double step, double value) {
        s.setSliderStyle(juce::Slider::LinearHorizontal); s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 22);
        s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        s.setRange(lo, hi, step); s.setValue(value, juce::dontSendNotification);
    };
    slider(height, .1, 30, .1, 1.2); slider(duration, 1, 20, 1, 20); slider(range, .5, 120, .5, 60);
    range.setName("Energy colour range");
    range.setTooltip("Smaller dB ranges reveal subtle differences in a diffuse tail. A narrow range also magnifies ray-tracing noise; increase Total tail rays to check it.");
    autoScale.setTooltip("Fit the colour range to 95% of lit cells in the current band. Changes contrast only; no retracing.");
    autoScale.onClick = [this] { autoContrast(); };
    duration.setTooltip("Integrate late ray energy over this horizon (up to 20 seconds). This does not change the audio tail.");
    for (auto* b : {&direct, &early, &tail}) b->setToggleState(true, juce::dontSendNotification);
    analyze.getProperties().set("style", "primary");
    analyze.onClick = [this] { startAnalysis(); }; cancel.onClick = [this] { cancelAnalysis(); };
    bands.onChange = [this] { repaint(); }; range.onValueChange = [this] { repaint(); };
    sources.onChange = quality.onChange = grid.onChange = [this] { update(); };
    height.onValueChange = duration.onValueChange = [this] { update(); };
    direct.onClick = early.onClick = tail.onClick = [this] { update(); };
    refreshSources(); setControlsEnabled(true);
}

EnergyMapView::~EnergyMapView() { stop.store(true); if (worker.joinable()) worker.join(); }

void EnergyMapView::layoutControls() {
    auto area = controls.getLocalBounds();
    title.setBounds(area.removeFromTop(22)); area.removeFromTop(6);
    auto row = [&](juce::Label& label, juce::Component& value) {
        label.setBounds(area.removeFromTop(18)); value.setBounds(area.removeFromTop(26)); area.removeFromTop(7);
    };
    row(sourceLabel, sources); row(bandLabel, bands); row(heightLabel, height);
    auto components = area.removeFromTop(26);
    direct.setBounds(components.removeFromLeft(components.getWidth() / 3));
    early.setBounds(components.removeFromLeft(components.getWidth() / 2)); tail.setBounds(components); area.removeFromTop(5);
    // Compact settings fit the 600 px minimum window without a scrolling panel.
    auto labeledCombo = [&](juce::Label& label, juce::Component& value) {
        auto r = area.removeFromTop(27); label.setBounds(r.removeFromLeft(118)); value.setBounds(r); area.removeFromTop(3);
    };
    labeledCombo(qualityLabel, quality); labeledCombo(gridLabel, grid);
    labeledCombo(timeLabel, duration);
    auto colourRow = area.removeFromTop(27); rangeLabel.setBounds(colourRow.removeFromLeft(118));
    autoScale.setBounds(colourRow.removeFromRight(44)); colourRow.removeFromRight(4); range.setBounds(colourRow); area.removeFromTop(3);
    area.removeFromTop(8); auto buttons = area.removeFromTop(28);
    analyze.setBounds(buttons.removeFromLeft((buttons.getWidth() - 8) / 2)); buttons.removeFromLeft(8); cancel.setBounds(buttons);
    area.removeFromTop(5); status.setBounds(area.removeFromTop(32)); area.removeFromTop(3);
    explanation.setBounds(area);
}

verb::EnergyMapOptions EnergyMapView::options() const {
    verb::EnergyMapOptions opt; opt.height = height.getValue(); opt.duration = duration.getValue(); opt.resolution = grid.getSelectedId();
    const int budgets[] = {12000, 48000, 192000}; opt.rays = budgets[std::clamp(quality.getSelectedId() - 1, 0, 2)];
    opt.components = (direct.getToggleState() ? 1 : 0) | (early.getToggleState() ? 2 : 0) | (tail.getToggleState() ? 4 : 0);
    opt.earlyDb = proc.apvts.getRawParameterValue("mode")->load() >= .5f ? proc.apvts.getRawParameterValue("early")->load() : 0.;
    opt.tailDb = proc.apvts.getRawParameterValue("mode")->load() >= .5f ? proc.apvts.getRawParameterValue("tail")->load() : 0.;
    return opt;
}

juce::String EnergyMapView::signature(const verb::Scene& sc, const verb::EnergyMapOptions& o) const {
    return juce::String(verb::serializeScene(sc)) + "|" + juce::String(o.height, 4) + "|" + juce::String(o.duration, 2)
        + "|" + juce::String(o.resolution) + "|" + juce::String(o.rays) + "|" + juce::String(o.components)
        + "|" + juce::String(o.earlyDb, 4) + "|" + juce::String(o.tailDb, 4);
}

void EnergyMapView::refreshSources() {
    if (working.load()) return;
    const auto cfg = proc.getSources(); juce::StringArray names; names.add("All audible sources");
    for (size_t i = 0; i < cfg.size(); ++i) names.add(juce::String((int)i + 1) + ": " + juce::String(cfg[i].name));
    bool changed = sources.getNumItems() != names.size();
    for (int i = 0; !changed && i < names.size(); ++i) changed = sources.getItemText(i) != names[i];
    if (changed) {
        const int selected = sources.getSelectedId(); sources.clear(juce::dontSendNotification);
        for (int i = 0; i < names.size(); ++i) sources.addItem(names[i], i + 1);
        sources.setSelectedId(selected >= 1 && selected <= names.size() ? selected : 1, juce::dontSendNotification);
    }
}

void EnergyMapView::setControlsEnabled(bool enabled) {
    for (auto* c : std::initializer_list<juce::Component*>{&sources, &height, &duration, &quality, &grid, &direct, &early, &tail, &analyze}) c->setEnabled(enabled);
    cancel.setEnabled(!enabled);
    autoScale.setEnabled(data != nullptr);
}

void EnergyMapView::startAnalysis() {
    if (working.load()) return;
    update();
    if (proc.getLoadState() != 2) { status.setText("Wait for the room to finish loading", juce::dontSendNotification); return; }
    const auto opt = options(); const auto sc = proc.getEnergyScene(sources.getSelectedId() - 2);
    if (sc.sources.empty() || !opt.components) {
        status.setText(sc.sources.empty() ? "No audible spatial sources selected" : "Select Direct, Early or Tail", juce::dontSendNotification); return;
    }
    submittedSignature = signature(sc, opt); stop.store(false); progress.store(0); working.store(true);
    setControlsEnabled(false); status.setText("Analyzing... 0%", juce::dontSendNotification);
    worker = std::thread([this, sc, opt] {
        std::shared_ptr<const verb::EnergyMap> done; juce::String error;
        try {
            done = std::make_shared<verb::EnergyMap>(verb::analyzeEnergyMap(sc, opt,
                [this] { return stop.load(); }, [this](double f) { progress.store(f); }));
        } catch (const std::exception& e) { error = e.what(); }
        catch (...) { error = "Analysis failed"; }
        { std::lock_guard<std::mutex> lock(completionMutex); pending = std::move(done); pendingError = error; }
        working.store(false);
    });
}

void EnergyMapView::cancelAnalysis() { if (working.load()) { stop.store(true); status.setText("Cancelling...", juce::dontSendNotification); } }

void EnergyMapView::update() {
    if (worker.joinable() && !working.load()) {
        worker.join();
        std::lock_guard<std::mutex> lock(completionMutex);
        if (pending && !stop.load()) { data = std::move(pending); resultSignature = submittedSignature; hover = -1; }
        else { status.setText(stop.load() ? "Analysis cancelled" : pendingError, juce::dontSendNotification); pending.reset(); }
        setControlsEnabled(true);
    }
    refreshSources();
    const auto key = signature(proc.getEnergyScene(sources.getSelectedId() - 2), options());
    stale = data && key != resultSignature;
    if (working.load()) status.setText(stop.load() ? "Cancelling..." : "Analyzing... " + juce::String((int)(100 * progress.load())) + "%", juce::dontSendNotification);
    else if (data && pendingError.isEmpty() && !stop.load()) status.setText(stale ? "Scene/settings changed - Analyze again" : "Ready - " + juce::String(data->nx) + " x " + juce::String(data->ny) + ", " + juce::String(data->raysUsed) + " tail rays", juce::dontSendNotification);
    repaint();
}

double EnergyMapView::cellEnergy(const verb::FieldEnergy& e) const {
    const int band = bands.getSelectedId() - 2;
    double value = 0;
    for (int b = 0; b < verb::NB; ++b) if (band < 0 || b == band) value += e.direct[b] + e.early[b] + e.tail[b];
    return band < 0 ? value / verb::NB : value;
}

void EnergyMapView::autoContrast() {
    if (!data) return;
    std::vector<double> energies;
    for (size_t i = 0; i < data->cells.size(); ++i) {
        const double e = cellEnergy(data->cells[i]);
        if (data->valid[i] && e > 0 && std::isfinite(e)) energies.push_back(e);
    }
    if (energies.empty()) return;
    std::sort(energies.begin(), energies.end());
    const double low = energies[(energies.size() - 1) / 20], peak = energies.back();
    range.setValue(std::clamp(std::ceil(10 * std::log10(peak / low) * 2) / 2, .5, 120.), juce::sendNotificationSync);
    repaint();
}

juce::Colour EnergyMapView::heat(float t) {
    const juce::Colour colours[] = {juce::Colour(0xff172c49), juce::Colour(0xff287eaa), skin::accent, skin::gold, juce::Colour(0xfff46c54)};
    const float f = juce::jlimit(0.f, 1.f, t) * 4; const int k = std::min(3, (int)f);
    return colours[k].interpolatedWith(colours[k + 1], f - k);
}

juce::Rectangle<float> EnergyMapView::plotBounds() const {
    auto area = getLocalBounds().toFloat().reduced(14).withTrimmedTop(46).withTrimmedRight(58).withTrimmedBottom(56);
    if (!data || area.isEmpty()) return area;
    const float w = (float)(data->nx * data->dx), h = (float)(data->ny * data->dy);
    const float scale = std::min(area.getWidth() / w, area.getHeight() / h);
    return juce::Rectangle<float>(w * scale, h * scale).withCentre(area.getCentre());
}

void EnergyMapView::paint(juce::Graphics& g) {
    g.fillAll(skin::canvas); g.setFont(juce::Font(juce::FontOptions(15.f))); g.setColour(skin::text);
    g.drawText("Sound energy coverage", 14, 10, getWidth() - 28, 22, juce::Justification::centredLeft);
    g.setFont(juce::Font(juce::FontOptions(11.f))); g.setColour(stale ? skin::gold : skin::textDim);
    g.drawText(stale ? "OUTDATED - previous analysis; press Analyze to update" : "Omnidirectional probes | modelled, time-integrated energy", 14, 34, getWidth() - 28, 18, juce::Justification::centredLeft);
    const auto plot = plotBounds();
    if (!data) { g.setColour(skin::textDim); g.drawFittedText("Choose sources and height, then press Analyze.", plot.toNearestInt(), juce::Justification::centred, 2); return; }
    double peak = 0;
    for (size_t i = 0; i < data->cells.size(); ++i) if (data->valid[i]) peak = std::max(peak, cellEnergy(data->cells[i]));
    auto screen = [&](double x, double y) { return juce::Point<float>(plot.getX() + (float)((x - data->x0) / (data->nx * data->dx)) * plot.getWidth(), plot.getBottom() - (float)((y - data->y0) / (data->ny * data->dy)) * plot.getHeight()); };
    g.saveState(); g.reduceClipRegion(plot.toNearestInt());
    for (int y = 0; y < data->ny; ++y) for (int x = 0; x < data->nx; ++x) {
        const size_t i = (size_t)y * data->nx + x;
        const double e = cellEnergy(data->cells[i]);
        const double db = peak > 0 && e > 0 ? 10 * std::log10(e / peak) : -200;
        g.setColour(data->valid[i] && e > 0 ? heat((float)(1 + db / range.getValue())) : skin::bg);
        const auto pos = screen(data->x0 + x * data->dx, data->y0 + (y + 1) * data->dy);
        g.fillRect(juce::Rectangle<float>(pos.x, pos.y, plot.getWidth() / data->nx + .5f, plot.getHeight() / data->ny + .5f));
    }
    for (const auto& w : data->scene.walls) {
        g.setColour(skin::text.withAlpha(data->options.height >= w.z0 && data->options.height <= std::min(w.z1, data->scene.height) ? .85f : .3f));
        const auto a = screen(w.ax, w.ay), b = screen(w.bx, w.by); g.drawLine(a.x, a.y, b.x, b.y, 1.3f);
    }
    int n = 0;
    for (const auto& src : data->scene.sources) {
        const auto p = screen(src.pos.x, src.pos.y); g.setColour(skin::bg); g.fillEllipse(p.x - 4, p.y - 4, 8, 8);
        g.setColour(skin::text); g.drawEllipse(p.x - 4, p.y - 4, 8, 8, 1); g.drawText(juce::String(++n), (int)p.x + 4, (int)p.y - 13, 25, 14, juce::Justification::left);
    }
    const double lx = proc.apvts.getRawParameterValue(verbp::paramId(verbp::kListener, 0))->load();
    const double ly = proc.apvts.getRawParameterValue(verbp::paramId(verbp::kListener, 1))->load();
    const auto listener = screen(lx, ly); g.setColour(skin::rose); g.drawEllipse(listener.x - 6, listener.y - 6, 12, 12, 2);
    g.drawLine(listener.x - 9, listener.y, listener.x + 9, listener.y, 1); g.drawLine(listener.x, listener.y - 9, listener.x, listener.y + 9, 1);
    if (hover >= 0) {
        const int x = hover % data->nx, y = hover / data->nx;
        const auto p = screen(data->x0 + x * data->dx, data->y0 + (y + 1) * data->dy); g.setColour(skin::text);
        g.drawRect(juce::Rectangle<float>(p.x, p.y, plot.getWidth() / data->nx, plot.getHeight() / data->ny), 1.5f);
    }
    g.restoreState();
    const float legendX = getWidth() - 53.f;
    for (int i = 0; i < 100; ++i) { g.setColour(heat(1 - i / 99.f)); g.fillRect(legendX, plot.getY() + i * plot.getHeight() / 100, 9.f, plot.getHeight() / 100 + 1); }
    g.setColour(skin::textDim);
    const int decimals = range.getValue() < 5 ? 2 : range.getValue() < 20 ? 1 : 0;
    for (int i = 0; i <= 4; ++i) g.drawText(juce::String(i == 0 ? 0. : -range.getValue() * i / 4, decimals), (int)legendX + 13, (int)(plot.getY() + plot.getHeight() * i / 4) - 7, 39, 14, juce::Justification::left);
    g.drawText("dB rel.", (int)legendX - 1, (int)plot.getY() - 20, 53, 16, juce::Justification::left);
    juce::String caption = "Peak " + (peak > 0 ? juce::String(10 * std::log10(peak), 1) + " dB re unit at 1 m" : juce::String("-- (no energy)"));
    caption += " | Z " + juce::String(data->options.height, 1) + " m | X " + juce::String(data->x0, 1) + ".." + juce::String(data->x0 + data->nx * data->dx, 1) + " m";
    g.drawFittedText(caption, 14, getHeight() - 45, getWidth() - 28, 18, juce::Justification::left, 1);
    juce::String detail = "Hover a cell for position and level. Pink marker: current listener XY.";
    if (hover >= 0) {
        const auto pos = data->position(hover % data->nx, hover / data->nx); const double e = cellEnergy(data->cells[(size_t)hover]);
        detail = "X " + juce::String(pos.x, 2) + "  Y " + juce::String(pos.y, 2) + " m | ";
        detail += !data->valid[(size_t)hover] ? "Outside usable height" : e > 0 && peak > 0 ? juce::String(10 * std::log10(e / peak), 1) + " dB rel. peak" : "No energy detected";
    }
    g.drawFittedText(detail, 14, getHeight() - 25, getWidth() - 28, 18, juce::Justification::left, 1);
}

void EnergyMapView::mouseMove(const juce::MouseEvent& e) {
    hover = -1; const auto plot = plotBounds();
    if (data && plot.contains(e.position)) {
        const int x = std::clamp((int)((e.position.x - plot.getX()) / plot.getWidth() * data->nx), 0, data->nx - 1);
        const int y = std::clamp((int)((plot.getBottom() - e.position.y) / plot.getHeight() * data->ny), 0, data->ny - 1);
        hover = y * data->nx + x;
    }
    repaint();
}
void EnergyMapView::mouseExit(const juce::MouseEvent&) { hover = -1; repaint(); }
