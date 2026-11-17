#pragma once
// Undo/redo: snapshot-based primitive commands + macros. Every user action = one pushed command.
#include <memory>
#include <string>
#include <vector>

#include "project/Model.h"

namespace kinetic {

class Command {
public:
    virtual ~Command() = default;
    virtual void apply(Project& p) = 0;
    virtual void revert(Project& p) = 0;
    // Try to absorb `next` (same gesture). Return true if merged; `this` then represents both.
    virtual bool mergeWith(const Command& /*next*/) { return false; }
    std::string label;
    std::string mergeKey;  // non-empty => consecutive commands with the same key may merge
};
using CommandPtr = std::unique_ptr<Command>;

struct CommandError : std::runtime_error { using std::runtime_error::runtime_error; };

class InsertClipCmd : public Command {
public:
    InsertClipCmd(Id trackId, Clip clip) : trackId_(trackId), clip_(std::move(clip)) { label = "Insert Clip"; }
    void apply(Project& p) override;
    void revert(Project& p) override;
private:
    Id trackId_;
    Clip clip_;
};

class RemoveClipCmd : public Command {
public:
    RemoveClipCmd(Id trackId, Clip snapshot) : trackId_(trackId), clip_(std::move(snapshot)) { label = "Remove Clip"; }
    void apply(Project& p) override;
    void revert(Project& p) override;
private:
    Id trackId_;
    Clip clip_;
};

// Replace a clip (possibly moving it to another track) with full before/after snapshots.
class ReplaceClipCmd : public Command {
public:
    ReplaceClipCmd(Id beforeTrack, Clip before, Id afterTrack, Clip after)
        : beforeTrack_(beforeTrack), afterTrack_(afterTrack), before_(std::move(before)), after_(std::move(after)) { label = "Edit Clip"; }
    void apply(Project& p) override;
    void revert(Project& p) override;
    bool mergeWith(const Command& next) override;
    const Clip& after() const { return after_; }
private:
    Id beforeTrack_, afterTrack_;
    Clip before_, after_;
};

class AddMarkerCmd : public Command {
public:
    AddMarkerCmd(Id compId, Marker m) : compId_(compId), marker_(std::move(m)) { label = "Add Marker"; }
    void apply(Project& p) override;
    void revert(Project& p) override;
private:
    Id compId_;
    Marker marker_;
};

class RemoveMarkerCmd : public Command {
public:
    RemoveMarkerCmd(Id compId, Marker m) : compId_(compId), marker_(std::move(m)) { label = "Remove Marker"; }
    void apply(Project& p) override;
    void revert(Project& p) override;
private:
    Id compId_;
    Marker marker_;
};

class AddTrackCmd : public Command {
public:
    AddTrackCmd(Id compId, Track t, size_t index) : compId_(compId), track_(std::move(t)), index_(index) { label = "Add Track"; }
    void apply(Project& p) override;
    void revert(Project& p) override;
private:
    Id compId_;
    Track track_;
    size_t index_;
};

// Generic snapshot replace of a track's flags/name (not its clips).
class SetTrackStateCmd : public Command {
public:
    SetTrackStateCmd(const Track& before, const Track& after) : before_(header(before)), after_(header(after)) { label = "Track State"; }
    void apply(Project& p) override { set(p, after_); }
    void revert(Project& p) override { set(p, before_); }
private:
    static Track header(const Track& t) { Track h = t; h.clips.clear(); return h; }
    static void set(Project& p, const Track& h);
    Track before_, after_;
};

class AddCompositionCmd : public Command {
public:
    explicit AddCompositionCmd(Composition c) : comp_(std::move(c)) { label = "New Composition"; }
    void apply(Project& p) override;
    void revert(Project& p) override;
private:
    Composition comp_;
};

class AddMediaCmd : public Command {
public:
    explicit AddMediaCmd(MediaRef m) : media_(std::move(m)) { label = "Import Media"; }
    void apply(Project& p) override;
    void revert(Project& p) override;
private:
    MediaRef media_;
};

// Id allocation is part of history so undo restores the project exactly.
class SetNextIdCmd : public Command {
public:
    SetNextIdCmd(Id before, Id after) : before_(before), after_(after) {}
    void apply(Project& p) override { p.nextId = after_; }
    void revert(Project& p) override { p.nextId = before_; }
private:
    Id before_, after_;
};

class MacroCommand : public Command {
public:
    explicit MacroCommand(std::string lbl) { label = std::move(lbl); }
    void add(CommandPtr c) { children_.push_back(std::move(c)); }
    bool empty() const { return children_.empty(); }
    size_t size() const { return children_.size(); }
    void apply(Project& p) override;
    void revert(Project& p) override;
private:
    std::vector<CommandPtr> children_;
};

class CommandStack {
public:
    explicit CommandStack(size_t limit = 500) : limit_(limit) {}
    // Applies and records. If the command throws, the project is left unchanged (best effort revert).
    void push(Project& p, CommandPtr cmd);
    bool canUndo() const { return index_ > 0; }
    bool canRedo() const { return index_ < history_.size(); }
    std::string undoLabel() const { return canUndo() ? history_[index_ - 1]->label : ""; }
    std::string redoLabel() const { return canRedo() ? history_[index_]->label : ""; }
    void undo(Project& p);
    void redo(Project& p);
    void endGesture() { gestureOpen_ = false; }  // stop merging into the last command
    void markClean() { cleanIndex_ = static_cast<long>(index_); }
    void markDirty() { cleanIndex_ = -1; }
    bool isDirty() const { return cleanIndex_ != static_cast<long>(index_); }
    size_t size() const { return history_.size(); }
    void clear() { history_.clear(); index_ = 0; cleanIndex_ = 0; }
private:
    std::vector<CommandPtr> history_;
    size_t index_ = 0;
    long cleanIndex_ = 0;
    size_t limit_;
    bool gestureOpen_ = false;
};

} // namespace kinetic
