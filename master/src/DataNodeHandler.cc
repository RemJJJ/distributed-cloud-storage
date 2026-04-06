#include "DataNodeHandler.h"
#include "NodeManager.h"
#include "db/MySQLPool.h"
#include "db/MySQLStatement.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include <exception>
#include <string>
#include <unordered_set>

// 注册节点
bool DataNodeHandler::handleRegisterNode(
    const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
    std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path() != "/registerNode") {
        sendError(resp, "404 Not Found",
                  fileserver::net::HttpResponse::k404NotFound, conn);
        return true;
    }
    LOG_INFO << "body.size() = " << req.body().size();
    // 解析body中的json字符串
    try {
        std::string body = req.body();
        if (body.empty()) {
            sendError(resp, "Empty body",
                      fileserver::net::HttpResponse::k400BadRequest, conn);
            return true;
        }

        nlohmann::json json = nlohmann::json::parse(body);

        // 检查字段是否存在
        if (!json.contains("ip") || !json.contains("port")) {
            sendError(resp, "Missing ip or port",
                      fileserver::net::HttpResponse::k400BadRequest, conn);
            return true;
        }
        std::string node_ip = json["ip"].get<std::string>();
        uint16_t node_port = json["port"].get<uint16_t>();
        std::string public_url = json.value("public_url", "");

        // 提取DataNode传来的老ID
        std::string reported_node_id = json.value("node_id", "");
        fn::InetAddress addr(node_ip, node_port);
        auto RegisterResponse = NodeManager::instance().registerNode(
            reported_node_id, addr, public_url);

        if (RegisterResponse.node_id.empty()) {
            sendError(resp, "Failed to generate token",
                      fileserver::net::HttpResponse::k500InternalServerError,
                      conn);
            return true;
        }
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        nlohmann::json respJson = {{"code", 0},
                                   {"message", "注册成功"},
                                   {"node_id", RegisterResponse.node_id},
                                   {"token", RegisterResponse.token}};
        resp->setBody(respJson.dump());
        resp->setContentType("application/json");
    } catch (const nlohmann::json::exception &e) {
        LOG_WARN << "JSON parse error: " << e.what();
        sendError(resp, "Invalid JSON format",
                  fileserver::net::HttpResponse::k400BadRequest, conn);
    } catch (const std::exception &e) {
        LOG_ERROR << "Unexpected error: " << e.what();
        sendError(resp, "Internal Server Error",
                  fileserver::net::HttpResponse::k500InternalServerError, conn);
    }
    return true;
}

// 心跳
bool DataNodeHandler::handleHeartbeat(const fn::TcpConnectionPtr &conn,
                                      fn::HttpRequest &req,
                                      std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost || req.path() != "/heartbeat") {
        sendError(resp, "404 Not Found",
                  fileserver::net::HttpResponse::k404NotFound, conn);
        return true;
    }

    // 从Header获取Token
    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.find("Bearer ") != 0) {
        sendError(resp, "Missing or invalid Authorization header",
                  fileserver::net::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string token = authHeader.substr(7); // 去掉"Bearer "

    // 验证Token并提取node_id
    auto node_id = TokenManager::instance().verifyNodeToken(token);

    if (node_id.empty()) {
        sendError(resp, "Invalid or expired DataNode token",
                  fileserver::net::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    auto info = NodeManager::instance().getNodeInfo(node_id);
    if (!info) {
        sendError(resp, "Node not found",
                  fileserver::net::HttpResponse::k404NotFound, conn);
        return true;
    }

    // 解析请求体
    try {
        std::string body = req.body();
        if (body.empty()) {
            sendError(resp, "empty body",
                      fileserver::net::HttpResponse::k400BadRequest, conn);
            return true;
        }
        json jsonData = json::parse(body);

        std::string node_ip = jsonData["ip"].get<std::string>();
        uint16_t node_port = jsonData["port"].get<uint16_t>();
        std::string public_url = jsonData.value("public_url", "");

        // 解析调度指标
        uint64_t disk_total = jsonData.value("disk_total_mb", 0ULL);
        uint64_t disk_free = jsonData.value("disk_free_mb", 0ULL);
        int active_uploads = jsonData.value("active_uploads", 0);
        int active_downloads = jsonData.value("active_downloads", 0);
        int active_transfers = jsonData.value(
            "active_transfers", active_uploads + active_downloads);

        fn::InetAddress newAddr(node_ip, node_port);
        NodeManager::instance().updateHeartbeat(
            node_id, newAddr, disk_total, disk_free, active_uploads,
            active_downloads, active_transfers, public_url);

        LOG_INFO << "update heartbeat success:" << node_id << " @ "
                 << newAddr.toIpPort();
    } catch (const json::parse_error &e) {
        LOG_WARN << "JSON parse error: " << e.what();
        sendError(resp, "Invalid JSON format",
                  fileserver::net::HttpResponse::k400BadRequest, conn);
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR << "Unexpected error: " << e.what();
        sendError(resp, "Internal Server Error",
                  fileserver::net::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    resp->setStatusCode(fn::HttpResponse::k200Ok);
    resp->setContentType("application/json");
    nlohmann::json respJson = {
        {"code", 0}, {"message", "心跳成功"}, {"node_id", node_id}};
    resp->setBody(respJson.dump());
    return true;
}

// 处理DataNode的上传完成通知
bool DataNodeHandler::handleUploadFinishNotify(
    const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
    std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path() != "/notify_upload_finish") {
        return false;
    }
    LOG_INFO << "收到 DataNode 的文件上传完成通知";

    try {
        std::string authHeader = req.getHeader("Authorization");
        if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
            sendError(resp, "未授权的节点", fn::HttpResponse::k401Unauthorized,
                      conn);
            return true;
        }

        std::string token = authHeader.substr(7);
        std::string tokenNodeId =
            TokenManager::instance().verifyNodeToken(token);
        if (tokenNodeId.empty()) {
            sendError(resp, "DataNode Token 无效或已过期",
                      fn::HttpResponse::k401Unauthorized, conn);
            return true;
        }

        // 2. 解析 DataNode 发来的 JSON 数据
        json reqJson = json::parse(req.body());
        std::string reqNodeId = reqJson.value("node_id", "");
        std::string fileIdStr = reqJson.value("file_id", "");
        size_t storedSize = reqJson.value("stored_size", 0);

        // 安全校验：Token 里的 node_id 必须和 JSON 声明的 node_id 一致！
        if (tokenNodeId != reqNodeId) {
            LOG_WARN << "节点 ID 伪造拦截: Token(" << tokenNodeId
                     << ") vs JSON(" << reqNodeId << ")";
            sendError(resp, "节点身份不匹配", fn::HttpResponse::k403Forbidden,
                      conn);
            return true;
        }

        int fileId = std::stoi(fileIdStr);
        // 3. 更新数据库中的文件状态
        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            sendError(resp, "数据库连接失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        // SQL 逻辑：只更新属于该节点，且状态为 'uploading' 的文件。
        // 同时更新实际存储大小 (因为前端传的 size 可能不准，以 DataNode
        // 实际落盘为准)
        std::string updateSql =
            "UPDATE files SET status = 'success', file_size = ?, updated_at = "
            "NOW() "
            "WHERE id = ? AND node_id = ? AND status = 'uploading'";

        db::MySQLStatement stmt(*mysql, updateSql);
        stmt.bindInt64(storedSize);
        stmt.bindInt(fileId);
        stmt.bindString(tokenNodeId);

        if (!stmt.execute()) {
            LOG_ERROR << "更新文件状态失败: " << stmt.getError();
            sendError(resp, "数据库更新失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        if (stmt.affectedRows() == 0) {
            // 可能是重复通知，或者文件不存在/已被删除
            LOG_WARN << "文件状态未改变 (可能已成功或不存在), FileID: "
                     << fileId;
        } else {
            LOG_INFO << "文件状态已更新为 success, FileID: " << fileId;
        }

        // 4. 返回成功响应给 DataNode
        json respJson = {{"code", 0}, {"message", "状态更新成功"}};
        std::string bodyStr = respJson.dump();

        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(bodyStr);
        resp->addHeader("Content-Length", std::to_string(bodyStr.size()));

        return true;
    } catch (const json::parse_error &e) {
        LOG_ERROR << "JSON 解析错误: " << e.what();
        sendError(resp, "无效的请求格式", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR << "处理通知失败: " << e.what();
        sendError(resp, "服务器内部错误",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool DataNodeHandler::handleReportFiles(
    const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
    std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path() != "/report_files") {
        sendError(resp, "404 Not Found", fn::HttpResponse::k404NotFound, conn);
        return true;
    }

    LOG_INFO << "Receive files report from datanode";
    try {
        std::string authHeader = req.getHeader("Authorization");
        if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
            sendError(resp, "未授权", fn::HttpResponse::k401Unauthorized, conn);
            return true;
        }
        std::string tokenNodeId =
            TokenManager::instance().verifyNodeToken(authHeader.substr(7));
        if (tokenNodeId.empty()) {
            sendError(resp, "Token invalid",
                      fileserver::net::HttpResponse::k401Unauthorized, conn);
            return true;
        }

        // 解析汇报的JSON
        json reqJson = json::parse(req.body());
        std::string reqNodeId = reqJson.value("node_id", "");
        if (tokenNodeId != reqNodeId) {
            sendError(resp, "Identification not matched",
                      fn::HttpResponse::k403Forbidden, conn);
            return true;
        }

        std::vector<std::string> reportedFiles =
            reqJson.value("files", std::vector<std::string>());

        std::unordered_set<std::string> physicalFiles(reportedFiles.begin(),
                                                      reportedFiles.end());

        // 数据库对账
        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            sendError(resp, "DB connection failed",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        std::string querySql = "SELECT id, filename, status, is_deleted FROM "
                               "files WHERE node_id = ?";
        db::MySQLStatement queryStmt(*mysql, querySql);
        queryStmt.bindString(reqNodeId);

        if (queryStmt.execute()) {
            auto rs = queryStmt.getResultSet();
            int lostCount = 0;
            int recoveredCount = 0;
            json orphanFiles = json::array();
            std::unordered_set<std::string> referencedFiles;

            // 更新语句
            std::string markLostSql =
                "UPDATE files SET status = 'failed' WHERE id = ?";
            std::string markSuccessSql =
                "UPDATE files SET status = 'success' WHERE id = ?";

            while (rs->next()) {
                int fileId = rs->getInt(0);
                std::string dbFilename = rs->getString(1);
                std::string dbStatus = rs->getString(2);
                int isDeleted = rs->getInt(3);

                if (isDeleted != 2) {
                    referencedFiles.insert(dbFilename);
                }

                if (physicalFiles.find(dbFilename) == physicalFiles.end()) {
                    // 数据库有，datanode没有 ->文件丢失
                    if (isDeleted != 2 &&
                        (dbStatus == "success" || dbStatus == "uploading")) {
                        db::MySQLStatement updateStmt(*mysql, markLostSql);
                        updateStmt.bindInt(fileId);
                        updateStmt.execute();
                        lostCount++;
                        LOG_WARN
                            << "Found lost files, set status failed. FileID: "
                            << fileId;
                    }
                } else {
                    // datanode有，数据库有
                    if (dbStatus == "uploading") {
                        db::MySQLStatement updateStmt(*mysql, markSuccessSql);
                        updateStmt.bindInt(fileId);
                        updateStmt.execute();
                        recoveredCount++;
                        LOG_INFO << "Fix uploading status to success, FileID: "
                                 << fileId;
                    }
                }
            }

            for (const auto &physicalFilename : physicalFiles) {
                if (referencedFiles.find(physicalFilename) ==
                    referencedFiles.end()) {
                    orphanFiles.push_back(physicalFilename);
                }
            }
            LOG_INFO << "DataNode " << reqNodeId
                     << " reconciliation complete. Lost: " << lostCount
                     << ". Fixed: " << recoveredCount
                     << ". Orphan files: " << orphanFiles.size();

            json respJson = {{"code", 0},
                             {"message", "Report handle complete"},
                             {"orphan_files", orphanFiles}};
            std::string bodyStr = respJson.dump();
            resp->setStatusCode(fileserver::net::HttpResponse::k200Ok);
            resp->setContentType("application/json");
            resp->setBody(bodyStr);
            resp->addHeader("Content-Length", std::to_string(bodyStr.size()));
            return true;
        }
        sendError(resp, "Report query failed",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR << "Handle report error: " << e.what();
        sendError(resp, "Internal Server Error",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
}
