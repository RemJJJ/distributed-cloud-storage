#pragma once
#include "net/Callbacks.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include "net/TcpConnection.h"
#include <memory>

namespace fn = fileserver::net;

class UserHandler {
  public:
    // 用户注册
    bool handleRegister(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                        std::shared_ptr<fn::HttpResponse> &resp);

    // 用户登录
    bool handleLogin(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                     std::shared_ptr<fn::HttpResponse> &resp);

    // 用户登出(可选: Token黑名单)
    // static bool handleLogout(const fn::TcpConnectionPtr& conn,
    // fn::HttpRequest& req,
    // std::shared_ptr<fn::HttpResponse>& resp);
    bool handleLogout(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                      std::shared_ptr<fn::HttpResponse> &resp);

    // 获取当前用户信息
    bool handleGetProfile(const fn::TcpConnectionPtr &conn,
                          fn::HttpRequest &req,
                          std::shared_ptr<fn::HttpResponse> &resp);

  private:
    // 辅助函数
    static void sendError(std::shared_ptr<fn::HttpResponse> &resp,
                          const std::string &message,
                          fn::HttpResponse::HttpStatusCode code,
                          const fn::TcpConnectionPtr &conn);
};