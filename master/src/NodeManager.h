#pragma once
#include "Config.h"
#include "DataNodeInfo.h"
#include "TokenManager.h"
#include "base/Timestamp.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
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
                         const fn::InetAddress &newAddr);

    ///@brief 启动超时检测定时器（在 Master 启动时调用一次）
    void startTimeoutChecker(fn::EventLoop *loop, double interval = 5.0);

    ///@brief 获取一个活着的节点
    std::shared_ptr<DataNodeInfo> getAliveNode();

    /// @brief 用node_id获取节点
    std::shared_ptr<DataNodeInfo> getNodeInfo(const std::string &node_id);

    /// @brief 获取配置值
    template <typename T>
    T getConfig(const std::string &key, T defaultVal = {}) const {
        return Config::instance().get(key, defaultVal);
    }

  private:
    ///@brief 扫描超时节点
    void checkTimeoutNodes();
    NodeManager() = default;
    NodeManager(const NodeManager &) = delete;
    NodeManager &operator=(const NodeManager &) = delete;

    std::mutex mutex_; // 保护nodes_的锁
    std::unordered_map<std::string, std::shared_ptr<DataNodeInfo>> nodes_;

    bool initialized_;
};