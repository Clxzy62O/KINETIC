#pragma once
#include <string>
#include <vector>

namespace kinetic::media {

struct AudioBuffer {
    int sampleRate = 48000;
    int channels = 2;
    std::vector<float> samples;  // interleaved
    size_t frames() const { return channels ? samples.size() / static_cast<size_t>(channels) : 0; }
    double duration() const { return sampleRate ? static_cast<double>(frames()) / sampleRate : 0; }
    std::vector<float> mono() const;
};

// Decodes the first audio stream, resampled to `rate` and `channels`.
AudioBuffer decodeAudio(const std::string& path, int rate = 48000, int channels = 2);

// Min/max peak pairs per bucket for drawing waveforms.
std::vector<std::pair<float, float>> waveformPeaks(const std::vector<float>& mono, size_t buckets);

// Writes a 32-bit float WAV file.
void writeWavF32(const std::string& path, const AudioBuffer& buf);

} // namespace kinetic::media
