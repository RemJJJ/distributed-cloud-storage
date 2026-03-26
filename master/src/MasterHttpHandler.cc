#include "MasterHttpHandler.h"
#include "DataNodeHandler.h"
#include "NodeManager.h"
#include "db/MySQLStatement.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include <memory>

HttpUploadHandler::HttpUploadHandler(int numThreads)
    : threadPool_("UploadHandler"), uploadDir_("uploads"),
      mappingFile_("uploads/filename_mapping.json"), activeRequests_(0) {
    threadPool_.start(numThreads);

    // 创建上传目录
    if (!fs::exists(uploadDir_)) {
        fs::create_directory(uploadDir_);
    }
    // 加载文件名映射
    loadFilenameMapping();

    // 初始化路由表
    initRoutes();
}

HttpUploadHandler::~HttpUploadHandler() {
    threadPool_.stop();
    // 保存文件名映射
    saveFilenameMapping();
}

void HttpUploadHandler::onConnection(const TcpConnectionPtr &conn) {
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

void HttpUploadHandler::initRoutes() {
    // 不需要token验证的路由
    addRoute("/favicon.ico", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleFavicon(conn, req, resp);
             });
    addRoute("/", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleIndex(conn, req, resp);
             });
    addRoute("/index.html", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleIndex(conn, req, resp);
             });
    addRoute("/register.html", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleIndex(conn, req, resp);
             });

    // UserHandler
    auto userHandler = std::make_shared<UserHandler>();
    addRoute("/register", HttpRequest::kPost,
             [userHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return userHandler->handleRegister(conn, req, resp);
             });
    addRoute("/login", HttpRequest::kPost,
             [userHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return userHandler->handleLogin(conn, req, resp);
             });

    // 需要会话验证的路由
    addRoute("/upload", HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleFileUpload(conn, req, resp);
             });
    addRoute("/files", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleListFiles(conn, req, resp);
             });
    addRoute("/download/([^/]+)", fileserver::net::HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleDownload(conn, req, resp);
             },
             {"filename"});

    // datanodeHandler路由
    auto datanodeHandler = std::make_shared<DataNodeHandler>();
    addRoute("/registerNode", HttpRequest::kPost,
             [datanodeHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                               std::shared_ptr<HttpResponse> &resp) {
                 return datanodeHandler->handleRegisterNode(conn, req, resp);
             });

    addRoute("/heartbeat", HttpRequest::kPost,
             [datanodeHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                               std::shared_ptr<HttpResponse> &resp) {
                 return datanodeHandler->handleHeartbeat(conn, req, resp);
             });
    addRoute("/notify_upload_finish", fileserver::net::HttpRequest::kPost,
             [datanodeHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                               std::shared_ptr<HttpResponse> &resp) {
                 return datanodeHandler->handleUploadFinishNotify(conn, req,
                                                                  resp);
             });
    addRoute("/report_files", fileserver::net::HttpRequest::kPost,
             [datanodeHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                               std::shared_ptr<HttpResponse> &resp) {
                 return datanodeHandler->handleReportFiles(conn, req, resp);
             });
}

void HttpUploadHandler::saveFilenameMapping() {
    std::lock_guard<std::mutex> lock(mappingMutex_);
    saveFilenameMappingInternal();
}

// 内部函数，不加锁，调用方需要确保已获得锁
void HttpUploadHandler::loadFilenameMapping() {
    try {
        if (fs::exists(mappingFile_)) {
            std::ifstream file(mappingFile_);
            filenameMapping_ =
                json::parse(file).get<std::map<std::string, std::string>>();
        }
    } catch (const std::exception &e) {
        LOG_ERROR << "Failed to load filename mapping: " << e.what();
    }
}

void HttpUploadHandler::saveFilenameMappingInternal() {
    try {
        std::ofstream file(mappingFile_);
        file << json(filenameMapping_).dump(2);
    } catch (const std::exception &e) {
        LOG_ERROR << "Failed to save filename mapping: " << e.what();
    }
}

void HttpUploadHandler::loadFilenameMappingInternal() {
    try {
        if (fs::exists(mappingFile_)) {
            std::ifstream file(mappingFile_);
            filenameMapping_ =
                json::parse(file).get<std::map<std::string, std::string>>();
        }
    } catch (const std::exception &e) {
        LOG_ERROR << "Failed to load filename mapping: " << e.what();
    }
}

// 外部接口，带锁保护
void HttpUploadHandler::addFilenameMapping(
    const std::string &serverFilename, const std::string &originalFilename) {
    std::lock_guard<std::mutex> lock(mappingMutex_);
    loadFilenameMappingInternal(); // 先加载最新的映射
    filenameMapping_[serverFilename] = originalFilename;
    saveFilenameMappingInternal();
}

std::string
HttpUploadHandler::generateUniqueFilename(const std::string &prefix) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch())
                         .count();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    return prefix + "_" + std::to_string(timestamp) + "_" +
           std::to_string(dis(gen));
}

std::string HttpUploadHandler::getFileType(const std::string &filename) {
    size_t dotPos = filename.find_last_of('.');
    if (dotPos != std::string::npos && dotPos < filename.length() - 1) {
        std::string extension = filename.substr(dotPos + 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       ::tolower);

        // 根据扩展名判断文件类型
        if (extension == "jpg" || extension == "jpeg" || extension == "png" ||
            extension == "gif") {
            return "image";
        } else if (extension == "mp4" || extension == "avi" ||
                   extension == "mov" || extension == "wmv") {
            return "video";
        } else if (extension == "pdf") {
            return "pdf";
        } else if (extension == "doc" || extension == "docx") {
            return "word";
        } else if (extension == "xls" || extension == "xlsx") {
            return "excel";
        } else if (extension == "ppt" || extension == "pptx") {
            return "powerpoint";
        } else if (extension == "txt" || extension == "csv") {
            return "text";
        } else {
            return "other";
        }
    }
    return "unknown";
}

bool HttpUploadHandler::handleIndex(const TcpConnectionPtr &conn,
                                    HttpRequest &req,
                                    std::shared_ptr<HttpResponse> &resp) {
    // 1. 设置基础响应头
    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->setContentType("text/html; charset=utf-8");

    // 2. 获取请求路径
    const std::string &path = req.path();
    std::string filePath;
    LOG_INFO << "Request path = " << path;

    // 3. 核心逻辑：通过可执行文件路径定位项目根目录（彻底抛弃__FILE__）
    char exePath[1024] = {0};
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len == -1) {
        // 读取可执行文件路径失败，降级使用相对路径（master/src →
        // 项目根）
        LOG_WARN << "Failed to get executable path: " << strerror(errno)
                 << ", use fallback relative path";
        filePath = "../../web/html/index.html";
    } else {
        // 解析可执行文件完整路径，回退到项目根目录
        std::string exeFullPath(exePath, len);
        LOG_DEBUG << "Executable full path = " << exeFullPath;

        // 步骤1：截取可执行文件所在目录（master/src）
        std::string::size_type pos = exeFullPath.find_last_of('/');
        if (pos == std::string::npos) {
            LOG_ERROR << "Invalid executable path: " << exeFullPath;
            filePath = "../../web/html/index.html";
        } else {
            std::string srcDir = exeFullPath.substr(0, pos); // master/src

            // 步骤2：回退到master目录
            pos = srcDir.find_last_of('/');
            if (pos == std::string::npos) {
                LOG_ERROR << "Failed to find master directory from: " << srcDir;
                filePath = "../web/html/index.html";
            } else {
                std::string masterDir = srcDir.substr(0, pos); // master

                // 步骤3：回退到项目根目录（distributed-fm）
                pos = masterDir.find_last_of('/');
                if (pos == std::string::npos) {
                    LOG_ERROR << "Failed to find project root from: "
                              << masterDir;
                    filePath = "./web/html/index.html";
                } else {
                    std::string projectRoot =
                        masterDir.substr(0, pos); // distributed-fm
                    LOG_INFO << "Project root directory = " << projectRoot;

                    // 步骤4：拼接目标HTML文件路径
                    if (path == "/register.html") {
                        filePath = projectRoot + "/web/html/register.html";
                    } else if (path == "/share.html" ||
                               path.find("/share/") == 0) {
                        filePath = projectRoot + "/web/html/share.html";
                    } else {
                        filePath = projectRoot + "/web/html/index.html";
                    }
                }
            }
        }
    }

    // 从文件中读取HTML内容
    std::ifstream file(filePath);
    if (!file.is_open()) {
        LOG_ERROR << "Failed to open " << filePath;
        sendError(resp, "Failed to open " + filePath,
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }

    std::string html((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    file.close();

    resp->addHeader("Connection", "close");
    resp->setBody(html);

    conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
        connection->shutdown();
        return true;
    });
    return true;
}

bool HttpUploadHandler::handleListFiles(const TcpConnectionPtr &conn,
                                        HttpRequest &req,
                                        std::shared_ptr<HttpResponse> &resp) {
    // 验证Token
    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.find("Bearer ") != 0) {
        sendError(resp, "Missing Authorization header",
                  fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string token = authHeader.substr(7);
    auto userId = TokenManager::instance().verifyUserToken(token);

    if (userId < 0) {
        sendError(resp, "Invalid token", fn::HttpResponse::k401Unauthorized,
                  conn);
        return true;
    }
    // 获取数据库连接
    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql) {
        sendError(resp, "数据库连接失败",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }

    // 3. 构建 SQL (使用 ? 占位符，防止注入)
    std::string listType = req.getQuery("type", "my");
    std::string sqlTemplate;

    if (listType == "my") {
        sqlTemplate = "SELECT f.id, f.filename, f.original_filename, "
                      "f.file_size, f.file_type, f.created_at, 1 as is_owner "
                      "FROM files f WHERE f.user_id = ?";
    } else if (listType == "shared") {
        sqlTemplate =
            "SELECT f.id, f.filename, f.original_filename, "
            "f.file_size, f.file_type, f.created_at, 0 as is_owner "
            "FROM files f "
            "JOIN file_shares fs ON f.id = fs.file_id "
            "WHERE (fs.shared_with_id = ? OR fs.share_type = 'public') "
            "AND f.user_id != ?";
    } else if (listType == "all") {
        sqlTemplate =
            "SELECT DISTINCT f.id, f.filename, f.original_filename, " // 增加DISTINCT防止重复
            "f.file_size, f.file_type, f.created_at, "
            "CASE WHEN f.user_id = ? THEN 1 ELSE 0 END as is_owner "
            "FROM files f "
            "LEFT JOIN file_shares fs ON f.id = fs.file_id "
            "WHERE f.user_id = ? OR fs.shared_with_id = ? OR fs.share_type = "
            "'public'";
    } else {
        sendError(resp, "Invalid list type", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    }

    LOG_INFO << "Executing file list query for user: " << userId;

    // 执行查询
    db::MySQLStatement stmt(*mysql, sqlTemplate);
    if (stmt.hasError()) {
        sendError(resp, "数据库错误", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    // 根据不同的查询类型绑定参数
    if (listType == "my") {
        stmt.bindInt(userId);
    } else if (listType == "shared") {
        stmt.bindInt(userId);
        stmt.bindInt(userId);
    } else if (listType == "all") {
        stmt.bindInt(userId);
        stmt.bindInt(userId);
        stmt.bindInt(userId);
    }

    if (!stmt.execute()) {
        LOG_ERROR << "Query failed: " << stmt.getError();
        sendError(resp, "查询失败", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }
    auto rs = stmt.getResultSet();
    json files = json::array();

    // 遍历文件列表结果
    while (rs->next()) {
        int fileId = rs->getInt(0);
        std::string filename = rs->getString(1);
        std::string originalFilename = rs->getString(2);
        uintmax_t fileSize =
            rs->getInt64(3); // 注意：getInt64 返回 long long，转 uintmax_t
        std::string fileType = rs->getString(4);
        std::string createdAt = rs->getString(5);
        bool isOwner = (rs->getInt(6) == 1);

        json shareInfo = nullptr;

        // 如果是所有者，查询分享信息 (同样使用预处理语句)
        if (isOwner) {
            std::string shareSql =
                "SELECT share_type, shared_with_id, share_code, "
                "expire_time, extract_code FROM file_shares "
                "WHERE file_id = ?";

            db::MySQLStatement shareStmt(*mysql, shareSql);
            if (!shareStmt.hasError()) {
                shareStmt.bindInt(fileId);
                if (shareStmt.execute()) {
                    auto shareRs = shareStmt.getResultSet();
                    if (shareRs->next()) {
                        std::string shareType = shareRs->getString(0);

                        shareInfo = {{"type", shareType}};

                        // share_code
                        if (!shareRs->isNull(2)) {
                            shareInfo["shareCode"] = shareRs->getString(2);
                        }

                        // extract_code (仅 protected 类型)
                        if (shareType == "protected" && !shareRs->isNull(4)) {
                            shareInfo["extractCode"] = shareRs->getString(4);
                        }

                        // shared_with_id & username (仅 user 类型)
                        if (shareType == "user" && !shareRs->isNull(1)) {
                            int sharedWithId = shareRs->getInt(1);
                            shareInfo["sharedWithId"] = sharedWithId;

                            // 查询共享用户名
                            std::string userSql =
                                "SELECT username FROM users WHERE id = ?";
                            db::MySQLStatement userStmt(*mysql, userSql);
                            if (!userStmt.hasError()) {
                                userStmt.bindInt(sharedWithId);
                                if (userStmt.execute()) {
                                    auto userRs = userStmt.getResultSet();
                                    if (userRs->next()) {
                                        shareInfo["sharedWithUsername"] =
                                            userRs->getString(0);
                                    }
                                }
                            }
                        }

                        // expire_time
                        if (!shareRs->isNull(3)) {
                            shareInfo["expireTime"] = shareRs->getString(3);
                        }
                    }
                }
            }
        }

        json fileInfo = {{"id", fileId},
                         {"name", filename},
                         {"originalName", originalFilename},
                         {"size", fileSize},
                         {"type", fileType},
                         {"createdAt", createdAt},
                         {"isOwner", isOwner}};

        if (shareInfo != nullptr) {
            fileInfo["shareInfo"] = shareInfo;
        }

        files.push_back(fileInfo);
    }

    // 构建响应
    json response;
    response["code"] = 0;
    response["message"] = "Success";
    response["files"] = files;

    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->setContentType("application/json");
    resp->addHeader("Connection", "close");
    resp->setBody(response.dump());

    conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
        LOG_INFO << "List files complete, closing connection";
        connection->shutdown();
        return true;
    });

    return true;
}

bool HttpUploadHandler::handleFavicon(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) { // 获取当前文件目录
    std::string currentDir = __FILE__;
    std::string::size_type pos = currentDir.find_last_of('/');
    std::string projectRoot = currentDir.substr(0, pos);
    std::string faviconPath = projectRoot + "/favicon.ico";

    // 读取 favicon.ico 文件
    std::ifstream file(faviconPath, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR << "Failed to open favicon.ico";
        resp->setStatusCode(HttpResponse::k404NotFound);
        resp->setStatusMessage("Not Found");
        resp->setContentType("image/x-icon");
        resp->addHeader("Connection", "close");
        resp->setBody("");
    } else {
        std::string iconData((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        file.close();

        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("image/x-icon");
        resp->addHeader("Connection", "close");
        resp->setBody(iconData);
    }

    conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
        connection->shutdown();
        return true;
    });
    return true;
}

// 文件上传
bool HttpUploadHandler::handleFileUpload(const TcpConnectionPtr &conn,
                                         HttpRequest &req,
                                         std::shared_ptr<HttpResponse> &resp) {
    // 路由匹配
    LOG_INFO << "Receive file upload request";

    try {
        std::string authHeader = req.getHeader("Authorization");
        if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
            sendError(resp, "未授权的访问，请先登录",
                      fn::HttpResponse::k401Unauthorized, conn);
            return true;
        }

        std::string userToken = authHeader.substr(7);
        auto userId = TokenManager::instance().verifyUserToken(userToken);
        if (userId < 0) {
            sendError(resp, "Token 无效或已过期，请重新登录",
                      fn::HttpResponse::k401Unauthorized, conn);
            return true;
        }

        // 解析客户端发来的文件源数据
        json requestData = json::parse(req.body());
        std::string originalFilename =
            requestData.value("original_filename", "");
        uintmax_t fileSize = requestData.value("file_size", 0);
        std::string fileType = getFileType(originalFilename);

        std::string serverFilename = generateUniqueFilename("upload");

        /// TODO: 秒传
        std::string fileMd5 = requestData.value("md5", "");

        if (originalFilename.empty() || fileSize == 0) {
            sendError(resp, "文件名和大小不能为空",
                      fn::HttpResponse::k400BadRequest, conn);
            return true;
        }

        // 在数据库中预先插入一条文件记录，状态设为 "uploading" (上传中)
        auto mysql = db::MySQLPool::instance().getConnection();
        if (!mysql) {
            sendError(resp, "数据库连接失败",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        // ==========================================
        // 🌟 新增：MD5 秒传逻辑
        // ==========================================
        if (!fileMd5.empty()) {
            // 查找数据库中是否已经有这个MD5且状态为success的文件
            std::string checkSql =
                "SELECT node_id, filename FROM files WHERE file_md5 = ? AND "
                "status = 'success' LIMIT 1";

            db::MySQLStatement checkStmt(*mysql, checkSql);
            checkStmt.bindString(fileMd5);

            if (checkStmt.execute()) {
                auto rs = checkStmt.getResultSet();
                if (rs->next()) {
                    // 秒传命中！获取已存在的物理文件信息
                    std::string existNodeId = rs->getString(0);
                    std::string existServerFilename = rs->getString(1);

                    LOG_INFO << "触发秒传!MD5: " << fileMd5
                             << " 复用物理文件: " << existServerFilename;

                    // 直接为当前用户插入一条新记录，状态直接标记为 'success'
                    std::string fastInsertSql =
                        "INSERT INTO files (filename, original_filename, "
                        "file_size, file_type, user_id, node_id, status, "
                        "file_md5, created_at, updated_at) "
                        "VALUES (?, ?, ?, ?, ?, ?, 'success', ?, NOW(), NOW())";
                    db::MySQLStatement fastStmt(*mysql, fastInsertSql);

                    fastStmt.bindString(
                        existServerFilename); // 指向同一个物理文件
                    fastStmt.bindString(originalFilename);
                    fastStmt.bindInt64(fileSize);
                    fastStmt.bindString(fileType);
                    fastStmt.bindInt(userId);
                    fastStmt.bindString(existNodeId);
                    fastStmt.bindString(fileMd5);
                    if (fastStmt.execute()) {
                        // 返回特殊的 code (比如
                        // 1)，告诉前端秒传成功，不需要再传给 DataNode 了
                        json response = {{"code", 1}, {"message", "秒传成功"}};
                        std::string bodyStr = response.dump();
                        resp->setStatusCode(fn::HttpResponse::k200Ok);
                        resp->setContentType("application/json");
                        resp->setBody(bodyStr);
                        resp->addHeader("Content-Length",
                                        std::to_string(bodyStr.size()));
                        return true;
                    }
                }
            }
        }

        // 从 NodeManager 获取一个存活的 DataNode
        auto dataNode = NodeManager::instance().getAliveNode();
        if (!dataNode) {
            LOG_ERROR << "没有可用的 DataNode 节点";
            sendError(resp, "系统繁忙，当前无可用存储节点",
                      fileserver::net::HttpResponse::k500InternalServerError,
                      conn);
            return true;
        }

        std::string insertSql =
            "INSERT INTO files (filename, original_filename, file_size, "
            "file_type, user_id, node_id, status, file_md5, created_at, "
            "updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, 'uploading', ?, NOW(), NOW())";
        db::MySQLStatement stmt(*mysql, insertSql);

        stmt.bindString(serverFilename);
        stmt.bindString(originalFilename);
        stmt.bindInt64(fileSize);
        stmt.bindString(fileType);
        stmt.bindInt(userId);
        stmt.bindString(dataNode->id_);
        stmt.bindString(fileMd5);

        if (!stmt.execute()) {
            LOG_ERROR << "插入文件记录失败: " << stmt.getError();
            sendError(resp, "数据库错误",
                      fn::HttpResponse::k500InternalServerError, conn);
            return true;
        }

        // 获取生成的文件ID
        auto fileId = stmt.insertId();

        // 生成专属token
        auto fileUploadResponse = TokenManager::instance().generateFileToken(
            userId, fileId, dataNode->id_, serverFilename);

        //  组装响应，告诉客户端去哪里上传
        json response = {
            {"code", 0},
            {"message", "获取上传地址成功"},
            {"data",
             {{"fileId", fileId},
              {"uploadUrl",
               "http://" + dataNode->addr_.toIpPort() + "/api/datanode/upload"},
              {"uploadToken", fileUploadResponse.token}}}};

        std::string bodyStr = response.dump();
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(bodyStr);
        resp->addHeader("Content-Length",
                        std::to_string(bodyStr.size())); // 别忘了这个！

        LOG_INFO << "分配 DataNode 成功: " << dataNode->addr_.toIpPort()
                 << " 给文件 ID: " << fileId;
        return true;
    } catch (const json::parse_error &e) {
        LOG_ERROR << "JSON 解析错误: " << e.what();
        sendError(resp, "无效的请求格式", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR << "处理上传申请失败: " << e.what();
        sendError(resp, "服务器内部错误",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool HttpUploadHandler::handleDownload(
    const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
    std::shared_ptr<fn::HttpResponse> &resp) {

    // 验证用户登录状态
    std::string authHeader = req.getHeader("Authorization");
    std::string userToken =
        (authHeader.empty()) ? req.getQuery("token") : authHeader.substr(7);

    int userId = TokenManager::instance().verifyUserToken(userToken);
    if (userId <= 0) {
        sendError(resp, "请先登录", fn::HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string serverFilename = req.getPathParam("filename");
    if (serverFilename.empty()) {
        sendError(resp, "文件名不能为空", fn::HttpResponse::k400BadRequest,
                  conn);
        return true;
    }

    auto mysql = db::MySQLPool::instance().getConnection();
    std::string querySql =
        "SELECT id, node_id, filename, file_size FROM files WHERE "
        "filename = ? AND user_id = ? AND status = 'success' LIMIT 1";
    db::MySQLStatement stmt(*mysql, querySql);
    stmt.bindString(serverFilename);
    stmt.bindInt(userId);

    if (!stmt.execute()) {
        sendError(resp, "数据库错误", fn::HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    auto rs = stmt.getResultSet();
    if (!rs->next()) {
        sendError(resp, "文件未找到或尚未上传成功",
                  fn::HttpResponse::k404NotFound, conn);
        return true;
    }

    int64_t fileId = rs->getInt64(0);
    std::string nodeId = rs->getString(1);
    std::string filename = rs->getString(2);
    uintmax_t fileSize = rs->getInt64(3);

    // 获取 DataNode 地址
    auto nodeInfo = NodeManager::instance().getNodeInfo(nodeId);
    if (!nodeInfo || !nodeInfo->isAlive_) {
        sendError(resp, "存储节点离线",
                  fn::HttpResponse::k500InternalServerError, conn);
        return true;
    }
    auto fileResponse = TokenManager::instance().generateFileToken(
        userId, fileId, nodeId, serverFilename);

    // 返回 DataNode 播放地址
    json response = {{"code", 0},
                     {"data",
                      {{"downloadUrl", "http://" + nodeInfo->addr_.toIpPort() +
                                           "/api/datanode/download"},
                       {"token", fileResponse.token},
                       {"fileSize", fileSize}}}};

    resp->setStatusCode(fn::HttpResponse::k200Ok);
    std::string respBody = response.dump();
    resp->setBody(respBody);
    resp->addHeader("Content-Length", std::to_string(respBody.size()));
    return true;
}