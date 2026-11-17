#pragma once
#include <complex>
#include <vector>

namespace kinetic::audio {

// In-place iterative radix-2 FFT. size must be a power of two.
void fft(std::vector<std::complex<float>>& a);

// Magnitude spectra (n/2+1 bins) of Hann-windowed frames.
class Stft {
public:
    Stft(int frameSize, int hop);
    int frameSize() const { return n_; }
    int hop() const { return hop_; }
    int bins() const { return n_ / 2 + 1; }
    // Returns magnitudes [frames][bins] (flattened row-major) for the whole signal.
    std::vector<float> magnitudes(const std::vector<float>& signal, int& frames) const;

private:
    int n_, hop_;
    std::vector<float> window_;
};

} // namespace kinetic::audio
