#pragma once
// Subprocess with pipes, spawned without a shell (no quoting/injection issues with file paths).
// Used by the FFmpeg subprocess media backend.
#include <stdexcept>
#include <string>
#include <vector>

namespace kinetic::media {

struct ProcessError : std::runtime_error { using std::runtime_error::runtime_error; };

class Subprocess {
public:
    enum Pipes { None = 0, Stdin = 1, Stdout = 2 };
    Subprocess(const std::vector<std::string>& argv, int pipes);
    ~Subprocess();
    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;

    // Reads exactly n bytes unless EOF; returns bytes read.
    size_t readFully(void* buf, size_t n);
    std::string readAll();
    void writeFully(const void* buf, size_t n);  // throws ProcessError if the child died
    void closeStdin();
    int wait();          // returns exit code (or 128+signal)
    void kill();
    std::string stderrText() const;  // captured diagnostics
    bool running() const { return pid_ > 0; }

private:
    int pid_ = -1;            // POSIX child pid / >0 while a Win32 child is running
    int inFd_ = -1, outFd_ = -1;
    void* hProc_ = nullptr;   // Win32 handles (unused on POSIX)
    void* hIn_ = nullptr;
    void* hOut_ = nullptr;
    std::string errPath_;
    int status_ = -1;
};

// Runs to completion and returns stdout; throws ProcessError (with stderr) on non-zero exit.
std::string runCapture(const std::vector<std::string>& argv);

// Locations of the FFmpeg tools (override with KINETIC_FFMPEG / KINETIC_FFPROBE).
std::string ffmpegPath();
std::string ffprobePath();

} // namespace kinetic::media
