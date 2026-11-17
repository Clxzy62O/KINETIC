#include "io/Preset.h"

#include <set>
#include <stdexcept>

#include "io/Serialize.h"

namespace kinetic::io {

static Json header(const std::string& kind, const std::string& name, const std::string& category) {
    Json j = Json::object();
    j.set("format", "kinetic-preset").set("schemaVersion", kPresetSchemaVersion).set("kind", kind)
     .set("name", name).set("category", category);
    return j;
}

Json makeEffectStackPreset(const std::string& name, const std::string& category, const std::vector<EffectInstance>& fx) {
    Json j = header("effectStack", name, category);
    Json req = Json::array();
    std::set<std::string> seen;
    Json list = Json::array();
    for (auto& e : fx) {
        if (seen.insert(e.defId + "@" + std::to_string(e.defVersion)).second) {
            Json r = Json::object();
            r.set("effect", e.defId).set("version", e.defVersion);
            req.push(std::move(r));
        }
        list.push(effectToJson(e));
    }
    j.set("requires", std::move(req));
    Json payload = Json::object();
    payload.set("effects", std::move(list));
    j.set("payload", std::move(payload));
    return j;
}

Json makeVelocityPreset(const std::string& name, const AnimCurve& speed, double dur) {
    Json j = header("velocityCurve", name, "Velocity");
    j.set("requires", Json::array());
    Json payload = Json::object();
    payload.set("speed", speed.toJson()).set("referenceDuration", dur);
    j.set("payload", std::move(payload));
    return j;
}

Json makeShakePreset(const std::string& name, const CameraShake& s) {
    Json j = header("cameraShake", name, "Shake");
    j.set("requires", Json::array());
    Json payload = Json::object();
    payload.set("shake", shakeToJson(s));
    j.set("payload", std::move(payload));
    return j;
}

static void validateHeader(const Json& p) {
    if (p.str("format", "") != "kinetic-preset") throw std::runtime_error("not a KINETIC preset");
    if (p.integer("schemaVersion", 0) > kPresetSchemaVersion)
        throw std::runtime_error("preset requires a newer KINETIC (preset schema " + std::to_string(p.integer("schemaVersion", 0)) + ")");
}

PresetReport checkPreset(const Json& preset, const EffectAvailableFn& available) {
    validateHeader(preset);
    PresetReport r;
    if (auto* req = preset.find("requires"); req && req->isArray())
        for (auto& e : req->asArray()) {
            std::string id = e.str("effect", "");
            int v = static_cast<int>(e.integer("version", 1));
            if (!available(id, v)) r.missingEffects.push_back(id + "@" + std::to_string(v));
        }
    return r;
}

Clip applyPreset(const Json& preset, const Clip& target, double dur, const EffectAvailableFn& available,
                 PresetReport& report, bool allowMissing) {
    report = checkPreset(preset, available);
    if (!report.ok() && !allowMissing) {
        std::string msg = "preset uses unavailable effects:";
        for (auto& m : report.missingEffects) msg += " " + m;
        throw std::runtime_error(msg);
    }
    Clip out = target;
    const std::string kind = preset.str("kind", "");
    const Json& payload = preset["payload"];
    if (kind == "effectStack") {
        for (auto& e : payload["effects"].asArray()) out.effects.push_back(effectFromJson(e));
        report.notes.push_back("Added " + std::to_string(payload["effects"].size()) + " effects");
        if (!report.ok()) report.notes.push_back("Unavailable effects were kept and will render as pass-through with a warning");
    } else if (kind == "velocityCurve") {
        AnimCurve s = AnimCurve::fromJson(payload["speed"], 1.0);
        double ref = payload.num("referenceDuration", dur);
        if (ref > 0 && dur > 0 && s.animated()) s.scaleTime(0, dur / ref);
        out.speed = s;
        report.notes.push_back("Velocity curve applied (scaled to clip length)");
    } else if (kind == "cameraShake") {
        out.shake = shakeFromJson(payload["shake"]);
        out.shake.enabled = true;
        report.notes.push_back("Camera shake applied");
    } else {
        throw std::runtime_error("unsupported preset kind '" + kind + "'");
    }
    return out;
}

} // namespace kinetic::io
