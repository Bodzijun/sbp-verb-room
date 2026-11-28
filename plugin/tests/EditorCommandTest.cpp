// Fast headless test of the C++ editor command model (EditorSession / SceneCommands) and of the two state fixes that
// came with it (edit while a different room loads, persisted clutter minDist). Run as CTest `editor_commands`.
// Everything uses tiny rooms and the cheapest tail settings: the whole test takes seconds, not minutes.
#include <juce_audio_processors/juce_audio_processors.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>

#include "../EditorSession.h"
#include "../PluginProcessor.h"
#include "../RoomLibrary.h"
#include "../SceneCommands.h"
#include "../../src/scene.hpp"
#include "EngineTestAccess.h"

using verbp::CommandResult;
using verbp::EditorSession;
using verbp::Model;

static int fails = 0, passes = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (ok) ++passes; else ++fails;
}

static bool waitReady(VerbProcessor& p, double seconds) {
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while ((juce::Time::getMillisecondCounterHiRes() - t0) < seconds * 1000) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        if (p.getLoadState() == 2) return true;
        if (p.getLoadState() == 3) return false;
    }
    return false;
}

// A box room with optional extra walls; materials Plaster / Carpet / Glass.
static std::string boxText(double w, double d, double h, int extraWalls = 0, bool furniture = false) {
    std::ostringstream o;
    o << "sr 48000\nlength 1.0\nheight " << h << "\norder 1\n"
      << "material Plaster 0.02 0.03 0.04 0.05 0.06 0.07 0.08 0.1 0.2\n"
      << "material Carpet 0.05 0.1 0.25 0.45 0.65 0.7 0.7 0.2 0.6\n"
      << "material Glass 0.18 0.06 0.04 0.03 0.02 0.02 0.02 0.05 0.1\n"
      << "floor Carpet\nceiling Plaster\n"
      << "wall 0 0 " << w << " 0 Plaster\nwall " << w << " 0 " << w << ' ' << d << " Plaster\n"
      << "wall " << w << ' ' << d << " 0 " << d << " Plaster\nwall 0 " << d << " 0 0 Glass\n";
    if (furniture) o << "wall 2 1 3 1 Glass 0 1.2\nzone floor Carpet 1 1 2 1 2 2 1 2\nzone ceiling Plaster at 2.5 1 1 2 1 2 2 1 2\n";
    for (int i = 0; i < extraWalls; ++i) o << "wall 1 1 " << 1.5 + 0.01 * i << " 1.5 Plaster\n";
    o << "listener 1 1 1.5 0\nsource A 1.25 1.25 1.5 0 0 0 0\nsource B 3 2 1.5 0 0 0 0\nreceiver L 1 1 1.5 55 0.5\nreceiver R 1 1 1.5 -55 0.5\n";
    return o.str();
}

static verb::Scene parseText(const std::string& t) { std::istringstream in(t); return verb::parseScene(in, "t"); }

static std::string stateSceneText(VerbProcessor& p) {  // the scene text of the project state exactly as a host would save it
    juce::MemoryBlock mb; p.getStateInformation(mb);
    auto xml = juce::AudioProcessor::getXmlFromBinary(mb.getData(), (int)mb.getSize());
    return xml ? xml->getStringAttribute("sceneText").toStdString() : std::string();
}

// ---------------------------------------------------------------------------------------------------- tiny JSON-schema checker
// Draft 2020-12 subset used by web-ui/schema: $ref/$defs, type, enum, const, required, properties, additionalProperties,
// items, min/max Items/Length/imum, exclusiveMinimum, pattern, anyOf, oneOf, allOf, if/then/else. No external dependency.
struct Schemas {
    std::map<std::string, juce::var> files;
    bool load(const juce::File& dir) {
        for (auto& f : dir.findChildFiles(juce::File::findFiles, false, "*.schema.json")) {
            juce::var v;
            if (juce::JSON::parse(f.loadFileAsString(), v).failed()) return false;
            files[f.getFileName().toStdString()] = v;
        }
        return !files.empty();
    }
    juce::var resolve(const std::string& ref, const std::string& curFile, std::string& fileOut) const {
        std::string file = curFile, ptr;
        const auto hash = ref.find('#');
        const std::string url = hash == std::string::npos ? ref : ref.substr(0, hash);
        if (hash != std::string::npos) ptr = ref.substr(hash + 1);
        if (!url.empty()) file = url.substr(url.find_last_of('/') + 1);
        fileOut = file;
        auto it = files.find(file);
        if (it == files.end()) return {};
        juce::var cur = it->second;
        std::stringstream ss(ptr);
        std::string seg;
        while (std::getline(ss, seg, '/')) {
            if (seg.empty()) continue;
            cur = cur.getProperty(juce::String(seg), {});
        }
        return cur;
    }
    static bool isObj(const juce::var& v) { return v.getDynamicObject() != nullptr; }
    static bool typeOk(const juce::String& t, const juce::var& v) {
        if (t == "object") return isObj(v);
        if (t == "array") return v.isArray();
        if (t == "string") return v.isString();
        if (t == "boolean") return v.isBool();
        if (t == "null") return v.isVoid();
        if (t == "number") return v.isInt() || v.isInt64() || v.isDouble();
        if (t == "integer") return v.isInt() || v.isInt64() || (v.isDouble() && std::floor((double)v) == (double)v);
        return true;
    }
    bool validate(const juce::var& sch, const juce::var& v, const std::string& file, std::string& err, const std::string& path) const {
        if (sch.isBool()) { if (!(bool)sch) err = path + ": schema false"; return (bool)sch; }
        auto* o = sch.getDynamicObject();
        if (!o) return true;
        auto fail = [&](const std::string& m) { if (err.empty()) err = path + ": " + m; return false; };
        if (o->hasProperty("$ref")) {
            std::string f;
            const juce::var t = resolve(o->getProperty("$ref").toString().toStdString(), file, f);
            if (t.isVoid()) return fail("unresolved $ref " + o->getProperty("$ref").toString().toStdString());
            if (!validate(t, v, f, err, path)) return false;
        }
        if (o->hasProperty("type")) {
            const juce::var t = o->getProperty("type");
            bool any = false;
            if (t.isArray()) { for (auto& e : *t.getArray()) any |= typeOk(e.toString(), v); } else any = typeOk(t.toString(), v);
            if (!any) return fail("wrong type, expected " + juce::JSON::toString(t, true).toStdString());
        }
        if (o->hasProperty("const") && !(o->getProperty("const") == v)) return fail("const mismatch");
        if (o->hasProperty("enum")) {
            bool any = false;
            for (auto& e : *o->getProperty("enum").getArray()) any |= (e == v);
            if (!any) return fail("not in enum");
        }
        const bool num = v.isInt() || v.isInt64() || v.isDouble();
        if (num) {
            const double d = (double)v;
            if (o->hasProperty("minimum") && d < (double)o->getProperty("minimum")) return fail("below minimum");
            if (o->hasProperty("maximum") && d > (double)o->getProperty("maximum")) return fail("above maximum");
            if (o->hasProperty("exclusiveMinimum") && d <= (double)o->getProperty("exclusiveMinimum")) return fail("not above exclusiveMinimum");
        }
        if (v.isString()) {
            const int len = v.toString().length();
            if (o->hasProperty("minLength") && len < (int)o->getProperty("minLength")) return fail("string too short");
            if (o->hasProperty("maxLength") && len > (int)o->getProperty("maxLength")) return fail("string too long");
            if (o->hasProperty("pattern") && !std::regex_search(v.toString().toStdString(), std::regex(o->getProperty("pattern").toString().toStdString()))) return fail("pattern mismatch");
        }
        if (v.isArray()) {
            const int n = v.getArray()->size();
            if (o->hasProperty("minItems") && n < (int)o->getProperty("minItems")) return fail("too few items");
            if (o->hasProperty("maxItems") && n > (int)o->getProperty("maxItems")) return fail("too many items");
            if (o->hasProperty("items"))
                for (int i = 0; i < n; ++i) if (!validate(o->getProperty("items"), (*v.getArray())[i], file, err, path + "[" + std::to_string(i) + "]")) return false;
        }
        if (isObj(v)) {
            auto* io = v.getDynamicObject();
            if (o->hasProperty("required"))
                for (auto& r : *o->getProperty("required").getArray()) if (!io->hasProperty(juce::Identifier(r.toString()))) return fail("missing required " + r.toString().toStdString());
            const juce::var props = o->getProperty("properties");
            for (auto& nv : io->getProperties()) {
                const juce::var sub = isObj(props) ? props.getProperty(nv.name.toString(), juce::var()) : juce::var();
                const bool known = isObj(props) && props.getDynamicObject()->hasProperty(nv.name);
                if (known) { if (!validate(sub, nv.value, file, err, path + "/" + nv.name.toString().toStdString())) return false; }
                else if (o->hasProperty("additionalProperties")) {
                    const juce::var ap = o->getProperty("additionalProperties");
                    if (ap.isBool() && !(bool)ap) return fail("unexpected property " + nv.name.toString().toStdString());
                    if (isObj(ap) && !validate(ap, nv.value, file, err, path + "/" + nv.name.toString().toStdString())) return false;
                }
            }
        }
        if (o->hasProperty("allOf"))
            for (auto& s : *o->getProperty("allOf").getArray()) if (!validate(s, v, file, err, path)) return false;
        if (o->hasProperty("anyOf")) {
            bool any = false;
            for (auto& s : *o->getProperty("anyOf").getArray()) { std::string e2; any |= validate(s, v, file, e2, path); }
            if (!any) return fail("matches no anyOf branch");
        }
        if (o->hasProperty("oneOf")) {
            int n = 0;
            for (auto& s : *o->getProperty("oneOf").getArray()) { std::string e2; n += validate(s, v, file, e2, path) ? 1 : 0; }
            if (n != 1) return fail("matches " + std::to_string(n) + " oneOf branches");
        }
        if (o->hasProperty("if")) {
            std::string e2;
            if (validate(o->getProperty("if"), v, file, e2, path)) { if (o->hasProperty("then") && !validate(o->getProperty("then"), v, file, err, path)) return false; }
            else if (o->hasProperty("else") && !validate(o->getProperty("else"), v, file, err, path)) return false;
        }
        return true;
    }
};
static Schemas schemas;
static int validatedMessages = 0;
static bool validateEnvelope(const juce::var& msg, std::string* why = nullptr) {
    std::string err;
    const juce::var root = schemas.files["envelope.schema.json"];
    const bool ok = schemas.validate(root, msg, "envelope.schema.json", err, "");
    if (why) *why = err;
    ++validatedMessages;
    return ok;
}

// ---------------------------------------------------------------------------------------------------- state comparison
// Exact scene text (what is saved and loaded), parameters within their quantisation, everything else structurally.
static bool modelEq(const Model& a, const Model& b, std::string* why = nullptr) {
    Model x = a, y = b;
    verbp::normalizeScene(x.scene); verbp::normalizeScene(y.scene);
    if (verb::serializeScene(x.scene) != verb::serializeScene(y.scene)) { if (why) *why = "scene text"; return false; }
    for (const auto& c : verbp::diffModels(a, b)) {
        using verbp::KeyType;
        switch (c.key.type) {
        case KeyType::Surface: case KeyType::SurfaceList: case KeyType::Height: case KeyType::Air: case KeyType::Clutter: case KeyType::Rough: case KeyType::Diffraction:
            continue;  // covered by the scene text (the display scene is re-parsed from 6-digit text after an engine rebuild)
        case KeyType::Param:
            if (std::fabs(std::get<float>(c.before) - std::get<float>(c.after)) < 1e-3f) continue;
            [[fallthrough]];
        default:
            if (why) *why = c.key.path();
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------- the rig
static juce::var J(const std::string& s) {
    juce::var v;
    if (juce::JSON::parse(juce::String(s), v).failed()) { std::printf("bad JSON in test: %s\n", s.c_str()); std::abort(); }
    return v;
}
static std::string U(uint64_t v) { return std::to_string(v); }

struct Rig {
    VerbProcessor p;
    double clockMs = 1e6;
    int reqCounter = 0;
    bool validateCommands = true;  // off for deliberately invalid commands (their ANSWERS are still validated)
    explicit Rig(int inputs = 2) {
        p.setPlayConfigDetails(inputs, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        p.setTailSettings({8000, false});
        session().setClock([this] { return clockMs; });
    }
    EditorSession& session() { return p.getEditorSession(); }
    bool load(const std::string& text, const char* name = "test room") {
        const auto t0 = std::chrono::steady_clock::now();
        p.loadSceneText(juce::String(text), name, true);
        const bool ok = waitReady(p, 90);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (sec > 1.5) std::printf("    (load took %.1f s: %d walls, %d sources)\n", sec, (int)parseText(text).walls.size(), (int)p.getSources().size());
        return ok;
    }
    Model model() { return verbp::readModel(p); }
    // Undoes every step (all tests undo what they do) so that the next test starts from the same room without a new engine build.
    void reset(const char* what) {
        auto& s = session();
        if (s.transactionOpen()) s.abortTransaction();
        while (s.canUndo()) s.undo();
        check(!s.canUndo() && !s.transactionOpen(), std::string("history empty, same room reused: ") + what);
    }
    // Sends a command envelope; validates it and every answer against the contract schemas when the op is in them.
    CommandResult send(const std::string& payload, const char* base = nullptr, const char* tx = nullptr, const char* reqId = nullptr, bool advance = true) {
        if (advance) clockMs += 5000;
        auto* env = new juce::DynamicObject();
        env->setProperty("schemaVersion", 1);
        env->setProperty("sessionId", "test-session");
        env->setProperty("kind", "command");
        env->setProperty("requestId", juce::String(reqId ? reqId : ("r-" + std::to_string(++reqCounter)).c_str()));
        const juce::var pl = J(payload);
        env->setProperty("baseRevision", juce::String(base ? std::string(base) : U(p.getRevision())));
        if (base && std::string(base) == "-") env->removeProperty("baseRevision");
        if (tx) env->setProperty("transactionId", juce::String(tx));
        env->setProperty("payload", pl);
        const juce::var e(env);
        const std::string op = pl.getProperty("op", {}).toString().toStdString();
        const bool inSchema = op != "listener.set" && op != "layers.set" && payload.find("minDistM") == std::string::npos;  // extensions are not in the frozen schema
        std::string why;
        if (inSchema && validateCommands && base == nullptr && !validateEnvelope(e, &why)) check(false, "command envelope violates the schema: " + op + " " + why);
        CommandResult r = session().execute(e);
        for (const auto& m : r.messages) {
            if (!validateEnvelope(m, &why)) check(false, "answer to " + op + " violates the schema: " + why + " :: " + juce::JSON::toString(m, true).toStdString());
        }
        return r;
    }
    const verb::Wall& wall(int i) { static verb::Scene s; s = p.getDisplayScene(); return s.walls[(size_t)i]; }
};

static const std::string kRef0 = R"({"kind":"wall","index":0})";

// ---------------------------------------------------------------------------------------------------- part A
static void testA2MinDist() {
    std::printf("A2: clutter minDist is persisted compatibly\n");
    verb::Scene s = parseText(boxText(4, 3, 3));
    s.clutter.count = 7; s.clutter.minDist = 1.25; s.clutter.seed = 9;
    const std::string text = verb::serializeScene(s);
    const verb::Scene back = parseText(text);
    check(std::fabs(back.clutter.minDist - 1.25) < 1e-9 && back.clutter.count == 7 && back.clutter.seed == 9, "minDist survives serialize -> parse");
    check(verb::serializeScene(back) == text, "serialize(parse(serialize(s))) is identical");
    s.clutter.count = 0; s.clutter.minDist = 0.8;
    const verb::Scene off = parseText(verb::serializeScene(s));
    check(std::fabs(off.clutter.minDist - 0.8) < 1e-9 && off.clutter.count == 0, "minDist also survives with clutter count 0");
    verb::Scene plain = parseText(boxText(4, 3, 3));
    check(verb::serializeScene(plain).find("clutter") == std::string::npos, "default minDist and count 0: no clutter line (old text unchanged)");
    const std::string old = boxText(4, 3, 3) + "clutter 5 0.4 1.6 2 10 3 0.5 1.8\n";
    const verb::Scene o = parseText(old);
    check(std::fabs(o.clutter.minDist - 0.5) < 1e-9 && o.clutter.count == 5 && o.clutter.seed == 3, "an old 8-token clutter line keeps the default minDist 0.5");
    verb::Scene def = o; def.clutter.minDist = 0.5;
    check(verb::serializeScene(def).find("clutter 5 0.4 1.6 2 10 3 0.5 1.8 0.5") != std::string::npos, "the clutter line always carries the 9th token when it is written");
    // files for the archived-0.1.15-parser check (out/T3/compat/check_old_t3.cpp, built against archive/pre-stage2-2026-11-10)
    juce::File("out/T3/compat").createDirectory();
    juce::File("out/T3/compat/minDist_new.txt").replaceWithText(juce::String(text));
    juce::File("out/T3/compat/minDist_new_count0.txt").replaceWithText(juce::String(verb::serializeScene(s)));
}

// A1: an edit of the displayed room made while a DIFFERENT room is loading.
static void testA1EditDuringLoad(VerbProcessor& p) {
    std::printf("A1: edit while a different room is loading\n");
    check(waitReady(p, 90), "processor starts with the demo room");
    const std::string textB = boxText(5, 4, 2.5);
    p.loadSceneText(juce::String(textB), "Room B", false);
    const bool stillLoading = p.getLoadState() == 1;
    p.editSceneMetadata([](verb::Scene& s) { if (!s.walls.empty()) { s.walls[0].editor.locked = true; s.walls[0].editor.name = "A edit"; } });
    p.editScene([](verb::Scene& s) { s.height = 7.0; });
    const std::string midText = stateSceneText(p);
    check(stillLoading, "room B was still loading when the edits arrived");
    check(midText.find("A edit") == std::string::npos && midText.find("height 7") == std::string::npos,
          "while loading, the project state does not contain the edit of the old room");
    check(parseText(midText).walls.size() == 4 && std::fabs(parseText(midText).height - 2.5) < 1e-9,
          "while loading, the project state holds the PENDING room B, not the serialisation of room A");
    check(waitReady(p, 90), "room B finishes loading");
    const verb::Scene shown = p.getDisplayScene();
    const std::string finalText = stateSceneText(p);
    check(std::fabs(shown.height - 2.5) < 1e-9 && !shown.walls[0].editor.locked && shown.walls[0].editor.name.empty(),
          "the pending room wins: the discarded edits are not on the displayed room B");
    check(parseText(finalText).walls.size() == 4 && std::fabs(parseText(finalText).height - 2.5) < 1e-9 && finalText.find("A edit") == std::string::npos,
          "after the load the saved text is room B without the old edit");
    check(verb::serializeScene(parseText(finalText)) == verb::serializeScene(shown), "saved text and displayed room agree");
    p.editScene([](verb::Scene& s) { s.height = 3.5; });
    check(std::fabs(p.getDisplayScene().height - 3.5) < 1e-9 && parseText(stateSceneText(p)).height == 3.5, "edits work again once the room is displayed");
}

// ---------------------------------------------------------------------------------------------------- part B
struct Step { std::string name, json; };

static void testEveryCommandApplyUndoRedo(Rig& r) {
    std::printf("B1: every command: apply -> undo -> redo gives identical state; then unwind and rewind the whole sequence\n");
    check(r.load(boxText(5, 4, 3, 0, true)), "test room (4 walls + partial wall, floor patch, ceiling plate) loads");
    auto& s = r.session();
    const auto& pr = verb::surfacePresets();
    const std::string preset = pr[20].name, preset2 = pr[10].name;
    const std::vector<Step> steps = {
        {"surface.translate", R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.5,0.25]})"},
        {"surface.moveVertex (auto-linked corner)", R"({"op":"surface.moveVertex","ref":{"kind":"wall","index":1},"vertex":0,"positionM":[5.25,0.25]})"},
        {"surface.moveVertex (zone corner)", R"({"op":"surface.moveVertex","ref":{"kind":"floorZone","index":0},"vertex":2,"positionM":[2.5,2.5]})"},
        {"surface.create wall", R"({"op":"surface.create","kind":"wall","a":[1,1],"b":[1.75,2.25],"preset":")" + preset + R"("})"},
        {"surface.create floorZone", R"({"op":"surface.create","kind":"floorZone","vertices":[[3,2],[4,2],[4,3],[3,3]]})"},
        {"surface.create ceilingZone plate", R"({"op":"surface.create","kind":"ceilingZone","vertices":[[3,1],[4,1],[4,2],[3,2]],"heightM":2.25})"},
        {"surface.create ceilingZone opening", R"({"op":"surface.create","kind":"ceilingZone","vertices":[[0.5,0.5],[1,0.5],[1,1],[0.5,1]],"material":null})"},
        {"surface.applyPreset", R"({"op":"surface.applyPreset","refs":[{"kind":"wall","index":2}],"preset":")" + preset2 + R"("})"},
        {"surface.setAbsorption", R"({"op":"surface.setAbsorption","refs":[{"kind":"wall","index":3}],"bands":[{"band":0,"alpha":0.5},{"band":3,"alpha":0.75}]})"},
        {"surface.setTransmission", R"({"op":"surface.setTransmission","refs":[{"kind":"wall","index":4}],"bands":[{"band":2,"tau":0.25},{"band":5,"tau":0.125}]})"},
        {"surface.setScatter", R"({"op":"surface.setScatter","refs":[{"kind":"wall","index":2}],"scatterLF":0.5,"scatterHF":null})"},
        {"surface.setHeights (wall)", R"({"op":"surface.setHeights","refs":[{"kind":"wall","index":4}],"zBottomM":0.25,"zTopM":2})"},
        {"surface.setHeights (zone to plate)", R"({"op":"surface.setHeights","refs":[{"kind":"floorZone","index":0}],"heightM":0.5})"},
        {"surface.setMetadata (rename + hide)", R"({"op":"surface.setMetadata","refs":[{"kind":"wall","index":0}],"name":"Front","hidden":true})"},
        {"surface.setMetadata (floor locked)", R"({"op":"surface.setMetadata","refs":[{"kind":"floor","index":0}],"locked":true})"},
        {"surface.setMetadata (floor unlocked)", R"({"op":"surface.setMetadata","refs":[{"kind":"floor","index":0}],"locked":false})"},
        {"surface.link", R"({"op":"surface.link","refs":[{"kind":"wall","index":0},{"kind":"wall","index":1}],"groupName":"Stage"})"},
        {"surface.unlink", R"({"op":"surface.unlink","refs":[{"kind":"wall","index":0}]})"},
        {"room.scale (with objects)", R"({"op":"room.scale","factor":1.25,"includeObjects":true})"},
        {"room.setHeight", R"({"op":"room.setHeight","heightM":3.5})"},
        {"room.setOpening (ceiling open)", R"({"op":"room.setOpening","ref":{"kind":"ceiling","index":0},"open":true})"},
        {"room.setOpening (ceiling closed)", R"({"op":"room.setOpening","ref":{"kind":"ceiling","index":0},"open":false})"},
        {"room.setAir", R"({"op":"room.setAir","tempC":25,"humidityPct":40})"},
        {"room.setClutter", R"({"op":"room.setClutter","count":6,"minDistM":0.75,"seed":11})"},
        {"room.setRoughness", R"({"op":"room.setRoughness","roughness":0.25,"seed":5})"},
        {"room.setDiffraction", R"({"op":"room.setDiffraction","enabled":false})"},
        {"source.add", R"({"op":"source.add"})"},
        {"source.setConfig (stereo)", R"({"op":"source.setConfig","index":0,"format":1,"name":"Stereo pair"})"},
        {"source.setConfig (color)", R"({"op":"source.setConfig","index":0,"color":"#e04040"})"},
        {"source.setPattern", R"({"op":"source.setPattern","index":1,"pattern":0.5})"},
        {"source.setSpread", R"({"op":"source.setSpread","index":0,"spreadM":1.25})"},
        {"source.setConfig (free points)", R"({"op":"source.setConfig","index":0,"freePoints":true})"},
        {"source.setPointOffset", R"({"op":"source.setPointOffset","index":0,"channel":1,"offsetM":[0.25,0.5,0.125]})"},
        {"source.remove", R"({"op":"source.remove","index":1})"},
        {"output.set", R"({"op":"output.set","layout":2,"micPattern":7})"},
        {"output.set (AmbiX 3rd order)", R"({"op":"output.set","layout":11})"},
        {"output.set (normalization and level)", R"({"op":"output.set","ambisonicsNormalization":2,"ambisonicsLevelDb":-3})"},
        {"tail.setQuality", R"({"op":"tail.setQuality","baseRays":12000,"adaptive":true})"},
        {"listener.set", R"({"op":"listener.set","xM":2,"yM":2.5,"yawDeg":45})"},
        {"surface.delete", R"({"op":"surface.delete","refs":[{"kind":"wall","index":4}]})"},
        {"room.clear", R"({"op":"room.clear"})"},
    };
    std::set<std::string> ops;
    std::vector<Model> states{r.model()};
    int ok = 0;
    for (const auto& st : steps) {
        const Model before = r.model();
        const uint64_t rev0 = r.p.getRevision();
        const int depth0 = s.undoDepth();
        auto res = r.send(st.json);
        const bool applied = res.ok && res.status == "accepted";
        Model after = r.model();
        std::string why;
        bool good = applied && !modelEq(before, after) && r.p.getRevision() > rev0 && s.undoDepth() == depth0 + 1 && !s.undoLabel().empty();
        auto u = s.undo();
        good = good && u.ok && u.status == "accepted" && modelEq(r.model(), before, &why) && s.canRedo();
        auto re = s.redo();
        good = good && re.ok && re.status == "accepted" && modelEq(r.model(), after, &why) && s.undoDepth() == depth0 + 1;
        ok += good;
        if (!good) std::printf("    detail %s: applied=%d code=%s msg=%s why=%s\n", st.name.c_str(), (int)applied, res.code.c_str(), res.message.c_str(), why.c_str());
        check(good, st.name + "  [" + s.undoLabel() + "]");
        states.push_back(r.model());
        ops.insert(juce::JSON::parse(juce::String(st.json)).getProperty("op", {}).toString().toStdString());
    }
    check(ok == (int)steps.size(), "all " + std::to_string(steps.size()) + " command steps passed apply/undo/redo (" + std::to_string(ops.size()) + " distinct ops)");
    // unwind: undo everything, one step at a time, each time landing on the state recorded before that step
    bool unwound = true;
    int n = (int)steps.size();
    std::string why;
    while (s.canUndo()) {
        --n;
        s.undo();
        if (!modelEq(r.model(), states[(size_t)n], &why)) { unwound = false; std::printf("    unwind mismatch before step %d (%s): %s\n", n, steps[(size_t)n].name.c_str(), why.c_str()); break; }
    }
    check(unwound && n == 0, "undoing the whole sequence step by step returns every intermediate state, ending at the loaded room");
    bool rewound = true;
    int m = 0;
    while (s.canRedo()) { s.redo(); ++m; if (!modelEq(r.model(), states[(size_t)m], &why)) { rewound = false; break; } }
    check(rewound && m == (int)steps.size(), "redoing the whole sequence returns every state again");
    while (s.canUndo()) s.undo();
}

static void testScopedUndo(Rig& r) {
    std::printf("B2: scoped undo does not revert unrelated later changes\n");
    r.reset("B2 part 1");
    auto& s = r.session();
    const verb::Scene s0 = r.p.getDisplayScene();
    // 1. a geometry command, then changes of OTHER keys made around the session (the native editor, layer masks)
    check(r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.5,0.25]})").ok, "geometry edit: translate wall 0");
    r.p.editScene([](verb::Scene& sc) { sc.tempC = 31; sc.humidity = 33; sc.clutter.count = 4; sc.roughness = 0.4; });  // air, clutter, roughness (not commands)
    r.p.setLayerMode(0, 1);                                                                                         // layer mask
    r.p.editScene([](verb::Scene& sc) {                                                                             // the material of ANOTHER wall
        verb::Material m = sc.mats[(size_t)sc.walls[1].mat]; m.alpha[2] = 0.75; m.name = "ext"; sc.mats.push_back(m); sc.walls[1].mat = (int)sc.mats.size() - 1;
    });
    const verb::Scene mid = r.p.getDisplayScene();
    auto u = s.undo();
    const verb::Scene after = r.p.getDisplayScene();
    check(u.ok && std::fabs(after.walls[0].ax - s0.walls[0].ax) < 1e-9 && std::fabs(after.walls[0].ay - s0.walls[0].ay) < 1e-9, "undo restores the geometry of wall 0");
    check(after.tempC == 31 && after.humidity == 33 && after.clutter.count == 4 && after.roughness == 0.4, "air, clutter and roughness changed later are NOT reverted");
    check(after.layerModes[0] == 1, "the layer mask set later is NOT reverted");
    check(std::fabs(after.mats[(size_t)after.walls[1].mat].alpha[2] - 0.75) < 1e-9 && after.mats[(size_t)after.walls[1].mat].name == "ext", "the material of another wall changed later is NOT reverted");
    check(u.externalConflicts == 0, "no key of the step was changed elsewhere: no conflict reported");
    (void)mid;
    // 2. two commands of different scope: undo reverts them one at a time, each only its own key
    check(r.load(boxText(5, 4, 3, 0, true)), "room reloads (history cleared)");
    r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.5,0.25]})");
    r.send(R"({"op":"room.setAir","tempC":10,"humidityPct":90})");
    r.send(R"({"op":"surface.setAbsorption","refs":[{"kind":"wall","index":2}],"bands":[{"band":1,"alpha":0.5}]})");
    s.undo();
    auto sc = r.p.getDisplayScene();
    check(sc.tempC == 10 && std::fabs(sc.walls[0].ax - (s0.walls[0].ax + 0.5)) < 1e-9, "first undo reverts only the material (air and geometry stay)");
    s.undo();
    sc = r.p.getDisplayScene();
    check(std::fabs(sc.tempC - s0.tempC) < 1e-9 && std::fabs(sc.walls[0].ax - (s0.walls[0].ax + 0.5)) < 1e-9, "second undo reverts only the air (geometry stays)");
    s.undo();
    check(std::fabs(r.p.getDisplayScene().walls[0].ax - s0.walls[0].ax) < 1e-9, "third undo reverts the geometry");
    // 3. a non-undoable command between: layer mask via the session survives the undo of the earlier geometry step
    r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":1}],"deltaM":[0.25,0]})");
    const int depth = s.undoDepth();
    const uint64_t rev = r.p.getRevision();
    auto lr = r.send(R"({"op":"layers.set","layer":1,"mode":2})");
    check(lr.ok && lr.status == "accepted" && s.undoDepth() == depth && r.p.getRevision() > rev, "layers.set is applied and revisioned but is not an undo step");
    s.undo();
    check(r.p.getDisplayScene().layerModes[1] == 2 && std::fabs(r.p.getDisplayScene().walls[1].ax - s0.walls[1].ax) < 1e-9, "undo of the earlier geometry leaves the layer mask alone");
    check(stateSceneText(r.p).find("#@verbroom-editor-v2 layers 0 2 0 0") != std::string::npos, "the mask is saved with the scene (v2 comment line)");
    // 4. the same key changed elsewhere meanwhile: reported as a conflict, still restored
    r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.5,0]})");
    r.p.editScene([](verb::Scene& sc2) { sc2.walls[0].ax += 0.125; });
    auto cu = s.undo();
    check(cu.ok && cu.externalConflicts >= 1 && std::fabs(r.p.getDisplayScene().walls[0].ax - s0.walls[0].ax) < 1e-9, "a key edited elsewhere meanwhile is reported (externalConflicts) and still restored");
    while (s.canUndo()) s.undo();
}

static void testTransactions(Rig& r) {
    std::printf("B3: drag transactions\n");
    r.reset("B3");
    auto& s = r.session();
    const double ax0 = r.wall(0).ax;
    const int depth0 = s.undoDepth();
    auto b = r.send(R"({"op":"transaction.begin","label":"Drag wall"})", nullptr, "tx-1");
    check(b.ok && s.transactionOpen(), "transaction.begin");
    double last = 0;
    bool allOk = true;
    for (int i = 1; i <= 6; ++i) {
        last = 0.125 * i;
        const std::string base = U(r.p.getRevision());
        auto u = r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[)" + std::to_string(last) + R"(,0]})", base.c_str(), "tx-1");
        allOk = allOk && u.ok && u.status == "accepted";
    }
    check(allOk && std::fabs(r.wall(0).ax - (ax0 + last)) < 1e-9, "6 updates: the wall is at start + LAST delta (deltas are not accumulated)");
    check(s.undoDepth() == depth0, "the history gets no step while the gesture is running");
    // pipelined update: a base revision of an earlier update of the same gesture is accepted
    auto pipelined = r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[1.0,0]})", U(r.p.getRevision() - 1).c_str(), "tx-1");
    check(pipelined.ok && std::fabs(r.wall(0).ax - (ax0 + 1.0)) < 1e-9, "a pipelined update (base = an earlier revision of the gesture) is accepted");
    auto c = r.send(R"({"op":"transaction.commit"})", nullptr, "tx-1");
    check(c.ok && s.undoDepth() == depth0 + 1 && !s.transactionOpen() && s.undoLabel() == "Drag wall", "commit: exactly ONE undo step, with the label of the gesture");
    s.undo();
    check(std::fabs(r.wall(0).ax - ax0) < 1e-9 && s.undoDepth() == depth0, "one undo returns the whole drag");
    s.redo();
    check(std::fabs(r.wall(0).ax - (ax0 + 1.0)) < 1e-9, "one redo re-applies the whole drag");
    s.undo();
    // cancel
    r.send(R"({"op":"transaction.begin"})", nullptr, "tx-2");
    r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.5,0.5]})", nullptr, "tx-2");
    const uint64_t revMoved = r.p.getRevision();
    auto cn = r.send(R"({"op":"transaction.cancel"})", nullptr, "tx-2");
    check(cn.ok && std::fabs(r.wall(0).ax - ax0) < 1e-9 && s.undoDepth() == depth0 && r.p.getRevision() > revMoved, "cancel restores the start, adds no step, and bumps the revision (state changed)");
    // a gesture with two different edits is still one step
    r.send(R"({"op":"transaction.begin","label":"Move and lock"})", nullptr, "tx-3");
    r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":1}],"deltaM":[0.25,0]})", nullptr, "tx-3");
    r.send(R"({"op":"surface.setMetadata","refs":[{"kind":"wall","index":1}],"name":"Right"})", nullptr, "tx-3");
    r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":1}],"deltaM":[0.5,0]})", nullptr, "tx-3");
    r.send(R"({"op":"transaction.commit"})", nullptr, "tx-3");
    check(s.undoDepth() == depth0 + 1 && r.wall(1).editor.name == "Right", "a gesture of two different edits is ONE step");
    s.undo();
    check(r.wall(1).editor.name.empty() && std::fabs(r.wall(1).ax - 5.0) < 1e-9, "its undo reverts both");
    // failing update keeps the transaction
    r.send(R"({"op":"transaction.begin"})", nullptr, "tx-4");
    auto bad = r.send(R"({"op":"surface.moveVertex","ref":{"kind":"wall","index":0},"vertex":0,"positionM":[5,0]})", nullptr, "tx-4");
    check(!bad.ok && bad.code == "invalid" && s.transactionOpen(), "an invalid update is rejected and the gesture stays open");
    r.send(R"({"op":"transaction.cancel"})", nullptr, "tx-4");
    // a command without the transaction id commits the interrupted gesture
    r.send(R"({"op":"transaction.begin"})", nullptr, "tx-5");
    r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.25,0]})", nullptr, "tx-5");
    r.send(R"({"op":"room.setAir","tempC":12})");
    check(!s.transactionOpen() && s.undoDepth() >= depth0 + 2, "a plain command while a gesture is open commits the gesture first (a dropped mouse-up cannot block editing)");
    while (s.canUndo()) s.undo();
}

static void testCoalescingAndNoChange(Rig& r) {
    std::printf("B4: coalescing and repeated identical edits\n");
    r.reset("B4");
    auto& s = r.session();
    const double t0 = r.p.getDisplayScene().tempC;
    auto a1 = r.send(R"({"op":"room.setAir","tempC":22})");
    r.clockMs += 100;
    auto a2 = r.send(R"({"op":"room.setAir","tempC":23})", nullptr, nullptr, nullptr, false);
    r.clockMs += 100;
    auto a3 = r.send(R"({"op":"room.setAir","tempC":24})", nullptr, nullptr, nullptr, false);
    check(a1.ok && a2.ok && a3.ok && s.undoDepth() == 1, "three setAir edits within 750 ms share ONE undo step");
    s.undo();
    check(std::fabs(r.p.getDisplayScene().tempC - t0) < 1e-9, "its undo goes back to the value before the first of them");
    s.redo();
    check(std::fabs(r.p.getDisplayScene().tempC - 24) < 1e-9, "its redo goes to the last value");
    r.send(R"({"op":"room.setAir","tempC":10})");
    check(s.undoDepth() == 2, "an edit after the window is a new step");
    // moving back to where it began leaves nothing to undo
    r.clockMs += 100;
    r.send(R"({"op":"room.setAir","tempC":24})", nullptr, nullptr, nullptr, false);
    check(s.undoDepth() == 1 && std::fabs(r.p.getDisplayScene().tempC - 24) < 1e-9, "an edit moved back to where its (merged) step began leaves no empty step behind");
    const uint64_t rev = r.p.getRevision();
    const int d = s.undoDepth();
    auto same = r.send(R"({"op":"room.setAir","tempC":24})");
    check(same.ok && same.status == "noChange" && r.p.getRevision() == rev && s.undoDepth() == d, "an identical repeated edit is noChange: no revision bump, no step");
    while (s.canUndo()) s.undo();
}

static void testRejections(Rig& r) {
    std::printf("B5: rejected commands change nothing (state, revision, history) and return the contract's error code\n");
    r.reset("B5");
    auto& s = r.session();
    auto expectErr = [&](const std::string& label, const std::string& json, const char* code, const char* base = nullptr) {
        const Model before = r.model();
        const uint64_t rev = r.p.getRevision();
        const int depth = s.undoDepth();
        r.validateCommands = false;
        auto res = r.send(json, base);
        r.validateCommands = true;
        std::string why;
        const bool same = modelEq(before, r.model(), &why) && r.p.getRevision() == rev && s.undoDepth() == depth;
        bool shape = !res.ok && res.code == code && !res.messages.empty();
        if (shape) {
            const juce::var pl = res.messages[0].getProperty("payload", {});
            shape = res.messages[0].getProperty("kind", {}).toString() == "error" && pl.getProperty("code", {}).toString().toStdString() == code && pl.getProperty("currentRevision", {}).toString().toStdString() == U(rev);
        }
        if (!(same && shape)) std::printf("    detail: code=%s (want %s) msg=%s same=%d why=%s\n", res.code.c_str(), code, res.message.c_str(), (int)same, why.c_str());
        check(same && shape, label);
    };
    expectErr("invalid: air out of range", R"({"op":"room.setAir","tempC":99})", "invalid");
    expectErr("invalid: absorption above 1", R"({"op":"surface.setAbsorption","refs":[{"kind":"wall","index":0}],"bands":[{"band":0,"alpha":1.5}]})", "invalid");
    expectErr("invalid: a surface that does not exist", R"({"op":"surface.translate","refs":[{"kind":"wall","index":9}],"deltaM":[1,0]})", "invalid");
    expectErr("invalid: zero-length wall by moving a corner", R"({"op":"surface.moveVertex","ref":{"kind":"wall","index":0},"vertex":0,"positionM":[5,0]})", "invalid");
    expectErr("invalid: zero-length wall on create", R"({"op":"surface.create","kind":"wall","a":[1,1],"b":[1,1]})", "invalid");
    expectErr("invalid: self-crossing zone", R"({"op":"surface.create","kind":"floorZone","vertices":[[0,0],[2,2],[2,0],[0,2]]})", "invalid");
    expectErr("invalid: unknown preset", R"({"op":"surface.applyPreset","refs":[{"kind":"wall","index":0}],"preset":"No such material"})", "invalid");
    expectErr("invalid: heightM on a wall", R"({"op":"surface.setHeights","refs":[{"kind":"wall","index":0}],"heightM":2})", "invalid");
    expectErr("invalid: minLen above maxLen", R"({"op":"room.setClutter","minLenM":2.5,"maxLenM":0.5})", "invalid");
    expectErr("invalid: source that does not exist", R"({"op":"source.setPattern","index":7,"pattern":0.5})", "invalid");
    expectErr("invalid: removing a source that does not exist", R"({"op":"source.remove","index":9})", "invalid");
    expectErr("invalid: only a ceiling can be opened (ref = floor)", R"({"op":"room.setOpening","ref":{"kind":"floor","index":0},"open":true})", "invalid");
    expectErr("invalid: no fields", R"({"op":"room.setAir"})", "invalid");
    expectErr("invalid: Ambisonics normalization", R"({"op":"output.set","ambisonicsNormalization":3})", "invalid");
    expectErr("invalid: Ambisonics level step", R"({"op":"output.set","ambisonicsLevelDb":-4})", "invalid");
    expectErr("invalid: output layout after the last format", R"({"op":"output.set","layout":12})", "invalid");
    expectErr("invalid: baseRevision missing in a mutation", R"({"op":"room.setHeight","heightM":3})", "invalid", "-");
    expectErr("unsupported: unknown op", R"({"op":"room.explode"})", "unsupported");
    expectErr("unsupported: history.redo (capability false in contract v1)", R"({"op":"history.redo"})", "unsupported");
    expectErr("unsupported: preset.list is served elsewhere", R"({"op":"preset.list"})", "unsupported");
    // stale revision: error + resync flag + event, nothing applied
    {
        const std::string old = U(r.p.getRevision() - 1);
        const Model before = r.model();
        auto res = r.send(R"({"op":"room.setHeight","heightM":3.5})", old.c_str());
        const juce::var pl = res.messages.empty() ? juce::var() : res.messages[0].getProperty("payload", {});
        bool ev = false;
        for (const auto& m : res.messages) ev |= m.getProperty("kind", {}).toString() == "event" && m.getProperty("payload", {}).getProperty("event", {}).toString() == "resync.required";
        check(!res.ok && res.code == "staleRevision" && res.resync && (bool)pl.getProperty("resync", false) && ev && modelEq(before, r.model()),
              "staleRevision: error with resync:true + resync.required event, nothing applied");
    }
    // locked objects
    check(r.send(R"({"op":"surface.setMetadata","refs":[{"kind":"wall","index":2}],"locked":true})").ok, "wall 2 gets locked (allowed on a locked surface's own metadata)");
    expectErr("locked: translate", R"({"op":"surface.translate","refs":[{"kind":"wall","index":2}],"deltaM":[1,0]})", "locked");
    expectErr("locked: move a corner", R"({"op":"surface.moveVertex","ref":{"kind":"wall","index":2},"vertex":0,"positionM":[4,4]})", "locked");
    expectErr("locked: delete", R"({"op":"surface.delete","refs":[{"kind":"wall","index":2}]})", "locked");
    expectErr("locked: absorption", R"({"op":"surface.setAbsorption","refs":[{"kind":"wall","index":2}],"bands":[{"band":0,"alpha":0.5}]})", "locked");
    expectErr("locked: preset", R"({"op":"surface.applyPreset","refs":[{"kind":"wall","index":2}],"preset":"Carpet"})", "locked");
    expectErr("locked: heights", R"({"op":"surface.setHeights","refs":[{"kind":"wall","index":2}],"zTopM":2})", "locked");
    expectErr("locked: scale the room", R"({"op":"room.scale","factor":1.5,"includeObjects":false})", "locked");
    expectErr("locked: clear the room", R"({"op":"room.clear"})", "locked");
    // a group with one locked member does not move (native rule)
    check(r.send(R"({"op":"surface.setMetadata","refs":[{"kind":"wall","index":2}],"locked":false})").ok, "wall 2 unlocked again");
    check(r.send(R"({"op":"surface.link","refs":[{"kind":"wall","index":2},{"kind":"wall","index":3}],"groupName":"Pair"})").ok, "walls 2+3 linked");
    check(r.send(R"({"op":"surface.setMetadata","refs":[{"kind":"wall","index":2}],"locked":true})").ok, "wall 2 locked again");
    expectErr("locked: translating wall 3 refuses because its linked partner is locked", R"({"op":"surface.translate","refs":[{"kind":"wall","index":3}],"deltaM":[1,0]})", "locked");
    check(r.send(R"({"op":"surface.setMetadata","refs":[{"kind":"wall","index":2}],"locked":false})").ok, "unlock works");
    check(r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":3}],"deltaM":[0.25,0]})").ok, "and then the linked group moves");
    while (s.canUndo()) s.undo();
    r.reset("B5 locks");
    // budgets. Checked on the command model directly: a real 128-wall room takes ~20 s to build in the engine, and the
    // validation does not need an engine. Two budgets also go through the whole session path (below).
    {
        auto mutateDirect = [&](const Model& m, const std::string& json) {
            const juce::var pl = J(json);
            const auto* d = verbp::findDescriptor(pl.getProperty("op", {}).toString().toStdString());
            const Model start = m;
            Model cand = m;
            verbp::Fail f = d->mutate(pl, cand, verbp::Ctx{r.p, start});
            if (!f.bad()) f = verbp::normalizeAndValidate(cand, start);
            return f;
        };
        auto expectBudget = [&](const std::string& label, const Model& m, const std::string& json, const char* code) {
            const auto f = mutateDirect(m, json);
            if (f.code != code) std::printf("    detail: got '%s' %s\n", f.code.c_str(), f.message.c_str());
            check(f.code == code, label);
        };
        Model walls128 = r.model();
        walls128.scene = parseText(boxText(5, 4, 3, 124));
        check(walls128.scene.walls.size() == 128, "(a model with exactly 128 walls)");
        expectBudget("budgetExceeded: the 129th wall", walls128, R"({"op":"surface.create","kind":"wall","a":[0.5,0.5],"b":[0.9,0.9]})", "budgetExceeded");
        {   // memory of the biggest possible step: the whole wall list, before and after
            Model cand = walls128;
            const Model st = walls128;
            verbp::findDescriptor("room.clear")->mutate(J(R"({"op":"room.clear"})"), cand, verbp::Ctx{r.p, st});
            verbp::normalizeScene(cand.scene);
            size_t bytes = 0;
            for (auto& c : verbp::diffModels(walls128, cand)) bytes += verbp::snapBytes(c.before) + verbp::snapBytes(c.after);
            std::printf("    history memory estimate of room.clear on 128 walls (one whole-list step): %zu bytes\n", bytes);
            check(bytes > 20000 && bytes < 400000, "a whole 128-wall list step is estimated well below 400 KB");
        }
        std::string zones;
        for (int i = 0; i < 8; ++i) zones += "zone floor Carpet at 0." + std::to_string(i + 2) + " 1 1 2 1 2 2 1 2\n";  // 8 plates
        for (int i = 0; i < 24; ++i) zones += "zone floor Plaster 1 1 2 1 2 2 1 2\n";                                      // 32 floor zones in all
        for (int i = 0; i < 24; ++i) zones += "zone ceiling Plaster 1 1 2 1 2 2 1 2\n";
        Model zm = r.model();
        zm.scene = parseText(boxText(5, 4, 3) + zones);
        check(zm.scene.floorZones.size() == 32, "(a model with 32 floor zones, 8 of them plates)");
        expectBudget("budgetExceeded: the 33rd floor zone", zm, R"({"op":"surface.create","kind":"floorZone","vertices":[[3,2],[4,2],[4,3],[3,3]]})", "budgetExceeded");
        expectBudget("budgetExceeded: a 9th plate (new ceiling zone with a height)", zm, R"({"op":"surface.create","kind":"ceilingZone","vertices":[[3,2],[4,2],[4,3],[3,3]],"heightM":2})", "budgetExceeded");
        expectBudget("budgetExceeded: a 9th plate (a patch turned into a plate)", zm, R"({"op":"surface.setHeights","refs":[{"kind":"floorZone","index":20}],"heightM":1})", "budgetExceeded");
        Model sm = r.model();
        sm.sources.assign(32, verbp::SourceCfg{});
        for (int i = 0; i < 32; ++i) sm.sources[(size_t)i].firstIn = i;
        expectBudget("budgetExceeded: the 33rd source", sm, R"({"op":"source.add"})", "budgetExceeded");
        Model pm = r.model();
        pm.sources.assign(10, verbp::SourceCfg{});
        for (int i = 0; i < 9; ++i) { pm.sources[(size_t)i].format = 6; pm.sources[(size_t)i].channels = 8; pm.sources[(size_t)i].firstIn = 0; }  // 9 x 7.1 = 63 points + 1
        check((int)verbp::buildPoints(pm.sources).size() == 64, "(a model with exactly 64 points)");
        expectBudget("budgetExceeded: a 65th point (one more source)", pm, R"({"op":"source.add"})", "budgetExceeded");
        expectBudget("budgetExceeded: a 65th point (a source changed to stereo)", pm, R"({"op":"source.setConfig","index":9,"format":1})", "budgetExceeded");
        expectBudget("budgetExceeded: input channel 65 (firstIn 60 + 8 channels)", pm, R"({"op":"source.setConfig","index":9,"format":6,"firstIn":60})", "budgetExceeded");
    }
    {   // the same refusals through the whole session path, on the real room
        std::string verts = R"({"op":"surface.create","kind":"ceilingZone","vertices":[)";
        for (int i = 0; i < 65; ++i) verts += std::string(i ? "," : "") + "[" + std::to_string(std::cos(i * 0.0967) * 2) + "," + std::to_string(std::sin(i * 0.0967) * 2) + "]";
        verts += "]}";
        expectErr("budgetExceeded through the session: 65 vertices", verts, "budgetExceeded");
        expectErr("budgetExceeded through the session: inputs (firstIn 60 + 8 channels)", R"({"op":"source.setConfig","index":0,"format":6,"firstIn":60})", "budgetExceeded");
    }
    // not ready while a different room loads (the A1 rule seen through the session); the same room is loaded again afterwards
    {
        const uint64_t rev = r.p.getRevision();
        r.p.loadSceneText(juce::String(boxText(5, 4, 3, 0, true)), "other", true);
        auto res = r.send(R"({"op":"room.setHeight","heightM":4})");
        check(!res.ok && res.code == "notReady", "notReady while a different room is loading");
        check(waitReady(r.p, 90), "the other room finishes loading");
        check(r.p.getRevision() > rev && std::fabs(r.p.getDisplayScene().height - 3.0) < 1e-9, "revision grew with the load; the discarded edit is not on the new room");
    }
}

static void testRevisionAndIds(Rig& r) {
    std::printf("B6: revision, the three live setters, duplicate command ids\n");
    r.reset("B6");
    auto& s = r.session();
    // the contract's finding: sceneSerial is not bumped by these three setters, the revision is
    {
        r.p.setSources([&] { auto c = r.p.getSources(); c[0].freePoints = true; c[0].format = 1; c[0].channels = 2; return c; }());
        const auto ser = r.p.getSceneSerial();
        uint64_t rev = r.p.getRevision();
        r.p.setSourcePattern(0, 0.5);
        check(r.p.getRevision() > rev && r.p.getSceneSerial() == ser, "setSourcePattern: revision +, sceneSerial unchanged (the gap found by the contract)");
        rev = r.p.getRevision();
        r.p.setSourceSpread(0, 1.5);
        check(r.p.getRevision() > rev && r.p.getSceneSerial() == ser, "setSourceSpread: revision +, sceneSerial unchanged");
        rev = r.p.getRevision();
        r.p.setPointOffset(0, 1, 0.1f, 0.2f, 0.3f);
        check(r.p.getRevision() > rev && r.p.getSceneSerial() == ser, "setPointOffset: revision +, sceneSerial unchanged");
        rev = r.p.getRevision();
        r.p.setSourcePattern(99, 0.5);
        check(r.p.getRevision() == rev, "a setter that rejects (bad index) does not bump");
    }
    check(r.load(boxText(5, 4, 3, 0, true)), "room reloads");
    // every accepted command bumps; session versions of the three live setters too
    uint64_t prev = r.p.getRevision();
    bool mono = true;
    for (const char* js : {R"({"op":"source.setPattern","index":0,"pattern":0.5})", R"({"op":"source.setSpread","index":0,"spreadM":2})",
                           R"({"op":"source.setConfig","index":0,"freePoints":true})", R"({"op":"source.setPointOffset","index":0,"channel":0,"offsetM":[0.5,0,0]})",
                           R"({"op":"room.setAir","tempC":26})", R"({"op":"listener.set","xM":3})"}) {
        auto res = r.send(js);
        const uint64_t cur = r.p.getRevision();
        mono = mono && res.ok && res.status == "accepted" && cur > prev && res.revision == cur;
        prev = cur;
    }
    check(mono, "pattern / spread / free-points / point-offset / air / listener commands each raise the revision; the ack carries it");
    prev = r.p.getRevision();
    s.undo(); check(r.p.getRevision() > prev, "undo raises the revision"); prev = r.p.getRevision();
    s.redo(); check(r.p.getRevision() > prev, "redo raises the revision");
    prev = r.p.getRevision();
    r.validateCommands = false;
    r.send(R"({"op":"room.setHeight","heightM":99})");  // out of range: rejected
    r.validateCommands = true;
    r.send(R"({"op":"room.setAir","tempC":26})");  // identical to the current value now -> noChange
    check(r.p.getRevision() == prev, "rejected and noChange commands leave the revision alone");
    // duplicate request ids
    const double ax0 = r.wall(0).ax;
    const int depth0 = s.undoDepth();
    auto first = r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.5,0]})", nullptr, nullptr, "dup-1");
    const uint64_t revAfter = r.p.getRevision();
    auto second = r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.5,0]})", U(revAfter).c_str(), nullptr, "dup-1");
    check(first.ok && !first.duplicate && second.ok && second.duplicate && second.revision == first.revision, "the same requestId is recognised as a duplicate and answered with the stored result");
    check(std::fabs(r.wall(0).ax - (ax0 + 0.5)) < 1e-9 && s.undoDepth() == depth0 + 1 && r.p.getRevision() == revAfter, "the duplicate was NOT applied again (moved once, one step, same revision)");
    auto stale = r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.5,0]})", "1", nullptr, "dup-1");
    check(stale.duplicate && stale.ok, "a re-sent id is answered from the cache even if its base revision is long stale");
    auto otherId = r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.5,0]})", nullptr, nullptr, "dup-2");
    check(otherId.ok && !otherId.duplicate && std::fabs(r.wall(0).ax - (ax0 + 1.0)) < 1e-9, "a new id is a new command");
    auto beginEnvelope = J(R"({"schemaVersion":1,"sessionId":"t6-page-a","kind":"command","requestId":"ui-1","transactionId":"old-drag","payload":{"op":"transaction.begin","label":"Old page"}})");
    check(s.execute(beginEnvelope).ok && s.transactionOpen(), "T6 first page opens its transaction");
    beginEnvelope.getDynamicObject()->setProperty("sessionId", "t6-page-b");
    beginEnvelope.getDynamicObject()->setProperty("transactionId", "new-drag");
    const auto reopened = s.execute(beginEnvelope);
    check(reopened.ok && !reopened.duplicate && s.transactionOpen(), "T6 reopened page ui-1 is a new begin, not a cached acknowledgement");
    auto endEnvelope = J(R"({"schemaVersion":1,"sessionId":"t6-page-b","kind":"command","requestId":"ui-2","transactionId":"new-drag","payload":{"op":"transaction.commit"}})");
    check(s.execute(endEnvelope).ok && !s.transactionOpen(), "T6 reopened page can commit the transaction it actually opened");
    while (s.canUndo()) s.undo();
}

static void testHistoryBounds(Rig& r) {
    std::printf("B7: bounded history\n");
    r.reset("B7");
    auto& s = r.session();
    for (int i = 1; i <= 70; ++i) r.send(std::string(R"({"op":"room.setHeight","heightM":)") + (i % 2 ? "3.5" : "3.0") + "}");
    check(s.undoDepth() == EditorSession::kMaxHistory && EditorSession::kMaxHistory == 60, "70 steps: the depth is capped at 60");
    check(s.historyBytes() < EditorSession::kMaxHistoryBytes, "the memory estimate (" + std::to_string(s.historyBytes()) + " bytes for 60 steps) is below the 8 MiB cap");
    int n = 0;
    while (s.canUndo()) { s.undo(); ++n; }
    check(n == 60 && std::fabs(r.p.getDisplayScene().height - 3.0) < 1e-9, "60 undos reach the state after step 10; the 10 oldest steps were dropped");
}

static void testEditorLifetimeAndLoads(Rig& r) {
    std::printf("B8: history survives the editor window; loads clear it\n");
    r.reset("B8");
    auto& s = r.session();
    r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.5,0]})");
    r.send(R"({"op":"room.setAir","tempC":14})");
    r.send(R"({"op":"session.setViewState","viewState":{"v":1,"selection":[{"kind":"wall","index":0}],"camera":{"yaw":-0.75}}})");
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed(r.p.createEditor());
        check(ed != nullptr && s.undoDepth() == 2, "an editor is created; the session history is there");
        r.send(R"({"op":"room.setHeight","heightM":3.25})");
    }
    std::unique_ptr<juce::AudioProcessorEditor> ed2(r.p.createEditor());
    check(s.undoDepth() == 3 && s.canUndo() && s.undoLabel() == "Set room height", "after the editor window was destroyed and recreated the history is intact (3 steps)");
    auto u = s.undo();
    check(u.ok && std::fabs(r.p.getDisplayScene().height - 3.0) < 1e-9, "and undo works after the window was reopened");
    check((int)s.viewState().getProperty("v", 0) == 1 && s.viewState().getProperty("selection", {}).isArray(), "the editor view state (selection, camera) survives too");
    ed2.reset();
    // loads clear the history and the view state and bump the revision
    r.send(R"({"op":"transaction.begin"})", nullptr, "tx-L");
    check(s.transactionOpen(), "a transaction is open before the load");
    uint64_t rev = r.p.getRevision();
    check(r.load(boxText(4, 4, 3)), "another room loads");
    check(!s.canUndo() && !s.canRedo() && s.undoDepth() == 0 && s.viewState().isVoid() && !s.transactionOpen() && r.p.getRevision() > rev,
          "loading a room clears the history, the view state and an open transaction, and bumps the revision");
    r.send(R"({"op":"room.setAir","tempC":14})");
    check(s.canUndo(), "a new history starts");
    juce::MemoryBlock state;
    r.p.getStateInformation(state);
    rev = r.p.getRevision();
    r.p.setStateInformation(state.getData(), (int)state.getSize());
    check(waitReady(r.p, 90) && !s.canUndo() && r.p.getRevision() > rev, "restoring a project state clears the history and bumps the revision");
    r.send(R"({"op":"room.setAir","tempC":16})");
    const juce::String preset = r.p.getRoomPresetText();
    juce::String err;
    rev = r.p.getRevision();
    check(preset.isNotEmpty() && r.p.loadRoomPresetText(preset, err) && waitReady(r.p, 90) && !s.canUndo() && r.p.getRevision() > rev, "importing a .verbroom preset clears the history and bumps the revision");
}

static void testSchemasAndFixtures(Rig& r) {
    std::printf("B9: schema checks of commands, answers, events and the fixtures\n");
    check(!schemas.files.empty(), std::to_string(schemas.files.size()) + " schema files loaded from web-ui/schema (read-only)");
    check(validatedMessages > 100, std::to_string(validatedMessages) + " envelopes (every command sent by this test + every ack / error / event answer) validated against envelope/command/event schemas");
    // negative: the checker really rejects
    std::string why;
    check(!validateEnvelope(J(R"({"schemaVersion":1,"sessionId":"x","kind":"command","requestId":"a","baseRevision":"1","payload":{"op":"room.setAir","tempC":99}})"), &why), "negative: setAir tempC 99 is rejected by the schema checker");
    check(!validateEnvelope(J(R"({"schemaVersion":1,"sessionId":"x","kind":"command","requestId":"a","payload":{"op":"room.setHeight","heightM":3}})"), &why), "negative: a mutation without baseRevision is rejected by the schema checker");
    // fixtures: every command / ack / error / event message
    int fixtureMsgs = 0;
    bool fixturesOk = true;
    for (auto& f : juce::File("web-ui/fixtures").findChildFiles(juce::File::findFiles, false, "*.json")) {
        const juce::var v = juce::JSON::parse(f.loadFileAsString());
        auto* msgs = v.getProperty("messages", {}).getArray();
        if (!msgs) continue;
        for (auto& m : *msgs) {
            const juce::String k = m.getProperty("kind", {}).toString();
            if (k == "snapshot") continue;  // state snapshots are the bridge's job (T-later); the command/answer/event messages are checked here
            const bool ok = validateEnvelope(m, &why);
            ++fixtureMsgs;
            if (!ok) { fixturesOk = false; std::printf("    fixture %s: %s\n", f.getFileName().toRawUTF8(), why.c_str()); }
        }
    }
    check(fixturesOk && fixtureMsgs > 0, std::to_string(fixtureMsgs) + " command / error / event / ack messages of web-ui/fixtures validate");
    (void)r;
}


static void testC1SourceColors(Rig& r) {
    std::printf("C1: source.setConfig color validation, normalization, undo/redo, source.remove\n");
    check(r.load(boxText(4, 3, 3)), "C1 command test room loads");
    auto& s = r.session();
    const uint64_t load0 = r.p.getLoadSerial();

    auto ok1 = r.send(R"({"op":"source.setConfig","index":0,"color":"#AA22CC"})");
    check(ok1.ok && r.p.getSources()[0].color == "#aa22cc" && s.undoLabel() == "Source colour",
          "C1 uppercase hex normalized to lowercase with Source colour undo label");
    check(r.p.getLoadSerial() == load0, "C1 color command does not trigger engine rebuild");

    // Rapid second color edit without advancing clock must NOT coalesce (one undo step per change)
    auto ok2 = r.send(R"({"op":"source.setConfig","index":0,"color":"#3aa76d"})", nullptr, nullptr, nullptr, false);
    check(ok2.ok && r.p.getSources()[0].color == "#3aa76d" && s.undoDepth() == 2,
          "C1 consecutive color picks create separate undo steps");
    check(s.undo().ok && r.p.getSources()[0].color == "#aa22cc", "C1 undo #1 restores previous color");
    check(s.undo().ok && r.p.getSources()[0].color.empty(), "C1 undo #2 restores auto (empty)");
    check(s.redo().ok && r.p.getSources()[0].color == "#aa22cc", "C1 redo restores color");

    auto clrNull = r.send(R"({"op":"source.setConfig","index":0,"color":null})");
    check(clrNull.ok && r.p.getSources()[0].color.empty(), "C1 color:null resets to auto");
    auto ok3 = r.send(R"({"op":"source.setConfig","index":0,"color":"#1188ff"})");
    auto clrEmpty = r.send(R"({"op":"source.setConfig","index":0,"color":""})");
    check(ok3.ok && clrEmpty.ok && r.p.getSources()[0].color.empty(), "C1 color:\"\" resets to auto");

    r.validateCommands = false;
    const uint64_t revBeforeBad = r.p.getRevision();
    for (const char* badJson : {
             R"({"op":"source.setConfig","index":0,"color":"red"})",
             R"({"op":"source.setConfig","index":0,"color":"#123"})",
             R"({"op":"source.setConfig","index":0,"color":"#12345g"})",
             R"({"op":"source.setConfig","index":0,"color":123})"
         }) {
        auto bad = r.send(badJson);
        check(!bad.ok && bad.code == "invalid", std::string("C1 invalid color rejected: ") + badJson);
    }
    check(r.p.getRevision() == revBeforeBad, "C1 invalid color does not bump revision");
    r.validateCommands = true;

    check(r.send(R"({"op":"source.setConfig","index":0,"color":"#e04040"})").ok, "C1 color source 0");
    check(r.send(R"({"op":"source.setConfig","index":1,"color":"#3a75c4"})").ok, "C1 color source 1");
    check(r.send(R"({"op":"source.remove","index":0})").ok, "C1 remove source 0");
    check(r.p.getSources().size() == 1 && r.p.getSources()[0].color == "#3a75c4", "C1 source.remove keeps color on remaining source");
    check(s.undo().ok && r.p.getSources().size() == 2 && r.p.getSources()[0].color == "#e04040" && r.p.getSources()[1].color == "#3a75c4",
          "C1 undo of source.remove restores both sources and their colours");
    r.reset("C1");
}

static std::string translateWall(size_t index, double dx, double dy, const char* options = "") {
    return R"({"op":"surface.translate","refs":[{"kind":"wall","index":)" + std::to_string(index)
        + R"(}],"deltaM":[)" + std::to_string(dx) + "," + std::to_string(dy) + "]" + options + "}";
}

static int openCorners(const verb::Scene& scene) {
    int open = 0;
    for (size_t i = 0; i < scene.walls.size(); ++i) {
        const auto& wall = scene.walls[i];
        const auto& next = scene.walls[(i + 1) % scene.walls.size()];
        if (std::hypot(wall.bx - next.ax, wall.by - next.ay) > 1e-4) ++open;
    }
    return open;
}

static void testWallTranslation(Rig& r) {
    const auto box = boxText(5, 4, 3);
    constexpr auto link = R"(,"autoLink":true)";
    if (!r.load(box)) { check(false, "F1 closed test room loads"); return; }
    const auto start = r.p.getDisplayScene();
    const auto wallsText = [&] { return verb::serializeScene(r.p.getDisplayScene()); };
    const auto initialText = wallsText();
    check(openCorners(start) == 0, "F1 four-wall fixture starts closed");
    for (auto option : {"", R"(,"autoLink":false)"}) {
        const auto result = r.send(translateWall(0, 0, 0.5, option));
        const auto moved = r.p.getDisplayScene();
        check(result.ok && moved.walls[0].ay == start.walls[0].ay + 0.5 && openCorners(moved) == 2,
              *option ? "F1 explicit false leaves two corners open" : "F1 omitted autoLink preserves the old move");
        check(r.session().undo().ok && wallsText() == initialText, "F1 standalone move can be undone");
    }
    const uint64_t revision = r.p.getRevision();
    auto rejected = r.send(translateWall(0, 0, 4, link));
    check(!rejected.ok && rejected.code == "invalid" && r.p.getRevision() == revision && wallsText() == initialText,
          "F1 zero-length neighbour rejects the entire move");

    check(r.send(R"({"op":"transaction.begin","label":"Move linked corners"})", nullptr, "f1-wall").ok, "F1 start a drag");
    check(r.send(translateWall(0, 0, 0.5, link), nullptr, "f1-wall").ok &&
          openCorners(r.p.getDisplayScene()) == 0, "F1 first live delta moves both linked corners");
    check(r.send(translateWall(0, 0, 0.75, R"(,"autoLink":false)"), nullptr, "f1-wall").ok &&
          openCorners(r.p.getDisplayScene()) == 2, "F1 replacing the live command restores the neighbours when linking is disabled");
    check(r.send(R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0,1,0.25],"autoLink":true})", nullptr, "f1-wall").ok,
          "F1 final live delta includes Z on the selected wall");
    const auto moved = r.p.getDisplayScene();
    check(openCorners(moved) == 0 && moved.walls[0].ay == 1 && moved.walls[1].ay == 1 && moved.walls[3].by == 1 &&
          moved.walls[0].z0 == 0.25 && moved.walls[1].z0 == start.walls[1].z0 && moved.walls[3].z1 == start.walls[3].z1,
          "F1 live motion uses the start endpoints and preserves neighbours' heights");
    const auto movedText = wallsText();
    check(r.send(R"({"op":"transaction.commit"})", nullptr, "f1-wall").ok && r.session().undoDepth() == 1 &&
          r.session().undo().ok && !r.session().canUndo() && wallsText() == initialText,
          "F1 one undo restores the entire drag, including both neighbours");
    check(r.session().redo().ok && wallsText() == movedText, "F1 redo restores the linked geometry");
    r.session().undo();

    check(r.send(R"({"op":"surface.setMetadata","refs":[{"kind":"wall","index":1}],"locked":true})").ok &&
          r.send(R"({"op":"surface.setMetadata","refs":[{"kind":"wall","index":3}],"hidden":true})").ok &&
          r.send(translateWall(0, 0, 1, link)).ok, "F1 moving a wall beside locked and hidden neighbours succeeds");
    const auto skipped = r.p.getDisplayScene();
    check(skipped.walls[0].ay == 1 && skipped.walls[1].ay == 0, "F1 locked neighbour stays put");
    check(skipped.walls[3].by == 0, "F1 hidden neighbour stays put");
    r.reset("F1 skip");

    check(r.send(R"({"op":"surface.link","refs":[{"kind":"wall","index":0},{"kind":"wall","index":1}],"groupName":"Corner pair"})").ok &&
          r.send(translateWall(0, 0.25, 0.5, link)).ok, "F1 includeGroups expands the selection");
    const auto grouped = r.p.getDisplayScene();
    check(openCorners(grouped) == 0 && grouped.walls[0].bx == 5.25 && grouped.walls[1].ax == 5.25 &&
          grouped.walls[1].by == 4.5 && grouped.walls[2].ay == 4.5,
          "F1 internal group joint and both exterior joints stay closed");
    r.reset("F1 group");

    const auto* descriptor = verbp::findDescriptor("surface.translate");
    std::mt19937 rnd(117);
    std::uniform_int_distribution<int> step(-5, 5);
    for (const auto& text : {box, verbp::vslGrandHall(false).text}) {
        Model before = r.model();
        before.scene = parseText(text);
        const auto n = before.scene.walls.size();
        const int initialOpen = openCorners(before.scene);
        int maxOpen = initialOpen;
        std::uniform_int_distribution<size_t> wall(0, n - 1);
        int validMoves = 0;
        for (int i = 0; i < 24; ++i) {
            Model candidate = before;
            auto f = descriptor->mutate(J(translateWall(wall(rnd), step(rnd) * 0.01, step(rnd) * 0.01, link)),
                                        candidate, {r.p, before});
            if (f.bad() || verbp::normalizeAndValidate(candidate, before).bad()) break;
            maxOpen = std::max(maxOpen, openCorners(candidate.scene));
            before = std::move(candidate);
            ++validMoves;
        }
        std::printf("    F1 closed contour: %zu walls, %d moves, open corners %d initially / %d maximum\n",
                    n, validMoves, initialOpen, maxOpen);
        check(n >= 4 && maxOpen == 0 && validMoves == 24, "F1 randomized wall translations preserve every consecutive joint");
    }
}

struct ImpulseResult { bool finite = true; double energy = 0, peak = 0; };
static ImpulseResult impulse(VerbProcessor& proc, int input = 0, std::vector<float>* captures = nullptr) {
    juce::AudioBuffer<float> audio(std::max(2, proc.getTotalNumInputChannels()), 256);
    juce::MidiBuffer midi;
    ImpulseResult result;
    for (int block = 0; block < 375; ++block) {
        audio.clear();
        if (!block) audio.setSample(input, 0, 1.0f);
        proc.processBlock(audio, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < audio.getNumSamples(); ++i) {
                const double sample = audio.getSample(ch, i);
                result.finite &= std::isfinite(sample);
                result.energy += sample * sample;
                result.peak = std::max(result.peak, std::abs(sample));
                if (captures) captures->push_back((float)sample);
            }
    }
    return result;
}

static bool waitForImpulseRoom(VerbProcessor& proc) {
    juce::AudioBuffer<float> silence(std::max(2, proc.getTotalNumInputChannels()), 256);
    juce::MidiBuffer midi;
    int settled = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        silence.clear();
        proc.processBlock(silence, midi);
        const auto engine = E9bAccess::engine(proc);
        if (proc.isWebSceneCurrent() && engine && engine->tailIsCurrent() && engine->motionStats().transState == 0) {
            if (++settled == 110) return true;
        } else settled = 0;
    }
    return false;
}

static void testReleasePresetImpulse(const char* name, const juce::String& presetPath, int inputs) {
    VerbProcessor original, restored;
    for (auto* proc : {&original, &restored}) {
        proc->setPlayConfigDetails(inputs, 2, 48000, 256);
        proc->prepareToPlay(48000, 256);
        proc->setKeepRoutingEnabled(false);
    }

    const std::string label = name;
    juce::String error;
    if ((presetPath.isNotEmpty() && !original.loadRoomPresetText(juce::File(presetPath).loadFileAsString(), error)) ||
        !waitForImpulseRoom(original)) {
        check(false, label + " initial room settles: " + error.toStdString());
        return;
    }

    const auto saved = juce::File(juce::String(name) + "-roundtrip.verbroom");
    const auto preset = original.getRoomPresetText();
    if (preset.isEmpty() || !saved.replaceWithText(preset) ||
        !restored.loadRoomPresetText(saved.loadFileAsString(), error) || !waitForImpulseRoom(restored)) {
        check(false, label + " saved room reloads and settles: " + error.toStdString());
        return;
    }
    check(original.getRoomText() == restored.getRoomText() &&
          original.numPoints() == restored.numPoints(), label + " geometry, materials and point count survive reload");

    const auto sources = original.getSources();
    const int lastInput = sources.back().firstIn + sources.back().channels - 1;
    const auto testInputs = lastInput > 0 ? std::vector<int>{0, lastInput} : std::vector<int>{0};
    for (const int input : testInputs) {
        std::vector<float> before, after;
        const auto a = impulse(original, input, &before);
        const auto b = impulse(restored, input, &after);
        double maxDelta = 0;
        for (size_t i = 0; i < before.size(); ++i)
            maxDelta = std::max(maxDelta, std::abs(double(before[i]) - after[i]));
        const double deltaDb = 10.0 * std::log10(b.energy / a.energy);
        std::printf("    %s input %d: energy %.12g -> %.12g, peak %.9g -> %.9g, delta %.9g dB, max sample delta %.9g\n",
                    name, input + 1, a.energy, b.energy, a.peak, b.peak, deltaDb, maxDelta);
        check(a.finite && b.finite && a.energy > 1e-8 && b.energy > 1e-8 &&
              std::abs(deltaDb) <= 0.0001 && maxDelta <= 1e-6,
              label + " finite, non-silent render survives reload on input " + std::to_string(input + 1));
    }
}

static void testWallPresetImpulse(Rig& r) {
    if (!r.load(boxText(5, 4, 3)) || !r.send(translateWall(0, 0, 0.25, R"(,"autoLink":true)")).ok ||
        !waitForImpulseRoom(r.p)) {
        check(false, "F1 the edited closed room builds in the real engine");
        return;
    }
    const auto moved = r.p.getDisplayScene();
    const auto preset = r.p.getRoomPresetText();
    const auto original = impulse(r.p);
    check(openCorners(moved) == 0 && original.finite && original.energy > 1e-6, "F1 edited closed room renders a finite, non-silent impulse");

    Rig restored;
    restored.p.setKeepRoutingEnabled(false);
    juce::String error;
    if (preset.isEmpty() || !restored.p.loadRoomPresetText(preset, error) || !waitForImpulseRoom(restored.p)) {
        check(false, "F1 edited .verbroom reloads and builds: " + error.toStdString());
        return;
    }
    const auto reloaded = restored.p.getDisplayScene();
    bool sameWalls = reloaded.walls.size() == moved.walls.size();
    for (size_t i = 0; sameWalls && i < moved.walls.size(); ++i) {
        const auto& a = moved.walls[i];
        const auto& b = reloaded.walls[i];
        const auto& matA = moved.mats[(size_t)a.mat];
        const auto& matB = reloaded.mats[(size_t)b.mat];
        sameWalls = a.ax == b.ax && a.ay == b.ay && a.bx == b.bx && a.by == b.by && a.z0 == b.z0 && a.z1 == b.z1 &&
                    matA.name == matB.name && matA.alpha == matB.alpha && matA.tau == matB.tau &&
                    matA.scatter == matB.scatter && matA.scatterHf == matB.scatterHf && matA.cls == matB.cls;
    }
    check(sameWalls && openCorners(reloaded) == 0,
          "F1 .verbroom round trip preserves wall endpoints, heights and materials");
    const auto replay = impulse(restored.p);
    const double differenceDb = 10.0 * std::log10(replay.energy / original.energy);
    std::printf("    F1 impulse (2 s, 48 kHz): energy %.9g -> %.9g, peak %.9g -> %.9g, delta %.9g dB\n",
                original.energy, replay.energy, original.peak, replay.peak, differenceDb);
    check(replay.finite && replay.energy > 1e-6 && std::abs(differenceDb) <= 0.0001,
          "F1 reloaded render is finite, non-silent and within 0.0001 dB of the original");
}

static std::pair<int, int> joinCounts(const juce::var& details) {
    return {(int)details.getProperty("joined", -1), (int)details.getProperty("skipped", -1)};
}

static void testG1AzimuthKeys(Rig& r) {
    std::printf("G1: azimuth key commands, schema, coalescing, undo/redo, source.remove, path.clear\n");
    r.reset("G1");
    auto& s = r.session();
    const int d0 = s.undoDepth();

    // 1. Rejections before path exists (< 2 points)
    {
        r.validateCommands = false;
        auto noPath = r.send(R"({"op":"path.setAzimuthKey","object":0,"t":25,"az":45})");
        auto noPathBulk = r.send(R"({"op":"path.setAzimuthKeys","object":0,"keys":[{"t":10,"az":20}]})");
        r.validateCommands = true;
        check(!noPath.ok && noPath.code == "invalid" && !noPathBulk.ok && noPathBulk.code == "invalid" && s.undoDepth() == d0,
              "G1 commands reject azKeys when object has < 2 path points");
    }

    // 2. Set moving path + add/update/remove azimuth keys (schema-validated!)
    check(r.send(R"({"op":"path.set","object":0,"points":[[1,1,1.5],[4,1,1.5]]})").ok, "G1 path.set on source 0");
    check(r.send(R"({"op":"path.setAzimuthKey","object":0,"t":60,"az":-90,"easing":"easeInOut"})").ok, "G1 add first azKey");
    check(r.send(R"({"op":"path.setAzimuthKey","object":0,"t":20,"az":45,"easing":"hold"})").ok, "G1 add earlier azKey sorts before t=60");
    auto mp = r.p.getMovementPath(0);
    check(mp.azKeys.size() == 2 && mp.azKeys[0].t == 20 && mp.azKeys[0].easing == verbp::AzimuthEasing::Hold
          && mp.azKeys[1].t == 60 && mp.azKeys[1].azDeg == -90,
          "G1 keys stay sorted by t");

    // Update by matching t preserves easing when omitted
    check(r.send(R"({"op":"path.setAzimuthKey","object":0,"t":20,"az":135})").ok, "G1 update by matching t");
    mp = r.p.getMovementPath(0);
    check(mp.azKeys.size() == 2 && mp.azKeys[0].azDeg == 135 && mp.azKeys[0].easing == verbp::AzimuthEasing::Hold,
          "G1 matching-t update preserves easing when easing is omitted");

    // Coalescing on index drag (same object/az/index within 750ms)
    const int depthBeforeDrag = s.undoDepth();
    check(r.send(R"({"op":"path.setAzimuthKey","object":0,"index":0,"az":100})").ok, "G1 coalesce step 1");
    r.clockMs += 100;
    check(r.send(R"({"op":"path.setAzimuthKey","object":0,"index":0,"az":110})", nullptr, nullptr, nullptr, false).ok, "G1 coalesce step 2");
    r.clockMs += 100;
    check(r.send(R"({"op":"path.setAzimuthKey","object":0,"index":0,"t":25,"az":120})", nullptr, nullptr, nullptr, false).ok, "G1 coalesce step 3");
    check(s.undoDepth() == depthBeforeDrag + 1, "G1 repeated index edits within 750ms coalesce into ONE undo step");
    check(s.undo().ok && r.p.getMovementPath(0).azKeys[0].t == 20 && r.p.getMovementPath(0).azKeys[0].azDeg == 135,
          "G1 undo of coalesced drag reverts to state before step 1");
    check(s.redo().ok && r.p.getMovementPath(0).azKeys[0].t == 25 && r.p.getMovementPath(0).azKeys[0].azDeg == 120,
          "G1 redo restores final coalesced drag value");

    // Editing different index starts a new step
    check(r.send(R"({"op":"path.setAzimuthKey","object":0,"index":1,"easing":"linear","az":-45})").ok, "G1 update index 1");
    check(s.undoDepth() == depthBeforeDrag + 2, "G1 editing another index does not coalesce with index 0");

    // Invalid rejections
    {
        const auto rev = r.p.getRevision();
        const int ud = s.undoDepth();
        r.validateCommands = false;
        for (const char* bad : {
                 R"({"op":"path.setAzimuthKey","object":0,"t":-1,"az":0})",
                 R"({"op":"path.setAzimuthKey","object":0,"t":101,"az":0})",
                 R"({"op":"path.setAzimuthKey","object":0,"t":50,"az":-181})",
                 R"({"op":"path.setAzimuthKey","object":0,"t":50,"az":181})",
                 R"({"op":"path.setAzimuthKey","object":0,"t":50,"az":0,"easing":"bounce"})",
                 R"({"op":"path.setAzimuthKey","object":0,"index":9,"az":0})",
                 R"({"op":"path.setAzimuthKey","object":0,"index":0})",
                 R"({"op":"path.removeAzimuthKey","object":0,"index":9})"
             }) {
            auto res = r.send(bad);
            check(!res.ok && res.code == "invalid", std::string("G1 invalid command rejected: ") + bad);
        }
        r.validateCommands = true;
        check(r.p.getRevision() == rev && s.undoDepth() == ud, "G1 rejected commands leave revision & history untouched");
    }

    // Remove key + bulk set + 128-key cap check
    check(r.send(R"({"op":"path.removeAzimuthKey","object":0,"index":0})").ok && r.p.getMovementPath(0).azKeys.size() == 1,
          "G1 path.removeAzimuthKey removes key cleanly");
    {
        std::string keys128 = R"({"op":"path.setAzimuthKeys","object":0,"keys":[)";
        for (int i = 0; i < 128; ++i) {
            if (i) keys128 += ",";
            keys128 += R"({"t":)" + std::to_string(100.0 * i / 127.0) + R"(,"az":)" + std::to_string((i % 2) ? 10 : -10) + "}";
        }
        keys128 += "]}";
        check(r.send(keys128).ok && r.p.getMovementPath(0).azKeys.size() == 128, "G1 bulk path.setAzimuthKeys accepts 128 keys");
        r.validateCommands = false;
        auto over = r.send(R"({"op":"path.setAzimuthKey","object":0,"t":50.1234,"az":15})");
        r.validateCommands = true;
        check(!over.ok && over.code == "invalid" && r.p.getMovementPath(0).azKeys.size() == 128, "G1 129th distinct key rejected");
    }

    // path.set preserving azKeys when omitted, path.clear clearing azKeys + undo restoring both
    check(r.send(R"({"op":"path.setAzimuthKeys","object":0,"keys":[{"t":10,"az":30,"easing":"hold"},{"t":90,"az":-30}]})").ok,
          "G1 set 2 keys before path.set/clear");
    check(r.send(R"({"op":"path.set","object":0,"points":[[1,1,1.5],[4,2,1.5],[4,3,1.5]]})").ok && r.p.getMovementPath(0).azKeys.size() == 2,
          "G1 path.set preserves azKeys when azKeys field is omitted");
    check(r.send(R"({"op":"path.setFlags","object":0,"orientMode":"lookAt","lookTarget":32,"orientOffset":15})").ok, "G1 set orientation flags");
    check(r.send(R"({"op":"path.clear","object":0})").ok, "G1 path.clear");
    check(r.p.getMovementPath(0).points.empty() && r.p.getMovementPath(0).azKeys.empty()
          && r.p.getMovementPath(0).lookAt && r.p.getMovementPath(0).orientOffset == 15,
          "G1 path.clear clears azKeys along with points while keeping orientation settings");
    check(s.undo().ok && r.p.getMovementPath(0).azKeys.size() == 2 && r.p.getMovementPath(0).points.size() == 3,
          "G1 undo of path.clear restores both points and azKeys");

    // Scoped undo: unrelated air edit does not revert azKeys
    const auto beforeAir = r.p.getMovementPath(0).azKeys;
    check(r.send(R"({"op":"path.setAzimuthKey","object":0,"index":0,"az":-75})").ok, "G1 scoped edit key");
    check(r.send(R"({"op":"room.setAir","tempC":15})").ok, "G1 unrelated air edit");
    check(s.undo().ok && r.p.getMovementPath(0).azKeys[0].azDeg == -75 && r.p.getDisplayScene().tempC != 15,
          "G1 air undo leaves azKeys alone");
    check(s.undo().ok && r.p.getMovementPath(0).azKeys == beforeAir, "G1 next undo reverts only azKeys");

    // source.remove reindexing shifts azKeys along with paths and undo restores both
    check(r.send(R"({"op":"path.set","object":1,"points":[[2,2,1.5],[2,3,1.5]],"azKeys":[{"t":25,"az":111,"easing":"easeIn"}]})").ok,
          "G1 path.set with inline azKeys on source 1");
    check(r.send(R"({"op":"source.remove","index":0})").ok, "G1 remove source 0");
    check(r.p.getMovementPath(0).azKeys.size() == 1 && r.p.getMovementPath(0).azKeys[0].azDeg == 111
          && r.p.getMovementPath(0).azKeys[0].easing == verbp::AzimuthEasing::EaseIn,
          "G1 source.remove shifts remaining source path + azKeys to index 0");
    check(s.undo().ok && r.p.getSources().size() == 2 && r.p.getMovementPath(0).azKeys.size() == 2
          && r.p.getMovementPath(1).azKeys.size() == 1 && r.p.getMovementPath(1).azKeys[0].azDeg == 111,
          "G1 undo of source.remove restores both sources and their azKeys");

    while (s.canUndo()) s.undo();
}

static void testCornerRepairClusters(Rig& r) {
    const auto* command = verbp::findDescriptor("scene.joinCorners");
    const auto model = [&](std::initializer_list<double> xs) {
        Model m = r.model();
        m.scene.walls.clear();
        for (double x : xs) {
            const auto i = m.scene.walls.size();
            m.scene.walls.push_back(verb::Wall{x, 0, 1.0 + i, 2, m.scene.floorMat});
        }
        return m;
    };
    const auto join = [&](Model& m, const char* args = "{}") {
        const Model before = m;
        juce::var details;
        const auto failure = command->mutate(J(args), m, {r.p, before, &details});
        return failure.bad() ? std::pair<int, int>{-1, -1} : joinCounts(details);
    };
    for (auto xs : {std::initializer_list<double>{0, .03}, {0, .04, -.04}, {0, .04, -.04, -.02}}) {
        auto m = model(xs);
        check(join(m) == std::make_pair(1, 0) &&
              std::all_of(m.scene.walls.begin(), m.scene.walls.end(), [](const auto& w) { return w.ax == 0; }),
              "F7 cluster with " + std::to_string(xs.size()) + " ends uses the first wall as anchor");
    }
    auto m = model({0, .04, .08});
    m.scene.walls[1].editor.locked = true;
    m.scene.walls[2].editor.hidden = true;
    check(join(m) == std::make_pair(1, 0) && m.scene.walls[0].ax == .04 && m.scene.walls[1].ax == .04 &&
          m.scene.walls[2].ax == .04 && m.scene.walls[2].editor.hidden, "F7 locked anchor retains a transitive cluster and includes a hidden wall");
    m = model({0, .03, .04});
    m.scene.walls[0].editor.locked = m.scene.walls[1].editor.locked = true;
    check(join(m) == std::make_pair(0, 1) && m.scene.walls[2].ax == .04, "F7 distinct locked ends skip the cluster");
    m = model({0, .04, .08});
    check(join(m) == std::make_pair(0, 1) && m.scene.walls[1].ax == .04, "F7 transitive radius beyond tolerance is skipped");
    m = model({0, .02});
    m.scene.walls[0].bx = .04;
    m.scene.walls[0].by = 0;
    check(join(m) == std::make_pair(0, 1) && m.scene.walls[0].bx == .04 && m.scene.walls[1].ax == .02,
          "F7 collapsing a wall skips the entire cluster");
    m = model({0, .03, .03});
    check(join(m) == std::make_pair(1, 0) && m.scene.walls[1].ax == 0 && m.scene.walls[2].ax == 0,
          "F7 an already sewn pair moves together when added to a repair cluster");
    m = model({0, .03, .04});
    check(join(m, R"({"refs":[{"kind":"wall","index":1},{"kind":"wall","index":2}]})") == std::make_pair(1, 0) &&
          m.scene.walls[0].ax == 0 && m.scene.walls[1].ax == .03 && m.scene.walls[2].ax == .03,
          "F7 refs restrict both the anchor and moved ends");
    for (int mode : {1, 2}) {
        m = model({0, .03});
        m.scene.layerModes[0] = mode;
        check(join(m) == std::make_pair(0, 0) && m.scene.walls[1].ax == .03, "F7 noneditable wall layer is excluded");
    }
    r.validateCommands = false;
    for (auto args : {R"({"op":"scene.joinCorners","toleranceM":0})", R"({"op":"scene.joinCorners","toleranceM":0.5})"}) {
        const auto result = r.send(args);
        check(!result.ok && result.code == "invalid" && result.field == "toleranceM", "F7 tolerance bounds reject " + std::string(args));
    }
    r.validateCommands = true;
}

static bool sameCornerBits(const verb::Scene& a, const verb::Scene& b) {
    if (a.walls.size() != b.walls.size()) return false;
    for (size_t i = 0; i < a.walls.size(); ++i) {
        const auto& u = a.walls[i];
        const auto& v = b.walls[i];
        const double coordsA[]{u.ax, u.ay, u.bx, u.by, u.z0, u.z1};
        const double coordsB[]{v.ax, v.ay, v.bx, v.by, v.z0, v.z1};
        if (std::memcmp(coordsA, coordsB, sizeof(coordsA)) != 0) return false;
    }
    return true;
}

static void testCornerSnapPrecision(Rig& r) {
    const auto* translate = verbp::findDescriptor("surface.translate");
    const auto* moveVertex = verbp::findDescriptor("surface.moveVertex");
    Model before = r.model();
    before.scene = parseText(boxText(5, 4, 3));
    const double x = 0.12345678901234567;
    before.scene.walls.push_back(verb::Wall{x, 1, 2, 3, before.scene.floorMat});
    Model after = before;
    const auto target = R"("snapTarget":{"kind":"wall","index":4,"vertex":0})";
    auto f = translate->mutate(J(translateWall(0, x - 5, 1, (std::string(R"(,"autoLink":true,"snapFrom":{"kind":"wall","index":0,"vertex":1},)") + target).c_str())),
                               after, {r.p, before});
    // The existing test helper prints a six-decimal delta. The real command must send the exact delta.
    check(f.bad(), "F7 rounded delta for a target reference is rejected");
    std::ostringstream args;
    args.precision(17);
    args << R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"autoLink":true,"deltaM":[)"
         << x - 5 << R"(,1],"snapFrom":{"kind":"wall","index":0,"vertex":1},)" << target << '}';
    after = before;
    f = translate->mutate(J(args.str()), after, {r.p, before});
    check(!f.bad() && after.scene.walls[0].bx == x && after.scene.walls[1].ax == x &&
          after.scene.walls[0].by == 1 && after.scene.walls[1].ay == 1, "F7 translation and linked end copy exact target bits");
    after = before;
    f = moveVertex->mutate(J(std::string(R"({"op":"surface.moveVertex","ref":{"kind":"wall","index":0},"vertex":1,"positionM":[0.12345678901234567,1],"autoLink":false,)") + target + "}"),
                           after, {r.p, before});
    check(!f.bad() && after.scene.walls[0].bx == x && after.scene.walls[0].by == 1 &&
          after.scene.walls[1].ax == 5, "F7 vertex snaps exactly with auto-link disabled");
}

static void testCornerRepairImpulse(Rig& r) {
    auto gap = parseText(boxText(5, 4, 3));
    const double x = 5.1234567890123457;
    gap.walls[0].bx = gap.walls[1].ax = gap.walls[1].bx = gap.walls[2].ax = x;
    gap.walls[1].ay = .03;
    const auto text = verb::serializeScene(gap, 6, 17);
    Rig unsealed;
    if (!r.load(text, "F7 3 cm corner gap") || !unsealed.load(text, "F7 3 cm corner gap") || !waitForImpulseRoom(unsealed.p)) {
        check(false, "F7 gapped test room settles");
        return;
    }
    check(openCorners(r.p.getDisplayScene()) == 1 && sameCornerBits(r.p.getDisplayScene(), gap), "F7 precisely one 3 cm gap before repair");
    const auto result = r.send(R"({"op":"scene.joinCorners"})");
    const auto repaired = r.p.getDisplayScene();
    check(result.ok && joinCounts(result.messages.front().getProperty("payload", {}).getProperty("details", {})) == std::make_pair(1, 0) &&
          openCorners(repaired) == 0 && repaired.walls[1].ax == x && repaired.walls[1].ay == 0,
          "F7 one repaired cluster seals the room and reports one join");
    if (!waitForImpulseRoom(r.p)) { check(false, "F7 sealed room settles"); return; }

    Rig restored;
    restored.p.setKeepRoutingEnabled(false);
    const auto file = juce::File("F7-joined-room.verbroom");
    juce::String why;
    if (!file.replaceWithText(r.p.getRoomPresetText()) || !restored.p.loadRoomPresetText(file.loadFileAsString(), why) ||
        !waitForImpulseRoom(restored.p)) { check(false, "F7 preset reload: " + why.toStdString()); return; }
    check(sameCornerBits(repaired, restored.p.getDisplayScene()), "F7 preset keeps every wall coordinate bit-identical");
    check(sameCornerBits(repaired, parseText(stateSceneText(r.p))), "F7 host state preserves the exact coordinates of edited walls");

    std::vector<float> gappedAudio, sealedAudio, restoredAudio;
    const auto gapped = impulse(unsealed.p, 0, &gappedAudio);
    const auto sealed = impulse(r.p, 0, &sealedAudio);
    const auto replay = impulse(restored.p, 0, &restoredAudio);
    const auto late = [](const std::vector<float>& audio) {
        double energy = 0;
        for (size_t block = 0; block < audio.size() / 512; ++block)
            for (size_t i = 0; i < 512; ++i)
                if (block * 256 + i % 256 >= 4800) {
                    const double v = audio[block * 512 + i];
                    energy += v * v;
                }
        return energy;
    };
    const double lateGap = late(gappedAudio), lateSealed = late(sealedAudio), lateReplay = late(restoredAudio);
    const double reloadDb = 10 * std::log10(replay.energy / sealed.energy);
    double maxDelta = 0;
    for (size_t i = 0; i < sealedAudio.size(); ++i)
        maxDelta = std::max(maxDelta, std::abs(double(sealedAudio[i]) - restoredAudio[i]));
    std::printf("    F7 48 kHz stereo, 2 s; total energies gap/sealed/reloaded %.12g / %.12g / %.12g\n", gapped.energy, sealed.energy, replay.energy);
    std::printf("    F7 late [0.1, 2] s gap/sealed/reloaded %.12g / %.12g / %.12g; sealed vs gap %+.9g dB\n",
                lateGap, lateSealed, lateReplay, 10 * std::log10(lateSealed / lateGap));
    std::printf("    F7 reload: total delta %+.9g dB, peak %.9g -> %.9g, max sample delta %.9g\n", reloadDb, sealed.peak, replay.peak, maxDelta);
    check(gapped.finite && sealed.finite && replay.finite && lateGap > 0 && lateSealed > 0 && lateReplay > 0,
          "F7 all three real-engine impulses are finite with nonzero late energy");
    check(std::abs(reloadDb) <= 0.0001 && maxDelta <= 1e-6, "F7 reloaded impulse level and samples match the repaired room");
    check(r.session().undoDepth() == 1 && r.session().undo().ok && !r.session().canUndo() &&
          sameCornerBits(r.p.getDisplayScene(), gap), "F7 one undo restores the complete gapped room exactly");

    juce::MemoryBlock currentState, legacyState;
    r.p.getStateInformation(currentState);
    auto legacy = juce::AudioProcessor::getXmlFromBinary(currentState.getData(), (int)currentState.getSize());
    const auto legacyText = boxText(5.125, 4, 3);
    if (legacy) {
        legacy->setAttribute("sceneText", juce::String(legacyText));
        juce::AudioProcessor::copyXmlToBinary(*legacy, legacyState);
        restored.p.setStateInformation(legacyState.getData(), (int)legacyState.getSize());
    }
    check(legacy && waitReady(restored.p, 90) && sameCornerBits(restored.p.getDisplayScene(), parseText(legacyText)),
          "F7 pre-change VerbState with six-digit geometry loads unchanged");
}

static void testSourceFormatImpulse() {
    Rig r(64);
    const auto room = verbp::vslGrandHall(false);
    r.p.loadRoomWithSetup(juce::String(room.text), juce::String(room.name), room.setup, room.micPattern);
    if (!waitReady(r.p, 90)) {
        check(false, "F5 VSL Grand Hall loads");
        return;
    }
    const auto before = r.p.getSources();
    check(before.size() == 32 && before.back().firstIn == 62, "F5 VSL starts with 32 stereo pairs");
    const auto commands = juce::JSON::parse(juce::File("f5-format-commands.json").loadFileAsString());
    if (!commands.isArray() || commands.size() != 32) {
        check(false, "F5 JavaScript format plan must supply 32 commands");
        return;
    }
    bool applied = r.send(R"({"op":"transaction.begin","label":"All formats"})", nullptr, "f5-format").ok;
    for (const auto& command : *commands.getArray())
        applied &= r.send(juce::JSON::toString(command, true).toStdString(), nullptr, "f5-format").ok;
    applied &= r.send(R"({"op":"transaction.commit"})", nullptr, "f5-format").ok;
    const auto isPackedMono = [](const std::vector<verbp::SourceCfg>& sources) {
        if (sources.size() != 32) return false;
        for (size_t i = 0; i < sources.size(); ++i)
            if (sources[i].format != 0 || sources[i].effectiveChannels() != 1 || sources[i].firstIn != (int)i) return false;
        return true;
    };
    check(applied && isPackedMono(r.p.getSources()), "F5 one transaction packs 32 mono sources on inputs 1–32");
    check(r.session().undoDepth() == 1 && r.session().undo().ok && r.p.getSources().back().firstIn == 62 &&
          r.p.getSources().back().format == 1 && !r.session().canUndo(), "F5 one undo restores stereo routing");
    check(r.session().redo().ok && isPackedMono(r.p.getSources()), "F5 redo restores mono routing");
    check(r.send(R"({"op":"transaction.begin","label":"Overflow probe"})", nullptr, "f5-overflow").ok &&
          r.send(R"({"op":"source.setConfig","index":0,"name":"Temporary"})", nullptr, "f5-overflow").ok &&
          r.send(R"({"op":"source.setConfig","index":1,"format":1,"firstIn":63})", nullptr, "f5-overflow").code == "budgetExceeded" &&
          r.send(R"({"op":"transaction.cancel"})", nullptr, "f5-overflow").ok &&
          r.p.getSources()[0].name == before[0].name && isPackedMono(r.p.getSources()) &&
          r.session().undoDepth() == 1, "F5 budgetExceeded followed by cancel restores the entire edit");

    if (!waitForImpulseRoom(r.p)) {
        check(false, "F5 edited hall settles");
        return;
    }
    const auto preset = r.p.getRoomPresetText();
    const auto file = juce::File("F5_VSL_Grand_Hall_Mono.verbroom");
    check(file.replaceWithText(preset), "F5 exports the real processor preset");
    Rig restored(64);
    restored.p.setKeepRoutingEnabled(false);
    juce::String error;
    if (!restored.p.loadRoomPresetText(file.loadFileAsString(), error) || !waitForImpulseRoom(restored.p)) {
        check(false, "F5 edited hall reloads and settles: " + error.toStdString());
        return;
    }
    check(isPackedMono(restored.p.getSources()) && restored.p.numPoints() == 32, "F5 reload has 32 points and no overlapping routes");
    for (int input : {0, 31}) {
        std::vector<float> originalSamples, replaySamples;
        const auto original = impulse(r.p, input, &originalSamples), replay = impulse(restored.p, input, &replaySamples);
        const double deltaDb = 10.0 * std::log10(replay.energy / original.energy);
        double maxDelta = 0;
        for (size_t i = 0; i < originalSamples.size(); ++i)
            maxDelta = std::max(maxDelta, std::abs((double)originalSamples[i] - replaySamples[i]));
        std::printf("    F5 input %d: energy %.9g -> %.9g, peak %.9g -> %.9g, delta %.9g dB, max sample delta %.9g\n",
                    input + 1, original.energy, replay.energy, original.peak, replay.peak, deltaDb, maxDelta);
        check(original.finite && replay.finite && original.energy > 1e-8 && std::abs(deltaDb) <= 0.001 && maxDelta <= 1e-6,
              "F5 finite non-silent impulse is consistent after reload on input " + std::to_string(input + 1));
    }
}

static void testSourceSumAndFreePoints() {
    Rig r;
    if (!r.load(boxText(5, 4, 3))) {
        check(false, "F5 Sum test room loads");
        return;
    }
    check(r.send(R"({"op":"source.setConfig","index":0,"format":1,"freePoints":true})").ok &&
          r.send(R"({"op":"source.setPointOffset","index":0,"channel":0,"offsetM":[0.25,0.5,0.1]})").ok,
          "F5 set a free point on an unsummed stereo source");
    const auto free = r.p.getSources()[0];
    check(r.send(R"({"op":"source.setConfig","index":0,"sum":true})").ok, "F5 Sum accepts an existing free-point layout");
    const auto summed = r.p.getSources();
    check(summed[0].sum && summed[0].freePoints && summed[0].ox == free.ox && summed[0].oy == free.oy
          && summed[0].oz == free.oz && verbp::buildPoints(summed)[0].channel == -1,
          "F5 Sum uses one point and preserves the free offsets");
    check(r.send(R"({"op":"source.setConfig","index":0,"freePoints":false})").ok &&
          r.send(R"({"op":"source.setConfig","index":0,"freePoints":true})").ok &&
          r.p.getSources()[0].sum && r.p.getSources()[0].freePoints && r.p.getSources()[0].ox.size() == 2,
          "F5 enabling Free pts under Sum preserves Sum and prepares both offsets");
    check(r.send(R"({"op":"source.setConfig","index":0,"sum":false})").ok &&
          r.p.getSources()[0].freePoints && verbp::buildPoints(r.p.getSources())[0].channel == 0,
          "F5 switching Sum off makes free points active again");
}

static void testG3AutomationRange(Rig& r) {
    std::printf("G3: automation range commands, schema validation, clamping and 1-step undo/redo\n");
    r.reset("G3 start");
    check(r.send(R"({"op":"listener.set","xM":14.0,"yM":1.0,"zM":1.5})").ok, "G3 listener.set at 14m before narrowing");
    check(r.send(R"({"op":"room.fitAutomationRange"})").ok, "G3 room.fitAutomationRange passes schema + executes");
    check(!r.p.getAutomationRange().isDefault(), "G3 fitted range is non-default");
    check(r.send(R"({"op":"room.setAutomationRange","xM":[0,8],"yM":[0,6],"zM":[0,4]})").ok,
          "G3 manual room.setAutomationRange passes schema + executes");
    check(std::abs(r.p.apvts.getRawParameterValue("lx")->load() - 8.0f) < 0.01f, "G3 lx=14 clamped to 8.0");
    check(!r.send(R"({"op":"listener.set","xM":12.0})").ok, "G3 listener.set rejects xM=12 outside [0,8]");
    check(r.session().undo().ok, "G3 undo restores pre-clamp coordinates and previous range in 1 step");
    check(std::abs(r.p.apvts.getRawParameterValue("lx")->load() - 14.0f) < 0.01f, "G3 undo restores lx=14");
    check(r.send(R"({"op":"room.setAutomationRange","reset":true})").ok && r.p.getAutomationRange().isDefault(),
          "G3 reset:true restores ±200 / 0..30 range");
    while (r.session().canUndo()) r.session().undo();
    r.reset("G3 end");
}

static void testG4ModulationCommands(Rig& r) {
    std::printf("G4: modulation.* and path.setLfo commands, schema validation, 64-route cap, source.remove reindex & undo\n");
    r.reset("G4 start");

    const std::string routeA = R"({"enabled":true,"object":0,"driver":"distanceToListener","curve":{"inputRange":[1,10],"outputRange":[20000,800],"invert":false,"clamp":true,"nodes":[{"x":0,"y":0,"smooth":false},{"x":0.5,"y":0.6,"smooth":true},{"x":1,"y":1,"smooth":false}]},"targetObject":-1,"target":"sourceLowPass","attackMs":15,"releaseMs":80})";
    const std::string routeB = R"({"enabled":true,"object":0,"driver":"distanceToPoint","point":[2,3,1.5],"curve":{"inputRange":[0,5],"outputRange":[-12,0],"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"target":"sourceGain"})";

    check(r.send(R"({"op":"modulation.addRoute","route":)" + routeA + "}").ok && r.p.getModulationRoutes().size() == 1,
          "G4 modulation.addRoute passes schema and adds route");
    check(r.send(R"({"op":"modulation.setRoute","index":0,"route":)" + routeB + "}").ok
          && r.p.getModulationRoutes()[0].target == verbp::ModTarget::SourceGain
          && r.p.getModulationRoutes()[0].pointY == 3.0,
          "G4 modulation.setRoute updates route with distanceToPoint");

    // Undo coalesced/previous route step
    check(r.session().undo().ok && r.p.getModulationRoutes()[0].target == verbp::ModTarget::SourceLowPass,
          "G4 undo restores modulation.setRoute");
    check(r.session().redo().ok && r.p.getModulationRoutes()[0].target == verbp::ModTarget::SourceGain,
          "G4 redo restores modulation.setRoute");

    // 64-route budget cap
    std::string arr64 = "[";
    for (int i = 0; i < 64; ++i) {
        if (i > 0) arr64 += ",";
        arr64 += routeA;
    }
    arr64 += "]";
    check(r.send(R"({"op":"modulation.setRoutes","routes":)" + arr64 + "}").ok && r.p.getModulationRoutes().size() == 64,
          "G4 modulation.setRoutes accepts 64 routes");
    const auto capRes = r.send(R"({"op":"modulation.addRoute","route":)" + routeA + "}");
    check(!capRes.ok && capRes.code == "budgetExceeded", "G4 modulation.addRoute rejects 65th route with budgetExceeded");

    // Validation rejections
    r.validateCommands = false;
    check(!r.send(R"({"op":"path.setLfo","object":0,"lfo":{"enabled":true,"rateHz":9,"depth":0.5}})").ok,
          "G4 path.setLfo rejects rateHz > 8");
    check(!r.send(R"({"op":"path.setLfo","object":0,"lfo":{"enabled":true,"rateHz":2,"depth":0.5}})").ok,
          "G4 path.setLfo rejects non-default LFO on empty path");
    check(!r.send(R"({"op":"modulation.addRoute","route":{"object":32,"driver":"x","curve":{"inputRange":[0,10],"outputRange":[0,100],"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"target":"mix","targetObject":0}})").ok,
          "G4 rejects global target with non-negative targetObject");
    r.validateCommands = true;

    // Path set + setLfo + flags + undo
    check(r.send(R"({"op":"path.set","object":0,"points":[[1,1,1.5],[4,1,1.5]]})").ok, "G4 path.set for LFO");
    check(r.send(R"({"op":"path.setLfo","object":0,"lfo":{"enabled":true,"shape":"smoothRandom","rateHz":3,"depth":0.4,"phaseDeg":45,"seed":12}})").ok,
          "G4 path.setLfo passes schema and updates PathLfo");
    check(r.p.getMovementPath(0).lfo.enabled && r.p.getMovementPath(0).lfo.shape == verbp::LfoShape::SmoothRandom && r.p.getMovementPath(0).lfo.seed == 12,
          "G4 processor stores PathLfo accurately");
    check(r.send(R"({"op":"path.clear","object":0})").ok && r.p.getMovementPath(0).lfo.isDefault(),
          "G4 path.clear resets PathLfo to default");

    // source.remove reindexing + 1-step undo/redo (Rig loads box with 2 sources: A and B)
    const std::string routeSrc1 = R"({"enabled":true,"object":1,"driver":"y","curve":{"inputRange":[0,10],"outputRange":[-6,0],"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"target":"sourceHighShelf"})";
    const std::string routeCross = R"({"enabled":true,"object":32,"driver":"z","curve":{"inputRange":[0,5],"outputRange":[20,1000],"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"targetObject":1,"target":"sourceHighPass"})";
    check(r.send(R"({"op":"modulation.setRoutes","routes":[)" + routeA + "," + routeSrc1 + "," + routeCross + "]}").ok
          && r.p.getModulationRoutes().size() == 3, "G4 set 3 routes across 2 sources");
    check(r.send(R"({"op":"source.remove","index":0})").ok, "G4 source.remove 0 succeeds");
    {
        const auto after = r.p.getModulationRoutes();
        check(after.size() == 2 && after[0].object == 0 && after[0].target == verbp::ModTarget::SourceHighShelf
              && after[1].object == verbp::kListener && after[1].targetObject == 0 && after[1].target == verbp::ModTarget::SourceHighPass,
              "G4 source.remove drops removed source routes and reindexes source 1 -> 0");
    }
    check(r.session().undo().ok && r.p.getModulationRoutes().size() == 3 && r.p.getSources().size() == 2,
          "G4 undo restores source 0 and original 3 modulation routes in 1 step");

    check(r.send(R"({"op":"modulation.removeRoute","index":0})").ok && r.p.getModulationRoutes().size() == 2,
          "G4 modulation.removeRoute works");
    check(r.send(R"({"op":"modulation.setRoutes","routes":[]})").ok && r.p.getModulationRoutes().empty(),
          "G4 modulation.setRoutes([]) clears routes");

    // G7: backend command clamping to target legal range & atomic 1-step undo on target switch
    {
        const std::string outOfRangeGain = R"({"enabled":true,"object":0,"driver":"distanceToListener","curve":{"inputRange":[1,15],"outputRange":[20,20000],"invert":false,"clamp":true,"nodes":[{"x":0,"y":0,"smooth":false},{"x":1,"y":1,"smooth":false}]},"targetObject":-1,"target":"sourceGain","attackMs":15,"releaseMs":80})";
        check(r.send(R"({"op":"modulation.addRoute","route":)" + outOfRangeGain + "}").ok,
              "G7 command accepts and clamps out-of-range sourceGain outputRange");
        {
            const auto routes = r.p.getModulationRoutes();
            check(routes.size() == 1 && routes[0].curve.outputMin == 12.0 && routes[0].curve.outputMax == 12.0,
                  "G7 sourceGain [20, 20000] clamped to [12, 12] in command layer");
        }
        const std::string outOfRangeMix = R"({"enabled":true,"object":32,"driver":"x","curve":{"inputRange":[-10,10],"outputRange":[200,-150],"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"target":"mix"})";
        check(r.send(R"({"op":"modulation.setRoute","index":0,"route":)" + outOfRangeMix + "}").ok,
              "G7 command accepts and clamps out-of-range mix outputRange");
        {
            const auto routes = r.p.getModulationRoutes();
            check(routes.size() == 1 && routes[0].curve.outputMin == 100.0 && routes[0].curve.outputMax == -100.0,
                  "G7 mix [200, -150] clamped to [100, -100]");
        }
        const std::string lpfFull = R"({"enabled":true,"object":0,"driver":"distanceToListener","curve":{"inputRange":[1,15],"outputRange":[20,20000],"invert":false,"clamp":true,"nodes":[{"x":0,"y":0,"smooth":true},{"x":1,"y":1,"smooth":false}]},"targetObject":-1,"target":"sourceLowPass","attackMs":15,"releaseMs":80})";
        const std::string gainSafe = R"({"enabled":true,"object":0,"driver":"distanceToListener","curve":{"inputRange":[1,15],"outputRange":[0,-24],"invert":false,"clamp":true,"nodes":[{"x":0,"y":0,"smooth":false},{"x":1,"y":1,"smooth":false}]},"targetObject":-1,"target":"sourceGain","attackMs":15,"releaseMs":80})";
        check(r.send(R"({"op":"modulation.setRoute","index":0,"route":)" + lpfFull + "}").ok,
              "G7 set LPF 20..20000 route");
        check(r.send(R"({"op":"modulation.setRoute","index":0,"route":)" + gainSafe + "}").ok,
              "G7 switch target to safe gain 0..-24 immediately (<750ms)");
        check(r.session().undo().ok, "G7 undo after target switch succeeds");
        {
            const auto routes = r.p.getModulationRoutes();
            check(routes.size() == 1 && routes[0].target == verbp::ModTarget::SourceLowPass
                  && routes[0].curve.outputMin == 20.0 && routes[0].curve.outputMax == 20000.0,
                  "G7 single undo restores LPF target AND [20, 20000] range without coalescing");
        }
        check(r.send(R"({"op":"modulation.setRoutes","routes":[]})").ok, "G7 clear routes");
    }

    // G8: target='none' ("Host output only") + hostOut 0..16 validation, source.remove, undo/redo
    {
        const std::string hostRoute = R"({"enabled":true,"object":1,"driver":"x","curve":{"inputRange":[0,10],"outputRange":[0,1],"invert":false,"clamp":true,"nodes":[{"x":0,"y":0,"smooth":false},{"x":1,"y":1,"smooth":false}]},"targetObject":-1,"target":"none","hostOut":4,"attackMs":15,"releaseMs":80})";
        check(r.send(R"({"op":"modulation.addRoute","route":)" + hostRoute + "}").ok,
              "G8 modulation.addRoute accepts target=none and hostOut=4");
        {
            const auto routes = r.p.getModulationRoutes();
            check(routes.size() == 1 && routes[0].target == verbp::ModTarget::None && routes[0].hostOut == 4,
                  "G8 processor stores target=None and hostOut=4");
        }
        r.validateCommands = false;
        check(!r.send(R"({"op":"modulation.addRoute","route":{"object":0,"driver":"x","curve":{"inputRange":[0,10],"outputRange":[0,1],"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"target":"none","targetObject":0,"hostOut":1}})").ok,
              "G8 rejects target=none with non-negative targetObject");
        check(!r.send(R"({"op":"modulation.addRoute","route":{"object":0,"driver":"x","curve":{"inputRange":[0,10],"outputRange":[0,1],"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"target":"none","hostOut":17}})").ok,
              "G8 rejects hostOut > 16");
        check(!r.send(R"({"op":"modulation.addRoute","route":{"object":0,"driver":"x","curve":{"inputRange":[0,10],"outputRange":[0,1],"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"target":"none","hostOut":-1}})").ok,
              "G8 rejects negative hostOut");
        r.validateCommands = true;

        // Changing hostOut on existing route creates a clean undo step (coalescing includes hostOut)
        const std::string hostRouteMoved = R"({"enabled":true,"object":1,"driver":"x","curve":{"inputRange":[0,10],"outputRange":[0,1],"invert":false,"clamp":true,"nodes":[{"x":0,"y":0,"smooth":false},{"x":1,"y":1,"smooth":false}]},"targetObject":-1,"target":"none","hostOut":9,"attackMs":15,"releaseMs":80})";
        check(r.send(R"({"op":"modulation.setRoute","index":0,"route":)" + hostRouteMoved + "}").ok
              && r.p.getModulationRoutes()[0].hostOut == 9,
              "G8 setRoute updates hostOut from 4 to 9");
        check(r.session().undo().ok && r.p.getModulationRoutes()[0].hostOut == 4,
              "G8 undo restores previous hostOut=4");
        check(r.session().redo().ok && r.p.getModulationRoutes()[0].hostOut == 9,
              "G8 redo restores hostOut=9");

        // source.remove drops route on source 0, reindexes source 1 -> 0 while preserving hostOut=9
        const std::string src0Host = R"({"enabled":true,"object":0,"driver":"y","curve":{"inputRange":[0,10],"outputRange":[0,1],"invert":false,"clamp":true,"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"targetObject":-1,"target":"none","hostOut":5,"attackMs":15,"releaseMs":80})";
        check(r.send(R"({"op":"modulation.setRoutes","routes":[)" + src0Host + "," + hostRouteMoved + "]}").ok,
              "G8 set two hostOut routes across 2 sources");
        check(r.send(R"({"op":"source.remove","index":0})").ok, "G8 source.remove 0 succeeds");
        {
            const auto after = r.p.getModulationRoutes();
            check(after.size() == 1 && after[0].object == 0 && after[0].target == verbp::ModTarget::None && after[0].hostOut == 9,
                  "G8 source.remove drops src0 route and keeps src1->src0 hostOut=9");
        }
        check(r.session().undo().ok && r.p.getModulationRoutes().size() == 2 && r.p.getModulationRoutes()[0].hostOut == 5,
              "G8 undo restores removed source and both hostOut routes");
        check(r.send(R"({"op":"modulation.setRoutes","routes":[]})").ok, "G8 clear routes");
    }

    while (r.session().canUndo()) r.session().undo();
    r.reset("G4 end");
}

static void testMotionCommand(Rig& r) {
    const auto serial = r.p.getSceneSerial();
    check(r.send(R"({"op":"tail.setMotion","enabled":true})").ok && r.p.motionTailEnabled(),
          "MO3b Motion command enables the document switch");
    r.p.setTailSettings({16000, true});
    check(r.session().undo().ok && !r.p.motionTailEnabled()
          && r.p.getTailSettings().rays == 16000 && r.p.getSceneSerial() == serial + 1,
          "MO3b scoped Motion undo preserves an independent tail quality edit");
    check(r.session().redo().ok && r.p.motionTailEnabled(), "MO3b Motion redo restores On");
    r.validateCommands = false;
    check(!r.send(R"({"op":"tail.setMotion","enabled":1})").ok && r.p.motionTailEnabled(),
          "MO3b Motion rejects non-boolean input");
    r.validateCommands = true;
    r.session().undo();
}

static void testV04aPathEndlessAndShapes(Rig& r) {
    std::printf("V04a: path.set / path.setFlags endless & endlessPeriodS commands, validation, and 1-step shape undo/redo\n");
    r.reset("V04a start");
    r.validateCommands = false;
    check(!r.send(R"({"op":"path.setFlags","object":0,"endless":true})").ok,
          "V04a rejects endless=true on empty path");
    check(!r.send(R"({"op":"path.setFlags","object":0,"endlessPeriodS":5.0})").ok,
          "V04a rejects non-default endlessPeriodS on empty path");
    r.validateCommands = true;

    // Generate a 16-node circle via path.set in one undoable command
    check(r.send(R"({"op":"path.set","object":0,"points":[[5,4,1.5],[4.8478,4.7654,1.5],[4.4142,5.4142,1.5],[3.7654,5.8478,1.5],[3,6,1.5],[2.2346,5.8478,1.5],[1.5858,5.4142,1.5],[1.1522,4.7654,1.5],[1,4,1.5],[1.1522,3.2346,1.5],[1.5858,2.5858,1.5],[2.2346,2.1522,1.5],[3,2,1.5],[3.7654,2.1522,1.5],[4.4142,2.5858,1.5],[4.8478,3.2346,1.5]],"enabled":true,"closed":true,"smooth":true,"endless":true,"endlessPeriodS":6.0})").ok,
          "V04a path.set applies closed circle shape with endless=true and endlessPeriodS=6.0 in one command");
    {
        const auto p0 = r.p.getMovementPath(0);
        check(p0.points.size() == 16 && p0.closed && p0.smooth && p0.endless && std::abs(p0.endlessPeriodS - 6.0) < 1e-9,
              "V04a processor stores 16-point closed smooth shape with endless=true and endlessPeriodS=6.0");
    }

    // Re-applying another shape (4-point Square) replaces the shape in ONE undoable step while preserving endless
    check(r.send(R"({"op":"path.set","object":0,"points":[[4.5,5.5,1.5],[1.5,5.5,1.5],[1.5,2.5,1.5],[4.5,2.5,1.5]],"enabled":true,"closed":true,"smooth":false})").ok,
          "V04a re-applying Square replaces shape in one command");
    {
        const auto pSq = r.p.getMovementPath(0);
        check(pSq.points.size() == 4 && pSq.closed && !pSq.smooth && pSq.endless && std::abs(pSq.endlessPeriodS - 6.0) < 1e-9,
              "V04a replaced Square has 4 points, smooth=false, and preserves endless=true");
    }
    check(r.session().undo().ok && r.p.getMovementPath(0).points.size() == 16 && r.p.getMovementPath(0).smooth,
          "V04a single undo restores previous 16-point Circle shape");
    check(r.session().redo().ok && r.p.getMovementPath(0).points.size() == 4 && !r.p.getMovementPath(0).smooth,
          "V04a single redo restores 4-point Square shape");

    // Toggle endless and endlessPeriodS via path.setFlags
    check(r.send(R"({"op":"path.setFlags","object":0,"endless":false,"endlessPeriodS":12.5})").ok,
          "V04a path.setFlags updates endless=false and endlessPeriodS=12.5");
    check(!r.p.getMovementPath(0).endless && std::abs(r.p.getMovementPath(0).endlessPeriodS - 12.5) < 1e-9,
          "V04a processor reflects endless=false and endlessPeriodS=12.5");

    r.validateCommands = false;
    check(!r.send(R"({"op":"path.setFlags","object":0,"endlessPeriodS":0.1})").ok,
          "V04a path.setFlags rejects endlessPeriodS < 0.25");
    check(!r.send(R"({"op":"path.setFlags","object":0,"endlessPeriodS":4000})").ok,
          "V04a path.setFlags rejects endlessPeriodS > 3600");
    check(!r.send(R"({"op":"path.setFlags","object":0,"endless":"yes"})").ok,
          "V04a path.setFlags rejects non-boolean endless");
    r.validateCommands = true;

    check(r.send(R"({"op":"path.clear","object":0})").ok
          && !r.p.getMovementPath(0).endless
          && r.p.getMovementPath(0).endlessPeriodS == verbp::kDefaultEndlessPeriodS,
          "V04a path.clear resets endless and endlessPeriodS to defaults");
    while (r.session().canUndo()) r.session().undo();
    r.reset("V04a end");
}

static void testV04bPathWanderCommands(Rig& r) {
    std::printf("  [V04b: path.setWander, path.set/setFlags wander, coalescing, undo/redo, validation]\n");
    r.reset("V04b start");
    r.validateCommands = true;

    // 1. Apply path.set with wander object in one undoable command
    check(r.send(R"({"op":"path.set","object":0,"points":[[1.5,2,1.5],[4.5,2,1.5],[4.5,6,1.5],[1.5,6,1.5]],"enabled":true,"closed":true,"smooth":true,"wander":{"enabled":true,"ampHorizM":0.6,"ampVertM":0.3,"rateHz":1.2,"smoothness":0.75,"pinEnds":false,"horizOnly":false,"seed":101}})").ok,
          "V04b path.set applies closed path with wander in one command");
    {
        const auto w = r.p.getMovementPath(0).wander;
        check(w.enabled && std::abs(w.ampHorizM - 0.6) < 1e-9 && std::abs(w.ampVertM - 0.3) < 1e-9
              && std::abs(w.rateHz - 1.2) < 1e-9 && std::abs(w.smoothness - 0.75) < 1e-9
              && !w.pinEnds && !w.horizOnly && w.seed == 101,
              "V04b processor stores PathWander from path.set");
    }

    // 2. path.setWander updates wander and coalesces inside a transaction (one undo step per gesture)
    const int depthBeforeTx = r.session().undoDepth();
    check(r.send(R"({"op":"transaction.begin","label":"Edit path wander horizontal amplitude"})", nullptr, "tx-v04b").ok, "V04b begin wander slider transaction");
    check(r.send(R"({"op":"path.setWander","object":0,"wander":{"enabled":true,"ampHorizM":0.8,"ampVertM":0.3,"rateHz":1.2,"smoothness":0.75,"pinEnds":false,"horizOnly":false,"seed":101}})", nullptr, "tx-v04b").ok,
          "V04b path.setWander step 1");
    check(r.send(R"({"op":"path.setWander","object":0,"wander":{"enabled":true,"ampHorizM":1.25,"ampVertM":0.4,"rateHz":1.5,"smoothness":0.9,"pinEnds":true,"horizOnly":true,"seed":202}})", nullptr, "tx-v04b").ok,
          "V04b path.setWander step 2");
    check(r.send(R"({"op":"transaction.commit"})", nullptr, "tx-v04b").ok, "V04b end wander slider transaction");
    check(r.session().undoDepth() == depthBeforeTx + 1, "V04b wander gesture records a single undo step");
    {
        const auto w = r.p.getMovementPath(0).wander;
        check(w.enabled && std::abs(w.ampHorizM - 1.25) < 1e-9 && std::abs(w.ampVertM - 0.4) < 1e-9
              && std::abs(w.rateHz - 1.5) < 1e-9 && std::abs(w.smoothness - 0.9) < 1e-9
              && w.pinEnds && w.horizOnly && w.seed == 202,
              "V04b processor reflects committed path.setWander values");
    }
    check(r.session().undo().ok && std::abs(r.p.getMovementPath(0).wander.ampHorizM - 0.6) < 1e-9 && r.p.getMovementPath(0).wander.seed == 101,
          "V04b single undo reverts path.setWander gesture");
    check(r.session().redo().ok && std::abs(r.p.getMovementPath(0).wander.ampHorizM - 1.25) < 1e-9 && r.p.getMovementPath(0).wander.seed == 202,
          "V04b single redo reapplies path.setWander gesture");

    // 3. path.setFlags with optional wander updates wander alongside flags
    check(r.send(R"({"op":"path.setFlags","object":0,"closed":false,"wander":{"enabled":true,"ampHorizM":0.45,"ampVertM":0.2,"rateHz":0.5,"smoothness":0.8,"pinEnds":true,"horizOnly":false,"seed":333}})").ok,
          "V04b path.setFlags accepts wander");
    check(!r.p.getMovementPath(0).closed && std::abs(r.p.getMovementPath(0).wander.ampHorizM - 0.45) < 1e-9 && r.p.getMovementPath(0).wander.seed == 333,
          "V04b processor reflects path.setFlags wander update");

    // 4. Rejections for out-of-range or invalid wander fields and empty path
    r.validateCommands = false;
    check(!r.send(R"({"op":"path.setWander","object":0,"wander":{"ampHorizM":-0.1}})").ok,
          "V04b path.setWander rejects negative ampHorizM");
    check(!r.send(R"({"op":"path.setWander","object":0,"wander":{"ampHorizM":25.0}})").ok,
          "V04b path.setWander rejects ampHorizM > 20");
    check(!r.send(R"({"op":"path.setWander","object":0,"wander":{"ampVertM":12.0}})").ok,
          "V04b path.setWander rejects ampVertM > 10");
    check(!r.send(R"({"op":"path.setWander","object":0,"wander":{"rateHz":0.01}})").ok,
          "V04b path.setWander rejects rateHz < 0.05");
    check(!r.send(R"({"op":"path.setWander","object":0,"wander":{"rateHz":9.0}})").ok,
          "V04b path.setWander rejects rateHz > 8");
    check(!r.send(R"({"op":"path.setWander","object":0,"wander":{"smoothness":1.5}})").ok,
          "V04b path.setWander rejects smoothness > 1");
    check(!r.send(R"({"op":"path.setWander","object":0,"wander":{"seed":0}})").ok,
          "V04b path.setWander rejects seed < 1");
    check(!r.send(R"({"op":"path.setWander","object":0,"wander":{"seed":1000000}})").ok,
          "V04b path.setWander rejects seed > 999999");
    check(!r.send(R"({"op":"path.setWander","object":32,"wander":{"enabled":true,"ampHorizM":0.5}})").ok,
          "V04b path.setWander rejects non-default wander on object with fewer than 2 path points");
    r.validateCommands = true;

    check(r.send(R"({"op":"path.clear","object":0})").ok && r.p.getMovementPath(0).wander.isDefault(),
          "V04b path.clear resets wander to defaults");
    while (r.session().canUndo()) r.session().undo();
    r.reset("V04b end");
}

static void testV04cZoneCommands(Rig& r) {
    std::printf("  [V04c: zone.create / zone.set / zone.delete / zone.move, ids, room clamp, validation, gesture = 1 undo step, routes with zoneId]\n");
    r.reset("V04c start");
    r.validateCommands = true;
    const int d0 = r.session().undoDepth();
    check(r.send(R"({"op":"zone.create","kind":"box","points":[[1,1],[2.5,3]]})").ok && r.p.getTriggerZones().size() == 1,
          "V04c zone.create box");
    {
        const auto z = r.p.getTriggerZones()[0];
        const double ceil = r.p.getDisplayScene().height;
        check(z.id == 1 && z.name == "Zone 1" && !z.polygon && z.zMin == 0.0 && std::abs(z.zMax - ceil) < 1e-9 && std::abs(z.hysteresis - 0.05) < 1e-12 && z.edge == 0.0 && z.enabled && z.visible,
              "V04c new zone: id 1, name, floor..ceiling, hysteresis 0.05 m, hard edge");
        check(r.session().undoLabel() == "Add zone" && r.session().undoDepth() == d0 + 1, "V04c zone.create is one undo step labelled 'Add zone'");
    }
    check(r.send(R"({"op":"zone.create","kind":"polygon","name":"Tile","points":[[3,1],[5,1],[5,3],[4,2],[3,3]],"edge":0.4,"hysteresis":0.1,"colour":"#22C55E","zMax":99})").ok,
          "V04c zone.create concave polygon with soft edge and colour");
    {
        const auto zs = r.p.getTriggerZones();
        const double ceil = r.p.getDisplayScene().height;
        check(zs.size() == 2 && zs[1].id == 2 && zs[1].polygon && zs[1].points.size() == 5 && zs[1].colour == "#22c55e" && std::abs(zs[1].zMax - ceil) < 1e-9 && std::abs(zs[1].edge - 0.4) < 1e-12,
              "V04c polygon zone stored with id 2, colour normalised, zMax clamped to the ceiling");
    }
    check(r.send(R"({"op":"zone.create","kind":"box","points":[[-50,-50],[1,1]]})").ok, "V04c zone.create box partly outside the room");
    {
        const auto z = r.p.getTriggerZones().back();
        check(z.id == 3 && z.points[0].first >= -1e-9 && z.points[0].second >= -1e-9, "V04c zone points are clamped to the room bounds");
    }
    r.validateCommands = false;
    const auto selfX = r.send(R"({"op":"zone.create","kind":"polygon","points":[[0.5,0.5],[2,2],[2,0.5],[0.5,2]]})");
    check(!selfX.ok && selfX.message.find("crosses itself") != std::string::npos,
          "V04c self-intersecting polygon rejected with a clear message");
    check(!r.send(R"({"op":"zone.create","kind":"box","points":[[1,1],[1,3]]})").ok, "V04c degenerate box rejected");
    check(!r.send(R"({"op":"zone.create","kind":"polygon","points":[[1,1],[2,1]]})").ok, "V04c polygon with 2 vertices rejected");
    check(!r.send(R"({"op":"zone.set","id":99,"name":"x"})").ok, "V04c zone.set on a missing id rejected");
    check(!r.send(R"({"op":"zone.set","id":1,"edge":11})").ok && !r.send(R"({"op":"zone.set","id":1,"hysteresis":-1})").ok && !r.send(R"({"op":"zone.set","id":1,"colour":"red"})").ok,
          "V04c out-of-range edge / hysteresis / colour rejected");
    r.validateCommands = true;
    // Drag gesture: 40 zone.set steps inside a transaction -> 1 undo step; Esc (cancel) -> 0 steps, geometry restored.
    const auto before = r.p.getTriggerZones();
    const int dBefore = r.session().undoDepth();
    check(r.send(R"({"op":"transaction.begin","label":"Move zone vertex"})", nullptr, "tx-v04c").ok, "V04c begin vertex drag");
    bool allOk = true;
    for (int k = 1; k <= 40; ++k) {
        const std::string x = juce::String(2.5 + 0.02 * k, 3).toStdString();
        allOk &= r.send(R"({"op":"zone.set","id":1,"points":[[1,1],[)" + x + R"(,3]]})", nullptr, "tx-v04c").ok;
        allOk &= std::abs(r.p.getTriggerZones()[0].points[1].first - (2.5 + 0.02 * k)) < 1e-6;
    }
    check(allOk, "V04c 40 live zone.set steps apply immediately inside the gesture (live preview)");
    check(r.send(R"({"op":"transaction.commit"})", nullptr, "tx-v04c").ok && r.session().undoDepth() == dBefore + 1 && r.session().undoLabel() == "Move zone vertex",
          "V04c zone vertex drag = 1 undo step with the gesture label");
    check(r.session().undo().ok && r.p.getTriggerZones() == before, "V04c one undo restores the zone before the drag");
    check(r.session().redo().ok && std::abs(r.p.getTriggerZones()[0].points[1].first - 3.3) < 1e-6, "V04c redo re-applies the drag");
    const auto mid = r.p.getTriggerZones();
    const int dMid = r.session().undoDepth();
    check(r.send(R"({"op":"transaction.begin","label":"Move zone"})", nullptr, "tx-v04c2").ok
          && r.send(R"({"op":"zone.set","id":2,"points":[[3.5,1.5],[5.5,1.5],[5.5,3.5],[4.5,2.5],[3.5,3.5]]})", nullptr, "tx-v04c2").ok
          && r.send(R"({"op":"transaction.cancel"})", nullptr, "tx-v04c2").ok,
          "V04c zone move gesture cancelled (Esc)");
    check(r.p.getTriggerZones() == mid && r.session().undoDepth() == dMid, "V04c cancelled gesture restores the zone and adds no undo step");
    // A single zone.set outside a gesture gets the descriptor label.
    check(r.send(R"({"op":"zone.set","id":1,"points":[[1,1],[3,3]]})").ok && r.session().undoLabel() == "Edit zone shape", "V04c zone.set points -> 'Edit zone shape'");
    // Routes referencing a zone; zoneId required for In zone / Distance to zone.
    check(r.send(R"({"op":"modulation.addRoute","route":{"enabled":true,"object":0,"driver":"inZone","zoneId":2,"curve":{"inputRange":[0,1],"outputRange":[0,1],"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"targetObject":-1,"target":"none","hostOut":1,"attackMs":0,"releaseMs":0}})").ok
          && r.p.getModulationRoutes().back().zoneId == 2 && r.p.getModulationRoutes().back().driver == verbp::ModDriver::InZone,
          "V04c modulation route with driver inZone + zoneId");
    check(r.send(R"({"op":"modulation.addRoute","route":{"enabled":true,"object":32,"driver":"zoneIndex","curve":{"inputRange":[0,3],"outputRange":[0,1],"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"targetObject":-1,"target":"none","hostOut":2}})").ok,
          "V04c listener route with driver zoneIndex");
    check(r.send(R"({"op":"modulation.addRoute","route":{"enabled":true,"object":0,"driver":"distanceToZone","zoneId":1,"curve":{"inputRange":[-1,3],"outputRange":[1,0],"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"targetObject":-1,"target":"none","hostOut":3}})").ok,
          "V04c route with driver distanceToZone");
    r.validateCommands = false;
    check(!r.send(R"({"op":"modulation.addRoute","route":{"enabled":true,"object":0,"driver":"inZone","curve":{"inputRange":[0,1],"outputRange":[0,1],"nodes":[{"x":0,"y":0},{"x":1,"y":1}]},"targetObject":-1,"target":"none","hostOut":1}})").ok,
          "V04c inZone route without zoneId rejected");
    r.validateCommands = true;
    check(r.send(R"({"op":"zone.move","id":3,"index":0})").ok && r.p.getTriggerZones()[0].id == 3 && r.session().undoLabel() == "Reorder zones", "V04c zone.move reorders the list");
    check(r.send(R"({"op":"zone.set","id":2,"name":"Tile floor","enabled":false,"visible":false})").ok && !r.p.getTriggerZones()[2].enabled && r.p.getTriggerZones()[2].name == "Tile floor" && r.session().undoLabel() == "Edit zone",
          "V04c zone.set name / enabled / visible ('Edit zone')");
    check(r.send(R"({"op":"zone.delete","id":2})").ok && r.p.getTriggerZones().size() == 2 && r.p.getModulationRoutes().size() == 3 && r.session().undoLabel() == "Delete zone",
          "V04c zone.delete keeps the routes (they read 'outside' until re-pointed)");
    check(r.session().undo().ok && r.p.getTriggerZones().size() == 3 && r.p.getTriggerZones()[2].id == 2, "V04c undo of delete restores the zone with its id and position");
    check(r.send(R"({"op":"zone.create","kind":"box","points":[[2,2],[3,2.5]]})").ok && r.p.getTriggerZones().back().id == 4, "V04c ids are stable (max + 1)");
    r.validateCommands = false;
    int created = (int)r.p.getTriggerZones().size();
    while (created < 16) { r.send(R"({"op":"zone.create","kind":"box","points":[[0.5,0.5],[1.5,1.5]]})"); ++created; }
    const auto over = r.send(R"({"op":"zone.create","kind":"box","points":[[0.5,0.5],[1.5,1.5]]})");
    check(r.p.getTriggerZones().size() == 16 && !over.ok && over.code == "budgetExceeded", "V04c at most 16 zones (budgetExceeded)");
    r.validateCommands = true;
    while (r.session().canUndo()) r.session().undo();
    check(r.p.getTriggerZones().empty() && r.p.getModulationRoutes().empty(), "V04c undo everything returns to no zones and no routes");
    r.reset("V04c end");
}

static void testV04a2LiveShapeAndSpiralAndFigure8(Rig& r) {
    std::printf("  [V04a-2: 64-step live shape parameter transaction coalescing, Esc cancel, Spiral start/end Z, Figure-8 default centre]\n");
    r.reset("V04a-2 start");
    r.validateCommands = true;
    const auto sc = r.p.getDisplayScene();

    // 1. Set an initial 4-point Square path on source 0
    verbp::PathShapeParams sq;
    sq.kind = verbp::PathShapeKind::Square;
    sq.cx = 2.5; sq.cy = 2.0; sq.z = 1.5; sq.radiusX = 1.0;
    const auto sqPath = verbp::generatePathShapeForRoom(sq, &sc, true).path;
    auto sendShape = [&](const verbp::MovementPath& mp, const char* txId = nullptr) {
        juce::String ptsJson = "[";
        for (size_t i = 0; i < mp.points.size(); ++i) {
            if (i > 0) ptsJson += ",";
            ptsJson += "[" + juce::String(mp.points[i][0], 4) + ","
                           + juce::String(mp.points[i][1], 4) + ","
                           + juce::String(mp.points[i][2], 4) + "]";
        }
        ptsJson += "]";
        juce::String cmd = R"({"op":"path.set","object":0,"points":)" + ptsJson
            + R"(,"enabled":true,"closed":true,"smooth":)" + (mp.smooth ? "true" : "false") + "}";
        return r.send(cmd.toStdString(), nullptr, txId);
    };
    check(sendShape(sqPath).ok && r.p.getMovementPath(0).points.size() == 4,
          "V04a-2 initial Square shape applied (1 undo step)");
    const auto initialPts = r.p.getMovementPath(0).points;
    const int depthBeforeLiveDrag = r.session().undoDepth();

    // 2. Simulate a 64-step live parameter drag (Circle radius from 0.6 m to 1.8 m) inside one transaction
    check(r.send(R"({"op":"transaction.begin","label":"Edit shape radius, m"})", nullptr, "tx-v04a2-live").ok,
          "V04a-2 begin 64-step live shape parameter transaction");
    bool allStepsUpdatedLive = true;
    verbp::MovementPath lastStepPath;
    for (int step = 0; step < 64; ++step) {
        verbp::PathShapeParams sp;
        sp.kind = verbp::PathShapeKind::Circle;
        sp.cx = 2.5; sp.cy = 2.0; sp.z = 1.5;
        sp.radiusX = 0.6 + (1.2 * (double)step / 63.0);
        sp.nodes = 16;
        lastStepPath = verbp::generatePathShapeForRoom(sp, &sc, true).path;
        const auto prevStepPts = r.p.getMovementPath(0).points;
        const auto res = sendShape(lastStepPath, "tx-v04a2-live");
        const auto curStepPts = r.p.getMovementPath(0).points;
        if (!res.ok || curStepPts.size() != 16 || curStepPts == prevStepPts) {
            allStepsUpdatedLive = false;
            break;
        }
    }
    check(allStepsUpdatedLive, "V04a-2 path points update live on every one of the 64 intermediate parameter steps before confirm");
    check(r.send(R"({"op":"transaction.commit"})", nullptr, "tx-v04a2-live").ok,
          "V04a-2 commit 64-step live shape parameter transaction");
    check(r.session().undoDepth() == depthBeforeLiveDrag + 1,
          "V04a-2 64-step live parameter drag produces exactly 1 undo step");

    // Verify final points are bit-exact equal to a single confirmed update at radius = 1.8 m
    const auto committedPts = r.p.getMovementPath(0).points;
    check(r.session().undo().ok && r.p.getMovementPath(0).points == initialPts,
          "V04a-2 single undo after 64-step drag restores original Square path");
    check(r.session().redo().ok && r.p.getMovementPath(0).points == committedPts,
          "V04a-2 single redo after 64-step drag restores final Circle path");

    // Compare against single-shot confirmed update
    r.session().undo();
    check(sendShape(lastStepPath).ok && r.p.getMovementPath(0).points == committedPts,
          "V04a-2 final points after 64-step live drag are bit-exact equal to a single confirmed update");

    // 3. Simulate a 60-step live parameter drag cancelled by Escape (transaction.cancel)
    const auto beforeCancelPts = r.p.getMovementPath(0).points;
    const int depthBeforeCancel = r.session().undoDepth();
    check(r.send(R"({"op":"transaction.begin","label":"Edit shape radius, m"})", nullptr, "tx-v04a2-esc").ok,
          "V04a-2 begin live drag to cancel with Escape");
    for (int step = 0; step < 60; ++step) {
        verbp::PathShapeParams sp;
        sp.kind = verbp::PathShapeKind::Star;
        sp.cx = 2.5; sp.cy = 2.0; sp.z = 1.5;
        sp.radiusX = 0.7 + (1.0 * (double)step / 59.0);
        sp.radiusY = 0.35 * sp.radiusX;
        sendShape(verbp::generatePathShapeForRoom(sp, &sc, true).path, "tx-v04a2-esc");
    }
    check(r.p.getMovementPath(0).points.size() == 10 && r.p.getMovementPath(0).points != beforeCancelPts,
          "V04a-2 live drag modified path to 10-point Star before Escape");
    check(r.send(R"({"op":"transaction.cancel"})", nullptr, "tx-v04a2-esc").ok,
          "V04a-2 Escape cancels live shape parameter transaction");
    check(r.p.getMovementPath(0).points == beforeCancelPts && r.session().undoDepth() == depthBeforeCancel,
          "V04a-2 Escape restores original path bit-identically and adds 0 undo steps");

    while (r.session().canUndo()) r.session().undo();
    r.reset("V04a-2 end");
}

static void testV04dLinkCommands(Rig& r) {
    std::printf("  [V04d: path.link / path.setFollow / path.unlink, rig of 1 target + 3 followers, no chains, policies, gesture = 1 undo step, schema]\n");
    r.reset("V04d start");
    r.validateCommands = true;
    while ((int)r.p.getSources().size() < 4) check(r.send(R"({"op":"source.add"})").ok, "V04d add a source for the rig");
    check(r.send(R"({"op":"path.set","object":0,"points":[[1.5,2,1.2],[4.5,2,1.2],[4.5,6,1.2],[1.5,6,1.2]],"enabled":true,"closed":true,"smooth":true})").ok, "V04d target path (closed, 4 points)");
    const int d0 = r.session().undoDepth();
    check(r.send(R"({"op":"path.link","objects":[1,2,3],"target":0})").ok && r.session().undoDepth() == d0 + 1 && r.session().undoLabel() == "Link 3 sources to path",
          "V04d multi-select path.link = one undo step 'Link 3 sources to path'");
    bool linked = true;
    for (int o = 1; o <= 3; ++o) { const auto& fp = r.p.getMovementPath(o); linked &= fp.follow.target == 0 && fp.points.empty() && fp.follow.followWander; }
    check(linked, "V04d three followers: target 0, no own points, Follow wander ON by default");
    check(r.send(R"({"op":"path.setFollow","object":1,"offset":[0,0,0.45]})").ok && r.session().undoLabel() == "Edit link offset"
          && std::abs(r.p.getMovementPath(1).follow.offset[2] - 0.45) < 1e-9, "V04d path.setFollow offset ('Edit link offset')");
    // Drag gesture: 30 live offset steps = 1 undo step; Esc restores.
    const int d1 = r.session().undoDepth();
    bool live = r.send(R"({"op":"transaction.begin","label":"Edit link offset"})", nullptr, "tx-v04d").ok;
    for (int k = 1; k <= 30; ++k) {
        const std::string v = juce::String(0.01 * k, 2).toStdString();
        live &= r.send(R"({"op":"path.setFollow","object":2,"offset":[0,)" + v + R"(,0.9]})", nullptr, "tx-v04d").ok;
        live &= std::abs(r.p.getMovementPath(2).follow.offset[1] - 0.01 * k) < 1e-9;
    }
    check(live, "V04d 30 live offset steps apply immediately inside the gesture");
    check(r.send(R"({"op":"transaction.commit"})", nullptr, "tx-v04d").ok && r.session().undoDepth() == d1 + 1 && r.session().undoLabel() == "Edit link offset",
          "V04d offset drag = 1 undo step");
    const auto mid = r.p.getMovementPath(3);
    const int d2 = r.session().undoDepth();
    check(r.send(R"({"op":"transaction.begin","label":"Edit link offset"})", nullptr, "tx-v04d2").ok
          && r.send(R"({"op":"path.setFollow","object":3,"offset":[0.3,-0.2,-0.9]})", nullptr, "tx-v04d2").ok
          && r.send(R"({"op":"transaction.cancel"})", nullptr, "tx-v04d2").ok, "V04d offset gesture cancelled (Esc)");
    check(r.p.getMovementPath(3) == mid && r.session().undoDepth() == d2, "V04d cancelled gesture restores the offset and adds no undo step");
    check(r.send(R"({"op":"path.setFollow","object":3,"followWander":false})").ok && !r.p.getMovementPath(3).follow.followWander && r.session().undoLabel() == "Follow wander",
          "V04d path.setFollow followWander ('Follow wander')");
    // No chains / cycles / self links; locked controls on a follower.
    r.validateCommands = false;
    const auto chain = r.send(R"({"op":"path.link","object":0,"target":1})");
    check(!chain.ok && chain.message.find("no chains") != std::string::npos, "V04d a follower cannot be a target (no chains)");
    const auto targetFollows = r.send(R"({"op":"path.link","object":0,"target":0})");
    check(!targetFollows.ok, "V04d self link rejected");
    check(!r.send(R"({"op":"path.setFollow","object":1,"offset":[0,0,60]})").ok, "V04d offset beyond +/-50 m rejected");
    check(!r.send(R"({"op":"path.setFollow","object":0,"offset":[0,0,1]})").ok, "V04d path.setFollow on a non-follower rejected");
    const auto locked = r.send(R"({"op":"path.setFlags","object":1,"laps":3})");
    check(!locked.ok && locked.message.find("unlink first") != std::string::npos, "V04d Laps locked on a follower (message names the target)");
    check(!r.send(R"({"op":"path.setFlags","object":1,"endless":true})").ok, "V04d Endless locked on a follower");
    { const auto g = r.send(R"({"op":"path.setFlags","object":1,"group":0})"); check(!g.ok && g.message.find("Coupling") != std::string::npos && r.p.getMovementPath(1).group == -1, "V04d Coupling refused on a follower"); }
    r.validateCommands = true;
    check(r.send(R"({"op":"path.setFlags","object":1,"orient":true})").ok && r.p.getMovementPath(1).orient, "V04d orientation stays editable on a follower");
    // Bridge document carries the follow record (schema-validated by the snapshot test in WebBridgeTest).
    // Unlink keeps the motion as an own path.
    check(r.send(R"({"op":"path.unlink","object":2})").ok && r.session().undoLabel() == "Unlink path"
          && !r.p.getMovementPath(2).follow.active() && r.p.getMovementPath(2).points.size() == 64,
          "V04d path.unlink: own path of 64 points ('Unlink path')");
    check(r.session().undo().ok && r.p.getMovementPath(2).follow.target == 0, "V04d undo of unlink restores the link");
    // Delete-target policy: clearing the target detaches all followers in the same undo step.
    const int d3 = r.session().undoDepth();
    check(r.send(R"({"op":"path.clear","object":0})").ok && r.session().undoDepth() == d3 + 1, "V04d path.clear of the target is one undo step");
    bool detached = true;
    for (int o = 1; o <= 3; ++o) detached &= !r.p.getMovementPath(o).follow.active() && r.p.getMovementPath(o).points.size() == 64;
    check(detached, "V04d clearing the target converts its followers to own paths");
    check(r.session().undo().ok && r.p.getMovementPath(1).follow.target == 0 && r.p.getMovementPath(3).follow.target == 0 && r.p.getMovementPath(0).points.size() == 4,
          "V04d one undo restores the target and all links");
    // Removing a source before the rig remaps the target index.
    check(r.send(R"({"op":"source.remove","index":0})").ok, "V04d remove the target source");
    bool afterRemove = true;
    for (int o = 0; o <= 2; ++o) afterRemove &= !r.p.getMovementPath(o).follow.active() && r.p.getMovementPath(o).points.size() == 64;
    check(afterRemove, "V04d source.remove of the target: followers keep their motion as own paths");
    check(r.session().undo().ok && r.p.getMovementPath(1).follow.target == 0, "V04d undo of source.remove restores the rig");
    while (r.session().canUndo()) r.session().undo();
    r.reset("V04d end");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        if (std::getenv("VERB_ONLY_F6")) {
            testReleasePresetImpulse("Default", {}, 2);
            testReleasePresetImpulse("VSL-Grand-Hall", "presets/VSL_Grand_Hall_Audience.verbroom", 64);
            std::printf("\nF6: %d PASS, %d FAIL\n", passes, fails);
            return fails ? 1 : 0;
        }
        schemas.load(juce::File("web-ui/schema"));
        if (std::getenv("VERB_ONLY_F5")) {
            testSourceFormatImpulse();
            testSourceSumAndFreePoints();
            std::printf("\nF5: %d PASS, %d FAIL\n", passes, fails);
            return fails ? 1 : 0;
        }
        testA2MinDist();
        Rig r;
        const auto ta = std::chrono::steady_clock::now();
        testA1EditDuringLoad(r.p);
        std::printf("    (A1: %.1f s)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - ta).count());
        auto timed = [&](const char* name, void (*fn)(Rig&)) {
            const auto a = std::chrono::steady_clock::now();
            fn(r);
            std::printf("    (%s: %.1f s)\n", name, std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count());
        };
        timed("B1", testEveryCommandApplyUndoRedo);
        timed("B2", testScopedUndo);
        timed("B3", testTransactions);
        timed("B4", testCoalescingAndNoChange);
        timed("B5", testRejections);
        timed("B6", testRevisionAndIds);
        timed("B7", testHistoryBounds);
        timed("B8", testEditorLifetimeAndLoads);
        timed("B9", testSchemasAndFixtures);
        timed("C1", testC1SourceColors);
        timed("F1 translation", testWallTranslation);
        timed("F1 preset impulse", testWallPresetImpulse);
        timed("F7 clusters", testCornerRepairClusters);
        timed("F7 snap precision", testCornerSnapPrecision);
        timed("F7 repair impulse", testCornerRepairImpulse);
        timed("G1 azimuth keys", testG1AzimuthKeys);
        timed("G3 automation range", testG3AutomationRange);
        timed("G4 modulation core", testG4ModulationCommands);
        timed("MO3b Motion", testMotionCommand);
        timed("V04a path shapes & endless", testV04aPathEndlessAndShapes);
        timed("V04b path wander", testV04bPathWanderCommands);
        timed("V04a-2 live shape & spiral & figure8", testV04a2LiveShapeAndSpiralAndFigure8);
        timed("V04c zones", testV04cZoneCommands);
        timed("V04d link to path", testV04dLinkCommands);
    } catch (const std::exception& e) { std::printf("  [FAIL] unexpected exception: %s\n", e.what()); ++fails; }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\neditor_commands: %d PASS, %d FAIL, %.1f s\n", passes, fails, sec);
    return fails ? 1 : 0;
}
