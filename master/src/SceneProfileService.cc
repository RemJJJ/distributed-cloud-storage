#include "SceneProfileService.h"
#include "base/Logging.h"
#include "base/ThreadPool.h"
#include "db/MySQLStatement.h"
#include <algorithm>
#include <exception>
#include <thread>

namespace {
std::string normalizeSceneTag(const std::string &sceneTag) {
    if (sceneTag == "learning" || sceneTag == "development") {
        return sceneTag;
    }
    return "general";
}
} // namespace

void SceneProfileService::ensureSchema() {
    std::call_once(ensureSchemaOnce_, [this]() {
        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            LOG_WARN << "Skip ensuring scene schema because DB connection is "
                        "unavailable";
            return;
        }

        ensureUserSceneTagColumn(*mysql);
        ensureFileDownloadCountColumn(*mysql);
    });
}

std::string SceneProfileService::calculateUserScene(int userId) {
    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        LOG_WARN << "Failed to calculate user scene because DB connection is "
                    "unavailable. user_id="
                 << userId;
        return "general";
    }

    SceneStats stats = queryUserStats(*mysql, userId);
    std::string sceneTag = "general";

    if (stats.totalFiles > 0) {
        double score = 0.0;

        // 后缀分：学习类多则偏 learning，开发类多则偏 development。
        score += (static_cast<double>(stats.learningFiles - stats.devFiles) /
                  static_cast<double>(stats.totalFiles)) *
                 40.0;

        // 大文件更偏学习场景，小文件更偏开发场景。
        if (stats.avgSize > 50.0 * 1024.0 * 1024.0) {
            score += 30.0;
        } else if (stats.avgSize < 5.0 * 1024.0 * 1024.0) {
            score -= 30.0;
        }

        // 下载频率高说明更像课程资料/视频反复观看。
        double rwRatio =
            static_cast<double>(stats.totalDownloads) / stats.totalFiles;
        if (rwRatio > 3.0) {
            score += 30.0;
        } else if (rwRatio < 1.0) {
            score -= 30.0;
        }

        score = std::max(-100.0, std::min(100.0, score));
        if (score > 30.0) {
            sceneTag = "learning";
        } else if (score < -30.0) {
            sceneTag = "development";
        }

        LOG_INFO << "Calculated user scene. user_id=" << userId
                 << ", total_files=" << stats.totalFiles
                 << ", learning_files=" << stats.learningFiles
                 << ", dev_files=" << stats.devFiles
                 << ", avg_size=" << stats.avgSize
                 << ", total_downloads=" << stats.totalDownloads
                 << ", scene_tag=" << sceneTag;
    }

    updateUserSceneTag(*mysql, userId, sceneTag);
    return sceneTag;
}

void SceneProfileService::refreshUserSceneAsync(
    int userId, fileserver::ThreadPool *threadPool) {
    if (!threadPool) {
        LOG_WARN << "ThreadPool is null, fallback to sync calculation";
        calculateUserScene(userId); // 如果没传线程池，降级为同步计算
        return;
    }

    // 丢给master线程池
    threadPool->run([userId]() {
        try {
            SceneProfileService::instance().calculateUserScene(userId);
        } catch (const std::exception &e) {
            LOG_WARN << "Async refresh user scene failed. user_id=" << userId
                     << ", error=" << e.what();
        }
    });
}

std::string
SceneProfileService::getUserSceneTag(db::MySQLPool::ConnectionGuard &mysql,
                                     int userId) {
    std::string sql = "SELECT scene_tag FROM users WHERE id = ? LIMIT 1";
    db::MySQLStatement stmt(mysql, sql);
    stmt.bindInt(userId);
    if (!stmt.execute()) {
        LOG_WARN << "Failed to query users.scene_tag for user " << userId
                 << ": " << stmt.getError();
        return "general";
    }

    auto rs = stmt.getResultSet();
    if (!rs || !rs->next()) {
        return "general";
    }
    return normalizeSceneTag(rs->getString(0));
}

void SceneProfileService::incrementDownloadCount(
    db::MySQLPool::ConnectionGuard &mysql, int fileId) {
    std::string sql =
        "UPDATE files SET download_count = download_count + 1 WHERE id = ?";
    db::MySQLStatement stmt(mysql, sql);
    stmt.bindInt(fileId);
    if (!stmt.execute()) {
        LOG_WARN << "Failed to increment files.download_count for file_id="
                 << fileId << ": " << stmt.getError();
    }
}

void SceneProfileService::ensureUserSceneTagColumn(
    db::MySQLPool::ConnectionGuard &mysql) {
    std::string checkSql = "SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS "
                           "WHERE TABLE_SCHEMA = DATABASE() "
                           "AND TABLE_NAME = 'users' "
                           "AND COLUMN_NAME = 'scene_tag' LIMIT 1";
    db::MySQLStatement checkStmt(mysql, checkSql);
    if (!checkStmt.execute()) {
        LOG_WARN << "Failed to inspect users.scene_tag: "
                 << checkStmt.getError();
        return;
    }

    auto rs = checkStmt.getResultSet();
    if (rs && rs->next()) {
        return;
    }

    std::string alterSql = "ALTER TABLE users ADD COLUMN scene_tag "
                           "VARCHAR(32) NOT NULL DEFAULT "
                           "'general'";
    db::MySQLStatement alterStmt(mysql, alterSql);
    if (!alterStmt.execute()) {
        LOG_WARN << "Failed to add users.scene_tag: " << alterStmt.getError();
        return;
    }

    LOG_INFO << "Added users.scene_tag column with default general";
}

void SceneProfileService::ensureFileDownloadCountColumn(
    db::MySQLPool::ConnectionGuard &mysql) {
    std::string checkSql = "SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS "
                           "WHERE TABLE_SCHEMA = DATABASE() "
                           "AND TABLE_NAME = 'files' "
                           "AND COLUMN_NAME = 'download_count' LIMIT 1";
    db::MySQLStatement checkStmt(mysql, checkSql);
    if (!checkStmt.execute()) {
        LOG_WARN << "Failed to inspect files.download_count: "
                 << checkStmt.getError();
        return;
    }

    auto rs = checkStmt.getResultSet();
    if (rs && rs->next()) {
        return;
    }

    std::string alterSql = "ALTER TABLE files ADD COLUMN download_count "
                           "INT NOT NULL DEFAULT 0";
    db::MySQLStatement alterStmt(mysql, alterSql);
    if (!alterStmt.execute()) {
        LOG_WARN << "Failed to add files.download_count: "
                 << alterStmt.getError();
        return;
    }

    LOG_INFO << "Added files.download_count column with default 0";
}

void SceneProfileService::updateUserSceneTag(
    db::MySQLPool::ConnectionGuard &mysql, int userId,
    const std::string &sceneTag) {
    std::string sql = "UPDATE users SET scene_tag = ? WHERE id = ?";
    db::MySQLStatement stmt(mysql, sql);
    stmt.bindString(normalizeSceneTag(sceneTag));
    stmt.bindInt(userId);
    if (!stmt.execute()) {
        LOG_WARN << "Failed to update users.scene_tag for user " << userId
                 << ": " << stmt.getError();
    }
}

SceneProfileService::SceneStats
SceneProfileService::queryUserStats(db::MySQLPool::ConnectionGuard &mysql,
                                    int userId) {
    std::string sql =
        "SELECT "
        "COUNT(*) AS total_files, "
        // 统计学习类文件数量
        "SUM(CASE WHEN LOWER(SUBSTRING_INDEX(original_filename, '.', -1)) "
        "IN ('mp4', 'pdf', 'ppt') THEN 1 ELSE 0 END) AS learning_files, "
        // 统计开发类文件数量
        "SUM(CASE WHEN LOWER(SUBSTRING_INDEX(original_filename, '.', -1)) "
        "IN ('cpp', 'py', 'js', 'json', 'h') THEN 1 ELSE 0 END) AS "
        "dev_files, "
        // 计算平均大小
        "COALESCE(AVG(file_size), 0) AS avg_size, "
        // 计算总下载次数
        "COALESCE(SUM(download_count), 0) AS total_downloads "
        "FROM files "
        "WHERE user_id = ? AND is_deleted = 0 AND status = 'success'";

    db::MySQLStatement stmt(mysql, sql);
    stmt.bindInt(userId);
    if (!stmt.execute()) {
        LOG_WARN << "Failed to query user scene stats for user " << userId
                 << ": " << stmt.getError();
        return {};
    }

    auto rs = stmt.getResultSet();
    if (!rs || !rs->next()) {
        return {};
    }

    SceneStats stats;
    stats.totalFiles = rs->getInt(0);
    stats.learningFiles = rs->getInt(1);
    stats.devFiles = rs->getInt(2);
    stats.avgSize = rs->getDouble(3);
    stats.totalDownloads = rs->getInt64(4);
    return stats;
}
