#pragma once
#include <string>
#include <vector>

#include "core/Json.h"
#include "project/Model.h"

namespace kinetic::io {

Json toJson(const Project& p);
// Loads any supported schema version (migrating older ones). Throws JsonError on malformed input.
Project fromJson(const Json& j, std::vector<std::string>* warnings = nullptr);

// Building blocks (shared with presets)
Json clipToJson(const Clip& c);
Clip clipFromJson(const Json& j);
Json effectToJson(const EffectInstance& e);
EffectInstance effectFromJson(const Json& j);
Json shakeToJson(const CameraShake& s);
CameraShake shakeFromJson(const Json& j);
Json colorToJson(const Color& c);
Color colorFromJson(const Json& j, const Color& def);

// Upgrades a project JSON in place to Project::kSchemaVersion. Returns the version it started at.
int migrate(Json& j, std::vector<std::string>* notes = nullptr);

} // namespace kinetic::io
