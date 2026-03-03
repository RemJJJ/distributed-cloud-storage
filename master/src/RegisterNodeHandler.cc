#include "RegisterNodeHandler.h"
#include <nlohmann/json.hpp>

bool RegisterNodeHandler::handleRegisterNode(
    const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
    std::shared_ptr<fn::HttpResponse> &resp) {
    if (req.method() != fn::HttpRequest::kPost ||
        req.path() != "/registerNode") {
        resp->setStatusCode(fn::HttpResponse::k404NotFound);
        resp->setBody("404 Not Found");
        return true;
    }
    LOG_INFO << "body.size() = " << req.body().size();
    // 解析body中的json字符串
    std::string body = req.body();
    nlohmann::json json = nlohmann::json::parse(body);
    std::string node_ip = json["ip"].get<std::string>();
    uint16_t node_port = json["port"].get<uint16_t>();
    fn::InetAddress addr(node_ip, node_port);
    NodeManager::instance().registerNode(addr);

    resp->setStatusCode(fn::HttpResponse::k200Ok);
    nlohmann::json respJson = {{"code", 0}, {"message", "注册成功"}};
    resp->setBody(respJson.dump());
    return true;
}