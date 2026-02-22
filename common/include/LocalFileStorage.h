#pragma once

#include "../../third_party/muduo/base/noncopyable.h"
#include "FileStorage.h"
#include <fstream>
#include <memory>

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

    /// @brief 获取已写入字节数
    uintmax_t totalBytes() const override { return total_bytes_; }

    /// @brief 获取保存在服务器上的文件名
    const std::string &filename() const override { return filename_; }

  private:
    std::string filename_; // 保存在服务器上的文件名
    std::ofstream file_;
    uintmax_t total_bytes_; // 已写入字节数
};