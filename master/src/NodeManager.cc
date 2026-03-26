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
#include <random>
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
                                  const fn::InetAddress &newAddr,
                                  uint64_t disk_total, uint64_t disk_free,
                                  int active_uploads) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = nodes_.find(node_id);
    if (it != nodes_.end()) {
        it->second->lastHeartbeat_ = fileserver::Timestamp::now();
        it->second->isAlive_ = true;

        // 更新调度指标
        it->second->diskTotalMb_ = disk_total;
        it->second->diskFreeMb_ = disk_free;
        // 这里直接覆盖活跃数，心跳是最准确的真是状态
        it->second->activeUploads_ = active_uploads;

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

std::shared_ptr<DataNodeInfo>
NodeManager::getAliveNode(uint64_t requiredSpace) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<DataNodeInfo>> candidateNodes;
    double totalScore = 0.0;

    // 向上取整，预留一点余量
    uint64_t requiredMb = (requiredSpace + 1024 * 1024 - 1) / (1024 * 1024);

    for (const auto &pair : nodes_) {
        auto node = pair.second;
        if (!node->isAlive_)
            continue;

        // 硬性容量过滤
        // 如果该节点的剩余空间装不下文件，直接淘汰
        if (node->diskFreeMb_ > 0 && node->diskFreeMb_ < requiredMb) {
            LOG_WARN << "Node " << node->id_
                     << " free space not enough (free:" << node->diskFreeMb_
                     << "MB, required:" << requiredMb << "MB)";
            continue;
        }

        // 降级机制：如果没有上报过指标
        if (node->diskTotalMb_ == 0) {
            node->currentScore_ =
                50.0; // 给个中等基础分，参与分配 (相当于 Round-Robin)
            candidateNodes.push_back(node);
            totalScore += node->currentScore_;
            continue;
        }

        // 熔断机制：计算磁盘使用率，超过95%直接剔除
        double diskUsageRate =
            1.0 - (double)node->diskFreeMb_ / node->diskTotalMb_;
        if (diskUsageRate > 0.95) {
            LOG_WARN << "Node " << node->id_
                     << " usage over 95%, Trigger circuit breaker protection";
            continue;
        }

        uint64_t projectedFreeMb = node->diskFreeMb_ > requiredMb
                                       ? (node->diskFreeMb_ - requiredMb)
                                       : 0;
        double diskFreeRate = (double)projectedFreeMb / node->diskTotalMb_;

        // 多维打分机制 (权重: 磁盘可用率0.7，并发负载0.3)
        // 活跃连接数越少，得分越高。加1防止除以0
        double loadScore = 1.0 / (node->activeUploads_ + 1.0);

        // 综合得分
        double finalScore = (0.7 * diskFreeRate) + (0.3 * loadScore);

        // 放大为整数级别，方便轮盘赌计算
        node->currentScore_ = finalScore * 10000.0;

        candidateNodes.push_back(node);
        totalScore += node->currentScore_;
    }

    // 兜底机制：如果所有节点都熔断了，或者没有存活节点
    if (candidateNodes.empty()) {
        LOG_ERROR << "No available node";
        return nullptr;
    }

    // 轮盘赌平滑调度(解决并发雪崩)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> dis(0.0, totalScore);
    double randomPoint = dis(gen);

    double currentSum = 0.0;
    for (const auto &node : candidateNodes) {
        currentSum += node->currentScore_;
        if (currentSum >= randomPoint) {
            // 预测性补偿
            // 既然Master选择了它，在它下次心跳汇报前，Master内存里主动把它的活跃数+1
            // 防止在心跳间隔内，多个并发请求全部分配给它
            node->activeUploads_ += 1;
            if (node->diskFreeMb_ > requiredMb) {
                node->diskFreeMb_ -= requiredMb;
            } else {
                node->diskFreeMb_ = 0;
            }
            LOG_INFO << "Chosen node: " << node->id_
                     << " (Score: " << node->currentScore_
                     << ", predict activeUploads: " << node->activeUploads_
                     << ")";
            return node;
        }
    }
    return candidateNodes.back();
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