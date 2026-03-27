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
    enum class State : std::uint8_t {
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

// 文件下载上下文
class FileDownContext {
  public:
    FileDownContext(const std::string &filepath,
                    const std::string &originalFilename)
        : filepath_(filepath), originalFilename_(originalFilename),
          fileSize_(0), currentPosition_(0), isComplete_(false) {
        // 获取文件大小
        fileSize_ = fs::file_size(filepath_);

        // 打开文件
        file_.open(filepath_, std::ios::binary | std::ios::in);
        if (!file_.is_open()) {
            LOG_ERROR << "Failed to open file: " << filepath_;
            throw std::runtime_error("Failed to open file: " + filepath_);
        }
        LOG_INFO << "Opening file for download: " << filepath_
                 << ", size: " << fileSize_;
    }

    ~FileDownContext() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    void seekTo(uintmax_t position) {
        if (!file_.is_open()) {
            throw std::runtime_error("File is not open: " + filepath_);
        }
        file_.seekg(position);
        currentPosition_ = position;
        isComplete_ = false;
    }

    bool readNextChunk(std::string &chunk) {
        if (!file_.is_open() || isComplete_) {
            return false;
        }

        const uintmax_t chunkSize = 1024 * 1024; // 1MB
        uintmax_t remainingBytes = fileSize_ - currentPosition_;
        uintmax_t bytesToRead = std::min(chunkSize, remainingBytes);

        if (bytesToRead == 0) {
            isComplete_ = true;
            return false;
        }

        std::vector<char> buffer(bytesToRead);
        file_.read(buffer.data(), bytesToRead);
        chunk.assign(buffer.data(), bytesToRead);
        currentPosition_ += bytesToRead;

        LOG_INFO << "Read chunk of " << bytesToRead
                 << " bytes, current position: " << currentPosition_ << "/"
                 << fileSize_;
        return true;
    }

    bool isComplete() const { return isComplete_; }
    uintmax_t getCurrentPosition() const { return currentPosition_; }
    uintmax_t getFileSize() const { return fileSize_; }
    const std::string &getOriginalFilename() const { return originalFilename_; }

  private:
    std::string filepath_;         // 文件路径
    std::string originalFilename_; // 原始文件名
    std::ifstream file_;           // 文件流
    uintmax_t fileSize_;           // 文件总大小
    uintmax_t currentPosition_;    // 当前读取位置
    bool isComplete_;              // 是否完成
};

class DataNode;

class DataNodeHttpHandler : public BaseHandler {
  private:
    std::string uploadDir_; // 上传目录
    DataNode *datanode_;

  public:
    DataNodeHttpHandler(DataNode *datanode);

    ~DataNodeHttpHandler();

    void initRoutes() override;

    void onConnection(const TcpConnectionPtr &conn) override;

  private:
    /// @brief 处理跨域
    void handleCROS();
    /// @brief 处理文件上传
    bool handleFileUpload(const TcpConnectionPtr &conn, HttpRequest &req,
                          std::shared_ptr<HttpResponse> &resp);

    /// @brief 处理文件下载
    bool handleFileDownload(const TcpConnectionPtr &conn, HttpRequest &req,
                            std::shared_ptr<HttpResponse> &resp);

    /// @brief 处理文件删除
    bool handleDeleteFile(const TcpConnectionPtr &conn, HttpRequest &req,
                          std::shared_ptr<HttpResponse> &resp);

    std::string getMimeType(const std::string &filename);
};