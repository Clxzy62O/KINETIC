#pragma once
// Audio analysis for beat sync: onsets (with kick/snare/hat banding), tempo, a beat grid from
// dynamic-programming beat tracking, downbeat/accent marking, an energy curve and coarse sections.
// Everything returned is a suggestion: the UI turns it into editable markers.
#include <string>
#include <vector>

#include "core/Json.h"

namespace kinetic::audio {

enum class OnsetBand { Kick, Snare, Hat, Other };
const char* toString(OnsetBand);

struct Onset { double time; double strength; OnsetBand band; };
struct Beat { double time; double strength; bool strong; int barPosition; };  // barPosition 0..3 (0 = downbeat)
struct Section { double start, end; std::string label; double energy; };     // label: intro/low/build/drop/high/outro

struct BeatOptions {
    double minBpm = 60, maxBpm = 200;
    double preferredBpm = 120;     // tempo prior centre (octave-error control)
    double onsetSensitivity = 1.0; // >1 finds more onsets
    double tightness = 100;        // beat-tracker adherence to the tempo grid
};

struct BeatAnalysis {
    double tempo = 0;          // BPM
    double tempoConfidence = 0;
    std::vector<Beat> beats;
    std::vector<Onset> onsets;
    std::vector<float> energy;  // RMS (dBFS-normalised 0..1) per hop
    std::vector<float> onsetEnvelope;
    double hopSeconds = 0;
    std::vector<Section> sections;
    std::vector<std::pair<double, std::string>> events;  // "drop" / "buildStart" times

    Json toJson() const;
};

BeatAnalysis analyzeBeats(const std::vector<float>& mono, int sampleRate, const BeatOptions& opt = {});

} // namespace kinetic::audio
