#include "BaseHandler.h"

BaseHandler::BaseHandler() {}

BaseHandler::~BaseHandler() {}

void BaseHandler::onConnection(const TcpConnectionPtr &conn) {
    // 默认实现，子类可覆盖
    LOG_INFO << "Connection established";
}

void BaseHandler::initRoutes() {
    // 默认实现，子类覆盖
    LOG_INFO << "BaseHandler::initRoutes";
}

bool BaseHandler::onRequest(const TcpConnectionPtr &conn, HttpRequest &req,
                            std::shared_ptr<HttpResponse> &resp) {
    std::string path = req.path();
    LOG_INFO << "Headers: " << req.methodString() << " " << path;
    LOG_INFO << "Content-Type: " << req.getHeader("Content-Type");
    LOG_INFO << "Body size: " << req.body().size();

    try {
        // 查找匹配路由
        for (const auto &route : routes_) {
            if (route.method != req.method()) {
                LOG_INFO << "Method mismatch: expected " << route.method
                         << ", got " << req.method();
                continue;
            }

            std::smatch matches;
            if (std::regex_match(path, matches, route.pattern)) {
                LOG_INFO << "Found matching route: " << path;
                // 提取路径参数
                std::unordered_map<std::string, std::string> params;
                for (size_t i = 0;
                     i < route.params.size() && i + 1 < matches.size(); i++) {
                    params[route.params[i]] = matches[i + 1];
                }

                // 将路径参数存储到请求对象中
                req.setPathParams(params);

                // 调用处理函数
                return route.handler(conn, req, resp);
            }
        }

        // 未找到匹配路由，返回404
        LOG_WARN << "No matching route found for " << path;
        return handleNotFound(conn, resp);
    } catch (const std::exception &e) {
        LOG_ERROR << "Error processing request: " << e.what();
        sendError(resp, "Internal Server Error",
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

void BaseHandler::addRoute(const std::string &path, HttpRequest::Method method,
                           RequestHandler handler) {
    std::string pattern = "^" + escapeRegex(path) + "$";
    routes_.emplace_back(pattern, std::vector<std::string>(), handler, method);
}

// 带参数的路由
void BaseHandler::addRoute(const std::string &pattern,
                           HttpRequest::Method method, RequestHandler handler,
                           const std::vector<std::string> &paramNames) {
    routes_.emplace_back(pattern, paramNames, handler, method);
}

std::string BaseHandler::escapeRegex(const std::string &str) {
    std::string result;
    for (char c : str) {
        if (c == '.' || c == '+' || c == '*' || c == '?' || c == '^' ||
            c == '$' || c == '(' || c == ')' || c == '[' || c == ']' ||
            c == '{' || c == '}' || c == '|' || c == '\\') {
            result += '\\';
        }
        result += c;
    }
    return result;
}

bool BaseHandler::handleNotFound(const TcpConnectionPtr &conn,
                                 std::shared_ptr<HttpResponse> &resp) {
    json response = {{"code", 404}, {"message", "Not Found"}};
    resp->setStatusCode(HttpResponse::k404NotFound);
    resp->setContentType("application/json");
    resp->addHeader("Connection", "close");
    resp->setBody(response.dump());

    conn->setWriteCompleteCallback([conn](const TcpConnectionPtr &connection) {
        connection->shutdown();
        return true;
    });
    return true;
}