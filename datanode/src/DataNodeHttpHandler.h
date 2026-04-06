#pragma once
#include "BaseHandler.h"
#include "FileStorage.h"
#include "PrefetchCache.h"
#include "TokenBucketRateLimiter.h"
#include "base/Logging.h"
#include "base/Thread.h"
#include "base/ThreadPool.h"
#include "net/EventLoop.h"
#include "net/HttpContext.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include "net/HttpServer.h"
#include <memory>
#include <nlohmann/json.hpp>
#include <ratio>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

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
                      std::shared_ptr<FileStorage> &&storage,
                      std::shared_ptr<TokenBucketRateLimiter> rateLimiter);

    ~FileUploadContext();

    /// @brief 写入数据
    void writeData(const char *data, size_t len);
    void throttleIfNeeded(size_t len);

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
    void setState(State state) { state_ = state; }

    bool releaseTransferCounter();

  private:
    uint64_t fileID_;                      // 文件ID
    std::string filename_;                 // 保存在服务器上的文件名
    std::shared_ptr<FileStorage> storage_; // 文件存储
    std::shared_ptr<TokenBucketRateLimiter> rateLimiter_;
    uintmax_t totalBytes_; // 已写入字节数
    State state_;          // 当前状态
    std::string boundary_; // multipart边界
    bool transferCounterReleased_ = false;
};

// 文件下载上下文
class FileDownContext {
  public:
    FileDownContext(const std::string &filepath,
                    const std::string &originalFilename,
                    const std::string &sceneTag,
                    const std::string &serviceLevel,
                    std::shared_ptr<TokenBucketRateLimiter> rateLimiter,
                    std::shared_ptr<PrefetchCache> prefetchCache,
                    ThreadPool *ioThreadPool);

    ~FileDownContext();

    void seekTo(uintmax_t position);

    bool readNextChunk(std::string &chunk);

    bool isComplete() const { return isComplete_; }
    uintmax_t getCurrentPosition() const { return currentPosition_; }
    uintmax_t getFileSize() const { return fileSize_; }
    const std::string &getOriginalFilename() const { return originalFilename_; }
    void throttleIfNeeded(size_t len) {
        if (rateLimiter_) {
            rateLimiter_->consume(len);
        }
    }
    bool releaseTransferCounter() {
        if (transferCounterReleased_) {
            return false;
        }
        transferCounterReleased_ = true;
        return true;
    }

  private:
    bool shouldUsePrefetchCache() const;
    bool isVipUser() const;
    bool isLearningUser() const;
    uintmax_t getPrefetchWindowBytes() const;
    uintmax_t getPrefetchLowWaterBytes() const;
    std::string buildCacheKey(uintmax_t offset) const;
    void schedulePrefetchWindow(uintmax_t nextOffset);

    std::string filepath_;         // 文件路径
    std::string originalFilename_; // 原始文件名
    std::string sceneTag_;
    std::string serviceLevel_;
    int fd_ = -1;
    std::shared_ptr<TokenBucketRateLimiter> rateLimiter_;
    std::shared_ptr<PrefetchCache> prefetchCache_;
    uintmax_t fileSize_;        // 文件总大小
    uintmax_t currentPosition_; // 当前读取位置
    bool isComplete_;           // 是否完成
    bool transferCounterReleased_ = false;

    // 线程池指针
    ThreadPool *ioThreadPool_;
};

struct ConnectionTransferState {
    std::shared_ptr<FileUploadContext> uploadContext;
    std::shared_ptr<FileDownContext> downloadContext;
};

class DataNode;

class DataNodeHttpHandler : public BaseHandler {
  private:
    std::string uploadDir_; // 上传目录
    DataNode *datanode_;
    std::shared_ptr<PrefetchCache> prefetchCache_;

    std::mutex transferMutex_; // 连接数锁
    std::unordered_map<std::string, std::shared_ptr<ConnectionTransferState>>
        activeTransfers_;

    ThreadPool threadPool_;

  public:
    DataNodeHttpHandler(DataNode *datanode, int numThreads = 4);

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

    bool handleTextPreview(const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp);

    /// @brief 处理文件删除
    bool handleDeleteFile(const TcpConnectionPtr &conn, HttpRequest &req,
                          std::shared_ptr<HttpResponse> &resp);

    bool isPreviewableTextFile(const std::string &filename) const;

    std::string getMimeType(const std::string &filename);
};
