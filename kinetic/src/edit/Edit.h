#pragma once
// Edit operations. Each function inspects the project (read-only) and returns an undoable command
// to be pushed on the CommandStack. Invalid edits throw CommandError and change nothing.
#include <optional>
#include <vector>

#include "edit/Commands.h"

namespace kinetic::edit {

// True if [start, start+dur) is free on the track (ignoring clips listed in `ignore`).
bool rangeFree(const Track& t, int64_t start, int64_t dur, const std::vector<Id>& ignore = {});

CommandPtr addClip(const Project& p, Id trackId, Clip clip);              // clip.id must be set (use allocClipId)
CommandPtr deleteClip(const Project& p, Id clipId);
CommandPtr rippleDelete(const Project& p, Id clipId);                     // closes the gap on the same track
CommandPtr moveClip(const Project& p, Id clipId, int64_t newStart, Id newTrackId = kNoId);
CommandPtr trimStart(const Project& p, Id clipId, int64_t newStart);       // keeps the clip's end fixed
CommandPtr trimEnd(const Project& p, Id clipId, int64_t newEnd);           // keeps the clip's start fixed
CommandPtr rollEdit(const Project& p, Id leftClipId, Id rightClipId, int64_t newCut);
CommandPtr slip(const Project& p, Id clipId, double deltaSourceSeconds);
CommandPtr split(const Project& p, Id clipId, int64_t frame);
CommandPtr duplicate(const Project& p, Id clipId, int64_t newStart, Id trackId = kNoId);
CommandPtr setClip(const Project& p, const Clip& updated, const std::string& label, const std::string& mergeKey = "");

// Snapping: nearest edit point (clip edges, markers, playhead) within tolerance, else nullopt.
struct SnapResult { int64_t frame; std::string what; };
std::optional<SnapResult> snap(const Composition& c, int64_t frame, int64_t toleranceFrames,
                               const std::vector<Id>& ignoreClips = {}, std::optional<int64_t> playhead = std::nullopt);

// Shifts the time of all clip-local animation (transform, effects, speed, shake) by -dt seconds
// (used when the clip's local origin moves, e.g. trimStart / split).
void shiftLocalAnimation(Clip& c, double dt);

} // namespace kinetic::edit
