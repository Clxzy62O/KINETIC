#include "edit/BeatSync.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "edit/Edit.h"

namespace kinetic::beatsync {

std::vector<BeatPoint> filter(const std::vector<BeatPoint>& beats, bool strongOnly, int64_t from, int64_t to) {
    std::vector<BeatPoint> out;
    std::set<int64_t> seen;
    for (auto& b : beats) {
        if (strongOnly && !b.strong) continue;
        if (b.frame < from || b.frame >= to) continue;
        if (!seen.insert(b.frame).second) continue;  // two beats rounding to one frame
        out.push_back(b);
    }
    std::sort(out.begin(), out.end(), [](const BeatPoint& a, const BeatPoint& b) { return a.frame < b.frame; });
    return out;
}

static std::string fr(int64_t f) { return "frame " + std::to_string(f); }

EditPlan planBeatMarkers(const Project& p, Id compId, const std::vector<BeatPoint>& beats) {
    const Composition* c = p.findComposition(compId);
    if (!c) throw CommandError("composition not found");
    std::set<int64_t> existing;
    for (auto& m : c->markers) if (m.kind == MarkerKind::Beat || m.kind == MarkerKind::StrongBeat) existing.insert(m.frame);
    EditPlan plan;
    auto macro = std::make_unique<MacroCommand>("Add Beat Markers");
    Id next = p.nextId;
    for (auto& b : filter(beats, false, 0, c->durationFrames)) {
        if (existing.count(b.frame)) continue;
        Marker m;
        m.id = next++;
        m.frame = b.frame;
        m.kind = b.strong ? MarkerKind::StrongBeat : MarkerKind::Beat;
        m.color = b.strong ? "#FBBF24" : "#38BDF8";
        m.strength = b.strength;
        m.group = "beats";
        macro->add(std::make_unique<AddMarkerCmd>(compId, m));
        ++plan.changes;
    }
    if (plan.changes) {
        macro->add(std::make_unique<SetNextIdCmd>(p.nextId, next));
        plan.command = std::move(macro);
    }
    plan.summary.push_back("Add " + std::to_string(plan.changes) + " beat markers");
    return plan;
}

EditPlan planCutsAtBeats(const Project& p, Id clipId, const std::vector<BeatPoint>& beats, bool strongOnly) {
    const Clip* clip = p.findClipConst(clipId);
    if (!clip) throw CommandError("clip not found");
    auto inside = filter(beats, strongOnly, clip->start + 1, clip->end());
    EditPlan plan;
    if (inside.empty()) { plan.summary.push_back("No beats inside the clip"); return plan; }
    // Plan on a scratch copy, splitting from the last beat backwards so `clipId` stays the left piece.
    Project scratch = p;
    auto macro = std::make_unique<MacroCommand>(strongOnly ? "Cut at Strong Beats" : "Cut at Beats");
    for (auto it = inside.rbegin(); it != inside.rend(); ++it) {
        auto cmd = edit::split(scratch, clipId, it->frame);
        cmd->apply(scratch);
        macro->add(std::move(cmd));
        plan.summary.push_back("Split at " + fr(it->frame));
        ++plan.changes;
    }
    std::reverse(plan.summary.begin(), plan.summary.end());
    plan.command = std::move(macro);
    return plan;
}

EditPlan planDistributeToBeats(const Project& p, Id trackId, const std::vector<Id>& clipIds,
                               const std::vector<BeatPoint>& beats, bool strongOnly, int64_t fromFrame) {
    const Track* t = p.findTrack(trackId);
    if (!t) throw CommandError("track not found");
    if (t->locked) throw CommandError("track is locked");
    auto bs = filter(beats, strongOnly, fromFrame);
    if (bs.size() < clipIds.size() + 1) throw CommandError("not enough beats to distribute " + std::to_string(clipIds.size()) + " clips");
    EditPlan plan;
    auto macro = std::make_unique<MacroCommand>("Distribute Clips to Beats");
    std::vector<std::pair<Id, Clip>> placed;  // (original track, new clip)
    for (Id id : clipIds) {
        const Composition* comp = nullptr;
        const Track* src = nullptr;
        const Clip* c = nullptr;
        for (auto& cc : p.compositions)
            for (auto& tt : cc.tracks)
                for (auto& cl : tt.clips)
                    if (cl.id == id) { comp = &cc; src = &tt; c = &cl; }
        if (!c) throw CommandError("clip " + std::to_string(id) + " not found");
        if (src->kind != t->kind) throw CommandError("clip '" + c->name + "' cannot go on this track type");
        if (src->locked) throw CommandError("track '" + src->name + "' is locked");
        (void)comp;
        macro->add(std::make_unique<RemoveClipCmd>(src->id, *c));
        placed.emplace_back(src->id, *c);
    }
    for (size_t i = 0; i < placed.size(); ++i) {
        Clip c = placed[i].second;
        int64_t gap = bs[i + 1].frame - bs[i].frame;
        c.start = bs[i].frame;
        c.duration = std::min(c.duration, gap);
        if (!edit::rangeFree(*t, c.start, c.duration, clipIds))
            throw CommandError("beat slot at " + fr(c.start) + " is occupied by another clip");
        plan.summary.push_back("'" + c.name + "' -> " + fr(c.start) + " (" + std::to_string(c.duration) + " frames)");
        macro->add(std::make_unique<InsertClipCmd>(trackId, c));
        ++plan.changes;
    }
    plan.command = std::move(macro);
    return plan;
}

EditPlan planFlashesOnBeats(const Project& p, Id trackId, const std::vector<BeatPoint>& beats, int flashFrames,
                            const Color& color, double peak, bool strongOnly, BlendMode blend) {
    const Composition* comp = nullptr;
    const Track* t = p.findTrack(trackId, &comp);
    if (!t) throw CommandError("track not found");
    if (t->locked) throw CommandError("track is locked");
    if (flashFrames < 1) throw CommandError("flash must be at least one frame");
    EditPlan plan;
    auto macro = std::make_unique<MacroCommand>("Flashes on Beats");
    Id next = p.nextId;
    auto bs = filter(beats, strongOnly, 0, comp->durationFrames);
    int skipped = 0;
    for (size_t i = 0; i < bs.size(); ++i) {
        int64_t dur = flashFrames;
        if (i + 1 < bs.size()) dur = std::min<int64_t>(dur, bs[i + 1].frame - bs[i].frame);
        dur = std::min<int64_t>(dur, comp->durationFrames - bs[i].frame);
        if (dur < 1 || !edit::rangeFree(*t, bs[i].frame, dur)) { ++skipped; continue; }
        Clip c;
        c.id = next++;
        c.name = "Flash";
        c.kind = ClipKind::Solid;
        c.solidColor = color;
        c.start = bs[i].frame;
        c.duration = dur;
        c.blend = blend;
        double len = framesToSeconds(dur, comp->fps);
        c.transform.opacity.setKey(0, peak * std::clamp(0.4 + 0.6 * bs[i].strength, 0.0, 1.0));
        c.transform.opacity.setKey(len, 0.0);
        c.transform.opacity.setSegmentEase(0, EasePreset::EaseOut);
        macro->add(std::make_unique<InsertClipCmd>(trackId, c));
        ++plan.changes;
    }
    if (plan.changes) {
        macro->add(std::make_unique<SetNextIdCmd>(p.nextId, next));
        plan.command = std::move(macro);
    }
    plan.summary.push_back("Add " + std::to_string(plan.changes) + " flashes" +
                           (skipped ? " (" + std::to_string(skipped) + " skipped: slot occupied)" : ""));
    return plan;
}

static void pulseKeys(AnimCurve& curve, const std::vector<double>& times, const std::vector<double>& strengths,
                      double base, double peak, double decay, double clipLen) {
    curve.clearKeys();
    curve.setConstant(base);
    curve.setKey(0, base, Interp::Linear);
    for (size_t i = 0; i < times.size(); ++i) {
        double t = times[i];
        double pre = std::max(0.0, t - 1e-3);
        if (pre > 0) curve.setKey(pre, base);
        curve.setKey(t, base + (peak - base) * strengths[i]);
        double end = std::min(clipLen, t + decay);
        if (i + 1 < times.size()) end = std::min(end, times[i + 1] - 2e-3);
        if (end > t) curve.setKey(end, base);
    }
    // Ease out of every peak (fast attack, smooth release).
    for (size_t i = 0; i < curve.keys().size(); ++i)
        if (curve.keys()[i].v != base) curve.setSegmentEase(i, EasePreset::EaseOut);
}

EditPlan planParamPulseOnBeats(const Project& p, Id clipId, size_t effectIndex, const std::string& paramId,
                               const std::vector<BeatPoint>& beats, double base, double peak, double decay,
                               bool strongOnly) {
    const Composition* comp = nullptr;
    const Clip* c = nullptr;
    for (auto& cc : p.compositions) for (auto& tt : cc.tracks) for (auto& cl : tt.clips) if (cl.id == clipId) { comp = &cc; c = &cl; }
    if (!c) throw CommandError("clip not found");
    if (effectIndex >= c->effects.size()) throw CommandError("effect index out of range");
    auto bs = filter(beats, strongOnly, c->start, c->end());
    Clip after = *c;
    std::vector<double> times, strengths;
    for (auto& b : bs) { times.push_back(framesToSeconds(b.frame - c->start, comp->fps)); strengths.push_back(b.strong ? 1.0 : std::clamp(b.strength, 0.3, 1.0)); }
    pulseKeys(after.effects[effectIndex].paramOrAdd(paramId, base), times, strengths, base, peak, decay,
              framesToSeconds(c->duration, comp->fps));
    EditPlan plan;
    plan.changes = static_cast<int>(times.size());
    plan.summary.push_back("Pulse '" + paramId + "' on " + std::to_string(times.size()) + " beats");
    plan.command = edit::setClip(p, after, "Pulse on Beats");
    return plan;
}

EditPlan planShakeOnBeats(const Project& p, Id clipId, const std::vector<BeatPoint>& beats, double amplitude,
                          double decay, bool strongOnly) {
    const Composition* comp = nullptr;
    const Clip* c = nullptr;
    for (auto& cc : p.compositions) for (auto& tt : cc.tracks) for (auto& cl : tt.clips) if (cl.id == clipId) { comp = &cc; c = &cl; }
    if (!c) throw CommandError("clip not found");
    auto bs = filter(beats, strongOnly, c->start, c->end());
    Clip after = *c;
    after.shake.enabled = true;
    std::vector<double> times, strengths;
    for (auto& b : bs) { times.push_back(framesToSeconds(b.frame - c->start, comp->fps)); strengths.push_back(b.strong ? 1.0 : std::clamp(b.strength, 0.3, 1.0)); }
    pulseKeys(after.shake.amplitude, times, strengths, 0.0, amplitude, decay, framesToSeconds(c->duration, comp->fps));
    EditPlan plan;
    plan.changes = static_cast<int>(times.size());
    plan.summary.push_back("Shake on " + std::to_string(times.size()) + " beats");
    plan.command = edit::setClip(p, after, "Shake on Beats");
    return plan;
}

} // namespace kinetic::beatsync
