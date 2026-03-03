#include "MasterClient.h"
#include "../../third_party/muduo/base/Logging.h"
#include <nlohmann/json.hpp>

using namespace nlohmann;

MasterClient::MasterClient(fn::EventLoop *loop,
                           const fn::InetAddress &masterAddr,
                           const fn::InetAddress &myAddr)
    : loop_(loop), client_(std::make_unique<fn::TcpClient>(
                       loop, masterAddr, "MasterClient - TcpClient")),
      masterAddr_(masterAddr), myAddr_(myAddr) {
    client_->setConnectionCallback(
        std::bind(&MasterClient::onConnection, this, std::placeholders::_1));
    client_->setMessageCallback(
        std::bind(&MasterClient::onMessage, this, std::placeholders::_1,
                  std::placeholders::_2, std::placeholders::_3));
} // namespace fileserver::net

MasterClient::~MasterClient() { LOG_INFO << "~MasterClient()"; }

void MasterClient::start() { client_->connect(); }

void MasterClient::onConnection(const fn::TcpConnectionPtr &conn) {
    if (conn->connected()) {
        conn_ = conn;
        LOG_INFO << "Connected to Master: " << masterAddr_.toIpPort();
        registerNode();
    } else {
        LOG_INFO << "Disconnected from Master: " << masterAddr_.toIpPort();
        conn_.reset();
    }
}

void MasterClient::onMessage(const fn::TcpConnectionPtr &conn, fn::Buffer *buf,
                             fileserver::Timestamp time) {
    // 第一版：直接把 Master 的响应读出来丢弃，或者打印日志看看
    std::string msg = buf->retrieveAllAsString();
    LOG_DEBUG << "收到 Master 响应: " << msg;
}

void MasterClient::registerNode() {
    json request = {
        {"ip", myAddr_.toIp()}, {"port", myAddr_.port()}, {"status", "active"}};
    post("/registerNode", request.dump());
    LOG_INFO << "Register node request sent: " << request.dump();
}

void MasterClient::startHeartbeat(double intervalSeconds) {
    loop_->runEvery(intervalSeconds,
                    std::bind(&MasterClient::sendHeartbeat, this));
}

void MasterClient::sendHeartbeat() {
    if (!conn_ || !conn_->connected()) {
        LOG_WARN << "未连接到Master, 跳过本次心跳";
        return;
    }
    json hbMessage = {
        {"ip", myAddr_.toIp()},
        {"port", myAddr_.port()},
        {"timestamp", fileserver::Timestamp::now().microSecondsSinceEpoch()}};
    post("/heartbeat", hbMessage.dump());
    LOG_INFO << "Heartbeat sent: " << hbMessage.dump();
}

void MasterClient::post(const std::string &path, const std::string &body) {
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
    // 关键：header 末尾必须有 \r\n\r\n 分隔 body
    requestHeader += "\r\n";

    std::string request = requestHeader + body;
    conn_->send(request);
    LOG_INFO << "POST request sent:" << "\nHeader: " << requestHeader
             << "\nBody: " << body;
}