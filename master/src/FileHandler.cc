#include "FileHandler.h"
#include "AsyncDeleteTask.h"
#include "NodeManager.h"
#include "TokenManager.h"
#include "db/MySQLPool.h"
#include "db/MySQLStatement.h"
#include "net/HttpResponse.h"
#include "random"
#include <chrono>
#include <string>

bool FileHandler::handleListFiles(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    // 验证Token
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

    // （可扩展）分页
    int limit = 50;
    int offset = 0;
    if (!req.getQuery("limit").empty())
        limit = std::stoi(req.getQuery("limit"));
    if (!req.getQuery("offset").empty())
        offset = std::stoi(req.getQuery("offset"));

    // 获取数据库连接
    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        sendError(resp, "数据库连接失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    // 动态构建SQL语句
    std::string sql =
        "SELECT "
        "f.id, f.filename, f.original_filename, f.file_size, f.created_at, "
        "s.share_type, s.target_user_id, s.share_id, s.expire_time, "
        "s.extract_code, "
        "u.username "
        "FROM files f "
        "LEFT JOIN file_shares s ON f.id = s.file_id "
        "LEFT JOIN users u ON s.target_user_id = u.id "
        "WHERE f.user_id = ? "
        "AND f.is_deleted = 0 "
        "AND f.status = 'success'";

    sql += " ORDER BY f.created_at DESC LIMIT ? OFFSET ?";

    LOG_INFO << "Executing file list query for user: " << userId;

    // 执行查询
    LOG_DEBUG << sql;
    db::MySQLStatement stmt(*mysql, sql);
    if (stmt.hasError()) {
        sendError(resp, "数据库错误", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    stmt.bindInt(userId);
    stmt.bindInt(limit);
    stmt.bindInt(offset);

    if (!stmt.execute()) {
        LOG_ERROR << "Query failed: " << stmt.getError();
        sendError(resp, "查询失败", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }
    auto rs = stmt.getResultSet();
    json files = json::array();

    // 遍历文件列表结果
    // 只适用于一个文件一个分享
    while (rs->next()) {
        json fileInfo;

        fileInfo["id"] = rs->getInt(0);
        fileInfo["name"] = rs->getString(1);
        fileInfo["originalName"] = rs->getString(2);
        fileInfo["size"] = rs->getInt64(3);
        fileInfo["createdAt"] = rs->getString(4);

        if (!rs->isNull(5)) {
            json shareInfo;
            std::string shareType = rs->getString(5);
            shareInfo["type"] = shareType;
            if (!rs->isNull(7)) {
                shareInfo["shareCode"] = rs->getString(7);
            }

            if (shareType == "protected" && !rs->isNull(9)) {
                shareInfo["extractCode"] = rs->getString(9);
            }

            if (shareInfo == "specific") {
                if (!rs->isNull(6)) {
                    shareInfo["sharedWithId"] = rs->getInt(6);
                }
                if (!rs->isNull(10)) {
                    shareInfo["sharedWithUsername"] = rs->getString(10);
                }
            }

            if (!rs->isNull(8)) {
                shareInfo["expireTime"] = rs->getString(8);
            }

            fileInfo["shareInfo"] = shareInfo;
        }

        files.push_back(fileInfo);
    }

    // 构建响应
    json response;
    response["code"] = 0;
    response["message"] = "Success";
    response["files"] = files;

    resp->setStatusCode(fileserver::net::HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->setContentType("application/json");
    resp->addHeader("Connection", "close");
    resp->setBody(response.dump());

    conn->setWriteCompleteCallback(
        [](const fileserver::net::TcpConnectionPtr &connection) {
            LOG_INFO << "List files complete, closing connection";
            connection->shutdown();
            return true;
        });

    return true;
}

bool FileHandler::handleFileUpload(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) { // 路由匹配
    LOG_INFO << "Receive file upload request";

    try {
        std::string authHeader = req.getHeader("Authorization");
        if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
            sendError(resp, "未授权的访问，请先登录",
                      fn::HttpResponse::k401Unauthorized, conn);
            return true;
        }

        std::string userToken = authHeader.substr(7);
        auto userId = TokenManager::instance().verifyUserToken(userToken);
        if (userId < 0) {
            sendError(resp, "Token 无效或已过期，请重新登录",
                      fn::HttpResponse::k401Unauthorized, conn);
            return true;
        }

        // 解析客户端发来的文件源数据
        json requestData = json::parse(req.body());
        std::string originalFilename =
            requestData.value("original_filename", "");
        uintmax_t fileSize = requestData.value("file_size", 0);
        std::string fileType = getFileType(originalFilename);

        std::string serverFilename = generateUniqueFilename("upload");

        /// TODO: 秒传
        std::string fileMd5 = requestData.value("md5", "");

        if (originalFilename.empty() || fileSize == 0) {
            sendError(resp, "文件名和大小不能为空",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        // 在数据库中预先插入一条文件记录，状态设为 "uploading" (上传中)
        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            sendError(resp, "数据库连接失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        // MD5 秒传逻辑
        if (!fileMd5.empty()) {
            // 查找数据库中是否已经有这个MD5且状态为success的文件
            std::string checkSql =
                "SELECT node_id, filename FROM files "
                "WHERE file_md5 = ? AND "
                "status = 'success' AND is_deleted = '0' LIMIT 1";

            db::MySQLStatement checkStmt(*mysql, checkSql);
            checkStmt.bindString(fileMd5);

            if (checkStmt.execute()) {
                auto rs = checkStmt.getResultSet();
                if (rs->next()) {
                    // 秒传命中！获取已存在的物理文件信息
                    std::string existNodeId = rs->getString(0);
                    std::string existServerFilename = rs->getString(1);

                    LOG_INFO << "触发秒传!MD5: " << fileMd5
                             << " 复用物理文件: " << existServerFilename;

                    // 直接为当前用户插入一条新记录，状态直接标记为
                    // 'success'
                    auto currentTime = getCurrentTimeStr();
                    std::string fastInsertSql =
                        "INSERT INTO files (filename, original_filename, "
                        "file_size, file_type, user_id, node_id, status, "
                        "file_md5, created_at, updated_at) "
                        "VALUES (?, ?, ?, ?, ?, ?, 'success', ?, ?, "
                        "?)";
                    db::MySQLStatement fastStmt(*mysql, fastInsertSql);

                    fastStmt.bindString(
                        existServerFilename); // 指向同一个物理文件
                    fastStmt.bindString(originalFilename);
                    fastStmt.bindInt64(fileSize);
                    fastStmt.bindString(fileType);
                    fastStmt.bindInt(userId);
                    fastStmt.bindString(existNodeId);
                    fastStmt.bindString(fileMd5);
                    fastStmt.bindString(currentTime); // 对应 created_at
                    fastStmt.bindString(currentTime); // 对应 updated_at
                    if (fastStmt.execute()) {
                        // 返回特殊的 code (比如
                        // 1)，告诉前端秒传成功，不需要再传给 DataNode 了
                        json response = {{"code", 1},
                                         {"message", "秒传成功"},
                                         {"file",
                                          {"id", fastStmt.insertId()},
                                          {"name", existServerFilename},
                                          {"originalName", originalFilename},
                                          {"size", fileSize},
                                          {"createdAt", currentTime}}};
                        std::string bodyStr = response.dump();
                        resp->setStatusCode(fn::HttpResponse::k200Ok);
                        resp->setContentType("application/json");
                        resp->setBody(bodyStr);
                        resp->addHeader("Content-Length",
                                        std::to_string(bodyStr.size()));
                        return true;
                    }
                }
            }
        }

        // 从 NodeManager 获取一个存活的 DataNode
        auto dataNode = NodeManager::instance().getAliveNode(fileSize);
        if (!dataNode) {
            LOG_ERROR << "没有可用的 DataNode 节点";
            sendError(resp, "系统繁忙，当前无可用存储节点",
                      fileserver::net::HttpResponse::k500InternalServerError,
                      conn);
            return true;
        }

        auto currentTime = getCurrentTimeStr();
        std::string insertSql =
            "INSERT INTO files (filename, original_filename, file_size, "
            "file_type, user_id, node_id, status, file_md5, created_at, "
            "updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, 'uploading', ?, ?, ?)";
        db::MySQLStatement stmt(*mysql, insertSql);

        stmt.bindString(serverFilename);
        stmt.bindString(originalFilename);
        stmt.bindInt64(fileSize);
        stmt.bindString(fileType);
        stmt.bindInt(userId);
        stmt.bindString(dataNode->id_);
        stmt.bindString(fileMd5);
        stmt.bindString(currentTime);
        stmt.bindString(currentTime);

        if (!stmt.execute()) {
            LOG_ERROR << "插入文件记录失败: " << stmt.getError();
            sendError(resp, "数据库错误",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        // 获取生成的文件ID
        auto fileId = stmt.insertId();

        // 生成专属token
        auto fileUploadResponse = TokenManager::instance().generateUploadToken(
            userId, fileId, dataNode->id_, originalFilename, serverFilename,
            currentTime);

        //  组装响应，告诉客户端去哪里上传
        json response = {
            {"code", 0},
            {"message", "获取上传地址成功"},
            {"data",
             {{"fileId", fileId},
              {"uploadUrl",
               "http://" + dataNode->addr_.toIpPort() + "/api/datanode/upload"},
              {"uploadToken", fileUploadResponse.token}}}};

        std::string bodyStr = response.dump();
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(bodyStr);
        resp->addHeader("Content-Length",
                        std::to_string(bodyStr.size())); // 别忘了这个！

        LOG_INFO << "分配 DataNode 成功: " << dataNode->addr_.toIpPort()
                 << " 给文件 ID: " << fileId;
        return true;
    } catch (const json::parse_error &e) {
        LOG_ERROR << "JSON 解析错误: " << e.what();
        sendError(resp, "无效的请求格式", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR << "处理上传申请失败: " << e.what();
        sendError(resp, "服务器内部错误",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool FileHandler::handleFileDownload(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) { // 验证用户登录状态
    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
        sendError(resp, "未授权的访问，请先登录",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string userToken = authHeader.substr(7);
    auto userId = TokenManager::instance().verifyUserToken(userToken);
    if (userId < 0) {
        sendError(resp, "Token 无效或已过期，请重新登录",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string serverFilename = req.getPathParam("filename");
    if (serverFilename.empty()) {
        sendError(resp, "文件名不能为空", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    }

    auto mysql = db::MySQLPool::instance().getConnection();
    std::string querySql =
        "SELECT f.id, f.node_id, f.original_filename, f.file_size "
        "FROM files f "
        "LEFT JOIN file_shares s ON f.id = s.file_id AND s.share_type = "
        "'specific' AND s.target_user_id = ? "
        "WHERE f.filename = ? AND f.status = 'success' "
        "AND (f.user_id = ? OR s.share_id IS NOT NULL) AND f.is_deleted = 0 "
        "LIMIT 1";
    db::MySQLStatement stmt(*mysql, querySql);
    stmt.bindInt(userId);            // 对应 s.target_user_id = ?
    stmt.bindString(serverFilename); // 对应 f.filename = ?
    stmt.bindInt(userId);            // 对应 f.user_id = ?

    if (!stmt.execute()) {
        sendError(resp, "数据库错误", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    auto rs = stmt.getResultSet();
    if (!rs->next()) {
        sendError(resp, "文件未找到或文件所有者已取消分享",
                  fn::HttpResponse::k404NotFound, conn);
        return true;
    }

    int64_t fileId = rs->getInt64(0);
    std::string nodeId = rs->getString(1);
    std::string original_filename = rs->getString(2);
    uintmax_t fileSize = rs->getInt64(3);

    // 获取 DataNode 地址
    LOG_DEBUG << "nodeId: " << nodeId;
    auto nodeInfo = NodeManager::instance().getNodeInfo(nodeId);
    if (!nodeInfo || !nodeInfo->isAlive_) {
        sendError(resp, "存储节点离线",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
    auto fileResponse = TokenManager::instance().generateDownloadToken(
        userId, original_filename, serverFilename);

    // 返回 DataNode 播放地址
    json response = {{"code", 0},
                     {"data",
                      {{"downloadUrl", "http://" + nodeInfo->addr_.toIpPort() +
                                           "/api/datanode/download"},
                       {"token", fileResponse},
                       {"fileSize", fileSize}}}};

    resp->setStatusCode(fn::HttpResponse::k200Ok);
    std::string respBody = response.dump();
    resp->setBody(respBody);
    resp->addHeader("Content-Length", std::to_string(respBody.size()));
    return true;
}

bool FileHandler::handleDeleteFile(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kDelete || req.path() != "/delete")
        return true;

    std::string token = req.getHeader("Authorization").substr(7);
    int userId = TokenManager::instance().verifyUserToken(token);
    if (userId < 0) {
        sendError(resp, "未授权的访问，请先登录",
                  fileserver::net::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string filename = req.getQuery("filename");
    if (filename.empty()) {
        sendError(resp, "文件名为空",
                  fileserver::net::HttpResponse::k400BadRequest, conn);
        return true;
    }

    auto mysql = db::MySQLPool::instance().getConnection();
    std::string sql = "UPDATE files SET is_deleted = 1, deleted_at = NOW() "
                      "WHERE filename = ? AND user_id = ? AND is_deleted = 0";
    db::MySQLStatement stmt(*mysql, sql);
    stmt.bindString(filename);
    stmt.bindInt(userId);

    if (!stmt.execute() || stmt.affectedRows() == 0) {
        sendError(resp, "文件不存在或已删除",
                  fileserver::net::HttpResponse::k404NotFound, conn);
        return true;
    }

    json response = {{"code", 0}, {"message", "已移入回收站"}};
    resp->setStatusCode(fn::HttpResponse::k200Ok);
    std::string respBody = response.dump();
    resp->setBody(respBody);
    resp->setContentType("application/json");
    resp->addHeader("Content-Length", std::to_string(respBody.size()));
    return true;
}

bool FileHandler::handleListRecycleBin(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kGet || req.path() != "/recycle_bin")
        return true;

    std::string token = req.getHeader("Authorization").substr(7);
    int userId = TokenManager::instance().verifyUserToken(token);
    if (userId < 0) {
        sendError(resp, "未登录",
                  fileserver::net::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string keyword = urlDecode(req.getQuery("keyword"));
    std::string fileType = req.getQuery("type");

    auto mysql = db::MySQLPool::instance().getConnection();
    std::string sql = "SELECT id, original_filename, file_size, deleted_at "
                      "FROM files "
                      "WHERE user_id = ? AND is_deleted = 1 ";

    if (!keyword.empty()) {
        sql += "AND original_filename LIKE ? ";
    }

    if (!fileType.empty()) {
        if (fileType == "image") {
            sql += "AND (original_filename LIKE '%.jpg' OR original_filename "
                   "LIKE '%.png' OR original_filename LIKE '%.gif') ";
        } else if (fileType == "video") {
            sql += "AND (original_filename LIKE '%.mp4' OR original_filename "
                   "LIKE '%.avi' OR original_filename LIKE '%.mkv') ";
        } else if (fileType == "document") {
            sql += "AND (original_filename LIKE '%.pdf' OR original_filename "
                   "LIKE '%.doc%' OR original_filename LIKE '%.txt') ";
        } else if (fileType == "code") {
            sql += "AND (original_filename LIKE '%.cpp' OR original_filename "
                   "LIKE '%.h' OR original_filename LIKE '%.py' OR "
                   "original_filename LIKE '%.js') ";
        }
    }

    sql += "ORDER BY deleted_at DESC";
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
        files.push_back({{"id", rs->getInt(0)},
                         {"originalName", rs->getString(1)},
                         {"size", rs->getInt64(2)},
                         {"deletedAt", rs->getString(3)}});
    }

    json response = {{"code", 0}, {"data", files}};
    std::string respBody = response.dump();
    resp->setStatusCode(fileserver::net::HttpResponse::k200Ok);
    resp->setBody(respBody);
    resp->setContentType("application/json");
    resp->addHeader("Content-Length", std::to_string(respBody.size()));
    return true;
}

bool FileHandler::handleRestoreFile(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost || req.path() != "/restore")
        return false;

    std::string token = req.getHeader("Authorization").substr(7);
    int userId = TokenManager::instance().verifyUserToken(token);
    if (userId < 0) {
        sendError(resp, "未登录",
                  fileserver::net::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    json reqData = json::parse(req.body());
    int fileId = reqData.value("fileId", 0);

    auto mysql = db::MySQLPool::instance().getConnection();
    std::string sql = "UPDATE files SET is_deleted = 0, deleted_at = NULL "
                      "WHERE id = ? AND user_id = ?";
    db::MySQLStatement stmt(*mysql, sql);
    stmt.bindInt(fileId);
    stmt.bindInt(userId);

    if (!stmt.execute() || stmt.affectedRows() == 0) {
        sendError(resp, "恢复失败",
                  fileserver::net::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    json response = {{"code", 0}, {"message", "文件已回复"}};
    std::string respBody = response.dump();
    resp->setStatusCode(fileserver::net::HttpResponse::k200Ok);
    resp->setBody(respBody);
    resp->setContentType("application/json");
    resp->addHeader("Content-Length", std::to_string(respBody.size()));
    return true;
}

bool FileHandler::handleHardDelete(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost || req.path() != "/hard_delete")
        return false;

    std::string token = req.getHeader("Authorization").substr(7);
    int userId = TokenManager::instance().verifyUserToken(token);
    if (userId < 0) {
        sendError(resp, "未登录",
                  fileserver::net::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    json reqData = json::parse(req.body());
    int fileId = reqData.value("fileId", 0);

    auto mysql = db::MySQLPool::instance().getConnection();

    std::string sql = "UPDATE files SET is_deleted = 2 WHERE id = ? AND "
                      "user_id = ? AND is_deleted = 1";
    db::MySQLStatement stmt(*mysql, sql);
    stmt.bindInt(fileId);
    stmt.bindInt(userId);

    if (!stmt.execute() || stmt.affectedRows() == 0) {
        sendError(resp, "文件不存在",
                  fileserver::net::HttpResponse::k404NotFound, conn);
        return true;
    }

    // 顺手清理掉分享记录（因为文件马上就要没了）
    std::string delShareSql = "DELETE FROM file_shares WHERE file_id = ?";
    db::MySQLStatement delShareStmt(*mysql, delShareSql);
    delShareStmt.bindInt(fileId);
    delShareStmt.execute();

    json response(
        {{"code", 0}, {"message", "彻底删除成功，系统将在后台清理物理空间"}});
    std::string bodyStr = response.dump();
    resp->setStatusCode(fileserver::net::HttpResponse::k200Ok);
    resp->addHeader("Content-Length", std::to_string(bodyStr.size()));
    resp->setContentType("application/json");
    resp->setBody(bodyStr);
    return true;
}

std::string FileHandler::generateUniqueFilename(const std::string &prefix) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch())
                         .count();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    return prefix + "_" + std::to_string(timestamp) + "_" +
           std::to_string(dis(gen));
}