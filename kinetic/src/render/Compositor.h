#pragma once
#include "project/Model.h"
#include "render/Image.h"

namespace kinetic::render {

// 2D affine: x' = a*x + b*y + tx ; y' = c*x + d*y + ty
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    static Affine translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static Affine scale(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
    static Affine rotateDeg(double deg);
    Affine operator*(const Affine& o) const;  // this ∘ o (apply o first)
    Affine inverse() const;
    bool isIdentity(double eps = 1e-9) const;
};

// Composites `src` (already in destination space, same size) over `dst` with blend mode & opacity.
void blend(ImageF& dst, const ImageF& src, BlendMode mode, float opacity);

// Draws `layer` into `dst` through `m` (layer pixel -> dst pixel), bilinear, then blends.
void drawTransformed(ImageF& dst, const ImageF& layer, const Affine& m, BlendMode mode, float opacity);

// Per-channel blend function on straight (unpremultiplied) colours.
float blendChannel(BlendMode mode, float cb, float cs);

} // namespace kinetic::render
