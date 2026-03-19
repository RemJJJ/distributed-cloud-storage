#pragma once
#include "base/Logging.h"
#include "net/EventLoop.h"
#include "net/HttpContext.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include "net/HttpServer.h"
#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

using namespace fileserver;
using namespace fileserver::net;
using json = nlohmann::json;

class BaseHandler : public std::enable_shared_from_this<BaseHandler> {
  protected:
    // 请求处理函数类型
    using RequestHandler =
        std::function<bool(const TcpConnectionPtr &, HttpRequest &,
                           std::shared_ptr<HttpResponse> &)>;

    // 【通用】路由模式结构
    struct RoutePattern {
        std::regex pattern;
        std::vector<std::string> params;
        RequestHandler handler;
        HttpRequest::Method method;

        RoutePattern(const std::string &pattern_str,
                     const std::vector<std::string> &param_names,
                     RequestHandler h, HttpRequest::Method m)
            : pattern(pattern_str), params(param_names), handler(h), method(m) {
        }
    };
    // 【通用】路由表
    std::vector<RoutePattern> routes_;

  public:
    BaseHandler() = default;
    virtual ~BaseHandler() = default;

    // 连接回调（子类可覆盖）
    virtual void onConnection(const TcpConnectionPtr &conn);

    // 【通用】核心请求处理（路由匹配，子类一般不覆盖）
    virtual bool onRequest(const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp);

  protected:
    // 【通用】注册路由
    void addRoute(const std::string &path, HttpRequest::Method method,
                  RequestHandler handler);

    // 子类必须实现：初始化自己的路由
    virtual void initRoutes() = 0;

    // 【通用】URL解码
    static std::string urlDecode(const std::string &encoded);

    // 【通用】正则转义
    static std::string escapeRegex(const std::string &str);

    // 【通用】统一错误响应
    static void sendError(const std::shared_ptr<HttpResponse> &resp,
                          const std::string &message,
                          HttpResponse::HttpStatusCode code,
                          const TcpConnectionPtr &conn);

    // 【通用】默认处理：404
    bool handleNotFound(const TcpConnectionPtr &conn,
                        std::shared_ptr<HttpResponse> &resp);
};