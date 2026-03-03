#pragma once
#include "../../third_party/muduo/net/Buffer.h"
#include "../../third_party/muduo/net/EventLoop.h"
#include "../../third_party/muduo/net/TcpClient.h"

namespace fn = fileserver::net;

class MasterClient {
  public:
    MasterClient(fn::EventLoop *loop, const fn::InetAddress &masterAddr,
                 const fn::InetAddress &myAddr);
    ~MasterClient();

    ///@brief 启动：发起对 Master 的连接
    void start();

    ///@brief 心跳定时器
    ///@c intervalSeconds 心跳间隔，推荐3-5秒
    void startHeartbeat(double intervalSeconds = 3.0);

  private:
    //------muduo网络回调------

    ///@brief 连接建立/断开时的回调
    void onConnection(const fn::TcpConnectionPtr &conn);

    ///@brief 收到Master响应时的回调
    void onMessage(const fn::TcpConnectionPtr &conn, fn::Buffer *buf,
                   fileserver::Timestamp receiveTime);

    //------业务逻辑函数------

    ///@brief 发送注册请求，onConnection建立连接之后
    void registerNode();

    ///@brief 发送心跳，定时器触发式
    void sendHeartbeat();

    ///@brief 发送HTTP POST请求
    ///@c path 请求路径
    ///@c body 请求体
    void post(const std::string &path, const std::string &body);

    //------成员变量------
    fn::EventLoop *loop_;                   // 绑定loop
    std::unique_ptr<fn::TcpClient> client_; // muduo TcpClient
    fn::TcpConnectionPtr conn_;             // 暂定，单loop下持有强引用安全

    fn::InetAddress masterAddr_;
    fn::InetAddress myAddr_;
};