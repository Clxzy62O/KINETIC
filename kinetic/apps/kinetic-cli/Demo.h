#pragma once
#include <string>

namespace kinetic::cli {

// Generates synthetic source media + a synthesized track, builds a full AMV-style project with the
// engine API (beat detection, beat-distributed cuts, velocity ramps, shake, glow, RGB split,
// flashes, speed lines, nested impact comp, colour grade), saves it and renders it.
int runDemo(const std::string& dir, double seconds, int width, int height, bool render);

// Synthesizes a short electronic track (intro -> build -> drop) for testing beat detection.
void synthesizeTrack(const std::string& wavPath, double bpm, double seconds, double dropAt, int sampleRate = 48000);

} // namespace kinetic::cli
