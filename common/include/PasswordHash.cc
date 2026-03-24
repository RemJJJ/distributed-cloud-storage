#include "PasswordHash.h"
#include "base/Logging.h"
#include <bcrypt/BCrypt.hpp>
#include <exception>
#include <string>

static constexpr int BCRYPT_COST = 12;

std::string PasswordHash::hash(const std::string &password) {
    try {
        // BCrypt::generateHash 自动生成盐并嵌入到结果中
        std::string hashed = BCrypt::generateHash(password, BCRYPT_COST);
        return hashed;
    } catch (const std::exception &e) {
        LOG_ERROR << "bcrypt hash failed: " << e.what();
        return "";
    }
}

bool PasswordHash::verify(const std::string &password,
                          const std::string &hash) {
    if (hash.empty() || hash.length() < 60) {
        LOG_WARN << "Invalid hash format";
        return false;
    }

    try {
        return BCrypt::validatePassword(password, hash);
    } catch (const std::exception &e) {
        LOG_ERROR << "bcrypt verify failed: " << e.what();
        return false;
    }
}

bool PasswordHash::needsRehash(const std::string &hash) {
    // 检查哈希字符串中的成本因子
    if (hash.length() < 7)
        return true;

    try {
        int currentCost = std::stoi(hash.substr(4, 2));
        return currentCost < BCRYPT_COST;
    } catch (...) {
    }
}