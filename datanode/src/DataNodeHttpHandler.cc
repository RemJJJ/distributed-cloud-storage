#include "DataNodeHttpHandler.h"
#include "DataNode.h"
#include "LocalFileStorage.h"
#include "MasterClient.h"
#include "TokenManager.h"
#include "base/ThreadPool.h"
#include "net/Callbacks.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr uintmax_t kDownloadChunkSize = 1024 * 1024; // 1MB
constexpr uintmax_t kLearningLargeFileThreshold = 50ULL * 1024ULL * 1024ULL;
constexpr uintmax_t kVipPrefetchWindowBytes = 10ULL * 1024ULL * 1024ULL;
constexpr uintmax_t kLearningPrefetchWindowBytes = 5ULL * 1024ULL * 1024ULL;
constexpr uintmax_t kPreviewFileSizeLimit = 2ULL * 1024ULL * 1024ULL;

std::shared_ptr<TokenBucketRateLimiter>
buildRateLimiter(const TokenManager::QoSPolicy &qosPolicy) {
    if (!qosPolicy.throttle_enabled || qosPolicy.rate_limit_bps == 0 ||
        qosPolicy.bucket_capacity_bytes == 0) {
        return nullptr;
    }

    return std::make_shared<TokenBucketRateLimiter>(
        qosPolicy.rate_limit_bps, qosPolicy.bucket_capacity_bytes);
}
} // namespace

// --------------FileUploadContext--------------
FileUploadContext::FileUploadContext(
    uint64_t fileID, const std::string &filename,
    std::shared_ptr<FileStorage> &&storage,
    std::shared_ptr<TokenBucketRateLimiter> rateLimiter)
    : fileID_(fileID), filename_(filename), storage_(std::move(storage)),
      rateLimiter_(std::move(rateLimiter)), totalBytes_(0),
      state_(State::kExpectHeaders), boundary_("") {
    if (!storage_) {
        LOG_ERROR << "FileStorage not initialized";
    }
    if (!storage_->open(filename_)) {
        throw std::runtime_error("Failed to open file: " + filename_);
    }
    LOG_INFO << "FileUploadContext created: " << filename;
}

FileUploadContext::~FileUploadContext() {
    if (storage_ && storage_->isOpen()) {
        storage_->close();
    }
}

// 写入数据：转发给LocalStorage
void FileUploadContext::writeData(const char *data, size_t len) {
    if (!storage_) {
        LOG_ERROR << "FileStorage not initialized";
        throw std::runtime_error("FileStorage not initialized");
    }
    try {
        storage_->write(data, len);
        totalBytes_ += len;
        LOG_INFO << "Wrote " << len
                 << "bytes to DataNode, total: " << totalBytes_;
    } catch (const std::exception &e) {
        LOG_ERROR << "FileStorage: write failed - " << e.what();
        throw; // 向上层抛出异常
    }
}

void FileUploadContext::throttleIfNeeded(size_t len) {
    if (rateLimiter_) {
        rateLimiter_->consume(len);
    }
}

bool FileUploadContext::releaseTransferCounter() {
    if (transferCounterReleased_) {
        return false;
    }
    transferCounterReleased_ = true;
    return true;
}

// --------------FileDownContext--------------
FileDownContext::FileDownContext(
    const std::string &filepath, const std::string &originalFilename,
    const std::string &sceneTag, const std::string &serviceLevel,
    std::shared_ptr<TokenBucketRateLimiter> rateLimiter,
    std::shared_ptr<PrefetchCache> prefetchCache, ThreadPool *ioThreadPool)
    : filepath_(filepath), originalFilename_(originalFilename),
      sceneTag_(sceneTag), serviceLevel_(serviceLevel), fd_(-1),
      rateLimiter_(std::move(rateLimiter)),
      prefetchCache_(std::move(prefetchCache)), fileSize_(0),
      currentPosition_(0), isComplete_(false), ioThreadPool_(ioThreadPool) {
    fileSize_ = fs::file_size(filepath_);
    fd_ = ::open(filepath_.c_str(), O_RDONLY);
    if (fd_ < 0) {
        LOG_ERROR << "Failed to open file: " << filepath_
                  << ", errno=" << std::strerror(errno);
        throw std::runtime_error("Failed to open file: " + filepath_);
    }
    LOG_INFO << "Opening file for download: " << filepath_
             << ", size: " << fileSize_ << ", scene_tag=" << sceneTag_;
}

FileDownContext::~FileDownContext() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void FileDownContext::seekTo(uintmax_t position) {
    if (fd_ < 0) {
        throw std::runtime_error("File is not open: " + filepath_);
    }
    currentPosition_ = position;
    isComplete_ = false;
}

bool FileDownContext::readNextChunk(std::string &chunk) {
    if (fd_ < 0 || isComplete_) {
        return false;
    }

    uintmax_t remainingBytes = fileSize_ - currentPosition_;
    uintmax_t bytesToRead = std::min(kDownloadChunkSize, remainingBytes);
    if (bytesToRead == 0) {
        isComplete_ = true;
        return false;
    }

    std::vector<char> buffer;
    bool cacheHit = false;

    // 如果是学习场景，先去缓存里找找看
    if (shouldUsePrefetchCache()) {
        cacheHit = prefetchCache_->get(buildCacheKey(currentPosition_), buffer);
        if (cacheHit && buffer.size() > bytesToRead) {
            buffer.resize(bytesToRead);
        }
    }

    if (!cacheHit) {
        buffer.resize(bytesToRead);
        ssize_t readBytes = ::pread(fd_, buffer.data(), bytesToRead,
                                    static_cast<off_t>(currentPosition_));
        if (readBytes <= 0) {
            isComplete_ = true;
            return false;
        }
        buffer.resize(static_cast<size_t>(readBytes));

        // 主线程读完磁盘后，如果开启了预取，顺手把这块数据也塞进缓存
        if (shouldUsePrefetchCache()) {
            prefetchCache_->put(buildCacheKey(currentPosition_), buffer,
                                isVipUser());
        }
    }

    chunk.assign(buffer.data(), buffer.size());
    currentPosition_ += buffer.size();
    if (currentPosition_ >= fileSize_) {
        isComplete_ = true;
    }

    // 核心创新点：触发异步预取！
    // 如果是学习场景，不仅把当前这块发给用户，还调用线程池把后面的数据提前读出来！
    if (shouldUsePrefetchCache()) {
        LOG_DEBUG << "start to schedule prefetch window";
        schedulePrefetchWindow(currentPosition_);
    }

    LOG_INFO << (cacheHit ? "Prefetch cache hit" : "Disk read")
             << ", bytes: " << buffer.size()
             << ", current position: " << currentPosition_ << "/" << fileSize_;
    return true;
}

bool FileDownContext::shouldUsePrefetchCache() const {
    if (!prefetchCache_) {
        return false;
    }

    if (isVipUser()) {
        return true;
    }

    return isLearningUser() && fileSize_ > kLearningLargeFileThreshold;
}

bool FileDownContext::isVipUser() const { return serviceLevel_ == "vip"; }

bool FileDownContext::isLearningUser() const { return sceneTag_ == "learning"; }

uintmax_t FileDownContext::getPrefetchWindowBytes() const {
    if (isVipUser()) {
        return kVipPrefetchWindowBytes;
    }
    if (isLearningUser()) {
        return kLearningPrefetchWindowBytes;
    }
    return 0;
}

uintmax_t FileDownContext::getPrefetchLowWaterBytes() const {
    const uintmax_t windowBytes = getPrefetchWindowBytes();
    return std::max<uintmax_t>(kDownloadChunkSize, windowBytes / 2);
}

std::string FileDownContext::buildCacheKey(uintmax_t offset) const {
    return filepath_ + "_" + std::to_string(offset);
}

void FileDownContext::schedulePrefetchWindow(uintmax_t nextOffset) {
    if (!shouldUsePrefetchCache() || nextOffset >= fileSize_) {
        return;
    }

    const uintmax_t lowWaterBytes = getPrefetchLowWaterBytes();
    const uintmax_t prefetchWindowBytes = getPrefetchWindowBytes();

    // 向全局缓存查询，从nextOffset 开始，后面连续有多少数据是安全的？
    uintmax_t safeBytes = prefetchCache_->getContinuousCachedBytes(
        filepath_, nextOffset, prefetchWindowBytes, kDownloadChunkSize);

    // 如果安全的数据量还高于低水位线，说明缓存充足，不需要唤醒后台线程
    if (safeBytes > lowWaterBytes) {
        LOG_INFO << "Global cache is sufficient (safety margin:" << safeBytes
                 << " > lowWater:" << lowWaterBytes << "), skip prefetch";
        return;
    }
    // 确保线程池指针不为空
    if (!ioThreadPool_) {
        LOG_WARN << "IO ThreadPool is null, skipping prefetch";
        return;
    }

    const std::string filepath = filepath_;
    const uintmax_t fileSize = fileSize_;
    const bool isVip = isVipUser();
    auto cache = prefetchCache_;

    uintmax_t actualStartOffset = nextOffset + safeBytes;

    ioThreadPool_->run([filepath, fileSize, actualStartOffset, safeBytes,
                        prefetchWindowBytes, isVip, cache]() {
        int fd = ::open(filepath.c_str(), O_RDONLY);
        if (fd < 0) {
            return;
        }

        uintmax_t offset = actualStartOffset;
        // 还要读多少字节(总窗口-已经安全的字节)
        uintmax_t remainingBytes = prefetchWindowBytes - safeBytes;
        uintmax_t prefetchedBytes = 0;
        while (offset < fileSize && prefetchedBytes < remainingBytes) {
            uintmax_t bytesToRead =
                std::min(kDownloadChunkSize, fileSize - offset);
            std::string key = filepath + "_" + std::to_string(offset);
            if (!cache->shouldAdmitPrefetch(static_cast<size_t>(bytesToRead),
                                            isVip)) {
                break;
            }
            if (!cache->tryMarkLoading(key)) {
                offset += bytesToRead;
                continue;
            }

            std::vector<char> buffer(bytesToRead);
            ssize_t readBytes = ::pread(fd, buffer.data(), bytesToRead,
                                        static_cast<off_t>(offset));
            if (readBytes <= 0) {
                cache->cancelLoading(key);
                offset += bytesToRead;
                continue;
            }

            buffer.resize(static_cast<size_t>(readBytes));
            cache->put(key, std::move(buffer), isVip);
            offset += static_cast<uintmax_t>(readBytes);
            prefetchedBytes += static_cast<uintmax_t>(readBytes);
            LOG_INFO << "prefetched:" << prefetchedBytes
                     << "bytes. Total:" << cache->currentBytes();
        }

        ::close(fd);
    });
}

// --------------Handler--------------
DataNodeHttpHandler::DataNodeHttpHandler(DataNode *datanode, int numThreads)
    : uploadDir_("uploads"), datanode_(datanode),
      prefetchCache_(std::make_shared<PrefetchCache>()),
      threadPool_("DNHttpHandlerThreadPool") {
    threadPool_.start(numThreads);
    // 创建上传目录
    if (!fs::exists(uploadDir_)) {
        LOG_DEBUG << "创建上传目录: " << uploadDir_;
        fs::create_directory(uploadDir_);
    }

    // 初始化路由表
    initRoutes();
    LOG_INFO << "Current prefetch bytes: " << prefetchCache_->currentBytes();
}

DataNodeHttpHandler::~DataNodeHttpHandler() {}

std::string DataNodeHttpHandler::getMimeType(const std::string &filename) {
    static std::unordered_map<std::string, std::string> mimeMap = {
        {".mp4", "video/mp4"},   {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"}, {".png", "image/png"},
        {".gif", "image/gif"},   {".pdf", "application/pdf"},
        {".txt", "text/plain"},  {".zip", "application/zip"},
        {".html", "text/html"},  {".mp3", "audio/mpeg"}};

    std::string ext;
    size_t pos = filename.find_last_of('.');
    if (pos != std::string::npos) {
        ext = filename.substr(pos);
    }

    if (mimeMap.count(ext)) {
        return mimeMap[ext];
    }

    return "application/octet-stream";
}

/// TODO: 初始化路由表
void DataNodeHttpHandler::initRoutes() {
    addRoute("/api/datanode/upload", HttpRequest::kOptions,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleFileUpload(conn, req, resp);
             });
    addRoute("/api/datanode/upload", HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleFileUpload(conn, req, resp);
             });
    addRoute("/api/datanode/download", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleFileDownload(conn, req, resp);
             });
    addRoute("/api/datanode/text_preview", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleTextPreview(conn, req, resp);
             });
    addRoute("/api/datanode/delete", fileserver::net::HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleDeleteFile(conn, req, resp);
             });
}

void DataNodeHttpHandler::onConnection(const TcpConnectionPtr &conn) {
    if (conn->connected()) {
        LOG_INFO << "New connection from " << conn->peerAddress().toIpPort();
        // 为每一个新连接创建一个HttpContext
        conn->setContext(std::make_shared<HttpContext>());
    } else {
        LOG_INFO << "Connection closed from " << conn->peerAddress().toIpPort();
        // 清理上下文

        std::shared_ptr<ConnectionTransferState> transferState;

        {
            std::lock_guard<std::mutex> lock(transferMutex_);
            auto it = activeTransfers_.find(conn->name());
            if (it != activeTransfers_.end()) {
                transferState = it->second;
                activeTransfers_.erase(it); // 移除记录
            }
        }

        // 如果找到了状态，说明任务被异常中断，执行减扣
        if (transferState) {
            if (transferState->uploadContext &&
                transferState->uploadContext->releaseTransferCounter()) {
                LOG_INFO << "Abnormal disconnection: cleaning up upload "
                            "context and reducing the active count";
                datanode_->decActiveUpload();
            }
            if (transferState->downloadContext &&
                transferState->downloadContext->releaseTransferCounter()) {
                LOG_INFO << "Abnormal disconnection: cleaning up download "
                            "context and reducing the active count";
                datanode_->decActiveDownload();
            }
        }
        conn->setContext(std::shared_ptr<void>());
    }
}

bool DataNodeHttpHandler::handleFileUpload(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {

    // ==========================================
    // 1. 拦截并处理浏览器的 CORS 预检请求 (OPTIONS)
    // ==========================================
    if (req.method() == fn::HttpRequest::kOptions) {
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        // 允许所有域名跨域（生产环境可以改成具体的域名如
        // http://localhost:8000）
        resp->addHeader("Access-Control-Allow-Origin", "*");
        // 允许的方法
        resp->addHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
        // 允许前端携带的自定义请求头
        resp->addHeader("Access-Control-Allow-Headers",
                        "Authorization, Content-Type, X-File-Name");
        // 让浏览器缓存这个预检结果 24 小时，不用每次上传都发 OPTIONS
        resp->addHeader("Access-Control-Max-Age", "86400");

        // OPTIONS 请求不需要 Body，直接返回 true 结束处理
        return true;
    }

    // 即使是真正的 POST响应，也必须带上跨域头，否则前端拿不到响应数据！
    resp->addHeader("Access-Control-Allow-Origin", "*");

    // 验证Token
    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
        sendError(resp, "缺少上传 Token", fn::HttpResponse::k401Unauthorized,
                  conn);
        return true;
    }

    TokenManager::uploadTokenPayload uploadPayload;
    std::string uploadToken = authHeader.substr(7);
    if (!TokenManager::instance().verifyUploadToken(uploadToken,
                                                    uploadPayload)) {
        sendError(resp, "上传 Token 无效或已过期",
                  HttpResponse::k401Unauthorized, conn);
        return true;
    }

    // 获取HttpContext
    auto httpContext =
        std::static_pointer_cast<HttpContext>(conn->getContext());
    if (!httpContext) {
        LOG_ERROR << "HttpContext is null";
        sendError(resp, "Internal Server Error",
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }
    LOG_INFO << "body.size() = " << req.body().size();

    std::shared_ptr<ConnectionTransferState> transferState;
    {
        std::lock_guard<std::mutex> lock(transferMutex_);
        auto it = activeTransfers_.find(conn->name());
        if (it != activeTransfers_.end()) {
            transferState = it->second;
        } else {
            transferState = std::make_shared<ConnectionTransferState>();
            activeTransfers_[conn->name()] = transferState;
        }
    }

    // 尝试获取已经存在的上传上下文
    std::shared_ptr<FileUploadContext> uploadContext =
        transferState->uploadContext;

    if (!uploadContext) {
        try {
            std::string filePath =
                uploadDir_ + "/" + uploadPayload.server_filename;
            auto localStorage = std::make_shared<LocalFileStorage>();
            uploadContext = std::make_shared<FileUploadContext>(
                uploadPayload.file_id, filePath, std::move(localStorage),
                buildRateLimiter(uploadPayload.qos_policy));
            transferState->uploadContext = uploadContext;
            LOG_INFO << "开始接收文件: " << uploadPayload.server_filename
                     << ", service_level="
                     << uploadPayload.qos_policy.service_level
                     << ", qos_mode=" << uploadPayload.qos_policy.qos_mode
                     << ", throttle="
                     << uploadPayload.qos_policy.throttle_enabled;

            // 新的上传任务开始，计数器+1
            datanode_->incActiveUpload();
        } catch (std::exception &e) {
            sendError(resp, "无法创建文件",
                      HttpResponse::k500InternalServerError, conn);
            return true;
        }
    }

    if (!req.body().empty()) {
        try {
            uploadContext->throttleIfNeeded(req.body().size());
            uploadContext->writeData(req.body().data(), req.body().size());
            req.setBody(""); // 清空内存，防止 OOM
        } catch (std::exception &e) {
            sendError(resp, "写入磁盘失败",
                      HttpResponse::k500InternalServerError, conn);
            return true;
        }
    }

    // 检查是否完成
    if (uploadContext->getState() == FileUploadContext::State::kComplete ||
        httpContext->gotAll()) {
        // 上传任务完成，计数器-1
        if (uploadContext->releaseTransferCounter()) {
            datanode_->decActiveUpload();
        }

        // 从Map中安全移除状态
        {
            std::lock_guard<std::mutex> lock(transferMutex_);
            activeTransfers_.erase(conn->name());
        }

        auto file_id = uploadContext->getFileID();
        auto server_filename = uploadContext->getFilename();
        auto stored_size = uploadContext->getTotalBytes();

        json respJson = {{"code", 0},
                         {"msg", "Upload success"},
                         {"file",
                          {{"id", file_id},
                           {"name", server_filename.substr(8)},
                           {"originalName", uploadPayload.original_filename},
                           {"size", stored_size},
                           {"createdAt", uploadPayload.created_time}}}};
        //   respJson["data"]["file_md5"] = uploadContext->getFileMd5();
        // 优化：只 dump 一次
        std::string bodyStr = respJson.dump();
        resp->setStatusCode(fileserver::net::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->addHeader("Connection", "close");
        resp->setBody(bodyStr);
        resp->addHeader("Content-Length", std::to_string(bodyStr.size()));

        // 清理上下文
        transferState->uploadContext.reset();
        if (!transferState->downloadContext) {
            httpContext->setContext(nullptr);
        }

        // 设置写完成回调以关闭连接
        conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
            connection->shutdown();
            return true;
        });

        // ---------------- 第二步：异步通知 Master ----------------
        // 不要在当前 onMessage 线程里同步调用 Master，避免阻塞网络线程
        // 用 muduo 的 EventLoop::runInLoop 或者线程池异步发送
        // notifyMasterAsync(uploadContext);
        auto masterClient = datanode_->getMasterClient();
        masterClient->notifyUploadFinish(std::to_string(file_id),
                                         server_filename, stored_size);

        return true;
    } else {
        LOG_INFO << "Waiting for more data, current state: "
                 << static_cast<uint8_t>(uploadContext->getState());
        return false;
    }
}

bool DataNodeHttpHandler::handleFileDownload(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {

    if (req.method() == fileserver::net::HttpRequest::kOptions) {
        // 处理跨域 (重要：视频播放需要 Range 头，必须在允许列表里)
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Headers", "Range, Authorization");
        resp->addHeader("Access-Control-Expose-Headers",
                        "Content-Range, Content-Length, Accept-Ranges");
        return true;
    }

    // 即使是真正的 POST响应，也必须带上跨域头，否则前端拿不到响应数据！
    resp->addHeader("Access-Control-Allow-Origin", "*");

    std::string token = req.getQuery("token");
    TokenManager::downloadTokenPayload downloadPayload;
    if (!TokenManager::instance().verifyDownloadToken(token, downloadPayload)) {
        sendError(resp, "非法请求", HttpResponse::k403Forbidden, conn);
        return true;
    }

    std::string filepath = uploadDir_ + "/" + downloadPayload.server_filename;
    if (!fs::exists(filepath)) {
        sendError(resp, "文件丢失", HttpResponse::k404NotFound, conn);
        return true;
    }

    uintmax_t fileSize = fs::file_size(filepath);

    std::string rangeHeader = req.getHeader("Range");
    uintmax_t startPos = 0;
    uintmax_t endPos = fileSize - 1;
    bool isRange = false;

    if (!rangeHeader.empty()) {
        std::regex rangeRegex("bytes=(\\d+)-(\\d*)");
        std::smatch matches;
        if (std::regex_search(rangeHeader, matches, rangeRegex)) {
            startPos = std::stoull(matches[1]);
            if (!matches[2].str().empty())
                endPos = std::stoull(matches[2]);
            isRange = true;
        }
    }

    resp->setStatusCode(isRange ? HttpResponse::k206PartialContent
                                : HttpResponse::k200Ok);
    resp->addHeader("Accept-Ranges", "bytes");
    resp->addHeader("Content-Disposition",
                    "attachment; filename=\"" +
                        downloadPayload.original_filename + "\"");
    resp->setContentType(getMimeType(downloadPayload.original_filename));
    if (isRange) {
        resp->addHeader("Content-Range", "bytes " + std::to_string(startPos) +
                                             "-" + std::to_string(endPos) +
                                             "/" + std::to_string(fileSize));
    }
    resp->addHeader("Content-Length", std::to_string(endPos - startPos + 1));

    auto httpContext =
        std::static_pointer_cast<HttpContext>(conn->getContext());
    if (!httpContext) {
        sendError(resp, "Internal Server Error",
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }

    auto transferState = std::make_shared<ConnectionTransferState>();

    auto downContext = std::make_shared<FileDownContext>(
        filepath, downloadPayload.original_filename, downloadPayload.scene_tag,
        downloadPayload.qos_policy.service_level,
        buildRateLimiter(downloadPayload.qos_policy), prefetchCache_,
        &threadPool_);
    downContext->seekTo(startPos);
    transferState->downloadContext = downContext;

    {
        std::lock_guard<std::mutex> lock(transferMutex_);
        activeTransfers_[conn->name()] = transferState;
    }
    datanode_->incActiveDownload();

    LOG_INFO << "Start download: " << downloadPayload.server_filename
             << ", service_level=" << downloadPayload.qos_policy.service_level
             << ", qos_mode=" << downloadPayload.qos_policy.qos_mode
             << ", throttle=" << downloadPayload.qos_policy.throttle_enabled;

    conn->setWriteCompleteCallback(
        [this, conn_name = conn->name(),
         downContext](const TcpConnectionPtr &connection) {
            std::string chunk;
            if (downContext->readNextChunk(chunk)) {
                downContext->throttleIfNeeded(chunk.size());
                connection->send(chunk);
                return true;
            } else {
                if (downContext->releaseTransferCounter()) {
                    datanode_->decActiveDownload();
                }
                {
                    std::lock_guard<std::mutex> lock(transferMutex_);
                    activeTransfers_.erase(conn_name);
                }
                connection->shutdown();
                return true;
            }
            return true;
        });

    return true;
}

bool DataNodeHttpHandler::isPreviewableTextFile(
    const std::string &filename) const {
    static const std::unordered_set<std::string> previewableExts = {
        "cpp", "cc",  "c",    "h",  "hpp", "py",  "js", "json",
        "ts",  "tsx", "java", "go", "rs",  "txt", "md"};

    auto pos = filename.find_last_of('.');
    if (pos == std::string::npos || pos + 1 >= filename.size()) {
        return false;
    }

    std::string ext = filename.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return previewableExts.count(ext) > 0;
}

bool DataNodeHttpHandler::handleTextPreview(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {
    if (req.method() == fileserver::net::HttpRequest::kOptions) {
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Authorization");
        return true;
    }

    resp->addHeader("Access-Control-Allow-Origin", "*");

    const std::string token = req.getQuery("token");
    TokenManager::downloadTokenPayload downloadPayload;
    if (!TokenManager::instance().verifyDownloadToken(token, downloadPayload)) {
        sendError(resp, "非法请求", HttpResponse::k403Forbidden, conn);
        return true;
    }

    if (downloadPayload.scene_tag != "development") {
        sendError(resp, "当前场景不支持代码预览", HttpResponse::k403Forbidden,
                  conn);
        return true;
    }

    if (!isPreviewableTextFile(downloadPayload.original_filename)) {
        sendError(resp, "该文件类型不支持文本预览",
                  HttpResponse::k400BadRequest, conn);
        return true;
    }

    const std::string filepath =
        uploadDir_ + "/" + downloadPayload.server_filename;
    if (!fs::exists(filepath)) {
        sendError(resp, "文件丢失", HttpResponse::k404NotFound, conn);
        return true;
    }

    const uintmax_t fileSize = fs::file_size(filepath);
    if (fileSize > kPreviewFileSizeLimit) {
        sendError(resp, "预览仅支持 2MB 以内的文本文件",
                  HttpResponse::k400BadRequest, conn);
        return true;
    }

    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        sendError(resp, "文件读取失败", HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setContentType("text/plain; charset=utf-8");
    resp->setBody(content);
    resp->addHeader("Content-Length", std::to_string(content.size()));
    return true;
}

bool DataNodeHttpHandler::handleDeleteFile(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {
    if (req.method() != HttpRequest::kPost ||
        req.path() != "/api/datanode/delete")
        return true;

    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
        sendError(resp, "未授权", HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string token = authHeader.substr(7);
    std::string serverFilename;
    if (!TokenManager::instance().verifyDeleteToken(token, serverFilename)) {
        sendError(resp, "删除 Token 无效或已过期", HttpResponse::k403Forbidden,
                  conn);
        return true;
    }

    std::string filepath = uploadDir_ + "/" + serverFilename;
    try {
        if (std::filesystem::exists(filepath)) {
            std::filesystem::remove(filepath);
            LOG_INFO << "DataNode 成功物理删除文件: " << filepath;
        } else {
            LOG_WARN << "DataNode 物理文件不存在 (可能已被删除): " << filepath;
            sendError(resp, "Physial file not found",
                      HttpResponse::k404NotFound, conn);
            return true;
        }
    } catch (const std::exception &e) {
        LOG_ERROR << "DataNode 删除物理文件失败: " << e.what();
        sendError(resp, "物理删除失败", HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setContentType("application/json");
    std::string bodyStr = R"({"code": 0, "message": "物理删除成功"})";
    resp->setBody(bodyStr);
    resp->addHeader("Content-Length", std::to_string(bodyStr.size()));
    conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
        connection->shutdown();
        return true;
    });
    return true;
}
