#pragma once

#include "base/Logging.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 线程安全LRU(最近最少使用)
class PrefetchCache {
  public:
    // 默认最大缓存100MB
    explicit PrefetchCache(size_t maxBytes = 100 * 1024 * 1024)
        : maxBytes_(maxBytes) {}

    // 尝试从缓存中获取数据
    bool get(const std::string &key, std::vector<char> &out) {
        std::lock_guard<std::mutex> lock(mutex_);
        pruneExpiredLocked();
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            return false; // 未命中
        }

        touchLocked(it); // 命中，把数据块移到LRU链表最前面
        out = it->second.data;
        return true;
    }

    // 防止“缓存击穿”
    // 如果10个用户同时请求同一个还没缓存的视频块，防止10个人同时读磁盘
    bool tryMarkLoading(const std::string &key) {
        std::lock_guard<std::mutex> lock(mutex_);
        pruneExpiredLocked();
        // 已经在缓存里，或别人正在读
        if (entries_.count(key) > 0 || loadingKeys_.count(key) > 0) {
            return false;
        }
        loadingKeys_.insert(key); // 标记为正在读磁盘
        return true;
    }

    size_t currentBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        pruneExpiredLocked();
        return currentBytes_;
    }

    size_t maxBytes() const { return maxBytes_; }

    bool shouldAdmitPrefetch(size_t incomingBytes, bool isVip) const {
        std::lock_guard<std::mutex> lock(mutex_);
        pruneExpiredLocked();
        if (incomingBytes == 0 || incomingBytes > maxBytes_) {
            return false;
        }

        if (isVip) { // VIP永远放行
            return true;
        }

        const size_t normalSoftLimit =
            maxBytes_ > kVipReservedBytes ? (maxBytes_ - kVipReservedBytes) : 0;

        // 总容量超出多少
        const size_t requiredForTotal =
            currentBytes_ + incomingBytes > maxBytes_
                ? (currentBytes_ + incomingBytes - maxBytes_)
                : 0;

        // 普通用户超出多少
        const size_t requiredForNormal =
            normalBytes_ + incomingBytes > normalSoftLimit
                ? (normalBytes_ + incomingBytes - normalSoftLimit)
                : 0;

        // 当前缓存里，能被淘汰的普通用户数据，够不够腾出空间
        return evictableNormalBytesLocked() >=
               std::max(requiredForTotal, requiredForNormal);
    }

    // 读取失败时取消标记
    void cancelLoading(const std::string &key) {
        std::lock_guard<std::mutex> lock(mutex_);
        loadingKeys_.erase(key);
    }

    // 把从磁盘读到的数据放进缓存
    void put(const std::string &key, std::vector<char> data, bool isVip) {
        std::lock_guard<std::mutex> lock(mutex_);
        pruneExpiredLocked();
        loadingKeys_.erase(key);

        if (data.empty() || data.size() > maxBytes_) {
            return;
        }

        auto existing = entries_.find(key);
        if (existing != entries_.end()) {
            removeEntryLocked(existing);
        }

        if (isVip) {
            evictIfNeededLocked(data.size());
            if (currentBytes_ + data.size() > maxBytes_) {
                return;
            }
        } else {
            const size_t normalSoftLimit = maxBytes_ > kVipReservedBytes
                                               ? (maxBytes_ - kVipReservedBytes)
                                               : 0;
            evictNormalIfNeededLocked(data.size(), normalSoftLimit);
            if (currentBytes_ + data.size() > maxBytes_ ||
                normalBytes_ + data.size() > normalSoftLimit) {
                return;
            }
        }

        lruKeys_.push_front(key);

        CacheEntry entry;
        entry.data = std::move(data);
        entry.lruIt = lruKeys_.begin();
        entry.lastAccess = std::chrono::steady_clock::now();
        entry.isVip = isVip;

        currentBytes_ += entry.data.size();
        if (!entry.isVip) {
            normalBytes_ += entry.data.size();
        }
        entries_.emplace(key, std::move(entry));
    }

    // 检查从offset
    // 开始，连续length字节的数据，是否已经足够安全(在缓存中或正在加载)
    // 返回实际连续存在的字节数
    uintmax_t getContinuousCachedBytes(const std::string &filepath,
                                       uintmax_t startOffset, uintmax_t length,
                                       uintmax_t chunkSize) {
        std::lock_guard<std::mutex> lock(mutex_);
        pruneExpiredLocked();

        uintmax_t continuousBytes = 0;
        uintmax_t currentOffset = startOffset;

        while (continuousBytes < length) {
            std::string key = filepath + "_" + std::to_string(currentOffset);
            // 如果在缓存里，或者有别的线程正在读它，算作“安全”
            if (entries_.count(key) > 0 || loadingKeys_.count(key) > 0) {
                continuousBytes += chunkSize;
                currentOffset += chunkSize;
            } else {
                break; // 遇到断层，停止检查
            }
        }
        return continuousBytes;
    }

  private:
    struct CacheEntry {
        std::vector<char> data;
        std::list<std::string>::iterator lruIt;
        std::chrono::steady_clock::time_point lastAccess;
        bool isVip = false;
    };

    using EntryMap = std::unordered_map<std::string, CacheEntry>;

    void removeEntryLocked(EntryMap::iterator it) {
        currentBytes_ -= it->second.data.size();
        if (!it->second.isVip) {
            normalBytes_ -= it->second.data.size();
        }
        lruKeys_.erase(it->second.lruIt);
        entries_.erase(it);
        LOG_INFO << "Removed entry from cache, size: " << it->second.data.size()
                 << ", isVip: " << it->second.isVip
                 << ", currentBytes: " << currentBytes_
                 << ", normalBytes: " << normalBytes_;
    }

    // 计算当前缓存里，所有普通用户数据的粽子结束
    // 用于判断在缓存满载时，是否有足够的冷数据可以被驱逐
    // 调用前必须持有锁
    size_t evictableNormalBytesLocked() const {
        size_t bytes = 0;
        for (const auto &key : lruKeys_) {
            auto it = entries_.find(key);
            if (it != entries_.end() && !it->second.isVip) {
                bytes += it->second.data.size();
            }
        }
        LOG_DEBUG << "Evictable normal bytes: " << bytes;
        return bytes;
    }

    // 惰性清理过期数据
    // 遍历LRU链表尾部，如果其最后访问时间超过了TTL，则销毁
    void pruneExpiredLocked() const {
        auto *self = const_cast<PrefetchCache *>(this);
        const auto now = std::chrono::steady_clock::now();
        while (!self->lruKeys_.empty()) {
            auto tailIt = std::prev(self->lruKeys_.end());
            auto entryIt = self->entries_.find(*tailIt);
            if (entryIt == self->entries_.end()) {
                self->lruKeys_.erase(tailIt);
                continue;
            }

            if (now - entryIt->second.lastAccess < kEntryTtl) {
                break;
            }
            self->removeEntryLocked(entryIt);
        }
    }

    // 带优先级隔离的 LRU 驱逐（仅针对普通用户）
    // 当总容量触及硬上限，或普通用户容量触及软上限时触发
    // 强制从LRU尾部扫描，仅驱逐普通用户数据
    void evictNormalIfNeededLocked(size_t incomingBytes,
                                   size_t normalSoftLimit) {
        while (!lruKeys_.empty() &&
               (currentBytes_ + incomingBytes > maxBytes_ ||
                normalBytes_ + incomingBytes > normalSoftLimit)) {
            bool removed = false;
            for (auto it = lruKeys_.rbegin(); it != lruKeys_.rend(); ++it) {
                auto entryIt = entries_.find(*it);
                if (entryIt == entries_.end() || entryIt->second.isVip) {
                    continue;
                }
                removeEntryLocked(entryIt);
                removed = true;
                break;
            }
            if (!removed) {
                break;
            }
        }
    }

    // 全局无差别 LRU 驱逐（针对 VIP 用户）
    // 当总容量触及硬上限时触发，直接从 LRU 尾部驱逐最老的数据
    void evictIfNeededLocked(size_t incomingBytes) {
        while (!lruKeys_.empty() && currentBytes_ + incomingBytes > maxBytes_) {
            auto tailIt = std::prev(lruKeys_.end());
            auto entryIt = entries_.find(*tailIt);
            if (entryIt == entries_.end()) {
                lruKeys_.erase(tailIt);
                continue;
            }
            removeEntryLocked(entryIt);
        }
    }

    // 把数据块移到LRU链表最前面
    void touchLocked(EntryMap::iterator it) {
        lruKeys_.erase(it->second.lruIt);
        lruKeys_.push_front(it->first);
        it->second.lruIt = lruKeys_.begin();
        it->second.lastAccess = std::chrono::steady_clock::now();
    }

    static constexpr size_t kVipReservedBytes = 20 * 1024 * 1024;
    static constexpr auto kEntryTtl = std::chrono::minutes(2);

    mutable std::mutex mutex_; // 全局互斥锁
    size_t maxBytes_;
    size_t currentBytes_ = 0;
    size_t normalBytes_ = 0;
    std::list<std::string> lruKeys_;              // LRU链表
    EntryMap entries_;                            // 哈希表，存储实际缓存数据
    std::unordered_set<std::string> loadingKeys_; // 正在加载的key
};
