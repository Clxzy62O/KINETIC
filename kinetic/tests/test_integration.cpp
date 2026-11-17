// End-to-end tests against real FFmpeg decoding/encoding.
#include <atomic>
#include <cmath>
#include <filesystem>

#include "Test.h"
#include "edit/Edit.h"
#include "export/Exporter.h"
#include "media/AudioDecode.h"
#include "media/Decoder.h"
#include "media/Probe.h"
#include "media/Process.h"
#include "render/Evaluator.h"

using namespace kinetic;
namespace fs = std::filesystem;

namespace {
// Lossless video whose every frame encodes its own index in the pixel colour.
std::string counterVideo() {
    std::string path = ktest::tmpDir() + "/counter.mkv";
    if (fs::exists(path)) return path;
    media::runCapture({media::ffmpegPath(), "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                       "color=c=black:s=64x48:r=24000/1001:d=6,format=rgb24,geq=r='mod(N*7,256)':g='mod(N*13,256)':b='mod(N,256)'",
                       "-c:v", "ffv1", "-pix_fmt", "bgr0", path});
    return path;
}
std::string sineWav(double freq, double seconds) {
    std::string path = ktest::tmpDir() + "/sine" + std::to_string(static_cast<int>(freq)) + ".wav";
    media::AudioBuffer b;
    b.sampleRate = 48000;
    b.channels = 2;
    size_t n = static_cast<size_t>(seconds * 48000);
    b.samples.resize(n * 2);
    for (size_t i = 0; i < n; ++i) b.samples[2 * i] = b.samples[2 * i + 1] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * freq * i / 48000.0));
    media::writeWavF32(path, b);
    return path;
}
int zeroCrossings(const std::vector<float>& m, size_t a, size_t b) {
    int z = 0;
    for (size_t i = a + 1; i < b; ++i) z += (m[i - 1] < 0) != (m[i] < 0);
    return z;
}
} // namespace

TEST(integration_probe_counter_video) {
    MediaRef m = media::probe(counterVideo());
    CHECK(m.hasVideo);
    CHECK_EQ(m.width, 64);
    CHECK_EQ(m.height, 48);
    CHECK_EQ(m.fps, Rational::fps23976());
    CHECK_NEAR(m.duration, 6.0, 0.1);
}

TEST(integration_decoder_is_frame_accurate_with_random_access) {
    MediaRef m = media::probe(counterVideo());
    m.id = 1;
    media::FrameProvider fp(64 << 20);
    for (int64_t k : {0, 50, 10, 11, 12, 13, 120, 7, 140, 141, 100}) {
        Image8Ptr img = fp.get(m, k, 64, 48);
        CHECK_EQ(static_cast<int>(img->px[0]), static_cast<int>((k * 7) % 256));
        CHECK_EQ(static_cast<int>(img->px[1]), static_cast<int>((k * 13) % 256));
        CHECK_EQ(static_cast<int>(img->px[2]), static_cast<int>(k % 256));
    }
    // One decoder per (media, size); sequential reads reuse its open pipe.
    CHECK(fp.decoderOpens() == 1);
    CHECK(fp.cache().hits() > 0 || fp.cache().entries() > 10);
}

TEST(integration_frame_cache_respects_budget) {
    MediaRef m = media::probe(counterVideo());
    m.id = 1;
    size_t frameBytes = 64 * 48 * 4;
    media::FrameProvider fp(frameBytes * 10);
    for (int64_t k = 0; k < 60; ++k) fp.get(m, k, 64, 48);
    CHECK(fp.cache().bytesUsed() <= frameBytes * 10);
    CHECK(fp.cache().entries() == 10);
}

TEST(integration_render_media_clip_with_velocity_and_blend) {
    Project p;
    MediaRef m = media::probe(counterVideo());
    m.id = p.allocId();
    p.media.push_back(m);
    Composition c = makeComposition(p, "c", 64, 48, Rational(24000, 1001), 48);
    p.rootCompositionId = c.id;
    p.compositions.push_back(c);
    auto& comp = p.compositions.back();
    Id tr = addTrack(p, comp, TrackKind::Video, "v").id;
    Clip cl;
    cl.id = p.allocId();
    cl.mediaId = m.id;
    cl.duration = 40;
    cl.speed = AnimCurve(2.0);  // comp frame f shows source frame 2f
    p.findTrack(tr)->clips.push_back(cl);
    media::FrameProvider fp(64 << 20);
    render::Renderer r(p, fp);
    for (int64_t f : {0, 5, 17}) {
        Image8 img = r.renderFrame8(c.id, f, {});
        CHECK_EQ(static_cast<int>(img.px[2]), static_cast<int>(2 * f));
    }
    // Frame blending at 0.5x: comp frame 3 = source 1.5 -> mix of frames 1 and 2.
    p.findTrack(tr)->clips[0].speed = AnimCurve(0.5);
    p.findTrack(tr)->clips[0].timeInterp = TimeInterp::Blend;
    Image8 img = r.renderFrame8(c.id, 3, {});
    double expect = linearToSrgb(0.5f * srgbToLinear(1 / 255.f) + 0.5f * srgbToLinear(2 / 255.f)) * 255;
    CHECK_NEAR(img.px[2], expect, 1.0);
}

TEST(integration_audio_mix_varispeed) {
    Project p;
    MediaRef m = media::probe(sineWav(440, 4));
    m.id = p.allocId();
    p.media.push_back(m);
    Composition c = makeComposition(p, "c", 64, 48, Rational(30, 1), 30);
    p.rootCompositionId = c.id;
    p.compositions.push_back(c);
    Id at = addTrack(p, p.compositions.back(), TrackKind::Audio, "a").id;
    Clip cl;
    cl.id = p.allocId();
    cl.mediaId = m.id;
    cl.duration = 30;
    cl.speed = AnimCurve(2.0);
    p.findTrack(at)->clips.push_back(cl);
    auto mix = exporter::mixAudio(p, p.compositions.back(), 0, 30, 48000, nullptr);
    CHECK_EQ(mix.frames(), size_t(48000));
    auto mono = mix.mono();
    // 1 s of 440 Hz at 2x speed -> 880 Hz -> ~1760 zero crossings.
    CHECK_NEAR(zeroCrossings(mono, 0, mono.size()), 1760, 10);
    p.findTrack(at)->muted = true;
    auto silent = exporter::mixAudio(p, p.compositions.back(), 0, 30, 48000, nullptr);
    for (float v : silent.samples) CHECK(v == 0.f);
}

TEST(integration_export_mp4_with_audio_roundtrip) {
    Project p;
    MediaRef v = media::probe(counterVideo());
    v.id = p.allocId();
    MediaRef a = media::probe(sineWav(220, 4));
    a.id = p.allocId();
    p.media = {v, a};
    Composition c = makeComposition(p, "c", 64, 48, Rational(30, 1), 60);
    p.rootCompositionId = c.id;
    p.compositions.push_back(c);
    auto& comp = p.compositions.back();
    Id vt = addTrack(p, comp, TrackKind::Video, "v").id;
    Id at = addTrack(p, comp, TrackKind::Audio, "a").id;
    Clip vc;
    vc.id = p.allocId();
    vc.mediaId = v.id;
    vc.duration = 60;
    vc.speed.setKey(0, 1.0);
    vc.speed.setKey(1.0, 0.25);
    p.findTrack(vt)->clips.push_back(vc);
    Clip ac;
    ac.id = p.allocId();
    ac.mediaId = a.id;
    ac.duration = 60;
    p.findTrack(at)->clips.push_back(ac);
    media::FrameProvider fp(64 << 20);
    exporter::ExportSettings s;
    s.outputPath = ktest::tmpDir() + "/out.mp4";
    s.quality = exporter::Quality::Draft;
    int progressCalls = 0;
    auto res = exporter::exportComposition(p, c.id, s, fp, [&](const exporter::Progress&) { ++progressCalls; });
    CHECK_EQ(res.frames, int64_t(60));
    CHECK_EQ(progressCalls, 60);
    MediaRef out = media::probe(s.outputPath);
    CHECK(out.hasVideo && out.hasAudio);
    CHECK_EQ(out.fps, Rational(30, 1));
    CHECK_NEAR(out.duration, 2.0, 0.1);
    CHECK(!fs::exists(s.outputPath + ".partial.mp4"));
}

TEST(integration_export_cancel_leaves_no_file) {
    Project p;
    Composition c = makeComposition(p, "c", 64, 48, Rational(30, 1), 300);
    p.rootCompositionId = c.id;
    p.compositions.push_back(c);
    media::FrameProvider fp(1 << 20);
    exporter::ExportSettings s;
    s.outputPath = ktest::tmpDir() + "/cancel.mp4";
    std::atomic<bool> cancel{false};
    CHECK_THROWS(exporter::exportComposition(p, c.id, s, fp, [&](const exporter::Progress& pr) { if (pr.done == 10) cancel = true; }, &cancel));
    CHECK(!fs::exists(s.outputPath));
    CHECK(!fs::exists(s.outputPath + ".partial.mp4"));
}
