#pragma once
#include "base/Logging.h"
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

class Config {

  public:
    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

    static Config &instance() {
        static Config inst;
        return inst;
    }

    // 加载配置文件
    bool load(const std::string &path) {
        try {
            std::ifstream file(path);
            if (!file.is_open()) {
                return false;
            }
            configData_ = json::parse(file);
            loaded_ = true;
            return true;
        } catch (const json::parse_error &e) {
            throw std::runtime_error(std::string("JSON 解析失败: ") + e.what());
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("配置加载失败: ") + e.what());
        }
    }

    // 检查是否已加载
    bool isLoaded() const { return loaded_; }

    // 删除或修复这段有问题的调试代码
    std::string getString(const std::string &key,
                          const std::string &defaultVal = "") const {
        if (!loaded_) {
            LOG_DEBUG << "Config not loaded";
            return defaultVal;
        }
        try {
            if (key.find('.') != std::string::npos) {
                const json &val = getValueByPath(key);
                return val.is_string() ? val.get<std::string>() : defaultVal;
            }
            return configData_.value(key, defaultVal);
        } catch (const std::exception &e) {
            LOG_DEBUG << "获取失败：" << e.what() << "，返回默认值";
            return defaultVal;
        }
    }

    // 获取整数
    int getInt(const std::string &key, int defaultVal = 0) const {
        if (!loaded_)
            return defaultVal;
        try {
            if (key.find('.') != std::string::npos) {
                const json &val = getValueByPath(key);
                return val.is_number_integer() ? val.get<int>() : defaultVal;
            }
            return configData_.value(key, defaultVal);
        } catch (...) {
            return defaultVal;
        }
    }

    // 获取布尔值
    bool getBool(const std::string &key, bool defaultVal = false) const {
        if (!loaded_)
            return defaultVal;
        try {
            if (key.find('.') != std::string::npos) {
                const json &val = getValueByPath(key);
                return val.is_boolean() ? val.get<bool>() : defaultVal;
            }
            return configData_.value(key, defaultVal);
        } catch (...) {
            return defaultVal;
        }
    }

    // 获取嵌套值（支持路径，如 "jwt.secret"）
    template <typename T>
    T get(const std::string &key, T defaultVal = T{}) const {
        if (!loaded_)
            return defaultVal;
        try {
            return configData_.value(key, defaultVal);
        } catch (...) {
            return defaultVal;
        }
    }

    // 检查键是否存在
    bool contains(const std::string &key) const {
        if (!loaded_)
            return false;
        return configData_.contains(key);
    }

  private:
    json configData_;
    bool loaded_ = false;

    Config() = default;

    const json &getValueByPath(const std::string &keyPath) const {
        std::istringstream iss(keyPath);
        std::string segment;
        const json *current = &configData_;

        // 先收集所有 segment
        std::vector<std::string> segments;
        while (std::getline(iss, segment, '.')) {
            segments.push_back(segment);
        }

        // 遍历所有 segment
        for (size_t i = 0; i < segments.size(); ++i) {
            const auto &seg = segments[i];

            auto it = current->find(seg);
            if (it == current->end()) {
                throw std::out_of_range("Key not found: " + keyPath);
            }

            // ★ 只有中间节点需要是对象，最后一个节点可以是任意类型
            if (i < segments.size() - 1 && !it->is_object()) {
                throw std::out_of_range("Intermediate key is not an object: " +
                                        keyPath);
            }

            current = &(*it);
        }
        return *current;
    }
};
