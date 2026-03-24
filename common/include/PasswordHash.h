#pragma once
#include <string>

// 用于bcrypt封装
class PasswordHash {
  public:
    // 生成密码哈希
    static std::string hash(const std::string &password);

    // 验证密码
    static bool verify(const std::string &passwrod, const std::string &hash);

    // 检查哈希是否需要重新生成
    static bool needsRehash(const std::string &hash);
};