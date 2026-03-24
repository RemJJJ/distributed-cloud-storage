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
                                        const std::string &username);

    /// @brief 生成datanodeToken
    nodeRegisterResponse
    generateNodeToken(const std::string &node_id,
                      const fileserver::net::InetAddress &addr);

    /// @brief 生成文件上传token
    fileUploadResponse generateFileToken(int userId, uint64_t file_id,
                                         const std::string &node_id,
                                         const std::string &server_filename);

    /// @brief 基础验证
    verifyResult verifyToken(const std::string &token);

    /// @brief 验证用户登录Token
    int verifyUserToken(const std::string &token);

    /// @brief 验证datanode Token
    std::string verifyNodeToken(const std::string &token);

    /// @brief 验证上传Token
    bool verifyUploadToken(const std::string &token, uint64_t &out_file_id,
                           std::string &out_server_filename);

    /// @brief 检查是否有效
    bool isValid(const std::string &token);

  private:
    TokenManager();

    std::string secretKey_;

    bool initialized_;
};