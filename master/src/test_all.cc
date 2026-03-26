#include "Config.h"
#include "MasterHttpHandler.h"
#include "NodeManager.h"
#include "base/Logging.h"
#include "db/MySQLPool.h"
#include <iostream>

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

    if (loggerLevel == "DEBUG" || loggerLevel == "-D") {
        Logger::setLogLevel(Logger::DEBUG);
    } else {
        Logger::setLogLevel(Logger::INFO);
    }
    // 加载配置文件

    LOG_INFO << "使用配置文件：" << configPath;

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

    // 初始化数据库连接池
    if (!db::MySQLPool::instance().initialize()) {
        LOG_ERROR << "数据库连接池初始化失败";
        return -1;
    }

    LOG_INFO << "数据库连接池启动成功";

    // 初始化Token管理器
    TokenManager::init(Config::instance().getString("jwt-secret"));
    // 验证是否初始化成功
    if (!TokenManager::isInitialized()) {
        LOG_FATAL << "TokenManager 初始化失败";
        return -1;
    }

    // 初始化节点管理器，同时初始化Token管理器
    NodeManager::init(configPath);
    // 验证是否初始化陈宫
    if (!NodeManager::isInitialized()) {
        LOG_FATAL << "NodeManager 初始化失败";
        return -1;
    }

    EventLoop loop;
    HttpServer server(&loop, InetAddress(8000), "http-upload-test");

    // 创建HTTP处理器
    auto handler = std::make_shared<HttpUploadHandler>(0);

    // 设置连接回调
    server.setConnectionCallback([handler](const TcpConnectionPtr &conn) {
        handler->onConnection(conn);
    });

    // 设置HTTP回调
    server.setHttpCallback([handler](const TcpConnectionPtr &conn,
                                     HttpRequest &req,
                                     std::shared_ptr<HttpResponse> &resp) {
        return handler->onRequest(conn, req, resp);
    });

    server.setThreadNum(4);
    NodeManager::instance().startTimeoutChecker(&loop, 5.0);
    server.start();
    std::cout << "HTTP upload server is running on port 8000..." << std::endl;
    std::cout << "Please visit http://localhost:8000" << std::endl;
    loop.loop();
    db::MySQLPool::instance().shutdown();
    return 0;
}