#pragma once
#include "HandlerUtils.h"
#include "NodeManager.h"
#include <memory>

class AdminHandler : public handlerUtils {
  public:
    bool handleStats(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                     std::shared_ptr<fn::HttpResponse> &resp);
    bool handleNodes(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                     std::shared_ptr<fn::HttpResponse> &resp);
    bool handleNodeCircuitBreak(const fn::TcpConnectionPtr &conn,
                                fn::HttpRequest &req,
                                std::shared_ptr<fn::HttpResponse> &resp);
    bool handleNodeBandwidthLimit(const fn::TcpConnectionPtr &conn,
                                  fn::HttpRequest &req,
                                  std::shared_ptr<fn::HttpResponse> &resp);
    bool handleGetPolicy(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                         std::shared_ptr<fn::HttpResponse> &resp);
    bool handleUpdatePolicy(const fn::TcpConnectionPtr &conn,
                            fn::HttpRequest &req,
                            std::shared_ptr<fn::HttpResponse> &resp);
    bool handleActiveUsers(const fn::TcpConnectionPtr &conn,
                           fn::HttpRequest &req,
                           std::shared_ptr<fn::HttpResponse> &resp);
    bool handleTrafficHistory(const fn::TcpConnectionPtr &conn,
                              fn::HttpRequest &req,
                              std::shared_ptr<fn::HttpResponse> &resp);

  private:
    int verifyAdminRequest(const fn::TcpConnectionPtr &conn,
                           fn::HttpRequest &req,
                           const std::shared_ptr<fn::HttpResponse> &resp);
};
