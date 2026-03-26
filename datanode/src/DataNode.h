#pragma once
#include "base/Logging.h"
#include "net/HttpServer.h"
#include "net/InetAddress.h"
#include <iostream>

class DataNodeHttpHandler;
class MasterClient;
namespace fn = fileserver::net;

class DataNode {
  public:
    DataNode(fn::EventLoop *loop, const fn::InetAddress &listenAddr,
             const fn::InetAddress &masterAddr);

    ~DataNode();

    /// @brief 启动所有服务
    void start();

    //-----------给子组件提供接口------------
    /// @brief 获取MasterClient
    MasterClient *getMasterClient() { return masterClient_.get(); }

    // 原子计数器，记录当前正在处理的上传请求数
    std::atomic<int> activeUploads_{0};

    void incActiveUpload() { activeUploads_++; }
    void decActiveUpload() { activeUploads_--; }
    int getActiveUploads() const { return activeUploads_.load(); }

  private:
    fn::EventLoop *loop_;
    std::unique_ptr<MasterClient> masterClient_;     // 持有MasterClient
    std::unique_ptr<fn::HttpServer> datanodeServer_; // 持有DataNodeServer
    std::shared_ptr<DataNodeHttpHandler> handler_;   // 业务处理器
};