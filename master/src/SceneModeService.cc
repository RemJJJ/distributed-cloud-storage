#include "SceneModeService.h"
#include "base/Logging.h"
#include "db/MySQLStatement.h"

namespace {
std::string normalizeSceneTag(const std::string &sceneTag) {
    if (sceneTag == "learning" || sceneTag == "development" ||
        sceneTag == "entertainment") {
        return sceneTag;
    }
    return "general";
}
} // namespace

void SceneModeService::ensureSchema() {
    std::call_once(ensureSchemaOnce_, [this]() {
        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            LOG_WARN << "Skip ensuring users.scene_tag because DB connection is "
                        "unavailable";
            return;
        }
        ensureUserSceneTagColumn(*mysql);
    });
}

std::string SceneModeService::getUserSceneTag(
    db::MySQLPool::ConnectionGuard &mysql, int userId) {
    db::MySQLStatement stmt(
        mysql, "SELECT scene_tag FROM users WHERE id = ? LIMIT 1");
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

bool SceneModeService::updateUserSceneTag(
    db::MySQLPool::ConnectionGuard &mysql, int userId,
    const std::string &sceneTag) {
    db::MySQLStatement stmt(mysql,
                            "UPDATE users SET scene_tag = ? WHERE id = ?");
    stmt.bindString(normalizeSceneTag(sceneTag));
    stmt.bindInt(userId);
    if (!stmt.execute()) {
        LOG_WARN << "Failed to update users.scene_tag for user " << userId
                 << ": " << stmt.getError();
        return false;
    }
    return true;
}

void SceneModeService::ensureUserSceneTagColumn(
    db::MySQLPool::ConnectionGuard &mysql) {
    db::MySQLStatement checkStmt(
        mysql,
        "SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS "
        "WHERE TABLE_SCHEMA = DATABASE() "
        "AND TABLE_NAME = 'users' "
        "AND COLUMN_NAME = 'scene_tag' LIMIT 1");
    if (!checkStmt.execute()) {
        LOG_WARN << "Failed to inspect users.scene_tag: "
                 << checkStmt.getError();
        return;
    }

    auto rs = checkStmt.getResultSet();
    if (rs && rs->next()) {
        return;
    }

    db::MySQLStatement alterStmt(
        mysql,
        "ALTER TABLE users ADD COLUMN scene_tag "
        "VARCHAR(32) NOT NULL DEFAULT 'general'");
    if (!alterStmt.execute()) {
        LOG_WARN << "Failed to add users.scene_tag: "
                 << alterStmt.getError();
        return;
    }

    LOG_INFO << "Added users.scene_tag column with default general";
}
