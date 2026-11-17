#include "render/Effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kinetic::render {

static constexpr double kPi = 3.14159265358979323846;

double Params::operator[](const std::string& id) const {
    for (auto& v : values) if (v.first == id) return v.second;
    return 0.0;
}

// ---------------------------------------------------------------- helpers
static inline float lum(const float* p) { return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; }

static inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
static inline float hashUnit(uint32_t a, uint32_t b, uint32_t c) {  // [0,1)
    return static_cast<float>(hash32(a * 0x9E3779B1u ^ hash32(b + 0x85EBCA77u) ^ hash32(c * 0xC2B2AE3Du)) >> 8) / 16777216.f;
}

static ImageF downsample2(const ImageF& in) {
    ImageF out((in.width + 1) / 2, (in.height + 1) / 2);
    parallelRows(out.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < out.width; ++x) {
                float acc[4] = {0, 0, 0, 0};
                int n = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        int sx = 2 * x + dx, sy = 2 * y + dy;
                        if (sx >= in.width || sy >= in.height) continue;
                        const float* p = in.at(sx, sy);
                        for (int c = 0; c < 4; ++c) acc[c] += p[c];
                        ++n;
                    }
                float* d = out.at(x, y);
                for (int c = 0; c < 4; ++c) d[c] = acc[c] / n;
            }
    });
    return out;
}

static void upsampleInto(const ImageF& small, ImageF& dst) {
    float sx = static_cast<float>(small.width) / dst.width, sy = static_cast<float>(small.height) / dst.height;
    parallelRows(dst.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < dst.width; ++x) {
                // Clamp sample position inside so edges don't fade to transparent.
                float fx = std::clamp((x + 0.5f) * sx, 0.5f, small.width - 0.5f);
                float fy = std::clamp((y + 0.5f) * sy, 0.5f, small.height - 0.5f);
                sampleBilinear(small, fx, fy, dst.at(x, y));
            }
    });
}

static void blurPass(const ImageF& src, ImageF& dst, const std::vector<float>& k, int radius, bool horizontal, bool repeat) {
    parallelRows(src.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < src.width; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (int i = -radius; i <= radius; ++i) {
                    int sx = horizontal ? x + i : x, sy = horizontal ? y : y + i;
                    if (repeat) { sx = std::clamp(sx, 0, src.width - 1); sy = std::clamp(sy, 0, src.height - 1); }
                    else if (sx < 0 || sy < 0 || sx >= src.width || sy >= src.height) continue;
                    const float* p = src.at(sx, sy);
                    float w = k[static_cast<size_t>(i + radius)];
                    acc[0] += p[0] * w; acc[1] += p[1] * w; acc[2] += p[2] * w; acc[3] += p[3] * w;
                }
                float* d = dst.at(x, y);
                d[0] = acc[0]; d[1] = acc[1]; d[2] = acc[2]; d[3] = acc[3];
            }
    });
}

void gaussianBlur(ImageF& img, double sigma, bool repeat) {
    if (sigma < 0.3 || img.empty()) return;
    if (sigma > 6.0 && img.width > 8 && img.height > 8) {
        // Pyramid: blur at half resolution (variance-matched), then upsample. Deterministic.
        ImageF small = downsample2(img);
        gaussianBlur(small, std::sqrt(std::max(0.0, sigma * sigma - 1.0)) / 2.0, repeat);
        upsampleInto(small, img);
        return;
    }
    int radius = static_cast<int>(std::ceil(sigma * 3.0));
    std::vector<float> k(static_cast<size_t>(2 * radius + 1));
    double sum = 0;
    for (int i = -radius; i <= radius; ++i) sum += k[static_cast<size_t>(i + radius)] = static_cast<float>(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (auto& w : k) w = static_cast<float>(w / sum);
    ImageF tmp(img.width, img.height);
    blurPass(img, tmp, k, radius, true, repeat);
    blurPass(tmp, img, k, radius, false, repeat);
}

// ---------------------------------------------------------------- effects
static void fxGaussian(ImageF& img, const Params& p, const EvalContext&) {
    gaussianBlur(img, p["radius"], p["repeatEdges"] > 0.5);
}

static void fxDirectional(ImageF& img, const Params& p, const EvalContext&) {
    double len = p["length"];
    if (len < 0.5) return;
    double a = p["angle"] * kPi / 180.0;
    float dx = static_cast<float>(std::cos(a)), dy = static_cast<float>(std::sin(a));
    int n = std::clamp(static_cast<int>(std::ceil(len)) + 1, 3, 64);
    ImageF src = img;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                float acc[4] = {0, 0, 0, 0}, s[4];
                for (int i = 0; i < n; ++i) {
                    float t = static_cast<float>((static_cast<double>(i) / (n - 1) - 0.5) * len);
                    sampleBilinear(src, x + 0.5f + dx * t, y + 0.5f + dy * t, s);
                    for (int c = 0; c < 4; ++c) acc[c] += s[c];
                }
                float* d = img.at(x, y);
                for (int c = 0; c < 4; ++c) d[c] = acc[c] / n;
            }
    });
}

static void fxZoomBlur(ImageF& img, const Params& p, const EvalContext&) {
    double amount = std::clamp(p["amount"], 0.0, 0.95);
    if (amount < 1e-3) return;
    float cx = static_cast<float>((0.5 + p["centerX"]) * img.width), cy = static_cast<float>((0.5 + p["centerY"]) * img.height);
    int n = std::clamp(static_cast<int>(p["samples"]), 4, 64);
    ImageF src = img;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                float acc[4] = {0, 0, 0, 0}, s[4];
                float px = x + 0.5f - cx, py = y + 0.5f - cy;
                for (int i = 0; i < n; ++i) {
                    float k = 1.f - static_cast<float>(amount) * static_cast<float>(i) / (n - 1);
                    sampleBilinear(src, cx + px * k, cy + py * k, s);
                    for (int c = 0; c < 4; ++c) acc[c] += s[c];
                }
                float* d = img.at(x, y);
                for (int c = 0; c < 4; ++c) d[c] = acc[c] / n;
            }
    });
}

static void fxGlow(ImageF& img, const Params& p, const EvalContext&) {
    double radius = p["radius"], intensity = p["intensity"];
    if (intensity <= 0 || radius < 0.3) return;
    float thr = srgbToLinear(static_cast<float>(std::clamp(p["threshold"], 0.0, 1.0)));
    float tint[3] = {srgbToLinear(static_cast<float>(p["tintR"])), srgbToLinear(static_cast<float>(p["tintG"])),
                     srgbToLinear(static_cast<float>(p["tintB"]))};
    ImageF bright(img.width, img.height);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                const float* s = img.at(x, y);
                float a = s[3];
                if (a <= 1e-5f) continue;
                float L = lum(s) / a;  // unpremultiplied luminance
                float k = L > thr ? (L - thr) / std::max(L, 1e-5f) : 0.f;
                float* d = bright.at(x, y);
                for (int c = 0; c < 3; ++c) d[c] = s[c] * k * tint[c];
                d[3] = std::min(1.f, lum(d));
            }
    });
    int octaves = std::clamp(static_cast<int>(std::lround(p["spread"])), 1, 5);
    ImageF acc(img.width, img.height);
    double r = radius;
    for (int o = 0; o < octaves; ++o, r *= 2.0) {
        ImageF layer = bright;
        gaussianBlur(layer, r, false);
        float w = 1.f / static_cast<float>(octaves);
        for (size_t i = 0; i < acc.px.size(); ++i) acc.px[i] += layer.px[i] * w;
    }
    float gain = static_cast<float>(intensity);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                float* d = img.at(x, y);
                const float* g = acc.at(x, y);
                for (int c = 0; c < 3; ++c) d[c] += g[c] * gain;
                d[3] = std::min(1.f, d[3] + (1.f - d[3]) * std::min(1.f, g[3] * gain));
            }
    });
}

static void fxRgbSplit(ImageF& img, const Params& p, const EvalContext&) {
    double amt = p["amount"];
    if (std::fabs(amt) < 0.05) return;
    double a = p["angle"] * kPi / 180.0;
    float ox = static_cast<float>(std::cos(a) * amt), oy = static_cast<float>(std::sin(a) * amt);
    ImageF src = img;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                float r[4], b[4];
                sampleBilinear(src, x + 0.5f + ox, y + 0.5f + oy, r);
                sampleBilinear(src, x + 0.5f - ox, y + 0.5f - oy, b);
                const float* g = src.at(x, y);
                float* d = img.at(x, y);
                d[0] = r[0];
                d[1] = g[1];
                d[2] = b[2];
                d[3] = std::max({r[3], g[3], b[3]});
            }
    });
}

static void fxFlash(ImageF& img, const Params& p, const EvalContext&) {
    float amt = static_cast<float>(std::clamp(p["amount"], 0.0, 1.0));
    if (amt <= 0) return;
    float col[3] = {srgbToLinear(static_cast<float>(p["colorR"])), srgbToLinear(static_cast<float>(p["colorG"])),
                    srgbToLinear(static_cast<float>(p["colorB"]))};
    for (size_t i = 0; i < img.px.size(); i += 4) {
        float a = img.px[i + 3];
        for (int c = 0; c < 3; ++c) img.px[i + c] = img.px[i + c] * (1 - amt) + col[c] * a * amt;
    }
}

static void fxColor(ImageF& img, const Params& p, const EvalContext&) {
    float expo = static_cast<float>(std::exp2(p["exposure"]));
    float contrast = static_cast<float>(p["contrast"]), bright = static_cast<float>(p["brightness"]);
    float sat = static_cast<float>(p["saturation"]);
    float temp = static_cast<float>(p["temperature"]), tintv = static_cast<float>(p["tint"]);
    float gain[3] = {1.f + 0.25f * temp, 1.f - 0.15f * tintv, 1.f - 0.25f * temp};
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* d = img.row(y);
            for (int x = 0; x < img.width; ++x, d += 4) {
                float a = d[3];
                if (a <= 1e-6f) continue;
                float c[3];
                for (int k = 0; k < 3; ++k) {
                    float v = d[k] / a * expo * gain[k];
                    // Contrast/brightness in a perceptual (gamma 2.2) domain around mid-grey.
                    float g = std::pow(std::max(v, 0.f), 1.f / 2.2f);
                    g = (g - 0.5f) * contrast + 0.5f + bright;
                    c[k] = std::pow(std::max(g, 0.f), 2.2f);
                }
                float L = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                for (int k = 0; k < 3; ++k) d[k] = std::max(0.f, L + (c[k] - L) * sat) * a;
            }
        }
    });
}

static void fxVignette(ImageF& img, const Params& p, const EvalContext&) {
    float amount = static_cast<float>(p["amount"]), size = static_cast<float>(p["size"]), soft = static_cast<float>(std::max(0.01, p["softness"]));
    float cx = img.width * 0.5f, cy = img.height * 0.5f;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                float dx = (x + 0.5f - cx) / cx, dy = (y + 0.5f - cy) / cy;
                float r = std::sqrt(dx * dx + dy * dy) / std::sqrt(2.f);
                float t = std::clamp((r - size) / soft, 0.f, 1.f);
                t = t * t * (3 - 2 * t);
                float f = 1.f - amount * t;
                float* d = img.at(x, y);
                d[0] *= f; d[1] *= f; d[2] *= f;
            }
    });
}

static void fxGrain(ImageF& img, const Params& p, const EvalContext& ctx) {
    float amount = static_cast<float>(p["amount"]);
    if (amount <= 0) return;
    float size = static_cast<float>(std::max(0.5, p["size"]));
    uint32_t seed = static_cast<uint32_t>(p["seed"]);
    uint32_t frame = static_cast<uint32_t>(ctx.compFrame);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                uint32_t gx = static_cast<uint32_t>(x / size), gy = static_cast<uint32_t>(y / size);
                float n = hashUnit(gx + seed * 7919u, gy, frame) * 2.f - 1.f;
                float* d = img.at(x, y);
                float f = std::max(0.f, 1.f + amount * n);
                d[0] *= f; d[1] *= f; d[2] *= f;
            }
    });
}

static void fxPosterize(ImageF& img, const Params& p, const EvalContext&) {
    float levels = static_cast<float>(std::clamp(p["levels"], 2.0, 64.0)) - 1.f;
    for (size_t i = 0; i < img.px.size(); i += 4) {
        float a = img.px[i + 3];
        if (a <= 1e-6f) continue;
        for (int c = 0; c < 3; ++c) {
            float s = linearToSrgb(img.px[i + c] / a);
            s = std::round(s * levels) / levels;
            img.px[i + c] = srgbToLinear(s) * a;
        }
    }
}

static void fxSharpen(ImageF& img, const Params& p, const EvalContext&) {
    float amount = static_cast<float>(p["amount"]);
    if (amount <= 0) return;
    ImageF blur = img;
    gaussianBlur(blur, p["radius"], true);
    for (size_t i = 0; i < img.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) img.px[i + c] = std::max(0.f, img.px[i + c] + amount * (img.px[i + c] - blur.px[i + c]));
}

static void fxScanlines(ImageF& img, const Params& p, const EvalContext&) {
    float spacing = static_cast<float>(std::max(1.0, p["spacing"])), thick = static_cast<float>(std::clamp(p["thickness"], 0.0, 1.0));
    float dark = static_cast<float>(std::clamp(p["darkness"], 0.0, 1.0));
    for (int y = 0; y < img.height; ++y) {
        float phase = std::fmod(y + 0.5f, spacing) / spacing;
        if (phase >= thick) continue;
        float* d = img.row(y);
        for (int x = 0; x < img.width; ++x, d += 4) { d[0] *= 1 - dark; d[1] *= 1 - dark; d[2] *= 1 - dark; }
    }
}

static void fxSpeedLines(ImageF& img, const Params& p, const EvalContext& ctx) {
    float opacity = static_cast<float>(std::clamp(p["opacity"], 0.0, 1.0));
    if (opacity <= 0) return;
    float cx = static_cast<float>((0.5 + p["centerX"]) * img.width), cy = static_cast<float>((0.5 + p["centerY"]) * img.height);
    float count = static_cast<float>(std::clamp(p["count"], 8.0, 720.0));
    float inner = static_cast<float>(std::clamp(p["innerRadius"], 0.0, 0.95));
    float thick = static_cast<float>(std::clamp(p["thickness"], 0.02, 1.0));
    uint32_t seed = static_cast<uint32_t>(p["seed"]);
    double rate = p["animate"];
    uint32_t epoch = rate > 0 ? static_cast<uint32_t>(std::floor(ctx.localTime * rate)) : 0;
    float col[3] = {srgbToLinear(static_cast<float>(p["colorR"])), srgbToLinear(static_cast<float>(p["colorG"])),
                    srgbToLinear(static_cast<float>(p["colorB"]))};
    float halfDiag = 0.5f * std::sqrt(static_cast<float>(img.width) * img.width + static_cast<float>(img.height) * img.height);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                float r = std::sqrt(dx * dx + dy * dy) / halfDiag;
                float u = static_cast<float>((std::atan2(dy, dx) / (2 * kPi) + 0.5)) * count;
                uint32_t i = static_cast<uint32_t>(u) % static_cast<uint32_t>(count);
                float f = u - std::floor(u);
                float h1 = hashUnit(i, seed, epoch), h2 = hashUnit(i, seed + 1, epoch), h3 = hashUnit(i, seed + 2, epoch);
                if (h1 > 0.65f) continue;
                float w = thick * (0.25f + 0.75f * h2);
                float prof = 1.f - std::fabs(f - 0.5f) / (0.5f * w);
                if (prof <= 0) continue;
                float rIn = inner + (1.f - inner) * 0.35f * h3;
                float fade = std::clamp((r - rIn) / 0.12f, 0.f, 1.f);
                float al = opacity * std::min(1.f, prof * 1.5f) * fade;
                if (al <= 0) continue;
                float* d = img.at(x, y);
                for (int c = 0; c < 3; ++c) d[c] = d[c] * (1 - al) + col[c] * al;
                d[3] = d[3] * (1 - al) + al;
            }
    });
}

// ---------------------------------------------------------------- registry
EffectRegistry& EffectRegistry::instance() {
    static EffectRegistry r;
    return r;
}

void EffectRegistry::add(EffectDef def) { defs_[def.id] = std::move(def); }

const EffectDef* EffectRegistry::find(const std::string& id, int version) const {
    auto it = defs_.find(id);
    if (it == defs_.end()) return nullptr;
    if (version > it->second.version) return nullptr;  // instance from a newer effect version
    return &it->second;
}

std::vector<const EffectDef*> EffectRegistry::list() const {
    std::vector<const EffectDef*> v;
    for (auto& kv : defs_) v.push_back(&kv.second);
    return v;
}

EffectInstance EffectRegistry::instantiate(const std::string& id) const {
    const EffectDef* d = find(id);
    if (!d) throw std::runtime_error("unknown effect '" + id + "'");
    EffectInstance e;
    e.defId = d->id;
    e.defVersion = d->version;
    for (auto& p : d->params) e.params.emplace_back(p.id, AnimCurve(p.def));
    return e;
}

EffectRegistry::EffectRegistry() {
    auto P = [](std::string id, std::string name, double def, double mn, double mx, bool quick = false, bool px = false) {
        return ParamDef{std::move(id), std::move(name), def, mn, mx, quick, px};
    };
    add({"kinetic.gaussianBlur", 1, "Gaussian Blur", "Blur",
         {P("radius", "Radius", 8, 0, 200, true, true), P("repeatEdges", "Repeat Edge Pixels", 1, 0, 1)}, fxGaussian});
    add({"kinetic.directionalBlur", 1, "Directional Blur", "Blur",
         {P("length", "Length", 30, 0, 400, true, true), P("angle", "Direction", 0, -360, 360, true)}, fxDirectional});
    add({"kinetic.zoomBlur", 1, "Zoom Blur", "Blur",
         {P("amount", "Amount", 0.2, 0, 0.95, true), P("centerX", "Center X", 0, -0.5, 0.5), P("centerY", "Center Y", 0, -0.5, 0.5),
          P("samples", "Quality (samples)", 24, 4, 64)}, fxZoomBlur});
    add({"kinetic.glow", 1, "Glow", "Glow",
         {P("threshold", "Threshold", 0.6, 0, 1, true), P("radius", "Radius", 12, 0, 200, true, true),
          P("intensity", "Intensity", 1.0, 0, 10, true), P("spread", "Spread (layers)", 3, 1, 5),
          P("tintR", "Tint R", 1, 0, 1), P("tintG", "Tint G", 1, 0, 1), P("tintB", "Tint B", 1, 0, 1)}, fxGlow});
    add({"kinetic.rgbSplit", 1, "RGB Split", "Distortion",
         {P("amount", "Amount", 8, -200, 200, true, true), P("angle", "Angle", 0, -360, 360, true)}, fxRgbSplit});
    add({"kinetic.flash", 1, "Impact Flash", "Impact",
         {P("amount", "Amount", 1, 0, 1, true), P("colorR", "Color R", 1, 0, 1), P("colorG", "Color G", 1, 0, 1),
          P("colorB", "Color B", 1, 0, 1)}, fxFlash});
    add({"kinetic.color", 1, "Color Adjust", "Color",
         {P("exposure", "Exposure", 0, -5, 5, true), P("contrast", "Contrast", 1, 0, 3, true), P("brightness", "Brightness", 0, -1, 1),
          P("saturation", "Saturation", 1, 0, 3, true), P("temperature", "Temperature", 0, -1, 1, true), P("tint", "Tint", 0, -1, 1)}, fxColor});
    add({"kinetic.vignette", 1, "Vignette", "Texture",
         {P("amount", "Amount", 0.5, 0, 1, true), P("size", "Size", 0.55, 0, 1.5), P("softness", "Softness", 0.4, 0.01, 1.5)}, fxVignette});
    add({"kinetic.filmGrain", 1, "Film Grain", "Texture",
         {P("amount", "Amount", 0.15, 0, 1, true), P("size", "Size", 1.5, 0.5, 8, false, true), P("seed", "Seed", 1, 0, 1e6)}, fxGrain});
    add({"kinetic.posterize", 1, "Posterize", "Stylize", {P("levels", "Levels", 6, 2, 64, true)}, fxPosterize});
    add({"kinetic.sharpen", 1, "Sharpen", "Stylize",
         {P("amount", "Amount", 0.8, 0, 5, true), P("radius", "Radius", 1.5, 0.3, 10, false, true)}, fxSharpen});
    add({"kinetic.scanlines", 1, "Scanlines", "Texture",
         {P("spacing", "Spacing", 4, 1, 64, true, true), P("thickness", "Thickness", 0.5, 0, 1), P("darkness", "Darkness", 0.3, 0, 1, true)}, fxScanlines});
    add({"kinetic.speedLines", 1, "Anime Speed Lines", "Impact",
         {P("opacity", "Opacity", 0.8, 0, 1, true), P("count", "Line Count", 120, 8, 720, true), P("innerRadius", "Clear Center", 0.35, 0, 0.95, true),
          P("thickness", "Thickness", 0.35, 0.02, 1), P("centerX", "Center X", 0, -0.5, 0.5), P("centerY", "Center Y", 0, -0.5, 0.5),
          P("animate", "Flicker Rate (Hz)", 12, 0, 60), P("seed", "Seed", 1, 0, 1e6),
          P("colorR", "Color R", 1, 0, 1), P("colorG", "Color G", 1, 0, 1), P("colorB", "Color B", 1, 0, 1)}, fxSpeedLines});
}

void applyEffectStack(ImageF& img, const std::vector<EffectInstance>& stack, const EvalContext& ctx,
                      std::vector<std::string>* warnings) {
    auto& reg = EffectRegistry::instance();
    for (auto& inst : stack) {
        if (!inst.enabled || inst.mix <= 0) continue;
        const EffectDef* def = reg.find(inst.defId, inst.defVersion);
        if (!def) {
            if (warnings) warnings->push_back("effect '" + inst.defId + "@" + std::to_string(inst.defVersion) +
                                              "' is unavailable; rendered as pass-through");
            continue;
        }
        Params params;
        for (auto& pd : def->params) {
            double v = inst.eval(pd.id, ctx.localTime, pd.def);
            if (pd.pixels) v *= ctx.pixelScale;
            params.values.emplace_back(pd.id, v);
        }
        if (inst.mix >= 1.0) {
            def->process(img, params, ctx);
        } else {
            ImageF orig = img;
            def->process(img, params, ctx);
            float m = static_cast<float>(inst.mix);
            for (size_t i = 0; i < img.px.size(); ++i) img.px[i] = orig.px[i] + (img.px[i] - orig.px[i]) * m;
        }
    }
}

} // namespace kinetic::render
