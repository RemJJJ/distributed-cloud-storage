#include "FileHandler.h"
#include "AsyncDeleteTask.h"
#include "HotCacheService.h"
#include "NodeManager.h"
#include "SceneModeService.h"
#include "StorageFeatureService.h"
#include "TokenManager.h"
#include "db/MySQLPool.h"
#include "db/MySQLStatement.h"
#include "net/HttpResponse.h"
#include "random"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string getExtensionLower(const std::string &filename) {
    const auto pos = filename.find_last_of('.');
    if (pos == std::string::npos || pos + 1 >= filename.size()) {
        return "";
    }
    return toLowerCopy(filename.substr(pos + 1));
}

bool isVideoFile(const std::string &filename) {
    const std::string ext = getExtensionLower(filename);
    return ext == "mp4" || ext == "avi" || ext == "mkv" || ext == "mov";
}

std::string trimCopy(const std::string &value) {
    const auto begin = std::find_if_not(
        value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto end = std::find_if_not(
        value.rbegin(), value.rend(),
        [](unsigned char c) { return std::isspace(c) != 0; })
                         .base();
    if (begin >= end) {
        return "";
    }
    return std::string(begin, end);
}

void sendJsonResponse(const std::shared_ptr<fn::HttpResponse> &resp,
                      const json &payload,
                      fn::HttpResponse::HttpStatusCode status =
                          fn::HttpResponse::k200Ok) {
    const std::string body = payload.dump();
    resp->setStatusCode(status);
    resp->setContentType("application/json");
    resp->setBody(body);
    resp->addHeader("Content-Length", std::to_string(body.size()));
}

void ensureVideoNotesTable(db::MySQLPool::ConnectionGuard &mysql) {
    db::MySQLStatement stmt(
        mysql,
        "CREATE TABLE IF NOT EXISTS video_notes ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "user_id INT NOT NULL,"
        "file_id INT NOT NULL,"
        "note_time_ms INT NOT NULL,"
        "tag VARCHAR(64) NOT NULL DEFAULT '',"
        "content TEXT NOT NULL,"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE "
        "CURRENT_TIMESTAMP,"
        "INDEX idx_video_notes_user_file_time "
        "(user_id, file_id, note_time_ms)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    if (!stmt.execute()) {
        LOG_WARN << "Failed to ensure video_notes table: " << stmt.getError();
    }
}

void ensureVideoLearningProgressTable(db::MySQLPool::ConnectionGuard &mysql) {
    db::MySQLStatement stmt(
        mysql,
        "CREATE TABLE IF NOT EXISTS video_learning_progress ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "user_id INT NOT NULL,"
        "file_id INT NOT NULL,"
        "last_position_ms INT NOT NULL DEFAULT 0,"
        "max_reached_ms INT NOT NULL DEFAULT 0,"
        "duration_ms INT NOT NULL DEFAULT 0,"
        "watched_ms INT NOT NULL DEFAULT 0,"
        "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE "
        "CURRENT_TIMESTAMP,"
        "UNIQUE KEY uq_video_learning_user_file (user_id, file_id),"
        "INDEX idx_video_learning_user (user_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    if (!stmt.execute()) {
        LOG_WARN << "Failed to ensure video_learning_progress table: "
                 << stmt.getError();
    }
}

void ensureVideoSegmentsTable(db::MySQLPool::ConnectionGuard &mysql) {
    db::MySQLStatement stmt(
        mysql,
        "CREATE TABLE IF NOT EXISTS video_segments ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "user_id INT NOT NULL,"
        "file_id INT NOT NULL,"
        "start_time_ms INT NOT NULL,"
        "end_time_ms INT NOT NULL,"
        "tag VARCHAR(64) NOT NULL DEFAULT '',"
        "content TEXT NOT NULL,"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE "
        "CURRENT_TIMESTAMP,"
        "INDEX idx_video_segments_user_file_time "
        "(user_id, file_id, start_time_ms)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    if (!stmt.execute()) {
        LOG_WARN << "Failed to ensure video_segments table: "
                 << stmt.getError();
    }
}

void ensureStudyCollectionsTables(db::MySQLPool::ConnectionGuard &mysql) {
    db::MySQLStatement collectionStmt(
        mysql,
        "CREATE TABLE IF NOT EXISTS study_collections ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "user_id INT NOT NULL,"
        "title VARCHAR(128) NOT NULL,"
        "description TEXT NOT NULL,"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE "
        "CURRENT_TIMESTAMP,"
        "INDEX idx_study_collections_user (user_id, updated_at)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    if (!collectionStmt.execute()) {
        LOG_WARN << "Failed to ensure study_collections table: "
                 << collectionStmt.getError();
    }

    db::MySQLStatement itemStmt(
        mysql,
        "CREATE TABLE IF NOT EXISTS study_collection_items ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "collection_id INT NOT NULL,"
        "user_id INT NOT NULL,"
        "file_id INT NOT NULL,"
        "sort_order INT NOT NULL DEFAULT 0,"
        "status VARCHAR(16) NOT NULL DEFAULT 'todo',"
        "tag VARCHAR(64) NOT NULL DEFAULT '',"
        "note TEXT NOT NULL,"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE "
        "CURRENT_TIMESTAMP,"
        "UNIQUE KEY uq_study_collection_file (collection_id, file_id),"
        "INDEX idx_study_collection_items_collection "
        "(collection_id, sort_order, id),"
        "INDEX idx_study_collection_items_user (user_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    if (!itemStmt.execute()) {
        LOG_WARN << "Failed to ensure study_collection_items table: "
                 << itemStmt.getError();
    }
}

void ensureLearningTables(db::MySQLPool::ConnectionGuard &mysql) {
    ensureVideoNotesTable(mysql);
    ensureVideoLearningProgressTable(mysql);
    ensureVideoSegmentsTable(mysql);
    ensureStudyCollectionsTables(mysql);
}

bool isImageFile(const std::string &filename) {
    const std::string ext = getExtensionLower(filename);
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" ||
           ext == "webp";
}

bool isPdfFile(const std::string &filename) {
    return getExtensionLower(filename) == "pdf";
}

bool isEditableTextFile(const std::string &filename) {
    const std::string ext = getExtensionLower(filename);
    return ext == "cpp" || ext == "cc" || ext == "c" || ext == "h" ||
           ext == "hpp" || ext == "py" || ext == "js" || ext == "json" ||
           ext == "ts" || ext == "tsx" || ext == "java" || ext == "go" ||
           ext == "rs" || ext == "txt" || ext == "md";
}

int getRecycleRetentionDays(const std::string &serviceLevel) {
    if (serviceLevel == "svip") {
        return 30;
    }
    if (serviceLevel == "vip") {
        return 7;
    }
    return 2;
}

json buildWatermarkPolicy(const std::string &serviceLevel,
                          const std::string &username,
                          bool enabled = true) {
    if (!enabled) {
        return {{"mode", "none"}, {"text", ""}, {"label", "无水印"}};
    }
    if (serviceLevel == "svip") {
        return {{"mode", "dynamic"},
                {"text", username.empty() ? "SVIP" : username},
                {"label", "用户名 + 时间戳动态水印"}};
    }
    return {{"mode", "none"}, {"text", ""}, {"label", "无水印"}};
}

std::string buildNodeAccessBaseUrl(const std::shared_ptr<DataNodeInfo> &node) {
    if (node && !node->publicUrl_.empty()) {
        return node->publicUrl_;
    }
    return "http://" + node->addr_.toIpPort();
}

std::string getUsernameByUserId(db::MySQLPool::ConnectionGuard &mysql,
                                int userId) {
    db::MySQLStatement stmt(mysql,
                            "SELECT username FROM users WHERE id = ? LIMIT 1");
    stmt.bindInt(userId);
    if (!stmt.execute()) {
        return "";
    }
    auto rs = stmt.getResultSet();
    if (rs && rs->next()) {
        return rs->getString(0);
    }
    return "";
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

std::string buildPlaceholders(size_t count) {
    std::string sql;
    for (size_t i = 0; i < count; ++i) {
        sql += (i == 0 ? "?" : ",?");
    }
    return sql;
}

std::vector<int> parseIdArray(const json &value) {
    std::vector<int> ids;
    if (!value.is_array()) {
        return ids;
    }
    for (const auto &item : value) {
        if (item.is_number_integer()) {
            int id = item.get<int>();
            if (id > 0) {
                ids.push_back(id);
            }
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
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

std::string buildStoredRelativePath(const std::string &folderPath,
                                    const std::string &requestRelativePath,
                                    const std::string &originalFilename) {
    if (!requestRelativePath.empty()) {
        return requestRelativePath;
    }
    if (!folderPath.empty() && !originalFilename.empty()) {
        return folderPath + "/" + originalFilename;
    }
    return "";
}
} // namespace

FileHandler::FileHandler() {
    StorageFeatureService::instance().ensureSchema();
    SceneModeService::instance().ensureSchema();
    auto mysql = db::MySQLPool::instance().getConnection();
    if (mysql) {
        ensureLearningTables(*mysql);
    }
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
    ensureLearningTables(*mysql);
    auto &storageService = StorageFeatureService::instance();
    std::string sceneTag =
        SceneModeService::instance().getUserSceneTag(*mysql, userId);
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
        "s.extract_code, u.username, "
        "COALESCE(vlp.last_position_ms, 0), "
        "COALESCE(vlp.max_reached_ms, 0), "
        "COALESCE(vlp.duration_ms, 0) "
        "FROM files f "
        "LEFT JOIN file_shares s ON f.id = s.file_id "
        "LEFT JOIN users u ON s.target_user_id = u.id "
        "LEFT JOIN video_learning_progress vlp ON vlp.file_id = f.id "
        "AND vlp.user_id = ? "
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
    stmt.bindInt(userId); // video_learning_progress.user_id
    stmt.bindInt(userId); // files.user_id
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
        fileInfo["learningProgress"] = {
            {"lastPositionMs", rs->getInt(13)},
            {"maxReachedMs", rs->getInt(14)},
            {"durationMs", rs->getInt(15)}};

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
        const std::string username = getUsernameByUserId(*mysql, userId);
        std::string sceneTag =
            SceneModeService::instance().getUserSceneTag(*mysql, userId);
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
        std::string folderPath;
        if (finalFolderId > 0) {
            auto folderInfo =
                storageService.getFolderInfo(*mysql, userId, finalFolderId);
            folderPath = folderInfo.fullPath;
        }
        const std::string storedRelativePath =
            buildStoredRelativePath(folderPath, relativePath, originalFilename);

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
                    fastStmt.bindString(storedRelativePath);
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
                              {"relativePath", storedRelativePath}}},
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
        stmt.bindString(storedRelativePath);
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
            userId, fileId, dataNode->id_, username, originalFilename,
            serverFilename,
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
    std::string accessMode = req.getQuery("mode");
    if (accessMode != "preview" && accessMode != "edit") {
        accessMode = "download";
    }
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
    ensureLearningTables(*mysql);
    const std::string entertainmentRoomCode = req.getQuery("roomCode");
    std::string querySql =
        "SELECT f.id, f.node_id, f.original_filename, f.file_size, "
        "f.user_id, owner.service_level, owner.username, "
        "COALESCE(s.share_id, '') "
        "FROM files f "
        "JOIN users owner ON f.user_id = owner.id "
        "LEFT JOIN file_shares s ON f.id = s.file_id AND s.share_type = "
        "'specific' AND s.target_user_id = ? "
        "WHERE f.filename = ? AND f.status = 'success' "
        "AND f.is_deleted = 0 "
        "LIMIT 1";
    db::MySQLStatement stmt(*mysql, querySql);
    stmt.bindInt(userId);            // 对应 s.target_user_id = ?
    stmt.bindString(serverFilename); // 对应 f.filename = ?

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
    const int ownerUserId = rs->getInt(4);
    const std::string ownerServiceLevel = rs->getString(5);
    const std::string ownerUsername = rs->getString(6);
    const std::string directShareId = rs->getString(7);
    std::string serviceLevel =
        StorageFeatureService::instance().getUserServiceLevel(*mysql, userId);
    std::string sceneTag =
        SceneModeService::instance().getUserSceneTag(*mysql, userId);
    bool hasFileAccess = ownerUserId == userId || !directShareId.empty();
    if (!hasFileAccess && !entertainmentRoomCode.empty() &&
        (serviceLevel == "vip" || serviceLevel == "svip")) {
        // 观影房房间码是一种临时授权：只允许访问该房间片单内的文件，且房间必须未过期。
        db::MySQLStatement roomStmt(
            *mysql,
            "SELECT 1 FROM entertainment_rooms r "
            "JOIN entertainment_playlist_items i ON i.playlist_id = r.playlist_id "
            "WHERE r.room_code = ? AND r.expires_at > NOW() "
            "AND i.file_id = ? LIMIT 1");
        roomStmt.bindString(entertainmentRoomCode);
        roomStmt.bindInt(fileId);
        if (roomStmt.execute()) {
            auto roomRs = roomStmt.getResultSet();
            hasFileAccess = roomRs && roomRs->next();
        }
    }
    if (!hasFileAccess) {
        sendError(resp, "文件未找到或文件所有者已取消分享",
                  fn::HttpResponse::k404NotFound, conn);
        return true;
    }
    std::string username;
    {
        db::MySQLStatement userStmt(*mysql,
                                    "SELECT username FROM users WHERE id = ? "
                                    "LIMIT 1");
        userStmt.bindInt(userId);
        if (userStmt.execute()) {
            auto userRs = userStmt.getResultSet();
            if (userRs && userRs->next()) {
                username = userRs->getString(0);
            }
        }
    }

    const bool videoFile = isVideoFile(original_filename);
    const bool imageFile = isImageFile(original_filename);
    const bool pdfFile = isPdfFile(original_filename);
    const bool editableTextFile = isEditableTextFile(original_filename);
    const bool canPreviewVideoByLevel =
        serviceLevel == "vip" || serviceLevel == "svip";
    const bool isOwner = ownerUserId == userId;
    const bool previewWatermarkTarget = videoFile || imageFile || pdfFile;
    int progressLastPositionMs = 0;
    int progressMaxReachedMs = 0;
    int progressDurationMs = 0;
    if (videoFile) {
        db::MySQLStatement progressStmt(
            *mysql,
            "SELECT last_position_ms, max_reached_ms, duration_ms "
            "FROM video_learning_progress "
            "WHERE user_id = ? AND file_id = ? LIMIT 1");
        progressStmt.bindInt(userId);
        progressStmt.bindInt(fileId);
        if (progressStmt.execute()) {
            auto progressRs = progressStmt.getResultSet();
            if (progressRs && progressRs->next()) {
                progressLastPositionMs = progressRs->getInt(0);
                progressMaxReachedMs = progressRs->getInt(1);
                progressDurationMs = progressRs->getInt(2);
            }
        }
    }

    if (accessMode == "preview") {
        if (videoFile) {
            if (!canPreviewVideoByLevel) {
                sendError(resp, "当前用户等级不支持视频预览",
                          fn::HttpResponse::k403Forbidden, conn);
                return true;
            }
        } else if (!(imageFile || pdfFile || editableTextFile)) {
            sendError(resp, "该文件类型暂不支持在线预览",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        if (editableTextFile && sceneTag != "development") {
            sendError(resp, "当前用户不支持文本在线预览",
                      fn::HttpResponse::k403Forbidden, conn);
            return true;
        }
    } else if (accessMode == "edit") {
        if ((serviceLevel != "vip" && serviceLevel != "svip") ||
            !editableTextFile ||
            sceneTag != "development") {
            sendError(resp, "当前用户不支持在线编辑该文件",
                      fn::HttpResponse::k403Forbidden, conn);
            return true;
        }
    }

    const std::string resolvedQuality = "original";
    const bool enableWatermark =
        accessMode == "preview" && previewWatermarkTarget &&
        ownerServiceLevel == "svip";

    const std::string watermarkIdentity =
        ownerServiceLevel == "svip"
            ? (ownerUsername.empty() ? std::string("SVIP")
                                     : ownerUsername + " " +
                                           getCurrentTimeStr())
            : ownerUsername;
    json watermark = buildWatermarkPolicy(ownerServiceLevel, watermarkIdentity,
                                          enableWatermark);

    // 获取 DataNode 地址
    LOG_DEBUG << "nodeId: " << nodeId;
    auto nodeInfo = NodeManager::instance().getNodeInfo(nodeId);
    if (!nodeInfo || !nodeInfo->isAlive_) {
        sendError(resp, "存储节点离线",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
    // 每次下载记录一次
    HotCacheService::instance().recordAccess(fileId, nodeId, serverFilename,
                                             fileSize, serviceLevel, sceneTag);
    auto fileResponse = TokenManager::instance().generateDownloadToken(
        userId, username, original_filename, serverFilename, sceneTag, accessMode,
        resolvedQuality, watermark.value("mode", "none"),
        watermark.value("text", ""),
        NodeManager::instance().buildQoSPolicy(serviceLevel, true));

    // 返回 DataNode 播放地址
    json response = {{"code", 0},
                     {"data",
                      {{"downloadUrl", buildNodeAccessBaseUrl(nodeInfo) +
                                           "/api/datanode/download"},
                       {"previewUrl", buildNodeAccessBaseUrl(nodeInfo) +
                                          "/api/datanode/text_preview"},
                       {"editUrl", buildNodeAccessBaseUrl(nodeInfo) +
                                       "/api/datanode/text_edit"},
                       {"snapshotUrl", buildNodeAccessBaseUrl(nodeInfo) +
                                           "/api/datanode/text_snapshot"},
                       {"rollbackUrl", buildNodeAccessBaseUrl(nodeInfo) +
                                           "/api/datanode/text_rollback"},
                       {"experimentUrl", buildNodeAccessBaseUrl(nodeInfo) +
                                             "/api/datanode/text_experiment"},
                       {"token", fileResponse},
                       {"fileSize", fileSize}}}};
    response["data"]["accessMode"] = accessMode;
    response["data"]["videoQuality"] = resolvedQuality;
    response["data"]["availableQualities"] = json::array();
    response["data"]["serviceLevel"] = serviceLevel;
    response["data"]["watermark"] = watermark;
    response["data"]["isOwner"] = isOwner;
    response["data"]["isVideo"] = videoFile;
    response["data"]["isImage"] = imageFile;
    response["data"]["isPdf"] = pdfFile;
    response["data"]["isEditableText"] = editableTextFile;
    response["data"]["learningProgress"] = {
        {"lastPositionMs", progressLastPositionMs},
        {"maxReachedMs", progressMaxReachedMs},
        {"durationMs", progressDurationMs}};

    resp->setStatusCode(fn::HttpResponse::k200Ok);
    std::string respBody = response.dump();
    resp->setBody(respBody);
    resp->addHeader("Content-Length", std::to_string(respBody.size()));
    return true;
}

bool FileHandler::handleListVideoNotes(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kGet ||
        req.path() != "/api/video_notes") {
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

    const std::string serverFilename = urlDecode(req.getQuery("filename"));
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
    ensureVideoNotesTable(*mysql);

    const std::string serviceLevel =
        StorageFeatureService::instance().getUserServiceLevel(*mysql, userId);
    std::string sceneTag =
        SceneModeService::instance().getUserSceneTag(*mysql, userId);
    if (serviceLevel != "svip" || sceneTag != "learning") {
        sendError(resp, "视频时间戳笔记仅学习模式下的 SVIP 用户可用",
                  fn::HttpResponse::k403Forbidden, conn);
        return true;
    }

    db::MySQLStatement fileStmt(
        *mysql,
        "SELECT f.id, f.original_filename "
        "FROM files f "
        "LEFT JOIN file_shares s ON f.id = s.file_id AND s.share_type = "
        "'specific' AND s.target_user_id = ? "
        "WHERE f.filename = ? AND f.status = 'success' AND f.is_deleted = 0 "
        "AND (f.user_id = ? OR s.share_id IS NOT NULL) "
        "LIMIT 1");
    fileStmt.bindInt(userId);
    fileStmt.bindString(serverFilename);
    fileStmt.bindInt(userId);
    if (!fileStmt.execute()) {
        sendError(resp, "数据库错误", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    auto fileRs = fileStmt.getResultSet();
    if (!fileRs || !fileRs->next()) {
        sendError(resp, "文件未找到或无权访问",
                  fn::HttpResponse::k404NotFound, conn);
        return true;
    }

    const int fileId = fileRs->getInt(0);
    const std::string originalFilename = fileRs->getString(1);
    if (!isVideoFile(originalFilename)) {
        sendError(resp, "该文件不是视频文件", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    }

    db::MySQLStatement noteStmt(
        *mysql,
        "SELECT id, note_time_ms, tag, content, created_at, updated_at "
        "FROM video_notes "
        "WHERE user_id = ? AND file_id = ? "
        "ORDER BY note_time_ms ASC, id ASC");
    noteStmt.bindInt(userId);
    noteStmt.bindInt(fileId);
    if (!noteStmt.execute()) {
        sendError(resp, "查询笔记失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    json notes = json::array();
    auto noteRs = noteStmt.getResultSet();
    while (noteRs && noteRs->next()) {
        notes.push_back({{"id", noteRs->getInt(0)},
                         {"timeMs", noteRs->getInt(1)},
                         {"tag", noteRs->getString(2)},
                         {"content", noteRs->getString(3)},
                         {"createdAt", noteRs->getString(4)},
                         {"updatedAt", noteRs->getString(5)}});
    }

    sendJsonResponse(resp, {{"code", 0}, {"notes", notes}});
    return true;
}

bool FileHandler::handleCreateVideoNote(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path() != "/api/video_notes") {
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
    ensureVideoNotesTable(*mysql);

    const std::string serviceLevel =
        StorageFeatureService::instance().getUserServiceLevel(*mysql, userId);
    std::string sceneTag =
        SceneModeService::instance().getUserSceneTag(*mysql, userId);
    if (serviceLevel != "svip" || sceneTag != "learning") {
        sendError(resp, "视频时间戳笔记仅学习模式下的 SVIP 用户可用",
                  fn::HttpResponse::k403Forbidden, conn);
        return true;
    }

    try {
        const json body = json::parse(req.body());
        const std::string serverFilename =
            trimCopy(body.value("filename", ""));
        const int timeMs = body.value("timeMs", -1);
        const std::string tag = trimCopy(body.value("tag", ""));
        const std::string content = trimCopy(body.value("content", ""));

        if (serverFilename.empty() || timeMs < 0) {
            sendError(resp, "笔记参数不完整", fn::HttpResponse::k400BadRequest,
                      conn);
            return true;
        }
        if (tag.size() > 64) {
            sendError(resp, "笔记标签不能超过64个字符",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }
        if (content.empty() || content.size() > 5000) {
            sendError(resp, "笔记内容不能为空且不能超过5000字符",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        db::MySQLStatement fileStmt(
            *mysql,
            "SELECT f.id, f.original_filename "
            "FROM files f "
            "LEFT JOIN file_shares s ON f.id = s.file_id AND s.share_type = "
            "'specific' AND s.target_user_id = ? "
            "WHERE f.filename = ? AND f.status = 'success' "
            "AND f.is_deleted = 0 "
            "AND (f.user_id = ? OR s.share_id IS NOT NULL) "
            "LIMIT 1");
        fileStmt.bindInt(userId);
        fileStmt.bindString(serverFilename);
        fileStmt.bindInt(userId);
        if (!fileStmt.execute()) {
            sendError(resp, "数据库错误",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        auto fileRs = fileStmt.getResultSet();
        if (!fileRs || !fileRs->next()) {
            sendError(resp, "文件未找到或无权访问",
                      fn::HttpResponse::k404NotFound, conn);
            return true;
        }

        const int fileId = fileRs->getInt(0);
        const std::string originalFilename = fileRs->getString(1);
        if (!isVideoFile(originalFilename)) {
            sendError(resp, "该文件不是视频文件",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        // 每条笔记绑定当前用户和当前视频文件。note_time_ms 用毫秒保存，
        // 这样跳转时不会因为浮点秒数四舍五入造成明显偏移。
        db::MySQLStatement insertStmt(
            *mysql,
            "INSERT INTO video_notes "
            "(user_id, file_id, note_time_ms, tag, content) "
            "VALUES (?, ?, ?, ?, ?)");
        insertStmt.bindInt(userId);
        insertStmt.bindInt(fileId);
        insertStmt.bindInt(timeMs);
        insertStmt.bindString(tag);
        insertStmt.bindString(content);
        if (!insertStmt.execute()) {
            sendError(resp, "保存笔记失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        const int noteId = static_cast<int>(insertStmt.insertId());
        sendJsonResponse(resp,
                         {{"code", 0},
                          {"message", "笔记已保存"},
                          {"note",
                           {{"id", noteId},
                            {"timeMs", timeMs},
                            {"tag", tag},
                            {"content", content}}}});
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "保存笔记失败：" + std::string(e.what()),
                  fn::HttpResponse::k400BadRequest, conn);
        return true;
    }
}

bool FileHandler::handleListVideoSegments(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kGet ||
        req.path() != "/api/video_segments") {
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

    const std::string serverFilename = urlDecode(req.getQuery("filename"));
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
    ensureVideoSegmentsTable(*mysql);

    const std::string serviceLevel =
        StorageFeatureService::instance().getUserServiceLevel(*mysql, userId);
    std::string sceneTag =
        SceneModeService::instance().getUserSceneTag(*mysql, userId);
    if (serviceLevel != "svip" || sceneTag != "learning") {
        sendError(resp, "学习片段包仅学习模式下的 SVIP 用户可用",
                  fn::HttpResponse::k403Forbidden, conn);
        return true;
    }

    db::MySQLStatement fileStmt(
        *mysql,
        "SELECT f.id, f.original_filename "
        "FROM files f "
        "LEFT JOIN file_shares s ON f.id = s.file_id AND s.share_type = "
        "'specific' AND s.target_user_id = ? "
        "WHERE f.filename = ? AND f.status = 'success' AND f.is_deleted = 0 "
        "AND (f.user_id = ? OR s.share_id IS NOT NULL) "
        "LIMIT 1");
    fileStmt.bindInt(userId);
    fileStmt.bindString(serverFilename);
    fileStmt.bindInt(userId);
    if (!fileStmt.execute()) {
        sendError(resp, "数据库错误", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }
    auto fileRs = fileStmt.getResultSet();
    if (!fileRs || !fileRs->next()) {
        sendError(resp, "文件未找到或无权访问",
                  fn::HttpResponse::k404NotFound, conn);
        return true;
    }

    const int fileId = fileRs->getInt(0);
    const std::string originalFilename = fileRs->getString(1);
    if (!isVideoFile(originalFilename)) {
        sendError(resp, "该文件不是视频文件", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    }

    db::MySQLStatement segmentStmt(
        *mysql,
        "SELECT id, start_time_ms, end_time_ms, tag, content, created_at, "
        "updated_at FROM video_segments "
        "WHERE user_id = ? AND file_id = ? "
        "ORDER BY start_time_ms ASC, id ASC");
    segmentStmt.bindInt(userId);
    segmentStmt.bindInt(fileId);
    if (!segmentStmt.execute()) {
        sendError(resp, "查询学习片段失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    json segments = json::array();
    auto rs = segmentStmt.getResultSet();
    while (rs && rs->next()) {
        segments.push_back({{"id", rs->getInt(0)},
                            {"startMs", rs->getInt(1)},
                            {"endMs", rs->getInt(2)},
                            {"tag", rs->getString(3)},
                            {"content", rs->getString(4)},
                            {"createdAt", rs->getString(5)},
                            {"updatedAt", rs->getString(6)}});
    }

    sendJsonResponse(resp, {{"code", 0}, {"segments", segments}});
    return true;
}

bool FileHandler::handleCreateVideoSegment(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path() != "/api/video_segments") {
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
    ensureVideoSegmentsTable(*mysql);

    const std::string serviceLevel =
        StorageFeatureService::instance().getUserServiceLevel(*mysql, userId);
    std::string sceneTag =
        SceneModeService::instance().getUserSceneTag(*mysql, userId);
    if (serviceLevel != "svip" || sceneTag != "learning") {
        sendError(resp, "学习片段包仅学习模式下的 SVIP 用户可用",
                  fn::HttpResponse::k403Forbidden, conn);
        return true;
    }

    try {
        const json body = json::parse(req.body());
        const std::string serverFilename =
            trimCopy(body.value("filename", ""));
        int startMs = body.value("startMs", -1);
        int endMs = body.value("endMs", -1);
        const std::string tag = trimCopy(body.value("tag", ""));
        const std::string content = trimCopy(body.value("content", ""));

        if (serverFilename.empty() || startMs < 0 || endMs <= startMs) {
            sendError(resp, "学习片段参数不完整或时间范围非法",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }
        if (tag.size() > 64) {
            sendError(resp, "片段标签不能超过64个字符",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }
        if (content.empty() || content.size() > 5000) {
            sendError(resp, "片段说明不能为空且不能超过5000字符",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        db::MySQLStatement fileStmt(
            *mysql,
            "SELECT f.id, f.original_filename "
            "FROM files f "
            "LEFT JOIN file_shares s ON f.id = s.file_id AND s.share_type = "
            "'specific' AND s.target_user_id = ? "
            "WHERE f.filename = ? AND f.status = 'success' "
            "AND f.is_deleted = 0 "
            "AND (f.user_id = ? OR s.share_id IS NOT NULL) "
            "LIMIT 1");
        fileStmt.bindInt(userId);
        fileStmt.bindString(serverFilename);
        fileStmt.bindInt(userId);
        if (!fileStmt.execute()) {
            sendError(resp, "数据库错误",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        auto fileRs = fileStmt.getResultSet();
        if (!fileRs || !fileRs->next()) {
            sendError(resp, "文件未找到或无权访问",
                      fn::HttpResponse::k404NotFound, conn);
            return true;
        }
        const int fileId = fileRs->getInt(0);
        const std::string originalFilename = fileRs->getString(1);
        if (!isVideoFile(originalFilename)) {
            sendError(resp, "该文件不是视频文件",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        db::MySQLStatement insertStmt(
            *mysql,
            "INSERT INTO video_segments "
            "(user_id, file_id, start_time_ms, end_time_ms, tag, content) "
            "VALUES (?, ?, ?, ?, ?, ?)");
        insertStmt.bindInt(userId);
        insertStmt.bindInt(fileId);
        insertStmt.bindInt(startMs);
        insertStmt.bindInt(endMs);
        insertStmt.bindString(tag);
        insertStmt.bindString(content);
        if (!insertStmt.execute()) {
            sendError(resp, "保存学习片段失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        const int segmentId = static_cast<int>(insertStmt.insertId());
        sendJsonResponse(resp,
                         {{"code", 0},
                          {"message", "学习片段已保存"},
                          {"segment",
                           {{"id", segmentId},
                            {"startMs", startMs},
                            {"endMs", endMs},
                            {"tag", tag},
                            {"content", content}}}});
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "保存学习片段失败：" + std::string(e.what()),
                  fn::HttpResponse::k400BadRequest, conn);
        return true;
    }
}

bool FileHandler::handleDeleteVideoSegment(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kDelete ||
        req.path() != "/api/video_segments") {
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

    int segmentId = 0;
    try {
        segmentId = std::stoi(req.getQuery("id"));
    } catch (...) {
        segmentId = 0;
    }
    if (segmentId <= 0) {
        sendError(resp, "学习片段 ID 非法", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    }

    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        sendError(resp, "数据库连接失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
    ensureVideoSegmentsTable(*mysql);

    db::MySQLStatement delStmt(
        *mysql, "DELETE FROM video_segments WHERE id = ? AND user_id = ?");
    delStmt.bindInt(segmentId);
    delStmt.bindInt(userId);
    if (!delStmt.execute()) {
        sendError(resp, "删除学习片段失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
    sendJsonResponse(resp, {{"code", 0}, {"message", "学习片段已删除"}});
    return true;
}

bool FileHandler::handleListStudyCollections(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kGet ||
        req.path() != "/api/study_collections") {
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
    ensureStudyCollectionsTables(*mysql);

    const std::string serviceLevel =
        StorageFeatureService::instance().getUserServiceLevel(*mysql, userId);
    std::string sceneTag =
        SceneModeService::instance().getUserSceneTag(*mysql, userId);
    if (serviceLevel != "svip" || sceneTag != "learning") {
        sendError(resp, "学习资料专题包仅学习模式下的 SVIP 用户可用",
                  fn::HttpResponse::k403Forbidden, conn);
        return true;
    }

    db::MySQLStatement collectionStmt(
        *mysql,
        "SELECT id, title, description, created_at, updated_at "
        "FROM study_collections WHERE user_id = ? "
        "ORDER BY updated_at DESC, id DESC");
    collectionStmt.bindInt(userId);
    if (!collectionStmt.execute()) {
        sendError(resp, "查询学习专题失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    json collections = json::array();
    std::unordered_map<int, size_t> collectionIndex;
    auto collectionRs = collectionStmt.getResultSet();
    while (collectionRs && collectionRs->next()) {
        const int collectionId = collectionRs->getInt(0);
        collectionIndex[collectionId] = collections.size();
        collections.push_back({{"id", collectionId},
                               {"title", collectionRs->getString(1)},
                               {"description", collectionRs->getString(2)},
                               {"createdAt", collectionRs->getString(3)},
                               {"updatedAt", collectionRs->getString(4)},
                               {"items", json::array()}});
    }

    db::MySQLStatement itemStmt(
        *mysql,
        "SELECT i.id, i.collection_id, i.file_id, i.sort_order, i.status, "
        "i.tag, i.note, i.created_at, i.updated_at, f.filename, "
        "f.original_filename, f.file_size, COALESCE(f.relative_path, '') "
        "FROM study_collection_items i "
        "JOIN files f ON i.file_id = f.id "
        "WHERE i.user_id = ? AND f.is_deleted = 0 AND f.status = 'success' "
        "ORDER BY i.collection_id ASC, i.sort_order ASC, i.id ASC");
    itemStmt.bindInt(userId);
    if (!itemStmt.execute()) {
        sendError(resp, "查询学习专题资料失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    auto itemRs = itemStmt.getResultSet();
    while (itemRs && itemRs->next()) {
        const int collectionId = itemRs->getInt(1);
        auto it = collectionIndex.find(collectionId);
        if (it == collectionIndex.end()) {
            continue;
        }
        collections[it->second]["items"].push_back(
            {{"id", itemRs->getInt(0)},
             {"collectionId", collectionId},
             {"fileId", itemRs->getInt(2)},
             {"sortOrder", itemRs->getInt(3)},
             {"status", itemRs->getString(4)},
             {"tag", itemRs->getString(5)},
             {"note", itemRs->getString(6)},
             {"createdAt", itemRs->getString(7)},
             {"updatedAt", itemRs->getString(8)},
             {"filename", itemRs->getString(9)},
             {"originalName", itemRs->getString(10)},
             {"size", itemRs->getInt64(11)},
             {"relativePath", itemRs->getString(12)}});
    }

    sendJsonResponse(resp, {{"code", 0}, {"collections", collections}});
    return true;
}

bool FileHandler::handleStudyCollectionAction(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path() != "/api/study_collections") {
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
    ensureStudyCollectionsTables(*mysql);

    const std::string serviceLevel =
        StorageFeatureService::instance().getUserServiceLevel(*mysql, userId);
    std::string sceneTag =
        SceneModeService::instance().getUserSceneTag(*mysql, userId);
    if (serviceLevel != "svip" || sceneTag != "learning") {
        sendError(resp, "学习资料专题包仅学习模式下的 SVIP 用户可用",
                  fn::HttpResponse::k403Forbidden, conn);
        return true;
    }

    try {
        const json body = json::parse(req.body());
        const std::string action = body.value("action", "");

        if (action == "create") {
            std::string title = trimCopy(body.value("title", ""));
            std::string description = trimCopy(body.value("description", ""));
            if (title.empty() || title.size() > 128) {
                sendError(resp, "专题名称不能为空且不能超过128个字符",
                          fn::HttpResponse::k400BadRequest, conn);
                return true;
            }
            if (description.size() > 2000) {
                description = description.substr(0, 2000);
            }
            db::MySQLStatement stmt(
                *mysql,
                "INSERT INTO study_collections "
                "(user_id, title, description) VALUES (?, ?, ?)");
            stmt.bindInt(userId);
            stmt.bindString(title);
            stmt.bindString(description);
            if (!stmt.execute()) {
                sendError(resp, "创建学习专题失败",
                          fn::HttpResponse::k500InternalServerError, conn);
                return true;
            }
            sendJsonResponse(resp,
                             {{"code", 0},
                              {"message", "学习专题已创建"},
                              {"collection",
                               {{"id", static_cast<int>(stmt.insertId())},
                                {"title", title},
                                {"description", description},
                                {"items", json::array()}}}});
            return true;
        }

        const int collectionId = body.value("collectionId", 0);
        if (collectionId <= 0) {
            sendError(resp, "专题 ID 非法", fn::HttpResponse::k400BadRequest,
                      conn);
            return true;
        }

        db::MySQLStatement ownerStmt(
            *mysql,
            "SELECT id FROM study_collections WHERE id = ? AND user_id = ? "
            "LIMIT 1");
        ownerStmt.bindInt(collectionId);
        ownerStmt.bindInt(userId);
        if (!ownerStmt.execute()) {
            sendError(resp, "查询专题失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }
        auto ownerRs = ownerStmt.getResultSet();
        if (!ownerRs || !ownerRs->next()) {
            sendError(resp, "学习专题不存在或无权访问",
                      fn::HttpResponse::k404NotFound, conn);
            return true;
        }

        if (action == "delete_collection") {
            db::MySQLStatement delItems(
                *mysql,
                "DELETE FROM study_collection_items WHERE collection_id = ? "
                "AND user_id = ?");
            delItems.bindInt(collectionId);
            delItems.bindInt(userId);
            delItems.execute();

            db::MySQLStatement delCollection(
                *mysql,
                "DELETE FROM study_collections WHERE id = ? AND user_id = ?");
            delCollection.bindInt(collectionId);
            delCollection.bindInt(userId);
            if (!delCollection.execute()) {
                sendError(resp, "删除学习专题失败",
                          fn::HttpResponse::k500InternalServerError, conn);
                return true;
            }
            sendJsonResponse(resp,
                             {{"code", 0}, {"message", "学习专题已删除"}});
            return true;
        }

        if (action == "add_item") {
            const int fileId = body.value("fileId", 0);
            std::string tag = trimCopy(body.value("tag", ""));
            std::string note = trimCopy(body.value("note", ""));
            if (tag.size() > 64) {
                tag = tag.substr(0, 64);
            }
            if (note.size() > 2000) {
                note = note.substr(0, 2000);
            }

            db::MySQLStatement fileStmt(
                *mysql,
                "SELECT id FROM files WHERE id = ? AND user_id = ? "
                "AND is_deleted = 0 AND status = 'success' LIMIT 1");
            fileStmt.bindInt(fileId);
            fileStmt.bindInt(userId);
            if (!fileStmt.execute()) {
                sendError(resp, "查询文件失败",
                          fn::HttpResponse::k500InternalServerError, conn);
                return true;
            }
            auto fileRs = fileStmt.getResultSet();
            if (!fileRs || !fileRs->next()) {
                sendError(resp, "文件不存在或无权加入专题",
                          fn::HttpResponse::k404NotFound, conn);
                return true;
            }

            int nextOrder = 0;
            db::MySQLStatement orderStmt(
                *mysql,
                "SELECT COALESCE(MAX(sort_order), 0) + 1 "
                "FROM study_collection_items WHERE collection_id = ?");
            orderStmt.bindInt(collectionId);
            if (orderStmt.execute()) {
                auto orderRs = orderStmt.getResultSet();
                if (orderRs && orderRs->next()) {
                    nextOrder = orderRs->getInt(0);
                }
            }

            db::MySQLStatement insertStmt(
                *mysql,
                "INSERT INTO study_collection_items "
                "(collection_id, user_id, file_id, sort_order, tag, note) "
                "VALUES (?, ?, ?, ?, ?, ?) "
                "ON DUPLICATE KEY UPDATE tag = VALUES(tag), "
                "note = VALUES(note), updated_at = NOW()");
            insertStmt.bindInt(collectionId);
            insertStmt.bindInt(userId);
            insertStmt.bindInt(fileId);
            insertStmt.bindInt(nextOrder);
            insertStmt.bindString(tag);
            insertStmt.bindString(note);
            if (!insertStmt.execute()) {
                sendError(resp, "加入学习专题失败",
                          fn::HttpResponse::k500InternalServerError, conn);
                return true;
            }
            sendJsonResponse(resp,
                             {{"code", 0}, {"message", "资料已加入专题"}});
            return true;
        }

        const int itemId = body.value("itemId", 0);
        if (itemId <= 0) {
            sendError(resp, "专题资料 ID 非法",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        if (action == "update_item") {
            std::string status = body.value("status", "todo");
            if (status != "todo" && status != "doing" &&
                status != "done") {
                status = "todo";
            }
            std::string tag = trimCopy(body.value("tag", ""));
            std::string note = trimCopy(body.value("note", ""));
            if (tag.size() > 64) {
                tag = tag.substr(0, 64);
            }
            if (note.size() > 2000) {
                note = note.substr(0, 2000);
            }
            db::MySQLStatement updateStmt(
                *mysql,
                "UPDATE study_collection_items "
                "SET status = ?, tag = ?, note = ? "
                "WHERE id = ? AND collection_id = ? AND user_id = ?");
            updateStmt.bindString(status);
            updateStmt.bindString(tag);
            updateStmt.bindString(note);
            updateStmt.bindInt(itemId);
            updateStmt.bindInt(collectionId);
            updateStmt.bindInt(userId);
            if (!updateStmt.execute()) {
                sendError(resp, "更新专题资料失败",
                          fn::HttpResponse::k500InternalServerError, conn);
                return true;
            }
            sendJsonResponse(resp,
                             {{"code", 0}, {"message", "专题资料已更新"}});
            return true;
        }

        if (action == "remove_item") {
            db::MySQLStatement delStmt(
                *mysql,
                "DELETE FROM study_collection_items "
                "WHERE id = ? AND collection_id = ? AND user_id = ?");
            delStmt.bindInt(itemId);
            delStmt.bindInt(collectionId);
            delStmt.bindInt(userId);
            if (!delStmt.execute()) {
                sendError(resp, "移除专题资料失败",
                          fn::HttpResponse::k500InternalServerError, conn);
                return true;
            }
            sendJsonResponse(resp,
                             {{"code", 0}, {"message", "专题资料已移除"}});
            return true;
        }

        sendError(resp, "未知学习专题操作", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "学习专题操作失败：" + std::string(e.what()),
                  fn::HttpResponse::k400BadRequest, conn);
        return true;
    }
}

bool FileHandler::handleUpdateVideoProgress(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path() != "/api/video_progress") {
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
    ensureLearningTables(*mysql);

    const std::string serviceLevel =
        StorageFeatureService::instance().getUserServiceLevel(*mysql, userId);
    std::string sceneTag =
        SceneModeService::instance().getUserSceneTag(*mysql, userId);
    if ((serviceLevel != "vip" && serviceLevel != "svip") ||
        sceneTag != "learning") {
        sendError(resp, "学习进度仅学习模式下的 VIP/SVIP 用户可用",
                  fn::HttpResponse::k403Forbidden, conn);
        return true;
    }

    try {
        const json body = json::parse(req.body());
        const std::string serverFilename =
            trimCopy(body.value("filename", ""));
        int currentTimeMs = body.value("currentTimeMs", 0);
        int durationMs = body.value("durationMs", 0);
        int playedDeltaMs = body.value("playedDeltaMs", 0);

        currentTimeMs = std::max(0, currentTimeMs);
        durationMs = std::max(0, durationMs);
        playedDeltaMs = std::max(0, std::min(30000, playedDeltaMs));
        if (serverFilename.empty()) {
            sendError(resp, "文件名不能为空", fn::HttpResponse::k400BadRequest,
                      conn);
            return true;
        }
        if (durationMs > 0 && currentTimeMs > durationMs) {
            currentTimeMs = durationMs;
        }

        db::MySQLStatement fileStmt(
            *mysql,
            "SELECT f.id, f.original_filename "
            "FROM files f "
            "LEFT JOIN file_shares s ON f.id = s.file_id AND s.share_type = "
            "'specific' AND s.target_user_id = ? "
            "WHERE f.filename = ? AND f.status = 'success' "
            "AND f.is_deleted = 0 "
            "AND (f.user_id = ? OR s.share_id IS NOT NULL) "
            "LIMIT 1");
        fileStmt.bindInt(userId);
        fileStmt.bindString(serverFilename);
        fileStmt.bindInt(userId);
        if (!fileStmt.execute()) {
            sendError(resp, "数据库错误",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        auto fileRs = fileStmt.getResultSet();
        if (!fileRs || !fileRs->next()) {
            sendError(resp, "文件未找到或无权访问",
                      fn::HttpResponse::k404NotFound, conn);
            return true;
        }

        const int fileId = fileRs->getInt(0);
        const std::string originalFilename = fileRs->getString(1);
        if (!isVideoFile(originalFilename)) {
            sendError(resp, "该文件不是视频文件",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        int oldLastPositionMs = 0;
        int oldMaxReachedMs = 0;
        int oldDurationMs = 0;
        int oldWatchedMs = 0;
        bool hasProgress = false;
        db::MySQLStatement queryStmt(
            *mysql,
            "SELECT last_position_ms, max_reached_ms, duration_ms, watched_ms "
            "FROM video_learning_progress "
            "WHERE user_id = ? AND file_id = ? LIMIT 1");
        queryStmt.bindInt(userId);
        queryStmt.bindInt(fileId);
        if (queryStmt.execute()) {
            auto progressRs = queryStmt.getResultSet();
            if (progressRs && progressRs->next()) {
                hasProgress = true;
                oldLastPositionMs = progressRs->getInt(0);
                oldMaxReachedMs = progressRs->getInt(1);
                oldDurationMs = progressRs->getInt(2);
                oldWatchedMs = progressRs->getInt(3);
            }
        }

        const int mergedDurationMs = std::max(oldDurationMs, durationMs);
        const int maxAdvanceBudgetMs = playedDeltaMs * 4 + 5000;
        int newMaxReachedMs = oldMaxReachedMs;
        if (currentTimeMs > oldMaxReachedMs) {
            // 防刷进度：最远学习进度不能因为一次拖动直接跳到结尾。
            // 它最多按本次真实播放时间推进，4倍容忍用于兼容SVIP倍速播放。
            newMaxReachedMs =
                std::min(currentTimeMs, oldMaxReachedMs + maxAdvanceBudgetMs);
        }
        if (mergedDurationMs > 0) {
            newMaxReachedMs = std::min(newMaxReachedMs, mergedDurationMs);
        }
        const int newWatchedMs = oldWatchedMs + playedDeltaMs;

        if (hasProgress) {
            db::MySQLStatement updateStmt(
                *mysql,
                "UPDATE video_learning_progress "
                "SET last_position_ms = ?, max_reached_ms = ?, "
                "duration_ms = ?, watched_ms = ? "
                "WHERE user_id = ? AND file_id = ?");
            updateStmt.bindInt(currentTimeMs);
            updateStmt.bindInt(newMaxReachedMs);
            updateStmt.bindInt(mergedDurationMs);
            updateStmt.bindInt(newWatchedMs);
            updateStmt.bindInt(userId);
            updateStmt.bindInt(fileId);
            if (!updateStmt.execute()) {
                sendError(resp, "更新学习进度失败",
                          fn::HttpResponse::k500InternalServerError, conn);
                return true;
            }
        } else {
            db::MySQLStatement insertStmt(
                *mysql,
                "INSERT INTO video_learning_progress "
                "(user_id, file_id, last_position_ms, max_reached_ms, "
                "duration_ms, watched_ms) VALUES (?, ?, ?, ?, ?, ?)");
            insertStmt.bindInt(userId);
            insertStmt.bindInt(fileId);
            insertStmt.bindInt(currentTimeMs);
            insertStmt.bindInt(newMaxReachedMs);
            insertStmt.bindInt(mergedDurationMs);
            insertStmt.bindInt(newWatchedMs);
            if (!insertStmt.execute()) {
                sendError(resp, "保存学习进度失败",
                          fn::HttpResponse::k500InternalServerError, conn);
                return true;
            }
        }

        const int percent =
            mergedDurationMs > 0
                ? std::min(100, static_cast<int>(
                                    (static_cast<int64_t>(newMaxReachedMs) *
                                     100) /
                                    mergedDurationMs))
                : 0;
        sendJsonResponse(resp,
                         {{"code", 0},
                          {"progress",
                           {{"lastPositionMs", currentTimeMs},
                            {"maxReachedMs", newMaxReachedMs},
                            {"durationMs", mergedDurationMs},
                            {"percent", percent}}}});
        static_cast<void>(oldLastPositionMs);
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "更新学习进度失败：" + std::string(e.what()),
                  fn::HttpResponse::k400BadRequest, conn);
        return true;
    }
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

bool FileHandler::handleBatchDelete(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path() != "/delete_batch") {
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
        sendError(resp, "未授权的访问，请先登录",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    try {
        const json reqData = json::parse(req.body());
        const std::vector<int> fileIds = parseIdArray(reqData.value("fileIds", json::array()));
        if (fileIds.empty()) {
            sendError(resp, "请选择要删除的文件",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            sendError(resp, "数据库连接失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        std::string sql =
            "UPDATE files SET is_deleted = 1, deleted_at = NOW() "
            "WHERE user_id = ? AND is_deleted = 0 AND id IN (" +
            buildPlaceholders(fileIds.size()) + ")";
        db::MySQLStatement stmt(*mysql, sql);
        stmt.bindInt(userId);
        for (int fileId : fileIds) {
            stmt.bindInt(fileId);
        }

        if (!stmt.execute()) {
            sendError(resp, "批量删除失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        json response = {{"code", 0},
                         {"message", "批量删除成功"},
                         {"affected", stmt.affectedRows()}};
        const std::string body = response.dump();
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(body);
        resp->addHeader("Content-Length", std::to_string(body.size()));
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "批量删除失败：" + std::string(e.what()),
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
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
    const std::string serviceLevel =
        StorageFeatureService::instance().getUserServiceLevel(*mysql, userId);
    std::string sql =
        "SELECT id, original_filename, file_size, deleted_at, "
        "GREATEST(0, "
        "(CASE "
        "WHEN ? = 'svip' THEN 30 "
        "WHEN ? = 'vip' THEN 7 "
        "ELSE 2 END) - TIMESTAMPDIFF(DAY, deleted_at, NOW())) AS "
        "remaining_days "
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
    stmt.bindString(serviceLevel);
    stmt.bindString(serviceLevel);
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
                         {"deletedAt", rs->getString(3)},
                         {"remainingDays", rs->getInt(4)}});
    }

    json response = {{"code", 0},
                     {"serviceLevel", serviceLevel},
                     {"retentionDays", getRecycleRetentionDays(serviceLevel)},
                     {"data", files}};
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

bool FileHandler::handleBatchHardDelete(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path() != "/hard_delete_batch") {
        return false;
    }

    const std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.find("Bearer ") != 0) {
        sendError(resp, "未登录", fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    const int userId =
        TokenManager::instance().verifyUserToken(authHeader.substr(7));
    if (userId < 0) {
        sendError(resp, "未登录", fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    try {
        const json reqData = json::parse(req.body());
        const std::vector<int> fileIds = parseIdArray(reqData.value("fileIds", json::array()));
        if (fileIds.empty()) {
            sendError(resp, "请选择要彻底删除的文件",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            sendError(resp, "数据库连接失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        std::string updateSql =
            "UPDATE files SET is_deleted = 2 WHERE user_id = ? AND is_deleted = 1 "
            "AND id IN (" +
            buildPlaceholders(fileIds.size()) + ")";
        db::MySQLStatement updateStmt(*mysql, updateSql);
        updateStmt.bindInt(userId);
        for (int fileId : fileIds) {
            updateStmt.bindInt(fileId);
        }

        if (!updateStmt.execute()) {
            sendError(resp, "批量彻底删除失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        std::string shareSql =
            "DELETE FROM file_shares WHERE file_id IN (" +
            buildPlaceholders(fileIds.size()) + ")";
        db::MySQLStatement shareStmt(*mysql, shareSql);
        for (int fileId : fileIds) {
            shareStmt.bindInt(fileId);
        }
        shareStmt.execute();

        json response = {{"code", 0},
                         {"message", "批量彻底删除成功"},
                         {"affected", updateStmt.affectedRows()}};
        const std::string body = response.dump();
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(body);
        resp->addHeader("Content-Length", std::to_string(body.size()));
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "批量彻底删除失败：" + std::string(e.what()),
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool FileHandler::handleDeleteFolder(
    const fileserver::net::TcpConnectionPtr &conn,
    fileserver::net::HttpRequest &req,
    std::shared_ptr<fileserver::net::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kDelete || req.path() != "/folders") {
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
        sendError(resp, "未授权的访问，请先登录",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    const int folderId = parsePositiveInt(req.getQuery("folder_id"), 0);
    if (folderId <= 0) {
        sendError(resp, "目录参数无效", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    }

    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        sendError(resp, "数据库连接失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    auto &storageService = StorageFeatureService::instance();
    const auto folder = storageService.getFolderInfo(*mysql, userId, folderId);
    if (folder.id <= 0) {
        sendError(resp, "目录不存在", fn::HttpResponse::k404NotFound, conn);
        return true;
    }

    const std::string folderPrefix = folder.fullPath + "/%";

    std::vector<int> folderIds;
    {
        std::string querySql =
            "SELECT id FROM folders WHERE user_id = ? AND (id = ? OR full_path LIKE ?)";
        db::MySQLStatement queryStmt(*mysql, querySql);
        queryStmt.bindInt(userId);
        queryStmt.bindInt(folderId);
        queryStmt.bindString(folderPrefix);
        if (!queryStmt.execute()) {
            sendError(resp, "查询目录失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }
        auto rs = queryStmt.getResultSet();
        while (rs && rs->next()) {
            folderIds.push_back(rs->getInt(0));
        }
    }

    if (folderIds.empty()) {
        sendError(resp, "目录不存在", fn::HttpResponse::k404NotFound, conn);
        return true;
    }

    std::string folderPlaceholders = buildPlaceholders(folderIds.size());
    std::string fileSql =
        "UPDATE files SET is_deleted = 1, deleted_at = NOW(), folder_id = NULL "
        "WHERE user_id = ? AND is_deleted = 0 AND folder_id IN (" +
        folderPlaceholders + ")";
    db::MySQLStatement fileStmt(*mysql, fileSql);
    fileStmt.bindInt(userId);
    for (int id : folderIds) {
        fileStmt.bindInt(id);
    }
    if (!fileStmt.execute()) {
        sendError(resp, "删除目录下文件失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    std::string deleteFolderSql =
        "DELETE FROM folders WHERE user_id = ? AND id IN (" +
        folderPlaceholders + ")";
    db::MySQLStatement folderStmt(*mysql, deleteFolderSql);
    folderStmt.bindInt(userId);
    for (int id : folderIds) {
        folderStmt.bindInt(id);
    }
    if (!folderStmt.execute()) {
        sendError(resp, "删除目录失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    json response = {{"code", 0},
                     {"message", "目录删除成功"},
                     {"affectedFolders", folderStmt.affectedRows()},
                     {"affectedFiles", fileStmt.affectedRows()}};
    const std::string body = response.dump();
    resp->setStatusCode(fn::HttpResponse::k200Ok);
    resp->setContentType("application/json");
    resp->setBody(body);
    resp->addHeader("Content-Length", std::to_string(body.size()));
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
