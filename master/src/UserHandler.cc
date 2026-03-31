#include "UserHandler.h"
#include "NodeManager.h"
#include "PasswordHash.h"
#include "base/Logging.h"
#include "db/MySQLPool.h"
#include "db/MySQLStatement.h"
#include "net/Callbacks.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include <mysql/mysql.h>
#include <nlohmann/json.hpp>
#include <mutex>
#include <random>
#include <string>

using json = nlohmann::json;

namespace {
std::string normalizeServiceLevel(const std::string &serviceLevel) {
    return serviceLevel == "vip" ? "vip" : "normal";
}
} // namespace

UserHandler::UserHandler() { ensureServiceLevelColumn(); }

void UserHandler::ensureServiceLevelColumn() {
    static std::once_flag onceFlag;
    std::call_once(onceFlag, []() {
        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            LOG_WARN << "Skip ensuring users.service_level because DB "
                        "connection is unavailable";
            return;
        }

        std::string checkSql =
            "SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS "
            "WHERE TABLE_SCHEMA = DATABASE() "
            "AND TABLE_NAME = 'users' "
            "AND COLUMN_NAME = 'service_level' LIMIT 1";
        db::MySQLStatement checkStmt(*mysql, checkSql);
        if (!checkStmt.execute()) {
            LOG_WARN << "Failed to inspect users.service_level: "
                     << checkStmt.getError();
            return;
        }

        auto rs = checkStmt.getResultSet();
        if (rs && rs->next()) {
            return;
        }

        std::string alterSql =
            "ALTER TABLE users ADD COLUMN service_level VARCHAR(16) NOT NULL "
            "DEFAULT 'normal'";
        db::MySQLStatement alterStmt(*mysql, alterSql);
        if (!alterStmt.execute()) {
            LOG_WARN << "Failed to add users.service_level: "
                     << alterStmt.getError();
            return;
        }

        LOG_INFO << "Added users.service_level column with default normal";
    });
}

std::string UserHandler::generateShareCode() {
    const std::string charset =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, charset.length() - 1);

    std::string code;
    for (int i = 0; i < 6; ++i) {
        code += charset[dis(gen)];
    }
    return code;
}

std::string UserHandler::generateExtractCode() {
    const std::string charset = "abcdefghjkmnpqrstuvwxyz23456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, charset.length() - 1);

    std::string code;
    for (int i = 0; i < 4; ++i) {
        code += charset[dis(gen)];
    }
    return code;
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
            "SELECT id, username, password, service_level "
            "FROM users WHERE username = ?";
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
        std::string serviceLevel = normalizeServiceLevel(rs->getString(3));

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
        auto loginResult =
            tm.generateUserToken(userId, username, serviceLevel);
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
                         {"username", username},
                         {"serviceLevel", serviceLevel}};
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
        sendError(resp, "404 not found",
                  fileserver::net::HttpResponse::k404NotFound, conn);
        return true;
    }

    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.find("Bearer ") != 0) {
        sendError(resp, "Missing Authorization header",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string token = authHeader.substr(7);
    auto &tm = TokenManager::instance();
    auto userId = tm.verifyUserToken(token);

    if (userId < 0) {
        sendError(resp, "Invalid token", fn::HttpResponse::k401Unauthorized,
                  conn);
        return true;
    }

    resp->setStatusCode(fn::HttpResponse::k200Ok);
    resp->setContentType("application/json");
    json response = {{"code", 0}, {"message", "登出成功"}};
    std::string bodyStr = response.dump();
    resp->setBody(bodyStr);
    resp->addHeader("Content-Length", std::to_string(bodyStr.size()));
    return true;
}

// 搜索用户
bool UserHandler::handleSearchUsers(const fn::TcpConnectionPtr &conn,
                                    fn::HttpRequest &req,
                                    std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kGet ||
        req.path() != "/users/search") {
        sendError(resp, "404 not found",
                  fileserver::net::HttpResponse::k404NotFound, conn);
        return true;
    }

    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.find("Bearer ") != 0) {
        sendError(resp, "Missing Authorization header",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string token = authHeader.substr(7);
    auto userId = TokenManager::instance().verifyUserToken(token);

    if (userId < 0) {
        sendError(resp, "Invalid token", fn::HttpResponse::k401Unauthorized,
                  conn);
        return true;
    }

    std::string keyword = req.getQuery("keyword");
    if (keyword.empty()) {
        sendError(resp, "搜索关键词不能为空", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    }

    // 解码中文
    keyword = urlDecode(keyword);
    LOG_INFO << "Search keyword: " << keyword;

    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        sendError(resp, "数据库连接失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    std::string querySql = "SELECT id, username, email FROM users WHERE "
                           "username LIKE ? AND id != ? LIMIT 10";
    db::MySQLStatement stmt(*mysql, querySql);
    stmt.bindString("%" + keyword + "%");
    stmt.bindInt(userId);

    if (!stmt.execute()) {
        LOG_ERROR << "SELECT users failed";
        sendError(resp, "Exec select failed",
                  fileserver::net::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    json response;
    response["code"] = 0;
    response["message"] = "Success";
    json users = json::array();

    auto rs = stmt.getResultSet();
    while (rs->next()) {
        users.push_back({{"id", rs->getInt(0)},
                         {"username", rs->getString(1)},
                         {"email", rs->getString(2)}});
    }

    response["users"] = users;
    resp->setStatusCode(fn::HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->setContentType("application/json");
    resp->addHeader("Connection", "close");
    std::string bodyStr = response.dump();
    resp->setBody(bodyStr);
    resp->addHeader("Content-Length", std::to_string(bodyStr.size()));
    conn->setWriteCompleteCallback([](const fn::TcpConnectionPtr &connection) {
        connection->shutdown();
        return true;
        ;
    });

    return true;
}

bool UserHandler::handleShareFile(const fn::TcpConnectionPtr &conn,
                                  fn::HttpRequest &req,
                                  std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost || req.path() != "/share") {
        sendError(resp, "404 not found",
                  fileserver::net::HttpResponse::k404NotFound, conn);
        return true;
    }

    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.find("Bearer ") != 0) {
        sendError(resp, "Missing Authorization header",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string token = authHeader.substr(7);
    auto userId = TokenManager::instance().verifyUserToken(token);

    if (userId < 0) {
        sendError(resp, "Invalid token", fn::HttpResponse::k401Unauthorized,
                  conn);
        return true;
    }

    try {
        json requestData = json::parse(req.body());
        int fileId = requestData.value("fileId", 0);
        std::string shareType = requestData.value("shareType", "public");

        if (fileId == 0 || (shareType != "public" && shareType != "protected" &&
                            shareType != "specific")) {
            sendError(resp, "参数错误", fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            sendError(resp, "数据库连接失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        // 验证文件所有权
        std::string fileQuery =
            "SELECT 1 FROM files WHERE id = ? AND user_id = ?";
        db::MySQLStatement stmt1(*mysql, fileQuery);
        stmt1.bindInt(fileId);
        stmt1.bindInt(userId);

        if (!stmt1.execute() || !stmt1.getResultSet()->next()) {
            sendError(resp, "您没有权限分享此文件或文件不存在",
                      fileserver::net::HttpResponse::k500InternalServerError,
                      conn);
            return true;
        }

        // 处理到期时间
        std::string expireStr = "";
        int expireHours = requestData.value("expireTime", 0);
        if (expireHours > 0) {
            auto now = std::chrono::system_clock::now();
            auto expireTime = now + std::chrono::hours(expireHours);
            std::time_t expire_c =
                std::chrono::system_clock::to_time_t(expireTime);

            std::stringstream ss;
            ss << std::put_time(std::localtime(&expire_c), "%Y-%m-%d %H:%M:%S");
            expireStr = ss.str(); // 得到类似 "2023-12-31 23:59:59" 的字符串
        }

        // 处理分享类型
        std::string extractCode = "";
        int targetUserId = 0;

        if (shareType == "specific") {
            targetUserId = requestData.value("sharedWithId", 0);
            if (targetUserId == 0) {
                sendError(resp, "必须指定目标用户ID",
                          fn::HttpResponse::k400BadRequest, conn);
                return true;
            }
            // 检查是否已经分享给该用户
            std::string checkQuery =
                "SELECT 1 FROM file_shares WHERE file_id = ? AND "
                "target_user_id = ? AND share_type = 'specific'";
            db::MySQLStatement stmt2(*mysql, checkQuery);
            stmt2.bindInt(fileId);
            stmt2.bindInt(targetUserId);
            if (stmt2.execute() && stmt2.getResultSet()->next()) {
                sendError(resp, "已经分享给该用户，请勿重复分享",
                          fn::HttpResponse::k400BadRequest, conn);
                return true;
            }

        } else if (shareType == "public" || shareType == "protected") {
            std::string existQuery =
                "SELECT share_id, extract_code FROM file_shares "
                "WHERE file_id = ? AND user_id = ? AND share_type = ? "
                "AND (expire_time IS NULL OR expire_time > NOW()) LIMIT 1";
            db::MySQLStatement existStmt(*mysql, existQuery);
            existStmt.bindInt(fileId);
            existStmt.bindInt(userId);
            existStmt.bindString(shareType);
            if (existStmt.execute()) {
                auto rs = existStmt.getResultSet();
                if (rs && rs->next()) {
                    // 命中：直接返回已有的信息
                    std::string oldShareId = rs->getString(0);
                    std::string oldExtractCode = rs->getString(1);

                    json response = {{"code", 0},
                                     {"message", "获取已有分享信息"},
                                     {"data",
                                      {{"shareId", oldShareId},
                                       {"shareType", shareType},
                                       {"shareLink", "/s/" + oldShareId}}}};
                    if (shareType == "protected" && !oldExtractCode.empty()) {
                        response["data"]["extractCode"] = oldExtractCode;
                    }
                    resp->setStatusCode(fn::HttpResponse::k200Ok);
                    resp->setStatusMessage("OK");
                    resp->setContentType("application/json");
                    resp->addHeader("Connection", "close");
                    std::string bodyStr = response.dump();
                    resp->setBody(bodyStr);
                    resp->addHeader("Content-Length",
                                    std::to_string(bodyStr.size()));
                    return true;
                }
            }

            if (shareType == "protected") {
                extractCode = generateExtractCode();
            }
        }

        std::string shareCode = generateShareCode();

        // 创建分享记录
        std::string insertQuery =
            "INSERT INTO file_shares(share_id, file_id, user_id, share_type, "
            "extract_code, target_user_id, expire_time, created_at) VALUES (?, "
            "?, ?, ?, ?, ?, ?, NOW())";
        db::MySQLStatement stmt3(*mysql, insertQuery);
        stmt3.bindString(shareCode);
        stmt3.bindInt(fileId);
        stmt3.bindInt(userId);
        stmt3.bindString(shareType);

        // extract_code
        if (extractCode.empty())
            stmt3.bindNull();
        else
            stmt3.bindString(extractCode);

        // target_user_id
        if (targetUserId == 0)
            stmt3.bindNull();
        else
            stmt3.bindInt(targetUserId);

        // expire_time
        if (expireStr.empty())
            stmt3.bindNull();
        else
            stmt3.bindString(expireStr);

        if (!stmt3.execute()) {
            LOG_ERROR << "Insert share failed: " << stmt3.getError();
            sendError(resp, "创建分享失败",
                      fileserver::net::HttpResponse::k500InternalServerError,
                      conn);
            return true;
        }

        json response = {
            {"code", 0},
            {"message", "分享成功"},
            {"data", {{"shareId", shareCode}, {"shareType", shareType}}}};

        if (shareType == "specific") {
            response["data"]["sharedWithId"] = targetUserId;
        } else if (shareType == "protected" || shareType == "public") {
            // public 和 protected 都有链接
            response["data"]["shareLink"] =
                "/share/" + shareCode; // 建议加上完整域名
            if (shareType == "protected") {
                response["data"]["extractCode"] = extractCode;
            }
        }
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("application/json");
        resp->addHeader("Connection", "close");
        std::string bodyStr = response.dump();
        resp->setBody(bodyStr);
        resp->addHeader("Content-Length", std::to_string(bodyStr.size()));

        conn->setWriteCompleteCallback(
            [](const fn::TcpConnectionPtr &connection) {
                connection->shutdown();
                return true;
            });

        return true;
    } catch (const std::exception &e) {
        LOG_ERROR << "分享文件错误: " << e.what();
        sendError(resp, "分享失败: " + std::string(e.what()),
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool UserHandler::handleCancelShare(const fn::TcpConnectionPtr &conn,
                                    fn::HttpRequest &req,
                                    std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost || req.path() != "/share/cancel")
        return true;

    std::string token = req.getHeader("Authorization").substr(7);
    int userId = TokenManager::instance().verifyUserToken(token);
    if (userId < 0) {
        sendError(resp, "未登录", fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    try {
        json reqData = json::parse(req.body());
        int fileId = reqData.value("fileId", 0);

        auto mysql = db::MySQLPool::instance().getConnection();
        // 只能删除自己分享的记录
        std::string sql =
            "DELETE FROM file_shares WHERE file_id = ? AND user_id = ?";
        db::MySQLStatement stmt(*mysql, sql);
        stmt.bindInt(fileId);
        stmt.bindInt(userId);

        if (!stmt.execute()) {
            sendError(resp, "数据库错误",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        json response = {{"code", 0}, {"message", "取消分享成功"}};
        std::string bodyStr = response.dump();
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(bodyStr);
        resp->addHeader("Content-Length", std::to_string(bodyStr.size()));
        return true;
    } catch (...) {
        sendError(resp, "参数错误", fn::HttpResponse::k400BadRequest, conn);
        return true;
    }
}

bool UserHandler::handleShareInfo(const fn::TcpConnectionPtr &conn,
                                  fn::HttpRequest &req,
                                  std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kGet || req.path() != "/share/info")
        return true;

    std::string shareId = req.getQuery("share_id");
    LOG_DEBUG << shareId;
    if (shareId.empty()) {
        sendError(resp, "分享链接无效", fn::HttpResponse::k400BadRequest, conn);
        return true;
    }

    auto mysql = db::MySQLPool::instance().getConnection();
    // 联合查询，并检查是否过期
    std::string sql =
        "SELECT f.original_filename, f.file_size, s.share_type, u.username "
        "FROM file_shares s "
        "JOIN files f ON s.file_id = f.id "
        "JOIN users u ON s.user_id = u.id "
        "WHERE s.share_id = ? AND (s.expire_time IS NULL OR s.expire_time > "
        "NOW()) AND f.is_deleted = 0";

    db::MySQLStatement stmt(*mysql, sql);
    stmt.bindString(shareId);
    stmt.execute();

    auto rs = stmt.getResultSet();
    if (!rs || !rs->next()) {
        sendError(resp, "分享已失效或文件所有者已删除文件",
                  fn::HttpResponse::k404NotFound, conn);
        return true;
    }

    json response = {{"code", 0},
                     {"data",
                      {{"filename", rs->getString(0)},
                       {"fileSize", rs->getInt64(1)},
                       {"shareType", rs->getString(2)},
                       {"sharer", rs->getString(3)}}}};

    std::string bodyStr = response.dump();
    resp->setStatusCode(fn::HttpResponse::k200Ok);
    resp->setContentType("application/json");
    resp->setBody(bodyStr);
    resp->addHeader("Content-Length", std::to_string(bodyStr.size()));
    return true;
}

bool UserHandler::handleShareVerify(const fn::TcpConnectionPtr &conn,
                                    fn::HttpRequest &req,
                                    std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost || req.path() != "/share/verify")
        return false;

    try {
        json reqData = json::parse(req.body());
        std::string shareId = reqData.value("shareId", "");
        std::string extractCode = reqData.value("extractCode", "");

        auto mysql = db::MySQLPool::instance().getConnection();
        std::string sql =
            "SELECT f.node_id, f.original_filename, f.filename, s.share_type, "
            "s.extract_code "
            "FROM file_shares s JOIN files f ON s.file_id = f.id "
            "WHERE s.share_id = ? AND (s.expire_time IS NULL OR s.expire_time "
            "> NOW()) AND f.is_deleted = 0";

        db::MySQLStatement stmt(*mysql, sql);
        stmt.bindString(shareId);

        stmt.execute();
        auto rs = stmt.getResultSet();

        if (!rs || !rs->next()) {
            sendError(resp, "分享已失效或文件所有者已删除文件",
                      fn::HttpResponse::k404NotFound, conn);
            return true;
        }
        std::string nodeId = rs->getString(0);
        std::string originalFilename = rs->getString(1);
        std::string serverFilename = rs->getString(2);
        std::string shareType = rs->getString(3);
        std::string realCode = rs->getString(4);

        // 验证提取码
        if (shareType == "protected" && extractCode != realCode) {
            sendError(resp, "提取码错误", fn::HttpResponse::k403Forbidden,
                      conn);
            return true;
        }

        // 验证通过，获取 DataNode 地址
        auto nodeInfo = NodeManager::instance().getNodeInfo(nodeId);
        if (!nodeInfo) {
            sendError(resp, "存储节点离线",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        // 🌟 核心：签发下载 Token (因为是外部人员，userId 传 0 即可，DataNode
        // 只认 serverFilename)
        std::string downloadToken =
            TokenManager::instance().generateDownloadToken(
                0, originalFilename, serverFilename,
                NodeManager::instance().buildQoSPolicy("normal", true));

        json response = {
            {"code", 0},
            {"data",
             {{"downloadUrl", "http://" + nodeInfo->addr_.toIpPort() +
                                  "/api/datanode/download"},
              {"token", downloadToken}}}};

        std::string bodyStr = response.dump();
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(bodyStr);
        resp->addHeader("Content-Length", std::to_string(bodyStr.size()));
        return true;

    } catch (...) {
        sendError(resp, "参数错误", fn::HttpResponse::k400BadRequest, conn);
        return true;
    }
}

bool UserHandler::handleListSharedWithMe(
    const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
    std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kGet ||
        req.path() != "/share/received")
        return true;

    std::string token = req.getHeader("Authorization").substr(7);
    int userId = TokenManager::instance().verifyUserToken(token);
    if (userId < 0) {
        sendError(resp, "未登录", fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string keyword = urlDecode(req.getQuery("keyword"));
    std::string fileType = req.getQuery("type");

    auto mysql = db::MySQLPool::instance().getConnection();
    // ===== 4. SQL 构建 =====
    std::string sql = "SELECT f.filename, f.original_filename, f.file_size, "
                      "u.username, s.created_at "
                      "FROM file_shares s "
                      "JOIN files f ON s.file_id = f.id "
                      "JOIN users u ON s.user_id = u.id "
                      "WHERE s.target_user_id = ? "
                      "AND s.share_type = 'specific' "
                      "AND f.is_deleted = 0 ";

    if (!keyword.empty()) {
        sql += "AND f.original_filename LIKE ? ";
    }

    if (!fileType.empty()) {
        if (fileType == "image") {
            sql +=
                "AND (f.original_filename LIKE '%.jpg' OR f.original_filename "
                "LIKE '%.png' OR f.original_filename LIKE '%.gif') ";
        } else if (fileType == "video") {
            sql +=
                "AND (f.original_filename LIKE '%.mp4' OR f.original_filename "
                "LIKE '%.avi' OR f.original_filename LIKE '%.mkv') ";
        } else if (fileType == "document") {
            sql +=
                "AND (f.original_filename LIKE '%.pdf' OR f.original_filename "
                "LIKE '%.doc%' OR f.original_filename LIKE '%.txt') ";
        } else if (fileType == "code") {
            sql += "AND (f.original_filename LIKE '%.cpp' OR "
                   "f.original_filename LIKE '%.h' OR f.original_filename LIKE "
                   "'%.py' OR f.original_filename LIKE '%.js') ";
        }
    }

    sql += "ORDER BY s.created_at DESC";

    db::MySQLStatement stmt(*mysql, sql);
    stmt.bindInt(userId);

    if (!keyword.empty()) {
        stmt.bindString("%" + keyword + "%");
    }

    if (!stmt.execute()) {
        sendError(resp, "查询失败", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }
    json files = json::array();
    auto rs = stmt.getResultSet();
    while (rs->next()) {
        files.push_back({{"filename", rs->getString(0)},
                         {"originalName", rs->getString(1)},
                         {"size", rs->getInt64(2)},
                         {"sharerName", rs->getString(3)},
                         {"shareTime", rs->getString(4)}});
    }

    json response = {{"code", 0}, {"data", files}};
    std::string bodyStr = response.dump();
    LOG_DEBUG << bodyStr;
    resp->setStatusCode(fn::HttpResponse::k200Ok);
    resp->setContentType("application/json");
    resp->setBody(bodyStr);
    resp->addHeader("Content-Length", std::to_string(bodyStr.size()));
    return true;
}
