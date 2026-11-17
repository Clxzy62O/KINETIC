#include <cmath>

#include "Test.h"
#include "edit/Edit.h"
#include "render/Compositor.h"
#include "render/Effects.h"
#include "render/Evaluator.h"

using namespace kinetic;
using namespace kinetic::render;

namespace {
ImageF solid(int w, int h, float r, float g, float b, float a) {
    ImageF i(w, h);
    i.fill(r * a, g * a, b * a, a);
    return i;
}
ImageF pattern(int w, int h) {
    ImageF i(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = i.at(x, y);
            bool on = ((x / 8) + (y / 8)) % 2;
            p[0] = on ? 0.9f : 0.05f;
            p[1] = static_cast<float>(x) / w;
            p[2] = static_cast<float>(y) / h;
            p[3] = 1.f;
        }
    return i;
}
double meanAbsDiff(const ImageF& a, const ImageF& b) {
    double s = 0;
    for (size_t i = 0; i < a.px.size(); ++i) s += std::fabs(a.px[i] - b.px[i]);
    return s / a.px.size();
}
struct Scene {
    Project p;
    Id comp, v1, v2, v3;
    Scene(int w = 64, int h = 36) {
        Composition c = makeComposition(p, "c", w, h, Rational(30, 1), 30);
        comp = c.id;
        p.compositions.push_back(c);
        p.rootCompositionId = comp;
        auto& cc = *p.findComposition(comp);
        v1 = addTrack(p, cc, TrackKind::Video, "1").id;
        v2 = addTrack(p, cc, TrackKind::Video, "2").id;
        v3 = addTrack(p, cc, TrackKind::Video, "3").id;
    }
    Clip& solidClip(Id track, Color col, BlendMode mode = BlendMode::Normal) {
        Clip c;
        c.id = p.allocId();
        c.kind = ClipKind::Solid;
        c.solidColor = col;
        c.duration = 30;
        c.blend = mode;
        p.findTrack(track)->clips.push_back(c);
        return p.findTrack(track)->clips.back();
    }
};
} // namespace

TEST(render_srgb_roundtrip_is_exact_for_8bit) {
    Image8 in(256, 1);
    for (int i = 0; i < 256; ++i) { in.px[i * 4] = in.px[i * 4 + 1] = in.px[i * 4 + 2] = static_cast<uint8_t>(i); in.px[i * 4 + 3] = 255; }
    Image8 out = toSrgb8(toLinear(in));
    CHECK(out.px == in.px);
}

TEST(render_blend_modes_math) {
    CHECK_NEAR(blendChannel(BlendMode::Screen, 0.5f, 0.5f), 0.75, 1e-6);
    CHECK_NEAR(blendChannel(BlendMode::Multiply, 0.5f, 0.5f), 0.25, 1e-6);
    CHECK_NEAR(blendChannel(BlendMode::Overlay, 0.25f, 0.5f), 0.25, 1e-6);
    CHECK_NEAR(blendChannel(BlendMode::Difference, 0.2f, 0.7f), 0.5, 1e-6);
    ImageF dst = solid(4, 4, 0.2f, 0.2f, 0.2f, 1);
    ImageF src = solid(4, 4, 1, 0, 0, 0.5f);
    blend(dst, src, BlendMode::Normal, 1.f);
    CHECK_NEAR(dst.at(0, 0)[0], 0.6, 1e-6);   // 1*0.5 + 0.2*0.5
    CHECK_NEAR(dst.at(0, 0)[3], 1.0, 1e-6);
    ImageF d2 = solid(4, 4, 0.5f, 0.5f, 0.5f, 1);
    blend(d2, solid(4, 4, 0.5f, 0.5f, 0.5f, 1), BlendMode::Screen, 1.f);
    CHECK_NEAR(d2.at(1, 1)[1], 0.75, 1e-6);
    // Blending onto transparent with a non-normal mode behaves like normal (W3C).
    ImageF d3(4, 4);
    blend(d3, solid(4, 4, 0.3f, 0.6f, 0.9f, 1), BlendMode::Multiply, 1.f);
    CHECK_NEAR(d3.at(0, 0)[2], 0.9, 1e-6);
}

TEST(render_transform_identity_and_rotation) {
    ImageF layer = pattern(16, 16);
    ImageF dst(16, 16);
    drawTransformed(dst, layer, Affine{}, BlendMode::Normal, 1.f);
    CHECK(meanAbsDiff(dst, layer) < 1e-7);
    // 90° rotation about the centre maps pixel (x, y) -> (15 - y, x).
    Affine m = Affine::translate(8, 8) * Affine::rotateDeg(90) * Affine::translate(-8, -8);
    ImageF rot(16, 16);
    drawTransformed(rot, layer, m, BlendMode::Normal, 1.f);
    for (int y = 1; y < 15; ++y)
        for (int x = 1; x < 15; ++x)
            for (int c = 0; c < 4; ++c) CHECK_NEAR(rot.at(15 - y, x)[c], layer.at(x, y)[c], 1e-5);
    Affine inv = m.inverse();
    Affine id = m * inv;
    CHECK(id.isIdentity(1e-9));
}

TEST(render_effects_registry_complete_and_deterministic) {
    auto& reg = EffectRegistry::instance();
    CHECK(reg.list().size() >= 13);
    for (auto* def : reg.list()) {
        EffectInstance inst = reg.instantiate(def->id);
        ImageF a = pattern(48, 32), b = pattern(48, 32);
        EvalContext ctx;
        ctx.compFrame = 7;
        applyEffectStack(a, {inst}, ctx, nullptr);
        applyEffectStack(b, {inst}, ctx, nullptr);
        CHECK(a.px == b.px);  // bit-identical across runs (and thread counts)
        for (float v : a.px) CHECK(std::isfinite(v));
    }
}

TEST(render_effect_pixel_params_scale_with_preview_resolution) {
    // A 4px blur at full res and the same blur rendered at half res must look the same.
    auto& reg = EffectRegistry::instance();
    EffectInstance blur = reg.instantiate("kinetic.gaussianBlur");
    blur.param("radius")->setConstant(4);
    ImageF full = pattern(128, 128);
    EvalContext c1;
    applyEffectStack(full, {blur}, c1, nullptr);
    ImageF half = pattern(128, 128);
    ImageF small(64, 64);
    drawTransformed(small, half, Affine::scale(0.5, 0.5), BlendMode::Normal, 1.f);
    EvalContext c2;
    c2.pixelScale = 0.5;
    applyEffectStack(small, {blur}, c2, nullptr);
    ImageF fullDown(64, 64);
    drawTransformed(fullDown, full, Affine::scale(0.5, 0.5), BlendMode::Normal, 1.f);
    CHECK(meanAbsDiff(small, fullDown) < 0.01);
}

TEST(render_unavailable_effect_passes_through_with_warning) {
    ImageF a = pattern(16, 16), ref = a;
    std::vector<std::string> w;
    applyEffectStack(a, {EffectInstance{"vendor.missing", 1}}, EvalContext{}, &w);
    CHECK(a.px == ref.px);
    CHECK_EQ(w.size(), size_t(1));
    // An instance saved by a newer version of an effect is treated as unavailable, not misrendered.
    w.clear();
    applyEffectStack(a, {EffectInstance{"kinetic.glow", 99}}, EvalContext{}, &w);
    CHECK_EQ(w.size(), size_t(1));
}

TEST(render_effect_mix_is_linear_interpolation) {
    auto& reg = EffectRegistry::instance();
    EffectInstance flash = reg.instantiate("kinetic.flash");
    ImageF base = pattern(8, 8), full = base, half = base;
    applyEffectStack(full, {flash}, {}, nullptr);
    flash.mix = 0.5;
    applyEffectStack(half, {flash}, {}, nullptr);
    for (size_t i = 0; i < base.px.size(); ++i) CHECK_NEAR(half.px[i], 0.5 * (base.px[i] + full.px[i]), 1e-6);
}

TEST(render_composition_layers_opacity_and_background) {
    Scene s;
    s.p.findComposition(s.comp)->background = {0, 0, 0, 1};
    Clip& red = s.solidClip(s.v1, {1, 0, 0, 1});
    red.transform.opacity = AnimCurve(0.5);
    media::FrameProvider fp(1 << 20);
    Renderer r(s.p, fp);
    Image8 img = r.renderFrame8(s.comp, 0, {});
    // linear 0.5 -> sRGB 0.7354 -> 188
    CHECK_EQ(static_cast<int>(img.px[0]), 188);
    CHECK_EQ(static_cast<int>(img.px[1]), 0);
    // Add blend of a 50% grey on top: linear 0.5 + 0.214 -> sRGB ~ 0.86
    s.solidClip(s.v2, {0.5, 0.5, 0.5, 1}, BlendMode::Add);
    Image8 img2 = r.renderFrame8(s.comp, 0, {});
    CHECK(img2.px[0] > 215 && img2.px[0] < 225);
    CHECK(img2.px[1] > 120 && img2.px[1] < 130);
}

TEST(render_adjustment_layer_affects_layers_below_only) {
    Scene s;
    Clip& grey = s.solidClip(s.v1, {0.5, 0.5, 0.5, 1});
    (void)grey;
    Clip adj;
    adj.id = s.p.allocId();
    adj.kind = ClipKind::Adjustment;
    adj.duration = 30;
    EffectInstance ex = EffectRegistry::instance().instantiate("kinetic.color");
    ex.param("exposure")->setConstant(1.0);
    adj.effects.push_back(ex);
    s.p.findTrack(s.v2)->clips.push_back(adj);
    media::FrameProvider fp(1 << 20);
    Renderer r(s.p, fp);
    ImageF img = r.renderFrame(s.comp, 0, {});
    CHECK_NEAR(img.at(0, 0)[0], 2 * srgbToLinear(0.5f), 1e-3);  // +1 stop doubles linear light
    // A layer above the adjustment is untouched.
    s.solidClip(s.v3, {0.5, 0.5, 0.5, 1});
    ImageF img2 = r.renderFrame(s.comp, 0, {});
    CHECK_NEAR(img2.at(0, 0)[0], srgbToLinear(0.5f), 1e-5);
}

TEST(render_nested_composition_and_velocity) {
    Scene outer;
    Composition inner = makeComposition(outer.p, "inner", 64, 36, Rational(30, 1), 60);
    Id innerId = inner.id;
    Track t;
    t.id = outer.p.allocId();
    Clip blue;
    blue.id = outer.p.allocId();
    blue.kind = ClipKind::Solid;
    blue.solidColor = {0, 0, 1, 1};
    blue.start = 30;      // blue only in the inner comp's second second
    blue.duration = 30;
    t.clips.push_back(blue);
    inner.tracks.push_back(t);
    outer.p.compositions.push_back(inner);
    Clip nest;
    nest.id = outer.p.allocId();
    nest.kind = ClipKind::Composition;
    nest.compositionId = innerId;
    nest.duration = 30;
    nest.speed = AnimCurve(2.0);  // plays the inner comp at double speed
    outer.p.findTrack(outer.v1)->clips.push_back(nest);
    CHECK(outer.p.validate().empty());
    media::FrameProvider fp(1 << 20);
    Renderer r(outer.p, fp);
    CHECK_NEAR(r.renderFrame(outer.comp, 10, {}).at(5, 5)[2], 0.0, 1e-6);  // inner frame 20
    CHECK_NEAR(r.renderFrame(outer.comp, 20, {}).at(5, 5)[2], 1.0, 1e-6);  // inner frame 40
}

TEST(render_preview_scale_matches_full_render) {
    Scene s(128, 72);
    Clip& c = s.solidClip(s.v1, {0.9, 0.4, 0.1, 1});
    c.transform.scale = AnimCurve(0.5);
    c.transform.rotation = AnimCurve(30);
    EffectInstance glow = EffectRegistry::instance().instantiate("kinetic.glow");
    glow.param("threshold")->setConstant(0.2);
    c.effects.push_back(glow);
    media::FrameProvider fp(1 << 20);
    Renderer r(s.p, fp);
    ImageF full = r.renderFrame(s.comp, 0, {1.0});
    ImageF half = r.renderFrame(s.comp, 0, {0.5});
    ImageF fullDown(64, 36);
    drawTransformed(fullDown, full, Affine::scale(0.5, 0.5), BlendMode::Normal, 1.f);
    CHECK_EQ(half.width, 64);
    CHECK(meanAbsDiff(half, fullDown) < 0.02);
}

TEST(render_camera_shake_deterministic_and_bounded) {
    CameraShake s;
    s.enabled = true;
    s.amplitude = AnimCurve(20);
    s.rotation = 3;
    double maxD = 0;
    for (int f = 0; f < 300; ++f) {
        auto a = evaluateShake(s, f / 60.0), b = evaluateShake(s, f / 60.0);
        CHECK(a.dx == b.dx && a.dy == b.dy && a.rotation == b.rotation);
        maxD = std::max({maxD, std::fabs(a.dx), std::fabs(a.dy)});
    }
    CHECK(maxD > 5 && maxD <= 20.0 + 1e-9);
    s.amplitude = AnimCurve(0);
    CHECK(evaluateShake(s, 0.3).dx == 0);
    s.amplitude = AnimCurve(20);
    s.decay = 5;
    CHECK(std::fabs(evaluateShake(s, 2.0).dx) < 20 * std::exp(-10) + 1e-9);
}

TEST(render_optical_flow_is_never_silently_substituted) {
    Scene s;
    MediaRef m;
    m.id = s.p.allocId();
    m.hasVideo = true;
    m.width = 64;
    m.height = 36;
    m.duration = 10;
    m.path = "/nonexistent.mp4";
    s.p.media.push_back(m);
    Clip c;
    c.id = s.p.allocId();
    c.mediaId = m.id;
    c.duration = 30;
    c.timeInterp = TimeInterp::OpticalFlow;
    s.p.findTrack(s.v1)->clips.push_back(c);
    media::FrameProvider fp(1 << 20);
    Renderer r(s.p, fp);
    CHECK_THROWS(r.renderFrame(s.comp, 0, {}));
}
