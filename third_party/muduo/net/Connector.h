#pragma once

#include <functional>
#include <memory>

#include "../base/noncopyable.h"
#include "Channel.h"
#include "InetAddress.h"
#include "TimerId.h"

namespace fileserver {
namespace net {
class Channel;
class EventLoop;

/// @brief Connector用于客户端发起TCP连接
/// 使用非阻塞连接 + Channel实现
/// 支持自动重连和连接超时
class Connector : noncopyable, public std::enable_shared_from_this<Connector> {
  public:
    /// @brief 新连接回调函数类型
    /// @param sockfd 成功建立连接的socket文件描述符
    using NewConnectionCallback = std::function<void(int sockfd)>;

    Connector(EventLoop *loop, const InetAddress &serverAddr);
    ~Connector();

    void setNewConnectionCallback(const NewConnectionCallback &cb) {
        newConnectionCallback_ = cb;
    }

    /// @brief 开始连接
    void start(); // 在任意线程调用

    /// @brief 重新连接
    void restart(); // 必须在loop线程中调用

    /// @brief 停止连接
    void stop(); // 可以在任意线程调用

    const InetAddress &serverAddress() const { return serverAddr_; }

  private:
    /// 连接状态
    enum States { kDisconnected, kConnecting, KConnected };
    static const int kMaxRetryDelayMs = 30 * 1000; // 最大重试延迟(30秒)
    static const int kInitRetryDelayMs = 500;      // 初始重试延迟(0.5秒)

    void setState(States s) { state_ = s; }

    // 在loop线程中启动连接
    void startInLoop();

    // 停止链接
    void stopInLoop();

    // 连接
    void connect();

    // 正在连接
    void connecting(int sockfd);

    // 处理写事件
    void handleWrite();

    // 处理错误
    void handleError();

    // 重试
    void retry(int sockfd);

    // 移除并重置Channel
    int removeAndResetChannel();

    // 重置Channel
    void resetChannel();

    EventLoop *loop_;
    InetAddress serverAddr_;                      // 服务器地址
    bool connect_;                                // 是否连接
    States state_;                                // 连接状态
    std::unique_ptr<Channel> channel_;            // 用于观察socket上的事件
    NewConnectionCallback newConnectionCallback_; // 新连接回调函数
    int retryDelatMs_;                            // 重试延迟
    TimerId timerId_;                             // 用于重试的定时器
};

using ConnectorPtr = std::shared_ptr<Connector>;

} // namespace net
} // namespace fileserver