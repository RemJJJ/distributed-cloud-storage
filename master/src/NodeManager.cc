#include "NodeManager.h"
#include "Config.h"
#include "DataNodeInfo.h"
#include "base/Logging.h"
#include "base/Timestamp.h"
#include "field_types.h"
#include "net/TimerId.h"
#include "nlohmann/json.hpp"
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>

void NodeManager::init(const std::string &configPath) {
    static std::once_flag flag;
    std::call_once(flag, [&]() {
        if (!Config::instance().isLoaded()) {
            Config::instance().load(configPath);
        }

        if (!TokenManager::instance().isInitialized()) {
            TokenManager::instance().init(configPath);
        }
        instance().initialized_ = true;
    });
}

bool NodeManager::isInitialized() { return instance().initialized_; }

TokenManager::nodeRegisterResponse
NodeManager::registerNode(const fn::InetAddress &addr) {
    std::string node_id;
    std::string token;
    TokenManager::nodeRegisterResponse nodeToken;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 创建或获取节点信息
        auto iter = nodes_.find(addr.toIpPort());
        if (iter == nodes_.end()) {
            auto info = std::make_shared<DataNodeInfo>(addr);
            nodes_[addr.toIpPort()] = info;
            node_id = info->id_;
        } else {
            node_id = iter->second->id_; // 如果已注册，沿用旧ID
        }
    }

    // 生成Token
    auto &tm = TokenManager::instance();
    return tm.generateNodeToken(node_id, addr);
}

void NodeManager::updateHeartbeat(const std::string &node_id,
                                  const fn::InetAddress &newAddr) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &pair : nodes_) {
        if (pair.second->id_ == node_id) {
            pair.second->lastHeartbeat_ = fileserver::Timestamp::now();
            pair.second->isAlive_ = true;
        }

        // 如果地址变了，更新地址
        if (pair.second->addr_.toIpPort() != newAddr.toIpPort()) {
            LOG_INFO << "节点地址更新：" << pair.first << " -> "
                     << newAddr.toIpPort();

            // 从旧 key 移除，用新 key 存储
            auto info = pair.second;
            nodes_.erase(pair.first);
            nodes_[newAddr.toIpPort()] = info;
        }
        return;
    }
    LOG_WARN << "更新心跳失败：节点不存在 node_id=" << node_id;
}

std::shared_ptr<DataNodeInfo>
NodeManager::getNodeInfo(const std::string &node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &node : nodes_) {
        if (node.second->id_ == node_id) {
            return node.second;
        }
    }
    return nullptr;
}

std::shared_ptr<DataNodeInfo> NodeManager::getAliveNode() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 轮询
        for (auto &node : nodes_) {
            if (node.second->isAlive_) {
                return node.second;
            }
        }
    }

    // 没有存活节点
    LOG_WARN << "No alive node found";

    return nullptr;
}

void NodeManager::startTimeoutChecker(fn::EventLoop *loop, double interval) {
    loop->runEvery(interval, std::bind(&NodeManager::checkTimeoutNodes, this));
}

void NodeManager::checkTimeoutNodes() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &pair : nodes_) {
        auto node = pair.second;
        if (node->isAlive_ && node->isTimeout()) {
            node->isAlive_ = false;
            LOG_WARN << "DataNode 超时离线: " << pair.first << " (最后心跳: "
                     << node->lastHeartbeat_.toFormattedString() << ")";
        }
    }
}