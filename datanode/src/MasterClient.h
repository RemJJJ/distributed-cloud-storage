#pragma once
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/TcpClient.h"
#include <queue>

namespace fn = fileserver::net;

/// @brief 与 Master通信的客户端
class MasterClient {
  public:
    MasterClient(fn::EventLoop *loop, const fn::InetAddress &masterAddr,
                 const fn::InetAddress &myAddr);
    ~MasterClient();

    /// @brief 启动：发起对 Master 的连接
    void start();

    /// @brief 心跳定时器
    /// @c intervalSeconds 心跳间隔，推荐3-5秒
    void startHeartbeat(double intervalSeconds = 3.0);

    // ---------------- 新增：通知上传完成的接口 ----------------
    /// @brief 线程安全地通知 Master：文件上传完成
    /// @param file_id 文件ID
    /// @param server_filename 服务器存储路径
    /// @param stored_size 存储大小
    void notifyUploadFinish(const std::string &file_id,
                            const std::string &server_filename,
                            size_t stored_size);

  private:
    //------muduo网络回调------

    /// @brief 连接建立/断开时的回调
    void onConnection(const fn::TcpConnectionPtr &conn);

    /// @brief 收到Master响应时的回调
    void onMessage(const fn::TcpConnectionPtr &conn, fn::Buffer *buf,
                   fileserver::Timestamp receiveTime);

    //------业务逻辑函数------

    /// @brief 发送注册请求，onConnection建立连接之后
    void registerNode();

    /// @brief 发送心跳，定时器触发式
    void sendHeartbeat();

    /// @brief 发送HTTP POST请求
    /// @c path 请求路径
    /// @c body 请求体
    void post(const std::string &path, const std::string &body);

    ///@brief 在EventLoop线程中实际执行通知的函数
    void doNotifyUploadFinish(const std::string &file_id,
                              const std::string &server_filename,
                              size_t stored_size);

    /// @brief 处理待发送的通知队列
    void procPendingNotice();

    //------成员变量------
    fn::EventLoop *loop_;                   // 绑定loop
    std::unique_ptr<fn::TcpClient> client_; // muduo TcpClient
    fn::TcpConnectionPtr conn_;             // 暂定，单loop下持有强引用安全

    fn::InetAddress masterAddr_;
    fn::InetAddress myAddr_;

    struct PendingNotice {
        std::string file_id;
        std::string server_filename;
        size_t stored_size;
    };
    std::queue<PendingNotice> pendingNotifications_;
    std::mutex pendingMutex_; // 保护队列的互斥锁
};