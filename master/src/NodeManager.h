#pragma once
#include "DataNodeInfo.h"
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
    // 单列模式：全局只有一个实例
    static NodeManager &instance() {
        static NodeManager instance_;
        return instance_;
    }

    ///@brief 注册节点
    void registerNode(const fn::InetAddress &addr);

    ///@brief 更新心跳
    void updateHeartbeat(const fn::InetAddress &addr);

    ///@brief 启动超时检测定时器
    // 【新增】启动超时检测定时器（在 Master 启动时调用一次）
    void startTimeoutChecker(fn::EventLoop *loop, double interval = 5.0);

    ///@brief 获取一个活着的节点
    std::shared_ptr<DataNodeInfo> getAliveNode();

  private:
    ///@brief 扫描超时节点
    void checkTimeoutNodes();
    NodeManager() = default;
    ~NodeManager() = default;
    NodeManager(const NodeManager &) = delete;
    NodeManager &operator=(const NodeManager &) = delete;

    std::mutex mutex_; // 保护nodes_的锁
    std::unordered_map<std::string, std::shared_ptr<DataNodeInfo>> nodes_;
};