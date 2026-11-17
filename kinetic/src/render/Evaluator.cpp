#include "render/Evaluator.h"

#include <algorithm>
#include <cmath>

#include "project/Velocity.h"
#include "render/Effects.h"

namespace kinetic::render {

static constexpr double kTau = 6.28318530717958647692;

// ---------------------------------------------------------------- camera shake
static double hashSigned(int64_t seed, int64_t i, int channel) {
    uint64_t x = static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(i) * 0xBF58476D1CE4E5B9ull ^
                 static_cast<uint64_t>(channel + 1) * 0x94D049BB133111EBull;
    x ^= x >> 31; x *= 0x7FB5D329728EA185ull; x ^= x >> 27; x *= 0x81DADEF4BC2DD44Dull; x ^= x >> 33;
    return static_cast<double>(x >> 11) / 4503599627370496.0 - 1.0;  // [-1, 1)
}
// Smooth value noise in [-1, 1] at `t` lattice units.
static double valueNoise(int64_t seed, double t, int channel) {
    double fl = std::floor(t);
    int64_t i = static_cast<int64_t>(fl);
    double f = t - fl;
    double u = f * f * f * (f * (f * 6 - 15) + 10);  // quintic fade
    return hashSigned(seed, i, channel) * (1 - u) + hashSigned(seed, i + 1, channel) * u;
}

ShakeOffset evaluateShake(const CameraShake& s, double t) {
    ShakeOffset o;
    if (!s.enabled) return o;
    double amp = s.amplitude.evaluate(t);
    if (s.decay > 0) amp *= std::exp(-s.decay * t);
    if (std::fabs(amp) < 1e-9) return o;
    double r = std::clamp(s.randomness, 0.0, 1.0);
    double ft = t * s.frequency;
    auto chan = [&](int ch, double phase) {
        return r * valueNoise(s.seed, ft, ch) + (1 - r) * std::sin(kTau * ft + phase);
    };
    double nx = chan(0, 0.0), ny = chan(1, 1.7);
    double dir = s.directionDeg * kTau / 360.0;
    double along = nx * std::cos(dir) + ny * std::sin(dir);
    double dl = std::clamp(s.directional, 0.0, 1.0);
    o.dx = amp * ((1 - dl) * nx + dl * along * std::cos(dir));
    o.dy = amp * ((1 - dl) * ny + dl * along * std::sin(dir));
    o.rotation = s.rotation * (amp / 100.0) * chan(2, 0.9);
    o.zoom = 1.0 + s.zoom * (amp / 100.0) * std::fabs(chan(3, 2.3));
    return o;
}

// ---------------------------------------------------------------- renderer
void Renderer::warn(const std::string& w) {
    if (std::find(warnings_.begin(), warnings_.end(), w) == warnings_.end()) warnings_.push_back(w);
}
std::vector<std::string> Renderer::takeWarnings() { return std::exchange(warnings_, {}); }

static double maxCurve(const AnimCurve& c) {
    if (!c.animated()) return std::fabs(c.constant());
    double m = 0;
    for (auto& k : c.keys()) m = std::max(m, std::fabs(k.v));
    return m;
}

bool Renderer::layerImage(const Composition& comp, const Clip& clip, double localT, int64_t frame, double scale,
                          int depth, ImageF& out, double& lw, double& lh) {
    switch (clip.kind) {
    case ClipKind::Solid: {
        lw = comp.width;
        lh = comp.height;
        int w = std::max(1, static_cast<int>(std::lround(lw * scale))), h = std::max(1, static_cast<int>(std::lround(lh * scale)));
        out = ImageF(w, h);
        float a = static_cast<float>(clip.solidColor[3]);
        out.fill(srgbToLinear(static_cast<float>(clip.solidColor[0])) * a, srgbToLinear(static_cast<float>(clip.solidColor[1])) * a,
                 srgbToLinear(static_cast<float>(clip.solidColor[2])) * a, a);
        return true;
    }
    case ClipKind::Composition: {
        const Composition* inner = p_.findComposition(clip.compositionId);
        if (!inner) { warn("clip '" + clip.name + "' references a missing composition"); return false; }
        if (depth > 16) throw RenderError("composition nesting too deep (cycle?) at '" + clip.name + "'");
        double src = Velocity::sourceTime(clip, localT);
        int64_t f = secondsToFrameFloor(src, inner->fps);
        if (f < 0 || f >= inner->durationFrames) return false;
        lw = inner->width;
        lh = inner->height;
        out = renderComp(*inner, f, scale, depth + 1);
        return true;
    }
    case ClipKind::Media: {
        const MediaRef* m = p_.findMedia(clip.mediaId);
        if (!m) { warn("clip '" + clip.name + "' references missing media"); return false; }
        if (!m->hasVideo) return false;
        lw = m->width;
        lh = m->height;
        // Decode near the size it will be displayed at (no unnecessary full-res work in preview).
        double fit = 1.0;
        if (clip.transform.fit == FitMode::Fit) fit = std::min(comp.width / lw, comp.height / lh);
        else if (clip.transform.fit == FitMode::Fill) fit = std::max(comp.width / lw, comp.height / lh);
        double maxScale = maxCurve(clip.transform.scale) *
                          std::max(maxCurve(clip.transform.scaleX), maxCurve(clip.transform.scaleY));
        if (clip.shake.enabled && clip.shake.zoom > 0) maxScale *= 1.0 + clip.shake.zoom * maxCurve(clip.shake.amplitude) / 100.0;
        double ds = std::clamp(fit * scale * maxScale, 0.05, 1.0);
        int dw = std::max(2, static_cast<int>(std::lround(lw * ds / 2)) * 2), dh = std::max(2, static_cast<int>(std::lround(lh * ds / 2)) * 2);
        if (ds >= 0.999) { dw = m->width; dh = m->height; }

        double src = Velocity::sourceTime(clip, localT);
        double fps = m->fps.toDouble();
        int64_t lastFrame = m->isStill ? 0 : std::max<int64_t>(0, static_cast<int64_t>(std::floor(m->duration * fps + 1e-6)) - 1);
        double srcFrame = src * fps;
        if (!m->isStill && (srcFrame < -0.5 || srcFrame > lastFrame + 1.0))
            warn("clip '" + clip.name + "' samples outside its source; holding the edge frame");
        if (clip.timeInterp == TimeInterp::OpticalFlow)
            throw RenderError("clip '" + clip.name + "' uses optical-flow interpolation, which is not installed. "
                              "Choose Frame Sampling or Frame Blending, or install the optical-flow module.");
        int64_t f0 = std::clamp<int64_t>(static_cast<int64_t>(std::floor(srcFrame + 1e-6)), 0, lastFrame);
        Image8Ptr a = frames_.get(*m, f0, dw, dh);
        out = toLinear(*a);
        if (clip.timeInterp == TimeInterp::Blend && !m->isStill) {
            double frac = srcFrame - std::floor(srcFrame + 1e-6);
            int64_t f1 = std::min(f0 + 1, lastFrame);
            if (frac > 1e-3 && f1 != f0) {
                ImageF b = toLinear(*frames_.get(*m, f1, dw, dh));
                float w = static_cast<float>(frac);
                for (size_t i = 0; i < out.px.size(); ++i) out.px[i] += (b.px[i] - out.px[i]) * w;
            }
        }
        (void)frame;
        return true;
    }
    case ClipKind::Adjustment: return false;
    }
    return false;
}

ImageF Renderer::renderComp(const Composition& comp, int64_t frame, double scale, int depth) {
    int W = std::max(1, static_cast<int>(std::lround(comp.width * scale)));
    int H = std::max(1, static_cast<int>(std::lround(comp.height * scale)));
    ImageF dst(W, H);  // transparent; background is applied by the caller for the root comp
    bool anySolo = std::any_of(comp.tracks.begin(), comp.tracks.end(), [](const Track& t) { return t.kind == TrackKind::Video && t.solo; });

    for (const Track& track : comp.tracks) {
        if (track.kind != TrackKind::Video || track.hidden) continue;
        if (anySolo && !track.solo) continue;
        const Clip* clip = track.clipAt(frame);
        if (!clip || !clip->enabled) continue;
        double localT = framesToSeconds(frame - clip->start, comp.fps);
        EvalContext ctx;
        ctx.localTime = localT;
        ctx.compFrame = frame;

        const Transform& tr = clip->transform;
        float opacity = static_cast<float>(std::clamp(tr.opacity.evaluate(localT), 0.0, 1.0));

        if (clip->kind == ClipKind::Adjustment) {
            if (clip->effectsBypassed || opacity <= 0) continue;
            ImageF adjusted = dst;
            ctx.pixelScale = scale;
            applyEffectStack(adjusted, clip->effects, ctx, &warnings_);
            if (opacity >= 1.f) dst = std::move(adjusted);
            else for (size_t i = 0; i < dst.px.size(); ++i) dst.px[i] += (adjusted.px[i] - dst.px[i]) * opacity;
            continue;
        }

        ImageF layer;
        double lw = 0, lh = 0;
        if (!layerImage(comp, *clip, localT, frame, scale, depth, layer, lw, lh)) continue;
        if (opacity <= 0) continue;
        ctx.pixelScale = layer.width / lw;  // buffer px per layer px
        if (!clip->effectsBypassed) applyEffectStack(layer, clip->effects, ctx, &warnings_);

        // layer buffer px -> layer px -> comp px (full res) -> dst px
        double fit = 1.0;
        if (tr.fit == FitMode::Fit) fit = std::min(comp.width / lw, comp.height / lh);
        else if (tr.fit == FitMode::Fill) fit = std::max(comp.width / lw, comp.height / lh);
        ShakeOffset sh = evaluateShake(clip->shake, localT);
        double s = tr.scale.evaluate(localT) * fit * sh.zoom;
        double sx = s * tr.scaleX.evaluate(localT), sy = s * tr.scaleY.evaluate(localT);
        Affine m = Affine::scale(scale, scale) *
                   Affine::translate(comp.width / 2.0 + tr.posX.evaluate(localT) + sh.dx, comp.height / 2.0 + tr.posY.evaluate(localT) + sh.dy) *
                   Affine::rotateDeg(tr.rotation.evaluate(localT) + sh.rotation) * Affine::scale(sx, sy) *
                   Affine::translate(-(lw / 2.0 + tr.anchorX.evaluate(localT)), -(lh / 2.0 + tr.anchorY.evaluate(localT))) *
                   Affine::scale(lw / layer.width, lh / layer.height);
        drawTransformed(dst, layer, m, clip->blend, opacity);
    }
    return dst;
}

ImageF Renderer::renderFrame(Id compId, int64_t frame, const RenderSettings& s) {
    const Composition* c = p_.findComposition(compId);
    if (!c) throw RenderError("composition not found");
    if (s.scale <= 0 || s.scale > 1) throw RenderError("render scale must be in (0, 1]");
    return renderComp(*c, frame, s.scale, 0);
}

Image8 Renderer::renderFrame8(Id compId, int64_t frame, const RenderSettings& s) {
    const Composition* c = p_.findComposition(compId);
    if (!c) throw RenderError("composition not found");
    ImageF img = renderFrame(compId, frame, s);
    ImageF out(img.width, img.height);
    float a = static_cast<float>(c->background[3]);
    out.fill(srgbToLinear(static_cast<float>(c->background[0])) * a, srgbToLinear(static_cast<float>(c->background[1])) * a,
             srgbToLinear(static_cast<float>(c->background[2])) * a, a);
    blend(out, img, BlendMode::Normal, 1.f);
    return toSrgb8(out);
}

} // namespace kinetic::render
