#pragma once

#include "Config.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class HotCacheService {
  public:
    // 预加载任务
    struct DispatchTask {
        int fileId = 0;
        std::string nodeId;
        std::string serverFilename;
        uint64_t preloadBytes = 0; // 预取字节数
        bool vipPriority = false;  // vip优先级标记
        int hotScore = 0;          // 当前热度分
    };

    static HotCacheService &instance() {
        static HotCacheService service;
        return service;
    }

    void init();
    bool enabled() const;

    // 记录一次“成功拿到下载地址”的访问事件。
    void recordAccess(int fileId, const std::string &nodeId,
                      const std::string &serverFilename, uint64_t fileSize,
                      const std::string &serviceLevel,
                      const std::string &sceneTag);

    // 定时扫描热点文件，返回本轮应该下发给 DataNode 的预热任务。
    std::vector<DispatchTask> collectDispatchTasks();

  private:
    struct HotFileEntry {
        int fileId = 0;
        std::string nodeId;
        std::string serverFilename;
        uint64_t fileSize = 0;
        int hotScore = 0; // 热度总分
        int totalHits = 0;
        int vipHits = 0;
        int learningHits = 0;
        int generalHits = 0;
        std::chrono::steady_clock::time_point lastAccessAt; // 最后访问时间
        std::chrono::steady_clock::time_point
            lastDispatchAt; // 最后下发任务时间
    };

    HotCacheService() = default;

    std::string buildEntryKey(const std::string &nodeId,
                              const std::string &serverFilename) const;

    // 计算本次访问得分
    int calculateAccessScore(const std::string &serviceLevel,
                             const std::string &sceneTag) const;
    uint64_t resolvePreloadBytes(const HotFileEntry &entry,
                                 bool &vipPriority) const;
    int resolveTriggerScore(const HotFileEntry &entry) const;

    mutable std::mutex mutex_;

    // 存储所有文件的热度状态
    std::unordered_map<std::string, HotFileEntry> entries_;

    bool initialized_ = false;
    bool enabled_ = true;
    int vipAccessScore_ = 40;
    int normalAccessScore_ = 10;
    int learningBonusScore_ = 6;
    int vipTriggerScore_ = 40;
    int learningTriggerScore_ = 32;
    int generalTriggerScore_ = 50;
    int vipPreloadMb_ = 12;
    int learningPreloadMb_ = 6;
    int generalPreloadMb_ = 2;
    int cooldownSeconds_ = 45;
    int idleExpireSeconds_ = 300;
    int maxTasksPerRound_ = 6;
    int maxTasksPerNode_ = 2;
    int dispatchDecayPercent_ = 60;
};
