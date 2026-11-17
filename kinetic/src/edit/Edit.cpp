#include "edit/Edit.h"

#include <algorithm>
#include <cmath>

#include "project/Velocity.h"

namespace kinetic::edit {

namespace {

struct Found {
    const Composition* comp;
    const Track* track;
    const Clip* clip;
};

Found findOrThrow(const Project& p, Id clipId) {
    for (auto& c : p.compositions)
        for (auto& t : c.tracks)
            for (auto& cl : t.clips)
                if (cl.id == clipId) return {&c, &t, &cl};
    throw CommandError("clip " + std::to_string(clipId) + " not found");
}

void requireUnlocked(const Track& t) {
    if (t.locked) throw CommandError("track '" + t.name + "' is locked");
}

// Media clips may not sample outside their source (a frame of slack covers rounding).
void checkSourceBounds(const Project& p, const Composition& comp, const Clip& c) {
    if (c.kind != ClipKind::Media) return;
    const MediaRef* m = p.findMedia(c.mediaId);
    if (!m || m->isStill || m->duration <= 0) return;
    double lo, hi;
    Velocity::sourceRange(c, comp.fps, lo, hi);
    double slack = framesToSeconds(1, m->fps) + 1e-6;
    if (lo < -slack || hi > m->duration + slack)
        throw CommandError("edit would read outside the source media (" + std::to_string(lo) + "s.." +
                           std::to_string(hi) + "s of " + std::to_string(m->duration) + "s)");
}

void applyAnimShift(AnimCurve& c, double dt) { c.offsetTime(-dt); }

} // namespace

void shiftLocalAnimation(Clip& c, double dt) {
    if (dt == 0) return;
    Transform& tr = c.transform;
    for (AnimCurve* a : {&tr.posX, &tr.posY, &tr.anchorX, &tr.anchorY, &tr.scale, &tr.scaleX, &tr.scaleY, &tr.rotation, &tr.opacity})
        applyAnimShift(*a, dt);
    applyAnimShift(c.speed, dt);
    applyAnimShift(c.shake.amplitude, dt);
    for (auto& e : c.effects)
        for (auto& pr : e.params) applyAnimShift(pr.second, dt);
}

bool rangeFree(const Track& t, int64_t start, int64_t dur, const std::vector<Id>& ignore) {
    int64_t end = start + dur;
    for (auto& c : t.clips) {
        if (std::find(ignore.begin(), ignore.end(), c.id) != ignore.end()) continue;
        if (c.start < end && start < c.end()) return false;
    }
    return true;
}

CommandPtr addClip(const Project& p, Id trackId, Clip clip) {
    const Composition* comp = nullptr;
    const Track* t = p.findTrack(trackId, &comp);
    if (!t) throw CommandError("track not found");
    requireUnlocked(*t);
    if (clip.id == kNoId || clip.id >= p.nextId) throw CommandError("clip needs an allocated id");
    if (clip.duration <= 0) throw CommandError("clip duration must be positive");
    if (clip.start < 0) throw CommandError("clip cannot start before frame 0");
    if (!rangeFree(*t, clip.start, clip.duration)) throw CommandError("clip overlaps an existing clip");
    if (clip.kind == ClipKind::Composition &&
        (clip.compositionId == comp->id || p.compositionReferences(clip.compositionId, comp->id)))
        throw CommandError("nesting this composition would create a cycle");
    checkSourceBounds(p, *comp, clip);
    auto cmd = std::make_unique<InsertClipCmd>(trackId, std::move(clip));
    cmd->label = "Add Clip";
    return cmd;
}

CommandPtr deleteClip(const Project& p, Id clipId) {
    auto f = findOrThrow(p, clipId);
    requireUnlocked(*f.track);
    auto cmd = std::make_unique<RemoveClipCmd>(f.track->id, *f.clip);
    cmd->label = "Delete Clip";
    return cmd;
}

CommandPtr rippleDelete(const Project& p, Id clipId) {
    auto f = findOrThrow(p, clipId);
    requireUnlocked(*f.track);
    auto macro = std::make_unique<MacroCommand>("Ripple Delete");
    macro->add(std::make_unique<RemoveClipCmd>(f.track->id, *f.clip));
    for (auto& c : f.track->clips) {
        if (c.start >= f.clip->end()) {
            Clip moved = c;
            moved.start -= f.clip->duration;
            macro->add(std::make_unique<ReplaceClipCmd>(f.track->id, c, f.track->id, moved));
        }
    }
    return macro;
}

CommandPtr moveClip(const Project& p, Id clipId, int64_t newStart, Id newTrackId) {
    auto f = findOrThrow(p, clipId);
    requireUnlocked(*f.track);
    if (newTrackId == kNoId) newTrackId = f.track->id;
    const Composition* destComp = nullptr;
    const Track* dest = p.findTrack(newTrackId, &destComp);
    if (!dest) throw CommandError("destination track not found");
    requireUnlocked(*dest);
    if (destComp != f.comp) throw CommandError("cannot move clips between compositions");
    if (dest->kind != f.track->kind) throw CommandError("cannot move a clip to a different kind of track");
    if (newStart < 0) throw CommandError("clip cannot start before frame 0");
    if (!rangeFree(*dest, newStart, f.clip->duration, {clipId})) throw CommandError("destination overlaps an existing clip");
    Clip after = *f.clip;
    after.start = newStart;
    auto cmd = std::make_unique<ReplaceClipCmd>(f.track->id, *f.clip, newTrackId, after);
    cmd->label = "Move Clip";
    cmd->mergeKey = "move:" + std::to_string(clipId);
    return cmd;
}

static Clip trimmedStart(const Clip& c, int64_t newStart, const Rational& fps) {
    int64_t delta = newStart - c.start;
    double dt = framesToSeconds(delta, fps);
    Clip out = c;
    out.sourceIn = Velocity::sourceTime(c, dt);
    out.start = newStart;
    out.duration = c.duration - delta;
    shiftLocalAnimation(out, dt);
    return out;
}

CommandPtr trimStart(const Project& p, Id clipId, int64_t newStart) {
    auto f = findOrThrow(p, clipId);
    requireUnlocked(*f.track);
    if (newStart >= f.clip->end()) throw CommandError("trim would make the clip empty");
    if (newStart < 0) throw CommandError("clip cannot start before frame 0");
    if (!rangeFree(*f.track, newStart, f.clip->end() - newStart, {clipId})) throw CommandError("trim overlaps a neighbouring clip");
    Clip after = trimmedStart(*f.clip, newStart, f.comp->fps);
    checkSourceBounds(p, *f.comp, after);
    auto cmd = std::make_unique<ReplaceClipCmd>(f.track->id, *f.clip, f.track->id, after);
    cmd->label = "Trim Start";
    cmd->mergeKey = "trimStart:" + std::to_string(clipId);
    return cmd;
}

CommandPtr trimEnd(const Project& p, Id clipId, int64_t newEnd) {
    auto f = findOrThrow(p, clipId);
    requireUnlocked(*f.track);
    if (newEnd <= f.clip->start) throw CommandError("trim would make the clip empty");
    if (!rangeFree(*f.track, f.clip->start, newEnd - f.clip->start, {clipId})) throw CommandError("trim overlaps a neighbouring clip");
    Clip after = *f.clip;
    after.duration = newEnd - f.clip->start;
    checkSourceBounds(p, *f.comp, after);
    auto cmd = std::make_unique<ReplaceClipCmd>(f.track->id, *f.clip, f.track->id, after);
    cmd->label = "Trim End";
    cmd->mergeKey = "trimEnd:" + std::to_string(clipId);
    return cmd;
}

CommandPtr rollEdit(const Project& p, Id leftId, Id rightId, int64_t newCut) {
    auto l = findOrThrow(p, leftId);
    auto r = findOrThrow(p, rightId);
    if (l.track != r.track) throw CommandError("roll edit needs adjacent clips on one track");
    requireUnlocked(*l.track);
    if (l.clip->end() != r.clip->start) throw CommandError("clips are not adjacent");
    if (newCut <= l.clip->start || newCut >= r.clip->end()) throw CommandError("roll point out of range");
    Clip la = *l.clip;
    la.duration = newCut - la.start;
    Clip ra = trimmedStart(*r.clip, newCut, r.comp->fps);
    checkSourceBounds(p, *l.comp, la);
    checkSourceBounds(p, *r.comp, ra);
    auto macro = std::make_unique<MacroCommand>("Roll Edit");
    macro->add(std::make_unique<ReplaceClipCmd>(l.track->id, *l.clip, l.track->id, la));
    macro->add(std::make_unique<ReplaceClipCmd>(r.track->id, *r.clip, r.track->id, ra));
    return macro;
}

CommandPtr slip(const Project& p, Id clipId, double delta) {
    auto f = findOrThrow(p, clipId);
    requireUnlocked(*f.track);
    Clip after = *f.clip;
    after.sourceIn += delta;
    checkSourceBounds(p, *f.comp, after);
    auto cmd = std::make_unique<ReplaceClipCmd>(f.track->id, *f.clip, f.track->id, after);
    cmd->label = "Slip";
    cmd->mergeKey = "slip:" + std::to_string(clipId);
    return cmd;
}

CommandPtr split(const Project& p, Id clipId, int64_t frame) {
    auto f = findOrThrow(p, clipId);
    requireUnlocked(*f.track);
    if (frame <= f.clip->start || frame >= f.clip->end()) throw CommandError("split point must be inside the clip");
    Clip left = *f.clip;
    left.duration = frame - f.clip->start;
    Clip right = trimmedStart(*f.clip, frame, f.comp->fps);
    right.id = p.nextId;
    auto macro = std::make_unique<MacroCommand>("Split");
    macro->add(std::make_unique<SetNextIdCmd>(p.nextId, p.nextId + 1));
    macro->add(std::make_unique<ReplaceClipCmd>(f.track->id, *f.clip, f.track->id, left));
    macro->add(std::make_unique<InsertClipCmd>(f.track->id, right));
    return macro;
}

CommandPtr duplicate(const Project& p, Id clipId, int64_t newStart, Id trackId) {
    auto f = findOrThrow(p, clipId);
    if (trackId == kNoId) trackId = f.track->id;
    Clip c = *f.clip;
    c.id = p.nextId;
    c.start = newStart;
    const Track* t = p.findTrack(trackId);
    if (!t) throw CommandError("track not found");
    requireUnlocked(*t);
    if (t->kind != f.track->kind) throw CommandError("incompatible track");
    if (!rangeFree(*t, c.start, c.duration)) throw CommandError("duplicate overlaps an existing clip");
    auto macro = std::make_unique<MacroCommand>("Duplicate");
    macro->add(std::make_unique<SetNextIdCmd>(p.nextId, p.nextId + 1));
    macro->add(std::make_unique<InsertClipCmd>(trackId, c));
    return macro;
}

CommandPtr setClip(const Project& p, const Clip& updated, const std::string& label, const std::string& mergeKey) {
    auto f = findOrThrow(p, updated.id);
    requireUnlocked(*f.track);
    if (updated.start != f.clip->start || updated.duration != f.clip->duration) {
        if (updated.duration <= 0 || !rangeFree(*f.track, updated.start, updated.duration, {updated.id}))
            throw CommandError("timing change overlaps a neighbouring clip");
    }
    auto cmd = std::make_unique<ReplaceClipCmd>(f.track->id, *f.clip, f.track->id, updated);
    cmd->label = label;
    cmd->mergeKey = mergeKey;
    return cmd;
}

std::optional<SnapResult> snap(const Composition& c, int64_t frame, int64_t tol, const std::vector<Id>& ignore,
                               std::optional<int64_t> playhead) {
    std::optional<SnapResult> best;
    int64_t bestDist = tol + 1;
    auto consider = [&](int64_t f, const char* what) {
        int64_t d = std::llabs(f - frame);
        if (d < bestDist) { bestDist = d; best = SnapResult{f, what}; }
    };
    for (auto& t : c.tracks)
        for (auto& cl : t.clips) {
            if (std::find(ignore.begin(), ignore.end(), cl.id) != ignore.end()) continue;
            consider(cl.start, "clip start");
            consider(cl.end(), "clip end");
        }
    for (auto& m : c.markers) consider(m.frame, m.kind == MarkerKind::Manual ? "marker" : "beat");
    if (playhead) consider(*playhead, "playhead");
    return best;
}

} // namespace kinetic::edit
