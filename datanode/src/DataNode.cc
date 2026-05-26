#include "DataNode.h"
#include "Config.h"
#include "DataNodeHttpHandler.h"
#include "DataNodeServer.h"
#include "MasterClient.h"
#include "TokenManager.h"
#include "base/Logging.h"
#include "net/Callbacks.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include "net/HttpServer.h"
#include "net/InetAddress.h"
#include "net/TcpServer.h"
#include <arpa/inet.h>
#include <chrono>
#include <ifaddrs.h>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>

namespace {
std::string normalizeServiceLevel(std::string serviceLevel) {
    std::transform(serviceLevel.begin(), serviceLevel.end(),
                   serviceLevel.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    if (serviceLevel == "svip") {
        return "svip";
    }
    if (serviceLevel == "vip") {
        return "vip";
    }
    return "normal";
}

uint64_t serviceWeight(const std::string &serviceLevel) {
    if (serviceLevel == "svip") {
        return 6;
    }
    if (serviceLevel == "vip") {
        return 3;
    }
    return 1;
}

uint64_t serviceWeightForMode(const std::string &serviceLevel,
                              const std::string &qosMode) {
    if (qosMode == "strict") {
        return serviceWeight(serviceLevel);
    }
    if (serviceLevel == "svip") {
        return 3;
    }
    if (serviceLevel == "vip") {
        return 2;
    }
    return 1;
}
} // namespace

DataNode::DataNode(fn::EventLoop *loop, const fn::InetAddress &listenAddr,
                   const fn::InetAddress &masterAddr)
    : loop_(loop) {
    std::string publicUrl = Config::instance().getString("datanode.public_url");
    if (publicUrl.empty()) {
        publicUrl = "http://" + listenAddr.toIpPort();
    } else if (!publicUrl.empty() && publicUrl.back() == '/') {
        publicUrl.pop_back();
    }
    masterClient_ = std::make_unique<MasterClient>(loop, masterAddr, listenAddr,
                                                   publicUrl, this);
    datanodeServer_ =
        std::make_unique<fn::HttpServer>(loop, listenAddr, "datanodeServer");
    handler_ = std::make_shared<DataNodeHttpHandler>(this);

    datanodeServer_->setConnectionCallback(
        [this](const TcpConnectionPtr &conn) {
            this->handler_->onConnection(conn);
        });

    datanodeServer_->setHttpCallback(
        [this](const TcpConnectionPtr &conn, HttpRequest &req,
               std::shared_ptr<HttpResponse> &resp) {
            return this->handler_->onRequest(conn, req, resp);
        });

    datanodeServer_->setThreadNum(16);
}

DataNode::~DataNode() {}

void DataNode::start() {
    datanodeServer_->start();
    masterClient_->start();
    masterClient_->startHeartbeat();
}

void DataNode::registerTransferSession(const std::string &sessionId, int userId,
                                       const std::string &username,
                                       const std::string &serviceLevel,
                                       const std::string &sceneTag,
                                       const std::string &transferType,
                                       const std::string &fileName,
                                       const std::string &startedAt) {
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now()
                                  .time_since_epoch())
                              .count();
    std::lock_guard<std::mutex> lock(sessionMutex_);
    transferSessions_[sessionId] = TransferSessionRuntime{
        userId, username, serviceLevel, sceneTag,
        transferType, fileName, startedAt, 0, 0, nowUs};
}

void DataNode::recordTransferBytes(const std::string &sessionId,
                                   uint64_t bytes) {
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now()
                                  .time_since_epoch())
                              .count();
    std::lock_guard<std::mutex> lock(sessionMutex_);
    auto it = transferSessions_.find(sessionId);
    if (it == transferSessions_.end()) {
        return;
    }
    auto &session = it->second;
    session.windowBytes += bytes;
    const int64_t elapsedUs = nowUs - session.lastRateUpdateUs;
    if (elapsedUs >= 500000) {
        session.currentBps =
            static_cast<uint64_t>(session.windowBytes * 1000000.0 / elapsedUs);
        session.windowBytes = 0;
        session.lastRateUpdateUs = nowUs;
    }
}

void DataNode::unregisterTransferSession(const std::string &sessionId) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    transferSessions_.erase(sessionId);
}

std::vector<DataNode::TransferSessionSnapshot>
DataNode::collectTransferSessionsLocked() const {
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now()
                                  .time_since_epoch())
                              .count();
    std::vector<TransferSessionSnapshot> snapshots;
    snapshots.reserve(transferSessions_.size());
    for (const auto &pair : transferSessions_) {
        const auto &session = pair.second;
        uint64_t currentBps = session.currentBps;
        const int64_t elapsedUs = nowUs - session.lastRateUpdateUs;
        if (elapsedUs > 0 && session.windowBytes > 0) {
            currentBps = static_cast<uint64_t>(session.windowBytes * 1000000.0 /
                                               elapsedUs);
        }
        snapshots.push_back(TransferSessionSnapshot{
            session.userId,      session.username, session.serviceLevel,
            session.sceneTag,    session.transferType,
            session.fileName,    currentBps,
            session.startedAt});
    }
    return snapshots;
}

uint64_t DataNode::collectTransferBpsLocked(const std::string &transferType) const {
    uint64_t totalBps = 0;
    for (const auto &session : collectTransferSessionsLocked()) {
        if (session.transferType == transferType) {
            totalBps += session.currentBps;
        }
    }
    return totalBps;
}

std::vector<DataNode::TransferSessionSnapshot>
DataNode::getTransferSessionSnapshots() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return collectTransferSessionsLocked();
}

uint64_t DataNode::getCurrentUploadBps() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return collectTransferBpsLocked("upload");
}

uint64_t DataNode::getCurrentDownloadBps() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return collectTransferBpsLocked("download");
}

int DataNode::getConnectedUsers() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    std::unordered_set<int> userIds;
    for (const auto &pair : transferSessions_) {
        if (pair.second.userId > 0) {
            userIds.insert(pair.second.userId);
        }
    }
    return static_cast<int>(userIds.size());
}

void DataNode::updateAdminPolicy(const AdminPolicyCache &policy) {
    std::lock_guard<std::mutex> lock(adminPolicyMutex_);
    adminPolicy_ = policy;
}

DataNode::AdminPolicyCache DataNode::getAdminPolicy() const {
    std::lock_guard<std::mutex> lock(adminPolicyMutex_);
    return adminPolicy_;
}

uint64_t DataNode::getGlobalBandwidthLimitBps() const {
    std::lock_guard<std::mutex> lock(adminPolicyMutex_);
    return adminPolicy_.globalBandwidthLimitBps;
}

uint64_t DataNode::getNodeBandwidthLimitBps() const {
    std::lock_guard<std::mutex> lock(adminPolicyMutex_);
    return adminPolicy_.nodeBandwidthLimitBps;
}

uint64_t DataNode::getEffectiveBandwidthLimitBps() const {
    std::lock_guard<std::mutex> lock(adminPolicyMutex_);
    const uint64_t globalLimit = adminPolicy_.globalBandwidthLimitBps;
    const uint64_t nodeLimit = adminPolicy_.nodeBandwidthLimitBps;
    if (globalLimit > 0 && nodeLimit > 0) {
        return std::min(globalLimit, nodeLimit);
    }
    return globalLimit > 0 ? globalLimit : nodeLimit;
}

uint64_t DataNode::getWeightedRateLimitBps(
    const std::string &serviceLevel, const std::string &qosMode) const {
    const uint64_t totalLimitBps = getEffectiveBandwidthLimitBps();
    if (totalLimitBps == 0) {
        return 0;
    }

    const std::string normalizedLevel = normalizeServiceLevel(serviceLevel);
    const uint64_t currentWeight =
        serviceWeightForMode(normalizedLevel, qosMode);

    std::lock_guard<std::mutex> lock(sessionMutex_);
    uint64_t totalWeight = 0;
    for (const auto &pair : transferSessions_) {
        totalWeight += serviceWeightForMode(
            normalizeServiceLevel(pair.second.serviceLevel), qosMode);
    }

    if (totalWeight == 0) {
        totalWeight = currentWeight;
    }

    const uint64_t allocated =
        static_cast<uint64_t>((static_cast<long double>(totalLimitBps) *
                               static_cast<long double>(currentWeight)) /
                              static_cast<long double>(totalWeight));
    return std::max<uint64_t>(allocated, 1);
}

// 自定义删除器
struct ifaddrs_deleter {
    void operator()(ifaddrs *p) const {
        if (p)
            freeifaddrs(p);
    }
};

std::string getLocalInternalIP() {
    ifaddrs *raw_addrs = nullptr;
    if (getifaddrs(&raw_addrs) == -1) {
        LOG_FATAL << "getifaddrs 调用失败";
        return "";
    }

    std::unique_ptr<ifaddrs, ifaddrs_deleter> addrs(raw_addrs);
    std::string result_ip;

    // 定义内网 IP 前缀列表 (用于简单验证)
    const std::vector<std::string> internal_prefixes = {
        "192.168.", "10.",     "172.16.", "172.17.", "172.18.", "172.19.",
        "172.20.",  "172.21.", "172.22.", "172.23.", "172.24.", "172.25.",
        "172.26.",  "172.27.", "172.28.", "172.29.", "172.30.", "172.31."};

    for (ifaddrs *ifa = addrs.get(); ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr)
            continue;

        // 排除回环接口和常见的虚拟接口
        std::string if_name(ifa->ifa_name);
        if (if_name == "lo" || if_name.find("docker") != std::string::npos ||
            if_name.find("virbr") != std::string::npos ||
            if_name.find("vmnet") != std::string::npos) {
            continue;
        }

        const int family = ifa->ifa_addr->sa_family;
        if (family == AF_INET) {
            char ip_buffer[INET_ADDRSTRLEN] = {0}; // 初始化
            auto *addr_in =
                reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
            inet_ntop(AF_INET, &(addr_in->sin_addr), ip_buffer,
                      INET_ADDRSTRLEN);

            std::string ip(ip_buffer);

            // 简单的内网 IP 验证
            bool is_internal = false;
            for (const auto &prefix : internal_prefixes) {
                if (ip.find(prefix) == 0) {
                    is_internal = true;
                    break;
                }
            }

            if (is_internal) {
                result_ip = ip;
                break; // 找到第一个符合条件的就立即返回
            }
        }
    }

    if (result_ip.empty()) {
        LOG_FATAL << "无法获取有效的内网 IPv4 地址，请检查网络配置";
    }

    return result_ip;
}

int main(int argc, char *argv[]) {
    std::string loggerLevel;
    std::string configPath = "config.json";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-D" || arg == "--debug") {
            loggerLevel = "DEBUG";
        } else if (arg.find("--config=") == 0) {
            configPath = arg.substr(9);
        } else if (i == 1 && loggerLevel.empty()) {
            // 兼容旧用法：第一个参数如果不是 -D，暂时认为是日志级别 (可选)
            loggerLevel = arg;
        } else if (i == 2) {
            // 兼容旧用法：第二个参数是配置路径
            configPath = arg;
        }
    }

    // 2. 设置日志级别
    if (loggerLevel == "DEBUG" || loggerLevel == "-D") {
        Logger::setLogLevel(Logger::DEBUG);
        LOG_INFO << "日志级别设置为 DEBUG";
    } else {
        Logger::setLogLevel(Logger::INFO);
    }

    try {
        if (!Config::instance().load(configPath)) {
            LOG_ERROR << "无法打开配置文件：" << configPath;
            return -1;
        }
        LOG_INFO << "配置文件加载成功";
    } catch (const std::exception &e) {
        LOG_ERROR << e.what();
        return -1;
    }

    // 初始化Token管理器
    TokenManager::init(Config::instance().getString("jwt-secret"));
    // 验证是否初始化成功
    if (!TokenManager::isInitialized()) {
        LOG_FATAL << "TokenManager 初始化失败";
        return -1;
    }

    // 获取自身内网IP：优先读配置，未配置时自动探测。
    std::string local_ip = Config::instance().getString("datanode.local_host");
    if (local_ip.empty()) {
        local_ip = getLocalInternalIP();
    }
    LOG_INFO << "Local listen IP: " << local_ip;

    EventLoop loop;

    // 3. 配置地址
    fileserver::net::InetAddress listenAddr(
        local_ip, Config::instance().getInt("datanode.local_port", 9000));
    fileserver::net::InetAddress masterAddr(
        Config::instance().getString("master.ip"),
        Config::instance().getInt("master.port")); // Master 地址

    DataNode datanode(&loop, listenAddr, masterAddr);
    datanode.start();

    loop.loop();
    return 0;
}
