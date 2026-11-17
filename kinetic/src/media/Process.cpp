#include "media/Process.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace kinetic::media {

static std::atomic<int> gSeq{0};

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Quote one argument per the MSVC CommandLineToArgvW rules.
static std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { ++i; ++bs; }
        if (i == a.size()) { out.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { out.append(bs * 2 + 1, L'\\'); out += L'"'; }
        else { out.append(bs, L'\\'); out += a[i]; }
    }
    return out + L"\"";
}

Subprocess::Subprocess(const std::vector<std::string>& argv, int pipes) {
    if (argv.empty()) throw ProcessError("empty argv");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if ((pipes & Stdin) && !CreatePipe(&inR, &inW, &sa, 1 << 20)) throw ProcessError("CreatePipe failed");
    if ((pipes & Stdout) && !CreatePipe(&outR, &outW, &sa, 1 << 20)) throw ProcessError("CreatePipe failed");
    if (inW) SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    if (outR) SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    char tmpDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpDir);
    errPath_ = std::string(tmpDir) + "kinetic-proc-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(gSeq++) + ".log";
    HANDLE err = CreateFileW(widen(errPath_).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    HANDLE nulIn = (pipes & Stdin) ? nullptr : CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE nulOut = (pipes & Stdout) ? nullptr : CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (pipes & Stdin) ? inR : nulIn;
    si.hStdOutput = (pipes & Stdout) ? outW : nulOut;
    si.hStdError = err;
    std::wstring cmd;
    for (auto& a : argv) { if (!cmd.empty()) cmd += L' '; cmd += quoteArg(widen(a)); }
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    DWORD lastErr = GetLastError();
    for (HANDLE h : {inR, outW, err, nulIn, nulOut}) if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) {
        if (inW) CloseHandle(inW);
        if (outR) CloseHandle(outR);
        throw ProcessError("could not start '" + argv[0] + "' (error " + std::to_string(lastErr) + "); is it on PATH?");
    }
    CloseHandle(pi.hThread);
    hProc_ = pi.hProcess;
    hIn_ = inW;
    hOut_ = outR;
    pid_ = static_cast<int>(pi.dwProcessId);
}

Subprocess::~Subprocess() {
    if (pid_ > 0) kill();
    if (hIn_) CloseHandle(static_cast<HANDLE>(hIn_));
    if (hOut_) CloseHandle(static_cast<HANDLE>(hOut_));
    if (hProc_) CloseHandle(static_cast<HANDLE>(hProc_));
    if (!errPath_.empty()) DeleteFileW(widen(errPath_).c_str());
}

size_t Subprocess::readFully(void* buf, size_t n) {
    if (!hOut_) return 0;
    size_t got = 0;
    auto* p = static_cast<char*>(buf);
    while (got < n) {
        DWORD r = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(n - got, 1u << 30));
        if (!ReadFile(static_cast<HANDLE>(hOut_), p + got, want, &r, nullptr) || r == 0) break;  // broken pipe = EOF
        got += r;
    }
    return got;
}

std::string Subprocess::readAll() {
    std::string out;
    char buf[65536];
    while (true) {
        size_t n = readFully(buf, sizeof buf);
        out.append(buf, n);
        if (n < sizeof buf) break;
    }
    return out;
}

void Subprocess::writeFully(const void* buf, size_t n) {
    if (!hIn_) throw ProcessError("stdin not piped");
    auto* p = static_cast<const char*>(buf);
    size_t off = 0;
    while (off < n) {
        DWORD w = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(n - off, 1u << 30));
        if (!WriteFile(static_cast<HANDLE>(hIn_), p + off, want, &w, nullptr))
            throw ProcessError("write to subprocess failed (error " + std::to_string(GetLastError()) + ")\n" + stderrText());
        off += w;
    }
}

void Subprocess::closeStdin() {
    if (hIn_) { CloseHandle(static_cast<HANDLE>(hIn_)); hIn_ = nullptr; }
}

int Subprocess::wait() {
    closeStdin();
    if (pid_ <= 0) return status_;
    WaitForSingleObject(static_cast<HANDLE>(hProc_), INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(static_cast<HANDLE>(hProc_), &code);
    pid_ = -1;
    status_ = static_cast<int>(code);
    return status_;
}

void Subprocess::kill() {
    if (pid_ > 0) {
        TerminateProcess(static_cast<HANDLE>(hProc_), 137);
        WaitForSingleObject(static_cast<HANDLE>(hProc_), 5000);
        pid_ = -1;
        status_ = 137;
    }
}

std::string Subprocess::stderrText() const {
    std::ifstream in(errPath_);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.size() > 4000) s = s.substr(s.size() - 4000);
    return s;
}

#else

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

extern char** environ;

namespace kinetic::media {

static std::atomic<int> gSeq{0};

static void ignoreSigpipeOnce() {
    static bool done = [] { ::signal(SIGPIPE, SIG_IGN); return true; }();
    (void)done;
}

Subprocess::Subprocess(const std::vector<std::string>& argv, int pipes) {
    ignoreSigpipeOnce();
    if (argv.empty()) throw ProcessError("empty argv");
    int inPipe[2] = {-1, -1}, outPipe[2] = {-1, -1};
    if ((pipes & Stdin) && ::pipe2(inPipe, O_CLOEXEC) != 0) throw ProcessError("pipe failed");
    if ((pipes & Stdout) && ::pipe2(outPipe, O_CLOEXEC) != 0) throw ProcessError("pipe failed");

    errPath_ = "/tmp/kinetic-proc-" + std::to_string(::getpid()) + "-" + std::to_string(gSeq++) + ".log";

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (pipes & Stdin) posix_spawn_file_actions_adddup2(&fa, inPipe[0], 0);
    else posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (pipes & Stdout) posix_spawn_file_actions_adddup2(&fa, outPipe[1], 1);
    else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, errPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);

    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    int rc = ::posix_spawnp(&pid_, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (pipes & Stdin) ::close(inPipe[0]);
    if (pipes & Stdout) ::close(outPipe[1]);
    if (rc != 0) {
        if (pipes & Stdin) ::close(inPipe[1]);
        if (pipes & Stdout) ::close(outPipe[0]);
        pid_ = -1;
        throw ProcessError("could not start '" + argv[0] + "': " + std::strerror(rc));
    }
    inFd_ = (pipes & Stdin) ? inPipe[1] : -1;
    outFd_ = (pipes & Stdout) ? outPipe[0] : -1;
}

Subprocess::~Subprocess() {
    if (pid_ > 0) { kill(); }
    if (inFd_ >= 0) ::close(inFd_);
    if (outFd_ >= 0) ::close(outFd_);
    if (!errPath_.empty()) ::unlink(errPath_.c_str());
}

size_t Subprocess::readFully(void* buf, size_t n) {
    if (outFd_ < 0) return 0;
    size_t got = 0;
    auto* p = static_cast<char*>(buf);
    while (got < n) {
        ssize_t r = ::read(outFd_, p + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        got += static_cast<size_t>(r);
    }
    return got;
}

std::string Subprocess::readAll() {
    std::string out;
    char buf[65536];
    while (true) {
        size_t n = readFully(buf, sizeof buf);
        out.append(buf, n);
        if (n < sizeof buf) break;
    }
    return out;
}

void Subprocess::writeFully(const void* buf, size_t n) {
    if (inFd_ < 0) throw ProcessError("stdin not piped");
    auto* p = static_cast<const char*>(buf);
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(inFd_, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            throw ProcessError(std::string("write to subprocess failed: ") + std::strerror(errno) + "\n" + stderrText());
        }
        off += static_cast<size_t>(w);
    }
}

void Subprocess::closeStdin() {
    if (inFd_ >= 0) { ::close(inFd_); inFd_ = -1; }
}

int Subprocess::wait() {
    closeStdin();
    if (pid_ <= 0) return status_;
    int st = 0;
    while (::waitpid(pid_, &st, 0) < 0 && errno == EINTR) {}
    pid_ = -1;
    status_ = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return status_;
}

void Subprocess::kill() {
    if (pid_ > 0) {
        ::kill(pid_, SIGKILL);
        int st;
        while (::waitpid(pid_, &st, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
        status_ = 137;
    }
}

std::string Subprocess::stderrText() const {
    std::ifstream in(errPath_);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.size() > 4000) s = s.substr(s.size() - 4000);
    return s;
}

#endif

std::string runCapture(const std::vector<std::string>& argv) {
    Subprocess p(argv, Subprocess::Stdout);
    std::string out = p.readAll();
    int rc = p.wait();
    if (rc != 0) throw ProcessError("'" + argv[0] + "' failed (exit " + std::to_string(rc) + "): " + p.stderrText());
    return out;
}

// Directory of the running executable (bundled FFmpeg lives next to kinetic.exe).
static std::string exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::filesystem::path p(std::wstring(buf, n));
#else
    std::error_code ec;
    std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
#endif
    return p.parent_path().string();
}

static std::string findTool(const char* envVar, const char* name) {
    const char* e = std::getenv(envVar);
    if (e && *e) return e;
#ifdef _WIN32
    std::string file = std::string(name) + ".exe";
#else
    std::string file = name;
#endif
    std::string dir = exeDir();
    if (!dir.empty()) {
        for (auto cand : {std::filesystem::path(dir) / file, std::filesystem::path(dir) / "ffmpeg" / file}) {
            std::error_code ec;
            if (std::filesystem::exists(cand, ec)) return cand.string();
        }
    }
    return name;  // fall back to PATH
}

std::string ffmpegPath() { return findTool("KINETIC_FFMPEG", "ffmpeg"); }
std::string ffprobePath() { return findTool("KINETIC_FFPROBE", "ffprobe"); }

} // namespace kinetic::media
