#pragma once

#include "../../third_party/muduo/base/noncopyable.h"
#include "FileStorage.h"
#include <filesystem>
#include <fstream>
#include <memory>
namespace fs = std::filesystem;

class FileUploadContext;

class LocalFileStorage : public FileStorage, fileserver::noncopyable {
  public:
    LocalFileStorage();
    LocalFileStorage(const std::string &filename);
    ~LocalFileStorage();

    /// @brief 打开文件
    bool open(const std::string &filename) override;

    /// @brief 写文件
    bool write(const char *data, size_t len) override;

    /// @brief 关闭文件
    bool close() override;

    /// @brief 设置close命令发送完成回调
    void setCloseCmdSentCallback(
        const std::function<void(bool success)> &cb) override;

    /// @brief 获取已写入字节数
    uintmax_t totalBytes() const override { return total_bytes_; }

    /// @brief 文件是否打开
    bool isOpen() const override { return file_.is_open(); }

    /// @brief 获取保存在服务器上的文件名
    const std::string &filename() const override { return filename_; }

  private:
    std::string filename_; // 保存在服务器上的文件名
    std::ofstream file_;
    uintmax_t total_bytes_; // 已写入字节数
};