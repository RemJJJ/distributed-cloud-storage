#pragma once
#include "FileStorage.h"
#include "net/EventLoop.h"
#include "net/TcpServer.h"
#include <map>

namespace fn = fileserver::net;

// 连接上下文
class ConnectionContext {
  public:
    enum class State { WAIT_COMMAND, WAIT_DATA };

    ConnectionContext();

    ConnectionContext(const std::string &filename,
                      std::unique_ptr<FileStorage> storage);

    ~ConnectionContext();

    bool parseRequest(fn::Buffer *buf, fileserver::Timestamp time);

    void writeData(const char *data, size_t len);

    void setState(State state) { state_ = state; }

    bool hasMoreDataToProcess() const;

  private:
    std::string filename_;                 // 服务器文件名
    std::unique_ptr<FileStorage> storage_; // 文件存储
    State state_;                          // 当前状态
    uintmax_t contentLength_;              // DATA剩余待读字节数
};

/// @brief 数据节点服务器
class DataNodeServer {
  public:
    // 构造：初始化TcpServer，设置回调
    DataNodeServer(fn::EventLoop *loop, const fn::InetAddress &listenAddr,
                   const std::string &name);

    // 启动服务
    void start();

  private:
    // 连接回调
    void onConnection(const fn::TcpConnectionPtr &conn);

    // 消息回调
    void onMessage(const fn::TcpConnectionPtr &conn, fn::Buffer *buf,
                   fileserver::Timestamp time);

    fn::TcpServer server_;
};