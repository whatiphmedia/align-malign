// Auto Malign Studio: undo / redo by snapshots of the session state (settings are small; audio is re-rendered from them).
#pragma once
#include <deque>
#include <string>
#include <utility>

namespace am::studio {

template <class State>
class UndoStack {
public:
    explicit UndoStack(size_t depth = 200) : depth_(depth) {}

    // Call BEFORE applying a change: remembers the current state under the change's label.
    // Edits with the same mergeKey in a row (dragging one knob) become one step.
    void push(const State& before, const std::string& label, const std::string& mergeKey = {}) {
        if (!mergeKey.empty() && !undo_.empty() && undo_.back().key == mergeKey && !redo_.size()) return;
        undo_.push_back({ before, label, mergeKey });
        while (undo_.size() > depth_) undo_.pop_front();
        redo_.clear();
    }
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string undoLabel() const { return undo_.empty() ? "" : undo_.back().label; }
    std::string redoLabel() const { return redo_.empty() ? "" : redo_.back().label; }

    // Returns true and replaces `current` with the previous state.
    bool undo(State& current) {
        if (undo_.empty()) return false;
        Entry e = std::move(undo_.back()); undo_.pop_back();
        redo_.push_back({ current, e.label, {} }); current = std::move(e.state); return true;
    }
    bool redo(State& current) {
        if (redo_.empty()) return false;
        Entry e = std::move(redo_.back()); redo_.pop_back();
        undo_.push_back({ current, e.label, {} }); current = std::move(e.state); return true;
    }
    void breakMerge() { if (!undo_.empty()) undo_.back().key.clear(); }   // e.g. on mouse-up
    void clear() { undo_.clear(); redo_.clear(); }
    size_t size() const { return undo_.size(); }

private:
    struct Entry { State state; std::string label, key; };
    size_t depth_;
    std::deque<Entry> undo_, redo_;
};

} // namespace am::studio
