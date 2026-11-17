#pragma once
// Velocity engine: speed curve -> source time mapping, presets, display sampling.
#include <string>
#include <vector>

#include "project/Model.h"

namespace kinetic {

enum class VelocityPreset {
    Constant,         // flat at `intensity`
    SmoothSlowdown,   // 1 -> slow, eased
    FastAcceleration, // slow -> fast
    ImpactRamp,       // fast -> near-freeze at impact point -> fast
    ReverseRamp,      // forward, decelerate through 0 into reverse
    AnimeVelocity,    // classic AMV: fast, hard slow at beat, recover
    HyperVelocity,    // aggressive multi-ramp
    SlowMotion,
    FastMotion,
    BeatDropRamp,     // build-up acceleration, hold slow after the drop
    SmoothInOut,      // slow at both ends, fast in the middle
};

const char* velocityPresetName(VelocityPreset);
bool velocityPresetFromName(const std::string&, VelocityPreset&);
std::vector<VelocityPreset> allVelocityPresets();

namespace Velocity {

// Source time (seconds) shown at clip-local output time `localT` (seconds).
double sourceTime(const Clip& clip, double localT);
// Source seconds consumed by the whole clip (end source time - sourceIn). Negative when net reverse.
double sourceSpan(const Clip& clip, const Rational& compFps);
// Min/max source time touched over the clip; used to validate against media duration.
void sourceRange(const Clip& clip, const Rational& compFps, double& minSrc, double& maxSrc);

// Writes keys into `speed` (clears existing keys). `duration` = clip length (s);
// `intensity` scales the effect (1 = default); `pivot` in [0,1] = impact point as fraction of clip.
void applyPreset(AnimCurve& speed, VelocityPreset preset, double duration, double intensity = 1.0, double pivot = 0.5);

// Speed sampled uniformly for drawing the coloured speed overlay on a clip.
std::vector<double> sampleForDisplay(const Clip& clip, double durationSec, int samples);

} // namespace Velocity
} // namespace kinetic
