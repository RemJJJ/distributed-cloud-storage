#include "DataNodeServer.h"
#include "MasterClient.h"
#include "base/Logging.h"
#include <iostream>

int main() {
    fileserver::Logger::setLogLevel(fileserver::Logger::DEBUG);
    fn::EventLoop loop;
    DataNodeServer server(&loop, fileserver::net::InetAddress(8888),
                          "DataNode");
    MasterClient client(&loop, fileserver::net::InetAddress("127.0.0.1", 8000),
                        fileserver::net::InetAddress("127.0.0.1", 8888));
    server.start();
    client.start();
    client.startHeartbeat();
    loop.loop();
    return 0;
}
