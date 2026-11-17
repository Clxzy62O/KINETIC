#include "io/Serialize.h"

#include <algorithm>

namespace kinetic::io {

template <class E>
static E enumOr(const Json& j, const std::string& key, E def) {
    E out = def;
    if (auto* v = j.find(key); v && v->isString()) parseEnum(v->asString(), out);
    return out;
}

Json colorToJson(const Color& c) { return JsonArray{c[0], c[1], c[2], c[3]}; }
Color colorFromJson(const Json& j, const Color& def) {
    if (!j.isArray() || j.size() < 3) return def;
    Color c = def;
    for (size_t i = 0; i < 4 && i < j.size(); ++i) c[i] = j.at(i).asNumber();
    return c;
}

static Json mediaToJson(const MediaRef& m) {
    Json o = Json::object();
    o.set("id", m.id).set("path", m.path).set("name", m.name)
     .set("hasVideo", m.hasVideo).set("hasAudio", m.hasAudio)
     .set("width", m.width).set("height", m.height).set("fps", m.fps.str())
     .set("duration", m.duration).set("videoCodec", m.videoCodec).set("audioCodec", m.audioCodec)
     .set("pixFmt", m.pixFmt).set("colorSpace", m.colorSpace)
     .set("sampleRate", m.sampleRate).set("channels", m.channels)
     .set("fileSize", m.fileSize).set("mtime", m.mtime).set("isStill", m.isStill);
    return o;
}
static MediaRef mediaFromJson(const Json& j) {
    MediaRef m;
    m.id = j.integer("id", 0);
    m.path = j.str("path", "");
    m.name = j.str("name", "");
    m.hasVideo = j.boolean("hasVideo", false);
    m.hasAudio = j.boolean("hasAudio", false);
    m.width = static_cast<int>(j.integer("width", 0));
    m.height = static_cast<int>(j.integer("height", 0));
    m.fps = Rational::parse(j.str("fps", "24/1"));
    m.duration = j.num("duration", 0);
    m.videoCodec = j.str("videoCodec", "");
    m.audioCodec = j.str("audioCodec", "");
    m.pixFmt = j.str("pixFmt", "");
    m.colorSpace = j.str("colorSpace", "");
    m.sampleRate = static_cast<int>(j.integer("sampleRate", 0));
    m.channels = static_cast<int>(j.integer("channels", 0));
    m.fileSize = j.integer("fileSize", 0);
    m.mtime = j.integer("mtime", 0);
    m.isStill = j.boolean("isStill", false);
    return m;
}

Json effectToJson(const EffectInstance& e) {
    Json o = Json::object();
    o.set("effect", e.defId).set("version", e.defVersion).set("enabled", e.enabled).set("mix", e.mix);
    Json ps = Json::object();
    for (auto& p : e.params) ps.set(p.first, p.second.toJson());
    o.set("params", std::move(ps));
    return o;
}
EffectInstance effectFromJson(const Json& j) {
    EffectInstance e;
    e.defId = j.str("effect", "");
    e.defVersion = static_cast<int>(j.integer("version", 1));
    e.enabled = j.boolean("enabled", true);
    e.mix = j.num("mix", 1.0);
    if (auto* ps = j.find("params"); ps && ps->isObject())
        for (auto& kv : ps->asObject()) e.params.emplace_back(kv.first, AnimCurve::fromJson(kv.second));
    return e;
}

Json shakeToJson(const CameraShake& s) {
    Json o = Json::object();
    o.set("enabled", s.enabled).set("amplitude", s.amplitude.toJson()).set("frequency", s.frequency)
     .set("rotation", s.rotation).set("decay", s.decay).set("randomness", s.randomness)
     .set("direction", s.directionDeg).set("directional", s.directional).set("zoom", s.zoom).set("seed", s.seed);
    return o;
}
CameraShake shakeFromJson(const Json& j) {
    CameraShake s;
    if (!j.isObject()) return s;
    s.enabled = j.boolean("enabled", false);
    if (auto* a = j.find("amplitude")) s.amplitude = AnimCurve::fromJson(*a, 0);
    s.frequency = j.num("frequency", 12);
    s.rotation = j.num("rotation", 0);
    s.decay = j.num("decay", 0);
    s.randomness = j.num("randomness", 1);
    s.directionDeg = j.num("direction", 0);
    s.directional = j.num("directional", 0);
    s.zoom = j.num("zoom", 0);
    s.seed = j.integer("seed", 1);
    return s;
}

static Json transformToJson(const Transform& t) {
    Json o = Json::object();
    o.set("posX", t.posX.toJson()).set("posY", t.posY.toJson())
     .set("anchorX", t.anchorX.toJson()).set("anchorY", t.anchorY.toJson())
     .set("scale", t.scale.toJson()).set("scaleX", t.scaleX.toJson()).set("scaleY", t.scaleY.toJson())
     .set("rotation", t.rotation.toJson()).set("opacity", t.opacity.toJson()).set("fit", toString(t.fit));
    return o;
}
static Transform transformFromJson(const Json& j) {
    Transform t;
    if (!j.isObject()) return t;
    auto curve = [&](const char* k, double def) { auto* v = j.find(k); return v ? AnimCurve::fromJson(*v, def) : AnimCurve(def); };
    t.posX = curve("posX", 0); t.posY = curve("posY", 0);
    t.anchorX = curve("anchorX", 0); t.anchorY = curve("anchorY", 0);
    t.scale = curve("scale", 1); t.scaleX = curve("scaleX", 1); t.scaleY = curve("scaleY", 1);
    t.rotation = curve("rotation", 0); t.opacity = curve("opacity", 1);
    t.fit = enumOr(j, "fit", FitMode::Fit);
    return t;
}

Json clipToJson(const Clip& c) {
    Json o = Json::object();
    o.set("id", c.id).set("name", c.name).set("kind", toString(c.kind));
    if (c.kind == ClipKind::Media) o.set("media", c.mediaId);
    if (c.kind == ClipKind::Composition) o.set("composition", c.compositionId);
    if (c.kind == ClipKind::Solid) o.set("color", colorToJson(c.solidColor));
    o.set("start", c.start).set("duration", c.duration).set("sourceIn", c.sourceIn)
     .set("speed", c.speed.toJson()).set("timeInterp", toString(c.timeInterp))
     .set("transform", transformToJson(c.transform)).set("shake", shakeToJson(c.shake))
     .set("blend", toString(c.blend)).set("enabled", c.enabled).set("effectsBypassed", c.effectsBypassed)
     .set("gainDb", c.gainDb);
    Json fx = Json::array();
    for (auto& e : c.effects) fx.push(effectToJson(e));
    o.set("effects", std::move(fx));
    return o;
}
Clip clipFromJson(const Json& j) {
    Clip c;
    c.id = j.integer("id", 0);
    c.name = j.str("name", "");
    c.kind = enumOr(j, "kind", ClipKind::Media);
    c.mediaId = j.integer("media", 0);
    c.compositionId = j.integer("composition", 0);
    if (auto* col = j.find("color")) c.solidColor = colorFromJson(*col, c.solidColor);
    c.start = j.integer("start", 0);
    c.duration = j.integer("duration", 0);
    c.sourceIn = j.num("sourceIn", 0);
    if (auto* s = j.find("speed")) c.speed = AnimCurve::fromJson(*s, 1.0);
    c.timeInterp = enumOr(j, "timeInterp", TimeInterp::Sample);
    if (auto* t = j.find("transform")) c.transform = transformFromJson(*t);
    if (auto* s = j.find("shake")) c.shake = shakeFromJson(*s);
    c.blend = enumOr(j, "blend", BlendMode::Normal);
    c.enabled = j.boolean("enabled", true);
    c.effectsBypassed = j.boolean("effectsBypassed", false);
    c.gainDb = j.num("gainDb", 0);
    if (auto* fx = j.find("effects"); fx && fx->isArray())
        for (auto& e : fx->asArray()) c.effects.push_back(effectFromJson(e));
    return c;
}

static Json trackToJson(const Track& t) {
    Json o = Json::object();
    o.set("id", t.id).set("kind", toString(t.kind)).set("name", t.name)
     .set("hidden", t.hidden).set("locked", t.locked).set("muted", t.muted).set("solo", t.solo).set("color", t.color);
    Json cs = Json::array();
    for (auto& c : t.clips) cs.push(clipToJson(c));
    o.set("clips", std::move(cs));
    return o;
}
static Track trackFromJson(const Json& j) {
    Track t;
    t.id = j.integer("id", 0);
    t.kind = enumOr(j, "kind", TrackKind::Video);
    t.name = j.str("name", "");
    t.hidden = j.boolean("hidden", false);
    t.locked = j.boolean("locked", false);
    t.muted = j.boolean("muted", false);
    t.solo = j.boolean("solo", false);
    t.color = j.str("color", t.color);
    if (auto* cs = j.find("clips"); cs && cs->isArray())
        for (auto& c : cs->asArray()) t.clips.push_back(clipFromJson(c));
    std::stable_sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    return t;
}

static Json compToJson(const Composition& c) {
    Json o = Json::object();
    o.set("id", c.id).set("name", c.name).set("width", c.width).set("height", c.height)
     .set("fps", c.fps.str()).set("durationFrames", c.durationFrames).set("background", colorToJson(c.background));
    Json ts = Json::array();
    for (auto& t : c.tracks) ts.push(trackToJson(t));
    o.set("tracks", std::move(ts));
    Json ms = Json::array();
    for (auto& m : c.markers) {
        Json mj = Json::object();
        mj.set("id", m.id).set("frame", m.frame).set("kind", toString(m.kind)).set("color", m.color)
          .set("label", m.label).set("group", m.group).set("strength", m.strength);
        ms.push(std::move(mj));
    }
    o.set("markers", std::move(ms));
    return o;
}
static Composition compFromJson(const Json& j) {
    Composition c;
    c.id = j.integer("id", 0);
    c.name = j.str("name", "");
    c.width = static_cast<int>(j.integer("width", 1920));
    c.height = static_cast<int>(j.integer("height", 1080));
    c.fps = Rational::parse(j.str("fps", "60/1"));
    c.durationFrames = j.integer("durationFrames", 600);
    if (auto* b = j.find("background")) c.background = colorFromJson(*b, c.background);
    if (auto* ts = j.find("tracks"); ts && ts->isArray())
        for (auto& t : ts->asArray()) c.tracks.push_back(trackFromJson(t));
    if (auto* ms = j.find("markers"); ms && ms->isArray())
        for (auto& mj : ms->asArray()) {
            Marker m;
            m.id = mj.integer("id", 0);
            m.frame = mj.integer("frame", 0);
            m.kind = enumOr(mj, "kind", MarkerKind::Manual);
            m.color = mj.str("color", m.color);
            m.label = mj.str("label", "");
            m.group = mj.str("group", "");
            m.strength = mj.num("strength", 1.0);
            c.markers.push_back(m);
        }
    return c;
}

Json toJson(const Project& p) {
    Json o = Json::object();
    o.set("format", "kinetic-project").set("schemaVersion", Project::kSchemaVersion);
    Json s = Json::object();
    s.set("name", p.settings.name).set("autosaveIntervalSec", p.settings.autosaveIntervalSec)
     .set("colorProfile", p.settings.colorProfile);
    o.set("settings", std::move(s));
    o.set("nextId", p.nextId).set("rootComposition", p.rootCompositionId);
    Json ms = Json::array();
    for (auto& m : p.media) ms.push(mediaToJson(m));
    o.set("media", std::move(ms));
    Json cs = Json::array();
    for (auto& c : p.compositions) cs.push(compToJson(c));
    o.set("compositions", std::move(cs));
    return o;
}

// ---------------------------------------------------------------- migrations
// v1 (pre-release): clips stored a scalar "clipSpeed" and transform "opacity" as 0..100 percent.
static void migrate1to2(Json& j, std::vector<std::string>* notes) {
    int converted = 0;
    Json* comps = j.findMut("compositions");
    if (comps && comps->isArray()) {
        for (auto& cj : comps->asArray()) {
            Json* tracks = cj.findMut("tracks");
            if (!tracks || !tracks->isArray()) continue;
            for (auto& tj : tracks->asArray()) {
                Json* clips = tj.findMut("clips");
                if (!clips || !clips->isArray()) continue;
                for (auto& clj : clips->asArray()) {
                    if (const Json* sp = clj.find("clipSpeed"); sp && sp->isNumber()) {
                        double v = sp->asNumber();
                        clj.set("speed", v);
                        auto& obj = clj.asObject();
                        obj.erase(std::remove_if(obj.begin(), obj.end(), [](auto& kv) { return kv.first == "clipSpeed"; }), obj.end());
                        ++converted;
                    }
                    if (Json* tr = clj.findMut("transform"); tr && tr->isObject()) {
                        if (const Json* op = tr->find("opacity"); op && op->isNumber())
                            tr->set("opacity", op->asNumber() / 100.0);
                    }
                }
            }
        }
    }
    if (notes) notes->push_back("Upgraded project from schema 1 to 2 (" + std::to_string(converted) + " clip speeds converted)");
}

int migrate(Json& j, std::vector<std::string>* notes) {
    if (j.str("format", "") != "kinetic-project") throw JsonError("not a KINETIC project file");
    int v = static_cast<int>(j.integer("schemaVersion", 0));
    int start = v;
    if (v < 1) throw JsonError("unknown project schema version");
    if (v > Project::kSchemaVersion)
        throw JsonError("project was saved by a newer KINETIC (schema " + std::to_string(v) + "); refusing to load to avoid data loss");
    if (v == 1) { migrate1to2(j, notes); v = 2; }
    j.set("schemaVersion", v);
    return start;
}

Project fromJson(const Json& in, std::vector<std::string>* warnings) {
    Json j = in;
    migrate(j, warnings);
    Project p;
    if (auto* s = j.find("settings")) {
        p.settings.name = s->str("name", p.settings.name);
        p.settings.autosaveIntervalSec = static_cast<int>(s->integer("autosaveIntervalSec", p.settings.autosaveIntervalSec));
        p.settings.colorProfile = s->str("colorProfile", p.settings.colorProfile);
    }
    p.nextId = j.integer("nextId", 1);
    p.rootCompositionId = j.integer("rootComposition", 0);
    if (auto* ms = j.find("media"); ms && ms->isArray())
        for (auto& m : ms->asArray()) p.media.push_back(mediaFromJson(m));
    if (auto* cs = j.find("compositions"); cs && cs->isArray())
        for (auto& c : cs->asArray()) p.compositions.push_back(compFromJson(c));
    return p;
}

} // namespace kinetic::io
