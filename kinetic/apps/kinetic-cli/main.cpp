// kinetic-cli: headless front end to the KINETIC engine.
#include <atomic>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "Demo.h"
#include "audio/BeatDetect.h"
#include "export/Exporter.h"
#include "io/Storage.h"
#include "media/AudioDecode.h"
#include "media/Probe.h"
#include "render/Effects.h"

using namespace kinetic;

static std::atomic<bool> gCancel{false};

static void usage() {
    std::cout <<
R"(KINETIC engine CLI

usage:
  kinetic-cli probe <media>                         Show stream info as JSON
  kinetic-cli beats <audio> [--json out.json]       Tempo, beats, downbeats, onsets, sections, drops
  kinetic-cli effects                               List available effects and parameters
  kinetic-cli validate <project.kinetic>            Load (with migration) and validate a project
  kinetic-cli recover <project.kinetic>             Report whether a valid autosave can be recovered
  kinetic-cli render <project.kinetic> <out> [--quality draft|balanced|high|maximum]
                     [--range A:B] [--width W --height H] [--codec NAME] [--no-audio]
                                                    Export (out: .mp4 .mov .webm .mkv .wav or frame_%05d.png)
  kinetic-cli frame <project.kinetic> <frame> <out.png> [--scale 0.5]
                                                    Render a single frame (preview path)
  kinetic-cli demo <dir> [--seconds 12] [--size 1280x720] [--no-render]
                                                    Generate media + build a full AMV project + render it
)";
}

static std::string opt(int argc, char** argv, const char* name, const std::string& def = "") {
    for (int i = 0; i + 1 < argc; ++i) if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}
static bool flag(int argc, char** argv, const char* name) {
    for (int i = 0; i < argc; ++i) if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

static int cmdProbe(const std::string& path) {
    MediaRef m = media::probe(path);
    Json j = Json::object();
    j.set("name", m.name).set("path", m.path).set("video", m.hasVideo).set("audio", m.hasAudio)
     .set("width", m.width).set("height", m.height).set("fps", m.fps.str()).set("fpsDecimal", m.fps.toDouble())
     .set("duration", m.duration).set("videoCodec", m.videoCodec).set("pixFmt", m.pixFmt).set("colorSpace", m.colorSpace)
     .set("audioCodec", m.audioCodec).set("sampleRate", m.sampleRate).set("channels", m.channels)
     .set("fileSize", m.fileSize).set("still", m.isStill);
    std::cout << j.dump(2) << "\n";
    return 0;
}

static int cmdBeats(const std::string& path, const std::string& jsonOut) {
    auto buf = media::decodeAudio(path, 44100, 2);
    auto a = audio::analyzeBeats(buf.mono(), buf.sampleRate);
    std::cout << "duration   " << buf.duration() << " s\n";
    std::cout << "tempo      " << a.tempo << " BPM (confidence " << a.tempoConfidence << ")\n";
    int strong = 0;
    for (auto& b : a.beats) strong += b.strong;
    std::cout << "beats      " << a.beats.size() << " (" << strong << " strong)\n";
    int kicks = 0, snares = 0, hats = 0;
    for (auto& o : a.onsets) { kicks += o.band == audio::OnsetBand::Kick; snares += o.band == audio::OnsetBand::Snare; hats += o.band == audio::OnsetBand::Hat; }
    std::cout << "onsets     " << a.onsets.size() << " (kick " << kicks << ", snare " << snares << ", hat " << hats << ")\n";
    std::cout << "sections  ";
    for (auto& s : a.sections) std::cout << " [" << s.start << "-" << s.end << "s " << s.label << "]";
    std::cout << "\nevents    ";
    for (auto& e : a.events) std::cout << " " << e.second << "@" << e.first << "s";
    std::cout << "\n";
    if (!jsonOut.empty()) {
        std::ofstream(jsonOut) << a.toJson().dump(2);
        std::cout << "wrote " << jsonOut << "\n";
    }
    return 0;
}

static int cmdEffects() {
    for (auto* d : render::EffectRegistry::instance().list()) {
        std::cout << d->id << "@" << d->version << "  " << d->name << "  [" << d->category << "]\n";
        for (auto& p : d->params)
            std::cout << "    " << p.id << " = " << p.def << "  (" << p.min << ".." << p.max << ")" << (p.quick ? "  quick" : "")
                      << (p.pixels ? "  px" : "") << "\n";
    }
    return 0;
}

static Project load(const std::string& path) {
    std::vector<std::string> w;
    Project p = io::loadProject(path, &w);
    for (auto& s : w) std::cerr << "warning: " << s << "\n";
    return p;
}

static int cmdRender(int argc, char** argv) {
    Project p = load(argv[2]);
    exporter::ExportSettings s;
    s.outputPath = argv[3];
    std::string q = opt(argc, argv, "--quality", "high");
    if (!exporter::parseQuality(q, s.quality)) { std::cerr << "bad --quality\n"; return 2; }
    std::string range = opt(argc, argv, "--range");
    if (!range.empty()) {
        auto c = range.find(':');
        s.startFrame = std::stoll(range.substr(0, c));
        s.endFrame = std::stoll(range.substr(c + 1));
    }
    s.width = std::stoi(opt(argc, argv, "--width", "0"));
    s.height = std::stoi(opt(argc, argv, "--height", "0"));
    s.videoCodec = opt(argc, argv, "--codec", "auto");
    s.includeAudio = !flag(argc, argv, "--no-audio");
    media::FrameProvider fp;
    std::signal(SIGINT, [](int) { gCancel = true; });
    auto res = exporter::exportComposition(p, p.rootCompositionId, s, fp, [](const exporter::Progress& pr) {
        std::cerr << "\r" << pr.done << "/" << pr.total << "  " << static_cast<int>(pr.framesPerSecond * 10) / 10.0 << " fps  ETA "
                  << static_cast<int>(pr.etaSeconds) << "s   " << std::flush;
    }, &gCancel);
    std::cerr << "\n";
    for (auto& w : res.warnings) std::cerr << "warning: " << w << "\n";
    std::cout << "wrote " << res.outputPath << " (" << res.frames << " frames, " << res.encoder << ", " << res.seconds << " s)\n";
    return 0;
}

static int cmdFrame(int argc, char** argv) {
    Project p = load(argv[2]);
    int64_t f = std::stoll(argv[3]);
    exporter::ExportSettings s;
    std::string out = argv[4];
    double scale = std::stod(opt(argc, argv, "--scale", "1"));
    const Composition* c = p.findComposition(p.rootCompositionId);
    s.width = static_cast<int>(std::lround(c->width * scale));
    s.height = static_cast<int>(std::lround(c->height * scale));
    s.startFrame = f;
    s.endFrame = f + 1;
    s.includeAudio = false;
    std::string pattern = out.substr(0, out.size() - 4) + "_%01d.png";
    s.outputPath = pattern;
    media::FrameProvider fp;
    auto res = exporter::exportComposition(p, p.rootCompositionId, s, fp);
    std::filesystem::rename(out.substr(0, out.size() - 4) + "_1.png", out);
    for (auto& w : res.warnings) std::cerr << "warning: " << w << "\n";
    std::cout << "wrote " << out << "\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    std::string cmd = argv[1];
    try {
        if (cmd == "probe" && argc >= 3) return cmdProbe(argv[2]);
        if (cmd == "beats" && argc >= 3) return cmdBeats(argv[2], opt(argc, argv, "--json"));
        if (cmd == "effects") return cmdEffects();
        if (cmd == "validate" && argc >= 3) {
            Project p = load(argv[2]);
            auto errs = p.validate();
            for (auto& e : errs) std::cout << "error: " << e << "\n";
            std::cout << (errs.empty() ? "valid" : "invalid") << " (" << p.compositions.size() << " compositions, " << p.media.size() << " media)\n";
            return errs.empty() ? 0 : 1;
        }
        if (cmd == "recover" && argc >= 3) {
            auto r = io::checkRecovery(argv[2]);
            std::cout << (r.available ? "recoverable: " : "nothing to recover: ") << r.reason << "\n";
            return 0;
        }
        if (cmd == "render" && argc >= 4) return cmdRender(argc, argv);
        if (cmd == "frame" && argc >= 5) return cmdFrame(argc, argv);
        if (cmd == "demo" && argc >= 3) {
            std::string size = opt(argc, argv, "--size", "1280x720");
            int w = std::stoi(size.substr(0, size.find('x'))), h = std::stoi(size.substr(size.find('x') + 1));
            return cli::runDemo(argv[2], std::stod(opt(argc, argv, "--seconds", "12")), w, h, !flag(argc, argv, "--no-render"));
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    usage();
    return 2;
}
