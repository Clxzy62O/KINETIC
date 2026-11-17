#pragma once
// Video decoding. IDecoder is the backend seam: milestone 1 ships the FFmpeg *subprocess* backend
// (real decoding through `ffmpeg -f rawvideo` pipes); the libav in-process backend with hardware
// decode implements the same interface later.
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>

#include "media/FrameCache.h"

namespace kinetic::media {

class Subprocess;

struct DecodeError : std::runtime_error { using std::runtime_error::runtime_error; };

class IDecoder {
public:
    virtual ~IDecoder() = default;
    // Returns source frame `index` (0-based at the media's frame rate) scaled to the decoder size.
    // Frames read on the way are pushed to `cache`.
    virtual Image8Ptr frame(int64_t index, FrameCache& cache) = 0;
};

class PipeDecoder : public IDecoder {
public:
    PipeDecoder(const MediaRef& m, int outW, int outH);
    ~PipeDecoder() override;
    Image8Ptr frame(int64_t index, FrameCache& cache) override;
    uint64_t reopenCount() const { return reopens_; }

private:
    void open(int64_t index);
    Image8Ptr readOne();
    MediaRef media_;
    int w_, h_;
    std::unique_ptr<Subprocess> proc_;
    int64_t next_ = -1;   // index of the next frame the pipe will deliver
    Image8Ptr last_;
    int64_t lastIndex_ = -1;
    bool eof_ = false;
    uint64_t reopens_ = 0;
};

// Owns decoders (one sequential reader per media+size) and the shared frame cache.
class FrameProvider {
public:
    explicit FrameProvider(size_t cacheBytes = size_t(1) << 30) : cache_(cacheBytes) {}
    Image8Ptr get(const MediaRef& m, int64_t frameIndex, int w, int h);
    FrameCache& cache() { return cache_; }
    uint64_t decoderOpens() const { return opens_; }

private:
    std::mutex m_;
    FrameCache cache_;
    struct DecKey {
        Id media; int w, h;
        bool operator<(const DecKey& o) const { return std::tie(media, w, h) < std::tie(o.media, o.w, o.h); }
    };
    std::map<DecKey, std::unique_ptr<PipeDecoder>> decoders_;
    std::map<DecKey, uint64_t> lastUse_;
    uint64_t tick_ = 0, opens_ = 0;
    static constexpr size_t kMaxOpenDecoders = 8;
};

} // namespace kinetic::media
