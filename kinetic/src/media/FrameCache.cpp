#include "media/FrameCache.h"

namespace kinetic::media {

Image8Ptr FrameCache::get(const FrameKey& k) {
    std::lock_guard l(m_);
    auto it = map_.find(k);
    if (it == map_.end()) { ++misses_; return nullptr; }
    lru_.splice(lru_.begin(), lru_, it->second);
    ++hits_;
    return it->second->second;
}

void FrameCache::put(const FrameKey& k, Image8Ptr img) {
    if (!img) return;
    std::lock_guard l(m_);
    auto it = map_.find(k);
    if (it != map_.end()) {
        used_ -= it->second->second->bytes();
        lru_.erase(it->second);
        map_.erase(it);
    }
    used_ += img->bytes();
    lru_.emplace_front(k, std::move(img));
    map_[k] = lru_.begin();
    evictLocked();
}

void FrameCache::evictLocked() {
    // Always keep the most recent entry even if it alone exceeds the budget.
    while (used_ > budget_ && lru_.size() > 1) {
        auto& back = lru_.back();
        used_ -= back.second->bytes();
        map_.erase(back.first);
        lru_.pop_back();
    }
}

void FrameCache::invalidateMedia(Id media) {
    std::lock_guard l(m_);
    for (auto it = lru_.begin(); it != lru_.end();) {
        if (it->first.media == media) {
            used_ -= it->second->bytes();
            map_.erase(it->first);
            it = lru_.erase(it);
        } else ++it;
    }
}

void FrameCache::clear() {
    std::lock_guard l(m_);
    lru_.clear();
    map_.clear();
    used_ = 0;
}

void FrameCache::setBudget(size_t bytes) {
    std::lock_guard l(m_);
    budget_ = bytes;
    evictLocked();
}

} // namespace kinetic::media
