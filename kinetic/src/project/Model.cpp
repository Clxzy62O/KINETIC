#include "project/Model.h"

#include <algorithm>
#include <functional>
#include <set>

namespace kinetic {

#define KINETIC_ENUM_TABLE(Type, ...)                                              \
    static const std::pair<Type, const char*> k##Type##Names[] = {__VA_ARGS__};   \
    const char* toString(Type v) {                                                 \
        for (auto& p : k##Type##Names) if (p.first == v) return p.second;          \
        return "?";                                                                \
    }                                                                              \
    bool parseEnum(const std::string& s, Type& out) {                              \
        for (auto& p : k##Type##Names) if (s == p.second) { out = p.first; return true; } \
        return false;                                                              \
    }

KINETIC_ENUM_TABLE(TrackKind, {TrackKind::Video, "video"}, {TrackKind::Audio, "audio"})
KINETIC_ENUM_TABLE(ClipKind, {ClipKind::Media, "media"}, {ClipKind::Composition, "composition"},
                   {ClipKind::Solid, "solid"}, {ClipKind::Adjustment, "adjustment"})
KINETIC_ENUM_TABLE(BlendMode, {BlendMode::Normal, "normal"}, {BlendMode::Add, "add"}, {BlendMode::Screen, "screen"},
                   {BlendMode::Multiply, "multiply"}, {BlendMode::Overlay, "overlay"}, {BlendMode::SoftLight, "softLight"},
                   {BlendMode::HardLight, "hardLight"}, {BlendMode::Difference, "difference"},
                   {BlendMode::Lighten, "lighten"}, {BlendMode::Darken, "darken"})
KINETIC_ENUM_TABLE(TimeInterp, {TimeInterp::Sample, "sample"}, {TimeInterp::Blend, "blend"},
                   {TimeInterp::OpticalFlow, "opticalFlow"})
KINETIC_ENUM_TABLE(FitMode, {FitMode::None, "none"}, {FitMode::Fit, "fit"}, {FitMode::Fill, "fill"})
KINETIC_ENUM_TABLE(MarkerKind, {MarkerKind::Manual, "manual"}, {MarkerKind::Beat, "beat"},
                   {MarkerKind::StrongBeat, "strongBeat"}, {MarkerKind::Section, "section"})

// ---------------------------------------------------------------- EffectInstance
AnimCurve* EffectInstance::param(const std::string& id) {
    for (auto& p : params) if (p.first == id) return &p.second;
    return nullptr;
}
const AnimCurve* EffectInstance::param(const std::string& id) const {
    for (auto& p : params) if (p.first == id) return &p.second;
    return nullptr;
}
AnimCurve& EffectInstance::paramOrAdd(const std::string& id, double def) {
    if (auto* p = param(id)) return *p;
    params.emplace_back(id, AnimCurve(def));
    return params.back().second;
}
double EffectInstance::eval(const std::string& id, double t, double def) const {
    const AnimCurve* c = param(id);
    return c ? c->evaluate(t) : def;
}

const Clip* Track::clipAt(int64_t frame) const {
    auto it = std::upper_bound(clips.begin(), clips.end(), frame, [](int64_t f, const Clip& c) { return f < c.start; });
    if (it == clips.begin()) return nullptr;
    --it;
    return it->contains(frame) ? &*it : nullptr;
}

// ---------------------------------------------------------------- Project lookups
MediaRef* Project::findMedia(Id id) { for (auto& m : media) if (m.id == id) return &m; return nullptr; }
const MediaRef* Project::findMedia(Id id) const { for (auto& m : media) if (m.id == id) return &m; return nullptr; }
Composition* Project::findComposition(Id id) { for (auto& c : compositions) if (c.id == id) return &c; return nullptr; }
const Composition* Project::findComposition(Id id) const { for (auto& c : compositions) if (c.id == id) return &c; return nullptr; }

Track* Project::findTrack(Id id, Composition** owner) {
    for (auto& c : compositions)
        for (auto& t : c.tracks)
            if (t.id == id) { if (owner) *owner = &c; return &t; }
    return nullptr;
}
const Track* Project::findTrack(Id id, const Composition** owner) const {
    for (auto& c : compositions)
        for (auto& t : c.tracks)
            if (t.id == id) { if (owner) *owner = &c; return &t; }
    return nullptr;
}
std::optional<ClipLocation> Project::findClip(Id id) {
    for (auto& c : compositions)
        for (auto& t : c.tracks)
            for (size_t i = 0; i < t.clips.size(); ++i)
                if (t.clips[i].id == id) return ClipLocation{&c, &t, i};
    return std::nullopt;
}
const Clip* Project::findClipConst(Id id) const {
    for (auto& c : compositions)
        for (auto& t : c.tracks)
            for (auto& cl : t.clips)
                if (cl.id == id) return &cl;
    return nullptr;
}

bool Project::compositionReferences(Id outer, Id inner) const {
    std::set<Id> seen;
    std::function<bool(Id)> walk = [&](Id cid) -> bool {
        if (!seen.insert(cid).second) return false;
        const Composition* c = findComposition(cid);
        if (!c) return false;
        for (auto& t : c->tracks)
            for (auto& cl : t.clips)
                if (cl.kind == ClipKind::Composition) {
                    if (cl.compositionId == inner) return true;
                    if (walk(cl.compositionId)) return true;
                }
        return false;
    };
    return walk(outer);
}

std::vector<std::string> Project::validate() const {
    std::vector<std::string> errs;
    std::set<Id> ids;
    auto claim = [&](Id id, const std::string& what) {
        if (id == kNoId) errs.push_back(what + " has no id");
        else if (!ids.insert(id).second) errs.push_back(what + " has duplicate id " + std::to_string(id));
        else if (id >= nextId) errs.push_back(what + " id " + std::to_string(id) + " >= nextId");
    };
    for (auto& m : media) claim(m.id, "media '" + m.name + "'");
    for (auto& c : compositions) {
        claim(c.id, "composition '" + c.name + "'");
        if (c.width <= 0 || c.height <= 0) errs.push_back("composition '" + c.name + "' has invalid size");
        if (c.fps.num <= 0) errs.push_back("composition '" + c.name + "' has invalid fps");
        for (auto& m : c.markers) claim(m.id, "marker");
        for (auto& t : c.tracks) {
            claim(t.id, "track '" + t.name + "'");
            int64_t prevEnd = INT64_MIN;
            for (auto& cl : t.clips) {
                claim(cl.id, "clip '" + cl.name + "'");
                if (cl.duration <= 0) errs.push_back("clip '" + cl.name + "' has non-positive duration");
                if (cl.start < prevEnd) errs.push_back("clip '" + cl.name + "' overlaps previous clip on track '" + t.name + "'");
                prevEnd = cl.end();
                if (cl.kind == ClipKind::Media && !findMedia(cl.mediaId))
                    errs.push_back("clip '" + cl.name + "' references missing media id " + std::to_string(cl.mediaId));
                if (cl.kind == ClipKind::Composition) {
                    if (!findComposition(cl.compositionId))
                        errs.push_back("clip '" + cl.name + "' references missing composition");
                    else if (cl.compositionId == c.id || compositionReferences(cl.compositionId, c.id))
                        errs.push_back("clip '" + cl.name + "' creates a composition cycle");
                }
            }
        }
    }
    if (!compositions.empty() && !findComposition(rootCompositionId)) errs.push_back("root composition missing");
    return errs;
}

Composition makeComposition(Project& p, const std::string& name, int w, int h, Rational fps, int64_t frames) {
    Composition c;
    c.id = p.allocId();
    c.name = name;
    c.width = w;
    c.height = h;
    c.fps = fps;
    c.durationFrames = frames;
    return c;
}

Track& addTrack(Project& p, Composition& c, TrackKind kind, const std::string& name) {
    Track t;
    t.id = p.allocId();
    t.kind = kind;
    t.name = name;
    if (kind == TrackKind::Audio) t.color = "#4ADE80";
    c.tracks.push_back(std::move(t));
    return c.tracks.back();
}

} // namespace kinetic
