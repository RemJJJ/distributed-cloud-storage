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
#include <iostream>

DataNode::DataNode(fn::EventLoop *loop, const fn::InetAddress &listenAddr,
                   const fn::InetAddress &masterAddr)
    : loop_(loop) {
    masterClient_ =
        std::make_unique<MasterClient>(loop, masterAddr, listenAddr);
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

    datanodeServer_->setThreadNum(4);
}

DataNode::~DataNode() {}

void DataNode::start() {
    datanodeServer_->start();
    masterClient_->start();
    masterClient_->startHeartbeat();
}

int main(int argc, char *argv[]) {
    std::string loggerLevel;
    std::string configPath = "config.json";
    if (argc > 1) {
        loggerLevel = argv[1];
    }
    if (argc > 2) {
        configPath = argv[2];
    }

    if (loggerLevel == "-D") {
        Logger::setLogLevel(Logger::DEBUG);
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
    TokenManager::init(configPath);
    // 验证是否初始化成功
    if (!TokenManager::isInitialized()) {
        LOG_FATAL << "TokenManager 初始化失败";
        return -1;
    }

    EventLoop loop;

    // 3. 配置地址
    fileserver::net::InetAddress listenAddr("127.0.0.1", 9000); // DataNoded地址
    fileserver::net::InetAddress masterAddr("127.0.0.1", 8000); // Master 地址

    DataNode datanode(&loop, listenAddr, masterAddr);
    datanode.start();

    loop.loop();
    return 0;
}
