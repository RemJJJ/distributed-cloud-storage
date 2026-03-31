#pragma once
#include "Config.h"
#include "DataNodeInfo.h"
#include "TokenManager.h"
#include "base/Timestamp.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include <cstdint>
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
                 const fn::InetAddress &addr);

    ///@brief 更新心跳
    void updateHeartbeat(const std::string &node_id,
                         const fn::InetAddress &newAddr, uint64_t disk_total,
                         uint64_t disk_free, int active_uploads,
                         int active_downloads, int active_transfers);

    ///@brief 启动超时检测定时器（在 Master 启动时调用一次）
    void startTimeoutChecker(fn::EventLoop *loop, double interval = 5.0);

    ///@brief 获取一个活着的节点
    std::shared_ptr<DataNodeInfo> getAliveNode(uint64_t requiredSpace = 0);

    /// @brief 用node_id获取节点
    std::shared_ptr<DataNodeInfo> getNodeInfo(const std::string &node_id);

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
    bool initialized_;
};
