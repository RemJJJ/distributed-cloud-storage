#pragma once
#include "HandlerUtils.h"
#include "net/Callbacks.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include "net/TcpConnection.h"
#include <memory>

namespace fn = fileserver::net;

class UserHandler : public handlerUtils {
  public:
    UserHandler();

    // 用户注册
    bool handleRegister(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                        std::shared_ptr<fn::HttpResponse> &resp);

    // 用户登录
    bool handleLogin(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                     std::shared_ptr<fn::HttpResponse> &resp);

    // 用户登出(可选: Token黑名单)
    bool handleLogout(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                      std::shared_ptr<fn::HttpResponse> &resp);

    // 获取当前用户信息
    bool handleGetProfile(const fn::TcpConnectionPtr &conn,
                          fn::HttpRequest &req,
                          std::shared_ptr<fn::HttpResponse> &resp);

    // 搜索用户
    bool handleSearchUsers(const fn::TcpConnectionPtr &conn,
                           fn::HttpRequest &req,
                           std::shared_ptr<fn::HttpResponse> &resp);

    // 处理文件分享
    bool handleShareFile(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                         std::shared_ptr<fn::HttpResponse> &resp);

    // 取消文件分享
    bool handleCancelShare(const fn::TcpConnectionPtr &conn,
                           fn::HttpRequest &req,
                           std::shared_ptr<fn::HttpResponse> &resp);

    // 获取分享文件信息
    bool handleShareInfo(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                         std::shared_ptr<fn::HttpResponse> &resp);

    // 验证提取吗并获得下载地址
    bool handleShareVerify(const fn::TcpConnectionPtr &conn,
                           fn::HttpRequest &req,
                           std::shared_ptr<fn::HttpResponse> &resp);

    // 查询定向分享给我的文件
    bool handleListSharedWithMe(const fn::TcpConnectionPtr &conn,
                                fn::HttpRequest &req,
                                std::shared_ptr<fn::HttpResponse> &resp);

  private:
    void ensureServiceLevelColumn();
    std::string generateShareCode();
    std::string generateExtractCode();
};
