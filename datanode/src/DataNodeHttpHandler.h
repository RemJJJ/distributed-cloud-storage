#pragma once
#include "BaseHandler.h"
#include "FileStorage.h"
#include "base/Logging.h"
#include "net/EventLoop.h"
#include "net/HttpContext.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include "net/HttpServer.h"
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <ratio>
#include <regex>

namespace fs = std::filesystem;

// 文件上传上下文
class FileUploadContext {
  public:
    enum class State : uint8_t {
        kExpectHeaders,  // 等待头部
        kExpectContent,  // 等待内容
        kExpectBoundary, // 等待边界
        kComplete        // 上传完成

    };

    FileUploadContext(uint64_t fileID, const std::string &filename,
                      std::shared_ptr<FileStorage> &&storage);

    ~FileUploadContext();

    /// @brief 写入数据
    void writeData(const char *data, size_t len);

    /// @brief 获取已写入字节数
    uintmax_t getTotalBytes() const { return storage_->totalBytes(); }

    // /// @brief 获取原始文件名
    // const std::string &getOriginalFilename() const { return
    // originalFilename_; }

    ///@brief 获取文件id
    uint64_t getFileID() const { return fileID_; }

    /// @brief 获取服务器文件名
    const std::string &getFilename() const { return filename_; }

    /// @brief 获取文件存储
    template <typename T> std::shared_ptr<T> getStorage() const {
        return std::static_pointer_cast<T>(storage_);
    }

    /// @brief 设置边界
    void setBoundary(const std::string &boundary) { boundary_ = boundary; }

    /// @brief 获取边界
    const std::string &getBoundary() const { return boundary_; }

    /// @brief 获取当前状态
    State getState() const { return state_; }

    /// @brief 设置当前状态
    void setState(State state) {
        State oldState = state_;
        state_ = state;
    }

  private:
    uint64_t fileID_;                      // 文件ID
    std::string filename_;                 // 保存在服务器上的文件名
    std::shared_ptr<FileStorage> storage_; // 文件存储
    uintmax_t totalBytes_;                 // 已写入字节数
    State state_;                          // 当前状态
    std::string boundary_;                 // multipart边界
};

class DataNode;

class DataNodeHttpHandler : public BaseHandler {
  private:
    std::string uploadDir_; // 上传目录
    DataNode *datanode_;

  public:
    DataNodeHttpHandler(DataNode *datanode);

    ~DataNodeHttpHandler();

    void initRoutes();

    void onConnection(const TcpConnectionPtr &conn);

  private:
    ///@brief 验证Token
    bool validateToken(const std::string &token);

    ///@brief 处理文件上传
    bool handleFileUpload(const TcpConnectionPtr &conn, HttpRequest &req,
                          std::shared_ptr<HttpResponse> &resp);
};