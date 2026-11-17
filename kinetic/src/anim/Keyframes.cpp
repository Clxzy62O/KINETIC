#include "anim/Keyframes.h"

#include <algorithm>
#include <cmath>

namespace kinetic {

const char* easePresetName(EasePreset p) {
    switch (p) {
    case EasePreset::Linear: return "linear";
    case EasePreset::EaseIn: return "easeIn";
    case EasePreset::EaseOut: return "easeOut";
    case EasePreset::EaseInOut: return "easeInOut";
    case EasePreset::Punch: return "punch";
    case EasePreset::Snap: return "snap";
    case EasePreset::Overshoot: return "overshoot";
    case EasePreset::Hold: return "hold";
    }
    return "linear";
}
bool easePresetFromName(const std::string& n, EasePreset& out) {
    for (int i = 0; i <= static_cast<int>(EasePreset::Hold); ++i) {
        auto p = static_cast<EasePreset>(i);
        if (n == easePresetName(p)) { out = p; return true; }
    }
    return false;
}

static double bez(double a, double b, double t) {  // 1D cubic with P0=0, P3=1
    double u = 1 - t;
    return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}
static double bezDeriv(double a, double b, double t) {
    double u = 1 - t;
    return 3 * u * u * a + 6 * u * t * (b - a) + 3 * t * t * (1 - b);
}

double cubicBezierEase(double x, Vec2 p1, Vec2 p2) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    double x1 = std::clamp(p1.x, 0.0, 1.0), x2 = std::clamp(p2.x, 0.0, 1.0);
    // Newton first, bisection fallback (x(t) is monotonic for x handles in [0,1]).
    double t = x;
    for (int i = 0; i < 8; ++i) {
        double err = bez(x1, x2, t) - x;
        if (std::fabs(err) < 1e-10) return bez(p1.y, p2.y, t);
        double d = bezDeriv(x1, x2, t);
        if (std::fabs(d) < 1e-9) break;
        t -= err / d;
        if (t < 0 || t > 1) break;
    }
    double lo = 0, hi = 1;
    t = x;
    for (int i = 0; i < 60; ++i) {
        double xv = bez(x1, x2, t);
        if (std::fabs(xv - x) < 1e-11) break;
        if (xv < x) lo = t; else hi = t;
        t = 0.5 * (lo + hi);
    }
    return bez(p1.y, p2.y, t);
}

Key& AnimCurve::setKey(double t, double v, Interp interp) {
    for (auto& k : keys_) {
        if (std::fabs(k.t - t) < 1e-9) { k.v = v; k.interpOut = interp; return k; }
    }
    Key k; k.t = t; k.v = v; k.interpOut = interp;
    auto it = std::lower_bound(keys_.begin(), keys_.end(), t, [](const Key& a, double tt) { return a.t < tt; });
    return *keys_.insert(it, k);
}

bool AnimCurve::removeKeyAt(double t, double eps) {
    auto it = std::find_if(keys_.begin(), keys_.end(), [&](const Key& k) { return std::fabs(k.t - t) < eps; });
    if (it == keys_.end()) return false;
    keys_.erase(it);
    return true;
}

void AnimCurve::setSegmentEase(size_t i, EasePreset p) {
    if (i >= keys_.size()) return;
    Key& a = keys_[i];
    Key* b = i + 1 < keys_.size() ? &keys_[i + 1] : nullptr;
    auto setH = [&](Vec2 o, Vec2 in) {
        a.interpOut = Interp::Bezier;
        a.outHandle = o;
        if (b) b->inHandle = in;
    };
    switch (p) {
    case EasePreset::Linear: a.interpOut = Interp::Linear; break;
    case EasePreset::Hold: a.interpOut = Interp::Hold; break;
    case EasePreset::EaseIn: setH({0.42, 0.0}, {1.0, 1.0}); break;
    case EasePreset::EaseOut: setH({0.0, 0.0}, {0.58, 1.0}); break;
    case EasePreset::EaseInOut: setH({0.42, 0.0}, {0.58, 1.0}); break;
    case EasePreset::Punch: setH({0.05, 0.9}, {0.2, 1.0}); break;     // fast attack, long settle
    case EasePreset::Snap: setH({0.9, 0.0}, {0.95, 0.1}); break;      // slow wind-up, sharp hit
    case EasePreset::Overshoot: setH({0.3, 1.6}, {0.6, 1.0}); break;  // elastic-ish overshoot
    }
}

void AnimCurve::scaleTime(double pivot, double f) {
    for (auto& k : keys_) k.t = pivot + (k.t - pivot) * f;
    if (f < 0) std::reverse(keys_.begin(), keys_.end());
}
void AnimCurve::offsetTime(double dt) { for (auto& k : keys_) k.t += dt; }

double AnimCurve::evaluate(double t) const {
    if (keys_.empty()) return constant_;
    if (t <= keys_.front().t) return keys_.front().v;
    if (t >= keys_.back().t) return keys_.back().v;
    auto it = std::upper_bound(keys_.begin(), keys_.end(), t, [](double tt, const Key& k) { return tt < k.t; });
    const Key& b = *it;
    const Key& a = *(it - 1);
    double span = b.t - a.t;
    if (span <= 0) return b.v;
    double x = (t - a.t) / span;
    switch (a.interpOut) {
    case Interp::Hold: return a.v;
    case Interp::Linear: return a.v + (b.v - a.v) * x;
    case Interp::Bezier: return a.v + (b.v - a.v) * cubicBezierEase(x, a.outHandle, b.inHandle);
    }
    return a.v;
}

double AnimCurve::integrate(double a, double b, double maxStep) const {
    if (b < a) return -integrate(b, a, maxStep);
    if (keys_.empty()) return constant_ * (b - a);
    // Split at key times so each piece is smooth (hold segments are exact steps).
    std::vector<double> cuts{a};
    for (auto& k : keys_) if (k.t > a && k.t < b) cuts.push_back(k.t);
    cuts.push_back(b);
    double total = 0;
    for (size_t s = 0; s + 1 < cuts.size(); ++s) {
        double x0 = cuts[s], x1 = cuts[s + 1];
        double len = x1 - x0;
        if (len <= 0) continue;
        int n = std::max(2, static_cast<int>(std::ceil(len / maxStep)));
        if (n % 2) ++n;
        double h = len / n;
        // Evaluate strictly inside the piece to avoid picking the neighbour segment at a hold boundary.
        auto f = [&](int i) {
            double x = x0 + i * h;
            if (i == 0) x = x0 + 1e-12;
            if (i == n) x = x1 - 1e-12;
            return evaluate(x);
        };
        double sum = f(0) + f(n);
        for (int i = 1; i < n; ++i) sum += f(i) * (i % 2 ? 4.0 : 2.0);
        total += sum * h / 3.0;
    }
    return total;
}

static const char* interpName(Interp i) {
    switch (i) { case Interp::Hold: return "hold"; case Interp::Linear: return "linear"; case Interp::Bezier: return "bezier"; }
    return "linear";
}
static Interp interpFrom(const std::string& s) {
    if (s == "hold") return Interp::Hold;
    if (s == "bezier") return Interp::Bezier;
    return Interp::Linear;
}

Json AnimCurve::toJson() const {
    if (keys_.empty()) return Json(constant_);
    Json o = Json::object();
    o.set("value", constant_);
    Json ks = Json::array();
    for (auto& k : keys_) {
        Json kj = Json::object();
        kj.set("t", k.t).set("v", k.v).set("interp", interpName(k.interpOut));
        if (k.interpOut == Interp::Bezier) kj.set("out", JsonArray{k.outHandle.x, k.outHandle.y});
        kj.set("in", JsonArray{k.inHandle.x, k.inHandle.y});
        ks.push(std::move(kj));
    }
    o.set("keys", std::move(ks));
    return o;
}

AnimCurve AnimCurve::fromJson(const Json& j, double def) {
    AnimCurve c(def);
    if (j.isNumber()) { c.constant_ = j.asNumber(); return c; }
    if (!j.isObject()) return c;
    c.constant_ = j.num("value", def);
    if (auto* ks = j.find("keys"); ks && ks->isArray()) {
        for (auto& kj : ks->asArray()) {
            Key k;
            k.t = kj.num("t", 0);
            k.v = kj.num("v", 0);
            k.interpOut = interpFrom(kj.str("interp", "linear"));
            if (auto* o = kj.find("out"); o && o->size() == 2) k.outHandle = {o->at(0).asNumber(), o->at(1).asNumber()};
            if (auto* in = kj.find("in"); in && in->size() == 2) k.inHandle = {in->at(0).asNumber(), in->at(1).asNumber()};
            c.keys_.push_back(k);
        }
        std::stable_sort(c.keys_.begin(), c.keys_.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
    }
    return c;
}

bool AnimCurve::operator==(const AnimCurve& o) const { return toJson() == o.toJson(); }

} // namespace kinetic
