#pragma once
// Thin OS layer (POSIX + Win32) so the rest of the engine stays portable.
#include <string>

namespace kinetic::platform {

long processId();
// Writes `data` to `path` and flushes it to stable storage (fsync / FlushFileBuffers). Throws on error.
void writeDurable(const std::string& path, const std::string& data);
// Atomically replaces `to` with `from` (rename(2) / MoveFileEx REPLACE_EXISTING|WRITE_THROUGH).
void atomicReplace(const std::string& from, const std::string& to);

} // namespace kinetic::platform
