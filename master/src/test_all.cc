#include "MasterHttpHandler.h"
#include <iostream>

int main() {
    Logger::setLogLevel(Logger::DEBUG);
    EventLoop loop;
    HttpServer server(&loop, InetAddress(8000), "http-upload-test");

    // 创建HTTP处理器
    auto handler = std::make_shared<HttpUploadHandler>(4);

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
    return 0;
}