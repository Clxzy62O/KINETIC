#include <filesystem>
#include <fstream>
#include <thread>

#include "Test.h"
#include "edit/Edit.h"
#include "io/Preset.h"
#include "io/Serialize.h"
#include "io/Storage.h"
#include "project/Velocity.h"

using namespace kinetic;
namespace fs = std::filesystem;

static Project sampleProject() {
    Project p;
    p.settings.name = "Test AMV";
    MediaRef m;
    m.id = p.allocId();
    m.name = "a.mp4";
    m.path = "/tmp/a.mp4";
    m.hasVideo = true;
    m.duration = 30;
    m.fps = Rational::fps23976();
    p.media.push_back(m);
    Composition c = makeComposition(p, "Main", 1280, 720, Rational::fps2997(), 900);
    p.rootCompositionId = c.id;
    p.compositions.push_back(c);
    auto& comp = p.compositions.back();
    auto& t = addTrack(p, comp, TrackKind::Video, "V1");
    Clip cl;
    cl.id = p.allocId();
    cl.name = "intro \"quoted\" ✨";
    cl.mediaId = m.id;
    cl.start = 0;
    cl.duration = 120;
    cl.sourceIn = 1.25;
    Velocity::applyPreset(cl.speed, VelocityPreset::AnimeVelocity, 4.0);
    cl.transform.rotation.setKey(0, 0);
    cl.transform.rotation.setKey(1, 15);
    cl.shake.enabled = true;
    cl.shake.amplitude = AnimCurve(20);
    EffectInstance glow{"kinetic.glow"};
    glow.paramOrAdd("intensity", 1.5).setKey(0.5, 3.0);
    cl.effects.push_back(glow);
    cl.blend = BlendMode::Screen;
    t.clips.push_back(cl);
    comp.markers.push_back(Marker{p.allocId(), 30, MarkerKind::StrongBeat, "#FBBF24", "drop", "beats", 0.9});
    return p;
}

TEST(io_project_roundtrip_is_lossless) {
    Project p = sampleProject();
    Json j = io::toJson(p);
    Project q = io::fromJson(Json::parse(j.dump(2)));
    CHECK(io::toJson(q) == j);
    CHECK(q.validate().empty());
    CHECK_EQ(q.compositions[0].fps, Rational::fps2997());
    CHECK(q.compositions[0].tracks[0].clips[0].blend == BlendMode::Screen);
}

TEST(io_migrates_schema1_projects) {
    const char* v1 = R"({"format":"kinetic-project","schemaVersion":1,"nextId":5,"rootComposition":1,
      "media":[],"compositions":[{"id":1,"name":"Old","fps":"24/1","tracks":[{"id":2,"kind":"video","clips":[
        {"id":3,"kind":"solid","start":0,"duration":10,"clipSpeed":2.5,"transform":{"opacity":50}}]}]}]})";
    std::vector<std::string> notes;
    Project p = io::fromJson(Json::parse(v1), &notes);
    const Clip& c = p.compositions[0].tracks[0].clips[0];
    CHECK_NEAR(c.speed.evaluate(0), 2.5, 0);
    CHECK_NEAR(c.transform.opacity.evaluate(0), 0.5, 1e-12);
    CHECK(!notes.empty());
    // A project from the future is refused instead of silently dropping data.
    CHECK_THROWS(io::fromJson(Json::parse(R"({"format":"kinetic-project","schemaVersion":99})")));
}

TEST(io_atomic_save_backups_and_refuses_invalid) {
    std::string path = ktest::tmpDir() + "/proj.kinetic";
    Project p = sampleProject();
    io::saveProject(p, path);
    p.settings.name = "v2";
    io::saveProject(p, path);
    p.settings.name = "v3";
    io::saveProject(p, path);
    CHECK(fs::exists(path + ".bak1"));
    CHECK(fs::exists(path + ".bak2"));
    CHECK_EQ(io::loadProject(path + ".bak1").settings.name, std::string("v2"));
    CHECK_EQ(io::loadProject(path).settings.name, std::string("v3"));
    // Invalid project (overlap): save throws and the good file is untouched.
    Project bad = p;
    Clip dup = bad.compositions[0].tracks[0].clips[0];
    dup.id = bad.allocId();
    dup.start = 10;
    bad.compositions[0].tracks[0].clips.push_back(dup);
    CHECK_THROWS(io::saveProject(bad, path));
    CHECK_EQ(io::loadProject(path).settings.name, std::string("v3"));
    for (auto& e : fs::directory_iterator(ktest::tmpDir()))
        CHECK(e.path().string().find(".tmp-") == std::string::npos);  // no stray temp files
}

TEST(io_recovery_never_offers_incomplete_autosave) {
    std::string path = ktest::tmpDir() + "/rec.kinetic";
    Project p = sampleProject();
    io::saveProject(p, path);
    CHECK(!io::checkRecovery(path).available);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    p.settings.name = "unsaved work";
    io::writeAutosave(p, path);
    auto r = io::checkRecovery(path);
    CHECK(r.available);
    CHECK_EQ(io::loadProject(r.path).settings.name, std::string("unsaved work"));
    // Simulate a crash mid-write of a (non-atomic) autosave: truncated file.
    std::string full = io::readFile(io::autosavePath(path));
    { std::ofstream o(io::autosavePath(path), std::ios::trunc); o << full.substr(0, full.size() / 2); }
    r = io::checkRecovery(path);
    CHECK(!r.available);
    CHECK(r.reason.find("corrupt") != std::string::npos);
    // The real project is still loadable.
    CHECK_EQ(io::loadProject(path).settings.name, std::string("Test AMV"));
    // Saving discards the autosave.
    io::saveProject(p, path);
    CHECK(!fs::exists(io::autosavePath(path)));
}

TEST(io_load_reports_missing_media) {
    std::string path = ktest::tmpDir() + "/missing.kinetic";
    Project p = sampleProject();
    p.media[0].path = "/definitely/not/here.mp4";
    io::saveProject(p, path);
    std::vector<std::string> w;
    io::loadProject(path, &w);
    bool found = false;
    for (auto& s : w) if (s.find("missing media") != std::string::npos) found = true;
    CHECK(found);
}

TEST(io_presets_apply_editable_and_report_missing) {
    Project p = sampleProject();
    const Clip& src = p.compositions[0].tracks[0].clips[0];
    EffectInstance mystery{"thirdparty.unknown", 3};
    std::vector<EffectInstance> stack = src.effects;
    stack.push_back(mystery);
    Json preset = io::makeEffectStackPreset("Impact Pack", "Impact", stack);
    auto avail = [](const std::string& id, int) { return id.rfind("kinetic.", 0) == 0; };
    Clip target;
    io::PresetReport rep;
    Clip out = io::applyPreset(Json::parse(preset.dump()), target, 2.0, avail, rep);
    CHECK_EQ(out.effects.size(), size_t(2));
    CHECK_EQ(rep.missingEffects.size(), size_t(1));
    CHECK_EQ(rep.missingEffects[0], std::string("thirdparty.unknown@3"));
    CHECK_THROWS(io::applyPreset(preset, target, 2.0, avail, rep, false));
    // Keys came across and are editable.
    CHECK(out.effects[0].param("intensity")->animated());

    Json vel = io::makeVelocityPreset("Ramp", src.speed, 4.0);
    Clip v = io::applyPreset(vel, target, 2.0, avail, rep);
    CHECK_NEAR(v.speed.keys().back().t, src.speed.keys().back().t * 0.5, 1e-9);  // time-scaled
    CHECK_NEAR(v.speed.evaluate(1.0), src.speed.evaluate(2.0), 1e-9);
}
