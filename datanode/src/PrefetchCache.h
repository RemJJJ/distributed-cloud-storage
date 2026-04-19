#pragma once

#include "base/Logging.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 读多写少场景下的分片缓存：
// 1. 读路径只拿分片 shared_lock，不再被全局大锁串行化
// 2. 缓存命中返回 shared_ptr，避免额外复制一份 vector
// 3. 淘汰和过期清理由写路径偶发执行，避免每次 get 都做维护
class PrefetchCache {
  public:
    using Buffer = std::vector<char>;
    using BufferPtr = std::shared_ptr<const Buffer>;

    explicit PrefetchCache(size_t maxBytes = 100 * 1024 * 1024)
        : maxBytes_(maxBytes) {}

    bool get(const std::string &key, BufferPtr &out) const {
        auto &shard = shardFor(key);
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        auto it = shard.entries.find(key);
        if (it == shard.entries.end()) {
            return false;
        }

        it->second->lastTouchNs.store(nowNs(), std::memory_order_relaxed);
        out = it->second->data;
        return static_cast<bool>(out);
    }

    bool tryMarkLoading(const std::string &key) {
        auto &shard = shardFor(key);
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        if (shard.entries.find(key) != shard.entries.end() ||
            shard.loadingKeys.find(key) != shard.loadingKeys.end()) {
            return false;
        }
        shard.loadingKeys.insert(key);
        return true;
    }

    size_t currentBytes() const {
        return currentBytes_.load(std::memory_order_relaxed);
    }

    size_t maxBytes() const { return maxBytes_; }

    bool shouldAdmitPrefetch(size_t incomingBytes, bool isVip) const {
        if (incomingBytes == 0 || incomingBytes > maxBytes_) {
            return false;
        }

        const size_t current = currentBytes_.load(std::memory_order_relaxed);
        if (isVip) {
            return true;
        }

        const size_t normalSoftLimit =
            maxBytes_ > kVipReservedBytes ? (maxBytes_ - kVipReservedBytes) : 0;
        const size_t normalBytes =
            normalBytes_.load(std::memory_order_relaxed);

        // 普通用户优先使用普通区，若普通区已有内容则允许触发替换。
        if (current + incomingBytes <= maxBytes_ &&
            normalBytes + incomingBytes <= normalSoftLimit) {
            return true;
        }
        return normalBytes > 0;
    }

    void cancelLoading(const std::string &key) {
        auto &shard = shardFor(key);
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        shard.loadingKeys.erase(key);
    }

    void put(const std::string &key, Buffer data, bool isVip) {
        if (data.empty() || data.size() > maxBytes_) {
            cancelLoading(key);
            return;
        }

        maybePruneExpired();

        auto entry = std::make_shared<CacheEntry>();
        entry->data = std::make_shared<Buffer>(std::move(data));
        entry->size = entry->data->size();
        entry->isVip = isVip;
        entry->lastTouchNs.store(nowNs(), std::memory_order_relaxed);

        auto &shard = shardFor(key);
        size_t replacedBytes = 0;
        bool replacedNormal = false;
        {
            std::unique_lock<std::shared_mutex> lock(shard.mutex);
            shard.loadingKeys.erase(key);
            auto existing = shard.entries.find(key);
            if (existing != shard.entries.end()) {
                replacedBytes = existing->second->size;
                replacedNormal = !existing->second->isVip;
                shard.entries.erase(existing);
            }
        }

        if (replacedBytes > 0) {
            currentBytes_.fetch_sub(replacedBytes, std::memory_order_relaxed);
            if (replacedNormal) {
                normalBytes_.fetch_sub(replacedBytes, std::memory_order_relaxed);
            }
        }

        if (!makeSpaceFor(entry->size, isVip)) {
            return;
        }

        const size_t insertedBytes = entry->size;
        {
            std::unique_lock<std::shared_mutex> lock(shard.mutex);
            auto existing = shard.entries.find(key);
            if (existing != shard.entries.end()) {
                const size_t existingBytes = existing->second->size;
                const bool existingNormal = !existing->second->isVip;
                shard.entries.erase(existing);
                currentBytes_.fetch_sub(existingBytes, std::memory_order_relaxed);
                if (existingNormal) {
                    normalBytes_.fetch_sub(existingBytes,
                                           std::memory_order_relaxed);
                }
            }
            shard.entries[key] = std::move(entry);
        }

        currentBytes_.fetch_add(insertedBytes, std::memory_order_relaxed);
        if (!isVip) {
            normalBytes_.fetch_add(insertedBytes, std::memory_order_relaxed);
        }
    }

    uintmax_t getContinuousCachedBytes(const std::string &filepath,
                                       uintmax_t startOffset, uintmax_t length,
                                       uintmax_t chunkSize) const {
        uintmax_t continuousBytes = 0;
        uintmax_t currentOffset = startOffset;

        while (continuousBytes < length) {
            const std::string key =
                filepath + "_" + std::to_string(currentOffset);
            if (containsOrLoading(key)) {
                continuousBytes += chunkSize;
                currentOffset += chunkSize;
            } else {
                break;
            }
        }
        return continuousBytes;
    }

  private:
    struct CacheEntry {
        BufferPtr data;
        size_t size = 0;
        bool isVip = false;
        std::atomic<uint64_t> lastTouchNs{0};
    };

    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<std::string, std::shared_ptr<CacheEntry>> entries;
        std::unordered_set<std::string> loadingKeys;
    };

    static constexpr size_t kShardCount = 32;
    static constexpr size_t kVipReservedBytes = 100 * 1024 * 1024;
    static constexpr auto kEntryTtl = std::chrono::minutes(10);
    static constexpr auto kMaintenanceInterval = std::chrono::seconds(5);

    static uint64_t nowNs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    Shard &shardFor(const std::string &key) {
        return shards_[std::hash<std::string>{}(key) % kShardCount];
    }

    const Shard &shardFor(const std::string &key) const {
        return shards_[std::hash<std::string>{}(key) % kShardCount];
    }

    bool containsOrLoading(const std::string &key) const {
        const auto &shard = shardFor(key);
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        return shard.entries.find(key) != shard.entries.end() ||
               shard.loadingKeys.find(key) != shard.loadingKeys.end();
    }

    bool makeSpaceFor(size_t incomingBytes, bool isVip) {
        std::lock_guard<std::mutex> guard(evictionMutex_);
        while (true) {
            const size_t current =
                currentBytes_.load(std::memory_order_relaxed);
            const size_t normal =
                normalBytes_.load(std::memory_order_relaxed);
            const size_t normalSoftLimit = maxBytes_ > kVipReservedBytes
                                               ? (maxBytes_ - kVipReservedBytes)
                                               : 0;

            if (isVip) {
                if (current + incomingBytes <= maxBytes_) {
                    return true;
                }
                if (!evictOne(true)) {
                    return false;
                }
                continue;
            }

            if (current + incomingBytes <= maxBytes_ &&
                normal + incomingBytes <= normalSoftLimit) {
                return true;
            }

            if (!evictOne(false)) {
                return false;
            }
        }
    }

    bool evictOne(bool allowVipEviction) {
        std::string victimKey;
        size_t victimShardIndex = 0;
        uint64_t oldestTouch = std::numeric_limits<uint64_t>::max();
        bool victimFound = false;
        bool victimIsVip = false;

        std::shared_ptr<CacheEntry> bestNormal;
        std::string bestNormalKey;
        size_t bestNormalShard = 0;

        std::shared_ptr<CacheEntry> bestAny;
        std::string bestAnyKey;
        size_t bestAnyShard = 0;

        for (size_t i = 0; i < kShardCount; ++i) {
            const auto &shard = shards_[i];
            std::shared_lock<std::shared_mutex> lock(shard.mutex);
            for (const auto &pair : shard.entries) {
                const auto &entry = pair.second;
                const uint64_t touch =
                    entry->lastTouchNs.load(std::memory_order_relaxed);
                if (!entry->isVip &&
                    (!bestNormal || touch <
                                        bestNormal->lastTouchNs.load(
                                            std::memory_order_relaxed))) {
                    bestNormal = entry;
                    bestNormalKey = pair.first;
                    bestNormalShard = i;
                }
                if (allowVipEviction &&
                    (!bestAny || touch <
                                    bestAny->lastTouchNs.load(
                                        std::memory_order_relaxed))) {
                    bestAny = entry;
                    bestAnyKey = pair.first;
                    bestAnyShard = i;
                }
            }
        }

        if (bestNormal) {
            victimKey = bestNormalKey;
            victimShardIndex = bestNormalShard;
            oldestTouch =
                bestNormal->lastTouchNs.load(std::memory_order_relaxed);
            victimFound = true;
            victimIsVip = false;
        } else if (allowVipEviction && bestAny) {
            victimKey = bestAnyKey;
            victimShardIndex = bestAnyShard;
            oldestTouch =
                bestAny->lastTouchNs.load(std::memory_order_relaxed);
            victimFound = true;
            victimIsVip = bestAny->isVip;
        }

        if (!victimFound) {
            return false;
        }

        auto &victimShard = shards_[victimShardIndex];
        std::unique_lock<std::shared_mutex> lock(victimShard.mutex);
        auto it = victimShard.entries.find(victimKey);
        if (it == victimShard.entries.end()) {
            return true;
        }
        if (!allowVipEviction && it->second->isVip) {
            return true;
        }
        const uint64_t currentTouch =
            it->second->lastTouchNs.load(std::memory_order_relaxed);
        if (currentTouch != oldestTouch && victimIsVip == it->second->isVip) {
            return true;
        }

        const size_t bytes = it->second->size;
        const bool isNormal = !it->second->isVip;
        victimShard.entries.erase(it);
        currentBytes_.fetch_sub(bytes, std::memory_order_relaxed);
        if (isNormal) {
            normalBytes_.fetch_sub(bytes, std::memory_order_relaxed);
        }
        return true;
    }

    void maybePruneExpired() {
        const uint64_t now = nowNs();
        const uint64_t nextAllowed =
            nextMaintenanceNs_.load(std::memory_order_relaxed);
        if (now < nextAllowed) {
            return;
        }

        std::lock_guard<std::mutex> guard(maintenanceMutex_);
        if (now < nextMaintenanceNs_.load(std::memory_order_relaxed)) {
            return;
        }
        pruneExpiredEntries(now);
        nextMaintenanceNs_.store(
            now + static_cast<uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          kMaintenanceInterval)
                          .count()),
            std::memory_order_relaxed);
    }

    void pruneExpiredEntries(uint64_t now) {
        const uint64_t ttlNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(kEntryTtl)
                .count());

        for (auto &shard : shards_) {
            std::vector<std::pair<size_t, bool>> removedEntries;
            {
                std::unique_lock<std::shared_mutex> lock(shard.mutex);
                for (auto it = shard.entries.begin(); it != shard.entries.end();) {
                    const uint64_t touch =
                        it->second->lastTouchNs.load(std::memory_order_relaxed);
                    if (now > touch && now - touch >= ttlNs) {
                        removedEntries.push_back(
                            {it->second->size, !it->second->isVip});
                        it = shard.entries.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            for (const auto &removed : removedEntries) {
                currentBytes_.fetch_sub(removed.first,
                                        std::memory_order_relaxed);
                if (removed.second) {
                    normalBytes_.fetch_sub(removed.first,
                                           std::memory_order_relaxed);
                }
            }
        }
    }

    size_t maxBytes_;
    mutable std::array<Shard, kShardCount> shards_;
    mutable std::mutex maintenanceMutex_;
    mutable std::mutex evictionMutex_;
    mutable std::atomic<uint64_t> nextMaintenanceNs_{0};
    std::atomic<size_t> currentBytes_{0};
    std::atomic<size_t> normalBytes_{0};
};
