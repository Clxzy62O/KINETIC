#include "Test.h"
#include "edit/BeatSync.h"
#include "edit/Edit.h"
#include "io/Serialize.h"
#include "project/Velocity.h"

using namespace kinetic;

namespace {
struct Fixture {
    Project p;
    CommandStack stack;
    Id comp = 0, v1 = 0, v2 = 0, a1 = 0, media = 0;
    Fixture() {
        MediaRef m;
        m.id = p.allocId();
        m.name = "op.mkv";
        m.path = "/nonexistent/op.mkv";
        m.hasVideo = true;
        m.duration = 90.0;
        m.fps = Rational::fps23976();
        media = m.id;
        p.media.push_back(m);
        Composition c = makeComposition(p, "Main", 1920, 1080, Rational(60, 1), 3600);
        comp = c.id;
        p.compositions.push_back(c);
        p.rootCompositionId = comp;
        auto& cc = *p.findComposition(comp);
        v1 = addTrack(p, cc, TrackKind::Video, "V1").id;
        v2 = addTrack(p, cc, TrackKind::Video, "V2").id;
        a1 = addTrack(p, cc, TrackKind::Audio, "A1").id;
    }
    Id add(Id track, int64_t start, int64_t dur, double srcIn = 0) {
        Clip c;
        c.id = p.allocId();
        c.name = "clip" + std::to_string(c.id);
        c.mediaId = media;
        c.start = start;
        c.duration = dur;
        c.sourceIn = srcIn;
        stack.push(p, edit::addClip(p, track, c));
        stack.endGesture();
        return c.id;
    }
    std::string state() const { Json j = io::toJson(p); j.set("nextId", 0); return j.dump(); }
    const Track& track(Id id) const { return *p.findTrack(id); }
};
} // namespace

TEST(timeline_add_rejects_overlap_and_cycles) {
    Fixture f;
    f.add(f.v1, 0, 60);
    Clip c;
    c.id = f.p.allocId();
    c.mediaId = f.media;
    c.start = 30;
    c.duration = 60;
    CHECK_THROWS(edit::addClip(f.p, f.v1, c));
    c.start = 60;
    f.stack.push(f.p, edit::addClip(f.p, f.v1, c));  // touching is fine
    CHECK_EQ(f.track(f.v1).clips.size(), size_t(2));
    // Nesting the root comp into itself is a cycle.
    Clip n;
    n.id = f.p.allocId();
    n.kind = ClipKind::Composition;
    n.compositionId = f.comp;
    n.start = 200;
    n.duration = 10;
    CHECK_THROWS(edit::addClip(f.p, f.v2, n));
    CHECK(f.p.validate().empty());
}

TEST(timeline_split_preserves_source_continuity_with_velocity) {
    Fixture f;
    Id id = f.add(f.v1, 100, 120, 10.0);  // 2s at 60fps
    {
        Clip c = *f.p.findClipConst(id);
        c.speed = AnimCurve();
        c.speed.setKey(0, 1);
        c.speed.setKey(2, 3);
        f.stack.push(f.p, edit::setClip(f.p, c, "speed"));
    }
    const Clip before = *f.p.findClipConst(id);
    double srcAtSplit = Velocity::sourceTime(before, 1.0);  // frame 160 = 1s into the clip
    f.stack.push(f.p, edit::split(f.p, id, 160));
    const auto& clips = f.track(f.v1).clips;
    CHECK_EQ(clips.size(), size_t(2));
    CHECK_EQ(clips[0].duration, int64_t(60));
    CHECK_EQ(clips[1].start, int64_t(160));
    CHECK_NEAR(clips[1].sourceIn, srcAtSplit, 1e-9);
    // Right half continues the same ramp: speed at its local 0 equals original speed at 1s.
    CHECK_NEAR(clips[1].speed.evaluate(0), before.speed.evaluate(1.0), 1e-9);
    CHECK_NEAR(Velocity::sourceTime(clips[1], 1.0), Velocity::sourceTime(before, 2.0), 1e-6);
    CHECK(f.p.validate().empty());
}

TEST(timeline_undo_redo_restores_exact_state) {
    Fixture f;
    std::string s0 = f.state();
    Id a = f.add(f.v1, 0, 60);
    Id b = f.add(f.v1, 60, 60);
    f.add(f.v1, 120, 60);
    std::string s1 = f.state();
    f.stack.push(f.p, edit::split(f.p, b, 90));
    f.stack.push(f.p, edit::rippleDelete(f.p, a));
    f.stack.push(f.p, edit::trimEnd(f.p, b, 20));
    std::string s2 = f.state();
    CHECK(s1 != s2);
    for (int i = 0; i < 3; ++i) f.stack.undo(f.p);
    CHECK_EQ(f.state(), s1);
    for (int i = 0; i < 3; ++i) f.stack.redo(f.p);
    CHECK_EQ(f.state(), s2);
    while (f.stack.canUndo()) f.stack.undo(f.p);
    CHECK_EQ(f.state(), s0);
}

TEST(timeline_ripple_delete_closes_gap) {
    Fixture f;
    Id a = f.add(f.v1, 0, 50);
    Id b = f.add(f.v1, 50, 30);
    Id c = f.add(f.v1, 100, 10);
    Id other = f.add(f.v2, 60, 10);  // other tracks are untouched
    f.stack.push(f.p, edit::rippleDelete(f.p, a));
    CHECK_EQ(f.p.findClipConst(b)->start, int64_t(0));
    CHECK_EQ(f.p.findClipConst(c)->start, int64_t(50));
    CHECK_EQ(f.p.findClipConst(other)->start, int64_t(60));
}

TEST(timeline_trim_and_roll_and_slip) {
    Fixture f;
    Id a = f.add(f.v1, 0, 120, 5.0);
    Id b = f.add(f.v1, 120, 120, 20.0);
    f.stack.push(f.p, edit::trimStart(f.p, a, 60));  // drop first 1s
    CHECK_EQ(f.p.findClipConst(a)->start, int64_t(60));
    CHECK_NEAR(f.p.findClipConst(a)->sourceIn, 6.0, 1e-9);
    f.stack.push(f.p, edit::rollEdit(f.p, a, b, 150));
    CHECK_EQ(f.p.findClipConst(a)->end(), int64_t(150));
    CHECK_EQ(f.p.findClipConst(b)->start, int64_t(150));
    CHECK_NEAR(f.p.findClipConst(b)->sourceIn, 20.5, 1e-9);
    f.stack.push(f.p, edit::slip(f.p, b, 2.0));
    CHECK_NEAR(f.p.findClipConst(b)->sourceIn, 22.5, 1e-9);
    CHECK_THROWS(edit::slip(f.p, b, -100.0));       // would read before the source start
    CHECK_THROWS(edit::trimEnd(f.p, a, 200));       // overlaps b
    CHECK_THROWS(edit::trimStart(f.p, a, 150));     // empty clip
}

TEST(timeline_drag_merges_into_one_undo_step) {
    Fixture f;
    Id a = f.add(f.v1, 0, 30);
    for (int x = 1; x <= 20; ++x) f.stack.push(f.p, edit::moveClip(f.p, a, x * 5));
    f.stack.endGesture();
    CHECK_EQ(f.p.findClipConst(a)->start, int64_t(100));
    f.stack.undo(f.p);
    CHECK_EQ(f.p.findClipConst(a)->start, int64_t(0));
    f.stack.push(f.p, edit::moveClip(f.p, a, 10, f.v2));  // move across tracks
    CHECK(f.track(f.v1).clips.empty());
    CHECK_EQ(f.track(f.v2).clips.size(), size_t(1));
    CHECK_THROWS(edit::moveClip(f.p, a, 0, f.a1));        // video clip onto an audio track
}

TEST(timeline_locked_track_rejects_edits) {
    Fixture f;
    Id a = f.add(f.v1, 0, 30);
    Track before = f.track(f.v1);
    Track after = before;
    after.locked = true;
    f.stack.push(f.p, std::make_unique<SetTrackStateCmd>(before, after));
    CHECK_THROWS(edit::moveClip(f.p, a, 50));
    CHECK_THROWS(edit::split(f.p, a, 10));
    f.stack.undo(f.p);
    f.stack.push(f.p, edit::moveClip(f.p, a, 50));
}

TEST(timeline_snapping) {
    Fixture f;
    f.add(f.v1, 0, 100);
    auto& comp = *f.p.findComposition(f.comp);
    comp.markers.push_back(Marker{f.p.allocId(), 237, MarkerKind::Beat});
    auto s = edit::snap(comp, 103, 5);
    CHECK(s && s->frame == 100);
    s = edit::snap(comp, 240, 5);
    CHECK(s && s->frame == 237 && s->what == "beat");
    CHECK(!edit::snap(comp, 170, 5));
    s = edit::snap(comp, 170, 5, {}, 172);
    CHECK(s && s->what == "playhead");
}

TEST(beatsync_plans_are_reviewable_then_undoable) {
    Fixture f;
    Id a = f.add(f.v1, 0, 600, 0.0);
    std::vector<beatsync::BeatPoint> beats;
    for (int i = 0; i < 20; ++i) beats.push_back({i * 30, 0.5, i % 4 == 0});
    std::string s0 = f.state();
    auto plan = beatsync::planCutsAtBeats(f.p, a, beats, false);
    CHECK_EQ(f.state(), s0);  // planning does not touch the project
    CHECK_EQ(plan.changes, 19);
    f.stack.push(f.p, std::move(plan.command));
    CHECK_EQ(f.track(f.v1).clips.size(), size_t(20));
    for (size_t i = 0; i < 20; ++i) CHECK_EQ(f.track(f.v1).clips[i].start, int64_t(i * 30));
    CHECK(f.p.validate().empty());
    f.stack.undo(f.p);
    CHECK_EQ(f.state(), s0);

    auto strong = beatsync::planCutsAtBeats(f.p, a, beats, true);
    CHECK_EQ(strong.changes, 4);  // beats 120, 240, 360, 480

    auto markers = beatsync::planBeatMarkers(f.p, f.comp, beats);
    f.stack.push(f.p, std::move(markers.command));
    CHECK_EQ(f.p.findComposition(f.comp)->markers.size(), size_t(20));
    CHECK(f.p.validate().empty());
}

TEST(beatsync_distribute_and_flashes_and_pulses) {
    Fixture f;
    Id a = f.add(f.v1, 0, 100, 0.0);
    Id b = f.add(f.v1, 100, 10, 10.0);
    Id c = f.add(f.v1, 110, 100, 20.0);
    std::vector<beatsync::BeatPoint> beats;
    for (int i = 0; i < 10; ++i) beats.push_back({300 + i * 40, 1.0, true});
    auto plan = beatsync::planDistributeToBeats(f.p, f.v1, {a, b, c}, beats, false);
    f.stack.push(f.p, std::move(plan.command));
    CHECK_EQ(f.p.findClipConst(a)->start, int64_t(300));
    CHECK_EQ(f.p.findClipConst(a)->duration, int64_t(40));  // trimmed to the beat interval
    CHECK_EQ(f.p.findClipConst(b)->start, int64_t(340));
    CHECK_EQ(f.p.findClipConst(b)->duration, int64_t(10));  // shorter clip leaves a gap
    CHECK_EQ(f.p.findClipConst(c)->start, int64_t(380));
    CHECK(f.p.validate().empty());

    auto fl = beatsync::planFlashesOnBeats(f.p, f.v2, beats, 6, {1, 1, 1, 1}, 0.8, false);
    f.stack.push(f.p, std::move(fl.command));
    CHECK_EQ(f.track(f.v2).clips.size(), size_t(10));
    const Clip& flash = f.track(f.v2).clips[0];
    CHECK(flash.kind == ClipKind::Solid);
    CHECK_NEAR(flash.transform.opacity.evaluate(0), 0.8, 1e-9);
    CHECK_NEAR(flash.transform.opacity.evaluate(1.0), 0.0, 1e-9);

    Clip withFx = *f.p.findClipConst(c);
    withFx.effects.push_back(EffectInstance{"kinetic.glow"});
    f.stack.push(f.p, edit::setClip(f.p, withFx, "fx"));
    auto pulse = beatsync::planParamPulseOnBeats(f.p, c, 0, "intensity", beats, 0.2, 2.0, 0.15, false);
    f.stack.push(f.p, std::move(pulse.command));
    const Clip& cc = *f.p.findClipConst(c);
    // clip c: start 380, beat at 380 -> local 0 peak; at 0.1s decaying
    CHECK_NEAR(cc.effects[0].eval("intensity", 0.0, 0), 2.0, 1e-9);
    CHECK(cc.effects[0].eval("intensity", 0.1, 0) < 2.0);
    CHECK_NEAR(cc.effects[0].eval("intensity", 0.3, 0), 0.2, 1e-9);
}
