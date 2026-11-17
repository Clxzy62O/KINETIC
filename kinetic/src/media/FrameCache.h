#pragma once
// Byte-budgeted LRU cache of decoded frames, shared by preview and export.
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>

#include "project/Model.h"
#include "render/Image.h"

namespace kinetic::media {

struct FrameKey {
    Id media = kNoId;
    int width = 0, height = 0;
    int64_t frame = 0;
    bool operator==(const FrameKey& o) const { return media == o.media && width == o.width && height == o.height && frame == o.frame; }
};
struct FrameKeyHash {
    size_t operator()(const FrameKey& k) const {
        uint64_t h = static_cast<uint64_t>(k.media) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<uint64_t>(k.frame) + 0x7F4A7C15ull + (h << 6) + (h >> 2);
        h ^= (static_cast<uint64_t>(k.width) << 32 | static_cast<uint64_t>(k.height)) + (h << 6) + (h >> 2);
        return static_cast<size_t>(h);
    }
};

class FrameCache {
public:
    explicit FrameCache(size_t budgetBytes) : budget_(budgetBytes) {}
    Image8Ptr get(const FrameKey& k);
    void put(const FrameKey& k, Image8Ptr img);
    void invalidateMedia(Id media);  // e.g. after relinking
    void clear();
    void setBudget(size_t bytes);
    size_t bytesUsed() const { std::lock_guard l(m_); return used_; }
    size_t entries() const { std::lock_guard l(m_); return map_.size(); }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }

private:
    void evictLocked();
    using Entry = std::pair<FrameKey, Image8Ptr>;
    mutable std::mutex m_;
    std::list<Entry> lru_;  // front = most recent
    std::unordered_map<FrameKey, std::list<Entry>::iterator, FrameKeyHash> map_;
    size_t budget_, used_ = 0;
    uint64_t hits_ = 0, misses_ = 0;
};

} // namespace kinetic::media
