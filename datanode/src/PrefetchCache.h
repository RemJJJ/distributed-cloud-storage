#pragma once

#include <chrono>
#include <cstddef>
#include <list>
#include <mutex>
#include <string>
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
        // 已经在缓存里，或别人正在读
        if (entries_.count(key) > 0 || loadingKeys_.count(key) > 0) {
            return false;
        }
        loadingKeys_.insert(key); // 标记为正在读磁盘
        return true;
    }

    // 读取失败时取消标记
    void cancelLoading(const std::string &key) {
        std::lock_guard<std::mutex> lock(mutex_);
        loadingKeys_.erase(key);
    }

    // 把从磁盘读到的数据放进缓存
    void put(const std::string &key, std::vector<char> data) {
        std::lock_guard<std::mutex> lock(mutex_);
        loadingKeys_.erase(key);

        if (data.empty()) {
            return;
        }
        if (data.size() > maxBytes_) {
            return;
        }

        // 已经存在，先删除旧的
        auto existing = entries_.find(key);
        if (existing != entries_.end()) {
            currentBytes_ -= existing->second.data.size();
            lruKeys_.erase(existing->second.lruIt);
            entries_.erase(existing);
        }

        // 内存不够了，淘汰最久没用过的数据
        evictIfNeededLocked(data.size());

        // 把新数据插到链表头部
        lruKeys_.push_front(key);

        CacheEntry entry;
        entry.data = std::move(data);
        entry.lruIt = lruKeys_.begin();
        entry.lastAccess = std::chrono::steady_clock::now();
        currentBytes_ += entry.data.size();
        entries_.emplace(key, std::move(entry));
    }

  private:
    struct CacheEntry {
        std::vector<char> data;
        std::list<std::string>::iterator lruIt;
        std::chrono::steady_clock::time_point lastAccess;
    };

    using EntryMap = std::unordered_map<std::string, CacheEntry>;

    // 把数据块移到LRU链表最前面
    void touchLocked(EntryMap::iterator it) {
        lruKeys_.erase(it->second.lruIt);
        lruKeys_.push_front(it->first);
        it->second.lruIt = lruKeys_.begin();
        it->second.lastAccess = std::chrono::steady_clock::now();
    }

    // 淘汰最久没用过的数据
    void evictIfNeededLocked(size_t incomingBytes) {
        while (!lruKeys_.empty() && currentBytes_ + incomingBytes > maxBytes_) {
            auto tailIt = std::prev(lruKeys_.end());
            auto entryIt = entries_.find(*tailIt);
            if (entryIt == entries_.end()) {
                lruKeys_.erase(tailIt);
                continue;
            }

            currentBytes_ -= entryIt->second.data.size();
            lruKeys_.erase(entryIt->second.lruIt);
            entries_.erase(entryIt);
        }
    }

    std::mutex mutex_; // 全局互斥锁
    size_t maxBytes_;
    size_t currentBytes_ = 0;
    std::list<std::string> lruKeys_;              // LRU链表
    EntryMap entries_;                            // 哈希表，存储实际缓存数据
    std::unordered_set<std::string> loadingKeys_; // 正在加载的key
};
