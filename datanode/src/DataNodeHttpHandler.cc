#include "DataNodeHttpHandler.h"
#include "DataNode.h"
#include "LocalFileStorage.h"
#include "MasterClient.h"
#include "TokenManager.h"
#include "base/ThreadPool.h"
#include "net/Callbacks.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/socket.h>

// --------------FileUploadContext--------------
FileUploadContext::FileUploadContext(uint64_t fileID,
                                     const std::string &filename,
                                     std::shared_ptr<FileStorage> &&storage)
    : fileID_(fileID), filename_(filename), storage_(std::move(storage)),
      totalBytes_(0), state_(State::kExpectHeaders), boundary_("") {
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

// --------------Handler--------------
DataNodeHttpHandler::DataNodeHttpHandler(DataNode *datanode)
    : uploadDir_("uploads"), datanode_(datanode) {
    // 创建上传目录
    if (!fs::exists(uploadDir_)) {
        LOG_DEBUG << "创建上传目录: " << uploadDir_;
        fs::create_directory(uploadDir_);
    }

    // 初始化路由表
    initRoutes();
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
        if (auto context =
                std::static_pointer_cast<HttpContext>(conn->getContext())) {
            if (auto uploadContext = context->getContext<FileUploadContext>()) {
                LOG_INFO << "Cleaning up upload context for file: "
                         << uploadContext->getFilename();
                datanode_->decActiveUpload();
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
        // 允许前端携带的自定义请求头（非常重要，少一个都不行！）
        resp->addHeader("Access-Control-Allow-Headers",
                        "Authorization, Content-Type, X-File-Name");
        // 让浏览器缓存这个预检结果 24 小时，不用每次上传都发 OPTIONS
        resp->addHeader("Access-Control-Max-Age", "86400");

        // OPTIONS 请求不需要 Body，直接返回 true 结束处理
        return true;
    }

    // ⚠️ 极其重要：即使是真正的 POST
    // 响应，也必须带上跨域头，否则前端拿不到响应数据！
    resp->addHeader("Access-Control-Allow-Origin", "*");

    // 验证Token
    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
        sendError(resp, "缺少上传 Token", fn::HttpResponse::k401Unauthorized,
                  conn);
        return true;
    }

    uint64_t fileID = 0;
    std::string serverFilename;
    std::string originalFilename;
    std::string created_time;
    std::string uploadToken = authHeader.substr(7);
    if (!TokenManager::instance().verifyUploadToken(
            uploadToken, fileID, originalFilename, serverFilename,
            created_time)) {
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

    // 尝试获取已经存在的上传上下文
    std::shared_ptr<FileUploadContext> uploadContext =
        httpContext->getContext<FileUploadContext>();

    if (!uploadContext) {
        try {
            std::string filePath = uploadDir_ + "/" + serverFilename;
            auto localStorage = std::make_shared<LocalFileStorage>();
            uploadContext = std::make_shared<FileUploadContext>(
                fileID, filePath, std::move(localStorage));
            httpContext->setContext(uploadContext);
            LOG_INFO << "开始接收文件: " << serverFilename;

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
        datanode_->decActiveUpload();

        auto file_id = uploadContext->getFileID();
        auto server_filename = uploadContext->getFilename();
        auto stored_size = uploadContext->getTotalBytes();

        json respJson = {{"code", 0},
                         {"msg", "Upload success"},
                         {"file",
                          {{"id", file_id},
                           {"name", server_filename.substr(8)},
                           {"originalName", originalFilename},
                           {"size", stored_size},
                           {"createdAt", created_time}}}};
        //   respJson["data"]["file_md5"] = uploadContext->getFileMd5();
        // 优化：只 dump 一次
        std::string bodyStr = respJson.dump();
        resp->setStatusCode(fileserver::net::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->addHeader("Connection", "close");
        resp->setBody(bodyStr);
        resp->addHeader("Content-Length", std::to_string(bodyStr.size()));

        // 清理上下文
        httpContext->setContext(nullptr);

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
    uint64_t file_id;
    std::string serverFilename;
    std::string originalFilename;
    if (!TokenManager::instance().verifyDownloadToken(token, originalFilename,
                                                      serverFilename)) {
        sendError(resp, "非法请求", HttpResponse::k403Forbidden, conn);
        return true;
    }

    std::string filepath = uploadDir_ + "/" + serverFilename;
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
                    "attachment; filename=\"" + originalFilename + "\"");
    resp->setContentType(getMimeType(originalFilename));
    if (isRange) {
        resp->addHeader("Content-Range", "bytes " + std::to_string(startPos) +
                                             "-" + std::to_string(endPos) +
                                             "/" + std::to_string(fileSize));
    }
    resp->addHeader("Content-Length", std::to_string(endPos - startPos + 1));

    auto httpContext =
        std::static_pointer_cast<HttpContext>(conn->getContext());
    auto downContext =
        std::make_shared<FileDownContext>(filepath, serverFilename);
    downContext->seekTo(startPos);
    httpContext->setContext(downContext);

    conn->setWriteCompleteCallback(
        [downContext](const TcpConnectionPtr &connection) {
            std::string chunk;
            if (downContext->readNextChunk(chunk)) {
                connection->send(chunk);
                return true;
            } else {
                connection->shutdown();
                return true;
            }
            return true;
        });

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