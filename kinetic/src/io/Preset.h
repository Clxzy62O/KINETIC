#pragma once
// Versioned presets. Applying a preset deep-copies its content into the target clip, so the
// result is ordinary, fully editable effects/keys. Missing effects are reported, never dropped.
#include <functional>
#include <string>
#include <vector>

#include "core/Json.h"
#include "project/Model.h"

namespace kinetic::io {

constexpr int kPresetSchemaVersion = 1;

using EffectAvailableFn = std::function<bool(const std::string& id, int version)>;

struct PresetReport {
    std::vector<std::string> missingEffects;  // "id@version"
    std::vector<std::string> notes;
    bool ok() const { return missingEffects.empty(); }
};

Json makeEffectStackPreset(const std::string& name, const std::string& category, const std::vector<EffectInstance>& fx);
Json makeVelocityPreset(const std::string& name, const AnimCurve& speed, double clipDurationSec);
Json makeShakePreset(const std::string& name, const CameraShake& shake);

// Inspect without applying (for the preset browser's compatibility badge).
PresetReport checkPreset(const Json& preset, const EffectAvailableFn& available);

// Returns the modified clip. Velocity presets are time-scaled to the target clip duration.
// If `allowMissing` is false and effects are missing, throws std::runtime_error with the report.
Clip applyPreset(const Json& preset, const Clip& target, double targetDurationSec, const EffectAvailableFn& available,
                 PresetReport& report, bool allowMissing = true);

} // namespace kinetic::io
