// T3: the editor session owned by the processor. It executes the structural commands of the WebView contract v1
// (JSON in, ack / error / events out), keeps the undo history, the open transaction, the idempotency cache and the
// editor view state. It survives closing and reopening the editor window. Message thread only (except
// notifyDocumentReplaced()); it never touches the audio thread: every change goes through the processor's normal setters.
#pragma once
#include <juce_core/juce_core.h>

#include <atomic>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "SceneCommands.h"

namespace verbp {

struct CommandResult {
    bool ok = false;        // accepted or noChange
    std::string status;     // "accepted" | "noChange" (empty on error)
    std::string code, message, field;  // error: code of common.schema.json#ErrorCode
    bool resync = false;    // error: the client must discard its state and wait for a snapshot
    bool duplicate = false; // this requestId was executed before: the stored result is returned, nothing is applied again
    int externalConflicts = 0;  // undo / redo: keys whose current value was not the one the step had produced (changed elsewhere meanwhile)
    uint64_t revision = 0;  // revision after the command (the current one on error)
    std::vector<std::string> changed;  // paths of the changed parts of the document
    std::vector<juce::var> messages;   // envelopes to send, in order: [0] ack or error, then events (history.changed, resync.required)
};

class EditorSession {
public:
    // Undo history limits. 60 steps = the native editor's depth and `undoDepth` maximum of event.schema.json.
    // Memory: one step stores only the changed keys (before + after): a wall edit ~1 KB, a drag of 10 surfaces ~10 KB, a
    // whole 128-wall list ~110 KB. The byte cap bounds the worst case (60 whole-list steps ~6.6 MB) to 8 MiB.
    static constexpr int kMaxHistory = 60;
    static constexpr size_t kMaxHistoryBytes = 8u << 20;
    static constexpr double kCoalesceMs = 750;  // identical edits (same op, same target) closer than this share one step
    static constexpr size_t kMaxViewStateBytes = 4096;
    static constexpr int kMaxRecentRequests = 256;

    explicit EditorSession(VerbProcessor&);
    ~EditorSession();

    // JSON envelope (kind "command") in; ack / error envelope plus events out (CommandResult::messages).
    CommandResult execute(const juce::var& envelope);
    CommandResult executeJson(const juce::String& json);
    // The same without the envelope. `baseRevision` empty = not checked.
    CommandResult run(const juce::var& payload, const juce::String& requestId = {}, const juce::String& baseRevision = {}, const juce::String& transactionId = {});

    CommandResult undo();
    CommandResult redo();
    bool canUndo();
    bool canRedo();
    std::string undoLabel();  // label of the step undo would revert (for the menu); empty if none
    std::string redoLabel();
    int undoDepth();
    int redoDepth();
    size_t historyBytes();  // estimate of the memory held by the history
    uint64_t revision();    // = the processor's structural revision (bumped by every accepted change, also by the native editor)
    // `history.redo` over JSON is `unsupported` (contract capability history.redo = false) until this is switched on.
    void setRedoEnabled(bool on) { redoEnabled = on; }
    juce::var viewState();       // opaque editor view state (selection, camera ...) sent by the UI; not saved with the project
    bool transactionOpen();
    void abortTransaction();     // reverts the open transaction (the client resynchronises)
    // Any thread: a room / preset / project state was requested. The history, the open transaction and the view state belong to
    // the old document and are dropped the next time the session is used on the message thread.
    void notifyDocumentReplaced() { ++replacedEpoch; }
    void setClock(std::function<double()> nowMs) { clock = std::move(nowMs); }
    void setSessionId(const juce::String& s) {
        if (s == sessionId) return;
        abortTransaction();
        recent.clear(); // request ids are scoped to the page session, not the processor lifetime
        sessionId = s;
    }

private:
    struct HistoryEntry {
        std::string label, ckey;
        double timeMs = 0;
        std::vector<KeyChange> changes;
        size_t bytes = 0;
    };
    struct TxCmd {
        std::string ckey;
        const Descriptor* d = nullptr;
        juce::var payload;
    };
    struct Tx {
        std::string id, label;
        Model start;
        std::vector<TxCmd> cmds;
        std::vector<KeyChange> applied;
        uint64_t beginRevision = 0, lastRevision = 0;
    };

    void syncEpoch();
    double now() const { return clock ? clock() : (double)juce::Time::getMillisecondCounterHiRes(); }
    CommandResult dispatch(const juce::var& payload, const juce::String& requestId, const juce::String& baseRevision, const juce::String& transactionId, bool envelope);
    CommandResult mutate(const Descriptor& d, const juce::var& payload, const juce::String& requestId, const juce::String& baseRevision, const juce::String& transactionId, bool envelope);
    CommandResult txBegin(const juce::var& payload, const juce::String& requestId, const juce::String& transactionId);
    CommandResult txEnd(bool commit, const juce::String& requestId, const juce::String& transactionId);
    CommandResult setViewStateOp(const juce::var& payload, const juce::String& requestId);
    CommandResult layersOp(const juce::var& payload, const juce::String& requestId, const juce::String& baseRevision, bool envelope);
    CommandResult stepHistory(bool undoDir, const juce::String& requestId);
    bool applyChanges(const std::vector<KeyChange>& changes, bool useAfter, Fail& fail, int* conflicts = nullptr);
    void ensureRevisionMoved(uint64_t before);
    void pushHistory(const std::string& label, const std::string& ckey, std::vector<KeyChange> changes);
    void trimHistory();
    void commitOpenTransaction();
    void revertApplied(Tx& t);
    Fail checkBase(const juce::String& baseRevision, bool envelope, bool requiredForEnvelope);
    // result builders
    CommandResult ok(const juce::String& requestId, const char* status, std::vector<std::string> changed, bool historyEvent, const char* resyncReason = nullptr);
    CommandResult error(const juce::String& requestId, const Fail& f, bool resync = false);
    juce::var envelope(const char* kind, const juce::String& requestId, juce::var payload) const;
    juce::var historyEvent();
    juce::var resyncEvent(const char* reason);
    CommandResult remember(const juce::String& requestId, CommandResult r);

    VerbProcessor& proc;
    juce::String sessionId{"editor-session"};
    std::atomic<uint64_t> replacedEpoch{0};
    uint64_t seenEpoch = 0;
    bool redoEnabled = false;
    std::function<double()> clock;
    std::deque<HistoryEntry> undoStack, redoStack;
    std::optional<Tx> tx;
    std::deque<std::pair<std::string, CommandResult>> recent;
    juce::var viewStateVar;
    std::string viewStateText;
    friend struct EditorSessionAccess;
};

}  // namespace verbp
