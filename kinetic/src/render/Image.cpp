#include "render/Image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <thread>

namespace kinetic {

void ImageF::fill(float r, float g, float b, float a) {
    for (size_t i = 0; i < px.size(); i += 4) { px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a; }
}

float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float linearToSrgb(float c) {
    if (c <= 0.f) return 0.f;
    if (c >= 1.f) return 1.f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

static const std::array<float, 256>& decodeLut() {
    static const std::array<float, 256> lut = [] {
        std::array<float, 256> l{};
        for (int i = 0; i < 256; ++i) l[i] = srgbToLinear(i / 255.f);
        return l;
    }();
    return lut;
}

// 4096-entry encode LUT over [0,1] with linear interpolation (accurate to < 0.5/255).
static const std::array<float, 4097>& encodeLut() {
    static const std::array<float, 4097> lut = [] {
        std::array<float, 4097> l{};
        for (int i = 0; i <= 4096; ++i) l[i] = linearToSrgb(i / 4096.f);
        return l;
    }();
    return lut;
}
static inline float encodeFast(float v) {
    if (v <= 0.f) return 0.f;
    if (v >= 1.f) return 1.f;
    // Low range is steep: use the exact function there.
    if (v < 0.0031308f * 4) return linearToSrgb(v);
    const auto& l = encodeLut();
    float f = v * 4096.f;
    int i = static_cast<int>(f);
    float t = f - i;
    return l[i] + (l[std::min(i + 1, 4096)] - l[i]) * t;
}

ImageF toLinear(const Image8& in) {
    ImageF out(in.width, in.height);
    const auto& lut = decodeLut();
    const size_t n = static_cast<size_t>(in.width) * in.height;
    parallelRows(in.height, [&](int y0, int y1) {
        for (size_t i = static_cast<size_t>(y0) * in.width; i < static_cast<size_t>(y1) * in.width && i < n; ++i) {
            const uint8_t* s = &in.px[i * 4];
            float a = s[3] / 255.f;
            float* d = &out.px[i * 4];
            d[0] = lut[s[0]] * a;
            d[1] = lut[s[1]] * a;
            d[2] = lut[s[2]] * a;
            d[3] = a;
        }
    });
    return out;
}

Image8 toSrgb8(const ImageF& in) {
    Image8 out(in.width, in.height);
    parallelRows(in.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* s = in.row(y);
            uint8_t* d = &out.px[static_cast<size_t>(y) * in.width * 4];
            for (int x = 0; x < in.width; ++x, s += 4, d += 4) {
                float a = std::clamp(s[3], 0.f, 1.f);
                float inv = a > 1e-6f ? 1.f / a : 0.f;
                for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(std::lround(encodeFast(s[c] * inv) * 255.f));
                d[3] = static_cast<uint8_t>(std::lround(a * 255.f));
            }
        }
    });
    return out;
}

void sampleBilinear(const ImageF& img, float x, float y, float out[4]) {
    // Pixel centres at integer + 0.5.
    float fx = x - 0.5f, fy = y - 0.5f;
    int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    float tx = fx - x0, ty = fy - y0;
    out[0] = out[1] = out[2] = out[3] = 0.f;
    const float w[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
    const int xs[4] = {x0, x0 + 1, x0, x0 + 1}, ys[4] = {y0, y0, y0 + 1, y0 + 1};
    for (int k = 0; k < 4; ++k) {
        if (w[k] == 0.f || xs[k] < 0 || ys[k] < 0 || xs[k] >= img.width || ys[k] >= img.height) continue;
        const float* p = img.at(xs[k], ys[k]);
        out[0] += p[0] * w[k];
        out[1] += p[1] * w[k];
        out[2] += p[2] * w[k];
        out[3] += p[3] * w[k];
    }
}

int workerCount() {
    static int n = [] {
        if (const char* e = std::getenv("KINETIC_THREADS")) return std::max(1, std::atoi(e));
        return static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    }();
    return n;
}

void parallelRows(int height, const std::function<void(int, int)>& fn) {
    int n = std::min(workerCount(), std::max(1, height / 16));
    if (n <= 1) { fn(0, height); return; }
    std::vector<std::thread> ts;
    int chunk = (height + n - 1) / n;
    for (int i = 0; i < n; ++i) {
        int y0 = i * chunk, y1 = std::min(height, y0 + chunk);
        if (y0 >= y1) break;
        ts.emplace_back(fn, y0, y1);
    }
    for (auto& t : ts) t.join();
}

} // namespace kinetic
