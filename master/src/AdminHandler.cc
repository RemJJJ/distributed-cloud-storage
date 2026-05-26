#include "AdminHandler.h"
#include "db/MySQLPool.h"
#include "db/MySQLStatement.h"
#include <algorithm>

namespace {
NodeManager::ClusterQosMode parseQosMode(const std::string &mode) {
    return mode == "strict" ? NodeManager::ClusterQosMode::kStrict
                            : NodeManager::ClusterQosMode::kElastic;
}

void sendJson(const std::shared_ptr<fn::HttpResponse> &resp,
              const nlohmann::json &payload) {
    resp->setStatusCode(fn::HttpResponse::k200Ok);
    resp->setContentType("application/json");
    const std::string body = payload.dump();
    resp->setBody(body);
    resp->addHeader("Content-Length", std::to_string(body.size()));
}
} // namespace

int AdminHandler::verifyAdminRequest(const fn::TcpConnectionPtr &conn,
                                     fn::HttpRequest &req,
                                     const std::shared_ptr<fn::HttpResponse>
                                         &resp) {
    const std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.find("Bearer ") != 0) {
        sendError(resp, "未授权的访问", fn::HttpResponse::k401Unauthorized,
                  conn);
        return -1;
    }

    const int adminUserId =
        TokenManager::instance().verifyAdminToken(authHeader.substr(7));
    if (adminUserId < 0) {
        sendError(resp, "管理员权限校验失败", fn::HttpResponse::k403Forbidden,
                  conn);
        return -1;
    }
    return adminUserId;
}

bool AdminHandler::handleStats(const fn::TcpConnectionPtr &conn,
                               fn::HttpRequest &req,
                               std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kGet ||
        req.path() != "/api/admin/stats") {
        return false;
    }
    if (verifyAdminRequest(conn, req, resp) < 0) {
        return true;
    }

    const auto stats = NodeManager::instance().getClusterRuntimeStats();
    sendJson(resp, {{"code", 0},
                    {"data",
                     {{"onlineNodes", stats.onlineNodes},
                      {"totalNodes", stats.totalNodes},
                      {"totalActiveConnections",
                       stats.totalActiveConnections},
                      {"totalUploadBps", stats.totalUploadBps},
                      {"totalDownloadBps", stats.totalDownloadBps},
                      {"totalBandwidthBps", stats.totalBandwidthBps},
                      {"totalDiskBytes", stats.totalDiskBytes},
                      {"totalDiskFreeBytes", stats.totalDiskFreeBytes},
                      {"qosMode", stats.qosMode},
                      {"manualQosOverride", stats.manualQosOverride},
                      {"globalBandwidthLimitBps",
                       stats.globalBandwidthLimitBps},
                      {"strictEnterActiveTransfers",
                       stats.strictEnterActiveTransfers},
                      {"strictExitActiveTransfers",
                       stats.strictExitActiveTransfers}}}});
    return true;
}

bool AdminHandler::handleNodes(const fn::TcpConnectionPtr &conn,
                               fn::HttpRequest &req,
                               std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kGet ||
        req.path() != "/api/admin/nodes") {
        return false;
    }
    if (verifyAdminRequest(conn, req, resp) < 0) {
        return true;
    }

    nlohmann::json nodes = nlohmann::json::array();
    for (const auto &node : NodeManager::instance().getAllNodes()) {
        const uint64_t totalBytes = node->diskTotalMb_ * 1024ULL * 1024ULL;
        const uint64_t freeBytes = node->diskFreeMb_ * 1024ULL * 1024ULL;
        const double diskUsagePercent =
            node->diskTotalMb_ == 0
                ? 0.0
                : (1.0 - static_cast<double>(node->diskFreeMb_) /
                             static_cast<double>(node->diskTotalMb_)) *
                      100.0;
        nodes.push_back(
            {{"nodeId", node->id_},
             {"ip", node->addr_.toIpPort()},
             {"publicUrl", node->publicUrl_},
             {"isAlive", node->isAlive_},
             {"isManuallyDisabled", node->isManuallyDisabled_},
             {"diskTotalBytes", totalBytes},
             {"diskFreeBytes", freeBytes},
             {"diskUsagePercent", diskUsagePercent},
             {"activeUploads", node->activeUploads_},
             {"activeDownloads", node->activeDownloads_},
             {"activeConnections", node->activeTransfers_},
             {"uploadBps", node->uploadBps_},
             {"downloadBps", node->downloadBps_},
             {"totalBandwidthBps", node->uploadBps_ + node->downloadBps_},
             {"connectedUsers", node->connectedUsers_},
             {"nodeBandwidthLimitBps", node->nodeBandwidthLimitBps_}});
    }

    sendJson(resp, {{"code", 0}, {"data", {{"nodes", nodes}}}});
    return true;
}

bool AdminHandler::handleNodeCircuitBreak(
    const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
    std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path().find("/api/admin/nodes/") != 0) {
        return false;
    }
    if (verifyAdminRequest(conn, req, resp) < 0) {
        return true;
    }

    const std::string nodeId = req.getPathParam("node_id");
    if (nodeId.empty()) {
        sendError(resp, "节点 ID 缺失", fn::HttpResponse::k400BadRequest, conn);
        return true;
    }

    auto nodeInfo = NodeManager::instance().getNodeInfo(nodeId);
    if (!nodeInfo) {
        sendError(resp, "节点不存在", fn::HttpResponse::k404NotFound, conn);
        return true;
    }

    try {
        const auto reqJson = nlohmann::json::parse(req.body());
        const bool disabled = reqJson.value("disabled", false);
        NodeManager::instance().setNodeManualDisabled(nodeId, disabled);
        sendJson(resp, {{"code", 0},
                        {"message", "node updated"},
                        {"data",
                         {{"nodeId", nodeId}, {"disabled", disabled}}}});
    } catch (const std::exception &e) {
        sendError(resp, "无效的 JSON 请求体",
                  fn::HttpResponse::k400BadRequest, conn);
    }
    return true;
}

bool AdminHandler::handleNodeBandwidthLimit(
    const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
    std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path().find("/api/admin/nodes/") != 0) {
        return false;
    }
    if (verifyAdminRequest(conn, req, resp) < 0) {
        return true;
    }

    const std::string nodeId = req.getPathParam("node_id");
    if (nodeId.empty()) {
        sendError(resp, "节点 ID 缺失", fn::HttpResponse::k400BadRequest, conn);
        return true;
    }

    auto nodeInfo = NodeManager::instance().getNodeInfo(nodeId);
    if (!nodeInfo) {
        sendError(resp, "节点不存在", fn::HttpResponse::k404NotFound, conn);
        return true;
    }

    try {
        const auto reqJson = nlohmann::json::parse(req.body());
        const uint64_t limit = reqJson.value("nodeBandwidthLimitBps", 0ULL);
        NodeManager::instance().setNodeBandwidthLimitBps(nodeId, limit);
        sendJson(resp, {{"code", 0},
                        {"message", "node bandwidth limit updated"},
                        {"data",
                         {{"nodeId", nodeId},
                          {"nodeBandwidthLimitBps", limit}}}});
    } catch (const std::exception &e) {
        sendError(resp, "无效的 JSON 请求体",
                  fn::HttpResponse::k400BadRequest, conn);
    }
    return true;
}

bool AdminHandler::handleGetPolicy(const fn::TcpConnectionPtr &conn,
                                   fn::HttpRequest &req,
                                   std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kGet ||
        req.path() != "/api/admin/policy") {
        return false;
    }
    if (verifyAdminRequest(conn, req, resp) < 0) {
        return true;
    }

    const auto snapshot = NodeManager::instance().getAdminPolicySnapshot();
    sendJson(resp, {{"code", 0},
                    {"data",
                     {{"qosMode", snapshot.qosMode},
                      {"manualQosOverride", snapshot.manualOverride},
                      {"globalBandwidthLimitBps",
                       snapshot.globalBandwidthLimitBps},
                      {"strictEnterActiveTransfers",
                       snapshot.strictEnterActiveTransfers},
                      {"strictExitActiveTransfers",
                       snapshot.strictExitActiveTransfers}}}});
    return true;
}

bool AdminHandler::handleUpdatePolicy(const fn::TcpConnectionPtr &conn,
                                      fn::HttpRequest &req,
                                      std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path() != "/api/admin/policy") {
        return false;
    }
    if (verifyAdminRequest(conn, req, resp) < 0) {
        return true;
    }

    try {
        const auto reqJson = nlohmann::json::parse(req.body());
        const std::string qosMode = reqJson.value("qosMode", "elastic");
        const bool manualOverride =
            reqJson.value("manualQosOverride", false);
        const uint64_t globalLimit =
            reqJson.value("globalBandwidthLimitBps", 0ULL);
        const int strictEnter =
            reqJson.value("strictEnterActiveTransfers", -1);
        const int strictExit =
            reqJson.value("strictExitActiveTransfers", -1);

        if ((strictEnter >= 0 || strictExit >= 0) &&
            (strictEnter < 1 || strictExit < 0 || strictExit > strictEnter)) {
            sendError(resp,
                      "QoS 阈值无效：进入阈值必须 >=1，退出阈值必须在 0 到进入阈值之间",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        NodeManager::instance().setManualQosOverride(
            manualOverride, parseQosMode(qosMode));
        NodeManager::instance().setGlobalBandwidthLimitBps(globalLimit);
        if (strictEnter >= 0 && strictExit >= 0) {
            NodeManager::instance().setStrictModeThresholds(strictEnter,
                                                            strictExit);
        }

        sendJson(resp, {{"code", 0}, {"message", "policy updated"}});
    } catch (const std::exception &e) {
        sendError(resp, "无效的 JSON 请求体",
                  fn::HttpResponse::k400BadRequest, conn);
    }
    return true;
}

bool AdminHandler::handleActiveUsers(const fn::TcpConnectionPtr &conn,
                                     fn::HttpRequest &req,
                                     std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kGet ||
        req.path() != "/api/admin/users/active") {
        return false;
    }
    if (verifyAdminRequest(conn, req, resp) < 0) {
        return true;
    }

    auto sessions = NodeManager::instance().getActiveUserAudits();
    std::sort(sessions.begin(), sessions.end(),
              [](const ActiveUserSessionInfo &lhs,
                 const ActiveUserSessionInfo &rhs) {
                  return lhs.currentBps_ > rhs.currentBps_;
              });

    nlohmann::json users = nlohmann::json::array();
    for (const auto &session : sessions) {
        users.push_back({{"userId", session.userId_},
                         {"username", session.username_},
                         {"serviceLevel", session.serviceLevel_},
                         {"sceneTag", session.sceneTag_},
                         {"nodeId", session.nodeId_},
                         {"transferType", session.transferType_},
                         {"fileName", session.fileName_},
                         {"currentBps", session.currentBps_},
                         {"startedAt", session.startedAt_}});
    }

    sendJson(resp, {{"code", 0}, {"data", {{"users", users}}}});
    return true;
}

bool AdminHandler::handleTrafficHistory(const fn::TcpConnectionPtr &conn,
                                        fn::HttpRequest &req,
                                        std::shared_ptr<fn::HttpResponse>
                                            &resp) {
    if (req.method() != fn::HttpRequest::kGet ||
        req.path() != "/api/admin/traffic/history") {
        return false;
    }
    if (verifyAdminRequest(conn, req, resp) < 0) {
        return true;
    }

    int windowSeconds = 60;
    try {
        windowSeconds = std::max(1, std::stoi(req.getQuery("window", "60")));
    } catch (const std::exception &) {
        windowSeconds = 60;
    }
    auto points = NodeManager::instance().getTrafficHistory(windowSeconds);
    nlohmann::json pointJson = nlohmann::json::array();
    for (const auto &point : points) {
        pointJson.push_back({{"timestampMs", point.timestampMs},
                             {"uploadBps", point.uploadBps},
                             {"downloadBps", point.downloadBps}});
    }

    sendJson(resp,
             {{"code", 0},
              {"data", {{"windowSeconds", windowSeconds}, {"points", pointJson}}}});
    return true;
}
