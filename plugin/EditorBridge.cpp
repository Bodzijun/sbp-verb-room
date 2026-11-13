#include "BoundedDiffraction.h"
#include "EditorBridge.h"
#include "WebContractDefaults.h"
#include "RoomLibrary.h"
#include "geometry.hpp"
#include "dsp.hpp"
#include <cstring>
#ifndef VERB_WEB_PLUGIN_VERSION
#define VERB_WEB_PLUGIN_VERSION "development"
#endif
namespace verbweb {
namespace {
juce::var tagJson(const verb::SurfaceTag& t) { return obj({{"name", juce::String(t.name)}, {"group", juce::String(t.group)}, {"hidden", t.hidden}, {"locked", t.locked}}); }
juce::var sourceJson(const verbp::SourceCfg& s) {
    juce::Array<juce::var> offsets;
    if (s.freePoints) for (size_t i = 0; i < s.ox.size() && i < s.oy.size() && i < s.oz.size(); ++i) offsets.add(array({s.ox[i], s.oy[i], s.oz[i]}));
    return obj({{"name", juce::String(s.name)}, {"format", s.format}, {"channels", s.channels}, {"firstIn", s.firstIn}, {"sum", s.sum}, {"spreadM", s.spread}, {"pattern", s.pattern}, {"freePoints", s.freePoints}, {"offsetsM", offsets}});
}
juce::File libraryDir() { return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("SBP/Verb Room/Rooms"); }
}
juce::var materialJson(const verb::Material& m) {
    return obj({{"name", juce::String(m.name)}, {"classId", m.cls}, {"alpha", numbers(m.alpha)}, {"tau", numbers(m.tau)}, {"scatterLF", m.scatter}, {"scatterHF", m.scatterHf < 0 ? juce::var() : juce::var(m.scatterHf)}});
}
juce::var sceneJson(const verb::Scene& s, const double* home) {
    juce::Array<juce::var> mats, walls;
    for (const auto& m : s.mats) mats.add(materialJson(m));
    for (const auto& w : s.walls) walls.add(obj({{"a", array({w.ax, w.ay})}, {"b", array({w.bx, w.by})}, {"zBottomM", w.z0}, {"zTopM", w.z1 >= 1e5 ? juce::var() : juce::var(w.z1)}, {"material", w.mat}, {"editor", tagJson(w.editor)}}));
    const auto zones = [](const std::vector<verb::Zone>& list) { juce::Array<juce::var> a; for (const auto& z : list) { juce::Array<juce::var> vertices; for (const auto& p : z.poly) vertices.add(array({p.first, p.second})); a.add(obj({{"vertices", vertices}, {"heightM", z.z < 0 ? juce::var() : juce::var(z.z)}, {"material", z.mat < 0 ? juce::var() : juce::var(z.mat)}, {"editor", tagJson(z.editor)}})); } return juce::var(a); };
    auto r = obj({{"heightM", s.height}, {"tempC", s.tempC}, {"humidityPct", s.humidity}, {"floorMaterial", s.floorMat}, {"ceilingMaterial", s.ceilMat < 0 ? juce::var() : juce::var(s.ceilMat)},
        {"materials", mats}, {"walls", walls}, {"floorZones", zones(s.floorZones)}, {"ceilingZones", zones(s.ceilZones)}, {"floorEditor", tagJson(s.floorEditor)}, {"ceilingEditor", tagJson(s.ceilEditor)},
        {"clutter", obj({{"count", s.clutter.count}, {"minLenM", s.clutter.minLen}, {"maxLenM", s.clutter.maxLen}, {"minHeightM", s.clutter.hMin}, {"maxHeightM", s.clutter.hMax}, {"tlMinDb", s.clutter.tlMin}, {"tlMaxDb", s.clutter.tlMax}, {"minDistM", s.clutter.minDist}, {"seed", s.clutter.seed}})},
        {"roughness", s.roughness}, {"roughSeed", s.roughSeed}, {"diffraction", s.diffraction}, {"preserved", obj({{"sr", s.sr}, {"lengthS", s.length}, {"order", s.order}, {"rays", s.rays}, {"seed", s.seed}, {"detectRadiusM", s.detectRadius}})}});
    if (home) r.getDynamicObject()->setProperty("home", obj({{"x", home[0]}, {"y", home[1]}, {"z", home[2]}, {"yawDeg", home[3]}}));
    return r;
}
EditorBridge::EditorBridge(VerbProcessor& p, Emit e) : proc(p), parameters(p), emit(std::move(e)), cpuBlocksSeen(p.webAudioBlockCount()) { proc.getEditorSession().setRedoEnabled(true); pathsRequest = obj({{"sourceIndex", 0}, {"maxOrder", 1}, {"maxPaths", 128}}); }
EditorBridge::~EditorBridge() { if (preferenceLocation != juce::File()) { preferenceLocation.getParentDirectory().createDirectory(); preferenceLocation.replaceWithText(juce::JSON::toString(preferences)); } proc.setWebMetersActive(false); lifetime.reset(); analysisCancel = true; if (analysisWorker.joinable()) analysisWorker.join(); parameters.endAll(); }
juce::var EditorBridge::envelope(const char* kind, juce::var payload, const juce::String& id) const {
    auto r = obj({{"schemaVersion", 1}, {"sessionId", sessionId}, {"kind", kind}, {"payload", finiteJson(payload)}}); if (id.isNotEmpty()) r.getDynamicObject()->setProperty("requestId", id); return r;
}
juce::var EditorBridge::ack(const juce::String& id, const juce::String& job) const {
    auto r = obj({{"requestId", id}, {"revision", decimal(proc.getRevision())}, {"status", "accepted"}}); if (job.isNotEmpty()) r.getDynamicObject()->setProperty("jobId", job); return envelope("ack", r, id);
}
juce::var EditorBridge::error(const juce::String& id, const char* code, const juce::String& message, bool resync) const {
    auto p = obj({{"code", code}, {"message", message}, {"currentRevision", decimal(proc.getRevision())}, {"resync", resync}}); if (id.isNotEmpty()) p.getDynamicObject()->setProperty("requestId", id); return envelope("error", p, id);
}
juce::var EditorBridge::host() const { return obj({{"inputChannels", proc.hostInputChannels()}, {"outputChannels", proc.hostOutputChannels()}, {"renderedChannels", proc.numOutputChannels()}, {"effectiveLayout", proc.effectiveLayout()}, {"numPoints", proc.numPoints()}, {"sampleRate", proc.getSampleRate()}}); }
juce::var EditorBridge::history() {
    auto& s = proc.getEditorSession(); return obj({{"event", "history.changed"}, {"canUndo", s.canUndo()}, {"undoDepth", s.undoDepth()}, {"canRedo", s.canRedo()}, {"undoLabel", juce::String(s.undoLabel()).substring(0, 60)}, {"redoLabel", juce::String(s.redoLabel()).substring(0, 60)}});
}
juce::var EditorBridge::document() const {
    double home[4]; const auto scene = proc.getDisplayScene(home); juce::Array<juce::var> sources;
    for (const auto& s : proc.getSources()) sources.add(sourceJson(s));
    const auto output = proc.getOutputCfg(); const auto tail = proc.getTailSettings(); const auto layers = proc.getLayerModes();
    return obj({{"sceneName", proc.getSceneName()}, {"scene", sceneJson(scene, home)}, {"sources", sources}, {"output", obj({{"layout", output.layout}, {"radiusM", output.radius}, {"micPattern", output.micPattern}})},
        {"tail", obj({{"baseRays", tail.rays}, {"adaptive", tail.adaptive}})}, {"editorFlags", obj({{"navLocked", proc.navigationLocked()}, {"outlinerVisible", proc.outlinerVisible()}, {"mixerCompact", proc.mixerCompactEnabled()}, {"inputMeters", proc.inputMetersEnabled()}, {"keepRouting", proc.keepRoutingEnabled()},
        {"layerModes", obj({{"walls", layers[0]}, {"floors", layers[1]}, {"ceilings", layers[2]}, {"plates", layers[3]}})}})}});
}
juce::var EditorBridge::snapshot() { return snapshotAttempt(2); }
juce::var EditorBridge::snapshotAttempt(int retries) {
    documentAvailable = true;
    const auto revision = proc.getRevision(), sceneSerial = proc.getSceneSerial(), loadSerial = proc.getLoadSerial();
    const int loadState = proc.getLoadState();
    auto caps = juce::JSON::parse(capabilityJson); caps.getDynamicObject()->setProperty("pluginVersion", VERB_WEB_PLUGIN_VERSION);
    auto params = parameters.snapshot(tailCurrent); params.getDynamicObject()->setProperty("descriptors", parameters.descriptors());
    auto prefs = preferences.clone(); const auto view = proc.getEditorSession().viewState(); if (view.isObject()) prefs.getDynamicObject()->setProperty("sessionViewState", view);
    auto payload = obj({{"revision", decimal(revision)}, {"sceneSerial", decimal(sceneSerial)}, {"loadSerial", decimal(loadSerial)}, {"capabilities", caps}, {"limits", juce::JSON::parse(limitJson)},
        {"parameters", params}, {"document", document()}, {"host", host()}, {"load", obj({{"state", loadState}, {"status", proc.getStatus()}, {"tailCurrent", tailCurrent}})}, {"preferences", prefs},
        {"jobs", obj({{"export", obj({{"state", proc.getExportState()}, {"message", proc.getExportMessage()}})}, {"rt60", obj({{"state", proc.getTargetRT60State()}})}, {"analysis", analysisRunning ? obj({{"jobId", analysisId}, {"state", "running"}, {"progress", analysisProgress.load()}}) : juce::var()}})}});
    if (retries > 0 && (revision != proc.getRevision() || sceneSerial != proc.getSceneSerial() || loadSerial != proc.getLoadSerial() || loadState != proc.getLoadState())) return snapshotAttempt(retries - 1);
    // Track exactly the serials sent. Even a continuously changing loader will force another snapshot.
    sentRevision = revision; sentScene = sceneSerial; sentLoad = loadSerial; lastState = loadState;
    return envelope("snapshot", payload);
}
void EditorBridge::emitEvent(juce::var e) {
    if (!(emit && ready && visible)) return;
    const auto name = prop(e, "event").toString();
    if (name.startsWith("status.")) {
        const auto serialized = juce::JSON::toString(e, true); if (lastEvents[name] == serialized) return; lastEvents[name] = serialized;
    }
    emit("verb.message", envelope("event", e));
}
void EditorBridge::publishSnapshot() { if (emit && ready && visible) { emit("verb.message", snapshot()); emitEvent(history()); } }
juce::var EditorBridge::handle(const juce::var& message) {
    const auto key = prop(message, "sessionId").toString() + ":" + prop(message, "requestId").toString();
    const bool command = prop(message, "kind").toString() == "command" && prop(message, "requestId").isString();
    if (command) for (const auto& cached : serviceReplies) if (cached.first == key) return array({cached.second, snapshot()});
    const auto reply = dispatch(message);
    if (command && reply.isArray() && reply.size() && prop(reply[0], "kind").toString() == "ack") {
        serviceReplies.push_back({key, reply[0].clone()});
        while (serviceReplies.size() > 256) serviceReplies.pop_front();
    }
    return reply;
}
juce::var EditorBridge::dispatch(const juce::var& message) {
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto id = prop(message, "requestId").toString();
    const auto result = [&](juce::var r) { return array({r}); };
    if (juce::JSON::toString(message, true).getNumBytesAsUTF8() > (1u << 20)) return result(error(id, "invalid", "Control payload exceeds 1 MiB"));
    if (!prop(message, "schemaVersion").isInt() || (int)prop(message, "schemaVersion") != 1) return result(error(id, "unsupported", "Unsupported schema major version"));
    const auto sid = prop(message, "sessionId").toString(); if (sid.isEmpty() || sid.length() > 128) return result(error(id, "invalid", "Invalid sessionId"));
    sessionId = sid; proc.getEditorSession().setSessionId(sid);
    if (prop(message, "kind").toString() == "hello") {
        const auto p = prop(message, "payload"); auto versions = prop(p, "supportedVersions");
        if (!versions.isArray() || !versions.getArray()->contains(juce::var(1)) || !(bool)prop(p, "ready")) return result(error(id, "unsupported", "Client does not support this contract"));
        ready = true; parameters.endAll();
        // The first scene is asynchronous. Never emit an invented/invalid empty document.
        if (!documentAvailable && proc.getLoadState() != 2) return juce::Array<juce::var>();
        lastDiagnostics.reset(); lastEvents.clear(); lastSlow = -1e20; pathsDirty = true; clutterDirty = true; lastClutterSignature = {};
        juce::Array<juce::var> replies{snapshot(), envelope("event", history())};
        // A hidden WebView unloads its JS context. Re-send the stored descriptor even if the
        // result completed before the replacement page installed listeners.
        if (analysisDescriptor.isObject()) {
            const auto resultId = prop(analysisDescriptor, "resultId");
            replies.add(envelope("event", obj({{"event", "analysis.result"}, {"jobId", resultId}, {"outcome", "done"}, {"result", analysisDescriptor}})));
            if (staleSent) replies.add(envelope("event", obj({{"event", "analysis.stale"}, {"resultId", resultId}, {"stale", true}})));
        }
        return replies;
    }
    if (!ready) return result(error(id, "notReady", "Send hello before commands"));
    if (prop(message, "kind").toString() != "command" || id.isEmpty()) return result(error(id, "invalid", "Expected command with requestId"));
    const auto payload = prop(message, "payload"); const auto op = prop(payload, "op").toString();
    const bool service = op == "telemetry.subscribe" || op == "preferences.set" || op == "preset.list" || op == "catalogs.list" || op == "preset.load" || op == "preset.save" || op == "ir.export" || op == "analysis.start" || op == "analysis.cancel" || op == "rt60.start" || op == "rt60.cancel" || op == "reflections.query" || op == "editor.setFlags";
    if (service && prop(message, "baseRevision").toString() != decimal(proc.getRevision())) return array({error(id, "staleRevision", "Structural revision changed", true), snapshot()});
    if (op == "telemetry.subscribe") {
        topics.clear(); if (const auto* a = prop(payload, "topics").getArray()) for (const auto& t : *a) topics.push_back(t.toString()); return result(ack(id));
    }
    if (op == "editor.setFlags") {
        const auto flags = prop(payload, "flags"); if (!flags.isObject()) return result(error(id, "invalid", "Expected flags object"));
        for (const auto& f : flags.getDynamicObject()->getProperties()) {
            if (!f.value.isBool() || !(f.name.toString() == "navLocked" || f.name.toString() == "outlinerVisible" || f.name.toString() == "mixerCompact" || f.name.toString() == "inputMeters" || f.name.toString() == "keepRouting")) return result(error(id, "invalid", "Unknown or invalid editor flag"));
        }
        const auto set = [&](const char* k, auto fn) { if (flags.hasProperty(k)) (proc.*fn)((bool)prop(flags, k)); };
        set("navLocked", &VerbProcessor::setNavigationLocked); set("outlinerVisible", &VerbProcessor::setOutlinerVisible); set("mixerCompact", &VerbProcessor::setMixerCompactEnabled); set("inputMeters", &VerbProcessor::setInputMetersEnabled); set("keepRouting", &VerbProcessor::setKeepRoutingEnabled);
        proc.bumpRevision(); return array({ack(id), snapshot()});
    }
    if (op == "preferences.set") {
        const auto patch = prop(payload, "patch");
        if (!patch.isObject() || juce::JSON::toString(patch).getNumBytesAsUTF8() > 16384) return result(error(id, "invalid", "Invalid preferences patch"));
        for (const auto& p : patch.getDynamicObject()->getProperties()) { if (p.name.toString() == "__proto__" || p.name.toString() == "constructor" || p.name.toString() == "prototype" || p.name.toString() == "editorFlags") return result(error(id, "invalid", "Use typed editor.setFlags for document flags")); }
        for (const auto& p : patch.getDynamicObject()->getProperties()) preferences.getDynamicObject()->setProperty(p.name, p.value.clone());
        if (preferenceLocation != juce::File()) { preferenceLocation.getParentDirectory().createDirectory(); if (!preferenceLocation.replaceWithText(juce::JSON::toString(preferences))) return result(error(id, "ioError", "Could not write preferences")); }
        if (patch.hasProperty("ui") && uiSwitch) uiSwitch(prop(patch, "ui").toString() == "native");
        return array({ack(id), snapshot()});
    }
    if (op == "preset.list" || op == "catalogs.list") return array({ack(id), envelope("event", catalogs(id))});
    if (op == "preset.save" || op == "preset.load" || op == "ir.export") {
        if (chooser) return result(error(id, "notReady", "A file chooser is already open"));
        if ((op == "preset.save" || op == "ir.export") && proc.isRoomLoadPending()) return result(error(id,"notReady","Wait for the room load"));
        if (op == "ir.export" && proc.getExportState() == 1) return result(error(id,"notReady","IR export is already running"));
        if (op == "preset.load" && prop(payload, "source").toString() == "builtin") {
            const auto rooms = verbp::builtinRooms(); const int index = (int)prop(payload, "id");
            if (index < 0 || index >= (int)rooms.size()) return result(error(id, "invalid", "Unknown built-in room"));
            const auto& r = rooms[(size_t)index]; if (!r.setup.empty()) proc.loadRoomWithSetup(r.text, r.name, r.setup, r.micPattern); else proc.loadSceneText(r.text, r.name, true);
        } else if (op == "preset.load" && prop(payload, "source").toString() == "user") {
            const auto wanted = prop(payload, "id").toString(); auto it = std::find_if(userRooms.begin(), userRooms.end(), [&](const juce::File& f) { return f.getFileName() == wanted; });
            if (it == userRooms.end()) return result(error(id, "invalid", "Unknown user preset")); loadFile(*it);
        } else fileCommand(payload, id);
        return array({ack(id), snapshot()});
    }
    if (op == "rt60.start") { if (proc.getTargetRT60State() == 1) return result(error(id, "notReady", "RT60 target worker is already running")); const double target = (double)prop(payload, "targetS"); if (!std::isfinite(target) || target <= 0 || target > 20) return result(error(id, "invalid", "Invalid RT60 target")); proc.startTargetRT60(target); return result(ack(id, "rt60")); }
    if (op == "rt60.cancel") { proc.cancelTargetRT60(); return result(ack(id)); }
    if (op == "analysis.start") { if (!proc.isWebSceneCurrent()) return result(error(id, "notReady", "Wait for current modeled geometry")); if (analysisRunning) return result(error(id, "notReady", "Analysis is running")); startAnalysis(payload); return result(ack(id, analysisId)); }
    if (op == "analysis.cancel") { analysisCancel = true; return result(ack(id)); }
    if (op == "reflections.query") {
        const auto source=prop(payload,"sourceIndex"), order=payload.getProperty("maxOrder",1), count=payload.getProperty("maxPaths",128);
        if (!source.isInt() || (int)source < 0 || (int)source >= (int)proc.getSources().size() || !order.isInt() || (int)order < 0 || (int)order > 1 || !count.isInt() || (int)count < 1 || (int)count > 256) return result(error(id,"invalid","Invalid reflection source/order/count"));
        pathsRequest = payload.clone(); pathsDirty = true; return array({ack(id), envelope("event", reflectionPaths(payload))}); }
    auto r = proc.getEditorSession().execute(message); juce::Array<juce::var> replies;
    if (r.ok || r.resync) clutterDirty = true;
    for (const auto& m : r.messages) replies.add(m);
    if (r.ok || r.resync) replies.add(snapshot()); // authoritative canonical state; includes list reindex/load/undo changes
    return replies;
}
void EditorBridge::suspend() { visible = false; proc.setWebMetersActive(false); parameters.endAll(); proc.getEditorSession().abortTransaction(); }
void EditorBridge::setPreferenceLocation(const juce::File& f) { preferenceLocation = f; if (f.existsAsFile()) { const auto p = juce::JSON::parse(f); if (p.isObject()) preferences = p; } }
void EditorBridge::windowSize(int w, int h) { preferences.getDynamicObject()->setProperty("windowWidth", std::clamp(w, 900, 2600)); preferences.getDynamicObject()->setProperty("windowHeight", std::clamp(h, 600, 1800)); }
std::pair<int, int> EditorBridge::preferredSize() const { return {std::clamp((int)preferences.getProperty("windowWidth", 1280), 900, 2600), std::clamp((int)preferences.getProperty("windowHeight", 800), 600, 1800)}; }

juce::var EditorBridge::catalogs(const juce::String& id) {
    juce::Array<juce::var> materials, rooms, users;
    for (const auto& m : verb::surfacePresets()) materials.add(materialJson(m));
    const auto builtins = verbp::builtinRooms(); for (size_t i = 0; i < builtins.size(); ++i) rooms.add(obj({{"id", (int)i}, {"name", juce::String(builtins[i].name)}}));
    userRooms.clear();
    const auto dir = libraryDir();
    if (dir.isDirectory()) { for (const auto& f : dir.findChildFiles(juce::File::findFiles, false, "*.txt;*.verbroom")) { if (userRooms.size() >= 4096) break; userRooms.push_back(f); } }
    std::sort(userRooms.begin(), userRooms.end(), [](const juce::File& a, const juce::File& b) { return a.getFileName().compareIgnoreCase(b.getFileName()) < 0; });
    for (const auto& f : userRooms) users.add(obj({{"id", f.getFileName()}, {"name", f.getFileNameWithoutExtension()}, {"format", f.hasFileExtension("verbroom") ? "verbroom" : "txt"}}));
    return obj({{"event", "catalogs.list"}, {"requestId", id}, {"materials", materials}, {"builtinRooms", rooms}, {"userPresets", users}});
}
bool EditorBridge::loadFile(const juce::File& f) {
    if (!f.existsAsFile() || f.getSize() > (16 << 20)) { if (emit) emit("verb.message", error({}, "ioError", "Preset does not exist or exceeds 16 MiB")); return false; }
    if (f.hasFileExtension("verbroom")) { juce::String why; if (!proc.loadRoomPresetText(f.loadFileAsString(), why)) { if (emit) emit("verb.message", error({}, "invalid", why)); return false; } }
    else if (f.hasFileExtension("txt")) {
        try { const auto text = f.loadFileAsString(); std::istringstream input(text.toStdString()); const auto parsed = verb::parseScene(input, f.getFileName().toStdString()); (void)parsed; proc.loadSceneText(text, f.getFileNameWithoutExtension(), true); }
        catch (const std::exception& e) { if (emit) emit("verb.message", error({}, "invalid", e.what())); return false; }
    } else { if (emit) emit("verb.message", error({}, "invalid", "Use .verbroom or .txt")); return false; }
    return true;
}
void EditorBridge::fileCommand(const juce::var& p, const juce::String& requestId) {
    if (chooser) { if (emit) emit("verb.message", error(requestId, "notReady", "A file chooser is already open")); return; }
    const auto op = prop(p, "op").toString(); const bool save = op == "preset.save", ir = op == "ir.export";
    emitEvent(obj({{"event","status.file"},{"operation",op},{"state","choosing"},{"message",save ? "Choose a room preset file to save" : ir ? "Choose an IR export folder" : "Choose a room preset to load"}}));
    libraryDir().createDirectory();
    const auto name = juce::File::createLegalFileName(p.getProperty("name", proc.getSceneName()).toString()).substring(0, 128);
    chooser = std::make_unique<juce::FileChooser>(save ? "Save full room preset (.verbroom)" : ir ? "Folder for impulse responses" : "Load room preset / geometry", save ? libraryDir().getChildFile(name + ".verbroom") : libraryDir(), ir ? "" : save ? "*.verbroom" : "*.verbroom;*.txt", true);
    const std::weak_ptr<int> weak = lifetime;
    const int flags = (save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting : juce::FileBrowserComponent::openMode) | (ir ? juce::FileBrowserComponent::canSelectDirectories : juce::FileBrowserComponent::canSelectFiles);
    chooser->launchAsync(flags, [this, weak, op, requestId](const juce::FileChooser& c) {
        if (weak.expired()) return; const auto file = c.getResult();
        if (file == juce::File()) { emitEvent(obj({{"event","status.file"},{"operation",op},{"state","cancelled"},{"message","File selection cancelled"}})); }
        else if (op == "preset.save") {
            const auto target = file.withFileExtension("verbroom");
            const auto write = [this, weak, target, requestId, op] { if (weak.expired()) return; const bool ok = target.replaceWithText(proc.getRoomPresetText()); if (!ok && emit) emit("verb.message", error(requestId, "ioError", "Could not write full room preset")); emitEvent(obj({{"event","status.file"},{"operation",op},{"state",ok ? "done" : "error"},{"message",ok ? "Saved " + target.getFileName() : "Could not save room preset"}})); emitEvent(catalogs(requestId)); };
            // warnAboutOverwriting covers the selected file. An appended/changed extension has a
            // DIFFERENT target, so confirm that actual target too rather than bypassing the warning.
            if (target != file && target.existsAsFile()) juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon, "Replace room preset?", target.getFileName(), "Replace", "Cancel", nullptr,
                juce::ModalCallbackFunction::create([this, weak, write, op](int result) { if (weak.expired()) return; if (result == 1) write(); else emitEvent(obj({{"event","status.file"},{"operation",op},{"state","cancelled"},{"message","Save cancelled"}})); }));
            else write();
        }
        else if (op == "ir.export") { proc.startExportIR(file); emitEvent(obj({{"event","status.file"},{"operation",op},{"state","done"},{"message","IR export started"}})); }
        else { const bool ok = loadFile(file); emitEvent(obj({{"event","status.file"},{"operation",op},{"state",ok ? "loading" : "error"},{"message",ok ? "Loading " + file.getFileName() : "Could not load room preset"}})); }
        chooser.reset(); publishSnapshot(); emitEvent(catalogs(requestId));
    });
}
namespace {
// The engine uses -1 for "no measurable decay" (e.g. receiver outside the room). The v1 contract only allows >= 0, so the wire says 0 (= unknown) instead of failing validation.
template<class T> juce::var unknownAsZero(const T& v) { juce::Array<juce::var> a; for (auto x : v) a.add(juce::var(std::isfinite((double)x) && x > 0 ? (double)x : 0.)); return a; }
inline double wireRt60(double x) { return std::isfinite(x) && x > 0 ? x : 0.; }
template<class T> juce::var matrix(const T& v) { juce::Array<juce::var> a; for (const auto& row : v) a.add(numbers(row)); return a; }
}
void EditorBridge::tick(bool isVisible, double now) {
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (!isVisible || !ready) { if (visible) suspend(); return; }
    if (!documentAvailable && proc.getLoadState() != 2) return;
    const bool resumed = !visible; visible = true;
    if (resumed) { lastEvents.clear(); lastDiagnostics.reset(); lastMeterSent = -1e20; }
    const auto wants = [&](const char* topic) { return std::find(topics.begin(), topics.end(), juce::String(topic)) != topics.end(); };
    proc.setWebMetersActive(wants("meters"));
    const bool paramChanged = parameters.changed();
    const bool sceneCurrent=proc.isWebSceneCurrent();
    if(sceneCurrent!=clutterSceneCurrentSeen){clutterSceneCurrentSeen=sceneCurrent;clutterDirty=true;}
    if (modelSeen != proc.getWebGeometrySerial()) { modelSeen = proc.getWebGeometrySerial(); pathsDirty = true; clutterDirty = true; }
    if (paramChanged || proc.apvts.getRawParameterValue("duck")->load()>0) { pathsDirty = true; if (emit) emit("verb.parameters", parameters.snapshot(tailCurrent)); }
    if (resumed || sentRevision != proc.getRevision() || sentLoad != proc.getLoadSerial() || lastState != proc.getLoadState()) {
        if (!resumed && sentRevision != proc.getRevision()) {
            proc.getEditorSession().abortTransaction();
            emitEvent(obj({{"event", "resync.required"}, {"reason", "structuralChange"}, {"currentRevision", decimal(proc.getRevision())}}));
        }
        if (sentLoad != proc.getLoadSerial()) parameters.endAll();
        pathsDirty = true; clutterDirty = true; publishSnapshot();
    }
    if (wants("meters") && now - lastMeters >= 1000.0 / 30.0) {
        lastMeters = now; juce::Array<juce::var> peaks; const int n = proc.hostInputChannels(); for (int i = 0; i < n; ++i) peaks.add(proc.takeWebInputPeak(i));
        const bool audible = std::any_of(peaks.begin(), peaks.end(), [](const juce::var& p) { return (double)p > 1e-5; });
        if (audible || now - lastMeterSent >= 1000) { lastMeterSent = now; emitEvent(obj({{"event", "meters.input"}, {"seq", (int)(meterSeq++ & 0x7fffffff)}, {"hostInputChannels", n}, {"peaks", peaks}})); }
    }
    // Atomic/immutable processor publications; no engineLock on this timer.
    const bool previousTailCurrent = tailCurrent; tailCurrent = proc.isWebTailCurrent();
    const auto blocks=proc.webAudioBlockCount();
    if(lastAudioActivity<0 || blocks!=audioBlocksSeen){lastAudioActivity=now;audioBlocksSeen=blocks;}
    const bool audioActive=now-lastAudioActivity<750.;
    if(blocks!=cpuBlocksSeen){lastCpuBlockAt=now;cpuBlocksSeen=blocks;}
    const bool cpuActive=lastCpuBlockAt>=-1e10 && now-lastCpuBlockAt<750.;
    const auto cpuTel=proc.webCpuTelemetry();
    const int cpuBlockSize=cpuTel.blockSize>0?cpuTel.blockSize:256;
    const double cpuSr=cpuTel.sampleRate>0?cpuTel.sampleRate:48000.;
    const int cpuOverruns=(int)std::min<uint64_t>(cpuTel.overruns,0x7fffffff);
    const bool cpuMetaChanged=cachedCpu.isObject() && ((int)prop(cachedCpu,"overruns")!=cpuOverruns || (int)prop(cachedCpu,"blockSize")!=cpuBlockSize || std::abs((double)prop(cachedCpu,"sampleRate")-cpuSr)>0.5);
    if(!cachedCpu.isObject() || resumed || cpuActive!=lastCpuActive || cpuMetaChanged || (cpuActive && now-lastCpuSample>=400.)) {
        lastCpuActive=cpuActive; lastCpuSample=now;
        const double meanPct=cpuActive?std::round((double)cpuTel.meanRatio*100.0):0.0;
        const double worstPct=cpuActive?std::round((double)cpuTel.worstRatio*100.0):0.0;
        cachedCpu=obj({{"active",cpuActive},{"meanPercent",meanPct},{"worstPercent",worstPct},{"overruns",cpuOverruns},{"recentOverrun",cpuActive&&cpuTel.recentOverrun},{"blockSize",cpuBlockSize},{"sampleRate",cpuSr}});
    }
    // A stopped/suspended host cannot consume a ready convolver swap or send
    // requested decay to the engine. That is waiting for audio, not computing.
    const int loadState=proc.getLoadState();
    const bool pending=loadState==1 || (loadState==2 && (!proc.isWebSceneCurrent() || (audioActive&&!tailCurrent)));
    if (!pending) rebuildPendingSince = -1;
    else if (rebuildPendingSince < 0) rebuildPendingSince = now;
    if (emit && (resumed || previousTailCurrent != tailCurrent)) emit("verb.parameters", parameters.snapshot(tailCurrent));
    if (wants("status")) emitEvent(obj({{"event", "status.load"}, {"state", proc.getLoadState()}, {"status", proc.getStatus()}, {"sceneSerial", decimal(proc.getSceneSerial())}, {"loadSerial", decimal(proc.getLoadSerial())}, {"tailCurrent", tailCurrent}, {"rebuilding", pending && now - rebuildPendingSince >= 150}, {"cpu", cachedCpu}}));
    if (now - lastSlow >= 1000) {
        lastSlow = now;
        // Immutable scene-worker publications; neither meter nor curve polling takes engineLock.
        if (wants("status")) {
            emitEvent(obj({{"event", "status.export"}, {"state", proc.getExportState()}, {"message", proc.getExportMessage()}}));
            // Never acquire engineLock (held for the entire audio callback) from UI telemetry.
            // The loader already publishes this immutable diagnostic snapshot.
            std::array<double,3> estimate{{-1.,-1.,-1.}};
            if(const auto d=proc.getWebTailDiagnostics()){
                const auto& b=d->physicalRT60;estimate={{(b[0]+b[1])/2.,(b[2]+b[3]+b[4])/3.,(b[5]+b[6])/2.}};
            }
            const double achieved = proc.getTargetRT60Achieved(); emitEvent(obj({{"event", "status.rt60"}, {"state", proc.getTargetRT60State()}, {"achievedS", achieved >= 0 ? juce::var(achieved) : juce::var()}, {"estimate", obj({{"low", wireRt60(estimate[0])}, {"mid", wireRt60(estimate[1])}, {"high", wireRt60(estimate[2])}})}}));
        }
        if (wants("tail")) if (const auto d = proc.getWebTailDiagnostics(); d && d != lastDiagnostics) { lastDiagnostics = d; emitEvent(obj({{"event", "diagnostics.tail"}, {"tailRevision", decimal(++tailEventRevision)}, {"durationS", d->duration}, {"points", 256},
            {"rt60", unknownAsZero(d->rt60)}, {"physicalRT60", unknownAsZero(d->physicalRT60)}, {"reliableSeconds", numbers(d->reliableSeconds)}, {"active", numbers(d->active)}, {"multipliers", numbers(d->multipliers)},
            {"effectiveMultipliers", numbers(d->effectiveMultipliers)}, {"reshapeStatus", numbers(d->reshapeStatus)}, {"sourceEffectiveMultipliers", matrix(d->sourceEffectiveMultipliers)}, {"sourceReshapeStatus", matrix(d->sourceReshapeStatus)},
            {"decayDb", matrix(d->decayDb)}, {"physicalDecayDb", matrix(d->physicalDecayDb)}, {"raysUsed", d->raysUsed}, {"rayLimit", d->rayLimit}, {"baseRays", d->baseRays}, {"shared", d->shared}, {"adaptive", d->adaptive}, {"needsMoreRays", d->needsMoreRays}})); }
    }
    if (wants("analysis") && now - lastAnalysis >= 100) { lastAnalysis = now; pollAnalysis(); }
    if (analysisDescriptor.isObject() && !staleSent && (paramChanged || sentRevision != proc.getRevision() || analysisRevision != decimal(proc.getRevision()))) {
        if (analysisSignature(analysisSource) != resultSignature) { staleSent = true; emitEvent(obj({{"event", "analysis.stale"}, {"resultId", prop(analysisDescriptor, "resultId")}, {"stale", true}})); }
    }
    // Throttle, never debounce: APVTS already holds the live preview pose. This synchronous,
    // read-only message-thread query uses modeled geometry without an engine rebuild/engineLock.
    // Only a dirty bit is retained (latest pose wins); there are no queued ray jobs. At 30 Hz
    // polling a 60 ms minimum interval yields ~15 Hz, including one exact trailing result.
    if (pathsRequest.isObject() && pathsDirty && now - lastPaths >= 60) { pathsDirty = false; lastPaths = now; emitEvent(reflectionPaths(pathsRequest)); }
    // The engine's actual (seeded) clutter placement, from the same immutable modeled scene the audio engine uses.
    // Throttle placement independently of the acoustic rebuild (latest edit wins).
    if (clutterDirty && now - lastClutter >= 50) {
        clutterDirty = false; lastClutter = now; auto event = clutterObjects();
        const auto signature = juce::JSON::toString(obj({{"objects", prop(event, "objects")}, {"requested", prop(event, "requested")}, {"stale", prop(event, "stale")}, {"preview",prop(event,"preview")}}), true);
        if (signature != lastClutterSignature) { lastClutterSignature = signature; emitEvent(event); }
    }
}

juce::String EditorBridge::analysisSignature(int source) const {
    return decimal(proc.getRevision()) + ":" + juce::String(source) + ":" + juce::JSON::toString(prop(parameters.snapshot(tailCurrent), "values"), true);
}
void EditorBridge::startAnalysis(const juce::var& p) {
    if (analysisWorker.joinable()) analysisWorker.join();
    analysisSource = (int)prop(p, "source");
    auto scene = proc.getEnergyScene(analysisSource, true); const auto effective = prop(parameters.snapshot(tailCurrent), "effective");
    verb::EnergyMapOptions options;
    options.resolution = (int)prop(p, "resolution"); options.rays = (int)prop(p, "rays"); options.height = (double)prop(p, "heightM"); options.duration = (double)prop(p, "durationS"); options.components = 7; options.earlyDb = (double)prop(effective, "earlyDb"); options.tailDb = (double)prop(effective, "tailDb");
    analysisId = "energy-" + juce::String(++analysisCounter); analysisRevision = decimal(proc.getRevision()); analysisInputSignature = analysisSignature(analysisSource);
    analysisCancel = false; analysisProgress = 0; analysisRunning = true;
    { std::lock_guard<std::mutex> lock(analysisMutex); analysisPending.reset(); analysisError.clear(); }
    analysisWorker = std::thread([this, scene = std::move(scene), options] {
        std::shared_ptr<const verb::EnergyMap> result; juce::String why;
        try { result = std::make_shared<const verb::EnergyMap>(verb::analyzeEnergyMap(scene, options, [this] { return analysisCancel.load(); }, [this](double p) { analysisProgress = p; })); }
        catch (const std::exception& e) { why = e.what(); }
        { std::lock_guard<std::mutex> lock(analysisMutex); analysisPending = std::move(result); analysisError = why; }
        analysisRunning = false;
    });
}
void EditorBridge::pollAnalysis() {
    if (analysisId.isEmpty()) return;
    if (analysisRunning) { emitEvent(obj({{"event", "analysis.progress"}, {"jobId", analysisId}, {"state", analysisCancel ? "cancelling" : "running"}, {"progress", analysisProgress.load()}})); return; }
    if (!analysisWorker.joinable()) return;
    analysisWorker.join(); std::shared_ptr<const verb::EnergyMap> result; juce::String why;
    { std::lock_guard<std::mutex> lock(analysisMutex); result = std::move(analysisPending); why = analysisError; }
    if (!result || analysisCancel) {
        emitEvent(obj({{"event", "analysis.result"}, {"jobId", analysisId}, {"outcome", analysisCancel ? "cancelled" : "error"}, {"message", why}})); return;
    }
    const auto& m = *result; const size_t n = m.cells.size(), mask = ((n + 31) / 32) * 4;
    analysisBinary.assign(mask + n * 84, std::byte{0}); for (auto& b : analysisBands) b.assign(mask + n * 4, std::byte{0});
    int valid = 0;
    for (size_t i = 0; i < n; ++i) {
        if (m.valid[i]) { ++valid; analysisBinary[i / 8] |= std::byte(1u << (i % 8)); for (auto& b : analysisBands) b[i / 8] |= std::byte(1u << (i % 8)); }
        const verb::BandEnergy* components[] = {&m.cells[i].direct, &m.cells[i].early, &m.cells[i].tail};
        for (int b = 0; b < 7; ++b) {
            float sum = 0;
            for (int c = 0; c < 3; ++c) {
                const float value = (float)(*components[c])[(size_t)b];
                std::memcpy(analysisBinary.data() + mask + (i * 21 + (size_t)c * 7 + b) * 4, &value, 4);
                if (m.options.components & (1 << c)) sum += value;
            }
            std::memcpy(analysisBands[(size_t)b].data() + mask + i * 4, &sum, 4);
        }
    }
    analysisDescriptor = obj({{"resultId", analysisId}, {"sceneRevision", analysisRevision}, {"nx", m.nx}, {"ny", m.ny}, {"x0", m.x0}, {"y0", m.y0}, {"dx", m.dx}, {"dy", m.dy}, {"heightM", m.options.height}, {"raysUsed", m.raysUsed}, {"validCells", valid},
        {"options", obj({{"source", analysisSource}, {"durationS", m.options.duration}, {"resolution", m.options.resolution}, {"rays", m.options.rays}, {"components", m.options.components}, {"earlyDb", m.options.earlyDb}, {"tailDb", m.options.tailDb}})},
        {"blob", obj({{"layoutVersion", 1}, {"fullUrl", "/analysis/" + analysisId + "/full.bin"}, {"bandUrlTemplate", "/analysis/" + analysisId + "/band/{band}.bin"}, {"maskByteLength", (int)mask}, {"fullByteLength", (int)analysisBinary.size()}})}});
    const auto prefix = "/analysis/" + analysisId;
    resourceCache[prefix + "/full.bin"] = analysisBinary;
    std::vector<std::byte> all(mask + n * 4, std::byte{0}); std::copy(analysisBinary.begin(), analysisBinary.begin() + mask, all.begin());
    for (size_t i = 0; i < n; ++i) { float mean = 0; for (const auto& b : analysisBands) { float value = 0; std::memcpy(&value, b.data() + mask + i * 4, 4); mean += value / 7.f; } std::memcpy(all.data() + mask + i * 4, &mean, 4); }
    resourceCache[prefix + "/band/all.bin"] = std::move(all);
    for (int b = 0; b < 7; ++b) resourceCache[prefix + "/band/" + juce::String(b) + ".bin"] = analysisBands[(size_t)b];
    resourceIds.push_back(analysisId);
    while (resourceIds.size() > 2) { const auto old = "/analysis/" + resourceIds.front() + "/"; for (auto it = resourceCache.begin(); it != resourceCache.end();) { if (it->first.startsWith(old)) it = resourceCache.erase(it); else ++it; } resourceIds.pop_front(); }
    resultSignature = analysisInputSignature; staleSent = false;
    emitEvent(obj({{"event", "analysis.result"}, {"jobId", analysisId}, {"outcome", "done"}, {"result", analysisDescriptor}}));
    if (analysisSignature(analysisSource) != resultSignature) { staleSent = true; emitEvent(obj({{"event", "analysis.stale"}, {"resultId", prop(analysisDescriptor, "resultId")}, {"stale", true}})); }
}
bool EditorBridge::resource(const juce::String& path, std::vector<std::byte>& bytes) const {
    const auto found = resourceCache.find(path);
    if (found == resourceCache.end()) return false;
    bytes = found->second; return true;
}

juce::var EditorBridge::reflectionPaths(const juce::var& request) {
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto started = juce::Time::getMillisecondCounterHiRes();
    constexpr double budgetMs = 6.;
    bool truncated=false, timedOut=false;
    const auto stop=[&]{if(juce::Time::getMillisecondCounterHiRes()-started>=budgetMs)timedOut=true;return timedOut;};
    const int source=std::clamp((int)prop(request,"sourceIndex"),0,31);
    const int maxPaths=std::clamp((int)request.getProperty("maxPaths",128),1,256),maxOrder=std::clamp((int)request.getProperty("maxOrder",1),0,1);
    const auto scene=proc.getEnergyScene(source,true);
    juce::Array<juce::var> paths;
    const auto finitePoint=[](verb::Vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);};
    if(!scene.sources.empty()&&!scene.mats.empty()&&!stop()){
        const verb::Geo geo(scene);
        const double size=proc.webAppliedSize()*.01;
        const double top=scene.ceilMat<0?30.*size:scene.height,c=verb::speedOfSound(scene.tempC);
        const verb::Vec3 receiver=verb::Vec3{proc.apvts.getRawParameterValue("lx")->load(),proc.apvts.getRawParameterValue("ly")->load(),std::clamp((double)proc.apvts.getRawParameterValue("lz")->load(),0.,top/size)}*size;
        const double earlyDb=proc.apvts.getRawParameterValue("mode")->load()>=.5f?proc.apvts.getRawParameterValue("early")->load():0.;
        const auto configs=proc.getSources();std::vector<int> channels;
        for(const auto& pm:verbp::buildPoints(configs))if(pm.source==source)channels.push_back(pm.channel);
        std::vector<int> points;
        if(request.hasProperty("pointIndex"))points.push_back(std::clamp((int)prop(request,"pointIndex"),0,(int)scene.sources.size()-1));
        else for(int i=0;i<(int)scene.sources.size();++i)points.push_back(i);
        const auto add=[&](int pointIndex,const std::vector<verb::Vec3>& vertices,int order,int surf,const float* amp,bool transmitted,bool diffracted){
            if(stop())return;
            if(paths.size()>=maxPaths){truncated=true;return;}
            if(vertices.size()<2||!std::all_of(vertices.begin(),vertices.end(),finitePoint)){truncated=true;return;}
            double length=0;for(size_t i=1;i<vertices.size();++i)length+=verb::norm(vertices[i]-vertices[i-1]);
            if(!std::isfinite(length)||!std::isfinite(c)||c<=0){truncated=true;return;}
            if(length/c>=.75)return;
            const auto& src=scene.sources[(size_t)pointIndex];
            const auto direction=vertices[1]-src.pos;const double distance=verb::norm(direction);
            const double directional=distance>1e-9?verb::dirGain(src,direction*(1./distance)):1.;
            const double gain=directional/std::max(.05,length)*(order?std::pow(10.,earlyDb/20.):1.);
            juce::Array<juce::var> energy;
            for(int band=0;band<7;++band){const double e=gain*gain*amp[band]*amp[band]*std::pow(10.,-verb::airAbsDbPerM(verb::kBandHz[band],scene.tempC,scene.humidity)*length/10.);if(!std::isfinite(e)||e<0){truncated=true;return;}energy.add(e);}
            juce::Array<juce::var> positions,surfaces;for(const auto& p:vertices)positions.add(array({p.x,p.y,p.z}));
            if(surf>=0){const char* kind=surf<geo.W?"wall":surf==geo.floorId()?"floor":"ceiling";int index=surf<geo.W?surf:0;
                if(geo.isPlate(surf)){const auto* pl=&geo.plate(surf);bool found=false;for(size_t i=0;i<scene.floorZones.size();++i)if(&scene.floorZones[i]==pl){kind="floorZone";index=(int)i;found=true;break;}if(!found)for(size_t i=0;i<scene.ceilZones.size();++i)if(&scene.ceilZones[i]==pl){kind="ceilingZone";index=(int)i;break;}}
                surfaces.add(obj({{"kind",kind},{"index",index}}));}
            paths.add(obj({{"sourceIndex",source},{"pointIndex",pointIndex},{"channelIndex",pointIndex<(int)channels.size()?channels[(size_t)pointIndex]:-1},{"receiverIndex",0},{"order",order},{"pointsM",positions},{"surfaces",surfaces},{"lengthM",length},{"delayS",length/c},{"bandEnergy",energy},{"occluded",false},{"transmitted",transmitted},{"diffracted",diffracted}}));
        };
        // Reserve the direct paths for ALL rendered points before any reflection
        // can consume the total cap. Sum and LFE follow buildPoints exactly.
        for(const int pi:points){if(stop())break;const auto& src=scene.sources[(size_t)pi];if(!finitePoint(src.pos)||!finitePoint(receiver)){truncated=true;continue;}
            float direct[7];std::fill_n(direct,7,1.f);
            const bool clear=verb::transmitLeg(geo,src.pos,receiver,-1,-1,direct),screened=direct[3]<.5f;
            if(clear){const bool transmission=std::any_of(direct,direct+7,[](float x){return x<.999999f;});if(scene.diffraction&&!stop())verb::litEdgeAttenuation(geo,src.pos,receiver,c,direct);add(pi,{src.pos,receiver},0,-1,direct,transmission,false);}
            if(scene.diffraction&&(!clear||screened)&&!stop()){
                verb::DiffPath dp;
                bool wall=boundedDiffractedPath(geo,src.pos,receiver,c,dp,clear,stop);
                verb::DiffPath plate;bool rim=verb::singleEdgePath(geo,src.pos,receiver,c,plate,-1,true);
                if(rim&&(!wall||plate.length<dp.length))dp=plate;
                if(wall||rim){
                    std::vector<verb::Vec3> vertices{src.pos,dp.first};
                    if(std::abs(dp.inner-verb::norm(dp.last-dp.first))>1e-6)truncated=true;
                    else{if(dp.inner>1e-6)vertices.push_back(dp.last);vertices.push_back(receiver);add(pi,vertices,0,-1,dp.amp,false,true);}
                }
            }
        }
        // Round-robin by surface keeps channel coverage balanced under the cap.
        if(maxOrder)for(int surf=0;surf<geo.nSurf()&&!stop();++surf)for(const int pi:points){
            if(stop()||paths.size()>=maxPaths){truncated=true;break;}
            if(surf==geo.ceilId()&&!geo.hasCeil)continue;
            const auto& src=scene.sources[(size_t)pi];const auto image=verb::mirror(geo,src.pos,surf);verb::Vec3 point;float amp[7];std::fill_n(amp,7,1.f);
            if(geo.separatedShell(src.pos,receiver,false))continue;
            if(!finitePoint(src.pos)||!finitePoint(image)||!finitePoint(receiver)) {truncated=true;continue;}
            if(!verb::crossSurfaceSoft(geo,receiver,image,surf,point,c,amp)||!finitePoint(point))continue;
            float softAmp[7];std::copy_n(amp,7,softAmp);
            if(stop())break;
            if(!verb::softReflectionLeg(geo,receiver,point,surf,-1,c,amp,1,src.pos)||!verb::softReflectionLeg(geo,point,src.pos,surf,-1,c,amp,2)||!verb::touchCross(geo,receiver,point,src.pos,amp))continue;
            const auto* material=geo.matAt(surf,point.x,point.y);if(!material)continue;
            bool transmission=false;for(int band=0;band<7;++band)transmission|=amp[band]<softAmp[band]*.999999f;
            verb::Vec3 hardPoint;const bool soft=!verb::crossSurface(geo,receiver,image,surf,hardPoint);
            for(int band=0;band<7;++band)amp[band]*=(float)std::sqrt(std::max(0.,verb::reflEnergy(*material,band)*(1-verb::scatterAt(*material,band))));
            add(pi,{src.pos,point,receiver},1,surf,amp,transmission,soft);
        }
    }
    return obj({{"event","diagnostics.paths"},{"revision",decimal(proc.getRevision())},{"sceneSerial",decimal(proc.getSceneSerial())},{"sourceIndex",source},{"pointCount",(int)scene.sources.size()},{"paths",paths},{"truncated",truncated||timedOut},{"stale",timedOut||proc.isRoomLoadPending()||!proc.isWebSceneCurrent()},{"budgetExceeded",timedOut},{"engineOrder",1},{"receiver","listener-centre"},{"queryTimeMs",juce::Time::getMillisecondCounterHiRes()-started}});
}

juce::var EditorBridge::clutterObjects() {
    // withClutter() appends its half-obstacles as the LAST walls, each with the material named "Clutter",
    // standing on the floor (z0 = 0) up to its own height. The modeled scene is immutable and shared with the engine.
    juce::Array<juce::var> objects; int requested = 0, seed = 0;
    auto modeled = proc.getWebModeledScene();
    bool early = false;
    if (modeled && proc.getLoadState() == 2 && !proc.isRoomLoadPending() && !proc.isWebSceneCurrent()) {
        auto raw = proc.getDisplayScene();
        verb::scaleRoomGeometry(raw,proc.webAppliedSize()*.01);
        // Engine::setScene retains its construction-time transducers. Reusing
        // those exact inputs also preserves minDist and RNG rejection order.
        raw.sources = modeled->sources; raw.receivers = modeled->receivers;
        modeled = std::make_shared<const verb::Scene>(verb::withClutter(verb::withRoughness(raw), 128));
        early = true;
    }
    if (modeled) {
        requested = modeled->clutter.count; seed = modeled->clutter.seed;
        const auto isClutter = [&](const verb::Wall& w) { return w.mat >= 0 && w.mat < (int)modeled->mats.size() && modeled->mats[(size_t)w.mat].name == "Clutter" && w.z0 == 0.; };
        size_t first = modeled->walls.size();
        while (first > 0 && (int)(modeled->walls.size() - first) < requested && isClutter(modeled->walls[first - 1])) --first;
        const double top = modeled->ceilMat < 0 ? 30. : modeled->height;
        for (size_t i = first; i < modeled->walls.size() && objects.size() < 1024; ++i) {
            const auto& w = modeled->walls[i];
            objects.add(obj({{"a", array({w.ax, w.ay})}, {"b", array({w.bx, w.by})}, {"z0", w.z0}, {"z1", std::min(w.z1, top)}}));
        }
    }
    return obj({{"event", "diagnostics.clutter"}, {"revision", decimal(proc.getRevision())}, {"sceneSerial", decimal(proc.getSceneSerial())}, {"requested", requested}, {"seed", seed}, {"objects", objects},
        {"stale", !early && (proc.isRoomLoadPending() || !proc.isWebSceneCurrent())}, {"source", "engine"}, {"preview", early}});
}
} // namespace verbweb
