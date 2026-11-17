#include "project/Velocity.h"

#include <algorithm>
#include <cmath>

namespace kinetic {

static const std::pair<VelocityPreset, const char*> kPresetNames[] = {
    {VelocityPreset::Constant, "constant"},
    {VelocityPreset::SmoothSlowdown, "smoothSlowdown"},
    {VelocityPreset::FastAcceleration, "fastAcceleration"},
    {VelocityPreset::ImpactRamp, "impactRamp"},
    {VelocityPreset::ReverseRamp, "reverseRamp"},
    {VelocityPreset::AnimeVelocity, "animeVelocity"},
    {VelocityPreset::HyperVelocity, "hyperVelocity"},
    {VelocityPreset::SlowMotion, "slowMotion"},
    {VelocityPreset::FastMotion, "fastMotion"},
    {VelocityPreset::BeatDropRamp, "beatDropRamp"},
    {VelocityPreset::SmoothInOut, "smoothInOut"},
};

const char* velocityPresetName(VelocityPreset p) {
    for (auto& kv : kPresetNames) if (kv.first == p) return kv.second;
    return "constant";
}
bool velocityPresetFromName(const std::string& n, VelocityPreset& out) {
    for (auto& kv : kPresetNames) if (n == kv.second) { out = kv.first; return true; }
    return false;
}
std::vector<VelocityPreset> allVelocityPresets() {
    std::vector<VelocityPreset> v;
    for (auto& kv : kPresetNames) v.push_back(kv.first);
    return v;
}

namespace Velocity {

double sourceTime(const Clip& clip, double localT) {
    return clip.sourceIn + clip.speed.integrate(0.0, localT);
}

double sourceSpan(const Clip& clip, const Rational& fps) {
    return clip.speed.integrate(0.0, framesToSeconds(clip.duration, fps));
}

void sourceRange(const Clip& clip, const Rational& fps, double& lo, double& hi) {
    double dur = framesToSeconds(clip.duration, fps);
    lo = hi = clip.sourceIn;
    // Walk frame by frame (source times we will actually sample).
    double acc = clip.sourceIn;
    double step = framesToSeconds(1, fps);
    double t = 0;
    for (int64_t f = 0; f < clip.duration; ++f) {
        lo = std::min(lo, acc);
        hi = std::max(hi, acc);
        acc += clip.speed.integrate(t, std::min(t + step, dur));
        t += step;
    }
}

void applyPreset(AnimCurve& s, VelocityPreset p, double d, double k, double pivot) {
    s.clearKeys();
    pivot = std::clamp(pivot, 0.05, 0.95);
    const double pt = d * pivot;
    auto key = [&](double t, double v, EasePreset ease) {
        s.setKey(std::clamp(t, 0.0, d), v);
        // ease applied after all keys exist (below)
        (void)ease;
    };
    std::vector<EasePreset> eases;
    auto add = [&](double t, double v, EasePreset e) { key(t, v, e); eases.push_back(e); };
    switch (p) {
    case VelocityPreset::Constant: s.setConstant(k); return;
    case VelocityPreset::SlowMotion: s.setConstant(0.25 / std::max(k, 0.01)); return;
    case VelocityPreset::FastMotion: s.setConstant(3.0 * k); return;
    case VelocityPreset::SmoothSlowdown:
        add(0, 1.0, EasePreset::EaseInOut); add(d, 0.2 / k, EasePreset::Linear); break;
    case VelocityPreset::FastAcceleration:
        add(0, 0.3, EasePreset::EaseIn); add(d, 3.0 * k, EasePreset::Linear); break;
    case VelocityPreset::ImpactRamp:
        add(0, 2.5 * k, EasePreset::EaseOut); add(pt, 0.1, EasePreset::EaseIn); add(d, 2.5 * k, EasePreset::Linear); break;
    case VelocityPreset::ReverseRamp:
        add(0, 1.0, EasePreset::EaseInOut); add(pt, 0.0, EasePreset::EaseIn); add(d, -1.5 * k, EasePreset::Linear); break;
    case VelocityPreset::AnimeVelocity:
        add(0, 3.0 * k, EasePreset::Snap); add(pt, 0.15, EasePreset::EaseIn); add(d, 2.0 * k, EasePreset::Linear); break;
    case VelocityPreset::HyperVelocity:
        add(0, 4.0 * k, EasePreset::Snap); add(d * 0.3, 0.2, EasePreset::EaseIn);
        add(d * 0.55, 4.0 * k, EasePreset::Snap); add(d * 0.8, 0.15, EasePreset::EaseIn); add(d, 3.0 * k, EasePreset::Linear); break;
    case VelocityPreset::BeatDropRamp:
        add(0, 0.8, EasePreset::EaseIn); add(pt, 3.5 * k, EasePreset::Punch); add(std::min(d, pt + d * 0.1), 0.2, EasePreset::Hold);
        add(d, 0.2, EasePreset::Linear); break;
    case VelocityPreset::SmoothInOut:
        add(0, 0.3, EasePreset::EaseInOut); add(d * 0.5, 2.0 * k, EasePreset::EaseInOut); add(d, 0.3, EasePreset::Linear); break;
    }
    for (size_t i = 0; i < s.keys().size() && i < eases.size(); ++i) s.setSegmentEase(i, eases[i]);
}

std::vector<double> sampleForDisplay(const Clip& clip, double dur, int n) {
    std::vector<double> out;
    out.reserve(static_cast<size_t>(std::max(n, 0)));
    for (int i = 0; i < n; ++i) out.push_back(clip.speed.evaluate(n > 1 ? dur * i / (n - 1) : 0));
    return out;
}

} // namespace Velocity
} // namespace kinetic
