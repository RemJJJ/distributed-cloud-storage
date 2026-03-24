#include "UserHandler.h"
#include "PasswordHash.h"
#include "TokenManager.h"
#include "base/Logging.h"
#include "db/MySQLPool.h"
#include "db/MySQLStatement.h"
#include "net/Callbacks.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include <mysql/mysql.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============ 辅助函数 ============
void UserHandler::sendError(std::shared_ptr<fn::HttpResponse> &resp,
                            const std::string &message,
                            fn::HttpResponse::HttpStatusCode code,
                            const fn::TcpConnectionPtr &conn) {
    json response = {{"code", static_cast<int>(code)}, {"message", message}};
    resp->setStatusCode(code);
    resp->setStatusMessage(message);
    resp->setContentType("application/json");
    resp->addHeader("Connection", "close");
    resp->setBody(response.dump());

    if (conn) {
        conn->setWriteCompleteCallback(
            [conn](const fn::TcpConnectionPtr &connection) {
                connection->shutdown();
                return true;
            });
    }
}

// ============ 用户注册 ============
bool UserHandler::handleRegister(const fn::TcpConnectionPtr &conn,
                                 fn::HttpRequest &req,
                                 std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost || req.path() != "/register") {
        return false;
    }

    LOG_INFO << "Handing register request";

    try {
        json requestData = json::parse(req.body());

        std::string username = requestData.value("username", "");
        std::string password = requestData.value("password", "");
        std::string email = requestData.value("email", "");

        // 参数验证
        if (username.empty() || password.empty()) {
            sendError(resp, "用户名和密码不能为空",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        // 密码哈希
        std::string hashedPassword = PasswordHash::hash(password);
        if (hashedPassword.empty()) {
            sendError(resp, "密码加密失败",
                      fileserver::net::HttpResponse::k500InternalServerError,
                      conn);
            return true;
        }

        LOG_INFO << "Register attempt for username: " << username;

        // 检查用户名是否已存在
        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            sendError(resp, "数据库连接失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        // 作用域让checkStmt提前释放
        {
            std::string checkSql = "SELECT id FROM users WHERE username = ?";
            db::MySQLStatement checkStmt(*mysql, checkSql);

            if (checkStmt.hasError()) {
                sendError(resp, "数据库错误",
                          fn::HttpResponse::k500InternalServerError, conn);
                return true;
            }

            checkStmt.bindString(username);

            if (!checkStmt.execute()) {
                sendError(resp, "数据库错误",
                          fn::HttpResponse::k500InternalServerError, conn);
                return true;
            }

            auto resultSet = checkStmt.getResultSet();
            if (resultSet && resultSet->next()) {
                // next() 返回 true 说明有结果，用户名已存在
                sendError(resp, "用户名已存在",
                          fn::HttpResponse::k400BadRequest, conn);
                return true;
            }
        }

        // 插入新用户 (使用 MySQLStatement) ---
        std::string insertSql =
            "INSERT INTO users (username, password, email) VALUES (?, ?, ?)";
        db::MySQLStatement insertStmt(*mysql, insertSql);

        if (insertStmt.hasError()) {
            sendError(resp, "数据库错误",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        //
        insertStmt.bindString(username);
        insertStmt.bindString(hashedPassword);
        if (email.empty()) {
            insertStmt.bindNull();
        } else {
            insertStmt.bindString(email);
        }

        if (!insertStmt.execute()) {
            LOG_ERROR << "Insert failed: " << insertStmt.getError();
            sendError(resp, "注册失败，请稍后重试",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        // --- 7. 获取新用户 ID ---
        int newUserId = static_cast<int>(insertStmt.insertId());

        LOG_INFO << "User registered successfully: " << username
                 << " (id=" << newUserId << ")";

        // 返回响应
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        json response = {{"code", 0},
                         {"message", "注册成功"},
                         {"userId", newUserId},
                         {"username", username}};
        resp->setBody(response.dump());
        resp->addHeader("Content-Length",
                        std::to_string(response.dump().size()));

        return true;

    } catch (const json::parse_error &e) {
        LOG_ERROR << "JSON parse error: " << e.what();
        sendError(resp, "无效的请求格式", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR << "Register error: " << e.what();
        sendError(resp, "注册失败：" + std::string(e.what()),
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

// ============ 用户登录 (已重构) ============
bool UserHandler::handleLogin(const fn::TcpConnectionPtr &conn,
                              fn::HttpRequest &req,
                              std::shared_ptr<fn::HttpResponse> &resp) {

    if (req.method() != fn::HttpRequest::kPost || req.path() != "/login") {
        return false;
    }

    try {
        json requestData = json::parse(req.body());
        std::string username = requestData.value("username", "");
        std::string password = requestData.value("password", "");

        if (username.empty() || password.empty()) {
            sendError(resp, "用户名和密码不能为空",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        LOG_INFO << "Login attempt for username: " << username;

        // --- 4. 查询用户 (使用 MySQLStatement) ---
        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            sendError(resp, "数据库连接失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        std::string querySql =
            "SELECT id, username, password FROM users WHERE username = ?";
        db::MySQLStatement stmt(*mysql, querySql);

        if (stmt.hasError()) {
            sendError(resp, "数据库错误",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        stmt.bindString(username);

        if (!stmt.execute()) {
            sendError(resp, "数据库错误",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        auto rs = stmt.getResultSet();
        if (!rs->next()) {
            // 没有查到数据
            LOG_WARN << "User not found: " << username;
            sendError(resp, "用户名或密码错误",
                      fn::HttpResponse::k401Unauthorized, conn);
            return true;
        }

        // 直接通过 ResultSet 获取，不再需要 char 数组缓冲区
        int userId = rs->getInt(0);
        std::string dbUsername = rs->getString(1);
        std::string hashedPassword = rs->getString(2);

        // bcrypt 验证密码 ---
        if (!PasswordHash::verify(password, hashedPassword)) {
            LOG_WARN << "Password verification failed for: " << username;
            sendError(resp, "用户名或密码错误",
                      fn::HttpResponse::k401Unauthorized, conn);
            return true;
        }

        LOG_INFO << "Login successful for user: " << username
                 << " (id=" << userId << ")";

        // 生成 JWT Token ---
        auto &tm = TokenManager::instance();
        auto loginResult = tm.generateUserToken(userId, username);
        if (loginResult.token.empty()) {
            sendError(resp, "Token 生成失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        // --- 7. 更新最后登录时间 (使用 MySQLStatement) ---
        {
            std::string updateSql =
                "UPDATE users SET last_login_time = NOW() WHERE id = ?";
            db::MySQLStatement updateStmt(*mysql, updateSql);
            if (!updateStmt.hasError()) {
                updateStmt.bindInt(userId);
                updateStmt.execute(); // 忽略更新失败的错误，不影响登录主流程
            }
        }

        // --- 8. 返回响应 ---
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        json response = {{"code", 0},
                         {"message", "登录成功"},
                         {"token", loginResult.token},
                         {"tokenType", "Bearer"},
                         {"expiresIn", 86400},
                         {"userId", userId},
                         {"username", username}};
        resp->setBody(response.dump());
        resp->addHeader("Content-Length",
                        std::to_string(response.dump().size()));

        return true;

    } catch (const json::parse_error &e) {
        LOG_ERROR << "JSON parse error: " << e.what();
        sendError(resp, "无效的请求格式", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR << "Login error: " << e.what();
        sendError(resp, "登录失败：" + std::string(e.what()),
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool UserHandler::handleLogout(const fn::TcpConnectionPtr &conn,
                               fn::HttpRequest &req,
                               std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost || req.path() != "/logout") {
        return false;
    }

    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.find("Bearer ") != 0) {
        sendError(resp, "Missing Authorization header",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string token = authHeader.substr(7);
    auto &tm = TokenManager::instance();
    auto result = tm.verifyToken(token);

    if (!result.success) {
        sendError(resp, "Invalid token", fn::HttpResponse::k401Unauthorized,
                  conn);
        return true;
    }

    resp->setStatusCode(fn::HttpResponse::k200Ok);
    resp->setContentType("application/json");
    json response = {{"code", 0}, {"message", "登出成功"}};
    resp->setBody(response.dump());
    return true;
}