#include "../../third_party/muduo/base/Logging.h"
#include "DataNodeServer.h"
#include <iostream>

int main() {
    fileserver::Logger::setLogLevel(fileserver::Logger::DEBUG);
    fn::EventLoop loop;
    DataNodeServer server(&loop, fileserver::net::InetAddress(8888),
                          "DataNode");
    server.start();
    LOG_INFO << "开始了";
    loop.loop();
    return 0;
}
