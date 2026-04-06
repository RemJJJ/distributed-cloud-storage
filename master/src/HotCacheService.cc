#include "HotCacheService.h"
#include "base/Logging.h"
#include <algorithm>
#include <unordered_map>

void HotCacheService::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return;
    }

    auto &config = Config::instance();
    enabled_ = config.getBool("hot_cache.enabled", true);
    vipAccessScore_ = config.getInt("hot_cache.vip_access_score", 40);
    normalAccessScore_ = config.getInt("hot_cache.normal_access_score", 10);
    learningBonusScore_ = config.getInt("hot_cache.learning_bonus_score", 6);
    vipTriggerScore_ = config.getInt("hot_cache.vip_trigger_score", 40);
    learningTriggerScore_ =
        config.getInt("hot_cache.learning_trigger_score", 32);
    generalTriggerScore_ = config.getInt("hot_cache.general_trigger_score", 50);
    vipPreloadMb_ = config.getInt("hot_cache.vip_preload_mb", 12);
    learningPreloadMb_ = config.getInt("hot_cache.learning_preload_mb", 6);
    generalPreloadMb_ = config.getInt("hot_cache.general_preload_mb", 2);
    cooldownSeconds_ = config.getInt("hot_cache.cooldown_seconds", 45);
    idleExpireSeconds_ = config.getInt("hot_cache.idle_expire_seconds", 300);
    maxTasksPerRound_ = config.getInt("hot_cache.max_tasks_per_round", 6);
    maxTasksPerNode_ = config.getInt("hot_cache.max_tasks_per_node", 2);
    dispatchDecayPercent_ = std::clamp(
        config.getInt("hot_cache.dispatch_decay_percent", 60), 0, 100);

    initialized_ = true;
    LOG_INFO << "Hot cache config loaded. enabled=" << enabled_
             << ", vip_trigger=" << vipTriggerScore_
             << ", learning_trigger=" << learningTriggerScore_
             << ", general_trigger=" << generalTriggerScore_
             << ", vip_preload_mb=" << vipPreloadMb_
             << ", learning_preload_mb=" << learningPreloadMb_
             << ", general_preload_mb=" << generalPreloadMb_;
}

bool HotCacheService::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

void HotCacheService::recordAccess(int fileId, const std::string &nodeId,
                                   const std::string &serverFilename,
                                   uint64_t fileSize,
                                   const std::string &serviceLevel,
                                   const std::string &sceneTag) {
    init();
    if (!enabled_ || fileId <= 0 || nodeId.empty() || serverFilename.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const std::string key = buildEntryKey(nodeId, serverFilename);

    std::lock_guard<std::mutex> lock(mutex_);
    auto &entry = entries_[key];

    bool isNewEntry = (entry.fileId == 0);

    entry.fileId = fileId;
    entry.nodeId = nodeId;
    entry.serverFilename = serverFilename;
    entry.fileSize = fileSize;
    entry.hotScore += calculateAccessScore(serviceLevel, sceneTag);
    // 更新统计画像
    entry.totalHits += 1;
    if (serviceLevel == "vip") {
        entry.vipHits += 1;
    }
    if (sceneTag == "learning") {
        entry.learningHits += 1;
    } else {
        entry.generalHits += 1;
    }
    entry.lastAccessAt = now;
    if (isNewEntry) {
        LOG_INFO << "Start tracking hot file candidate. file_id=" << fileId
                 << ", node_id=" << nodeId << ", filename=" << serverFilename
                 << ", service_level=" << serviceLevel;
    }
}

std::vector<HotCacheService::DispatchTask>
HotCacheService::collectDispatchTasks() {
    init();
    std::vector<DispatchTask> tasks;
    if (!enabled_) {
        return tasks;
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);

    // 遍历entries_, 删除超过idleExpireSeconds_(默认5分钟) 没访问过的文件
    for (auto it = entries_.begin(); it != entries_.end();) {
        const auto idleSeconds =
            std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.lastAccessAt)
                .count();
        if (idleSeconds >= idleExpireSeconds_) {
            LOG_INFO << "Evicting idle hot file entry. file_id="
                     << it->second.fileId << ", node_id=" << it->second.nodeId
                     << ", idle_seconds=" << idleSeconds;
            it = entries_.erase(it);
            continue;
        }
        ++it;
    }

    // 按热度降序
    std::unordered_map<std::string, int> nodeTaskCount;
    std::vector<std::reference_wrapper<HotFileEntry>> candidates;
    candidates.reserve(entries_.size());
    for (auto &pair : entries_) {
        candidates.emplace_back(pair.second);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const std::reference_wrapper<HotFileEntry> &lhs,
                 const std::reference_wrapper<HotFileEntry> &rhs) {
                  return lhs.get().hotScore > rhs.get().hotScore;
              });

    for (HotFileEntry &entry : candidates) {
        // 总量限制，避免每轮下发任务太多，把 DataNode 带宽压垮。
        if (static_cast<int>(tasks.size()) >= maxTasksPerRound_) {
            break;
        }

        // 冷却时间，防止同一个文件刚下发完任务，没过几秒又下发一次（防刷屏）。
        const auto cooldownSeconds =
            std::chrono::duration_cast<std::chrono::seconds>(
                now - entry.lastDispatchAt)
                .count();
        if (entry.lastDispatchAt.time_since_epoch().count() > 0 &&
            cooldownSeconds < cooldownSeconds_) {
            continue;
        }

        bool vipPriority = false;
        // 预取字节数
        const uint64_t preloadBytes = resolvePreloadBytes(entry, vipPriority);
        // 触发分数，热度必须达到门槛（VIP 40 分 / 学习 32 分 / 普通 50 分）。
        const int triggerScore = resolveTriggerScore(entry);
        if (preloadBytes == 0 || entry.hotScore < triggerScore) {
            continue;
        }

        // 节点限流，防止所有任务都集中在同一个 DataNode
        // 上，导致该节点负载过高。
        int &currentNodeTasks = nodeTaskCount[entry.nodeId];
        if (currentNodeTasks >= maxTasksPerNode_) {
            continue;
        }

        LOG_INFO << "Selected hot file for pre-dispatch. file_id="
                 << entry.fileId << ", node_id=" << entry.nodeId
                 << ", hot_score=" << entry.hotScore
                 << ", trigger_score=" << triggerScore
                 << ", preload_mb=" << (preloadBytes / 1024 / 1024)
                 << ", vip=" << vipPriority;

        tasks.push_back({entry.fileId, entry.nodeId, entry.serverFilename,
                         preloadBytes, vipPriority, entry.hotScore});
        currentNodeTasks += 1;
        entry.lastDispatchAt = now;

        // 每次下发后做一次衰减，避免同一个文件在短时间内反复霸榜。
        entry.hotScore = (entry.hotScore * dispatchDecayPercent_) / 100;
        entry.totalHits =
            std::max(0, (entry.totalHits * dispatchDecayPercent_) / 100);
        entry.vipHits =
            std::max(0, (entry.vipHits * dispatchDecayPercent_) / 100);
        entry.learningHits =
            std::max(0, (entry.learningHits * dispatchDecayPercent_) / 100);
        entry.generalHits =
            std::max(0, (entry.generalHits * dispatchDecayPercent_) / 100);
    }

    if (!tasks.empty()) {
        LOG_INFO << "Hot cache scheduler selected " << tasks.size()
                 << " file(s) for preload";
    }
    return tasks;
}

std::string
HotCacheService::buildEntryKey(const std::string &nodeId,
                               const std::string &serverFilename) const {
    return nodeId + ":" + serverFilename;
}

int HotCacheService::calculateAccessScore(const std::string &serviceLevel,
                                          const std::string &sceneTag) const {
    // VIP用户一次加40，普通用户一次10分，学习场景加成:如果是learning场景，额外加6分
    int score = (serviceLevel == "vip") ? vipAccessScore_ : normalAccessScore_;
    if (sceneTag == "learning") {
        score += learningBonusScore_;
    }
    return score;
}

uint64_t HotCacheService::resolvePreloadBytes(const HotFileEntry &entry,
                                              bool &vipPriority) const {
    vipPriority = entry.vipHits > 0;
    if (vipPriority) {
        return static_cast<uint64_t>(vipPreloadMb_) * 1024ULL * 1024ULL;
    }
    if (entry.learningHits > entry.generalHits) {
        return static_cast<uint64_t>(learningPreloadMb_) * 1024ULL * 1024ULL;
    }
    return static_cast<uint64_t>(generalPreloadMb_) * 1024ULL * 1024ULL;
}

int HotCacheService::resolveTriggerScore(const HotFileEntry &entry) const {
    if (entry.vipHits > 0) {
        return vipTriggerScore_;
    }
    if (entry.learningHits > entry.generalHits) {
        return learningTriggerScore_;
    }
    return generalTriggerScore_;
}
