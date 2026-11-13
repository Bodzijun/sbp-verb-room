// T3: the structural editing commands of the WebView contract v1 (docs/WebView_contract_v1.md, web-ui/schema/command.schema.json).
//
// Design (so that undo is SCOPED, unlike the whole-scene snapshots of the native editor):
//   * A command is a pure function `mutate(args, Model&)` that validates its arguments and edits a COPY of the editable
//     model (scene, sources, output, tail settings, source/listener parameters).
//   * The session diffs the copy against the current model into KEYED changes (one key per wall / zone / floor / ceiling,
//     per scalar group such as air or clutter, per source, per parameter ...). Only the changed keys are stored, with
//     their before and after values (materials by value, so indices may move).
//   * Applying, undoing and redoing all go through the same `applyTargets()`: it writes exactly those keys to the processor
//     through its normal setters, so later changes of other keys (air, clutter, other surfaces, layer masks, ...) stay.
// Message thread only. Nothing here touches the audio thread.
#pragma once
#include <juce_core/juce_core.h>

#include <array>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "PluginProcessor.h"
#include "SceneObjects.h"

namespace verbp {

struct Model {
    verb::Scene scene;
    std::vector<SourceCfg> sources;
    OutputCfg output;
    TailSettings tail;
    std::map<std::string, float> params;  // pose / gain / mute / solo of every source and the listener pose (APVTS raw values)
};

struct Fail {
    std::string code, message, field;  // code empty = no failure; codes of common.schema.json#ErrorCode
    bool bad() const { return !code.empty(); }
};

// ---- keyed changes ----
enum class KeyType { Surface, SurfaceList, Height, Air, Clutter, Rough, Diffraction, Source, SourceList, Output, Tail, Param };
struct Key {
    KeyType type = KeyType::Height;
    int kind = 0;   // Surface / SurfaceList: 1 wall, 2 floor, 3 ceiling, 4 floor zone, 5 ceiling zone
    int index = 0;  // Surface: index (0 for floor / ceiling); Source: index
    std::string id;  // Param: parameter id
    bool operator<(const Key& o) const {
        if (type != o.type) return type < o.type;
        if (kind != o.kind) return kind < o.kind;
        if (index != o.index) return index < o.index;
        return id < o.id;
    }
    bool operator==(const Key& o) const { return type == o.type && kind == o.kind && index == o.index && id == o.id; }
    bool isList() const { return type == KeyType::SurfaceList || type == KeyType::SourceList; }
    std::string path() const;  // JSON-pointer-like path in the snapshot document, for `changed`
};

struct SurfaceSnap {  // one wall / zone / floor / ceiling with its material BY VALUE
    int kind = 1;
    verb::Wall wall{0, 0, 0, 0, 0};
    verb::Zone zone;
    verb::Material mat;
    bool hasMat = false;  // false: open ceiling / open zone
    verb::SurfaceTag tag;
};
struct HeightSnap { double h = 3; };
struct AirSnap { double tempC = 20, humidity = 50; };
struct RoughSnap { double roughness = 0; int seed = 1; };
struct DiffSnap { bool on = true; };
using Snap = std::variant<SurfaceSnap, std::vector<SurfaceSnap>, HeightSnap, AirSnap, verb::ClutterCfg, RoughSnap, DiffSnap, SourceCfg,
                          std::vector<SourceCfg>, OutputCfg, TailSettings, float>;

struct KeyChange {
    Key key;
    Snap before, after;
    bool acoustic = true;  // false: only editor metadata (names, lock, hide, group) changed: no engine update needed
};

struct Target {  // what applyTargets() writes
    Key key;
    const Snap* snap = nullptr;
    bool acoustic = true;
};

Model readModel(VerbProcessor&);
// Exactly what VerbProcessor::editScene does to a candidate before it is accepted.
void normalizeScene(verb::Scene&);
std::vector<KeyChange> diffModels(const Model& before, const Model& after);
std::optional<Snap> captureKey(const Model&, const Key&);
bool snapsEqual(const Snap&, const Snap&);
size_t snapBytes(const Snap&);
// Writes the targets to the processor (scene keys in ONE editScene / editSceneMetadata call, then sources, output, tail and
// parameters). `conflicts` counts targets whose index no longer exists (skipped).
Fail applyTargets(VerbProcessor&, const std::vector<Target>&, int* conflicts = nullptr);
std::vector<std::string> changedPaths(const std::vector<KeyChange>&);
bool indicesMoved(const std::vector<KeyChange>&);  // a list changed length (surfaces / sources): indices are unstable

struct Ctx {
    VerbProcessor& proc;
    const Model& start;  // the model a transaction started from (== the current model for a plain command)
};
struct Descriptor {
    const char* op;
    bool undoable;  // false: applied and revisioned, but not on the undo stack (and why: see SceneCommands.cpp kDescriptors)
    std::string (*label)(const juce::var&);        // human-readable label for the undo menu
    std::string (*coalesceKey)(const juce::var&);  // empty = never merged with a previous step
    Fail (*mutate)(const juce::var&, Model&, const Ctx&);
};
const Descriptor* findDescriptor(const std::string& op);
// Ops the contract defines that are not structural edits of the room (served by other services): recognised, not executed here.
bool isOtherServiceOp(const std::string& op);

// After `mutate`: sanitise like editScene does and refuse a candidate the sanitiser would silently change.
Fail normalizeAndValidate(Model& candidate, const Model& before);

}  // namespace verbp
