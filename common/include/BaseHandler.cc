#include "BaseHandler.h"

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

std::string BaseHandler::urlDecode(const std::string &encoded) {
    std::string result;
    char ch;
    size_t i;
    int ii;
    size_t len = encoded.length();

    for (i = 0; i < len; i++) {
        if (encoded[i] != '%') {
            if (encoded[i] == '+') {
                result += ' ';
            } else {
                result += encoded[i];
            }
        } else {
            sscanf(encoded.substr(i + 1, 2).c_str(), "%x", &ii);
            ch = static_cast<char>(ii);
            result += ch;
            i = i + 2;
        }
    }
    return result;
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

void BaseHandler::sendError(const std::shared_ptr<HttpResponse> &resp,
                            const std::string &message,
                            HttpResponse::HttpStatusCode code,
                            const TcpConnectionPtr &conn) {
    json response = {{"code", static_cast<int>(code)}, {"message", message}};
    resp->setStatusCode(code);
    resp->setStatusMessage(message);
    resp->setContentType("application/json");
    resp->addHeader("Connection", "close");
    resp->setBody(response.dump());

    if (conn) {
        conn->setWriteCompleteCallback(
            [conn](const TcpConnectionPtr &connection) {
                connection->shutdown();
                return true;
            });
    }
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