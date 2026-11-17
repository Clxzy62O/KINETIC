#pragma once
// Crash-safe project persistence: atomic writes, rotating backups, autosave sidecar, recovery.
#include <string>
#include <vector>

#include "project/Model.h"

namespace kinetic::io {

struct IoError : std::runtime_error { using std::runtime_error::runtime_error; };

std::string readFile(const std::string& path);
// Writes to "<path>.tmp-<pid>", fsyncs, then renames over `path`. The old file is never truncated.
void writeFileAtomic(const std::string& path, const std::string& data);

// Validates, then atomically saves. Before replacing an existing project, rotates up to `keepBackups`
// copies (<path>.bak1 is the most recent previous save). Throws IoError and leaves disk untouched
// if the project fails validation.
void saveProject(const Project& p, const std::string& path, int keepBackups = 3);
Project loadProject(const std::string& path, std::vector<std::string>* warnings = nullptr);

std::string autosavePath(const std::string& projectPath);
void writeAutosave(const Project& p, const std::string& projectPath);
void discardAutosave(const std::string& projectPath);

struct RecoveryInfo {
    bool available = false;      // a valid autosave newer than the project exists
    std::string path;
    std::string reason;          // why recovery is / isn't offered
};
// Never offers an autosave that fails to parse or validate; never overwrites the project itself.
RecoveryInfo checkRecovery(const std::string& projectPath);

} // namespace kinetic::io
