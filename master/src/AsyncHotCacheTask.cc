#include "AsyncHotCacheTask.h"
#include "base/Logging.h"

namespace fn = fileserver::net;

AsyncHotCacheTask::AsyncHotCacheTask(fn::EventLoop *loop,
                                     const fn::InetAddress &dnAddr,
                                     const std::string &hotCacheToken,
                                     const std::string &serverFilename)
    : client_(loop, dnAddr, "AsyncHotCacheTask_" + serverFilename),
      hotCacheToken_(hotCacheToken), serverFilename_(serverFilename) {}

AsyncHotCacheTask::~AsyncHotCacheTask() {
    LOG_DEBUG << "AsyncHotCacheTask destroy, server_filename="
              << serverFilename_;
}

void AsyncHotCacheTask::start() {
    client_.setConnectionCallback(std::bind(&AsyncHotCacheTask::onConnection,
                                            shared_from_this(),
                                            std::placeholders::_1));
    client_.setMessageCallback(std::bind(&AsyncHotCacheTask::onMessage,
                                         shared_from_this(),
                                         std::placeholders::_1,
                                         std::placeholders::_2,
                                         std::placeholders::_3));
    client_.connect();
    timeoutTimerId_ = client_.getLoop()->runAfter(
        5.0, std::bind(&AsyncHotCacheTask::onTimeout, shared_from_this()));
}

void AsyncHotCacheTask::onTimeout() {
    LOG_WARN << "Async hot cache task timeout, server_filename="
             << serverFilename_;
    client_.disconnect();
}

void AsyncHotCacheTask::onConnection(const fn::TcpConnectionPtr &conn) {
    if (conn->connected()) {
        std::string request = "POST /api/datanode/hot_cache HTTP/1.1\r\n"
                              "Host: " +
                              conn->peerAddress().toIp() +
                              "\r\n"
                              "Authorization: Bearer " +
                              hotCacheToken_ +
                              "\r\n"
                              "Content-Length: 0\r\n"
                              "Connection: close\r\n\r\n";
        conn->send(request);
        return;
    }

    client_.getLoop()->cancel(timeoutTimerId_);
}

void AsyncHotCacheTask::onMessage(const fn::TcpConnectionPtr &conn,
                                  fn::Buffer *buf, fileserver::Timestamp time) {
    static_cast<void>(conn);
    static_cast<void>(time);
    client_.getLoop()->cancel(timeoutTimerId_);
    const std::string response = buf->retrieveAllAsString();
    if (response.find("HTTP/1.1 200") != std::string::npos) {
        LOG_INFO << "Hot cache task accepted by DataNode, server_filename="
                 << serverFilename_;
    } else {
        LOG_WARN << "Hot cache task failed, server_filename="
                 << serverFilename_
                 << ", response=" << response.substr(0, 120);
    }
    client_.disconnect();
}
