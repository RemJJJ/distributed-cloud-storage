#pragma once

#include "../base/noncopyable.h"
#include "Channel.h"
#include "EventLoop.h"
#include "Socket.h"
#include <functional>

namespace fileserver {
namespace net {
class EventLoop;
class InetAddress;

/// @brief Acceptor用于接受TCP新链接
/// 主要功能：
/// 1. 创建监听socket
/// 2. 绑定地址和端口
/// 3. 开始监听
/// 4. 接受新链接并通过回调函数通知上层
///
/// 使用非阻塞socket + channel实现
/// 支持优雅关闭和防止文件描述符耗尽
class Acceptor : noncopyable {
  public:
    /// @brief 新链接回调函数类型
    /// @param sockfd 新链接的文件描述符
    /// @param InetAddress 对端地址
    using NewConnectionCallback =
        std::function<void(int sockfd, const InetAddress &)>;

    /// @brief 构造函数
    /// @param loop 事件循环
    /// @param listenAddr 监听地址
    /// @param reuseport 是否启用端口复用
    Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport);
    ~Acceptor();

    /// @brief 设置新链接回调函数
    /// 当有新连接到达时会调用此函数
    void setNewConnectionCallback(const NewConnectionCallback &cb) {
        newConnectionCallback_ = cb;
    }

    /// @brief 是否正在监听
    bool isListening() const { return listening_; }

    /// @brief 开始监听
    /// 此函数必须设置回调函数之后调用
    void listen();

  private:
    /// @brief 处理新链接到达
    /// 由Channel回调
    void handleRead();

    EventLoop *loop_;                             // Acceptor所属的事件循环
    Socket acceptSocket_;                         // 监听socket
    Channel acceptChannel_;                       // 监听Channel
    NewConnectionCallback newConnectionCallback_; // 新链接回调函数
    bool listening_;                              // 是否正在监听
    int idleFd_; // 空闲的文件描述符，用于防止文件描述符耗尽
};
} // namespace net
} // namespace fileserver