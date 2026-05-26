#include "MasterClient.h"
#include "DataNode.h"
#include "base/Logging.h"
#include "base/Timestamp.h"
#include "net/HttpContext.h"
#include "net/TimerId.h"
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>

using namespace nlohmann;
namespace fs = std::filesystem;

MasterClient::MasterClient(fn::EventLoop *loop,
                           const fn::InetAddress &masterAddr,
                           const fn::InetAddress &myAddr,
                           const std::string &publicUrl, DataNode *datanode)
    : loop_(loop), datanode_(datanode),
      client_(std::make_unique<fn::TcpClient>(loop, masterAddr,
                                              "MasterClient - TcpClient")),
      masterAddr_(masterAddr), myAddr_(myAddr), publicUrl_(publicUrl) {
    client_->setConnectionCallback(
        std::bind(&MasterClient::onConnection, this, std::placeholders::_1));
    client_->setMessageCallback(
        std::bind(&MasterClient::onMessage, this, std::placeholders::_1,
                  std::placeholders::_2, std::placeholders::_3));

    client_->enableRetry();

    // 初始化时尝试从本地文件读取已有的NodeID
    std::ifstream ifs("node_id.dat");
    if (ifs.is_open()) {
        ifs >> nodeId_;
        ifs.close();
        LOG_INFO << "从本地回复DataNode身份, NodeID: " << nodeId_;
    } else {
        LOG_INFO << "未找到本地NodeID, 将作为全新节点注册";
    }
} // namespace fileserver::net

MasterClient::~MasterClient() { LOG_INFO << "~MasterClient()"; }

void MasterClient::start() { client_->connect(); }

std::string MasterClient::getNodeId() const {
    std::lock_guard<std::mutex> lock(authMutex_);
    return nodeId_;
}

bool MasterClient::isRegistered() const { return registered_.load(); }

void MasterClient::onConnection(const fn::TcpConnectionPtr &conn) {
    if (conn->connected()) {
        conn_ = conn;
        LOG_INFO << "Connected to Master: " << masterAddr_.toIpPort();
        registerNode();

        // 连接成功后，补发之前没法出去的通知
        loop_->runInLoop([this]() { procPendingNotice(); });
    } else {
        LOG_INFO << "Disconnected from Master: " << masterAddr_.toIpPort();
        conn_.reset();
        registered_.store(false);

        // 断开时取消定时器
        loop_->cancel(tokenRefreshTimerId_);
        tokenRefreshTimerId_ = fn::TimerId();
    }
}

void MasterClient::onMessage(const fn::TcpConnectionPtr &conn, fn::Buffer *buf,
                             fileserver::Timestamp time) {

    // 解析状态行
    const char *crlf = buf->findCRLF();
    if (!crlf) {
        LOG_DEBUG << "等待完整状态行...";
        return; // 数据不完整，等待更多
    }

    // 解析状态行：HTTP/1.1 401 Unauthorized
    std::string statusLine(buf->peek(), crlf);
    buf->retrieveUntil(crlf + 2); // 跳过状态行和\r\n

    size_t firstSpace = statusLine.find(' ');
    size_t secondSpace = statusLine.find(' ', firstSpace + 1);

    if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
        LOG_ERROR << "无效的状态行：" << statusLine;
        return;
    }

    int statusCode = std::stoi(
        statusLine.substr(firstSpace + 1, secondSpace - firstSpace - 1));

    LOG_DEBUG << "HTTP 状态码：" << statusCode;

    // 检查 401 未授权
    if (statusCode == 401) {
        LOG_WARN << "Token 失效 (HTTP 401)，重新注册...";
        registerNode();
        buf->retrieveAll(); // 丢弃剩余数据
        return;
    }

    // 解析头部，找到空行
    size_t headerBytes = 0;
    bool foundEmptyLine = false;

    while (buf->readableBytes() >= 2) {
        const char *nextCRLF = buf->findCRLF();
        if (!nextCRLF) {
            // 还没找到完整的行，等待更多数据
            // 先把已读取的状态行恢复回去（因为我们要等完整头部）
            LOG_DEBUG << "等待完整头部...";
            return;
        }

        size_t lineLen = nextCRLF - buf->peek();
        // 检查是否是空行（头部结束标志）
        if (lineLen == 0) {
            foundEmptyLine = true;
            buf->retrieveUntil(nextCRLF + 2); // 跳过空行的\r\n
            break;
        }

        // 跳过这一行头部
        buf->retrieveUntil(nextCRLF + 2);
    }
    if (!foundEmptyLine) {
        LOG_DEBUG << "等待头部结束空行...";
        return;
    }

    // 剩余的就是body
    std::string body = buf->retrieveAllAsString();
    if (statusCode == 200) {
        parseResponseBody(body);
    } else {
        LOG_WARN << "HTTP 错误状态码：" << statusCode;
    }
}

void MasterClient::parseResponseBody(const std::string &body) {
    try {
        json respJson = json::parse(body);

        if (respJson.contains("node_id") && respJson.contains("token")) {
            handleRegisterResponse(body);
        } else if (respJson.contains("orphan_files")) {
            handleReportFilesResponse(respJson);
        } else if (respJson.contains("admin_policy")) {
            handleAdminPolicyResponse(respJson);
        } else if (respJson.contains("code")) {
            int code = respJson["code"].get<int>();
            if (code != 0) {
                LOG_WARN << "Master 业务错误: code=" << code;
            }
        }
    } catch (const json::exception &e) {
        LOG_DEBUG << "响应 body 不是 JSON: " << e.what();
    }
}

void MasterClient::handleReportFilesResponse(const json &respJson) {
    std::vector<std::string> orphanFiles =
        respJson.value("orphan_files", std::vector<std::string>{});
    if (orphanFiles.empty()) {
        return;
    }

    int deletedCount = 0;
    for (const auto &filename : orphanFiles) {
        try {
            const fs::path filepath = fs::path("uploads") / filename;
            if (fs::exists(filepath) && fs::is_regular_file(filepath)) {
                fs::remove(filepath);
                deletedCount++;
                LOG_WARN << "Delete orphan physical file after report_files: "
                         << filepath.string();
            }
        } catch (const std::exception &e) {
            LOG_ERROR << "Failed to delete orphan physical file " << filename
                      << ": " << e.what();
        }
    }
    LOG_INFO << "Report_files orphan cleanup complete, deleted_count="
             << deletedCount;
}

void MasterClient::handleAdminPolicyResponse(const json &respJson) {
    const auto &policyJson = respJson["admin_policy"];
    DataNode::AdminPolicyCache policy;
    policy.qosMode = policyJson.value("qos_mode", "elastic");
    policy.manualOverride = policyJson.value("manual_override", false);
    policy.globalBandwidthLimitBps =
        policyJson.value("global_bandwidth_limit_bps", 0ULL);
    policy.nodeBandwidthLimitBps =
        policyJson.value("node_bandwidth_limit_bps", 0ULL);
    datanode_->updateAdminPolicy(policy);
}

void MasterClient::handleRegisterResponse(const std::string &response) {
    try {
        json respJson = json::parse(response);

        std::string newToken = respJson["token"].get<std::string>();
        std::string newNodeId = respJson["node_id"].get<std::string>();

        {
            std::lock_guard<std::mutex> lock(authMutex_);
            token_ = newToken;

            // 如果Master给的ID和本地不一样，保存到磁盘
            if (nodeId_ != newNodeId) {
                nodeId_ = newNodeId;
                std::ofstream ofs("node_id.dat");
                if (ofs.is_open()) {
                    ofs << nodeId_;
                    ofs.close();
                } else {
                    LOG_ERROR << "无法将NodeID保存到本地文件node_id.dat";
                }
            }
            registered_.store(true);
        }
        LOG_INFO << "注册成功! nodeId=" << nodeId_
                 << ", token 前 10 字符=" << newToken.substr(0, 10) << "...";

        // 先取消旧定时器，在创建新的
        loop_->cancel(tokenRefreshTimerId_);
        tokenRefreshTimerId_ = loop_->runEvery(
            TOKEN_REFRESH_INTERVAL, [this]() { checkTokenExpired(); });

        // 注册成功并获取 Token 后，异步触发“全量文件汇报”
        // 不要在当前网络回调中同步执行扫盘，避免阻塞 IO 线程！
        loop_->runInLoop([this]() { reportLocalFiles(); });

    } catch (const json::exception &e) {
        LOG_ERROR << "解析注册响应失败：" << e.what();
    }
}

void MasterClient::checkTokenExpired() {
    // Token快过期了，主动重新注册获取新Token
    if (conn_ && conn_->connected()) {
        LOG_INFO << "Token 即将过期，主动刷新...";
        registerNode();
    }
}

void MasterClient::registerNode() {
    json request = {{"ip", myAddr_.toIp()},
                    {"port", myAddr_.port()},
                    {"status", "active"},
                    {"public_url", publicUrl_}};

    std::string currentId = getNodeId();
    if (!currentId.empty()) {
        request["node_id"] = currentId;
    }
    post("/registerNode", request.dump(), false);
    LOG_INFO << "Register node request sent: " << request.dump();
}

// ====================心跳请求(需要Token)===================
void MasterClient::startHeartbeat(double intervalSeconds) {
    loop_->runEvery(intervalSeconds,
                    std::bind(&MasterClient::sendHeartbeat, this));
}

void MasterClient::sendHeartbeat() {
    if (!conn_ || !conn_->connected()) {
        LOG_WARN << "未连接到Master, 跳过本次心跳";
        return;
    }

    if (!registered_.load()) {
        LOG_WARN << "尚未注册，先注册再心跳";
        registerNode(); // ???
        return;
    }

    std::string currentToken;
    std::string currentNodeId;
    {
        std::lock_guard<std::mutex> lock(authMutex_);
        currentToken = token_;
        currentNodeId = nodeId_;
    }

    // 获取磁盘信息
    uint64_t diskTotalMb = 0;
    uint64_t diskFreeMb = 0;
    try {
        std::filesystem::space_info si = std::filesystem::space("uploads");
        diskTotalMb = si.capacity / (1024 * 1024);
        diskFreeMb = si.available / (1024 * 1024);
    } catch (const std::exception &e) {
        LOG_WARN << "Get disk space failed: " << e.what();
    }

    // 获取当前活跃上传数
    int activeUploads = datanode_->getActiveUploads();
    int activeDownloads = datanode_->getActiveDownloads();
    int activeTransfers = datanode_->getActiveTransfers();
    uint64_t uploadBps = datanode_->getCurrentUploadBps();
    uint64_t downloadBps = datanode_->getCurrentDownloadBps();
    int connectedUsers = datanode_->getConnectedUsers();
    json activeSessions = json::array();
    for (const auto &session : datanode_->getTransferSessionSnapshots()) {
        activeSessions.push_back(
            {{"user_id", session.userId},
             {"username", session.username},
             {"service_level", session.serviceLevel},
             {"scene_tag", session.sceneTag},
             {"transfer_type", session.transferType},
             {"file_name", session.fileName},
             {"current_bps", session.currentBps},
             {"started_at", session.startedAt}});
    }
    json hbMessage = {
        {"node_id", currentNodeId},
        {"ip", myAddr_.toIp()},
        {"port", myAddr_.port()},
        {"public_url", publicUrl_},
        {"disk_total_mb", diskTotalMb},
        {"disk_free_mb", diskFreeMb},
        {"active_uploads", activeUploads},
        {"active_downloads", activeDownloads},
        {"active_transfers", activeTransfers},
        {"upload_bps", uploadBps},
        {"download_bps", downloadBps},
        {"connected_users", connectedUsers},
        {"active_user_sessions", activeSessions},
        {"timestamp", fileserver::Timestamp::now().microSecondsSinceEpoch()}};
    post("/heartbeat", hbMessage.dump(), true);
    LOG_INFO << "Heartbeat sent. Frees: " << diskFreeMb
             << "MB, Uploads: " << activeUploads
             << ", Downloads: " << activeDownloads
             << ", Transfers: " << activeTransfers;
}

void MasterClient::post(const std::string &path, const std::string &body,
                        bool needAuth) {
    if (!conn_ || !conn_->connected()) {
        LOG_ERROR << "Not connected to Master, cannot send POST request";
        return;
    }
    std::string requestHeader;
    // 2. 拼接 HTTP 头（严格按规范，换行符必须是 \r\n，末尾必须有 \r\n\r\n 分隔
    // header 和 body）
    // 核心：POST 行（路径必须以 / 开头，HTTP/1.1 版本）
    requestHeader += "POST " + path + " HTTP/1.1\r\n";
    // Host 字段：建议用 Master 的 IP 而非 "master" 字符串（替换成你的 masterIp_
    // 成员变量）
    requestHeader += "Host: " + masterAddr_.toIp() + ":" +
                     std::to_string(masterAddr_.port()) + "\r\n";
    // 必须字段：Content-Type（JSON 格式固定为 application/json）
    requestHeader += "Content-Type: application/json; charset=utf-8\r\n";
    // 必须字段：Content-Length（严格匹配 body 字节数）
    requestHeader += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    // 可选：长连接（根据需要保留）
    requestHeader += "Connection: Keep-Alive\r\n";
    // 可选：User-Agent（增加通用性）
    requestHeader += "User-Agent: MasterClient/1.0\r\n";

    // 如果需要认证，添加Authorization头
    if (needAuth) {
        std::string authHeader;
        {
            std::lock_guard<std::mutex> lock(authMutex_);
            authHeader = token_;
        }
        if (!authHeader.empty()) {
            requestHeader += "Authorization: Bearer " + authHeader + "\r\n";
        } else {
            LOG_WARN << "需要认证但Token为空!";
        }
    }
    // 关键：header 末尾必须有 \r\n\r\n 分隔 body
    requestHeader += "\r\n";

    std::string request = requestHeader + body;
    conn_->send(request);
}

// ----------线程安全的通知入口------------
void MasterClient::notifyUploadFinish(const std::string &file_id,
                                      const std::string &server_filename,
                                      size_t stored_size) {
    // 先把通知放进队列
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingNotifications_.push({file_id, server_filename, stored_size});
    }

    // 切换到MasterClient绑定的EventLoop中执行
    loop_->runInLoop([this]() { procPendingNotice(); });
}

// ------------实际执行通知的函数(在EventLoop线程中)--------------
void MasterClient::doNotifyUploadFinish(const std::string &file_id,
                                        const std::string &server_filename,
                                        size_t stored_size) {
    if (!conn_ || !conn_->connected()) {
        LOG_WARN << "Not connected to Master, notification queued: " << file_id;
        // 连接断开，重新放回队列（等下一次 onConnection 成功后再发）
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingNotifications_.push({file_id, server_filename, stored_size});
        return;
    }

    if (!registered_.load()) {
        LOG_WARN << "尚未注册,先注册再通知";
        registerNode();
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingNotifications_.push({file_id, server_filename, stored_size});
        return;
    }

    std::string currentToken;
    std::string currentNodeId;
    {
        std::lock_guard<std::mutex> lock(authMutex_);
        currentToken = token_;
        currentNodeId = nodeId_;
    }

    // 构造JSON请求体
    json reqJson;
    reqJson["node_id"] = currentNodeId;
    reqJson["file_id"] = file_id;
    reqJson["node_ip"] = myAddr_.toIp();
    reqJson["node_port"] = myAddr_.port();
    reqJson["server_filename"] = server_filename;
    reqJson["stored_size"] = stored_size;
    reqJson["upload_finish_ts"] =
        fileserver::Timestamp::now().microSecondsSinceEpoch();

    // post
    std::string path = "/notify_upload_finish";
    std::string body = reqJson.dump();
    post(path, body, true);

    LOG_INFO << "Notified Master of upload finish: " << file_id;
}

// ------------------处理待发送队列---------------------
void MasterClient::procPendingNotice() {
    loop_->assertInLoopThread();

    std::queue<PendingNotice> tempQueue;

    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        std::swap(tempQueue, pendingNotifications_);
    }

    // 逐个发送
    while (!tempQueue.empty()) {
        auto &notification = tempQueue.front();
        doNotifyUploadFinish(notification.file_id, notification.server_filename,
                             notification.stored_size);
        tempQueue.pop();
    }
}

// ------------------扫描本地文件---------------------
void MasterClient::reportLocalFiles() {
    if (!conn_ || !conn_->connected() || !registered_.load()) {
        return;
    }

    std::string uploadDir = "uploads";
    std::vector<std::string> localFiles;

    // 扫描本地目录
    try {
        if (fs::exists(uploadDir) && fs::is_directory(uploadDir)) {
            for (const auto &entry : fs::directory_iterator(uploadDir)) {
                if (entry.is_regular_file()) {
                    // 只收集文件名
                    localFiles.push_back(entry.path().filename().string());
                }
            }
        }
    } catch (const std::exception &e) {
        LOG_ERROR << "扫描本地文件目录失败：" << e.what();
        return;
    }

    // 如果没有任何文件，不汇报，或者发个空列表告诉Master我很干净
    LOG_INFO << "Scan completed. Got " << localFiles.size() << " file(s)";

    json reportJson;
    reportJson["node_id"] = getNodeId();
    reportJson["files"] = localFiles;

    post("/report_files", reportJson.dump(), true);
}
