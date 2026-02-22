#include "HttpUploadHandler.h"
#include "../../common/include/RemoteFileStorage.h"

HttpUploadHandler::HttpUploadHandler(int numThreads, const std::string &dbHost,
                                     const std::string &dbUser,
                                     const std::string &dbPassword,
                                     const std::string &dbName,
                                     unsigned int dbPort)
    : threadPool_("UploadHandler"), uploadDir_("uploads"),
      mappingFile_("uploads/filename_mapping.json"), activeRequests_(0),
      mysql(NULL), dbHost(dbHost), dbUser(dbUser), dbPassword(dbPassword),
      dbName(dbName), dbPort(dbPort) {
    threadPool_.start(numThreads);

    // 初始化节点管理器
    nodeManager_ = std::make_shared<NodeManager>();

    // 创建上传目录
    if (!fs::exists(uploadDir_)) {
        fs::create_directory(uploadDir_);
    }

    // 初始化数据库连接
    if (!initDatabase()) {
        LOG_ERROR << "初始化数据库失败，系统可能无法正常工作";
    }

    // 加载文件名映射
    loadFilenameMapping();

    // 初始化路由表
    initRoutes();
}

bool HttpUploadHandler::handleFileUpload(const TcpConnectionPtr &conn,
                                         HttpRequest &req, HttpResponse *resp) {
    // 验证会话
    std::string sessionId = req.getHeader("X-Session-ID");
    int userId;
    std::string usernameFromSession;

    if (!validateSession(sessionId, userId, usernameFromSession)) {
        sendError(resp, "未登录或会话已过期", HttpResponse::k401Unauthorized,
                  conn);
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
        // 解析 multipart/form-data 边界
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
            // 获取原始文件名
            std::string originalFilename;

            // 首先尝试从X-File-Name头部获取文件名
            std::string headerFilename = req.getHeader("X-File-Name");
            if (!headerFilename.empty()) {
                originalFilename = urlDecode(headerFilename);
                LOG_INFO << "Got filename from X-File-Name header: "
                         << originalFilename;
            } else {
                // 从Content-Disposition头中获取文件名
                std::string body = req.body();
                if (body.empty()) {
                    sendError(resp, "Request body is empty",
                              HttpResponse::k400BadRequest, conn);
                    return true;
                }

                std::regex filenameRegex(
                    "Content-Disposition:.*filename=\"([^\"]+)\"");
                if (std::regex_search(body, matches, filenameRegex) &&
                    matches[1].matched) {
                    originalFilename = matches[1].str();
                    LOG_INFO << "Got filename from Content-Disposition: "
                             << originalFilename;
                } else {
                    originalFilename = "unknown_file";
                    LOG_INFO << "Using default filename: " << originalFilename;
                }
            }
            // 生成服务器端文件名
            std::string filename = generateUniqueFilename("upload");
            std::string filepath = uploadDir_ + "/" + filename;

            // 选择存储节点
            auto node = nodeManager_->selectNode();
            // 创建上传上下文
            std::unique_ptr<FileStorage> storage;

            if (node.isLocal) {
                storage = std::make_unique<LocalFileStorage>();
            } else {
                storage =
                    std::make_unique<RemoteFileStorage>(node.ip, node.port);
            }

            uploadContext = std::make_shared<FileUploadContext>(
                filename, originalFilename, std::move(storage));
            httpContext->setContext(uploadContext);

            // 设置边界
            uploadContext->setBoundary(boundary);

            // 解析body中的文件内容
            std::string body = req.body();
            size_t pos = body.find("\r\n\r\n");
            if (pos != std::string::npos) {
                // 跳过头部信息，获取文件内容
                pos += 4;

                // 检查是否是结束边界
                std::string endBoundary = boundary + "--";
                size_t endPos = body.find(endBoundary);
                if (endPos != std::string::npos) {
                    LOG_INFO << "Found end boundary";
                    // 先写入边界之前的内容
                    if (endPos > 0) {
                        uploadContext->writeData(body.data() + pos,
                                                 endPos - pos);
                        LOG_INFO << "Wrote " << endPos - pos
                                 << " bytes before end boundary, total: "
                                 << uploadContext->getTotalBytes();
                    }
                    // 找到结束边界，上传完成
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
            LOG_INFO << "Created upload context for file: " << filepath;

        } catch (const std::exception &e) {
            LOG_ERROR << "Failed to create upload context: " << e.what();
            sendError(resp, "Failed to create file",
                      HttpResponse::k500InternalServerError, conn);
            return true;
        }
    } else {
        try {
            // 处理后续的数据块
            std::string body = req.body();
            if (!body.empty()) {
                LOG_INFO << "uploadContext->getState() = "
                         << static_cast<int>(uploadContext->getState());
                switch (uploadContext->getState()) {
                case FileUploadContext::State::kExpectBoundary: {
                    // 检查是否是结束边界 格式为： --boundary--
                    std::string endBoundary =
                        uploadContext->getBoundary() + "--";
                    size_t endPos = body.find(endBoundary);
                    if (endPos != std::string::npos) {
                        // 在这里添加写入边界之前的内容
                        if (endPos > 0) {
                            uploadContext->writeData(body.data(), endPos);
                            LOG_INFO << "Wrote " << endPos
                                     << " bytes before end boundary, total: "
                                     << uploadContext->getTotalBytes();
                        }
                        LOG_INFO << "Found end boundary;";
                        // 找到结束边界，上传完成
                        uploadContext->setState(
                            FileUploadContext::State::kComplete);
                        break;
                    }
                    // 检查是否是普通边界 格式为： --boundary
                    size_t boundaryPos =
                        body.find(uploadContext->getBoundary());
                    LOG_INFO << "检查是否是普通边界 boundaryPos:"
                             << boundaryPos;
                    if (boundaryPos != std::string::npos) {
                        // 找到新边界的开始，跳过边界和头部
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
                    // 检查是否包含下一个边界
                    size_t boundaryPos =
                        body.find(uploadContext->getBoundary());
                    if (boundaryPos != std::string::npos) {
                        // 写入边界之前的内容
                        uploadContext->writeData(body.data(), boundaryPos);
                        LOG_INFO << "Wrote " << boundaryPos << " bytes, total: "
                                 << uploadContext->getTotalBytes();
                        // 更新状态
                        uploadContext->setState(
                            FileUploadContext::State::kExpectBoundary);
                    } else {
                        // 没有找到边界，写入所有内容
                        uploadContext->writeData(body.data(), body.size());
                        LOG_INFO << "Wrote " << body.size() << " bytes, total: "
                                 << uploadContext->getTotalBytes();
                        // 更新状态
                    }
                    break;
                }
                case FileUploadContext::State::kComplete: {
                    // 上传已完成，忽略后续数据
                    break;
                }
                default: {
                    LOG_INFO << "Unknown state: "
                             << static_cast<int>(uploadContext->getState());
                    break;
                }
                }
            }
        } catch (const std::exception &e) {
            LOG_ERROR << "Error processing data chunk: " << e.what();
            sendError(resp, "Failed to process data",
                      HttpResponse::k500InternalServerError, conn);
            return true;
        }
    }
    req.setBody(""); // 清空请求体

    // 检查是否完成
    if (uploadContext->getState() == FileUploadContext::State::kComplete ||
        httpContext->gotAll()) {
        // 上传完成，准备响应
        std::string serverFilename =
            fs::path(uploadContext->getFilename()).filename().string();
        std::string originalFilename = uploadContext->getOriginalFilename();
        uintmax_t fileSize = uploadContext->getTotalBytes();

        // 检测文件类型
        std::string fileType = getFileType(originalFilename);

        // 保存文件信息到数据库
        std::string query =
            "INSERT INTO files (filename, original_filename, file_size, "
            "file_type, user_id) VALUES ('" +
            escapeString(serverFilename) + "', '" +
            escapeString(originalFilename) + "', " + std::to_string(fileSize) +
            ", '" + escapeString(fileType) + "', " + std::to_string(userId) +
            ")";

        if (!executeQuery(query)) {
            LOG_ERROR << "保存文件信息到数据库失败";
        }

        int fileId = static_cast<int>(mysql_insert_id(mysql));

        json response = {{"code", 0},
                         {"message", "上传成功"},
                         {"fileId", fileId},
                         {"filename", serverFilename},
                         {"originalFilename", originalFilename},
                         {"size", fileSize}};

        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("application/json");
        resp->addHeader("Connection", "close");
        resp->setBody(response.dump());

        // 清理上下文
        httpContext->setContext(nullptr);

        conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
            LOG_INFO << "Upload complete, closing connection";
            connection->shutdown();
            return true;
        });
        return true;
    } else {
        LOG_INFO << "Waiting for more data, current state: "
                 << static_cast<int>(uploadContext->getState());
        return false;
    }
}