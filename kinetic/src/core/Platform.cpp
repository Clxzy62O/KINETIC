#include "core/Platform.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kinetic::platform {

#ifdef _WIN32

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

long processId() { return static_cast<long>(GetCurrentProcessId()); }

void writeDurable(const std::string& path, const std::string& data) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create '" + path + "' (error " + std::to_string(GetLastError()) + ")");
    size_t off = 0;
    while (off < data.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - off, 1u << 30)), written = 0;
        if (!WriteFile(h, data.data() + off, chunk, &written, nullptr)) {
            CloseHandle(h);
            DeleteFileW(widen(path).c_str());
            throw std::runtime_error("write failed for '" + path + "'");
        }
        off += written;
    }
    BOOL ok = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(widen(path).c_str()); throw std::runtime_error("flush failed for '" + path + "'"); }
}

void atomicReplace(const std::string& from, const std::string& to) {
    if (!MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD e = GetLastError();
        DeleteFileW(widen(from).c_str());
        throw std::runtime_error("replace of '" + to + "' failed (error " + std::to_string(e) + ")");
    }
}

#else

long processId() { return static_cast<long>(::getpid()); }

void writeDurable(const std::string& path, const std::string& data) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) throw std::runtime_error("cannot create '" + path + "': " + std::strerror(errno));
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            ::close(fd);
            ::unlink(path.c_str());
            throw std::runtime_error("write failed for '" + path + "': " + std::strerror(e));
        }
        off += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        ::unlink(path.c_str());
        throw std::runtime_error("fsync/close failed for '" + path + "'");
    }
}

void atomicReplace(const std::string& from, const std::string& to) {
    if (::rename(from.c_str(), to.c_str()) != 0) {
        int e = errno;
        ::unlink(from.c_str());
        throw std::runtime_error("rename to '" + to + "' failed: " + std::strerror(e));
    }
    std::filesystem::path dir = std::filesystem::path(to).parent_path();
    if (dir.empty()) dir = ".";
    int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }  // persist the directory entry
}

#endif

} // namespace kinetic::platform
