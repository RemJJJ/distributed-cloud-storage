#include "StorageFeatureService.h"
#include "base/Logging.h"
#include "db/MySQLPool.h"
#include "db/MySQLStatement.h"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>
#include <unordered_map>

namespace {
// 普通用户:5GB
constexpr uint64_t kNormalQuotaBytes = 5ULL * 1024ULL * 1024ULL * 1024ULL;
// VIP用户:20GB
constexpr uint64_t kVipQuotaBytes = 20ULL * 1024ULL * 1024ULL * 1024ULL;
} // namespace

void StorageFeatureService::ensureSchema() {
    std::call_once(ensureSchemaOnce_, [this]() {
        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            LOG_WARN << "Skip ensuring storage schema because DB connection is "
                        "unavailable";
            return;
        }

        ensureUsersServiceLevelColumn(*mysql);
        ensureFoldersTable(*mysql);
        ensureFilesFolderColumns(*mysql);
    });
}

std::string
StorageFeatureService::normalizeServiceLevel(const std::string &serviceLevel) {
    return serviceLevel == "vip" ? "vip" : "normal";
}

uint64_t StorageFeatureService::getQuotaBytesByServiceLevel(
    const std::string &serviceLevel) const {
    return normalizeServiceLevel(serviceLevel) == "vip" ? kVipQuotaBytes
                                                        : kNormalQuotaBytes;
}

std::string StorageFeatureService::getUserServiceLevel(
    db::MySQLPool::ConnectionGuard &mysql, int userId) {
    std::string sql = "SELECT service_level FROM users WHERE id = ? LIMIT 1";
    db::MySQLStatement stmt(mysql, sql);
    stmt.bindInt(userId);
    if (!stmt.execute()) {
        LOG_WARN << "Failed to query users.service_level for user " << userId
                 << ": " << stmt.getError();
        return "normal";
    }

    auto rs = stmt.getResultSet();
    if (!rs || !rs->next()) {
        return "normal";
    }
    return normalizeServiceLevel(rs->getString(0));
}

StorageFeatureService::StorageSummary
StorageFeatureService::getStorageSummary(db::MySQLPool::ConnectionGuard &mysql,
                                         int userId) {
    StorageSummary summary;
    // VIP还是普通用户
    summary.serviceLevel = getUserServiceLevel(mysql, userId);
    summary.isVip = summary.serviceLevel == "vip";
    // 根据等级赋予总容量
    summary.quotaBytes = getQuotaBytesByServiceLevel(summary.serviceLevel);
    // 统计目前已经上传多少字节
    summary.usedBytes = queryUserUsedBytes(mysql, userId);
    // 计算剩余容量
    summary.remainingBytes = summary.usedBytes >= summary.quotaBytes
                                 ? 0
                                 : (summary.quotaBytes - summary.usedBytes);
    return summary;
}

// Master收到上传请求前调用。如果文件大小>剩余空间，拒绝
bool StorageFeatureService::canUploadFile(db::MySQLPool::ConnectionGuard &mysql,
                                          int userId, uint64_t incomingBytes,
                                          StorageSummary *summary) {
    StorageSummary current = getStorageSummary(mysql, userId);
    if (summary) {
        *summary = current;
    }
    return current.usedBytes + incomingBytes <= current.quotaBytes;
}

// 统计已用空间
uint64_t
StorageFeatureService::queryUserUsedBytes(db::MySQLPool::ConnectionGuard &mysql,
                                          int userId) {
    std::string sql = "SELECT COALESCE(SUM(file_size), 0) "
                      "FROM files WHERE user_id = ? AND is_deleted = 0 "
                      "AND status IN ('uploading', 'success')";
    db::MySQLStatement stmt(mysql, sql);
    stmt.bindInt(userId);
    if (!stmt.execute()) {
        LOG_WARN << "Failed to query user storage usage. user_id=" << userId
                 << ", error=" << stmt.getError();
        return 0;
    }
    auto rs = stmt.getResultSet();
    if (!rs || !rs->next()) {
        return 0;
    }
    return static_cast<uint64_t>(std::max<int64_t>(0, rs->getInt64(0)));
}

// 校验目录是否合法
bool StorageFeatureService::folderExists(db::MySQLPool::ConnectionGuard &mysql,
                                         int userId, int folderId) {
    if (folderId <= 0) {
        return true;
    }

    std::string sql =
        "SELECT 1 FROM folders WHERE id = ? AND user_id = ? LIMIT 1";
    db::MySQLStatement stmt(mysql, sql);
    stmt.bindInt(folderId);
    stmt.bindInt(userId);
    if (!stmt.execute()) {
        LOG_WARN << "Failed to check folder existence. folder_id=" << folderId
                 << ", error=" << stmt.getError();
        return false;
    }
    auto rs = stmt.getResultSet();
    return rs && rs->next();
}

// 查单个目录
StorageFeatureService::FolderInfo
StorageFeatureService::getFolderInfo(db::MySQLPool::ConnectionGuard &mysql,
                                     int userId, int folderId) {
    if (folderId <= 0) {
        return {};
    }

    std::string sql =
        "SELECT id, COALESCE(parent_id, 0), name, full_path, created_at "
        "FROM folders WHERE id = ? AND user_id = ? LIMIT 1";
    db::MySQLStatement stmt(mysql, sql);
    stmt.bindInt(folderId);
    stmt.bindInt(userId);
    if (!stmt.execute()) {
        LOG_WARN << "Failed to query folder info. folder_id=" << folderId
                 << ", error=" << stmt.getError();
        return {};
    }
    auto rs = stmt.getResultSet();
    if (!rs || !rs->next()) {
        return {};
    }

    FolderInfo info;
    info.id = rs->getInt(0);
    info.parentId = rs->getInt(1);
    info.name = rs->getString(2);
    info.fullPath = rs->getString(3);
    info.createdAt = rs->getString(4);
    return info;
}

std::vector<StorageFeatureService::FolderInfo>
StorageFeatureService::listFolders(db::MySQLPool::ConnectionGuard &mysql,
                                   int userId, int parentFolderId) {
    std::string sql =
        "SELECT id, COALESCE(parent_id, 0), name, full_path, created_at "
        "FROM folders WHERE user_id = ? AND ";
    if (parentFolderId > 0) {
        sql += "parent_id = ? ";
    } else {
        sql += "parent_id IS NULL ";
    }
    sql += "ORDER BY name ASC";

    db::MySQLStatement stmt(mysql, sql);
    stmt.bindInt(userId);
    if (parentFolderId > 0) {
        stmt.bindInt(parentFolderId);
    }
    std::vector<FolderInfo> folders;
    if (!stmt.execute()) {
        LOG_WARN << "Failed to list folders for user " << userId << ": "
                 << stmt.getError();
        return folders;
    }
    auto rs = stmt.getResultSet();
    while (rs && rs->next()) {
        FolderInfo folder;
        folder.id = rs->getInt(0);
        folder.parentId = rs->getInt(1);
        folder.name = rs->getString(2);
        folder.fullPath = rs->getString(3);
        folder.createdAt = rs->getString(4);
        folders.push_back(folder);
    }
    return folders;
}

std::vector<StorageFeatureService::FolderInfo>
StorageFeatureService::getFolderBreadcrumbs(
    db::MySQLPool::ConnectionGuard &mysql, int userId, int folderId) {
    std::vector<FolderInfo> breadcrumbs;
    if (folderId <= 0) {
        return breadcrumbs;
    }

    FolderInfo current = getFolderInfo(mysql, userId, folderId);
    if (current.id <= 0)
        return breadcrumbs;

    // 拆 full_path
    std::vector<std::string> paths;
    std::stringstream ss(current.fullPath);
    std::string segment, path;

    while (std::getline(ss, segment, '/')) {
        if (path.empty())
            path = segment;
        else
            path += "/" + segment;
        paths.push_back(path);
    }

    std::vector<std::string> pathHashes;
    pathHashes.reserve(paths.size());
    for (const auto &p : paths) {
        pathHashes.push_back(computePathHash(p));
    }

    // 构造IN查询
    std::string sql =
        "SELECT id, COALESCE(parent_id, 0), name, full_path, created_at FROM "
        "folders WHERE user_id = ? AND path_hash IN (";

    for (size_t i = 0; i < pathHashes.size(); i++) {
        sql += (i == 0 ? "?" : ",?");
    }
    sql += ")";

    db::MySQLStatement stmt(mysql, sql);
    stmt.bindInt(userId);
    for (const auto &hash : pathHashes) {
        stmt.bindString(hash);
    }

    if (!stmt.execute())
        return breadcrumbs;

    auto rs = stmt.getResultSet();
    std::unordered_map<std::string, FolderInfo> map;

    while (rs && rs->next()) {
        FolderInfo f;
        f.id = rs->getInt(0);
        f.parentId = rs->getInt(1);
        f.name = rs->getString(2);
        f.fullPath = rs->getString(3);
        f.createdAt = rs->getString(4);
        map[f.fullPath] = f;
    }

    // 按路径顺序组装
    for (auto &p : paths) {
        if (map.count(p)) {
            breadcrumbs.push_back(map[p]);
        }
    }
    return breadcrumbs;
}

std::string StorageFeatureService::sanitizeFolderSegment(
    const std::string &folderName) const {
    std::string result;
    result.reserve(folderName.size());
    for (char ch : folderName) {
        if (ch == '/' || ch == '\\') {
            continue;
        }
        result.push_back(ch);
    }

    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    result.erase(result.begin(),
                 std::find_if(result.begin(), result.end(),
                              [&](char c) { return !isSpace(c); }));
    result.erase(
        std::find_if(
            result.rbegin(), result.rend(),
            [&](char c) { return !isSpace(static_cast<unsigned char>(c)); })
            .base(),
        result.end());

    if (result.empty() || result == "." || result == "..") {
        return "";
    }
    return result;
}

std::string
StorageFeatureService::buildChildPath(const std::string &parentPath,
                                      const std::string &folderName) const {
    if (parentPath.empty()) {
        return folderName;
    }
    return parentPath + "/" + folderName;
}

std::string StorageFeatureService::computePathHash(
    const std::string &path) const {
    unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
    SHA256(reinterpret_cast<const unsigned char *>(path.data()), path.size(),
           digest);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char byte : digest) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

int StorageFeatureService::ensureFolderPath(
    db::MySQLPool::ConnectionGuard &mysql, int userId, int baseFolderId,
    const std::vector<std::string> &segments) {

    int currentFolderId = baseFolderId;
    std::string currentPath;

    // 处理 baseFolder
    if (baseFolderId > 0) {
        FolderInfo baseFolder = getFolderInfo(mysql, userId, baseFolderId);
        if (baseFolder.id <= 0) {
            return 0;
        }
        currentPath = baseFolder.fullPath;
    }

    // 遍历路径
    for (const auto &rawSegment : segments) {
        std::string segment = sanitizeFolderSegment(rawSegment);
        if (segment.empty()) {
            continue;
        }

        std::string targetPath = buildChildPath(currentPath, segment);
        std::string targetPathHash = computePathHash(targetPath);

        // 🚀 核心：INSERT + ON DUPLICATE KEY
        std::string sql =
            "INSERT INTO folders (user_id, parent_id, name, full_path, "
            "path_hash, "
            "created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?, NOW(), NOW()) "
            "ON DUPLICATE KEY UPDATE id = LAST_INSERT_ID(id)";

        db::MySQLStatement stmt(mysql, sql);
        stmt.bindInt(userId);

        if (currentFolderId > 0) {
            stmt.bindInt(currentFolderId);
        } else {
            stmt.bindNull();
        }

        stmt.bindString(segment);
        stmt.bindString(targetPath);
        stmt.bindString(targetPathHash);

        if (!stmt.execute()) {
            LOG_WARN << "Failed to ensure folder path segment. user_id="
                     << userId << ", path=" << targetPath
                     << ", error=" << stmt.getError();
            return 0;
        }

        // 关键：不管是 INSERT 还是已存在，都能拿到 id
        currentFolderId = static_cast<int>(stmt.insertId());
        currentPath = targetPath;
    }

    return currentFolderId;
}

int StorageFeatureService::ensureFolderPathFromString(
    db::MySQLPool::ConnectionGuard &mysql, int userId, int baseFolderId,
    const std::string &folderPath) {
    if (folderPath.empty()) {
        return baseFolderId;
    }

    std::vector<std::string> segments;
    std::stringstream ss(folderPath);
    std::string item;
    while (std::getline(ss, item, '/')) {
        if (!item.empty()) {
            segments.push_back(item);
        }
    }
    return ensureFolderPath(mysql, userId, baseFolderId, segments);
}

void StorageFeatureService::ensureUsersServiceLevelColumn(
    db::MySQLPool::ConnectionGuard &mysql) {
    std::string checkSql = "SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS "
                           "WHERE TABLE_SCHEMA = DATABASE() "
                           "AND TABLE_NAME = 'users' "
                           "AND COLUMN_NAME = 'service_level' LIMIT 1";
    db::MySQLStatement checkStmt(mysql, checkSql);
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
    db::MySQLStatement alterStmt(mysql, alterSql);
    if (!alterStmt.execute()) {
        LOG_WARN << "Failed to add users.service_level: "
                 << alterStmt.getError();
        return;
    }
}

void StorageFeatureService::ensureFoldersTable(
    db::MySQLPool::ConnectionGuard &mysql) {
    std::string sql = "CREATE TABLE IF NOT EXISTS folders ("
                      "id INT AUTO_INCREMENT PRIMARY KEY,"
                      "user_id INT NOT NULL,"
                      "parent_id INT NULL,"
                      "name VARCHAR(255) NOT NULL,"
                      "full_path VARCHAR(1024) NOT NULL,"
                      "path_hash CHAR(64) NOT NULL,"
                      "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                      "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP "
                      "ON UPDATE CURRENT_TIMESTAMP,"
                      "UNIQUE KEY uniq_user_path_hash (user_id, path_hash),"
                      "KEY idx_user_parent (user_id, parent_id)"
                      ")";
    db::MySQLStatement stmt(mysql, sql);
    if (!stmt.execute()) {
        LOG_WARN << "Failed to ensure folders table: " << stmt.getError();
    }

    {
        std::string checkSql = "SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS "
                               "WHERE TABLE_SCHEMA = DATABASE() "
                               "AND TABLE_NAME = 'folders' "
                               "AND COLUMN_NAME = 'path_hash' LIMIT 1";
        db::MySQLStatement checkStmt(mysql, checkSql);
        if (checkStmt.execute()) {
            auto rs = checkStmt.getResultSet();
            if (!(rs && rs->next())) {
                db::MySQLStatement alterStmt(
                    mysql,
                    "ALTER TABLE folders ADD COLUMN path_hash CHAR(64) NOT NULL "
                    "DEFAULT '' AFTER full_path");
                if (!alterStmt.execute()) {
                    LOG_WARN << "Failed to add folders.path_hash: "
                             << alterStmt.getError();
                }
            }
        }
    }

    {
        db::MySQLStatement backfillStmt(
            mysql,
            "UPDATE folders SET path_hash = SHA2(full_path, 256) "
            "WHERE path_hash = '' OR path_hash IS NULL");
        if (!backfillStmt.execute()) {
            LOG_WARN << "Failed to backfill folders.path_hash: "
                     << backfillStmt.getError();
        }
    }

    {
        std::string checkSql = "SELECT 1 FROM INFORMATION_SCHEMA.STATISTICS "
                               "WHERE TABLE_SCHEMA = DATABASE() "
                               "AND TABLE_NAME = 'folders' "
                               "AND INDEX_NAME = 'uniq_user_path' LIMIT 1";
        db::MySQLStatement checkStmt(mysql, checkSql);
        if (checkStmt.execute()) {
            auto rs = checkStmt.getResultSet();
            if (rs && rs->next()) {
                db::MySQLStatement dropStmt(
                    mysql, "ALTER TABLE folders DROP INDEX uniq_user_path");
                if (!dropStmt.execute()) {
                    LOG_WARN << "Failed to drop old folders.uniq_user_path: "
                             << dropStmt.getError();
                }
            }
        }
    }

    {
        std::string checkSql = "SELECT 1 FROM INFORMATION_SCHEMA.STATISTICS "
                               "WHERE TABLE_SCHEMA = DATABASE() "
                               "AND TABLE_NAME = 'folders' "
                               "AND INDEX_NAME = 'uniq_user_path_hash' LIMIT 1";
        db::MySQLStatement checkStmt(mysql, checkSql);
        if (checkStmt.execute()) {
            auto rs = checkStmt.getResultSet();
            if (!(rs && rs->next())) {
                db::MySQLStatement createStmt(
                    mysql,
                    "ALTER TABLE folders ADD UNIQUE KEY uniq_user_path_hash "
                    "(user_id, path_hash)");
                if (!createStmt.execute()) {
                    LOG_WARN << "Failed to add folders.uniq_user_path_hash: "
                             << createStmt.getError();
                }
            }
        }
    }
}

void StorageFeatureService::ensureFilesFolderColumns(
    db::MySQLPool::ConnectionGuard &mysql) {
    struct ColumnSpec {
        const char *name;
        const char *alterSql;
    };

    const ColumnSpec specs[] = {
        {"folder_id", "ALTER TABLE files ADD COLUMN folder_id INT NULL"},
        {"relative_path",
         "ALTER TABLE files ADD COLUMN relative_path VARCHAR(1024) NOT NULL "
         "DEFAULT ''"}};

    for (const auto &spec : specs) {
        std::string checkSql = "SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS "
                               "WHERE TABLE_SCHEMA = DATABASE() "
                               "AND TABLE_NAME = 'files' AND COLUMN_NAME = '" +
                               std::string(spec.name) + "' LIMIT 1";
        db::MySQLStatement checkStmt(mysql, checkSql);
        if (!checkStmt.execute()) {
            LOG_WARN << "Failed to inspect files." << spec.name << ": "
                     << checkStmt.getError();
            continue;
        }
        auto rs = checkStmt.getResultSet();
        if (rs && rs->next()) {
            continue;
        }

        db::MySQLStatement alterStmt(mysql, spec.alterSql);
        if (!alterStmt.execute()) {
            LOG_WARN << "Failed to add files." << spec.name << ": "
                     << alterStmt.getError();
            continue;
        }
    }
}
