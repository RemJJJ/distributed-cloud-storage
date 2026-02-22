#pragma once
#include "../../common/include/FileStorage.h"
#include "../../common/include/LocalFileStorage.h"
#include <memory>
#include <string>
class FileUploadContext {
  public:
    enum class State {
        kExpectHeaders,  // 等待头部
        kExpectContent,  // 等待内容
        kExpectBoundary, // 等待边界
        kComplete        // 上传完成
    };

    FileUploadContext(const std::string &filename,
                      const std::string &originalFilename,
                      std::unique_ptr<FileStorage> storage = nullptr);
    ~FileUploadContext();

    /// @brief 写入数据
    void writeData(const char *data, size_t len);

    /// @brief 获取已写入字节数
    uintmax_t getTotalBytes() const { return storage_->totalBytes(); }

    /// @brief 获取原始文件名
    const std::string &getOriginalFilename() const { return originalFilename_; }

    /// @brief 获取服务器文件名
    const std::string &getFilename() const { return storage_->filename(); }

    void setBoundary(const std::string &boundary) { boundary_ = boundary; }
    State getState() const { return state_; }
    void setState(State state) { state_ = state; }
    const std::string &getBoundary() const { return boundary_; }

  private:
    std::string originalFilename_;         // 原始文件名
    std::unique_ptr<FileStorage> storage_; // 文件存储
    State state_;                          // 当前状态
    std::string boundary_;                 // multipart边界
};