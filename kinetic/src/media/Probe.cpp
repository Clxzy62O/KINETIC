#include "media/Probe.h"

#include <algorithm>
#include <filesystem>

#include "core/Json.h"
#include "media/Process.h"

namespace fs = std::filesystem;

namespace kinetic::media {

bool isStillPath(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".webp" || ext == ".tif" || ext == ".tiff";
}

MediaRef probe(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) throw ProcessError("media not found: " + path);
    std::string out = runCapture({ffprobePath(), "-v", "error", "-print_format", "json", "-show_streams", "-show_format", path});
    Json j = Json::parse(out);
    MediaRef m;
    m.path = fs::absolute(path).string();
    m.name = fs::path(path).filename().string();
    m.fileSize = static_cast<int64_t>(fs::file_size(path, ec));
    m.mtime = static_cast<int64_t>(fs::last_write_time(path, ec).time_since_epoch().count());
    m.isStill = isStillPath(path);
    if (auto* streams = j.find("streams"); streams && streams->isArray()) {
        for (auto& s : streams->asArray()) {
            std::string type = s.str("codec_type", "");
            if (type == "video" && !m.hasVideo) {
                // Skip attached pictures (cover art in audio files).
                if (auto* disp = s.find("disposition"); disp && disp->integer("attached_pic", 0) == 1) continue;
                m.hasVideo = true;
                m.width = static_cast<int>(s.integer("width", 0));
                m.height = static_cast<int>(s.integer("height", 0));
                m.videoCodec = s.str("codec_name", "");
                m.pixFmt = s.str("pix_fmt", "");
                m.colorSpace = s.str("color_space", "unknown");
                std::string rate = s.str("r_frame_rate", "");
                std::string avg = s.str("avg_frame_rate", "");
                std::string use = !avg.empty() && avg != "0/0" ? avg : rate;
                if (!use.empty() && use != "0/0") {
                    Rational r = Rational::parse(use);
                    // Snap near-NTSC rationals (e.g. 2997/125) to the exact form.
                    m.fps = Rational::parse(std::to_string(r.toDouble()));
                    if (std::fabs(m.fps.toDouble() - r.toDouble()) > 0.01) m.fps = r;
                }
                if (s.has("duration")) m.duration = std::stod(s.str("duration", "0"));
            } else if (type == "audio" && !m.hasAudio) {
                m.hasAudio = true;
                m.audioCodec = s.str("codec_name", "");
                m.sampleRate = std::stoi(s.str("sample_rate", "0"));
                m.channels = static_cast<int>(s.integer("channels", 0));
            }
        }
    }
    if (auto* fmt = j.find("format")) {
        double d = std::stod(fmt->str("duration", "0"));
        if (m.duration <= 0 || (!m.hasVideo && d > 0)) m.duration = d;
    }
    if (m.isStill) m.duration = 0;
    if (!m.hasVideo && !m.hasAudio) throw ProcessError("no audio or video streams in " + path);
    return m;
}

} // namespace kinetic::media
