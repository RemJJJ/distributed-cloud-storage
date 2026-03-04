#pragma once
#include "../../common/include/FileStorage.h"
#include "../../third_party/muduo/net/Buffer.h"
#include "../../third_party/muduo/net/EventLoop.h"
#include "../../third_party/muduo/net/TcpClient.h"
#include <string>

namespace fn = fileserver::net;

class RemoteFileStorage
    : public FileStorage,
      public std::enable_shared_from_this<RemoteFileStorage> {
  public:
    RemoteFileStorage(const fn::InetAddress &addr, fn::EventLoop *loop);

    ~RemoteFileStorage() override;

    bool open(const std::string &filename) override;

    bool write(const char *data, size_t len) override;

    bool close() override;

    bool isConnected() const;

    uintmax_t totalBytes() const override;

    bool isOpen() const override;

    const std::string &filename() const override;

    fn::InetAddress getAddr() const { return addr_; }

    void setCloseCmdSentCallback(
        const std::function<void(bool success)> &cb) override {
        closeCmdSentCallback_ = cb;
    }

  private:
    void onConnection(const fn::TcpConnectionPtr &conn);
    std::function<void(bool success)> closeCmdSentCallback_;
    fn::InetAddress addr_;
    std::string filename_;
    std::unique_ptr<fn::TcpClient> client_;
    fn::Buffer sendBuffer_;
    uintmax_t totalBytes_;
    bool closeRequested_ = false; // 是否已请求关闭
};