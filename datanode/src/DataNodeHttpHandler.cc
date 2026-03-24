#include "DataNodeHttpHandler.h"
#include "DataNode.h"
#include "LocalFileStorage.h"
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
            }
        }
        conn->setContext(std::shared_ptr<void>());
    }
}

/// TODO:
bool DataNodeHttpHandler::validateToken(const std::string &token) {}

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
    std::string uploadToken = authHeader.substr(7);
    if (!TokenManager::instance().verifyUploadToken(uploadToken, fileID,
                                                    serverFilename)) {
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
        /// 解析 multipart/form-data 边界
        std::string contentType = req.getHeader("Content-Type");
        if (contentType.empty()) {
            sendError(resp, "Content-Type header is missing",
                      HttpResponse::k400BadRequest, conn);
            return true;
        }
        std::regex boundaryRegex("boundary=(.+)$");
        std::smatch matches;
        if (!std::regex_search(contentType, matches, boundaryRegex)) {
            sendError(resp, "Invalid Content-Type",
                      HttpResponse::k400BadRequest, conn);
            return true;
        }
        std::string boundary = "--" + matches[1].str();
        LOG_INFO << "Boundary: " << boundary;

        try {

            // 创建存储
            auto localStorage = std::make_shared<LocalFileStorage>();

            // 创建上传上下文
            std::string filePath = uploadDir_ + "/" + serverFilename;
            uploadContext = std::make_shared<FileUploadContext>(
                fileID, filePath, std::move(localStorage));

            uploadContext->setBoundary(boundary);
            httpContext->setContext(uploadContext);

            // 解析body
            std::string body = req.body();
            size_t pos = body.find("\r\n\r\n");
            if (pos != std::string::npos) {
                // 跳过头部，获取文件内容
                pos += 4;

                // 检查是否是结束边界
                std::string endBoundary = boundary + "--";
                size_t endPos = body.find(endBoundary);
                if (endPos != std::string::npos) {
                    LOG_INFO << "Found end boudnary";

                    // 去掉结束边界前的\r\n
                    size_t validEnd = endPos;
                    if (validEnd > pos && body[validEnd - 1] == '\n') {
                        validEnd--;
                    }
                    if (validEnd > pos && body[validEnd - 1] == '\r') {
                        validEnd--;
                    }
                    if (validEnd > pos) {
                        size_t writeLen = validEnd - pos;
                        uploadContext->writeData(body.data() + pos, writeLen);
                        LOG_INFO << "Wrote " << writeLen << " bytes, total: "
                                 << uploadContext->getTotalBytes();
                    }
                    // 找到结束边界， 上传完成
                    uploadContext->setState(
                        FileUploadContext::State::kComplete);
                } else {
                    // 直接写入从pos开始的所有内容
                    if (pos < body.size()) {
                        uploadContext->writeData(body.data() + pos,
                                                 body.size() - pos);
                        LOG_INFO << "Wrote " << body.size() - pos
                                 << " bytes, total: "
                                 << uploadContext->getTotalBytes();
                    }
                    uploadContext->setState(
                        FileUploadContext::State::kExpectBoundary);
                }
            }

            req.setBody(""); // 清空请求体
            LOG_INFO << "Created upload context for file: " << serverFilename;

        } catch (std::exception &e) {
            LOG_ERROR << "Failed to create upload context: " << e.what();
            sendError(resp, "Failed to create file",
                      HttpResponse::k500InternalServerError, conn);
            return true;
        }
    } else {
        try {
            // 处理后续数据块
            std::string body = req.body();
            if (!body.empty()) {
                switch (uploadContext->getState()) {
                case FileUploadContext::State::kExpectBoundary: {
                    // 检查结束边界
                    std::string endBoundary =
                        uploadContext->getBoundary() + "--";
                    size_t endPos = body.find(endBoundary);
                    if (endPos != std::string::npos) {
                        size_t validEnd = endPos;
                        if (validEnd > 0 && body[validEnd - 1] == '\n') {
                            validEnd--;
                        }
                        if (validEnd > 0 && body[validEnd - 1] == '\r') {
                            validEnd--;
                        }
                        if (validEnd > 0) {
                            size_t writeLen = validEnd;
                            uploadContext->writeData(body.data(), writeLen);
                            LOG_INFO << "Wrote " << writeLen
                                     << " bytes, total: "
                                     << uploadContext->getTotalBytes();
                        }
                        LOG_INFO << "Found end boundary";
                        // 找到结束边界，上传完成
                        uploadContext->setState(
                            FileUploadContext::State::kComplete);
                        break;
                    }
                    // 检查普通边界
                    size_t boundaryPos =
                        body.find(uploadContext->getBoundary());
                    if (boundaryPos != std::string::npos) {
                        // 找到新边界
                        size_t contentStart =
                            body.find("\r\n\r\n", boundaryPos);
                        if (contentStart != std::string::npos) {
                            contentStart += 4;
                            // 写入边界之前的内容
                            if (boundaryPos > 0) {
                                uploadContext->writeData(body.data(),
                                                         boundaryPos);
                                LOG_INFO << "Wrote " << boundaryPos
                                         << " bytes, total: "
                                         << uploadContext->getTotalBytes();
                            }
                            // 更新状态
                            uploadContext->setState(
                                FileUploadContext::State::kExpectContent);
                        }
                    } else {
                        // 没有找到边界，写入所有内容
                        uploadContext->writeData(body.data(), body.size());

                        LOG_INFO << "Wrote " << body.size() << " bytes, total: "
                                 << uploadContext->getTotalBytes();
                    }
                    break;
                }
                case FileUploadContext::State::kExpectContent: {
                    // 检查是否包含下一边界
                    size_t boundaryPos =
                        body.find(uploadContext->getBoundary());
                    if (boundaryPos != std::string ::npos) {
                        // 写入边界前的内容
                        uploadContext->writeData(body.data(), boundaryPos);
                        LOG_INFO << "Wrtoe " << boundaryPos << " bytes, total: "
                                 << uploadContext->getTotalBytes();
                        // 更新状态
                        uploadContext->setState(
                            FileUploadContext::State::kExpectBoundary);
                    } else {
                        // 没有找到边界，写入所有内容
                        uploadContext->writeData(body.data(), body.size());
                        LOG_INFO << "Wrote " << body.size() << " bytes, total: "
                                 << uploadContext->getTotalBytes();
                    }
                    break;
                }
                case FileUploadContext::State::kComplete: {
                    // 上传已完成，忽略
                    break;
                }
                default: {
                    LOG_INFO << "Unknown state: "
                             << static_cast<uint8_t>(uploadContext->getState());
                    break;
                }
                }
            }
        } catch (std::exception &e) {
            LOG_ERROR << "Error processing data chunk: " << e.what();
            sendError(resp, "Failed to process data",
                      HttpResponse::k500InternalServerError, conn);
            return true;
        }
    }
    req.setBody("");

    // 检查是否完成
    if (uploadContext->getState() == FileUploadContext::State::kComplete ||
        httpContext->gotAll()) {
        auto file_id = uploadContext->getFileID();
        auto server_filename = uploadContext->getFilename();
        auto stored_size = uploadContext->getTotalBytes();
        json respJson = {{"code", 0},
                         {"msg", "Upload success"},
                         {"data",
                          {{"file_id", file_id},
                           {"server_filename", server_filename},
                           {"stored_size", stored_size}}}};
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