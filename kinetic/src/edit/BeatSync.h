#pragma once
// Beat-based editing. Every function returns a *plan*: an undoable command plus a human-readable
// summary that the UI shows for review. Nothing touches the project until the plan is pushed.
#include <string>
#include <vector>

#include "edit/Commands.h"

namespace kinetic::beatsync {

struct BeatPoint {
    int64_t frame = 0;
    double strength = 1.0;  // 0..1 normalised onset strength
    bool strong = false;    // downbeat / accent
};

struct EditPlan {
    CommandPtr command;            // null when there is nothing to do
    std::vector<std::string> summary;
    int changes = 0;
};

std::vector<BeatPoint> filter(const std::vector<BeatPoint>& beats, bool strongOnly, int64_t from = INT64_MIN, int64_t to = INT64_MAX);

// Adds beat markers to a composition (skips frames that already have a beat marker).
EditPlan planBeatMarkers(const Project& p, Id compId, const std::vector<BeatPoint>& beats);
// Splits a clip at every (strong) beat inside it.
EditPlan planCutsAtBeats(const Project& p, Id clipId, const std::vector<BeatPoint>& beats, bool strongOnly);
// Lays the given clips out back-to-back on `trackId`, one per beat interval, starting at the first
// beat >= `fromFrame`. Clips are trimmed (end) to the interval; clips shorter than it leave a gap.
EditPlan planDistributeToBeats(const Project& p, Id trackId, const std::vector<Id>& clipIds,
                               const std::vector<BeatPoint>& beats, bool strongOnly, int64_t fromFrame = 0);
// Adds a solid flash clip on `trackId` at each beat, opacity keyed peak -> 0 over `flashFrames`.
EditPlan planFlashesOnBeats(const Project& p, Id trackId, const std::vector<BeatPoint>& beats, int flashFrames,
                            const Color& color, double peakOpacity, bool strongOnly, BlendMode blend = BlendMode::Add);
// Keys an effect parameter to pulse (base -> peak*strength -> base) at each beat inside the clip.
EditPlan planParamPulseOnBeats(const Project& p, Id clipId, size_t effectIndex, const std::string& paramId,
                               const std::vector<BeatPoint>& beats, double base, double peak, double decaySec,
                               bool strongOnly);
// Keys camera-shake amplitude to spike on beats.
EditPlan planShakeOnBeats(const Project& p, Id clipId, const std::vector<BeatPoint>& beats, double amplitude,
                          double decaySec, bool strongOnly);

} // namespace kinetic::beatsync
