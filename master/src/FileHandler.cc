#include "FileHandler.h"
#include "AsyncDeleteTask.h"
#include "NodeManager.h"
#include "SceneProfileService.h"
#include "StorageFeatureService.h"
#include "TokenManager.h"
#include "db/MySQLPool.h"
#include "db/MySQLStatement.h"
#include "net/HttpResponse.h"
#include "random"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::string buildNodeAccessBaseUrl(const std::shared_ptr<DataNodeInfo> &node) {
    if (node && !node->publicUrl_.empty()) {
        return node->publicUrl_;
    }
    return "http://" + node->addr_.toIpPort();
}

int parsePositiveInt(const std::string &value, int defaultValue) {
    if (value.empty()) {
        return defaultValue;
    }
    try {
        return std::max(0, std::stoi(value));
    } catch (...) {
        return defaultValue;
    }
}

std::string toLikePattern(const std::string &keyword) {
    return "%" + keyword + "%";
}

std::string buildFileTypeFilterSql(const std::string &fileType) {
    if (fileType == "image") {
        return " AND (LOWER(f.original_filename) LIKE '%.jpg' OR "
               "LOWER(f.original_filename) LIKE '%.jpeg' OR "
               "LOWER(f.original_filename) LIKE '%.png' OR "
               "LOWER(f.original_filename) LIKE '%.gif' OR "
               "LOWER(f.original_filename) LIKE '%.webp')";
    }
    if (fileType == "video") {
        return " AND (LOWER(f.original_filename) LIKE '%.mp4' OR "
               "LOWER(f.original_filename) LIKE '%.avi' OR "
               "LOWER(f.original_filename) LIKE '%.mkv' OR "
               "LOWER(f.original_filename) LIKE '%.mov')";
    }
    if (fileType == "document") {
        return " AND (LOWER(f.original_filename) LIKE '%.pdf' OR "
               "LOWER(f.original_filename) LIKE '%.doc%' OR "
               "LOWER(f.original_filename) LIKE '%.txt' OR "
               "LOWER(f.original_filename) LIKE '%.ppt%')";
    }
    if (fileType == "code") {
        return " AND (LOWER(f.original_filename) LIKE '%.cpp' OR "
               "LOWER(f.original_filename) LIKE '%.cc' OR "
               "LOWER(f.original_filename) LIKE '%.c' OR "
               "LOWER(f.original_filename) LIKE '%.h' OR "
               "LOWER(f.original_filename) LIKE '%.hpp' OR "
               "LOWER(f.original_filename) LIKE '%.py' OR "
               "LOWER(f.original_filename) LIKE '%.js' OR "
               "LOWER(f.original_filename) LIKE '%.json' OR "
               "LOWER(f.original_filename) LIKE '%.ts')";
    }
    return "";
}

std::vector<std::string>
splitRelativeParentSegments(const std::string &relativePath) {
    std::string normalized = relativePath;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    const auto pos = normalized.find_last_of('/');
    if (pos == std::string::npos) {
        return {};
    }

    std::string folderPath = normalized.substr(0, pos);
    std::vector<std::string> segments;
    std::stringstream ss(folderPath);
    std::string segment;
    while (std::getline(ss, segment, '/')) {
        if (!segment.empty()) {
            segments.push_back(segment);
        }
    }
    return segments;
}
} // namespace

FileHandler::FileHandler() {
    StorageFeatureService::instance().ensureSchema();
    SceneProfileService::instance().ensureSchema();
}

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

    const int limit =
        std::max(1, std::min(100, parsePositiveInt(req.getQuery("limit"), 20)));
    int page = std::max(1, parsePositiveInt(req.getQuery("page"), 1));
    int offset = parsePositiveInt(req.getQuery("offset"), -1);
    if (offset < 0) {
        offset = (page - 1) * limit;
    } else {
        page = offset / limit + 1;
    }
    const int folderId = parsePositiveInt(req.getQuery("folder_id"), 0);
    const std::string fileType = req.getQuery("type");
    const std::string keyword = urlDecode(req.getQuery("keyword"));

    // 获取数据库连接
    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        sendError(resp, "数据库连接失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
    auto &storageService = StorageFeatureService::instance();
    std::string sceneTag =
        SceneProfileService::instance().getUserSceneTag(*mysql, userId);
    auto storageSummary = storageService.getStorageSummary(*mysql, userId);

    if (folderId > 0 &&
        !storageService.folderExists(*mysql, userId, folderId) &&
        fileType.empty()) {
        sendError(resp, "目录不存在", fn::HttpResponse::k404NotFound, conn);
        return true;
    }

    std::vector<StorageFeatureService::FolderInfo> folders;
    if (fileType.empty()) {
        folders = storageService.listFolders(*mysql, userId, folderId);
    }

    std::string countSql =
        "SELECT COUNT(*) "
        "FROM files f "
        "WHERE f.user_id = ? AND f.is_deleted = 0 AND f.status = 'success'";
    std::string dataSql =
        "SELECT f.id, f.filename, f.original_filename, f.file_size, "
        "f.created_at, COALESCE(f.folder_id, 0), f.relative_path, "
        "s.share_type, s.target_user_id, s.share_id, s.expire_time, "
        "s.extract_code, u.username "
        "FROM files f "
        "LEFT JOIN file_shares s ON f.id = s.file_id "
        "LEFT JOIN users u ON s.target_user_id = u.id "
        "WHERE f.user_id = ? AND f.is_deleted = 0 AND f.status = 'success'";

    const bool ignoreFolderByType = !fileType.empty();
    if (!keyword.empty()) {
        countSql += " AND f.original_filename LIKE ?";
        dataSql += " AND f.original_filename LIKE ?";
    }
    if (!ignoreFolderByType) {
        if (folderId > 0) {
            countSql += " AND f.folder_id = ?";
            dataSql += " AND f.folder_id = ?";
        } else {
            countSql += " AND f.folder_id IS NULL";
            dataSql += " AND f.folder_id IS NULL";
        }
    }
    countSql += buildFileTypeFilterSql(fileType);
    dataSql += buildFileTypeFilterSql(fileType);
    dataSql += " ORDER BY f.created_at DESC LIMIT ? OFFSET ?";

    db::MySQLStatement countStmt(*mysql, countSql);
    countStmt.bindInt(userId);
    if (!keyword.empty()) {
        countStmt.bindString(toLikePattern(keyword));
    }
    if (!ignoreFolderByType && folderId > 0) {
        countStmt.bindInt(folderId);
    }
    if (!countStmt.execute()) {
        LOG_ERROR << "Count query failed: " << countStmt.getError();
        sendError(resp, "查询失败", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    int totalFiles = 0;
    auto countRs = countStmt.getResultSet();
    if (countRs && countRs->next()) {
        totalFiles = countRs->getInt(0);
    }

    db::MySQLStatement stmt(*mysql, dataSql);
    if (stmt.hasError()) {
        sendError(resp, "数据库错误", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }
    stmt.bindInt(userId);
    if (!keyword.empty()) {
        stmt.bindString(toLikePattern(keyword));
    }
    if (!ignoreFolderByType && folderId > 0) {
        stmt.bindInt(folderId);
    }
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
    json folderJson = json::array();

    for (const auto &folder : folders) {
        folderJson.push_back({{"id", folder.id},
                              {"parentId", folder.parentId},
                              {"name", folder.name},
                              {"fullPath", folder.fullPath},
                              {"createdAt", folder.createdAt}});
    }

    // 遍历文件列表结果
    // 只适用于一个文件一个分享
    while (rs->next()) {
        json fileInfo;

        fileInfo["id"] = rs->getInt(0);
        fileInfo["name"] = rs->getString(1);
        fileInfo["originalName"] = rs->getString(2);
        fileInfo["size"] = rs->getInt64(3);
        fileInfo["createdAt"] = rs->getString(4);
        fileInfo["folderId"] = rs->getInt(5);
        fileInfo["relativePath"] = rs->getString(6);

        if (!rs->isNull(7)) {
            json shareInfo;
            std::string shareType = rs->getString(7);
            shareInfo["type"] = shareType;
            if (!rs->isNull(9)) {
                shareInfo["shareCode"] = rs->getString(9);
            }

            if (shareType == "protected" && !rs->isNull(11)) {
                shareInfo["extractCode"] = rs->getString(11);
            }

            if (shareType == "specific") {
                if (!rs->isNull(8)) {
                    shareInfo["sharedWithId"] = rs->getInt(8);
                }
                if (!rs->isNull(12)) {
                    shareInfo["sharedWithUsername"] = rs->getString(12);
                }
            }

            if (!rs->isNull(10)) {
                shareInfo["expireTime"] = rs->getString(10);
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
    response["folders"] = folderJson;
    response["sceneTag"] = sceneTag;
    response["serviceLevel"] = storageSummary.serviceLevel;
    response["storage"] = {{"quotaBytes", storageSummary.quotaBytes},
                           {"usedBytes", storageSummary.usedBytes},
                           {"remainingBytes", storageSummary.remainingBytes},
                           {"isVip", storageSummary.isVip}};
    response["pagination"] = {
        {"page", page},
        {"limit", limit},
        {"offset", offset},
        {"total", totalFiles},
        {"totalPages", limit > 0 ? (totalFiles + limit - 1) / limit : 0}};
    response["currentFolderId"] = folderId;
    response["breadcrumbs"] = json::array();
    for (const auto &folder :
         storageService.getFolderBreadcrumbs(*mysql, userId, folderId)) {
        response["breadcrumbs"].push_back({{"id", folder.id},
                                           {"name", folder.name},
                                           {"fullPath", folder.fullPath}});
    }

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
        const int requestedFolderId = requestData.value("folder_id", 0);
        const std::string relativePath = requestData.value("relative_path", "");
        const std::string preferredNodeId =
            requestData.value("preferred_node_id", "");
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

        auto &storageService = StorageFeatureService::instance();
        if (requestedFolderId > 0 &&
            !storageService.folderExists(*mysql, userId, requestedFolderId)) {
            sendError(resp, "目标目录不存在", fn::HttpResponse::k404NotFound,
                      conn);
            return true;
        }

        StorageFeatureService::StorageSummary storageSummary;
        if (!storageService.canUploadFile(*mysql, userId, fileSize,
                                          &storageSummary)) {
            sendError(resp, "存储空间不足，当前账号配额已接近上限",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        const std::string serviceLevel = storageSummary.serviceLevel;
        const std::string sceneTag =
            SceneProfileService::instance().getUserSceneTag(*mysql, userId);
        const bool batchMode = sceneTag == "development";

        int finalFolderId = storageService.ensureFolderPath(
            *mysql, userId, requestedFolderId,
            splitRelativeParentSegments(relativePath));
        if (!relativePath.empty() && finalFolderId == 0 &&
            requestedFolderId == 0 &&
            !splitRelativeParentSegments(relativePath).empty()) {
            sendError(resp, "创建目录失败",
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
                        "file_md5, folder_id, relative_path, created_at, "
                        "updated_at) "
                        "VALUES (?, ?, ?, ?, ?, ?, 'success', ?, ?, ?, ?, "
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
                    if (finalFolderId > 0) {
                        fastStmt.bindInt(finalFolderId);
                    } else {
                        fastStmt.bindNull();
                    }
                    fastStmt.bindString(relativePath);
                    fastStmt.bindString(currentTime); // 对应 created_at
                    fastStmt.bindString(currentTime); // 对应 updated_at
                    if (fastStmt.execute()) {
                        // 返回特殊的 code (比如
                        // 1)，告诉前端秒传成功，不需要再传给 DataNode 了
                        json response = {
                            {"code", 1},
                            {"message", "秒传成功"},
                            {"file",
                             {{"id", fastStmt.insertId()},
                              {"name", existServerFilename},
                              {"originalName", originalFilename},
                              {"size", fileSize},
                              {"createdAt", currentTime},
                              {"folderId", finalFolderId},
                              {"relativePath", relativePath}}},
                            {"storage",
                             {{"quotaBytes", storageSummary.quotaBytes},
                              {"usedBytes", storageSummary.usedBytes},
                              {"remainingBytes",
                               storageSummary.remainingBytes}}}};
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

        // 批量开发上传优先复用同一个 DataNode，便于浏览器 Keep-Alive 串行发送。
        std::shared_ptr<DataNodeInfo> dataNode;
        if (!preferredNodeId.empty()) {
            auto preferredNode =
                NodeManager::instance().getNodeInfo(preferredNodeId);
            if (preferredNode && preferredNode->isAlive_) {
                const uint64_t requiredMb =
                    (fileSize + 1024 * 1024 - 1) / (1024 * 1024);
                if (preferredNode->diskFreeMb_ == 0 ||
                    preferredNode->diskFreeMb_ >= requiredMb) {
                    dataNode = preferredNode;
                }
            }
        }
        if (!dataNode) {
            dataNode = NodeManager::instance().getAliveNode(fileSize);
        }
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
            "file_type, user_id, node_id, status, file_md5, folder_id, "
            "relative_path, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, 'uploading', ?, ?, ?, ?, ?)";
        db::MySQLStatement stmt(*mysql, insertSql);

        stmt.bindString(serverFilename);
        stmt.bindString(originalFilename);
        stmt.bindInt64(fileSize);
        stmt.bindString(fileType);
        stmt.bindInt(userId);
        stmt.bindString(dataNode->id_);
        stmt.bindString(fileMd5);
        if (finalFolderId > 0) {
            stmt.bindInt(finalFolderId);
        } else {
            stmt.bindNull();
        }
        stmt.bindString(relativePath);
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
            currentTime, sceneTag, batchMode,
            NodeManager::instance().buildQoSPolicy(serviceLevel, false));

        // 组装响应，告诉客户端去哪里上传
        json response = {{"code", 0},
                         {"message", "获取上传地址成功"},
                         {"data",
                          {{"fileId", fileId},
                           {"nodeId", dataNode->id_},
                           {"batchMode", batchMode},
                           {"serviceLevel", serviceLevel},
                           {"sceneTag", sceneTag},
                           {"folderId", finalFolderId},
                           {"uploadUrl", buildNodeAccessBaseUrl(dataNode) +
                                             "/api/datanode/upload"},
                           {"uploadToken", fileUploadResponse.token}}}};

        std::string bodyStr = response.dump();
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(bodyStr);
        resp->addHeader("Content-Length", std::to_string(bodyStr.size()));

        LOG_INFO << "分配 DataNode 成功: " << dataNode->addr_.toIpPort()
                 << ", public_url="
                 << (dataNode->publicUrl_.empty() ? "(fallback to internal)"
                                                  : dataNode->publicUrl_)
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
    if (!mysql) {
        sendError(resp, "数据库连接失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
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

    int fileId = rs->getInt(0);
    std::string nodeId = rs->getString(1);
    std::string original_filename = rs->getString(2);
    uintmax_t fileSize = rs->getInt64(3);
    std::string serviceLevel =
        StorageFeatureService::instance().getUserServiceLevel(*mysql, userId);
    std::string sceneTag =
        SceneProfileService::instance().getUserSceneTag(*mysql, userId);

    // 获取 DataNode 地址
    LOG_DEBUG << "nodeId: " << nodeId;
    auto nodeInfo = NodeManager::instance().getNodeInfo(nodeId);
    if (!nodeInfo || !nodeInfo->isAlive_) {
        sendError(resp, "存储节点离线",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
    SceneProfileService::instance().incrementDownloadCount(*mysql, fileId);
    auto fileResponse = TokenManager::instance().generateDownloadToken(
        userId, original_filename, serverFilename, sceneTag,
        NodeManager::instance().buildQoSPolicy(serviceLevel, true));

    // 返回 DataNode 播放地址
    json response = {{"code", 0},
                     {"data",
                      {{"downloadUrl", buildNodeAccessBaseUrl(nodeInfo) +
                                           "/api/datanode/download"},
                       {"previewUrl", buildNodeAccessBaseUrl(nodeInfo) +
                                          "/api/datanode/text_preview"},
                       {"token", fileResponse},
                       {"fileSize", fileSize}}}};

    resp->setStatusCode(fn::HttpResponse::k200Ok);
    std::string respBody = response.dump();
    resp->setBody(respBody);
    resp->addHeader("Content-Length", std::to_string(respBody.size()));
    return true;
}

bool FileHandler::handleCreateFolder(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost || req.path() != "/folders") {
        return false;
    }

    const std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.find("Bearer ") != 0) {
        sendError(resp, "未授权的访问，请先登录",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    const int userId =
        TokenManager::instance().verifyUserToken(authHeader.substr(7));
    if (userId < 0) {
        sendError(resp, "Token 无效或已过期，请重新登录",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        sendError(resp, "数据库连接失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    try {
        const json requestData = json::parse(req.body());
        const std::string folderName = requestData.value("name", "");
        const int parentFolderId = requestData.value("parent_id", 0);

        auto &storageService = StorageFeatureService::instance();
        if (parentFolderId > 0 &&
            !storageService.folderExists(*mysql, userId, parentFolderId)) {
            sendError(resp, "父目录不存在", fn::HttpResponse::k404NotFound,
                      conn);
            return true;
        }

        const int folderId = storageService.ensureFolderPath(
            *mysql, userId, parentFolderId, {folderName});
        if (folderId <= 0) {
            sendError(resp, "创建目录失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        auto folder = storageService.getFolderInfo(*mysql, userId, folderId);
        json response = {{"code", 0},
                         {"message", "目录创建成功"},
                         {"folder",
                          {{"id", folder.id},
                           {"parentId", folder.parentId},
                           {"name", folder.name},
                           {"fullPath", folder.fullPath},
                           {"createdAt", folder.createdAt}}}};
        const std::string body = response.dump();
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(body);
        resp->addHeader("Content-Length", std::to_string(body.size()));
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "创建目录失败：" + std::string(e.what()),
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool FileHandler::handleListFolders(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kGet || req.path() != "/folders") {
        return false;
    }

    const std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.find("Bearer ") != 0) {
        sendError(resp, "未授权的访问，请先登录",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    const int userId =
        TokenManager::instance().verifyUserToken(authHeader.substr(7));
    if (userId < 0) {
        sendError(resp, "Token 无效或已过期，请重新登录",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        sendError(resp, "数据库连接失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    const int parentFolderId = parsePositiveInt(req.getQuery("parent_id"), 0);
    auto &storageService = StorageFeatureService::instance();
    if (parentFolderId > 0 &&
        !storageService.folderExists(*mysql, userId, parentFolderId)) {
        sendError(resp, "目录不存在", fn::HttpResponse::k404NotFound, conn);
        return true;
    }

    json folders = json::array();
    for (const auto &folder :
         storageService.listFolders(*mysql, userId, parentFolderId)) {
        folders.push_back({{"id", folder.id},
                           {"parentId", folder.parentId},
                           {"name", folder.name},
                           {"fullPath", folder.fullPath},
                           {"createdAt", folder.createdAt}});
    }

    json response = {{"code", 0}, {"folders", folders}};
    const std::string body = response.dump();
    resp->setStatusCode(fn::HttpResponse::k200Ok);
    resp->setContentType("application/json");
    resp->setBody(body);
    resp->addHeader("Content-Length", std::to_string(body.size()));
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
