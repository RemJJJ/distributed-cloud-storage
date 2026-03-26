#pragma once
#include "NodeManager.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include "net/TcpConnection.h"

class DataNodeHandler {
  public:
    // 注册节点
    bool handleRegisterNode(const fn::TcpConnectionPtr &conn,
                            fn::HttpRequest &req,
                            std::shared_ptr<fn::HttpResponse> &resp);
    // 心跳
    bool handleHeartbeat(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                         std::shared_ptr<fn::HttpResponse> &resp);

    // 处理上传完成
    bool handleUploadFinishNotify(const fn::TcpConnectionPtr &conn,
                                  fn::HttpRequest &req,
                                  std::shared_ptr<fn::HttpResponse> &resp);

    // 处理上传文件信息
    bool handleReportFiles(const fn::TcpConnectionPtr &conn,
                           fn::HttpRequest &req,
                           std::shared_ptr<fn::HttpResponse> &resp);

  private:
    // 辅助函数
    static void sendError(std::shared_ptr<fn::HttpResponse> &resp,
                          const std::string &message,
                          fn::HttpResponse::HttpStatusCode code,
                          const fn::TcpConnectionPtr &conn);
};