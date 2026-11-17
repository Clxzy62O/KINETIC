#pragma once
// Image buffers. Decoded media is RGBA8 sRGB (straight alpha). All compositing happens in
// linear-light, premultiplied RGBA float (ImageF) and is encoded back to sRGB on output.
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace kinetic {

struct Image8 {
    int width = 0, height = 0;
    std::vector<uint8_t> px;  // RGBA, row-major, tightly packed
    Image8() = default;
    Image8(int w, int h) : width(w), height(h), px(static_cast<size_t>(w) * h * 4, 0) {}
    size_t bytes() const { return px.size(); }
};
using Image8Ptr = std::shared_ptr<const Image8>;

struct ImageF {
    int width = 0, height = 0;
    std::vector<float> px;  // RGBA linear premultiplied
    ImageF() = default;
    ImageF(int w, int h) : width(w), height(h), px(static_cast<size_t>(w) * h * 4, 0.f) {}
    float* row(int y) { return px.data() + static_cast<size_t>(y) * width * 4; }
    const float* row(int y) const { return px.data() + static_cast<size_t>(y) * width * 4; }
    float* at(int x, int y) { return px.data() + (static_cast<size_t>(y) * width + x) * 4; }
    const float* at(int x, int y) const { return px.data() + (static_cast<size_t>(y) * width + x) * 4; }
    void fill(float r, float g, float b, float a);
    bool empty() const { return width == 0 || height == 0; }
};

float srgbToLinear(float c);
float linearToSrgb(float c);

// RGBA8 sRGB straight -> linear premultiplied float.
ImageF toLinear(const Image8& in);
// Linear premultiplied float -> RGBA8 sRGB straight (with ordered dither off; deterministic).
Image8 toSrgb8(const ImageF& in);

// Bilinear sample (premultiplied, clamp-to-transparent outside).
void sampleBilinear(const ImageF& img, float x, float y, float out[4]);

// Deterministic parallel-for over rows (each row processed exactly once, order-independent ops only).
void parallelRows(int height, const std::function<void(int y0, int y1)>& fn);
int workerCount();

} // namespace kinetic
