#include <cmath>
#include <random>

#include "Test.h"
#include "audio/BeatDetect.h"
#include "audio/Fft.h"

using namespace kinetic;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Kick on every beat (accented downbeats), hats on off-beats, light noise floor.
std::vector<float> clickTrack(double bpm, double seconds, int sr, double offset, bool hats, double loudFrom = -1) {
    std::vector<float> x(static_cast<size_t>(seconds * sr), 0.f);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> U(-1.f, 1.f);
    for (auto& v : x) v = 0.004f * U(rng);
    double beat = 60.0 / bpm;
    for (int b = 0; offset + b * beat < seconds; ++b) {
        double t = offset + b * beat;
        double gain = (b % 4 == 0 ? 1.0 : 0.55) * (loudFrom >= 0 && t < loudFrom ? 0.15 : 1.0);
        size_t s0 = static_cast<size_t>(t * sr);
        double ph = 0;
        for (size_t k = 0; k < static_cast<size_t>(0.2 * sr) && s0 + k < x.size(); ++k) {
            double u = static_cast<double>(k) / sr;
            ph += 2 * kPi * (50 + 120 * std::exp(-u * 30)) / sr;
            x[s0 + k] += static_cast<float>(0.8 * gain * std::sin(ph) * std::exp(-u * 12));
        }
        if (hats) {
            size_t h0 = static_cast<size_t>((t + beat / 2) * sr);
            for (size_t k = 0; k < static_cast<size_t>(0.03 * sr) && h0 + k < x.size(); ++k)
                x[h0 + k] += 0.15f * U(rng) * static_cast<float>(std::exp(-static_cast<double>(k) / sr * 120));
        }
    }
    return x;
}
} // namespace

TEST(fft_matches_dft) {
    std::vector<std::complex<float>> a(16);
    for (int i = 0; i < 16; ++i) a[i] = {static_cast<float>(std::sin(2 * kPi * 3 * i / 16.0)), 0.f};
    auto b = a;
    audio::fft(b);
    for (int k = 0; k < 16; ++k) {
        std::complex<double> s = 0;
        for (int n = 0; n < 16; ++n) s += std::complex<double>(a[n].real(), 0) * std::polar(1.0, -2 * kPi * k * n / 16.0);
        CHECK_NEAR(b[k].real(), s.real(), 1e-4);
        CHECK_NEAR(b[k].imag(), s.imag(), 1e-4);
    }
}

TEST(beats_tempo_and_alignment_on_click_track) {
    const int sr = 44100;
    for (double bpm : {90.0, 120.0, 128.0, 150.0}) {
        double offset = 0.37;
        auto x = clickTrack(bpm, 20, sr, offset, true);
        auto a = audio::analyzeBeats(x, sr);
        CHECK_NEAR(a.tempo, bpm, 1.5);
        CHECK(a.beats.size() > 20);
        double beat = 60.0 / bpm, worst = 0;
        int strongOnDownbeat = 0, strongTotal = 0;
        for (auto& b : a.beats) {
            double k = std::round((b.time - offset) / beat);
            worst = std::max(worst, std::fabs(b.time - (offset + k * beat)));
            if (b.strong) { ++strongTotal; strongOnDownbeat += (static_cast<long>(k) % 4 == 0); }
        }
        CHECK(worst < 0.015);  // every beat within 15 ms (under one frame at 60 fps)
        CHECK(strongTotal > 0);
        CHECK(strongOnDownbeat >= strongTotal * 0.9);
    }
}

TEST(beats_onsets_are_banded) {
    auto x = clickTrack(120, 10, 44100, 0.25, true);
    auto a = audio::analyzeBeats(x, 44100);
    int kicks = 0, hats = 0;
    for (auto& o : a.onsets) { kicks += o.band == audio::OnsetBand::Kick; hats += o.band == audio::OnsetBand::Hat; }
    CHECK(kicks >= 15);
    CHECK(hats >= 10);
}

TEST(beats_detects_energy_drop) {
    auto x = clickTrack(128, 24, 44100, 0.2, true, 12.0);
    auto a = audio::analyzeBeats(x, 44100);
    bool found = false;
    // Loud section starts at 12 s; the first loud kick is beat 26 = 0.2 + 26 * 60/128 = 12.3875 s.
    for (auto& e : a.events) if (e.second == "drop" && std::fabs(e.first - 12.3875) < 0.03) found = true;
    CHECK(found);
    CHECK(!a.sections.empty());
    CHECK(a.sections.front().label == "low");
    CHECK(a.sections.back().label == "high");
}

TEST(beats_silence_is_safe) {
    std::vector<float> x(44100 * 3, 0.f);
    auto a = audio::analyzeBeats(x, 44100);
    CHECK(a.beats.empty());
    CHECK(a.onsets.empty());
    auto b = audio::analyzeBeats({}, 44100);
    CHECK(b.tempo == 0);
}
