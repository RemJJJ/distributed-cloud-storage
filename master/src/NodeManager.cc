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
NodeManager::registerNode(const std::string &reported_node_id,
                          const fn::InetAddress &addr) {
    std::string final_node_id;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 如果DataNode传来了老ID，并且这个ID看起来合法，就信任它
        if (!reported_node_id.empty()) {
            final_node_id = reported_node_id;

            // 如果内存里没有这个老节点的信息，重新建一个
            if (nodes_.find(final_node_id) == nodes_.end()) {
                auto info = std::make_shared<DataNodeInfo>(addr);
                info->id_ = final_node_id;
                nodes_[final_node_id] = info;
            } else {
                // 如果内存里有，更新它的最新IP地址
                nodes_[final_node_id]->addr_ = addr;
            }
        } else {
            // 全新节点
            auto info = std::make_shared<DataNodeInfo>(addr);
            final_node_id = info->id_;
            nodes_[final_node_id] = info;
        }

        // 标记存活
        nodes_[final_node_id]->isAlive_ = true;
        nodes_[final_node_id]->lastHeartbeat_ = fileserver::Timestamp::now();
    }

    // 生成token返回
    auto &tm = TokenManager::instance();
    return tm.generateNodeToken(final_node_id, addr);
}

void NodeManager::updateHeartbeat(const std::string &node_id,
                                  const fn::InetAddress &newAddr) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = nodes_.find(node_id);
    if (it != nodes_.end()) {
        it->second->lastHeartbeat_ = fileserver::Timestamp::now();
        it->second->isAlive_ = true;

        // 如果更换了IP或端口，直接更新
        if (it->second->addr_.toIpPort() != newAddr.toIpPort()) {
            LOG_INFO << "Address of datanode changed: " << node_id << " -> "
                     << newAddr.toIpPort();
            it->second->addr_ = newAddr;
        }
    } else {
        LOG_WARN << "Update heartbeat failed: datanode not. node_id="
                 << node_id;
    }
}

std::shared_ptr<DataNodeInfo>
NodeManager::getNodeInfo(const std::string &node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodes_.find(node_id);
    if (it != nodes_.end()) {
        return it->second;
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