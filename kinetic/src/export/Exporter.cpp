#include "export/Exporter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>

#include "core/Platform.h"
#include "media/Process.h"
#include "project/Velocity.h"
#include "render/Evaluator.h"

namespace fs = std::filesystem;

namespace kinetic::exporter {

const char* toString(Quality q) {
    switch (q) {
    case Quality::Draft: return "draft";
    case Quality::Balanced: return "balanced";
    case Quality::High: return "high";
    case Quality::Maximum: return "maximum";
    }
    return "high";
}
bool parseQuality(const std::string& s, Quality& q) {
    for (Quality v : {Quality::Draft, Quality::Balanced, Quality::High, Quality::Maximum})
        if (s == toString(v)) { q = v; return true; }
    return false;
}

static const std::string& encoderList() {
    static std::once_flag once;
    static std::string list;
    std::call_once(once, [] {
        try { list = media::runCapture({media::ffmpegPath(), "-hide_banner", "-encoders"}); } catch (...) {}
    });
    return list;
}
static bool hasEncoder(const std::string& name) {
    return encoderList().find(" " + name + " ") != std::string::npos;
}

std::string chooseEncoder(const std::string& container, const std::string& requested) {
    if (requested != "auto") {
        if (!hasEncoder(requested)) throw ExportError("encoder '" + requested + "' is not available in this FFmpeg build");
        return requested;
    }
    std::vector<std::string> prefs;
    if (container == "webm") prefs = {"libvpx-vp9", "libvpx"};
    else if (container == "png" || container == "jpg" || container == "jpeg") prefs = {container == "png" ? "png" : "mjpeg"};
    else prefs = {"libx264", "h264_videotoolbox", "h264_nvenc", "h264_qsv", "h264_amf", "libopenh264", "mpeg4"};
    for (auto& e : prefs) if (hasEncoder(e)) return e;
    throw ExportError("no suitable video encoder found for ." + container);
}

static std::vector<std::string> encoderArgs(const std::string& enc, Quality q) {
    int idx = static_cast<int>(q);
    if (enc == "libx264") {
        const char* crf[] = {"28", "23", "18", "14"};
        const char* preset[] = {"veryfast", "fast", "medium", "slow"};
        return {"-c:v", enc, "-crf", crf[idx], "-preset", preset[idx], "-pix_fmt", "yuv420p"};
    }
    if (enc == "libvpx-vp9" || enc == "libvpx") {
        const char* crf[] = {"40", "34", "28", "20"};
        return {"-c:v", enc, "-crf", crf[idx], "-b:v", "0", "-row-mt", "1", "-pix_fmt", "yuv420p"};
    }
    if (enc == "prores_ks") return {"-c:v", enc, "-profile:v", idx >= 2 ? "3" : "2", "-pix_fmt", "yuv422p10le"};
    if (enc == "png") return {"-c:v", "png"};
    if (enc == "mjpeg") return {"-c:v", "mjpeg", "-q:v", idx >= 2 ? "2" : "5"};
    if (enc == "mpeg4") {
        const char* qv[] = {"8", "5", "3", "2"};
        return {"-c:v", enc, "-q:v", qv[idx], "-pix_fmt", "yuv420p"};
    }
    // Hardware / other encoders: bitrate-based defaults.
    const char* br[] = {"8M", "16M", "30M", "60M"};
    return {"-c:v", enc, "-b:v", br[idx], "-pix_fmt", "yuv420p"};
}

// ---------------------------------------------------------------- audio mix
media::AudioBuffer mixAudio(const Project& p, const Composition& c, int64_t f0, int64_t f1, int sr,
                            std::vector<std::string>* warnings) {
    media::AudioBuffer out;
    out.sampleRate = sr;
    out.channels = 2;
    double t0 = framesToSeconds(f0, c.fps), t1 = framesToSeconds(f1, c.fps);
    size_t total = static_cast<size_t>(std::llround((t1 - t0) * sr));
    out.samples.assign(total * 2, 0.f);
    std::map<Id, media::AudioBuffer> decoded;
    bool anySolo = std::any_of(c.tracks.begin(), c.tracks.end(), [](const Track& t) { return t.kind == TrackKind::Audio && t.solo; });
    for (const Track& t : c.tracks) {
        if (t.kind != TrackKind::Audio || t.muted || (anySolo && !t.solo)) continue;
        for (const Clip& clip : t.clips) {
            if (!clip.enabled || clip.end() <= f0 || clip.start >= f1) continue;
            const MediaRef* m = p.findMedia(clip.mediaId);
            if (clip.kind != ClipKind::Media || !m || !m->hasAudio) continue;
            auto it = decoded.find(m->id);
            if (it == decoded.end()) {
                if (!fs::exists(m->path)) { if (warnings) warnings->push_back("missing audio media '" + m->name + "' (silence rendered)"); continue; }
                it = decoded.emplace(m->id, media::decodeAudio(m->path, sr, 2)).first;
            }
            const media::AudioBuffer& src = it->second;
            const size_t srcFrames = src.frames();
            float gain = static_cast<float>(std::pow(10.0, clip.gainDb / 20.0));
            int64_t a = std::max(clip.start, f0), b = std::min(clip.end(), f1);
            // Source time is exact at every video-frame boundary; linear within a frame (varispeed).
            for (int64_t f = a; f < b; ++f) {
                double lt0 = framesToSeconds(f - clip.start, c.fps), lt1 = framesToSeconds(f + 1 - clip.start, c.fps);
                double s0 = Velocity::sourceTime(clip, lt0), s1 = Velocity::sourceTime(clip, lt1);
                size_t o0 = static_cast<size_t>(std::llround((framesToSeconds(f, c.fps) - t0) * sr));
                size_t o1 = std::min(total, static_cast<size_t>(std::llround((framesToSeconds(f + 1, c.fps) - t0) * sr)));
                for (size_t o = o0; o < o1; ++o) {
                    double u = o1 > o0 ? static_cast<double>(o - o0) / static_cast<double>(o1 - o0) : 0;
                    double sp = (s0 + (s1 - s0) * u) * sr;
                    if (sp < 0) continue;
                    size_t i = static_cast<size_t>(sp);
                    if (i + 1 >= srcFrames) continue;
                    float w = static_cast<float>(sp - static_cast<double>(i));
                    for (int ch = 0; ch < 2; ++ch) {
                        float v = src.samples[i * 2 + ch] * (1 - w) + src.samples[(i + 1) * 2 + ch] * w;
                        out.samples[o * 2 + ch] += v * gain;
                    }
                }
            }
        }
    }
    float peak = 0;
    for (float v : out.samples) peak = std::max(peak, std::fabs(v));
    if (peak > 1.0f && warnings) warnings->push_back("audio mix peaks at " + std::to_string(20 * std::log10(peak)).substr(0, 5) + " dBFS (clipping)");
    return out;
}

// ---------------------------------------------------------------- export
ExportResult exportComposition(const Project& p, Id compId, const ExportSettings& s, media::FrameProvider& frames,
                               const std::function<void(const Progress&)>& onProgress, const std::atomic<bool>* cancel) {
    const Composition* comp = p.findComposition(compId);
    if (!comp) throw ExportError("composition not found");
    auto errs = p.validate();
    if (!errs.empty()) throw ExportError("project is invalid: " + errs.front());
    int64_t f0 = std::max<int64_t>(0, s.startFrame);
    int64_t f1 = s.endFrame < 0 ? comp->durationFrames : std::min(s.endFrame, comp->durationFrames);
    if (f1 <= f0) throw ExportError("empty export range");

    ExportResult res;
    res.outputPath = s.outputPath;
    std::string ext = fs::path(s.outputPath).extension().string();
    if (!ext.empty()) ext = ext.substr(1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    bool isSequence = s.outputPath.find('%') != std::string::npos;
    auto t0 = std::chrono::steady_clock::now();

    // Audio first (also needed for .wav-only export).
    std::string wavTmp;
    bool haveAudio = false;
    if (s.includeAudio && !isSequence) {
        media::AudioBuffer mix = mixAudio(p, *comp, f0, f1, s.sampleRate, &res.warnings);
        bool silent = std::all_of(mix.samples.begin(), mix.samples.end(), [](float v) { return v == 0.f; });
        if (ext == "wav") {
            media::writeWavF32(s.outputPath + ".partial.wav", mix);
            fs::rename(s.outputPath + ".partial.wav", s.outputPath);
            res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            res.encoder = "pcm_f32le";
            return res;
        }
        if (!silent) {
            wavTmp = (fs::temp_directory_path() / ("kinetic-mix-" + std::to_string(platform::processId()) + ".wav")).string();
            media::writeWavF32(wavTmp, mix);
            haveAudio = true;
        }
    }

    int W = s.width > 0 ? s.width : comp->width, H = s.height > 0 ? s.height : comp->height;
    double scaleX = static_cast<double>(W) / comp->width, scaleY = static_cast<double>(H) / comp->height;
    if (std::fabs(scaleX - scaleY) > 1e-3) throw ExportError("export size must keep the composition aspect ratio");
    if (scaleX > 1.0) throw ExportError("export larger than the composition is not supported; resize the composition instead");
    render::RenderSettings rs;
    rs.scale = scaleX;

    std::string enc = chooseEncoder(isSequence ? ext : ext, s.videoCodec);
    res.encoder = enc;
    // Write to a temporary name; a failed or cancelled export never leaves a broken file behind.
    std::string partial = isSequence ? s.outputPath : s.outputPath + ".partial." + ext;
    std::vector<std::string> argv{media::ffmpegPath(), "-hide_banner", "-loglevel", "error", "-y",
                                  "-f", "rawvideo", "-pix_fmt", "rgba", "-s", std::to_string(static_cast<int>(std::lround(comp->width * rs.scale))) + "x" +
                                  std::to_string(static_cast<int>(std::lround(comp->height * rs.scale))),
                                  "-r", comp->fps.str(), "-i", "pipe:0"};
    if (haveAudio) argv.insert(argv.end(), {"-i", wavTmp});
    argv.insert(argv.end(), {"-map", "0:v:0"});
    if (haveAudio) argv.insert(argv.end(), {"-map", "1:a:0"});
    auto ea = encoderArgs(enc, s.quality);
    argv.insert(argv.end(), ea.begin(), ea.end());
    if ((enc == "libx264" || enc.find("264") != std::string::npos) && (W % 2 || H % 2))
        throw ExportError("H.264 requires even dimensions");
    if (haveAudio) {
        if (ext == "webm") argv.insert(argv.end(), {"-c:a", "libopus", "-b:a", std::to_string(std::min(s.audioBitrateK, 256)) + "k"});
        else if (ext == "mov" && enc == "prores_ks") argv.insert(argv.end(), {"-c:a", "pcm_s16le"});
        else argv.insert(argv.end(), {"-c:a", "aac", "-b:a", std::to_string(s.audioBitrateK) + "k"});
    }
    argv.insert(argv.end(), {"-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709"});
    if (ext == "mp4" || ext == "mov") argv.insert(argv.end(), {"-movflags", "+faststart"});
    if (isSequence) argv.insert(argv.end(), {"-f", "image2"});
    else if (ext == "mp4" || ext == "mov" || ext == "webm" || ext == "mkv") argv.insert(argv.end(), {"-f", ext == "mkv" ? "matroska" : ext});
    argv.push_back(partial);

    auto cleanup = [&] {
        std::error_code ec;
        if (!wavTmp.empty()) fs::remove(wavTmp, ec);
        if (!isSequence) fs::remove(partial, ec);
    };

    render::Renderer renderer(p, frames);
    Progress prog;
    prog.total = f1 - f0;
    try {
        media::Subprocess ff(argv, media::Subprocess::Stdin);
        for (int64_t f = f0; f < f1; ++f) {
            if (cancel && cancel->load()) {
                ff.kill();
                cleanup();
                throw ExportError("export cancelled");
            }
            Image8 img = renderer.renderFrame8(compId, f, rs);
            ff.writeFully(img.px.data(), img.px.size());
            prog.done = f - f0 + 1;
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            prog.framesPerSecond = el > 0 ? prog.done / el : 0;
            prog.etaSeconds = prog.framesPerSecond > 0 ? (prog.total - prog.done) / prog.framesPerSecond : 0;
            if (onProgress) onProgress(prog);
        }
        int rc = ff.wait();
        if (rc != 0) throw ExportError("encoder failed (exit " + std::to_string(rc) + "): " + ff.stderrText());
    } catch (const ExportError&) {
        cleanup();
        throw;
    } catch (const std::exception& e) {
        cleanup();
        throw ExportError(std::string("export failed: ") + e.what());
    }
    if (!isSequence) fs::rename(partial, s.outputPath);
    std::error_code ec;
    if (!wavTmp.empty()) fs::remove(wavTmp, ec);
    for (auto& w : renderer.takeWarnings()) res.warnings.push_back(w);
    res.frames = f1 - f0;
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

} // namespace kinetic::exporter
