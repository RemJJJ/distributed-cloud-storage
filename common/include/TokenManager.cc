#include "TokenManager.h"
#include "jwt-cpp/jwt.h"
#include "jwt-cpp/traits/kazuho-picojson/defaults.h"
#include <base/Logging.h>
#include <chrono>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>

// 初始化
void TokenManager::init(const std::string &secretKey) {
    static std::once_flag flag;
    std::call_once(flag, [&]() {
        instance().secretKey_ = secretKey;
        instance().initialized_ = true;
    });
}

// 私有构造
TokenManager::TokenManager() {}

bool TokenManager::isValid(const std::string &token) {
    return verifyToken(token).success;
}

bool TokenManager::isInitialized() { return instance().initialized_; }

// 生成用户token
TokenManager::userLoginResponse
TokenManager::generateUserToken(int userId, const std::string &username) {
    auto now = std::chrono::system_clock::now();
    std::string token;
    try {
        token = jwt::create()
                    .set_issuer("fileserver_master")
                    .set_type("JWT")
                    .set_payload_claim("token_type",
                                       jwt::claim(std::string("user")))
                    .set_payload_claim("user_id",
                                       jwt::claim(std::to_string(userId)))
                    .set_payload_claim("username", jwt::claim(username))
                    .set_issued_at(now)
                    .set_expires_at(
                        now + std::chrono::hours(24)) // 用户登录有效期 24 小时
                    .sign(jwt::algorithm::hs256{secretKey_});
    } catch (const std::exception &e) {
        LOG_ERROR << "Failed to sign JWT: " << e.what();
        return {"", ""}; // 返回空表示失败
    }

    LOG_INFO << "user: " << username << " login";
    return {std::to_string(userId), token};
}

// 生成文件上传token
TokenManager::fileUploadResponse TokenManager::generateUploadToken(
    int userId, uint64_t file_id, const std::string &node_id,
    const std::string &original_filename, const std::string &server_filename,
    const std::string &created_time) {
    std::string token;
    auto now = std::chrono::system_clock::now();
    try {
        token = jwt::create()
                    .set_issuer("fileserver_master")
                    .set_type("JWT")
                    .set_payload_claim("token_type",
                                       jwt::claim(std::string("upload")))
                    .set_payload_claim("user_id",
                                       jwt::claim(std::to_string(userId)))
                    .set_payload_claim("file_id",
                                       jwt::claim(std::to_string(file_id)))
                    .set_payload_claim("node_id", jwt::claim(node_id))
                    .set_payload_claim("original_filename",
                                       jwt::claim(original_filename))
                    .set_payload_claim("server_filename",
                                       jwt::claim(server_filename))
                    .set_payload_claim("created_time", jwt::claim(created_time))
                    .set_issued_at(now)
                    .set_expires_at(now +
                                    std::chrono::minutes(
                                        30)) // 上传 Token 只有 30 分钟有效期！
                    .sign(jwt::algorithm::hs256{secretKey_});
    } catch (const std::exception &e) {
        LOG_ERROR << "Failed to sign JWT: " << e.what();
        return {"", ""}; // 返回空表示失败
    }
    LOG_INFO << "Create file upload token";
    return {std::to_string(file_id), token};
}

// 生成下载Token
std::string
TokenManager::generateDownloadToken(int userId,
                                    const std::string &original_filename,
                                    const std::string &server_filename) {
    auto now = std::chrono::system_clock::now();
    std::string token;
    try {
        token = jwt::create()
                    .set_issuer("fileserver_master")
                    .set_type("JWT")
                    .set_payload_claim("token_type",
                                       jwt::claim(std::string("download")))
                    .set_payload_claim("user_id",
                                       jwt::claim(std::to_string(userId)))
                    .set_payload_claim("server_filename",
                                       jwt::claim(server_filename))
                    .set_payload_claim("original_filename",
                                       jwt::claim(original_filename))
                    .set_issued_at(now)
                    .set_expires_at(
                        now + std::chrono::hours(2)) // 下载链接 2 小时有效
                    .sign(jwt::algorithm::hs256{secretKey_});
    } catch (const std::exception &e) {
        LOG_ERROR << "Failed to sign JWT: " << e.what();
        return "";
    }
    LOG_INFO << "Create file download token";
    return token;
}

// 生成节点token
TokenManager::nodeRegisterResponse
TokenManager::generateNodeToken(const std::string &node_id,
                                const fileserver::net::InetAddress &addr) {
    std::string token;
    auto now = std::chrono::system_clock::now();
    try {
        token = jwt::create()
                    .set_issuer("fileserver_master")
                    .set_type("JWT")
                    .set_payload_claim("token_type",
                                       jwt::claim(std::string("datanode")))
                    .set_payload_claim("node_id", jwt::claim(node_id))
                    .set_payload_claim("address", jwt::claim(addr.toIpPort()))
                    .set_issued_at(now)
                    // DataNode Token 可以设置长一点，或者做定期刷新机制
                    .set_expires_at(now + std::chrono::hours(24))
                    .sign(jwt::algorithm::hs256{secretKey_});
    } catch (const std::exception &e) {
        LOG_ERROR << "Failed to sign JWT: " << e.what();
        return {"", ""}; // 返回空表示失败
    }
    LOG_INFO << "register node " << addr.toIpPort();
    return {node_id, token};
}

// 生成删除Token
std::string
TokenManager::generateDeleteToken(const std::string &server_filename) {
    auto now = std::chrono::system_clock::now();
    return jwt::create()
        .set_issuer("fileserver_master")
        .set_type("JWT")
        .set_payload_claim("token_type", jwt::claim(std::string("delete")))
        .set_payload_claim("server_filename", jwt::claim(server_filename))
        .set_issued_at(now)
        .set_expires_at(now +
                        std::chrono::minutes(5)) // 删除指令 5 分钟内有效即可
        .sign(jwt::algorithm::hs256{secretKey_});
}

// ================= 基础验证逻辑 =================

TokenManager::verifyResult TokenManager::verifyToken(const std::string &token) {
    if (!initialized_)
        return verifyResult::Fail("TokenManager 未初始化");

    try {
        auto decoded = jwt::decode(token);
        auto verifier = jwt::verify()
                            .allow_algorithm(jwt::algorithm::hs256{secretKey_})
                            .with_issuer("fileserver_master");

        // 验证签名和过期时间
        verifier.verify(decoded);

        // 巧妙转换：获取原始 JSON 字符串并用 nlohmann::json 解析
        std::string payload_str = decoded.get_payload();
        json payload_json = json::parse(payload_str);

        return verifyResult::Ok("auth", "fileserver_master", 0, payload_json);

    } catch (const std::exception &e) {
        return verifyResult::Fail(std::string("Token 验证失败: ") + e.what());
    }
}

// ================= 专用业务验证逻辑 =================
int TokenManager::verifyUserToken(const std::string &token) {
    auto result = verifyToken(token);
    if (!result.success) {
        LOG_WARN << "用户 Token 验证失败: " << result.error;
        return -1;
    }

    // 检查 Token 类型是否匹配
    if (result.payload.value("token_type", "") != "user") {
        LOG_WARN << "Token 类型不匹配，期望 user";
        return -1;
    }

    try {
        return std::stoi(result.payload.value("user_id", "-1"));
    } catch (...) {
        return -1;
    }
}

std::string TokenManager::verifyNodeToken(const std::string &token) {
    auto result = verifyToken(token);
    if (!result.success) {
        LOG_WARN << "DataNode Token 验证失败: " << result.error;
        return "";
    }

    if (result.payload.value("token_type", "") != "datanode") {
        LOG_WARN << "Token 类型不匹配，期望 datanode";
        return "";
    }

    return result.payload.value("node_id", "");
}

bool TokenManager::verifyUploadToken(const std::string &token,
                                     uint64_t &out_file_id,
                                     std::string &out_original_filename,
                                     std::string &out_server_filename,
                                     std::string &out_created_time) {
    auto result = verifyToken(token);
    if (!result.success) {
        LOG_WARN << "上传 Token 验证失败: " << result.error;
        return false;
    }

    if (result.payload.value("token_type", "") != "upload") {
        LOG_WARN << "Token 类型不匹配，期望 upload";
        return false;
    }

    try {
        out_file_id = std::stoull(result.payload.value("file_id", "0"));
        out_original_filename = result.payload.value("original_filename", "");
        out_server_filename = result.payload.value("server_filename", "");
        out_created_time = result.payload.value("created_time", "");

        if (out_file_id == 0 || out_server_filename.empty() ||
            out_original_filename.empty()) {
            LOG_WARN << "上传 Token 缺少关键字段";
            return false;
        }
        return true;
    } catch (...) {
        LOG_WARN << "上传 Token 字段解析异常";
        return false;
    }
}

bool TokenManager::verifyDownloadToken(const std::string &token,
                                       std::string &out_original_filename,
                                       std::string &out_server_filename) {
    auto result = verifyToken(token);
    if (!result.success ||
        result.payload.value("token_type", "") != "download") {
        return false;
    }
    out_original_filename = result.payload.value("original_filename", "");
    out_server_filename = result.payload.value("server_filename", "");
    return !out_server_filename.empty();
}

bool TokenManager::verifyDeleteToken(const std::string &token,
                                     std::string &out_server_filename) {
    auto result = verifyToken(token);
    if (!result.success || result.payload.value("token_type", "") != "delete") {
        return false;
    }
    out_server_filename = result.payload.value("server_filename", "");
    return !out_server_filename.empty();
}