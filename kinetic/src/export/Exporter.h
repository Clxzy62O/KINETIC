#pragma once
// Export: renders a composition through the same Renderer as preview and pipes frames to an
// encoder. Audio tracks are mixed (with velocity-aware varispeed) and muxed.
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "media/AudioDecode.h"
#include "media/Decoder.h"
#include "project/Model.h"

namespace kinetic::exporter {

enum class Quality { Draft, Balanced, High, Maximum };
const char* toString(Quality);
bool parseQuality(const std::string&, Quality&);

struct ExportSettings {
    std::string outputPath;          // .mp4 .mov .webm .mkv .wav, or an image pattern like out_%05d.png
    Quality quality = Quality::High; // encoder effort/bitrate only; never changes effects or resolution
    std::string videoCodec = "auto"; // auto | libx264 | libopenh264 | mpeg4 | libvpx-vp9 | prores_ks | <any ffmpeg encoder>
    int width = 0, height = 0;       // 0 = composition size (export scales the composition, never effects)
    int64_t startFrame = 0, endFrame = -1;  // [start, end) ; -1 = composition end
    int audioBitrateK = 320;
    int sampleRate = 48000;
    bool includeAudio = true;
};

struct Progress {
    int64_t done = 0, total = 0;
    double framesPerSecond = 0, etaSeconds = 0;
};

struct ExportResult {
    std::string outputPath;
    int64_t frames = 0;
    double seconds = 0;
    std::string encoder;
    std::vector<std::string> warnings;
};

struct ExportError : std::runtime_error { using std::runtime_error::runtime_error; };

// Mixes the composition's audio tracks for [startFrame, endFrame).
media::AudioBuffer mixAudio(const Project& p, const Composition& c, int64_t startFrame, int64_t endFrame, int sampleRate,
                            std::vector<std::string>* warnings);

// Picks the encoder for a container given what the local FFmpeg provides.
std::string chooseEncoder(const std::string& container, const std::string& requested);

ExportResult exportComposition(const Project& p, Id compId, const ExportSettings& s, media::FrameProvider& frames,
                               const std::function<void(const Progress&)>& onProgress = {},
                               const std::atomic<bool>* cancel = nullptr);

} // namespace kinetic::exporter
