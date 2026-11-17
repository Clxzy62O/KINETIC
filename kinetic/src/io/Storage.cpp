#include "io/Storage.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/Platform.h"
#include "io/Serialize.h"

namespace fs = std::filesystem;

namespace kinetic::io {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw IoError("cannot open '" + path + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFileAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp-" + std::to_string(platform::processId());
    try {
        platform::writeDurable(tmp, data);
        platform::atomicReplace(tmp, path);
    } catch (const std::exception& e) {
        throw IoError(e.what());
    }
}

static std::string serializeValid(const Project& p) {
    auto errs = p.validate();
    if (!errs.empty()) {
        std::string msg = "refusing to save an invalid project:";
        for (auto& e : errs) msg += "\n  - " + e;
        throw IoError(msg);
    }
    return toJson(p).dump(2) + "\n";
}

void saveProject(const Project& p, const std::string& path, int keepBackups) {
    std::string data = serializeValid(p);
    std::error_code ec;
    if (keepBackups > 0 && fs::exists(path, ec)) {
        for (int i = keepBackups; i > 1; --i) {
            std::string from = path + ".bak" + std::to_string(i - 1), to = path + ".bak" + std::to_string(i);
            if (fs::exists(from, ec)) fs::rename(from, to, ec);
        }
        fs::copy_file(path, path + ".bak1", fs::copy_options::overwrite_existing, ec);
        if (ec) throw IoError("could not back up previous project: " + ec.message());
    }
    writeFileAtomic(path, data);
    discardAutosave(path);  // the real save supersedes the autosave
}

Project loadProject(const std::string& path, std::vector<std::string>* warnings) {
    Json j = Json::parse(readFile(path));
    Project p = fromJson(j, warnings);
    auto errs = p.validate();
    if (!errs.empty() && warnings)
        for (auto& e : errs) warnings->push_back("validation: " + e);
    // Missing-media detection.
    for (auto& m : p.media) {
        std::error_code ec;
        if (!fs::exists(m.path, ec)) {
            if (warnings) warnings->push_back("missing media: '" + m.name + "' (" + m.path + ")");
        } else if (m.fileSize && static_cast<int64_t>(fs::file_size(m.path, ec)) != m.fileSize) {
            if (warnings) warnings->push_back("media changed on disk since import: '" + m.name + "'");
        }
    }
    return p;
}

std::string autosavePath(const std::string& projectPath) { return projectPath + ".autosave"; }

void writeAutosave(const Project& p, const std::string& projectPath) {
    writeFileAtomic(autosavePath(projectPath), serializeValid(p));
}

void discardAutosave(const std::string& projectPath) {
    std::error_code ec;
    fs::remove(autosavePath(projectPath), ec);
}

RecoveryInfo checkRecovery(const std::string& projectPath) {
    RecoveryInfo r;
    r.path = autosavePath(projectPath);
    std::error_code ec;
    if (!fs::exists(r.path, ec)) { r.reason = "no autosave"; return r; }
    if (fs::exists(projectPath, ec) && fs::last_write_time(r.path, ec) <= fs::last_write_time(projectPath, ec)) {
        r.reason = "autosave is older than the saved project";
        return r;
    }
    try {
        Project p = fromJson(Json::parse(readFile(r.path)));
        auto errs = p.validate();
        if (!errs.empty()) { r.reason = "autosave failed validation: " + errs.front(); return r; }
    } catch (const std::exception& e) {
        r.reason = std::string("autosave is incomplete or corrupt: ") + e.what();
        return r;
    }
    r.available = true;
    r.reason = "valid autosave newer than the project";
    return r;
}

} // namespace kinetic::io
