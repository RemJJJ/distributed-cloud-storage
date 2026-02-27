#pragma once
#include <cstdint>
#include <functional>
#include <string>

class FileStorage {
  public:
    FileStorage() = default;
    virtual ~FileStorage() = default;

    /// @brief 打开文件
    virtual bool open(const std::string &filename) = 0;

    /// @brief 写文件
    virtual bool write(const char *data, size_t len) = 0;

    // /// @brief 读文件
    // virtual bool read(const std::string &filename, const char *data) = 0;

    /// @brief 关闭文件
    virtual bool close() = 0;

    /// @brief 设置close命令发送完成回调
    virtual void
    setCloseCmdSentCallback(const std::function<void(bool success)> &cb) = 0;

    /// @brief 获取总字节数
    virtual uintmax_t totalBytes() const = 0;

    /// @brief 文件是否打开
    virtual bool isOpen() const = 0;

    /// @brief 获取文件名
    virtual const std::string &filename() const = 0;
};