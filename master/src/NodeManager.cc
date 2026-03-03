#include "NodeManager.h"
#include "../../third_party/muduo/base/Logging.h"

void NodeManager::registerNode(const fn::InetAddress &addr) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        nodes_[addr.toIpPort()] = std::make_shared<DataNodeInfo>(addr);
    }
    LOG_INFO << "register node " << addr.toIpPort();
}

void NodeManager::updateHeartbeat(const fn::InetAddress &addr) {
    std::string key = addr.toIpPort();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (nodes_.find(key) != nodes_.end()) {
            nodes_[key]->lastHeartbeat_ = fileserver::Timestamp::now();
            nodes_[key]->isAlive_ = true;
        }
    }
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