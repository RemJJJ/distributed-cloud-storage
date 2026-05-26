#pragma once
#include "nlohmann/json.hpp"
#include <chrono>
#include <cstdint>
#include <jwt-cpp/jwt.h>
#include <memory>
#include <mutex>
#include <net/InetAddress.h>
#include <stdexcept>

using json = nlohmann::json;

// Token管理器
class TokenManager {
  public:
    struct QoSPolicy {
        std::string service_level = "normal";
        std::string qos_mode = "elastic";
        bool throttle_enabled = false;
        uint64_t rate_limit_bps = 0;       // bytes per second
        uint64_t bucket_capacity_bytes = 0;
    };

    struct uploadTokenPayload {
        int user_id = 0;
        std::string username;
        uint64_t file_id = 0;
        std::string original_filename;
        std::string server_filename;
        std::string created_time;
        std::string scene_tag = "general";
        bool batch_mode = false;
        QoSPolicy qos_policy;
    };

    struct downloadTokenPayload {
        int user_id = 0;
        std::string username;
        std::string original_filename;
        std::string server_filename;
        std::string scene_tag = "general";
        std::string access_mode = "download";
        std::string video_quality = "original";
        std::string watermark_mode = "none";
        std::string watermark_text;
        QoSPolicy qos_policy;
    };

    struct hotCacheTokenPayload {
        std::string server_filename;
        uint64_t preload_bytes = 0;
        bool vip_priority = false;
    };

    struct userLoginResponse {
        std::string user_id;
        std::string token;
    };

    struct nodeRegisterResponse {
        std::string node_id;
        std::string token;
    };

    struct fileUploadResponse {
        std::string file_id;
        std::string token;
    };

    struct verifyResult {
        bool success;      // 是否成功
        std::string error; // 错误信息
        std::string subject;
        std::string issuer;
        int64_t expiration; // 过期时间戳
        json payload;       // 完整payload

        // 便捷构造
        static verifyResult Ok(const std::string &subject,
                               const std::string &issuer = "",
                               int64_t expiration = 0,
                               const json &payload = json{}) {
            return {true, "", subject, issuer, expiration, payload};
        }

        static verifyResult Fail(const std::string &error) {
            return {false, error, "", "", 0, json{}};
        }
    };

    ~TokenManager() = default;

    /// @brief 单例模式
    static TokenManager &instance() {
        static TokenManager instance;
        return instance;
    }

    /// @brief 初始化(启动时调用一次)
    static void init(const std::string &secretKey);

    /// @brief 检查是否已初始化
    static bool isInitialized();

    // 禁止拷贝
    TokenManager(const TokenManager &) = delete;
    TokenManager &operator=(const TokenManager &) = delete;

    /// @brief 生成用户登录Token
    userLoginResponse generateUserToken(int userId,
                                        const std::string &username,
                                        const std::string &service_level,
                                        bool is_admin);

    /// @brief 生成datanodeToken
    nodeRegisterResponse
    generateNodeToken(const std::string &node_id,
                      const fileserver::net::InetAddress &addr);

    /// @brief 生成文件上传token
    fileUploadResponse generateUploadToken(int userId, uint64_t file_id,
                                           const std::string &node_id,
                                           const std::string &username,
                                           const std::string &original_filename,
                                           const std::string &server_filename,
                                           const std::string &created_time,
                                           const std::string &scene_tag,
                                           bool batch_mode,
                                           const QoSPolicy &qos_policy);

    /// @brief 生成文件下载token
    std::string generateDownloadToken(int userId,
                                      const std::string &username,
                                      const std::string &original_filename,
                                      const std::string &server_filename,
                                      const std::string &scene_tag,
                                      const std::string &access_mode,
                                      const std::string &video_quality,
                                      const std::string &watermark_mode,
                                      const std::string &watermark_text,
                                      const QoSPolicy &qos_policy);

    /// @brief 生成文件删除Token
    std::string generateDeleteToken(const std::string &server_filename);

    /// @brief 生成热点缓存预热 Token
    std::string generateHotCacheToken(const std::string &server_filename,
                                      uint64_t preload_bytes,
                                      bool vip_priority);

    /// @brief 基础验证
    verifyResult verifyToken(const std::string &token);

    /// @brief 验证用户登录Token
    int verifyUserToken(const std::string &token);
    int verifyAdminToken(const std::string &token);

    /// @brief 验证datanode Token
    std::string verifyNodeToken(const std::string &token);

    /// @brief 验证上传Token
    bool verifyUploadToken(const std::string &token,
                           uploadTokenPayload &out_payload);

    /// @brief 验证下载Token
    bool verifyDownloadToken(const std::string &token,
                             downloadTokenPayload &out_payload);

    /// @brief 验证删除文件Token
    bool verifyDeleteToken(const std::string &token,
                           std::string &out_server_filename);

    /// @brief 验证热点缓存预热 Token
    bool verifyHotCacheToken(const std::string &token,
                             hotCacheTokenPayload &out_payload);

    /// @brief 检查是否有效
    bool isValid(const std::string &token);

  private:
    QoSPolicy parseQoSPolicy(const json &payload) const;

    TokenManager();

    std::string secretKey_;

    bool initialized_;
};
