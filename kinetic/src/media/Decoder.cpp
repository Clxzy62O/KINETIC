#include "media/Decoder.h"

#include <algorithm>
#include <cstdio>
#include <tuple>

#include "media/Process.h"

namespace kinetic::media {

static constexpr int64_t kMaxForwardRead = 90;  // frames; beyond this a re-seek is cheaper

PipeDecoder::PipeDecoder(const MediaRef& m, int outW, int outH) : media_(m), w_(outW), h_(outH) {
    if (!m.hasVideo) throw DecodeError("media '" + m.name + "' has no video stream");
    if (w_ <= 0 || h_ <= 0) throw DecodeError("invalid decode size");
}

PipeDecoder::~PipeDecoder() = default;

void PipeDecoder::open(int64_t index) {
    proc_.reset();
    eof_ = false;
    std::vector<std::string> argv{ffmpegPath(), "-hide_banner", "-loglevel", "error", "-nostdin"};
    if (!media_.isStill && index > 0) {
        // Seek a quarter frame before the target so the target is the first frame with pts >= ss.
        double ss = (static_cast<double>(index) - 0.25) / media_.fps.toDouble();
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.6f", ss);
        argv.insert(argv.end(), {"-ss", buf});
    }
    argv.insert(argv.end(), {"-i", media_.path, "-map", "0:v:0", "-an", "-sn", "-fps_mode", "passthrough"});
    std::string vf;
    if (w_ != media_.width || h_ != media_.height)
        vf = "scale=" + std::to_string(w_) + ":" + std::to_string(h_) + ":flags=bicubic";
    if (!vf.empty()) argv.insert(argv.end(), {"-vf", vf});
    if (media_.isStill) argv.insert(argv.end(), {"-frames:v", "1"});
    argv.insert(argv.end(), {"-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1"});
    proc_ = std::make_unique<Subprocess>(argv, Subprocess::Stdout);
    next_ = media_.isStill ? 0 : index;
    ++reopens_;
}

Image8Ptr PipeDecoder::readOne() {
    auto img = std::make_shared<Image8>(w_, h_);
    size_t got = proc_->readFully(img->px.data(), img->px.size());
    if (got != img->px.size()) {
        eof_ = true;
        int rc = proc_->wait();
        if (rc != 0 && got == 0 && lastIndex_ < 0)
            throw DecodeError("decoding '" + media_.name + "' failed: " + proc_->stderrText());
        return nullptr;
    }
    return img;
}

Image8Ptr PipeDecoder::frame(int64_t index, FrameCache& cache) {
    if (index < 0) index = 0;
    if (media_.isStill) {
        if (last_) return last_;
        open(0);
        last_ = readOne();
        if (!last_) throw DecodeError("could not decode still image '" + media_.name + "'");
        cache.put({media_.id, w_, h_, 0}, last_);
        return last_;
    }
    if (!proc_ || eof_ || index < next_ || index - next_ > kMaxForwardRead) {
        if (eof_ && index >= next_ && last_) return last_;  // past the end: last real frame
        open(index);
    }
    while (true) {
        Image8Ptr img = readOne();
        if (!img) {
            if (last_) return last_;
            throw DecodeError("frame " + std::to_string(index) + " is beyond the end of '" + media_.name + "'");
        }
        int64_t i = next_++;
        cache.put({media_.id, w_, h_, i}, img);
        last_ = img;
        lastIndex_ = i;
        if (i == index) return img;
    }
}

Image8Ptr FrameProvider::get(const MediaRef& m, int64_t frameIndex, int w, int h) {
    if (m.isStill) frameIndex = 0;
    FrameKey key{m.id, w, h, frameIndex};
    if (auto hit = cache_.get(key)) return hit;
    std::lock_guard l(m_);
    if (auto hit = cache_.get(key)) return hit;
    DecKey dk{m.id, w, h};
    auto it = decoders_.find(dk);
    if (it == decoders_.end()) {
        if (decoders_.size() >= kMaxOpenDecoders) {
            auto oldest = std::min_element(lastUse_.begin(), lastUse_.end(), [](auto& a, auto& b) { return a.second < b.second; });
            decoders_.erase(oldest->first);
            lastUse_.erase(oldest);
        }
        it = decoders_.emplace(dk, std::make_unique<PipeDecoder>(m, w, h)).first;
        ++opens_;
    }
    lastUse_[dk] = ++tick_;
    return it->second->frame(frameIndex, cache_);
}

} // namespace kinetic::media
