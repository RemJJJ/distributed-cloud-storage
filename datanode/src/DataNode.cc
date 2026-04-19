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
#include <ifaddrs.h>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>

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
