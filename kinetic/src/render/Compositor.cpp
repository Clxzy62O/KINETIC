#include "render/Compositor.h"

#include <algorithm>
#include <cmath>

namespace kinetic::render {

Affine Affine::rotateDeg(double deg) {
    double r = deg * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
    return {c, -s, s, c, 0, 0};
}
Affine Affine::operator*(const Affine& o) const {
    return {a * o.a + b * o.c, a * o.b + b * o.d, c * o.a + d * o.c, c * o.b + d * o.d,
            a * o.tx + b * o.ty + tx, c * o.tx + d * o.ty + ty};
}
Affine Affine::inverse() const {
    double det = a * d - b * c;
    if (std::fabs(det) < 1e-12) return {0, 0, 0, 0, 0, 0};
    double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
    return {ia, ib, ic, id, -(ia * tx + ib * ty), -(ic * tx + id * ty)};
}
bool Affine::isIdentity(double eps) const {
    return std::fabs(a - 1) < eps && std::fabs(b) < eps && std::fabs(c) < eps && std::fabs(d - 1) < eps &&
           std::fabs(tx) < eps && std::fabs(ty) < eps;
}

float blendChannel(BlendMode mode, float cb, float cs) {
    switch (mode) {
    case BlendMode::Normal: return cs;
    case BlendMode::Add: return cb + cs;
    case BlendMode::Screen: return cb + cs - cb * cs;
    case BlendMode::Multiply: return cb * cs;
    case BlendMode::Overlay: return cb <= 0.5f ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    case BlendMode::HardLight: return cs <= 0.5f ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    case BlendMode::SoftLight: {
        if (cs <= 0.5f) return cb - (1 - 2 * cs) * cb * (1 - cb);
        float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(std::max(cb, 0.f));
        return cb + (2 * cs - 1) * (d - cb);
    }
    case BlendMode::Difference: return std::fabs(cb - cs);
    case BlendMode::Lighten: return std::max(cb, cs);
    case BlendMode::Darken: return std::min(cb, cs);
    }
    return cs;
}

static inline void blendPixel(float* d, const float* s, BlendMode mode, float opacity) {
    float as = s[3] * opacity;
    if (as <= 0.f) return;
    if (mode == BlendMode::Normal) {
        float k = 1.f - as;
        d[0] = s[0] * opacity + d[0] * k;
        d[1] = s[1] * opacity + d[1] * k;
        d[2] = s[2] * opacity + d[2] * k;
        d[3] = as + d[3] * k;
        return;
    }
    if (mode == BlendMode::Add) {  // linear-light additive (light, flashes, glows)
        d[0] += s[0] * opacity;
        d[1] += s[1] * opacity;
        d[2] += s[2] * opacity;
        d[3] = std::min(1.f, d[3] + as);
        return;
    }
    // W3C compositing: Co = cs*(1-ab) + cb*(1-as) + as*ab*B(cb, cs) on premultiplied values.
    float ab = d[3];
    for (int c = 0; c < 3; ++c) {
        float csp = s[c] * opacity;
        float cbS = ab > 1e-6f ? d[c] / ab : 0.f;
        float csS = s[3] > 1e-6f ? s[c] / s[3] : 0.f;
        d[c] = csp * (1 - ab) + d[c] * (1 - as) + as * ab * blendChannel(mode, cbS, csS);
    }
    d[3] = as + ab * (1 - as);
}

void blend(ImageF& dst, const ImageF& src, BlendMode mode, float opacity) {
    if (opacity <= 0.f) return;
    parallelRows(dst.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* d = dst.row(y);
            const float* s = src.row(y);
            for (int x = 0; x < dst.width; ++x, d += 4, s += 4) blendPixel(d, s, mode, opacity);
        }
    });
}

void drawTransformed(ImageF& dst, const ImageF& layer, const Affine& m, BlendMode mode, float opacity) {
    if (opacity <= 0.f || layer.empty()) return;
    // Fast path: pure integer translation.
    if (std::fabs(m.a - 1) < 1e-9 && std::fabs(m.d - 1) < 1e-9 && std::fabs(m.b) < 1e-9 && std::fabs(m.c) < 1e-9 &&
        std::fabs(m.tx - std::round(m.tx)) < 1e-6 && std::fabs(m.ty - std::round(m.ty)) < 1e-6) {
        int ox = static_cast<int>(std::lround(m.tx)), oy = static_cast<int>(std::lround(m.ty));
        parallelRows(dst.height, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                int sy = y - oy;
                if (sy < 0 || sy >= layer.height) continue;
                for (int x = std::max(0, ox); x < std::min(dst.width, ox + layer.width); ++x)
                    blendPixel(dst.at(x, y), layer.at(x - ox, sy), mode, opacity);
            }
        });
        return;
    }
    Affine inv = m.inverse();
    // Destination bounding box of the layer.
    double xs[4] = {0, static_cast<double>(layer.width), 0, static_cast<double>(layer.width)};
    double ys[4] = {0, 0, static_cast<double>(layer.height), static_cast<double>(layer.height)};
    double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
    for (int i = 0; i < 4; ++i) {
        double X = m.a * xs[i] + m.b * ys[i] + m.tx, Y = m.c * xs[i] + m.d * ys[i] + m.ty;
        minX = std::min(minX, X); maxX = std::max(maxX, X); minY = std::min(minY, Y); maxY = std::max(maxY, Y);
    }
    int bx0 = std::max(0, static_cast<int>(std::floor(minX)) - 1), bx1 = std::min(dst.width, static_cast<int>(std::ceil(maxX)) + 1);
    int by0 = std::max(0, static_cast<int>(std::floor(minY)) - 1), by1 = std::min(dst.height, static_cast<int>(std::ceil(maxY)) + 1);
    if (bx0 >= bx1 || by0 >= by1) return;
    // Downscaling by more than 2x: pre-filter would be needed for quality; the evaluator decodes
    // media near the target size, so bilinear here is sufficient.
    parallelRows(by1 - by0, [&](int r0, int r1) {
        for (int y = by0 + r0; y < by0 + r1; ++y)
            for (int x = bx0; x < bx1; ++x) {
                double px = x + 0.5, py = y + 0.5;
                float sx = static_cast<float>(inv.a * px + inv.b * py + inv.tx);
                float sy = static_cast<float>(inv.c * px + inv.d * py + inv.ty);
                if (sx < -1 || sy < -1 || sx > layer.width + 1 || sy > layer.height + 1) continue;
                float s[4];
                sampleBilinear(layer, sx, sy, s);
                blendPixel(dst.at(x, y), s, mode, opacity);
            }
    });
}

} // namespace kinetic::render
