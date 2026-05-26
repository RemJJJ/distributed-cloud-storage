#pragma once
#include "Config.h"
#include "DataNodeInfo.h"
#include "TokenManager.h"
#include "base/Timestamp.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace std {
template <> struct hash<fn::InetAddress> {
    size_t operator()(const fn::InetAddress &addr) const {
        return std::hash<std::string>()(addr.toIpPort());
    }
};
} // namespace std

class NodeManager {
  public:
    struct ClusterRuntimeStats {
        int onlineNodes = 0;
        int totalNodes = 0;
        int totalActiveConnections = 0;
        uint64_t totalUploadBps = 0;
        uint64_t totalDownloadBps = 0;
        uint64_t totalBandwidthBps = 0;
        uint64_t totalDiskBytes = 0;
        uint64_t totalDiskFreeBytes = 0;
        std::string qosMode = "elastic";
        bool manualQosOverride = false;
        uint64_t globalBandwidthLimitBps = 0;
        int strictEnterActiveTransfers = 0;
        int strictExitActiveTransfers = 0;
    };

    struct TrafficSample {
        int64_t timestampMs = 0;
        uint64_t uploadBps = 0;
        uint64_t downloadBps = 0;
    };

    struct AdminPolicySnapshot {
        std::string qosMode = "elastic";
        bool manualOverride = false;
        uint64_t globalBandwidthLimitBps = 0;
        uint64_t nodeBandwidthLimitBps = 0;
        int strictEnterActiveTransfers = 0;
        int strictExitActiveTransfers = 0;
    };

    enum class ClusterQosMode { kElastic, kStrict };

    ~NodeManager() = default;
    /// @brief 首次调用时传入配置路径，查看config是否初始化
    static void init(const std::string &configPath = "config.json");

    /// @brief 单列模式：全局只有一个实例
    static NodeManager &instance() {
        static NodeManager instance;
        return instance;
    }

    /// @brief 检查是否已初始化
    static bool isInitialized();

    ///@brief 注册节点
    TokenManager::nodeRegisterResponse
    registerNode(const std::string &reported_node_id,
                 const fn::InetAddress &addr,
                 const std::string &public_url = "");

    ///@brief 更新心跳
    void updateHeartbeat(const std::string &node_id,
                         const fn::InetAddress &newAddr, uint64_t disk_total,
                         uint64_t disk_free, int active_uploads,
                         int active_downloads, int active_transfers,
                         uint64_t upload_bps, uint64_t download_bps,
                         int connected_users,
                         const std::vector<ActiveUserSessionInfo> &active_users,
                         const std::string &public_url = "");

    ///@brief 启动超时检测定时器（在 Master 启动时调用一次）
    void startTimeoutChecker(fn::EventLoop *loop, double interval = 5.0);

    ///@brief 获取一个活着的节点
    std::shared_ptr<DataNodeInfo> getAliveNode(uint64_t requiredSpace = 0);

    /// @brief 用node_id获取节点
    std::shared_ptr<DataNodeInfo> getNodeInfo(const std::string &node_id);
    std::vector<std::shared_ptr<DataNodeInfo>> getAllNodes();
    ClusterRuntimeStats getClusterRuntimeStats();
    std::vector<ActiveUserSessionInfo> getActiveUserAudits();
    std::vector<TrafficSample> getTrafficHistory(int windowSeconds) const;
    AdminPolicySnapshot getAdminPolicySnapshot() const;
    AdminPolicySnapshot getAdminPolicySnapshot(
        const std::string &node_id) const;
    void setNodeManualDisabled(const std::string &node_id, bool disabled);
    void setNodeBandwidthLimitBps(const std::string &node_id, uint64_t limit);
    void setManualQosOverride(bool enabled, ClusterQosMode mode);
    void setGlobalBandwidthLimitBps(uint64_t limit);
    void setStrictModeThresholds(int enterActiveTransfers,
                                 int exitActiveTransfers);

    /// @brief 获取当前集群 QoS 模式
    ClusterQosMode getClusterQosMode() const;

    /// @brief 根据用户等级和方向生成 QoS 策略
    TokenManager::QoSPolicy buildQoSPolicy(const std::string &service_level,
                                           bool is_download) const;

    /// @brief 获取配置值
    template <typename T>
    T getConfig(const std::string &key, T defaultVal = {}) const {
        return Config::instance().get(key, defaultVal);
    }

  private:
    ///@brief 扫描超时节点
    void checkTimeoutNodes();
    int getTotalActiveTransfersLocked() const;
    void refreshClusterQosModeLocked();
    void appendTrafficSampleLocked();
    static std::string normalizeServiceLevel(const std::string &service_level);
    NodeManager() = default;
    NodeManager(const NodeManager &) = delete;
    NodeManager &operator=(const NodeManager &) = delete;

    mutable std::mutex mutex_; // 保护nodes_的锁
    std::unordered_map<std::string, std::shared_ptr<DataNodeInfo>> nodes_;

    bool qosEnabled_ = false;
    // 双阈值迟滞控制(防止状态震荡)
    int strictEnterActiveTransfers_ = 8;
    int strictExitActiveTransfers_ = 4;
    int normalUploadRateKbps_ = 512;
    int normalDownloadRateKbps_ = 1024;
    int tokenBucketCapacityKb_ = 256;
    ClusterQosMode clusterQosMode_ = ClusterQosMode::kElastic;
    bool manualQosOverride_ = false;
    ClusterQosMode manualQosMode_ = ClusterQosMode::kElastic;
    uint64_t globalBandwidthLimitBps_ = 0;
    mutable std::deque<TrafficSample> trafficHistory_;
    bool initialized_;
};
