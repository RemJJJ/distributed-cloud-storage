#include "EntertainmentHandler.h"
#include "TokenManager.h"
#include "base/Logging.h"
#include "db/MySQLStatement.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <random>
#include <unordered_map>

using json = nlohmann::json;

namespace {
std::string normalizeServiceLevel(const std::string &serviceLevel) {
    if (serviceLevel == "svip" || serviceLevel == "vip") {
        return serviceLevel;
    }
    return "normal";
}

std::string lowerExt(const std::string &filename) {
    const auto pos = filename.find_last_of('.');
    if (pos == std::string::npos || pos + 1 >= filename.size()) {
        return "";
    }
    std::string ext = filename.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

bool isEntertainmentMedia(const std::string &filename) {
    const std::string ext = lowerExt(filename);
    return ext == "mp4" || ext == "avi" || ext == "mkv" || ext == "mov" ||
           ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" ||
           ext == "webp" || ext == "pdf" || ext == "mp3" || ext == "wav" ||
           ext == "flac" || ext == "aac" || ext == "ogg";
}

bool isVideoMedia(const std::string &filename) {
    const std::string ext = lowerExt(filename);
    return ext == "mp4" || ext == "avi" || ext == "mkv" || ext == "mov";
}

bool isImageMedia(const std::string &filename) {
    const std::string ext = lowerExt(filename);
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" ||
           ext == "webp";
}

std::string normalizePlaylistType(const std::string &type) {
    return type == "image" ? "image" : "video";
}

std::string clampText(std::string value, size_t maxLen) {
    if (value.size() > maxLen) {
        value.resize(maxLen);
    }
    return value;
}
} // namespace

EntertainmentHandler::EntertainmentHandler() { ensureSchema(); }

void EntertainmentHandler::sendJson(const std::shared_ptr<fn::HttpResponse> &resp,
                                    const json &payload,
                                    fn::HttpResponse::HttpStatusCode status) {
    const std::string body = payload.dump();
    resp->setStatusCode(status);
    resp->setContentType("application/json");
    resp->setBody(body);
    resp->addHeader("Content-Length", std::to_string(body.size()));
}

bool EntertainmentHandler::isVipLike(const std::string &serviceLevel) const {
    return serviceLevel == "vip" || serviceLevel == "svip";
}

bool EntertainmentHandler::isSvip(const std::string &serviceLevel) const {
    return serviceLevel == "svip";
}

bool EntertainmentHandler::verifyUser(
    const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
    const std::shared_ptr<fn::HttpResponse> &resp, UserContext &ctx) {
    const std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.find("Bearer ") != 0) {
        sendError(resp, "未授权的访问，请先登录", fn::HttpResponse::k401Unauthorized,
                  conn);
        return false;
    }

    const int userId =
        TokenManager::instance().verifyUserToken(authHeader.substr(7));
    if (userId < 0) {
        sendError(resp, "Token 无效或已过期，请重新登录",
                  fn::HttpResponse::k401Unauthorized, conn);
        return false;
    }

    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        sendError(resp, "数据库连接失败", fn::HttpResponse::k500InternalServerError,
                  conn);
        return false;
    }
    db::MySQLStatement stmt(*mysql,
                            "SELECT service_level FROM users WHERE id = ? "
                            "LIMIT 1");
    stmt.bindInt(userId);
    if (!stmt.execute()) {
        sendError(resp, "查询用户等级失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return false;
    }
    auto rs = stmt.getResultSet();
    ctx.userId = userId;
    ctx.serviceLevel = "normal";
    if (rs && rs->next()) {
        ctx.serviceLevel = normalizeServiceLevel(rs->getString(0));
    }
    return true;
}

void EntertainmentHandler::ensureSchema() {
    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        LOG_WARN << "Skip ensuring entertainment schema because DB connection "
                    "is unavailable";
        return;
    }

    const char *playlistSql =
        "CREATE TABLE IF NOT EXISTS entertainment_playlists ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "user_id INT NOT NULL,"
        "title VARCHAR(128) NOT NULL,"
        "description TEXT NOT NULL,"
        "playlist_type VARCHAR(16) NOT NULL DEFAULT 'video',"
        "theme VARCHAR(32) NOT NULL DEFAULT 'classic',"
        "cover_file_id INT NOT NULL DEFAULT 0,"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE "
        "CURRENT_TIMESTAMP,"
        "INDEX idx_entertainment_playlists_user (user_id, updated_at)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    db::MySQLStatement playlistStmt(*mysql, playlistSql);
    if (!playlistStmt.execute()) {
        LOG_WARN << "Failed to ensure entertainment_playlists: "
                 << playlistStmt.getError();
    }
    db::MySQLStatement playlistTypeColumnStmt(
        *mysql,
        "ALTER TABLE entertainment_playlists ADD COLUMN playlist_type "
        "VARCHAR(16) NOT NULL DEFAULT 'video'");
    playlistTypeColumnStmt.execute();
    db::MySQLStatement inferImagePlaylistStmt(
        *mysql,
        "UPDATE entertainment_playlists p SET playlist_type = 'image' "
        "WHERE playlist_type = 'video' "
        "AND EXISTS (SELECT 1 FROM entertainment_playlist_items i "
        "JOIN files f ON i.file_id = f.id WHERE i.playlist_id = p.id "
        "AND LOWER(f.original_filename) REGEXP '\\\\.(jpg|jpeg|png|gif|webp)$') "
        "AND NOT EXISTS (SELECT 1 FROM entertainment_playlist_items i "
        "JOIN files f ON i.file_id = f.id WHERE i.playlist_id = p.id "
        "AND LOWER(f.original_filename) NOT REGEXP '\\\\.(jpg|jpeg|png|gif|webp)$')");
    inferImagePlaylistStmt.execute();

    const char *itemSql =
        "CREATE TABLE IF NOT EXISTS entertainment_playlist_items ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "playlist_id INT NOT NULL,"
        "user_id INT NOT NULL,"
        "file_id INT NOT NULL,"
        "sort_order INT NOT NULL DEFAULT 0,"
        "note TEXT NOT NULL,"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE "
        "CURRENT_TIMESTAMP,"
        "UNIQUE KEY uq_entertainment_playlist_file (playlist_id, file_id),"
        "INDEX idx_entertainment_items_playlist (playlist_id, sort_order, id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    db::MySQLStatement itemStmt(*mysql, itemSql);
    if (!itemStmt.execute()) {
        LOG_WARN << "Failed to ensure entertainment_playlist_items: "
                 << itemStmt.getError();
    }

    const char *danmakuSql =
        "CREATE TABLE IF NOT EXISTS entertainment_danmaku ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "user_id INT NOT NULL,"
        "file_id INT NOT NULL,"
        "time_ms INT NOT NULL,"
        "text VARCHAR(300) NOT NULL,"
        "color VARCHAR(32) NOT NULL DEFAULT '#ffffff',"
        "is_shared TINYINT NOT NULL DEFAULT 0,"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "INDEX idx_entertainment_danmaku_file_time (file_id, time_ms)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    db::MySQLStatement danmakuStmt(*mysql, danmakuSql);
    if (!danmakuStmt.execute()) {
        LOG_WARN << "Failed to ensure entertainment_danmaku: "
                 << danmakuStmt.getError();
    }

    const char *roomSql =
        "CREATE TABLE IF NOT EXISTS entertainment_rooms ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "room_code VARCHAR(16) NOT NULL UNIQUE,"
        "host_user_id INT NOT NULL,"
        "playlist_id INT NOT NULL,"
        "current_item_id INT NOT NULL DEFAULT 0,"
        "current_time_ms INT NOT NULL DEFAULT 0,"
        "playing TINYINT NOT NULL DEFAULT 0,"
        "playback_rate DOUBLE NOT NULL DEFAULT 1.0,"
        "danmaku_enabled TINYINT NOT NULL DEFAULT 1,"
        "expires_at DATETIME NOT NULL,"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE "
        "CURRENT_TIMESTAMP,"
        "INDEX idx_entertainment_rooms_host (host_user_id, updated_at)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    db::MySQLStatement roomStmt(*mysql, roomSql);
    if (!roomStmt.execute()) {
        LOG_WARN << "Failed to ensure entertainment_rooms: "
                 << roomStmt.getError();
    }
    db::MySQLStatement playbackRateColumnStmt(
        *mysql,
        "ALTER TABLE entertainment_rooms ADD COLUMN playback_rate DOUBLE "
        "NOT NULL DEFAULT 1.0");
    playbackRateColumnStmt.execute();
    db::MySQLStatement danmakuEnabledColumnStmt(
        *mysql,
        "ALTER TABLE entertainment_rooms ADD COLUMN danmaku_enabled TINYINT "
        "NOT NULL DEFAULT 1");
    danmakuEnabledColumnStmt.execute();
}

bool EntertainmentHandler::fileAccessible(db::MySQLPool::ConnectionGuard &mysql,
                                          int userId, int fileId,
                                          json *fileInfo,
                                          const std::string &roomCode) {
    db::MySQLStatement stmt(
        mysql,
        "SELECT f.id, f.filename, f.original_filename, f.file_size, "
        "COALESCE(f.relative_path, ''), f.user_id, COALESCE(s.share_id, '') "
        "FROM files f "
        "LEFT JOIN file_shares s ON f.id = s.file_id AND s.share_type = "
        "'specific' AND s.target_user_id = ? "
        "WHERE f.id = ? AND f.status = 'success' AND f.is_deleted = 0 "
        "LIMIT 1");
    stmt.bindInt(userId);
    stmt.bindInt(fileId);
    if (!stmt.execute()) {
        return false;
    }
    auto rs = stmt.getResultSet();
    if (!rs || !rs->next()) {
        return false;
    }
    bool accessible = rs->getInt(5) == userId || !rs->getString(6).empty();
    if (!accessible && !roomCode.empty()) {
        // 观影房授权：房间码有效且文件属于该房间绑定片单即可访问。
        db::MySQLStatement roomStmt(
            mysql,
            "SELECT 1 FROM entertainment_rooms r "
            "JOIN entertainment_playlist_items i ON i.playlist_id = r.playlist_id "
            "WHERE r.room_code = ? AND r.expires_at > NOW() "
            "AND i.file_id = ? LIMIT 1");
        roomStmt.bindString(roomCode);
        roomStmt.bindInt(fileId);
        if (roomStmt.execute()) {
            auto roomRs = roomStmt.getResultSet();
            accessible = roomRs && roomRs->next();
        }
    }
    if (!accessible) {
        return false;
    }
    if (fileInfo) {
        *fileInfo = {{"fileId", rs->getInt(0)},
                     {"filename", rs->getString(1)},
                     {"originalName", rs->getString(2)},
                     {"size", rs->getInt64(3)},
                     {"relativePath", rs->getString(4)},
                     {"mediaType", lowerExt(rs->getString(2))}};
    }
    return true;
}

bool EntertainmentHandler::playlistOwned(db::MySQLPool::ConnectionGuard &mysql,
                                         int userId, int playlistId) {
    db::MySQLStatement stmt(
        mysql,
        "SELECT id FROM entertainment_playlists WHERE id = ? AND user_id = ? "
        "LIMIT 1");
    stmt.bindInt(playlistId);
    stmt.bindInt(userId);
    if (!stmt.execute()) {
        return false;
    }
    auto rs = stmt.getResultSet();
    return rs && rs->next();
}

json EntertainmentHandler::listPlaylists(db::MySQLPool::ConnectionGuard &mysql,
                                         int userId) {
    json playlists = json::array();
    std::unordered_map<int, size_t> index;

    db::MySQLStatement playlistStmt(
        mysql,
        "SELECT id, title, description, theme, cover_file_id, "
        "COALESCE(playlist_type, 'video'), created_at, updated_at "
        "FROM entertainment_playlists WHERE user_id = ? "
        "ORDER BY updated_at DESC, id DESC");
    playlistStmt.bindInt(userId);
    if (!playlistStmt.execute()) {
        return playlists;
    }

    auto prs = playlistStmt.getResultSet();
    while (prs && prs->next()) {
        const int playlistId = prs->getInt(0);
        index[playlistId] = playlists.size();
        playlists.push_back({{"id", playlistId},
                             {"title", prs->getString(1)},
                             {"description", prs->getString(2)},
                             {"theme", prs->getString(3)},
                             {"coverFileId", prs->getInt(4)},
                             {"playlistType", normalizePlaylistType(prs->getString(5))},
                             {"createdAt", prs->getString(6)},
                             {"updatedAt", prs->getString(7)},
                             {"items", json::array()}});
    }

    db::MySQLStatement itemStmt(
        mysql,
        "SELECT i.id, i.playlist_id, i.file_id, i.sort_order, i.note, "
        "f.filename, f.original_filename, f.file_size, "
        "COALESCE(f.relative_path, '') "
        "FROM entertainment_playlist_items i "
        "JOIN files f ON i.file_id = f.id "
        "WHERE i.user_id = ? AND f.status = 'success' AND f.is_deleted = 0 "
        "ORDER BY i.playlist_id ASC, i.sort_order ASC, i.id ASC");
    itemStmt.bindInt(userId);
    if (!itemStmt.execute()) {
        return playlists;
    }
    auto irs = itemStmt.getResultSet();
    while (irs && irs->next()) {
        const int playlistId = irs->getInt(1);
        auto it = index.find(playlistId);
        if (it == index.end()) {
            continue;
        }
        playlists[it->second]["items"].push_back(
            {{"id", irs->getInt(0)},
             {"playlistId", playlistId},
             {"fileId", irs->getInt(2)},
             {"sortOrder", irs->getInt(3)},
             {"note", irs->getString(4)},
             {"filename", irs->getString(5)},
             {"originalName", irs->getString(6)},
             {"size", irs->getInt64(7)},
             {"relativePath", irs->getString(8)},
             {"mediaType", lowerExt(irs->getString(6))}});
    }
    return playlists;
}

bool EntertainmentHandler::handlePlaylists(
    const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
    std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.path() != "/api/entertainment/playlists") {
        return false;
    }

    UserContext ctx;
    if (!verifyUser(conn, req, resp, ctx)) {
        return true;
    }
    if (!isVipLike(ctx.serviceLevel)) {
        sendError(resp, "娱乐片单仅 VIP/SVIP 用户可用",
                  fn::HttpResponse::k403Forbidden, conn);
        return true;
    }

    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        sendError(resp, "数据库连接失败", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    if (req.method() == fn::HttpRequest::kGet) {
        sendJson(resp, {{"code", 0}, {"playlists", listPlaylists(*mysql, ctx.userId)}});
        return true;
    }
    if (req.method() != fn::HttpRequest::kPost) {
        return false;
    }

    try {
        json body = json::parse(req.body());
        const std::string action = body.value("action", "");

        if (action == "create") {
            std::string title = clampText(body.value("title", ""), 128);
            if (title.empty()) {
                sendError(resp, "片单名称不能为空", fn::HttpResponse::k400BadRequest,
                          conn);
                return true;
            }
            std::string theme = isSvip(ctx.serviceLevel)
                                    ? clampText(body.value("theme", "classic"), 32)
                                    : "classic";
            std::string playlistType =
                normalizePlaylistType(body.value("playlistType", "video"));
            db::MySQLStatement stmt(
                *mysql,
                "INSERT INTO entertainment_playlists "
                "(user_id, title, description, playlist_type, theme, cover_file_id) "
                "VALUES (?, ?, ?, ?, ?, ?)");
            stmt.bindInt(ctx.userId);
            stmt.bindString(title);
            stmt.bindString(clampText(body.value("description", ""), 1000));
            stmt.bindString(playlistType);
            stmt.bindString(theme);
            stmt.bindInt(isSvip(ctx.serviceLevel)
                             ? body.value("coverFileId", 0)
                             : 0);
            if (!stmt.execute()) {
                sendError(resp, "创建片单失败",
                          fn::HttpResponse::k500InternalServerError, conn);
                return true;
            }
        } else {
            const int playlistId = body.value("playlistId", 0);
            if (!playlistOwned(*mysql, ctx.userId, playlistId)) {
                sendError(resp, "片单不存在或无权访问",
                          fn::HttpResponse::k404NotFound, conn);
                return true;
            }

            if (action == "delete") {
                db::MySQLStatement delItems(
                    *mysql,
                    "DELETE FROM entertainment_playlist_items WHERE "
                    "playlist_id = ? AND user_id = ?");
                delItems.bindInt(playlistId);
                delItems.bindInt(ctx.userId);
                delItems.execute();
                db::MySQLStatement delPlaylist(
                    *mysql,
                    "DELETE FROM entertainment_playlists WHERE id = ? AND "
                    "user_id = ?");
                delPlaylist.bindInt(playlistId);
                delPlaylist.bindInt(ctx.userId);
                delPlaylist.execute();
            } else if (action == "add_item") {
                const int fileId = body.value("fileId", 0);
                json fileInfo;
                if (!fileAccessible(*mysql, ctx.userId, fileId, &fileInfo) ||
                    !isEntertainmentMedia(fileInfo.value("originalName", ""))) {
                    sendError(resp, "文件不存在或不是娱乐媒体文件",
                              fn::HttpResponse::k404NotFound, conn);
                    return true;
                }
                std::string playlistType = "video";
                db::MySQLStatement typeStmt(
                    *mysql,
                    "SELECT COALESCE(playlist_type, 'video') FROM "
                    "entertainment_playlists WHERE id = ? AND user_id = ? "
                    "LIMIT 1");
                typeStmt.bindInt(playlistId);
                typeStmt.bindInt(ctx.userId);
                if (typeStmt.execute()) {
                    auto trs = typeStmt.getResultSet();
                    if (trs && trs->next()) {
                        playlistType = normalizePlaylistType(trs->getString(0));
                    }
                }
                const std::string originalName = fileInfo.value("originalName", "");
                if ((playlistType == "video" && !isVideoMedia(originalName)) ||
                    (playlistType == "image" && !isImageMedia(originalName))) {
                    sendError(resp,
                              playlistType == "video"
                                  ? "视频片单只能加入视频文件"
                                  : "图片片单只能加入图片文件",
                              fn::HttpResponse::k400BadRequest, conn);
                    return true;
                }
                int nextOrder = 0;
                db::MySQLStatement orderStmt(
                    *mysql,
                    "SELECT COALESCE(MAX(sort_order), 0) + 1 FROM "
                    "entertainment_playlist_items WHERE playlist_id = ?");
                orderStmt.bindInt(playlistId);
                if (orderStmt.execute()) {
                    auto ors = orderStmt.getResultSet();
                    if (ors && ors->next()) {
                        nextOrder = ors->getInt(0);
                    }
                }
                db::MySQLStatement stmt(
                    *mysql,
                    "INSERT INTO entertainment_playlist_items "
                    "(playlist_id, user_id, file_id, sort_order, note) "
                    "VALUES (?, ?, ?, ?, ?) ON DUPLICATE KEY UPDATE "
                    "note = VALUES(note), updated_at = NOW()");
                stmt.bindInt(playlistId);
                stmt.bindInt(ctx.userId);
                stmt.bindInt(fileId);
                stmt.bindInt(nextOrder);
                stmt.bindString(clampText(body.value("note", ""), 1000));
                stmt.execute();
            } else if (action == "remove_item") {
                db::MySQLStatement stmt(
                    *mysql,
                    "DELETE FROM entertainment_playlist_items WHERE id = ? "
                    "AND playlist_id = ? AND user_id = ?");
                stmt.bindInt(body.value("itemId", 0));
                stmt.bindInt(playlistId);
                stmt.bindInt(ctx.userId);
                stmt.execute();
            } else if (action == "update_item") {
                db::MySQLStatement stmt(
                    *mysql,
                    "UPDATE entertainment_playlist_items SET note = ?, "
                    "sort_order = ? WHERE id = ? AND playlist_id = ? AND "
                    "user_id = ?");
                stmt.bindString(clampText(body.value("note", ""), 1000));
                stmt.bindInt(body.value("sortOrder", 0));
                stmt.bindInt(body.value("itemId", 0));
                stmt.bindInt(playlistId);
                stmt.bindInt(ctx.userId);
                stmt.execute();
            } else if (action == "update_playlist") {
                std::string theme = isSvip(ctx.serviceLevel)
                                        ? clampText(body.value("theme", "classic"), 32)
                                        : "classic";
                db::MySQLStatement stmt(
                    *mysql,
                    "UPDATE entertainment_playlists SET title = ?, "
                    "description = ?, playlist_type = ?, theme = ?, cover_file_id = ? "
                    "WHERE id = ? AND user_id = ?");
                stmt.bindString(clampText(body.value("title", ""), 128));
                stmt.bindString(clampText(body.value("description", ""), 1000));
                stmt.bindString(normalizePlaylistType(body.value("playlistType", "video")));
                stmt.bindString(theme);
                stmt.bindInt(isSvip(ctx.serviceLevel)
                                 ? body.value("coverFileId", 0)
                                 : 0);
                stmt.bindInt(playlistId);
                stmt.bindInt(ctx.userId);
                stmt.execute();
            } else {
                sendError(resp, "未知片单操作", fn::HttpResponse::k400BadRequest,
                          conn);
                return true;
            }
        }
        sendJson(resp, {{"code", 0}, {"playlists", listPlaylists(*mysql, ctx.userId)}});
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "片单操作失败：" + std::string(e.what()),
                  fn::HttpResponse::k400BadRequest, conn);
        return true;
    }
}

bool EntertainmentHandler::handleDanmaku(
    const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
    std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.path() != "/api/entertainment/danmaku") {
        return false;
    }
    UserContext ctx;
    if (!verifyUser(conn, req, resp, ctx)) {
        return true;
    }
    if (!isVipLike(ctx.serviceLevel)) {
        sendError(resp, "娱乐弹幕仅 VIP/SVIP 用户可用",
                  fn::HttpResponse::k403Forbidden, conn);
        return true;
    }
    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        sendError(resp, "数据库连接失败", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    if (req.method() == fn::HttpRequest::kGet) {
        const int fileId = std::max(0, std::atoi(req.getQuery("fileId").c_str()));
        const std::string roomCode = req.getQuery("roomCode");
        if (!fileAccessible(*mysql, ctx.userId, fileId, nullptr, roomCode)) {
            sendError(resp, "文件不存在或无权访问",
                      fn::HttpResponse::k404NotFound, conn);
            return true;
        }
        const bool roomVisible = !roomCode.empty();
        db::MySQLStatement stmt(
            *mysql,
            roomVisible
                ? "SELECT d.id, d.user_id, u.username, d.time_ms, d.text, "
                  "d.color, d.is_shared, d.created_at FROM entertainment_danmaku d "
                  "LEFT JOIN users u ON d.user_id = u.id "
                  "WHERE d.file_id = ? ORDER BY d.time_ms ASC, d.id ASC"
                : "SELECT d.id, d.user_id, u.username, d.time_ms, d.text, "
                  "d.color, d.is_shared, d.created_at FROM entertainment_danmaku d "
                  "LEFT JOIN users u ON d.user_id = u.id "
                  "WHERE d.file_id = ? AND (d.user_id = ? OR d.is_shared = 1) "
                  "ORDER BY d.time_ms ASC, d.id ASC");
        stmt.bindInt(fileId);
        if (!roomVisible) {
            stmt.bindInt(ctx.userId);
        }
        stmt.execute();
        json danmaku = json::array();
        auto rs = stmt.getResultSet();
        while (rs && rs->next()) {
            danmaku.push_back({{"id", rs->getInt(0)},
                               {"userId", rs->getInt(1)},
                               {"username", rs->getString(2)},
                               {"timeMs", rs->getInt(3)},
                               {"text", rs->getString(4)},
                               {"color", rs->getString(5)},
                               {"shared", rs->getInt(6) != 0},
                               {"createdAt", rs->getString(7)}});
        }
        sendJson(resp, {{"code", 0}, {"danmaku", danmaku}});
        return true;
    }

    try {
        json body = json::parse(req.body());
        const std::string action = body.value("action", "create");
        if (action == "delete") {
            db::MySQLStatement stmt(
                *mysql,
                "DELETE FROM entertainment_danmaku WHERE id = ? AND user_id = ?");
            stmt.bindInt(body.value("id", 0));
            stmt.bindInt(ctx.userId);
            stmt.execute();
            sendJson(resp, {{"code", 0}});
            return true;
        }

        const int fileId = body.value("fileId", 0);
        const std::string roomCode = body.value("roomCode", "");
        if (!fileAccessible(*mysql, ctx.userId, fileId, nullptr, roomCode)) {
            sendError(resp, "文件不存在或无权访问",
                      fn::HttpResponse::k404NotFound, conn);
            return true;
        }
        const std::string color = isSvip(ctx.serviceLevel)
                                      ? clampText(body.value("color", "#ffffff"), 32)
                                      : "#ffffff";
        const bool shared = isSvip(ctx.serviceLevel) && body.value("shared", false);
        db::MySQLStatement stmt(
            *mysql,
            "INSERT INTO entertainment_danmaku "
            "(user_id, file_id, time_ms, text, color, is_shared) "
            "VALUES (?, ?, ?, ?, ?, ?)");
        stmt.bindInt(ctx.userId);
        stmt.bindInt(fileId);
        stmt.bindInt(std::max(0, body.value("timeMs", 0)));
        stmt.bindString(clampText(body.value("text", ""), 300));
        stmt.bindString(color);
        stmt.bindInt(shared ? 1 : 0);
        if (!stmt.execute()) {
            sendError(resp, "发送弹幕失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }
        sendJson(resp, {{"code", 0}});
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "弹幕操作失败：" + std::string(e.what()),
                  fn::HttpResponse::k400BadRequest, conn);
        return true;
    }
}

std::string EntertainmentHandler::generateRoomCode() {
    static const char chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static thread_local std::mt19937 rng(
        static_cast<unsigned int>(std::chrono::steady_clock::now()
                                      .time_since_epoch()
                                      .count()));
    std::uniform_int_distribution<size_t> dist(0, sizeof(chars) - 2);
    std::string code;
    for (int i = 0; i < 6; ++i) {
        code.push_back(chars[dist(rng)]);
    }
    return code;
}

json EntertainmentHandler::buildRoomStatus(db::MySQLPool::ConnectionGuard &mysql,
                                           const std::string &roomCode) {
    db::MySQLStatement stmt(
        mysql,
        "SELECT r.room_code, r.host_user_id, u.username, r.playlist_id, "
        "r.current_item_id, r.current_time_ms, r.playing, "
        "COALESCE(r.playback_rate, 1.0), "
        "COALESCE(r.danmaku_enabled, 1), r.expires_at "
        "FROM entertainment_rooms r LEFT JOIN users u ON r.host_user_id = u.id "
        "WHERE r.room_code = ? AND r.expires_at > NOW() LIMIT 1");
    stmt.bindString(roomCode);
    if (!stmt.execute()) {
        return json::object();
    }
    auto rs = stmt.getResultSet();
    if (!rs || !rs->next()) {
        return json::object();
    }
    const int hostUserId = rs->getInt(1);
    json room = {{"code", rs->getString(0)},
                 {"hostUserId", hostUserId},
                 {"hostUsername", rs->getString(2)},
                 {"playlistId", rs->getInt(3)},
                 {"currentItemId", rs->getInt(4)},
                 {"currentTimeMs", rs->getInt(5)},
                 {"playing", rs->getInt(6) != 0},
                 {"playbackRate", rs->getDouble(7)},
                 {"danmakuEnabled", rs->getInt(8) != 0},
                 {"expiresAt", rs->getString(9)}};
    room["playlists"] = listPlaylists(mysql, hostUserId);
    return room;
}

bool EntertainmentHandler::handleRooms(const fn::TcpConnectionPtr &conn,
                                       fn::HttpRequest &req,
                                       std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.path() != "/api/entertainment/rooms") {
        return false;
    }
    UserContext ctx;
    if (!verifyUser(conn, req, resp, ctx)) {
        return true;
    }
    if (!isVipLike(ctx.serviceLevel)) {
        sendError(resp, "观影房仅 VIP/SVIP 用户可用",
                  fn::HttpResponse::k403Forbidden, conn);
        return true;
    }
    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        sendError(resp, "数据库连接失败", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    if (req.method() == fn::HttpRequest::kGet) {
        const std::string code = req.getQuery("code");
        if (code.empty()) {
            if (!isSvip(ctx.serviceLevel)) {
                sendError(resp, "只有 SVIP 房主可以查看自己创建的观影房",
                          fn::HttpResponse::k403Forbidden, conn);
                return true;
            }
            const int playlistId = std::atoi(req.getQuery("playlistId").c_str());
            json rooms = json::array();
            if (playlistId > 0) {
                db::MySQLStatement stmt(
                    *mysql,
                    "SELECT room_code FROM entertainment_rooms WHERE host_user_id = ? "
                    "AND playlist_id = ? AND expires_at > NOW() "
                    "ORDER BY updated_at DESC, id DESC");
                stmt.bindInt(ctx.userId);
                stmt.bindInt(playlistId);
                if (stmt.execute()) {
                    auto rs = stmt.getResultSet();
                    while (rs && rs->next()) {
                        json room = buildRoomStatus(*mysql, rs->getString(0));
                        if (!room.empty()) {
                            room["isHost"] = true;
                            rooms.push_back(room);
                        }
                    }
                }
            } else {
                db::MySQLStatement stmt(
                    *mysql,
                    "SELECT room_code FROM entertainment_rooms WHERE host_user_id = ? "
                    "AND expires_at > NOW() ORDER BY updated_at DESC, id DESC");
                stmt.bindInt(ctx.userId);
                if (stmt.execute()) {
                    auto rs = stmt.getResultSet();
                    while (rs && rs->next()) {
                        json room = buildRoomStatus(*mysql, rs->getString(0));
                        if (!room.empty()) {
                            room["isHost"] = true;
                            rooms.push_back(room);
                        }
                    }
                }
            }
            sendJson(resp, {{"code", 0}, {"rooms", rooms}});
            return true;
        }
        json room = buildRoomStatus(*mysql, code);
        if (room.empty()) {
            sendError(resp, "观影房不存在或已过期",
                      fn::HttpResponse::k404NotFound, conn);
            return true;
        }
        room["isHost"] = room.value("hostUserId", -1) == ctx.userId;
        sendJson(resp, {{"code", 0}, {"room", room}});
        return true;
    }

    try {
        json body = json::parse(req.body());
        const std::string action = body.value("action", "");
        if (action == "create") {
            if (!isSvip(ctx.serviceLevel)) {
                sendError(resp, "只有 SVIP 用户可以创建观影房",
                          fn::HttpResponse::k403Forbidden, conn);
                return true;
            }
            const int playlistId = body.value("playlistId", 0);
            if (!playlistOwned(*mysql, ctx.userId, playlistId)) {
                sendError(resp, "片单不存在或无权访问",
                          fn::HttpResponse::k404NotFound, conn);
                return true;
            }
            db::MySQLStatement typeStmt(
                *mysql,
                "SELECT COALESCE(playlist_type, 'video') FROM "
                "entertainment_playlists WHERE id = ? AND user_id = ? "
                "LIMIT 1");
            typeStmt.bindInt(playlistId);
            typeStmt.bindInt(ctx.userId);
            std::string playlistType = "video";
            if (typeStmt.execute()) {
                auto typeRs = typeStmt.getResultSet();
                if (typeRs && typeRs->next()) {
                    playlistType = normalizePlaylistType(typeRs->getString(0));
                }
            }
            if (playlistType != "video") {
                sendError(resp, "只有视频片单可以创建观影房",
                          fn::HttpResponse::k400BadRequest, conn);
                return true;
            }
            std::string code = generateRoomCode();
            db::MySQLStatement stmt(
                *mysql,
                "INSERT INTO entertainment_rooms "
                "(room_code, host_user_id, playlist_id, expires_at) "
                "VALUES (?, ?, ?, DATE_ADD(NOW(), INTERVAL 6 HOUR))");
            stmt.bindString(code);
            stmt.bindInt(ctx.userId);
            stmt.bindInt(playlistId);
            if (!stmt.execute()) {
                code = generateRoomCode();
                db::MySQLStatement retry(
                    *mysql,
                    "INSERT INTO entertainment_rooms "
                    "(room_code, host_user_id, playlist_id, expires_at) "
                    "VALUES (?, ?, ?, DATE_ADD(NOW(), INTERVAL 6 HOUR))");
                retry.bindString(code);
                retry.bindInt(ctx.userId);
                retry.bindInt(playlistId);
                retry.execute();
            }
            json room = buildRoomStatus(*mysql, code);
            room["isHost"] = true;
            sendJson(resp, {{"code", 0}, {"room", room}});
            return true;
        }

        const std::string roomCode = body.value("roomCode", "");
        if (action == "delete") {
            const int playlistId = body.value("playlistId", 0);
            bool deleted = false;
            if (playlistId > 0) {
                db::MySQLStatement stmt(
                    *mysql,
                    "DELETE FROM entertainment_rooms WHERE room_code = ? "
                    "AND host_user_id = ? AND playlist_id = ?");
                stmt.bindString(roomCode);
                stmt.bindInt(ctx.userId);
                stmt.bindInt(playlistId);
                deleted = stmt.execute() && stmt.affectedRows() > 0;
            } else {
                db::MySQLStatement stmt(
                    *mysql,
                    "DELETE FROM entertainment_rooms WHERE room_code = ? "
                    "AND host_user_id = ?");
                stmt.bindString(roomCode);
                stmt.bindInt(ctx.userId);
                deleted = stmt.execute() && stmt.affectedRows() > 0;
            }
            if (!deleted) {
                sendError(resp, "只有房主可以删除自己的观影房",
                          fn::HttpResponse::k403Forbidden, conn);
                return true;
            }
            sendJson(resp, {{"code", 0}});
            return true;
        }
        if (action == "update") {
            db::MySQLStatement stmt(
                *mysql,
                "UPDATE entertainment_rooms SET current_item_id = ?, "
                "current_time_ms = ?, playing = ?, playback_rate = ?, "
                "danmaku_enabled = ? "
                "WHERE room_code = ? "
                "AND host_user_id = ? AND expires_at > NOW()");
            stmt.bindInt(body.value("currentItemId", 0));
            stmt.bindInt(std::max(0, body.value("currentTimeMs", 0)));
            stmt.bindInt(body.value("playing", false) ? 1 : 0);
            stmt.bindDouble(std::max(0.25, std::min(4.0, body.value("playbackRate", 1.0))));
            stmt.bindInt(body.value("danmakuEnabled", true) ? 1 : 0);
            stmt.bindString(roomCode);
            stmt.bindInt(ctx.userId);
            if (!stmt.execute() || stmt.affectedRows() == 0) {
                sendError(resp, "只有房主可以同步观影状态",
                          fn::HttpResponse::k403Forbidden, conn);
                return true;
            }
        }
        json room = buildRoomStatus(*mysql, roomCode);
        if (room.empty()) {
            sendError(resp, "观影房不存在或已过期",
                      fn::HttpResponse::k404NotFound, conn);
            return true;
        }
        room["isHost"] = room.value("hostUserId", -1) == ctx.userId;
        sendJson(resp, {{"code", 0}, {"room", room}});
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "观影房操作失败：" + std::string(e.what()),
                  fn::HttpResponse::k400BadRequest, conn);
        return true;
    }
}
