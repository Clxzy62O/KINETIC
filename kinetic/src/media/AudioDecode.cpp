#include "media/AudioDecode.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "media/Process.h"

namespace kinetic::media {

std::vector<float> AudioBuffer::mono() const {
    std::vector<float> m(frames());
    for (size_t i = 0; i < m.size(); ++i) {
        float s = 0;
        for (int c = 0; c < channels; ++c) s += samples[i * channels + c];
        m[i] = s / static_cast<float>(channels);
    }
    return m;
}

AudioBuffer decodeAudio(const std::string& path, int rate, int channels) {
    Subprocess p({ffmpegPath(), "-hide_banner", "-loglevel", "error", "-nostdin", "-i", path, "-map", "0:a:0", "-vn",
                  "-ac", std::to_string(channels), "-ar", std::to_string(rate), "-f", "f32le", "pipe:1"},
                 Subprocess::Stdout);
    std::string raw = p.readAll();
    int rc = p.wait();
    if (rc != 0) throw ProcessError("audio decode failed for '" + path + "': " + p.stderrText());
    AudioBuffer b;
    b.sampleRate = rate;
    b.channels = channels;
    b.samples.resize(raw.size() / sizeof(float));
    std::memcpy(b.samples.data(), raw.data(), b.samples.size() * sizeof(float));
    return b;
}

std::vector<std::pair<float, float>> waveformPeaks(const std::vector<float>& mono, size_t buckets) {
    std::vector<std::pair<float, float>> out;
    if (mono.empty() || buckets == 0) return out;
    out.reserve(buckets);
    for (size_t b = 0; b < buckets; ++b) {
        size_t s = b * mono.size() / buckets, e = std::max(s + 1, (b + 1) * mono.size() / buckets);
        float lo = 0, hi = 0;
        for (size_t i = s; i < e && i < mono.size(); ++i) { lo = std::min(lo, mono[i]); hi = std::max(hi, mono[i]); }
        out.emplace_back(lo, hi);
    }
    return out;
}

void writeWavF32(const std::string& path, const AudioBuffer& buf) {
    std::ofstream o(path, std::ios::binary);
    if (!o) throw std::runtime_error("cannot write " + path);
    auto u32 = [&](uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
    uint32_t dataBytes = static_cast<uint32_t>(buf.samples.size() * 4);
    o.write("RIFF", 4); u32(36 + dataBytes); o.write("WAVE", 4);
    o.write("fmt ", 4); u32(16); u16(3 /*IEEE float*/); u16(static_cast<uint16_t>(buf.channels));
    u32(static_cast<uint32_t>(buf.sampleRate)); u32(static_cast<uint32_t>(buf.sampleRate * buf.channels * 4));
    u16(static_cast<uint16_t>(buf.channels * 4)); u16(32);
    o.write("data", 4); u32(dataBytes);
    o.write(reinterpret_cast<const char*>(buf.samples.data()), dataBytes);
    if (!o) throw std::runtime_error("write failed: " + path);
}

} // namespace kinetic::media
