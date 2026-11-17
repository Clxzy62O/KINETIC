#include "edit/Commands.h"

#include <algorithm>

namespace kinetic {

static Track& trackOrThrow(Project& p, Id trackId) {
    Track* t = p.findTrack(trackId);
    if (!t) throw CommandError("track " + std::to_string(trackId) + " not found");
    return *t;
}
static Composition& compOrThrow(Project& p, Id id) {
    Composition* c = p.findComposition(id);
    if (!c) throw CommandError("composition " + std::to_string(id) + " not found");
    return *c;
}

static void insertSorted(Track& t, const Clip& c) {
    auto it = std::lower_bound(t.clips.begin(), t.clips.end(), c.start, [](const Clip& a, int64_t s) { return a.start < s; });
    t.clips.insert(it, c);
}
static void removeById(Track& t, Id id) {
    auto it = std::find_if(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.id == id; });
    if (it == t.clips.end()) throw CommandError("clip " + std::to_string(id) + " not on track '" + t.name + "'");
    t.clips.erase(it);
}

void InsertClipCmd::apply(Project& p) { insertSorted(trackOrThrow(p, trackId_), clip_); }
void InsertClipCmd::revert(Project& p) { removeById(trackOrThrow(p, trackId_), clip_.id); }
void RemoveClipCmd::apply(Project& p) { removeById(trackOrThrow(p, trackId_), clip_.id); }
void RemoveClipCmd::revert(Project& p) { insertSorted(trackOrThrow(p, trackId_), clip_); }

void ReplaceClipCmd::apply(Project& p) {
    removeById(trackOrThrow(p, beforeTrack_), before_.id);
    insertSorted(trackOrThrow(p, afterTrack_), after_);
}
void ReplaceClipCmd::revert(Project& p) {
    removeById(trackOrThrow(p, afterTrack_), after_.id);
    insertSorted(trackOrThrow(p, beforeTrack_), before_);
}
bool ReplaceClipCmd::mergeWith(const Command& next) {
    auto* n = dynamic_cast<const ReplaceClipCmd*>(&next);
    if (!n || n->before_.id != after_.id || n->beforeTrack_ != afterTrack_) return false;
    after_ = n->after_;
    afterTrack_ = n->afterTrack_;
    return true;
}

static void insertMarker(Composition& c, const Marker& m) {
    auto it = std::lower_bound(c.markers.begin(), c.markers.end(), m.frame, [](const Marker& a, int64_t f) { return a.frame < f; });
    c.markers.insert(it, m);
}
static void removeMarker(Composition& c, Id id) {
    auto it = std::find_if(c.markers.begin(), c.markers.end(), [&](const Marker& m) { return m.id == id; });
    if (it == c.markers.end()) throw CommandError("marker not found");
    c.markers.erase(it);
}
void AddMarkerCmd::apply(Project& p) { insertMarker(compOrThrow(p, compId_), marker_); }
void AddMarkerCmd::revert(Project& p) { removeMarker(compOrThrow(p, compId_), marker_.id); }
void RemoveMarkerCmd::apply(Project& p) { removeMarker(compOrThrow(p, compId_), marker_.id); }
void RemoveMarkerCmd::revert(Project& p) { insertMarker(compOrThrow(p, compId_), marker_); }

void AddTrackCmd::apply(Project& p) {
    auto& c = compOrThrow(p, compId_);
    size_t i = std::min(index_, c.tracks.size());
    c.tracks.insert(c.tracks.begin() + static_cast<long>(i), track_);
}
void AddTrackCmd::revert(Project& p) {
    auto& c = compOrThrow(p, compId_);
    auto it = std::find_if(c.tracks.begin(), c.tracks.end(), [&](const Track& t) { return t.id == track_.id; });
    if (it == c.tracks.end()) throw CommandError("track not found");
    c.tracks.erase(it);
}

void SetTrackStateCmd::set(Project& p, const Track& h) {
    Track& t = trackOrThrow(p, h.id);
    auto clips = std::move(t.clips);
    t = h;
    t.clips = std::move(clips);
}

void AddCompositionCmd::apply(Project& p) {
    if (p.findComposition(comp_.id)) throw CommandError("composition id exists");
    p.compositions.push_back(comp_);
}
void AddCompositionCmd::revert(Project& p) {
    auto it = std::find_if(p.compositions.begin(), p.compositions.end(), [&](const Composition& c) { return c.id == comp_.id; });
    if (it != p.compositions.end()) p.compositions.erase(it);
}
void AddMediaCmd::apply(Project& p) { p.media.push_back(media_); }
void AddMediaCmd::revert(Project& p) {
    auto it = std::find_if(p.media.begin(), p.media.end(), [&](const MediaRef& m) { return m.id == media_.id; });
    if (it != p.media.end()) p.media.erase(it);
}

void MacroCommand::apply(Project& p) {
    size_t done = 0;
    try {
        for (; done < children_.size(); ++done) children_[done]->apply(p);
    } catch (...) {
        while (done > 0) children_[--done]->revert(p);  // keep the project consistent
        throw;
    }
}
void MacroCommand::revert(Project& p) {
    for (size_t i = children_.size(); i > 0; --i) children_[i - 1]->revert(p);
}

void CommandStack::push(Project& p, CommandPtr cmd) {
    cmd->apply(p);  // throws => nothing recorded
    if (index_ < history_.size()) {
        history_.erase(history_.begin() + static_cast<long>(index_), history_.end());
        if (cleanIndex_ > static_cast<long>(index_)) cleanIndex_ = -1;  // clean state no longer reachable
    }
    if (gestureOpen_ && index_ > 0 && !cmd->mergeKey.empty() && history_[index_ - 1]->mergeKey == cmd->mergeKey &&
        history_[index_ - 1]->mergeWith(*cmd)) {
        if (cleanIndex_ == static_cast<long>(index_)) cleanIndex_ = -1;
        return;
    }
    history_.push_back(std::move(cmd));
    ++index_;
    gestureOpen_ = true;
    if (history_.size() > limit_) {
        history_.erase(history_.begin());
        --index_;
        --cleanIndex_;
    }
}

void CommandStack::undo(Project& p) {
    if (!canUndo()) return;
    history_[index_ - 1]->revert(p);
    --index_;
    gestureOpen_ = false;
}
void CommandStack::redo(Project& p) {
    if (!canRedo()) return;
    history_[index_]->apply(p);
    ++index_;
    gestureOpen_ = false;
}

} // namespace kinetic
