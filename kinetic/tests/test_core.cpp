#include "Test.h"
#include "anim/Keyframes.h"
#include "core/Json.h"
#include "core/Rational.h"
#include "project/Velocity.h"

using namespace kinetic;

TEST(json_roundtrip_preserves_order_and_values) {
    Json j = Json::parse(R"({"b":1,"a":[1,2.5,-3e-2,true,null],"s":"q\"\\\né😀","o":{}})");
    CHECK_EQ(j.asObject()[0].first, std::string("b"));
    CHECK_NEAR(j["a"].at(1).asNumber(), 2.5, 0);
    CHECK_NEAR(j["a"].at(2).asNumber(), -0.03, 1e-15);
    CHECK_EQ(j["s"].asString(), std::string("q\"\\\n\xC3\xA9\xF0\x9F\x98\x80"));
    Json k = Json::parse(j.dump(2));
    CHECK(j == k);
    CHECK(Json::parse(j.dump()) == j);
}

TEST(json_rejects_malformed) {
    CHECK_THROWS(Json::parse("{\"a\":}"));
    CHECK_THROWS(Json::parse("[1,2"));
    CHECK_THROWS(Json::parse("{} trailing"));
    CHECK_THROWS(Json::parse("\"unterminated"));
    CHECK_THROWS(Json::parse(std::string(1000, '[')));
}

TEST(json_number_precision_roundtrip) {
    for (double d : {0.1, 1.0 / 3.0, 1e-300, 123456789.123456789, -0.000123}) {
        Json j(d);
        CHECK_EQ(Json::parse(j.dump()).asNumber(), d);
    }
}

TEST(rational_ntsc_rates) {
    Rational r = Rational::parse("23.976");
    CHECK_EQ(r, Rational(24000, 1001));
    CHECK_EQ(Rational::parse("30000/1001"), Rational::fps2997());
    CHECK_EQ(Rational::parse("60"), Rational(60, 1));
    // 1001 frames at 23.976 is exactly 41.708333... s; frame round-trip is exact.
    CHECK_EQ(secondsToNearestFrame(framesToSeconds(1001, r), r), int64_t(1001));
}

TEST(keyframes_linear_hold_bezier) {
    AnimCurve c(5);
    CHECK_EQ(c.evaluate(10), 5.0);
    c.setKey(0, 0);
    c.setKey(1, 10);
    c.setKey(2, 20, Interp::Hold);
    c.setKey(3, 0);
    CHECK_NEAR(c.evaluate(0.5), 5, 1e-12);
    CHECK_NEAR(c.evaluate(-1), 0, 0);       // clamps before first key
    CHECK_NEAR(c.evaluate(2.9), 20, 0);     // hold
    CHECK_NEAR(c.evaluate(99), 0, 0);       // clamps after last key
    c.setSegmentEase(0, EasePreset::EaseInOut);
    CHECK_NEAR(c.evaluate(0.5), 5, 1e-6);   // symmetric ease passes through the midpoint
    CHECK(c.evaluate(0.2) < 2.0);           // slow start
    CHECK(c.evaluate(0.8) > 8.0);           // slow end
}

TEST(keyframes_bezier_matches_css_reference) {
    // CSS ease (0.25,0.1,0.25,1) at x=0.5 is ~0.8024 (reference value from browser engines).
    CHECK_NEAR(cubicBezierEase(0.5, {0.25, 0.1}, {0.25, 1.0}), 0.8024033877399112, 1e-6);
    CHECK_NEAR(cubicBezierEase(0.0, {0.25, 0.1}, {0.25, 1.0}), 0, 0);
    CHECK_NEAR(cubicBezierEase(1.0, {0.25, 0.1}, {0.25, 1.0}), 1, 0);
}

TEST(keyframes_integral_exact_for_linear_and_hold) {
    AnimCurve c;
    c.setKey(0, 1);
    c.setKey(1, 3);           // linear 1->3 over 1s: area 2
    CHECK_NEAR(c.integrate(0, 1), 2.0, 1e-9);
    CHECK_NEAR(c.integrate(0, 2), 2.0 + 3.0, 1e-9);  // held at 3 after last key
    CHECK_NEAR(c.integrate(1, 0), -2.0, 1e-9);
    AnimCurve h;
    h.setKey(0, 2, Interp::Hold);
    h.setKey(1, 0);
    CHECK_NEAR(h.integrate(0, 1), 2.0, 1e-9);
    CHECK_NEAR(h.integrate(0, 1.5), 2.0, 1e-9);
}

TEST(keyframes_json_roundtrip) {
    AnimCurve c(1);
    c.setKey(0, 1);
    c.setKey(0.5, 4);
    c.setSegmentEase(0, EasePreset::Punch);
    AnimCurve d = AnimCurve::fromJson(Json::parse(c.toJson().dump()));
    CHECK(c == d);
    for (double t = 0; t <= 0.5; t += 0.05) CHECK_NEAR(c.evaluate(t), d.evaluate(t), 1e-12);
}

TEST(velocity_constant_and_ramp) {
    Clip clip;
    clip.sourceIn = 10;
    clip.speed = AnimCurve(2.0);
    CHECK_NEAR(Velocity::sourceTime(clip, 1.5), 13.0, 1e-9);
    clip.speed = AnimCurve();
    clip.speed.setKey(0, 1);
    clip.speed.setKey(1, 3);
    CHECK_NEAR(Velocity::sourceTime(clip, 1.0), 12.0, 1e-9);
    // Ease-in-out between 1 and 3 still integrates to exactly the average (symmetric curve).
    clip.speed.setSegmentEase(0, EasePreset::EaseInOut);
    CHECK_NEAR(Velocity::sourceTime(clip, 1.0), 12.0, 1e-6);
}

TEST(velocity_reverse_and_freeze) {
    Clip clip;
    clip.sourceIn = 5;
    clip.speed = AnimCurve(-1.0);
    CHECK_NEAR(Velocity::sourceTime(clip, 2), 3.0, 1e-9);
    clip.speed = AnimCurve(0.0);
    CHECK_NEAR(Velocity::sourceTime(clip, 2), 5.0, 1e-12);
    clip.speed = AnimCurve();
    clip.speed.setKey(0, 1, Interp::Hold);
    clip.speed.setKey(1, 0, Interp::Hold);   // freeze after 1s
    CHECK_NEAR(Velocity::sourceTime(clip, 3), 6.0, 1e-9);
}

TEST(velocity_presets_are_editable_keys) {
    for (auto p : allVelocityPresets()) {
        AnimCurve s;
        Velocity::applyPreset(s, p, 2.0, 1.0, 0.5);
        if (p == VelocityPreset::Constant || p == VelocityPreset::SlowMotion || p == VelocityPreset::FastMotion) {
            CHECK(!s.animated());
        } else {
            CHECK(s.keys().size() >= 2);
            CHECK_NEAR(s.keys().front().t, 0, 1e-12);
            CHECK(s.keys().back().t <= 2.0 + 1e-9);
            s.setKey(1.0, 0.7);   // user can edit it like any curve
        }
        VelocityPreset back;
        CHECK(velocityPresetFromName(velocityPresetName(p), back) && back == p);
    }
    AnimCurve s;
    Velocity::applyPreset(s, VelocityPreset::ImpactRamp, 2.0, 1.0, 0.5);
    CHECK(s.evaluate(1.0) < 0.2);   // near-freeze at the impact point
    CHECK(s.evaluate(0.0) > 2.0);
}
