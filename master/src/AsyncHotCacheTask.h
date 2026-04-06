#pragma once

#include "base/Timestamp.h"
#include "net/Buffer.h"
#include "net/Callbacks.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/TcpClient.h"
#include "net/TcpConnection.h"
#include "net/TimerId.h"
#include <memory>
#include <string>

// 异步热点数据缓存下发
class AsyncHotCacheTask
    : public std::enable_shared_from_this<AsyncHotCacheTask> {
  public:
    AsyncHotCacheTask(fileserver::net::EventLoop *loop,
                      const fileserver::net::InetAddress &dnAddr,
                      const std::string &hotCacheToken,
                      const std::string &serverFilename);

    ~AsyncHotCacheTask();
    void start();

  private:
    void onConnection(const fileserver::net::TcpConnectionPtr &conn);
    void onMessage(const fileserver::net::TcpConnectionPtr &conn,
                   fileserver::net::Buffer *buf, fileserver::Timestamp time);
    void onTimeout();

    fileserver::net::TcpClient client_;
    std::string hotCacheToken_; 
    std::string serverFilename_;
    fileserver::net::TimerId timeoutTimerId_;
};
