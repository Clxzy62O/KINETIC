#pragma once
// Unified keyframe engine. Every animatable parameter in KINETIC (transform, effect params,
// velocity, shake, masks...) is an AnimCurve.
#include <string>
#include <vector>

#include "core/Json.h"

namespace kinetic {

enum class Interp { Hold, Linear, Bezier };

struct Vec2 { double x = 0, y = 0; };

struct Key {
    double t = 0;              // seconds, clip-local
    double v = 0;
    Interp interpOut = Interp::Linear;  // interpolation of the segment leaving this key
    Vec2 outHandle{1.0 / 3, 1.0 / 3};   // P1 of the outgoing segment, normalized (time, value)
    Vec2 inHandle{2.0 / 3, 2.0 / 3};    // P2 of the incoming segment, normalized (time, value)
};

enum class EasePreset { Linear, EaseIn, EaseOut, EaseInOut, Punch, Snap, Overshoot, Hold };
const char* easePresetName(EasePreset p);
bool easePresetFromName(const std::string& n, EasePreset& out);

class AnimCurve {
public:
    AnimCurve() = default;
    explicit AnimCurve(double constant) : constant_(constant) {}

    bool animated() const { return !keys_.empty(); }
    double constant() const { return constant_; }
    void setConstant(double v) { constant_ = v; }

    const std::vector<Key>& keys() const { return keys_; }
    // Inserts or replaces a key at time t (keys within 1e-9 s are the same key). Keeps keys sorted.
    Key& setKey(double t, double v, Interp interp = Interp::Linear);
    bool removeKeyAt(double t, double eps = 1e-6);
    void clearKeys() { keys_.clear(); }
    // Applies an easing preset to the segment starting at key index i (out of i, into i+1).
    void setSegmentEase(size_t i, EasePreset preset);
    // Scales key timing around a pivot (graph editor "scale timing").
    void scaleTime(double pivot, double factor);
    void offsetTime(double dt);

    double evaluate(double t) const;
    // Definite integral from a to b (used by velocity). Numeric (Simpson) with segment splitting.
    double integrate(double a, double b, double maxStep = 1.0 / 960.0) const;

    Json toJson() const;
    static AnimCurve fromJson(const Json& j, double def = 0.0);

    bool operator==(const AnimCurve& o) const;

private:
    double constant_ = 0;
    std::vector<Key> keys_;
};

// Solves a normalized CSS-style cubic bezier: returns y for x in [0,1].
double cubicBezierEase(double x, Vec2 p1, Vec2 p2);

} // namespace kinetic
