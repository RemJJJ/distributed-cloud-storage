#pragma once
#include "../../common/include/FileStorage.h"
#include "../../common/include/LocalFileStorage.h"
#include "../../third_party/muduo/base/Logging.h"
#include <memory>
#include <string>
class FileUploadContext
    : public std::enable_shared_from_this<FileUploadContext> {
  public:
    enum class State {
        kExpectHeaders,  // 等待头部
        kExpectContent,  // 等待内容
        kExpectBoundary, // 等待边界
        kComplete,       // 上传完成
        kFailed          // 上传失败
    };

    FileUploadContext(const std::string &filename,
                      const std::string &originalFilename,
                      std::shared_ptr<FileStorage> storage = nullptr);
    ~FileUploadContext();

    /// @brief 写入数据
    void writeData(const char *data, size_t len);

    /// @brief 获取已写入字节数
    uintmax_t getTotalBytes() const { return storage_->totalBytes(); }

    /// @brief 获取原始文件名
    const std::string &getOriginalFilename() const { return originalFilename_; }

    /// @brief 获取服务器文件名
    const std::string &getFilename() const { return storage_->filename(); }

    /// @brief 获取文件存储
    template <typename T> std::shared_ptr<T> getStorage() const {
        return std::static_pointer_cast<T>(storage_);
    }

    /// @brief 设置切换状态回调
    void setStateChangeCallback(const std::function<void(State newState)> &cb) {
        stateChangeCallback_ = cb;
    }

    void setBoundary(const std::string &boundary) { boundary_ = boundary; }
    State getState() const { return state_; }
    void setState(State state) {
        State oldState = state_;
        state_ = state;
        if (stateChangeCallback_ && oldState != state_) {
            LOG_DEBUG << "执行状态更新回调, 状态：" << static_cast<int>(state_);
            stateChangeCallback_(state);
        }
    }
    const std::string &getBoundary() const { return boundary_; }

    /// @brief 标记上传完成
    void setUploadComplete(bool complete) { uploadComplete_ = complete; }
    bool isUploadComplete() const { return uploadComplete_; }

  private:
    std::string filename_;                 // 保存在服务器上的文件名
    std::string originalFilename_;         // 原始文件名
    std::shared_ptr<FileStorage> storage_; // 文件存储
    uintmax_t totalBytes_;                 // 已写入字节
    State state_;                          // 当前状态
    std::string boundary_;                 // multipart边界
    std::function<void(State newState)> stateChangeCallback_; // 状态切换回调
    bool uploadComplete_;                                     // 是否上传完成
};