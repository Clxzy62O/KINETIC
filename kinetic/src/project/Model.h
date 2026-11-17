#pragma once
// Pure project data. No UI state, no rendering, no file IO. All mutation goes through edit/Commands.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "anim/Keyframes.h"
#include "core/Rational.h"

namespace kinetic {

using Id = int64_t;
constexpr Id kNoId = 0;

using Color = std::array<double, 4>;  // straight (non-premultiplied) sRGB RGBA, 0..1

enum class TrackKind { Video, Audio };
enum class ClipKind { Media, Composition, Solid, Adjustment };
enum class BlendMode { Normal, Add, Screen, Multiply, Overlay, SoftLight, HardLight, Difference, Lighten, Darken };
enum class TimeInterp { Sample, Blend, OpticalFlow };
enum class FitMode { None, Fit, Fill };
enum class MarkerKind { Manual, Beat, StrongBeat, Section };

const char* toString(TrackKind);
const char* toString(ClipKind);
const char* toString(BlendMode);
const char* toString(TimeInterp);
const char* toString(FitMode);
const char* toString(MarkerKind);
bool parseEnum(const std::string&, TrackKind&);
bool parseEnum(const std::string&, ClipKind&);
bool parseEnum(const std::string&, BlendMode&);
bool parseEnum(const std::string&, TimeInterp&);
bool parseEnum(const std::string&, FitMode&);
bool parseEnum(const std::string&, MarkerKind&);

struct MediaRef {
    Id id = kNoId;
    std::string path;
    std::string name;
    // Probe results (cached in the project so a missing file can still be described).
    bool hasVideo = false, hasAudio = false;
    int width = 0, height = 0;
    Rational fps{24, 1};
    double duration = 0;
    std::string videoCodec, audioCodec, pixFmt, colorSpace;
    int sampleRate = 0, channels = 0;
    // Fingerprint for missing-media detection / relinking.
    int64_t fileSize = 0;
    int64_t mtime = 0;
    bool isStill = false;
};

struct EffectInstance {
    std::string defId;      // e.g. "kinetic.glow"
    int defVersion = 1;
    bool enabled = true;
    double mix = 1.0;       // effect intensity: blend between input and output
    std::vector<std::pair<std::string, AnimCurve>> params;

    AnimCurve* param(const std::string& id);
    const AnimCurve* param(const std::string& id) const;
    AnimCurve& paramOrAdd(const std::string& id, double def);
    double eval(const std::string& id, double t, double def) const;
};

struct Transform {
    // Positions are comp pixels relative to comp centre; anchor is layer pixels relative to layer centre.
    AnimCurve posX{0}, posY{0};
    AnimCurve anchorX{0}, anchorY{0};
    AnimCurve scale{1.0};            // uniform multiplier on top of fit
    AnimCurve scaleX{1.0}, scaleY{1.0};
    AnimCurve rotation{0};           // degrees, clockwise
    AnimCurve opacity{1.0};
    FitMode fit = FitMode::Fit;
};

struct CameraShake {
    bool enabled = false;
    AnimCurve amplitude{0};   // pixels
    double frequency = 12;    // Hz
    double rotation = 0;      // degrees at amplitude 1 (scaled by amplitude/100)
    double decay = 0;         // per second exponential decay from each key... (0 = none)
    double randomness = 1.0;  // 0 = pure sine, 1 = pure noise
    double directionDeg = 0;  // dominant direction when directional > 0
    double directional = 0;   // 0 = omni, 1 = only along direction
    double zoom = 0;          // scale jitter (fraction) at amplitude 100
    int64_t seed = 1;
};

struct Clip {
    Id id = kNoId;
    std::string name;
    ClipKind kind = ClipKind::Media;
    Id mediaId = kNoId;
    Id compositionId = kNoId;
    Color solidColor{1, 1, 1, 1};

    int64_t start = 0;      // composition frames
    int64_t duration = 0;   // composition frames
    double sourceIn = 0;    // seconds into the source at local time 0

    AnimCurve speed{1.0};   // velocity curve over clip-local output seconds
    TimeInterp timeInterp = TimeInterp::Sample;

    Transform transform;
    CameraShake shake;
    BlendMode blend = BlendMode::Normal;
    std::vector<EffectInstance> effects;
    bool effectsBypassed = false;
    bool enabled = true;
    double gainDb = 0;      // audio clips

    int64_t end() const { return start + duration; }
    bool contains(int64_t frame) const { return frame >= start && frame < end(); }
};

struct Track {
    Id id = kNoId;
    TrackKind kind = TrackKind::Video;
    std::string name;
    bool hidden = false, locked = false, muted = false, solo = false;
    std::string color = "#A78BFA";
    std::vector<Clip> clips;  // sorted by start, non-overlapping

    const Clip* clipAt(int64_t frame) const;
};

struct Marker {
    Id id = kNoId;
    int64_t frame = 0;
    MarkerKind kind = MarkerKind::Manual;
    std::string color = "#38BDF8";
    std::string label;
    std::string group;
    double strength = 1.0;
};

struct Composition {
    Id id = kNoId;
    std::string name;
    int width = 1920, height = 1080;
    Rational fps{60, 1};
    int64_t durationFrames = 600;
    Color background{0, 0, 0, 1};
    std::vector<Track> tracks;  // index 0 = bottom
    std::vector<Marker> markers;

    double durationSeconds() const { return framesToSeconds(durationFrames, fps); }
};

struct ProjectSettings {
    std::string name = "Untitled";
    int autosaveIntervalSec = 120;
    std::string colorProfile = "sRGB/Rec.709";
};

struct ClipLocation {
    Composition* comp = nullptr;
    Track* track = nullptr;
    size_t index = 0;
    Clip* clip() const { return track ? &track->clips[index] : nullptr; }
};

class Project {
public:
    static constexpr int kSchemaVersion = 2;

    ProjectSettings settings;
    std::vector<MediaRef> media;
    std::vector<Composition> compositions;
    Id rootCompositionId = kNoId;
    Id nextId = 1;

    Id allocId() { return nextId++; }

    MediaRef* findMedia(Id id);
    const MediaRef* findMedia(Id id) const;
    Composition* findComposition(Id id);
    const Composition* findComposition(Id id) const;
    Composition* rootComposition() { return findComposition(rootCompositionId); }
    Track* findTrack(Id id, Composition** owner = nullptr);
    const Track* findTrack(Id id, const Composition** owner = nullptr) const;
    std::optional<ClipLocation> findClip(Id id);
    const Clip* findClipConst(Id id) const;

    // Returns human-readable problems; empty means valid.
    std::vector<std::string> validate() const;
    // True if comp `outer` (transitively) references comp `inner`.
    bool compositionReferences(Id outer, Id inner) const;
};

// Helpers for building projects programmatically (CLI, tests, templates).
Composition makeComposition(Project& p, const std::string& name, int w, int h, Rational fps, int64_t frames);
Track& addTrack(Project& p, Composition& c, TrackKind kind, const std::string& name);

} // namespace kinetic
