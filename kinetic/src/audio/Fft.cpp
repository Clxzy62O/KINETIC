#include "audio/Fft.h"

#include <cmath>
#include <stdexcept>

namespace kinetic::audio {

void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    if (n == 0 || (n & (n - 1))) throw std::invalid_argument("fft size must be a power of two");
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        std::complex<float> wl(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (size_t k = 0; k < len / 2; ++k) {
                auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

Stft::Stft(int frameSize, int hop) : n_(frameSize), hop_(hop), window_(static_cast<size_t>(frameSize)) {
    for (int i = 0; i < n_; ++i) window_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / n_));
}

std::vector<float> Stft::magnitudes(const std::vector<float>& x, int& frames) const {
    frames = x.size() < static_cast<size_t>(n_) ? 1 : static_cast<int>((x.size() - n_) / hop_) + 1;
    std::vector<float> out(static_cast<size_t>(frames) * bins());
    std::vector<std::complex<float>> buf(static_cast<size_t>(n_));
    for (int f = 0; f < frames; ++f) {
        size_t off = static_cast<size_t>(f) * hop_;
        for (int i = 0; i < n_; ++i) {
            size_t idx = off + i;
            buf[static_cast<size_t>(i)] = {idx < x.size() ? x[idx] * window_[static_cast<size_t>(i)] : 0.f, 0.f};
        }
        fft(buf);
        float* row = &out[static_cast<size_t>(f) * bins()];
        for (int b = 0; b < bins(); ++b) row[b] = std::abs(buf[static_cast<size_t>(b)]);
    }
    return out;
}

} // namespace kinetic::audio
