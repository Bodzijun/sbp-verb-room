#include "EditorSession.h"

#include <algorithm>
#include <set>

namespace verbp {

namespace {
juce::var makeObject(std::initializer_list<std::pair<const char*, juce::var>> props) {
    auto* o = new juce::DynamicObject();
    for (const auto& p : props) o->setProperty(p.first, p.second);
    return juce::var(o);
}
juce::String u64(uint64_t v) { return juce::String(std::to_string(v)); }
bool parseU64(const juce::String& s, uint64_t& out) {
    if (s.isEmpty() || s.length() > 20 || !s.containsOnly("0123456789")) return false;
    out = (uint64_t)s.getLargeIntValue();
    return true;
}
bool sameKeys(const std::vector<KeyChange>& a, const std::vector<KeyChange>& b) {
    if (a.size() != b.size()) return false;
    std::set<Key> sa, sb;
    for (const auto& x : a) sa.insert(x.key);
    for (const auto& x : b) sb.insert(x.key);
    return sa == sb;
}
size_t entryBytes(const std::vector<KeyChange>& c) {
    size_t n = 256;
    for (const auto& x : c) n += snapBytes(x.before) + snapBytes(x.after) + sizeof(Key) + x.key.id.capacity();
    return n;
}
}  // namespace

EditorSession::EditorSession(VerbProcessor& p) : proc(p) {}
EditorSession::~EditorSession() = default;

// ============================================================================================ result builders
juce::var EditorSession::envelope(const char* kind, const juce::String& requestId, juce::var payload) const {
    auto* o = new juce::DynamicObject();
    o->setProperty("schemaVersion", 1);
    o->setProperty("sessionId", sessionId);
    o->setProperty("kind", kind);
    if (requestId.isNotEmpty()) o->setProperty("requestId", requestId);
    o->setProperty("payload", payload);
    return juce::var(o);
}

juce::var EditorSession::historyEvent() {
    return makeObject({{"event", "history.changed"}, {"canUndo", canUndo()}, {"undoDepth", undoDepth()}, {"canRedo", canRedo()}, {"undoLabel", juce::String(undoLabel()).substring(0, 60)}, {"redoLabel", juce::String(redoLabel()).substring(0, 60)}});
}
juce::var EditorSession::resyncEvent(const char* reason) {
    return makeObject({{"event", "resync.required"}, {"reason", reason}, {"currentRevision", u64(proc.getRevision())}});
}

CommandResult EditorSession::ok(const juce::String& requestId, const char* status, std::vector<std::string> changed, bool historyEv, const char* resyncReason) {
    CommandResult r;
    r.ok = true;
    r.status = status;
    r.revision = proc.getRevision();
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    r.changed = changed;
    juce::Array<juce::var> paths;
    for (const auto& c : changed) paths.add(juce::String(c));
    auto* po = new juce::DynamicObject();
    po->setProperty("requestId", requestId);
    po->setProperty("revision", u64(r.revision));
    po->setProperty("status", juce::String(status));
    if (!paths.isEmpty()) po->setProperty("changed", paths);
    r.messages.push_back(envelope("ack", requestId, juce::var(po)));
    if (historyEv) r.messages.push_back(envelope("event", {}, historyEvent()));
    if (resyncReason) r.messages.push_back(envelope("event", {}, resyncEvent(resyncReason)));
    return r;
}

CommandResult EditorSession::error(const juce::String& requestId, const Fail& f, bool resync) {
    CommandResult r;
    r.ok = false;
    r.code = f.code; r.message = f.message; r.field = f.field; r.resync = resync;
    r.revision = proc.getRevision();
    auto* po = new juce::DynamicObject();
    if (requestId.isNotEmpty()) po->setProperty("requestId", requestId);
    po->setProperty("code", juce::String(f.code));
    po->setProperty("message", juce::String(f.message));
    po->setProperty("currentRevision", u64(r.revision));
    if (!f.field.empty()) po->setProperty("field", juce::String(f.field));
    if (resync) po->setProperty("resync", true);
    r.messages.push_back(envelope("error", requestId, juce::var(po)));
    if (resync) r.messages.push_back(envelope("event", {}, resyncEvent(f.code == "staleRevision" ? "staleRevision" : "structuralChange")));
    return r;
}

CommandResult EditorSession::remember(const juce::String& requestId, CommandResult r) {
    if (requestId.isNotEmpty() && r.ok) {  // only successes: a rejected command may legitimately be retried with the same id
        recent.push_back({requestId.toStdString(), r});
        while ((int)recent.size() > kMaxRecentRequests) recent.pop_front();
    }
    return r;
}

// ============================================================================================ state accessors
void EditorSession::syncEpoch() {
    const uint64_t e = replacedEpoch.load();
    if (e == seenEpoch) return;
    seenEpoch = e;  // a room / preset / project state replaced the document: the old history cannot apply to it
    undoStack.clear(); redoStack.clear(); tx.reset();
    viewStateVar = juce::var(); viewStateText.clear();
}

bool EditorSession::canUndo() { syncEpoch(); return !undoStack.empty(); }
bool EditorSession::canRedo() { syncEpoch(); return !redoStack.empty(); }
std::string EditorSession::undoLabel() { syncEpoch(); return undoStack.empty() ? std::string() : undoStack.back().label; }
std::string EditorSession::redoLabel() { syncEpoch(); return redoStack.empty() ? std::string() : redoStack.back().label; }
int EditorSession::undoDepth() { syncEpoch(); return (int)undoStack.size(); }
int EditorSession::redoDepth() { syncEpoch(); return (int)redoStack.size(); }
size_t EditorSession::historyBytes() {
    syncEpoch();
    size_t n = 0;
    for (const auto& e : undoStack) n += e.bytes;
    for (const auto& e : redoStack) n += e.bytes;
    return n;
}
uint64_t EditorSession::revision() { syncEpoch(); return proc.getRevision(); }
juce::var EditorSession::viewState() { syncEpoch(); return viewStateVar; }
bool EditorSession::transactionOpen() { syncEpoch(); return tx.has_value(); }

void EditorSession::ensureRevisionMoved(uint64_t before) {
    if (proc.getRevision() == before) proc.bumpRevision();  // a change made only through parameters
}

bool EditorSession::applyChanges(const std::vector<KeyChange>& changes, bool useAfter, Fail& fail, int*) {
    std::vector<Target> targets;
    targets.reserve(changes.size());
    for (const auto& c : changes) targets.push_back({c.key, useAfter ? &c.after : &c.before, c.acoustic});
    fail = applyTargets(proc, targets);
    return !fail.bad();
}

// ============================================================================================ history
void EditorSession::trimHistory() {
    size_t total = 0;
    for (const auto& e : undoStack) total += e.bytes;
    while (!undoStack.empty() && ((int)undoStack.size() > kMaxHistory || (total > kMaxHistoryBytes && undoStack.size() > 1))) {
        total -= undoStack.front().bytes;
        undoStack.pop_front();
    }
}

void EditorSession::pushHistory(const std::string& label, const std::string& ckey, std::vector<KeyChange> changes) {
    redoStack.clear();
    const double t = now();
    if (!undoStack.empty() && !ckey.empty() && undoStack.back().ckey == ckey && t - undoStack.back().timeMs <= kCoalesceMs
        && !indicesMoved(undoStack.back().changes) && !indicesMoved(changes)) {
        HistoryEntry& e = undoStack.back();  // the same edit repeated (a slider moved): one undo step from the first before to the last after
        for (auto& c : changes) {
            auto it = std::find_if(e.changes.begin(), e.changes.end(), [&](const KeyChange& x) { return x.key == c.key; });
            if (it == e.changes.end()) e.changes.push_back(std::move(c));
            else { it->after = std::move(c.after); it->acoustic = it->acoustic || c.acoustic; }
        }
        e.changes.erase(std::remove_if(e.changes.begin(), e.changes.end(), [](const KeyChange& x) { return snapsEqual(x.before, x.after); }), e.changes.end());
        e.timeMs = t;
        e.bytes = entryBytes(e.changes);
        if (e.changes.empty()) undoStack.pop_back();  // moved back to where it began: nothing left to undo
        return;
    }
    HistoryEntry e;
    e.label = label; e.ckey = ckey; e.timeMs = t;
    e.bytes = entryBytes(changes) + label.capacity() + ckey.capacity();
    e.changes = std::move(changes);
    undoStack.push_back(std::move(e));
    trimHistory();
}

CommandResult EditorSession::stepHistory(bool undoDir, const juce::String& requestId) {
    if (proc.isRoomLoadPending()) return error(requestId, {"notReady", "A room is loading; wait for it before editing.", ""});
    if (tx) commitOpenTransaction();
    auto& from = undoDir ? undoStack : redoStack;
    auto& to = undoDir ? redoStack : undoStack;
    if (from.empty()) return ok(requestId, "noChange", {}, false);
    HistoryEntry entry = from.back();
    // changed elsewhere since this step ran? (the native editor, another command path) -- reported, still restored
    int conflicts = 0;
    {
        const Model cur = readModel(proc);
        for (const auto& c : entry.changes) {
            auto s = captureKey(cur, c.key);
            if (!s || !snapsEqual(*s, undoDir ? c.after : c.before)) ++conflicts;
        }
    }
    const uint64_t revBefore = proc.getRevision();
    Fail f;
    if (!applyChanges(entry.changes, !undoDir, f)) return error(requestId, f);
    ensureRevisionMoved(revBefore);
    from.pop_back();
    to.push_back(entry);
    if (!undoDir) trimHistory();
    auto r = ok(requestId, "accepted", changedPaths(entry.changes), true, undoDir ? "undo" : "structuralChange");
    r.externalConflicts = conflicts;
    return r;
}

CommandResult EditorSession::undo() { syncEpoch(); return stepHistory(true, {}); }
CommandResult EditorSession::redo() { syncEpoch(); return stepHistory(false, {}); }

// ============================================================================================ transactions
void EditorSession::revertApplied(Tx& t) {
    if (t.applied.empty()) return;
    const uint64_t revBefore = proc.getRevision();
    Fail f;
    applyChanges(t.applied, false, f);
    ensureRevisionMoved(revBefore);
    t.applied.clear();
    t.lastRevision = proc.getRevision();
}

void EditorSession::commitOpenTransaction() {
    if (!tx) return;
    if (!tx->applied.empty()) {
        std::string label = tx->label;
        if (label.empty() && !tx->cmds.empty()) label = tx->cmds.front().d->label(tx->cmds.front().payload);
        pushHistory(label, {}, tx->applied);
    }
    tx.reset();
}

void EditorSession::abortTransaction() {
    syncEpoch();
    if (!tx) return;
    revertApplied(*tx);
    tx.reset();
}

CommandResult EditorSession::txBegin(const juce::var& payload, const juce::String& requestId, const juce::String& transactionId) {
    if (transactionId.isEmpty()) return error(requestId, {"invalid", "transaction.begin needs a transactionId", "transactionId"});
    if (proc.isRoomLoadPending()) return error(requestId, {"notReady", "A room is loading; wait for it before editing.", ""});
    std::string label;
    if (payload.hasProperty("label")) {
        const juce::var l = payload.getProperty("label", {});
        if (!l.isString() || l.toString().length() > 60) return error(requestId, {"invalid", "label must be a string of at most 60 characters", "label"});
        label = l.toString().toStdString();
    }
    if (tx && tx->id == transactionId.toStdString()) return ok(requestId, "noChange", {}, false);  // re-sent
    if (tx) commitOpenTransaction();                                                                 // an interrupted gesture is kept
    Tx t;
    t.id = transactionId.toStdString();
    t.label = label;
    t.start = readModel(proc);
    t.beginRevision = t.lastRevision = proc.getRevision();
    tx = std::move(t);
    return ok(requestId, "accepted", {}, false);
}

CommandResult EditorSession::txEnd(bool commit, const juce::String& requestId, const juce::String& transactionId) {
    if (!tx || tx->id != transactionId.toStdString()) return error(requestId, {"invalid", "no such open transaction", "transactionId"});
    const bool any = !tx->applied.empty();
    const auto paths = changedPaths(tx->applied);
    const bool moved = indicesMoved(tx->applied);
    if (commit) {
        commitOpenTransaction();
        return ok(requestId, any ? "accepted" : "noChange", paths, any, moved ? "structuralChange" : nullptr);
    }
    revertApplied(*tx);
    tx.reset();
    return ok(requestId, any ? "accepted" : "noChange", paths, false, moved ? "structuralChange" : nullptr);
}

// ============================================================================================ mutations
Fail EditorSession::checkBase(const juce::String& baseRevision, bool envelope, bool requiredForEnvelope) {
    if (baseRevision.isEmpty()) {
        if (envelope && requiredForEnvelope) return {"invalid", "baseRevision is required for this command", "baseRevision"};
        return {};
    }
    uint64_t b = 0;
    if (!parseU64(baseRevision, b)) return {"invalid", "baseRevision must be a decimal uint64 string", "baseRevision"};
    if (b != proc.getRevision())
        return {"staleRevision", "Command was built on revision " + std::to_string(b) + "; current revision is " + std::to_string(proc.getRevision()) + ".", "baseRevision"};
    return {};
}

CommandResult EditorSession::mutate(const Descriptor& d, const juce::var& payload, const juce::String& requestId, const juce::String& baseRevision,
                                    const juce::String& transactionId, bool envelope) {
    if (proc.isRoomLoadPending()) return error(requestId, {"notReady", "A room is loading; wait for it before editing.", ""});
    const bool inTx = transactionId.isNotEmpty();
    if (tx && (!inTx || tx->id != transactionId.toStdString())) commitOpenTransaction();
    if (inTx && !(tx && tx->id == transactionId.toStdString())) return error(requestId, {"invalid", "no such open transaction", "transactionId"});

    // ---- base revision
    if (inTx) {
        // A gesture may pipeline its updates, so the base is only required to be a revision the transaction knows (>= its begin);
        // any other change of the structure while the gesture runs invalidates it.
        if (baseRevision.isEmpty() && envelope) return error(requestId, {"invalid", "baseRevision is required for this command", "baseRevision"});
        uint64_t b = 0;
        const bool parsed = baseRevision.isEmpty() || parseU64(baseRevision, b);
        if (!parsed) return error(requestId, {"invalid", "baseRevision must be a decimal uint64 string", "baseRevision"});
        const bool external = proc.getRevision() != tx->lastRevision;
        if (external || (!baseRevision.isEmpty() && (b < tx->beginRevision || b > proc.getRevision()))) {
            const Fail f{"staleRevision", "The structure changed during the transaction; current revision is " + std::to_string(proc.getRevision()) + ".", "baseRevision"};
            abortTransaction();
            return error(requestId, f, true);
        }
    } else if (auto f = checkBase(baseRevision, envelope, true); f.bad()) {
        return error(requestId, f, f.code == "staleRevision");
    }

    const std::string op = d.op;
    std::string ck = d.coalesceKey ? d.coalesceKey(payload) : std::string();
    if (!ck.empty()) ck = op + "|" + ck;

    if (!inTx) {
        const uint64_t revBefore = proc.getRevision();
        const Model before = readModel(proc);
        Model cand = before;
        Fail f = d.mutate(payload, cand, Ctx{proc, before});
        if (!f.bad()) f = normalizeAndValidate(cand, before);
        if (f.bad()) return error(requestId, f);
        auto changes = diffModels(before, cand);
        if (changes.empty()) return ok(requestId, "noChange", {}, false);
        Fail af;
        if (!applyChanges(changes, true, af)) {
            Fail ignore;
            applyChanges(changes, false, ignore);  // best effort: put back what was already written
            return error(requestId, af);
        }
        ensureRevisionMoved(revBefore);
        const auto paths = changedPaths(changes);
        const bool moved = indicesMoved(changes);
        if (d.undoable) pushHistory(d.label(payload), ck, std::move(changes));
        return ok(requestId, "accepted", paths, d.undoable, moved ? "structuralChange" : nullptr);
    }

    // ---- inside a transaction: replay all commands of the gesture from its start (last value of the same edit wins; deltas are
    //      relative to the start, never accumulated), then move the processor from the applied state to the new one
    Tx& t = *tx;
    std::vector<TxCmd> cmds = t.cmds;
    TxCmd nc;
    nc.d = &d; nc.payload = payload; nc.ckey = ck;
    auto same = !ck.empty() ? std::find_if(cmds.begin(), cmds.end(), [&](const TxCmd& c) { return c.ckey == ck; }) : cmds.end();
    if (same != cmds.end()) *same = nc; else cmds.push_back(nc);
    Model cand = t.start;
    for (const auto& c : cmds) {
        Fail f = c.d->mutate(c.payload, cand, Ctx{proc, t.start});
        if (f.bad()) return error(requestId, f);  // the transaction keeps its previous state
    }
    if (auto f = normalizeAndValidate(cand, t.start); f.bad()) return error(requestId, f);
    auto changes = diffModels(t.start, cand);
    const bool hadApplied = !t.applied.empty();
    const uint64_t revBefore = proc.getRevision();
    Fail af;
    bool good;
    if (sameKeys(changes, t.applied) && !indicesMoved(changes)) {
        good = changes.empty() || applyChanges(changes, true, af);
    } else {
        Fail rf;
        if (hadApplied) applyChanges(t.applied, false, rf);          // back to the start of the gesture ...
        good = changes.empty() || applyChanges(changes, true, af);   // ... then to the new value
    }
    if (!good) return error(requestId, af);
    if (hadApplied || !changes.empty()) ensureRevisionMoved(revBefore);
    t.cmds = cmds;
    t.applied = std::move(changes);
    t.lastRevision = proc.getRevision();
    return ok(requestId, (hadApplied || !t.applied.empty()) ? "accepted" : "noChange", changedPaths(t.applied), false);
}

// ============================================================================================ other session ops
CommandResult EditorSession::setViewStateOp(const juce::var& payload, const juce::String& requestId) {
    const juce::var vs = payload.getProperty("viewState", {});
    if (!vs.getDynamicObject()) return error(requestId, {"invalid", "viewState must be an object", "viewState"});
    const juce::String text = juce::JSON::toString(vs, true);
    if (text.getNumBytesAsUTF8() > kMaxViewStateBytes) return error(requestId, {"invalid", "viewState is larger than 4096 bytes", "viewState"});
    const bool same = text.toStdString() == viewStateText;
    viewStateVar = vs;
    viewStateText = text.toStdString();
    return ok(requestId, same ? "noChange" : "accepted", {}, false);  // session memory only: no revision, no undo
}

CommandResult EditorSession::layersOp(const juce::var& payload, const juce::String& requestId, const juce::String& baseRevision, bool envelope) {
    const juce::var l = payload.getProperty("layer", {}), m = payload.getProperty("mode", {});
    if (!(l.isInt() || l.isInt64()) || (int)l < 0 || (int)l > 3) return error(requestId, {"invalid", "layer must be an integer 0..3", "layer"});
    if (!(m.isInt() || m.isInt64()) || (int)m < 0 || (int)m > 2) return error(requestId, {"invalid", "mode must be 0 (shown), 1 (locked) or 2 (hidden)", "mode"});
    if (proc.isRoomLoadPending()) return error(requestId, {"notReady", "A room is loading; wait for it before editing.", ""});
    if (auto f = checkBase(baseRevision, envelope, true); f.bad()) return error(requestId, f, f.code == "staleRevision");
    if (proc.getLayerModes()[(size_t)(int)l] == (int)m) return ok(requestId, "noChange", {}, false);
    if (!proc.editSceneMetadata([&](verb::Scene& s) { s.layerModes[(size_t)(int)l] = (int)m; }))
        return error(requestId, {"notReady", "The scene edit was rejected.", ""});
    // Layer masks are a view setting (like Lock pan): stored with the scene, bump the revision, but never on the undo stack.
    return ok(requestId, "accepted", {"document.editorFlags.layerModes"}, false);
}

// ============================================================================================ entry points
CommandResult EditorSession::dispatch(const juce::var& payload, const juce::String& requestId, const juce::String& baseRevision,
                                      const juce::String& transactionId, bool envelope) {
    syncEpoch();
    if (requestId.isNotEmpty()) {
        const std::string id = requestId.toStdString();
        for (const auto& kv : recent)
            if (kv.first == id) { CommandResult r = kv.second; r.duplicate = true; return r; }  // a re-sent command is not applied twice
    }
    const std::string op = payload.getProperty("op", {}).toString().toStdString();
    if (!payload.getDynamicObject() || op.empty()) return error(requestId, {"invalid", "the payload needs an op", "op"});
    CommandResult r;
    if (op == "transaction.begin") r = txBegin(payload, requestId, transactionId);
    else if (op == "transaction.commit") r = txEnd(true, requestId, transactionId);
    else if (op == "transaction.cancel") r = txEnd(false, requestId, transactionId);
    else if (op == "history.undo" || op == "history.redo") {
        const bool isUndo = op == "history.undo";
        if (!isUndo && !redoEnabled) return error(requestId, {"unsupported", "history.redo is not available in contract v1 (capability history.redo = false).", "op"});
        if (auto f = checkBase(baseRevision, envelope, true); f.bad()) return error(requestId, f, f.code == "staleRevision");
        r = stepHistory(isUndo, requestId);
    }
    else if (op == "session.setViewState") r = setViewStateOp(payload, requestId);
    else if (op == "layers.set") r = layersOp(payload, requestId, baseRevision, envelope);
    else if (const Descriptor* d = findDescriptor(op)) r = mutate(*d, payload, requestId, baseRevision, transactionId, envelope);
    else if (isOtherServiceOp(op)) return error(requestId, {"unsupported", "this command is served by another service, not by the editing session", "op"});
    else return error(requestId, {"unsupported", "unknown command: " + op, "op"});
    return remember(requestId, r);
}

CommandResult EditorSession::run(const juce::var& payload, const juce::String& requestId, const juce::String& baseRevision, const juce::String& transactionId) {
    return dispatch(payload, requestId, baseRevision, transactionId, false);
}

CommandResult EditorSession::execute(const juce::var& env) {
    syncEpoch();
    const juce::String requestId = env.getProperty("requestId", {}).toString();
    if (!env.getDynamicObject()) return error(requestId, {"invalid", "the envelope must be an object", ""});
    if (env.hasProperty("sessionId") && env.getProperty("sessionId", {}).isString()) setSessionId(env.getProperty("sessionId", {}).toString());
    const juce::var sv = env.getProperty("schemaVersion", {});
    if (!(sv.isInt() || sv.isInt64()) || (int)sv != 1) return error(requestId, {"unsupported", "unsupported schemaVersion", "schemaVersion"});
    if (env.getProperty("kind", {}).toString() != "command") return error(requestId, {"invalid", "only kind \"command\" is executed here", "kind"});
    if (requestId.isEmpty()) return error(requestId, {"invalid", "a command needs a requestId", "requestId"});
    const juce::var payload = env.getProperty("payload", {});
    return dispatch(payload, requestId, env.getProperty("baseRevision", {}).toString(), env.getProperty("transactionId", {}).toString(), true);
}

CommandResult EditorSession::executeJson(const juce::String& json) {
    juce::var v;
    if (juce::JSON::parse(json, v).failed()) return error({}, {"invalid", "the message is not valid JSON", ""});
    return execute(v);
}

}  // namespace verbp
