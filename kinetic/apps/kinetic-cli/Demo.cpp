#include "Demo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "audio/BeatDetect.h"
#include "edit/BeatSync.h"
#include "edit/Edit.h"
#include "export/Exporter.h"
#include "io/Storage.h"
#include "media/AudioDecode.h"
#include "media/Probe.h"
#include "media/Process.h"
#include "project/Velocity.h"
#include "render/Effects.h"

namespace fs = std::filesystem;

namespace kinetic::cli {

namespace {
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    double next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<double>(s >> 11) / 9007199254740992.0; }
    double sym() { return next() * 2 - 1; }
};
constexpr double kPi = 3.14159265358979323846;
}

void synthesizeTrack(const std::string& path, double bpm, double seconds, double dropAt, int sr) {
    media::AudioBuffer buf;
    buf.sampleRate = sr;
    buf.channels = 2;
    size_t N = static_cast<size_t>(seconds * sr);
    std::vector<float> L(N, 0.f), R(N, 0.f);
    Rng rng(42);
    const double beat = 60.0 / bpm;
    const double buildStart = std::max(0.0, dropAt - 8 * beat);
    auto add = [&](size_t i, double v, double pan = 0) {
        if (i >= N) return;
        L[i] += static_cast<float>(v * (1 - pan) );
        R[i] += static_cast<float>(v * (1 + pan));
    };
    auto kick = [&](double t, double gain) {
        size_t s0 = static_cast<size_t>(t * sr);
        double ph = 0;
        for (size_t k = 0; k < static_cast<size_t>(0.35 * sr); ++k) {
            double u = static_cast<double>(k) / sr;
            double f = 48 + 110 * std::exp(-u * 28);
            ph += 2 * kPi * f / sr;
            add(s0 + k, gain * std::sin(ph) * std::exp(-u * 9) + gain * 0.3 * std::exp(-u * 300) * rng.sym());
        }
    };
    auto snare = [&](double t, double gain) {
        size_t s0 = static_cast<size_t>(t * sr);
        for (size_t k = 0; k < static_cast<size_t>(0.2 * sr); ++k) {
            double u = static_cast<double>(k) / sr;
            add(s0 + k, gain * (0.6 * rng.sym() * std::exp(-u * 22) + 0.4 * std::sin(2 * kPi * 190 * u) * std::exp(-u * 30)));
        }
    };
    double hpPrev = 0, xPrev = 0;
    auto hat = [&](double t, double gain, double pan) {
        size_t s0 = static_cast<size_t>(t * sr);
        for (size_t k = 0; k < static_cast<size_t>(0.05 * sr); ++k) {
            double u = static_cast<double>(k) / sr;
            double x = rng.sym();
            double hp = 0.95 * (hpPrev + x - xPrev);  // crude high-pass
            hpPrev = hp; xPrev = x;
            add(s0 + k, gain * hp * std::exp(-u * 90), pan);
        }
    };
    int totalBeats = static_cast<int>(seconds / beat) + 1;
    for (int b = 0; b < totalBeats; ++b) {
        double t = b * beat;
        if (t >= seconds) break;
        bool down = b % 4 == 0;
        if (t < buildStart) {                       // intro: half-time kick, soft hats
            if (b % 2 == 0) kick(t, down ? 0.45 : 0.3);
            hat(t + beat / 2, 0.08, 0.3);
        } else if (t < dropAt) {                    // build: snare roll accelerating, no kick
            double prog = (t - buildStart) / std::max(1e-6, dropAt - buildStart);
            int div = prog < 0.5 ? 1 : prog < 0.75 ? 2 : 4;
            for (int k = 0; k < div; ++k) snare(t + k * beat / div, 0.15 + 0.35 * prog);
            hat(t, 0.06, -0.3);
        } else {                                    // drop: four-on-the-floor
            kick(t, down ? 0.95 : 0.75);
            if (b % 2 == 1) snare(t, 0.55);
            for (int k = 0; k < 4; ++k) hat(t + k * beat / 4, k == 2 ? 0.14 : 0.07, k % 2 ? 0.4 : -0.4);
        }
    }
    // Pad (intro + build) and bass (drop); riser noise during the build.
    for (size_t i = 0; i < N; ++i) {
        double t = static_cast<double>(i) / sr;
        if (t < dropAt) {
            double pad = 0.035 * (std::sin(2 * kPi * 220 * t) + std::sin(2 * kPi * 277.2 * t) + std::sin(2 * kPi * 329.6 * t));
            add(i, pad * std::min(1.0, t / 1.5));
            if (t >= buildStart) {
                double prog = (t - buildStart) / std::max(1e-6, dropAt - buildStart);
                add(i, 0.12 * prog * prog * rng.sym());
            }
        } else {
            double inBeat = std::fmod(t - dropAt, beat) / beat;
            double duck = std::min(1.0, inBeat * 4);  // sidechain feel
            double saw = 2 * std::fmod(55.0 * t, 1.0) - 1;
            add(i, 0.16 * saw * duck);
        }
    }
    float peak = 1e-6f;
    for (size_t i = 0; i < N; ++i) peak = std::max({peak, std::fabs(L[i]), std::fabs(R[i])});
    buf.samples.resize(N * 2);
    for (size_t i = 0; i < N; ++i) { buf.samples[2 * i] = L[i] / peak * 0.89f; buf.samples[2 * i + 1] = R[i] / peak * 0.89f; }
    media::writeWavF32(path, buf);
}

static void genVideo(const std::string& path, const std::string& lavfi, double seconds) {
    if (fs::exists(path)) return;
    media::runCapture({media::ffmpegPath(), "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", lavfi,
                       "-t", std::to_string(seconds), "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-g", "48",
                       "-pix_fmt", "yuv420p", path});
}

static MediaRef importMedia(Project& p, CommandStack& st, const std::string& path) {
    MediaRef m = media::probe(path);
    m.id = p.nextId;
    MediaRef copy = m;
    auto macro = std::make_unique<MacroCommand>("Import " + m.name);
    macro->add(std::make_unique<SetNextIdCmd>(p.nextId, p.nextId + 1));
    macro->add(std::make_unique<AddMediaCmd>(m));
    st.push(p, std::move(macro));
    st.endGesture();
    return copy;
}

int runDemo(const std::string& dir, double seconds, int W, int H, bool doRender) {
    fs::create_directories(dir);
    auto& reg = render::EffectRegistry::instance();
    const double bpm = 128, beat = 60.0 / bpm;
    const double dropAt = std::round((seconds * 0.45) / beat) * beat;  // drop on a beat ~45% in

    std::cout << "[1/6] Generating source media (FFmpeg lavfi, H.264 long-GOP like real anime rips)...\n";
    const std::string media = dir + "/media";
    fs::create_directories(media);
    double srcLen = seconds + 8;
    std::string sz = std::to_string(W) + "x" + std::to_string(H);
    genVideo(media + "/mandelbrot.mp4", "mandelbrot=size=" + sz + ":rate=24000/1001:inner=convergence", srcLen);
    genVideo(media + "/life.mp4", "life=size=" + std::to_string(W / 4) + "x" + std::to_string(H / 4) +
             ":rate=24000/1001:ratio=0.12:mold=8:life_color=#A78BFA:death_color=#101114:mold_color=#38BDF8,scale=" + sz + ":flags=neighbor", srcLen);
    genVideo(media + "/cells.mp4", "cellauto=size=" + std::to_string(W / 4) + "x" + std::to_string(H / 4) +
             ":rate=24000/1001:rule=110:random_fill_ratio=0.5,scale=" + sz + ":flags=neighbor,negate,colorchannelmixer=rr=0.65:gg=0.55:bb=0.98", srcLen);
    genVideo(media + "/bars.mp4", "testsrc2=size=" + sz + ":rate=24000/1001", srcLen);
    const std::string song = media + "/track.wav";
    synthesizeTrack(song, bpm, seconds, dropAt);

    std::cout << "[2/6] Building project (every change goes through the undo stack)...\n";
    Project p;
    p.settings.name = "KINETIC Demo AMV";
    CommandStack st;
    std::vector<MediaRef> vids;
    for (auto n : {"mandelbrot.mp4", "life.mp4", "cells.mp4", "bars.mp4"}) vids.push_back(importMedia(p, st, media + "/" + n));
    MediaRef music = importMedia(p, st, song);

    const Rational fps(30, 1);
    const int64_t frames = static_cast<int64_t>(std::llround(seconds * fps.toDouble()));
    Composition main = makeComposition(p, "Main", W, H, fps, frames);
    main.background = {0.063, 0.067, 0.078, 1};
    Id mainId = main.id;
    Id vFootage = p.allocId(), vImpact = p.allocId(), vBuild = p.allocId(), vFlash = p.allocId(), vGrade = p.allocId(), aMusic = p.allocId();
    auto mkTrack = [](Id id, TrackKind k, const char* n) { Track t; t.id = id; t.kind = k; t.name = n; if (k == TrackKind::Audio) t.color = "#4ADE80"; return t; };
    main.tracks = {mkTrack(vFootage, TrackKind::Video, "Footage"), mkTrack(vImpact, TrackKind::Video, "Impact"),
                   mkTrack(vBuild, TrackKind::Video, "Build FX"), mkTrack(vFlash, TrackKind::Video, "Flashes"),
                   mkTrack(vGrade, TrackKind::Video, "Grade"), mkTrack(aMusic, TrackKind::Audio, "Music")};
    p.rootCompositionId = mainId;
    st.push(p, std::make_unique<AddCompositionCmd>(main));
    st.endGesture();

    {   // Music on A1
        Clip c;
        c.id = p.allocId();
        c.name = music.name;
        c.mediaId = music.id;
        c.start = 0;
        c.duration = frames;
        st.push(p, edit::addClip(p, aMusic, c));
    }

    std::cout << "[3/6] Analyzing the track (onsets, tempo, beat grid, downbeats, energy)...\n";
    auto audio = media::decodeAudio(song, 48000, 2);
    auto analysis = audio::analyzeBeats(audio.mono(), audio.sampleRate);
    std::cout << "      tempo " << analysis.tempo << " BPM (true " << bpm << "), " << analysis.beats.size() << " beats, "
              << analysis.onsets.size() << " onsets";
    double detectedDrop = -1;
    for (auto& e : analysis.events) if (e.second == "drop") { detectedDrop = e.first; break; }
    std::cout << ", drop detected at " << detectedDrop << " s (true " << dropAt << " s)\n";
    std::vector<beatsync::BeatPoint> beats;
    for (auto& b : analysis.beats) beats.push_back({secondsToNearestFrame(b.time, fps), b.strength, b.strong});
    auto markers = beatsync::planBeatMarkers(p, mainId, beats);
    if (markers.command) st.push(p, std::move(markers.command));
    st.endGesture();

    double drop = detectedDrop > 0 ? detectedDrop : dropAt;
    int64_t dropF = secondsToNearestFrame(drop, fps);
    int64_t buildF = secondsToNearestFrame(std::max(0.0, drop - 8 * beat), fps);

    std::cout << "[4/6] Cutting footage to the beat (strong beats in the intro, every beat after the drop)...\n";
    std::vector<beatsync::BeatPoint> cuts{{0, 1.0, true}};
    for (auto& b : beats) {
        if (b.frame <= 0 || b.frame >= frames) continue;
        if (b.frame < dropF ? b.strong : true) cuts.push_back({b.frame, b.strength, true});
    }
    if (std::none_of(cuts.begin(), cuts.end(), [&](auto& b) { return b.frame == dropF; })) cuts.push_back({dropF, 1, true});
    std::sort(cuts.begin(), cuts.end(), [](auto& a, auto& b) { return a.frame < b.frame; });
    cuts.push_back({frames, 1, true});
    std::vector<Id> clipIds;
    Rng rng(7);
    {
        int64_t parking = frames + 1000;  // stage clips past the end, then distribute them onto the beats
        for (size_t i = 0; i + 1 < cuts.size(); ++i) {
            const MediaRef& m = vids[i % vids.size()];
            Clip c;
            c.id = p.allocId();
            c.name = m.name.substr(0, m.name.find('.')) + " " + std::to_string(i + 1);
            c.mediaId = m.id;
            c.duration = cuts[i + 1].frame - cuts[i].frame;
            c.start = parking;
            parking += c.duration + 10;
            c.sourceIn = 0.5 + rng.next() * 3.0;
            st.push(p, edit::addClip(p, vFootage, c));
            st.endGesture();
            clipIds.push_back(c.id);
        }
        auto plan = beatsync::planDistributeToBeats(p, vFootage, clipIds, cuts, false, 0);
        std::cout << "      plan: " << plan.summary.size() << " placements, e.g. " << plan.summary.front() << "\n";
        st.push(p, std::move(plan.command));
        st.endGesture();
    }

    std::cout << "[5/6] Velocity ramps, shake, glow pulses, RGB split, flashes, speed lines, impact comp, grade...\n";
    for (Id id : clipIds) {
        Clip c = *p.findClipConst(id);
        double dur = framesToSeconds(c.duration, fps);
        bool afterDrop = c.start >= dropF;
        bool inBuild = c.start >= buildF && c.start < dropF;
        if (afterDrop) {
            Velocity::applyPreset(c.speed, (c.start / 7) % 2 ? VelocityPreset::AnimeVelocity : VelocityPreset::ImpactRamp, dur, 1.0, 0.45);
            c.timeInterp = TimeInterp::Blend;  // frame blending through the slow part of the ramp
            auto glow = reg.instantiate("kinetic.glow");
            glow.param("threshold")->setConstant(0.55);
            glow.param("radius")->setConstant(14);
            c.effects.push_back(glow);
            auto split = reg.instantiate("kinetic.rgbSplit");
            split.param("angle")->setConstant(20);
            c.effects.push_back(split);
            c.shake.frequency = 14;
            c.shake.rotation = 2.0;
            c.shake.zoom = 0.04;
            c.shake.seed = c.id;
            // Punch-in on every cut.
            c.transform.scale.setKey(0, 1.12);
            c.transform.scale.setKey(std::min(dur, 0.25), 1.0);
            c.transform.scale.setSegmentEase(0, EasePreset::EaseOut);
        } else if (inBuild) {
            Velocity::applyPreset(c.speed, VelocityPreset::FastAcceleration, dur, 0.8);
        } else {
            Velocity::applyPreset(c.speed, VelocityPreset::SmoothSlowdown, dur, 1.0);
            c.timeInterp = TimeInterp::Blend;
        }
        st.push(p, edit::setClip(p, c, "Style clip"));
        st.endGesture();
        if (afterDrop) {
            auto shake = beatsync::planShakeOnBeats(p, id, beats, 26, 0.18, false);
            st.push(p, std::move(shake.command));
            auto pulse = beatsync::planParamPulseOnBeats(p, id, 0, "intensity", beats, 0.5, 2.2, 0.2, false);
            st.push(p, std::move(pulse.command));
            auto rgb = beatsync::planParamPulseOnBeats(p, id, 1, "amount", beats, 0.0, 18.0, 0.12, false);
            st.push(p, std::move(rgb.command));
            st.endGesture();
        }
    }

    // Nested "Impact" composition: punch-in zoom + directional blur + white flash, one beat long.
    {
        int64_t len = std::max<int64_t>(4, secondsToNearestFrame(beat, fps));
        Composition imp = makeComposition(p, "Impact", W, H, fps, len);
        Id impTrack = p.allocId(), flashTrack = p.allocId();
        imp.tracks = {mkTrack(impTrack, TrackKind::Video, "Shot"), mkTrack(flashTrack, TrackKind::Video, "Flash")};
        Id impId = imp.id;
        st.push(p, std::make_unique<AddCompositionCmd>(imp));
        Clip shot;
        shot.id = p.allocId();
        shot.name = "impact shot";
        shot.mediaId = vids[0].id;
        shot.start = 0;
        shot.duration = len;
        shot.sourceIn = 6.0;
        shot.speed = AnimCurve(0.35);  // slow-mo hit
        shot.timeInterp = TimeInterp::Blend;
        shot.transform.scale.setKey(0, 1.6);
        shot.transform.scale.setKey(framesToSeconds(len, fps), 1.05);
        shot.transform.scale.setSegmentEase(0, EasePreset::Punch);
        auto dblur = reg.instantiate("kinetic.directionalBlur");
        dblur.param("length")->setKey(0, 90);
        dblur.param("length")->setKey(0.2, 0);
        dblur.param("angle")->setConstant(-15);
        shot.effects.push_back(dblur);
        auto lines = reg.instantiate("kinetic.speedLines");
        lines.param("opacity")->setKey(0, 0.9);
        lines.param("opacity")->setKey(framesToSeconds(len, fps), 0.0);
        shot.effects.push_back(lines);
        st.push(p, edit::addClip(p, impTrack, shot));
        Clip flash;
        flash.id = p.allocId();
        flash.name = "white flash";
        flash.kind = ClipKind::Solid;
        flash.solidColor = {1, 1, 1, 1};
        flash.start = 0;
        flash.duration = std::min<int64_t>(len, 6);
        flash.blend = BlendMode::Add;
        flash.transform.opacity.setKey(0, 1.0);
        flash.transform.opacity.setKey(framesToSeconds(flash.duration, fps), 0.0);
        flash.transform.opacity.setSegmentEase(0, EasePreset::EaseOut);
        st.push(p, edit::addClip(p, flashTrack, flash));
        Clip nest;
        nest.id = p.allocId();
        nest.name = "Impact (nested)";
        nest.kind = ClipKind::Composition;
        nest.compositionId = impId;
        nest.start = dropF;
        nest.duration = std::min(len, frames - dropF);
        st.push(p, edit::addClip(p, vImpact, nest));
        st.endGesture();
    }

    // Build-up adjustment layer: zoom blur + speed lines ramping toward the drop.
    if (dropF - buildF > 4) {
        Clip adj;
        adj.id = p.allocId();
        adj.name = "Build-up FX";
        adj.kind = ClipKind::Adjustment;
        adj.start = buildF + (dropF - buildF) / 2;
        adj.duration = dropF - adj.start;
        double d = framesToSeconds(adj.duration, fps);
        auto zb = reg.instantiate("kinetic.zoomBlur");
        zb.param("amount")->setKey(0, 0.0);
        zb.param("amount")->setKey(d, 0.35);
        zb.param("amount")->setSegmentEase(0, EasePreset::EaseIn);
        adj.effects.push_back(zb);
        auto sl = reg.instantiate("kinetic.speedLines");
        sl.param("opacity")->setKey(0, 0.0);
        sl.param("opacity")->setKey(d, 0.75);
        adj.effects.push_back(sl);
        st.push(p, edit::addClip(p, vBuild, adj));
        st.endGesture();
    }

    // White flashes on the strong beats after the drop.
    {
        std::vector<beatsync::BeatPoint> after;
        for (auto& b : beats) if (b.frame > dropF + secondsToNearestFrame(beat, fps)) after.push_back(b);
        auto fl = beatsync::planFlashesOnBeats(p, vFlash, after, 5, {1, 1, 1, 1}, 0.55, true);
        if (fl.command) st.push(p, std::move(fl.command));
        st.endGesture();
    }

    // Global grade on an adjustment layer (always last = top).
    {
        Clip g;
        g.id = p.allocId();
        g.name = "Anime Grade";
        g.kind = ClipKind::Adjustment;
        g.start = 0;
        g.duration = frames;
        auto col = reg.instantiate("kinetic.color");
        col.param("contrast")->setConstant(1.12);
        col.param("saturation")->setConstant(1.25);
        col.param("temperature")->setConstant(-0.12);
        g.effects.push_back(col);
        auto vig = reg.instantiate("kinetic.vignette");
        vig.param("amount")->setConstant(0.45);
        g.effects.push_back(vig);
        auto grain = reg.instantiate("kinetic.filmGrain");
        grain.param("amount")->setConstant(0.06);
        g.effects.push_back(grain);
        st.push(p, edit::addClip(p, vGrade, g));
        st.endGesture();
    }

    auto errs = p.validate();
    if (!errs.empty()) { std::cerr << "project invalid: " << errs.front() << "\n"; return 1; }
    std::string projPath = dir + "/demo.kinetic";
    io::saveProject(p, projPath);
    {
        Json aj = analysis.toJson();
        std::ofstream(dir + "/demo.beats.json") << aj.dump(2);
    }
    std::cout << "      saved " << projPath << " (" << st.size() << " undoable actions, "
              << p.findComposition(mainId)->markers.size() << " beat markers)\n";

    if (!doRender) return 0;
    std::cout << "[6/6] Rendering " << frames << " frames at " << W << "x" << H << " (CPU reference renderer)...\n";
    media::FrameProvider fp(size_t(1) << 30);
    exporter::ExportSettings es;
    es.outputPath = dir + "/demo.mp4";
    es.quality = exporter::Quality::High;
    auto res = exporter::exportComposition(p, mainId, es, fp, [](const exporter::Progress& pr) {
        if (pr.done % 15 == 0 || pr.done == pr.total)
            std::cout << "\r      " << pr.done << "/" << pr.total << " frames  " << static_cast<int>(pr.framesPerSecond * 10) / 10.0
                      << " fps  ETA " << static_cast<int>(pr.etaSeconds) << " s   " << std::flush;
    });
    std::cout << "\n      wrote " << res.outputPath << " with " << res.encoder << " in " << static_cast<int>(res.seconds) << " s\n";
    for (auto& w : res.warnings) std::cout << "      warning: " << w << "\n";
    return 0;
}

} // namespace kinetic::cli
