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

HttpUploadHandler::~HttpUploadHandler() {
    threadPool_.stop();
    // 保存文件名映射
    saveFilenameMapping();
    closeDatabase();
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

bool HttpUploadHandler::onRequest(const TcpConnectionPtr &conn,
                                  HttpRequest &req,
                                  std::shared_ptr<HttpResponse> &resp) {
    std::string path = req.path();
    LOG_INFO << "Headers: " << req.methodString() << " " << path;
    LOG_INFO << "Content-Type: " << req.getHeader("Content-Type");
    LOG_INFO << "Body size: " << req.body().size();

    try {
        // 查找匹配路由
        for (const auto &route : routes_) {
            if (route.method != req.method()) {
                LOG_INFO << "Method mismatch: expected " << route.method
                         << ", got " << req.method();
                continue;
            }

            std::smatch matches;
            if (std::regex_match(path, matches, route.pattern)) {
                LOG_INFO << "Found matching route: " << path;
                // 提取路径参数
                std::unordered_map<std::string, std::string> params;
                for (size_t i = 0;
                     i < route.params.size() && i + 1 < matches.size(); i++) {
                    params[route.params[i]] = matches[i + 1];
                }

                // 将路径参数存储到请求对象中
                req.setPathParams(params);

                // 调用处理函数
                return route.handler(conn, req, resp);
            }
        }

        // 未找到匹配路由，返回404
        LOG_WARN << "No matching route found for " << path;
        return handleNotFound(conn, resp);
    } catch (const std::exception &e) {
        LOG_ERROR << "Error processing request: " << e.what();
        sendError(resp, "Internal Server Error",
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool HttpUploadHandler::initDatabase() {
    mysql = mysql_init(NULL);
    if (!mysql) {
        LOG_ERROR << "MySQL初始化失败";
        return false;
    }

    if (!mysql_real_connect(mysql, dbHost.c_str(), dbUser.c_str(),
                            dbPassword.c_str(), dbName.c_str(), dbPort, NULL,
                            0)) {
        LOG_ERROR << "MySQL连接失败: " << mysql_error(mysql);
        mysql_close(mysql);
        mysql = NULL;
        return false;
    }

    // 设置字符集为utf8
    if (mysql_set_character_set(mysql, "utf8") != 0) {
        LOG_ERROR << "设置字符集失败：" << mysql_error(mysql);
        return false;
    }

    LOG_INFO << "数据库连接成功";
    return true;
}

void HttpUploadHandler::closeDatabase() {
    if (mysql) {
        mysql_close(mysql);
        mysql = NULL;
    }
}

bool HttpUploadHandler::executeQuery(const std::string &query) {
    if (!mysql) {
        LOG_ERROR << "数据库未连接";
        return false;
    }

    if (mysql_query(mysql, query.c_str()) != 0) {
        LOG_ERROR << "执行查询失败：" << mysql_error(mysql);
        return false;
    }
    return true;
}

MYSQL_RES *HttpUploadHandler::executeQueryWithResult(const std::string &query) {
    if (!executeQuery(query)) {
        return NULL;
    }
    return mysql_store_result(mysql);
}

void HttpUploadHandler::initRoutes() {
    // 不需要会话验证的路由
    addRoute("/favicon.ico", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleFavicon(conn, req, resp);
             });
    addRoute("/register", HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleRegister(conn, req, resp);
             });
    addRoute("/upload", HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleFileUpload(conn, req, resp);
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
    addRoute("/login", HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleLogin(conn, req, resp);
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

    // 节点路由
    auto regHandler = std::make_shared<RegisterNodeHandler>();
    addRoute("/registerNode", HttpRequest::kPost,
             [regHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                          std::shared_ptr<HttpResponse> &resp) {
                 return regHandler->handleRegisterNode(conn, req, resp);
             });

    auto hbHandler = std::make_shared<HeartbeatHandler>();
    addRoute("/heartbeat", HttpRequest::kPost,
             [hbHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                         std::shared_ptr<HttpResponse> &resp) {
                 return hbHandler->handleHeartbeat(conn, req, resp);
             });
}

void HttpUploadHandler::addRoute(const std::string &path,
                                 HttpRequest::Method method,
                                 RequestHandler handler) {
    std::string pattern = "^" + escapeRegex(path) + "$";
    routes_.emplace_back(pattern, std::vector<std::string>(), handler, method);
}

// 转移正则表达式特殊字符
std::string HttpUploadHandler::escapeRegex(const std::string &str) {
    std::string result;
    for (char c : str) {
        if (c == '.' || c == '+' || c == '*' || c == '?' || c == '^' ||
            c == '$' || c == '(' || c == ')' || c == '[' || c == ']' ||
            c == '{' || c == '}' || c == '|' || c == '\\') {
            result += '\\';
        }
        result += c;
    }
    return result;
}

// sha256 哈希算法简单实现
std::string HttpUploadHandler::sha256(const std::string &input) {
    std::hash<std::string> hasher;
    auto hash = hasher(input);
    std::stringstream ss;
    ss << std::hex << hash;
    return ss.str();
}

// 会话管理
// 生成会话ID
std::string HttpUploadHandler::generateSessionId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 35);

    const char *chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string sessionId;
    sessionId.reserve(32);

    for (int i = 0; i < 32; i++) {
        sessionId += chars[dis(gen)];
    }

    return sessionId;
}

// 保存会话
void HttpUploadHandler::saveSession(const std::string &sessionId, int userId,
                                    const std::string &username) {
    std::string query =
        "INSERT INTO sessions (session_id, user_id, "
        "username, expire_time) VALUES ('" +
        escapeString(sessionId) + "', " + std::to_string(userId) + ", '" +
        escapeString(username) + "', " + "DATE_ADD(NOW(), INTERVAL 30 MINUTE))";
    executeQuery(query);
}

bool HttpUploadHandler::validateSession(const std::string &sessionId,
                                        int &userId, std::string &username) {
    if (sessionId.empty()) {
        LOG_WARN << "sessionId is empty";
        return false;
    }

    std::string query =
        "SELECT user_id, username FROM sessions WHERE session_id = '" +
        escapeString(sessionId) + "' AND expire_time > NOW()";
    LOG_INFO << "query = " << query;
    MYSQL_RES *result = executeQueryWithResult(query);

    if (!result || mysql_num_rows(result) == 0) {
        if (result) {
            mysql_free_result(result);
        }
        LOG_WARN << "mysql_num_rows is invalid";
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    userId = std::stoi(row[0]);
    username = row[1];
    mysql_free_result(result);

    // 更新回话过期时间
    std::string updateQuery =
        "UPDATE sessions SET expire_time = DATE_ADD(NOW(), INTERVAL 30 "
        "MINUTE) WHERE session_id = '" +
        escapeString(sessionId) + "'";
    executeQuery(updateQuery);
    LOG_INFO << "validateSession success";
    return true;
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

void HttpUploadHandler::sendError(const std::shared_ptr<HttpResponse> &resp,
                                  const std::string &message,
                                  HttpResponse::HttpStatusCode code,
                                  const TcpConnectionPtr &conn) {
    json response = {{"code", static_cast<int>(code)}, {"message", message}};
    resp->setStatusCode(code);
    resp->setStatusMessage(message);
    resp->setContentType("application/json");
    resp->addHeader("Connection", "close");
    resp->setBody(response.dump());

    if (conn) {
        conn->setWriteCompleteCallback(
            [conn](const TcpConnectionPtr &connection) {
                connection->shutdown();
                return true;
            });
    }
}

// url解码函数
std::string HttpUploadHandler::urlDecode(const std::string &encoded) {
    std::string result;
    char ch;
    size_t i;
    int ii;
    size_t len = encoded.length();

    for (i = 0; i < len; i++) {
        if (encoded[i] != '%') {
            if (encoded[i] == '+') {
                result += ' ';
            } else {
                result += encoded[i];
            }
        } else {
            sscanf(encoded.substr(i + 1, 2).c_str(), "%x", &ii);
            ch = static_cast<char>(ii);
            result += ch;
            i = i + 2;
        }
    }
    return result;
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

// 转义SQL字符串
std::string HttpUploadHandler::escapeString(const std::string &str) {
    if (!mysql) {
        return str;
    }

    char *escaped = new char[str.length() * 2 + 1];
    mysql_real_escape_string(mysql, escaped, str.c_str(), str.length());
    std::string result(escaped);
    delete[] escaped;

    return result;
}

bool HttpUploadHandler::handleIndex(const TcpConnectionPtr &conn,
                                    HttpRequest &req,
                                    std::shared_ptr<HttpResponse> &resp) {
    // 1. 设置基础响应头
    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->setContentType("text/html; charset=utf-8");

    // 2. 获取请求路径
    std::string path = req.path();
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
        std::string::size_type pos = exeFullPath.find_last_of("/");
        if (pos == std::string::npos) {
            LOG_ERROR << "Invalid executable path: " << exeFullPath;
            filePath = "../../web/html/index.html";
        } else {
            std::string srcDir = exeFullPath.substr(0, pos); // master/src

            // 步骤2：回退到master目录
            pos = srcDir.find_last_of("/");
            if (pos == std::string::npos) {
                LOG_ERROR << "Failed to find master directory from: " << srcDir;
                filePath = "../web/html/index.html";
            } else {
                std::string masterDir = srcDir.substr(0, pos); // master

                // 步骤3：回退到项目根目录（distributed-fm）
                pos = masterDir.find_last_of("/");
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
    // 验证会话
    std::string sessionId = req.getHeader("X-Session-ID");
    int userId;
    std::string usernameFromSession;

    if (!validateSession(sessionId, userId, usernameFromSession)) {
        sendError(resp, "未登录或会话已过期", HttpResponse::k401Unauthorized,
                  conn);
        return true;
    }

    // 获取文件列表类型，默认只显示自己的文件
    std::string listType = req.getQuery("type", "my");

    std::string query;
    if (listType == "my") {
        // 获取自己的文件select
        query = "SELECT f.id, f.filename, f.original_filename, "
                "f.file_size, "
                "f.file_type, "
                "f.created_at, 1 as is_owner FROM files f WHERE "
                "f.user_id = " +
                std::to_string(userId);
    } else if (listType == "shared") {
        // 获取分享给自己的文件
        query = "SELECT f.id, f.filename, f.original_filename, "
                "f.file_size, f.file_type, "
                "f.created_at, 0 as is_owner FROM files f "
                "JOIN file_shares fs ON f.id = fs.file_id "
                "WHERE (fs.shared_with_id = " +
                std::to_string(userId) +
                " OR fs.share_type = 'public') "
                "AND f.user_id != " +
                std::to_string(userId);
    } else if (listType == "all") {
        // 获取所有文件(包括自己和分享的)
        query = "SELECT f.id, f.filename, f.original_filename, "
                "f.file_size, f.file_type, "
                "f.created_at, CASE WHEN f.user_id = " +
                std::to_string(userId) +
                " THEN 1 ELSE 0 END as is_owner "
                "FROM files f "
                "LEFT JOIN file_shares fs ON f.id = fs.file_id "
                "WHERE f.user_id = " +
                std::to_string(userId) +
                " OR "
                "fs.shared_with_id = " +
                std::to_string(userId) + " OR fs.share_type = 'public'";
    }
    LOG_INFO << "query = " << query;
    MYSQL_RES *result = executeQueryWithResult(query);

    json response;
    response["code"] = 0;
    response["message"] = "Success";
    json files = json::array();

    if (result) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            int fileId = std::stoi(row[0]);
            std::string filename = row[1];
            std::string originalFilename = row[2];
            uintmax_t fileSize = std::stoull(row[3]);
            std::string fileType = row[4];
            std::string createdAt = row[5];
            bool isOwner = (std::stoi(row[6]) == 1);

            // 如果是文件所有者，获取分享信息
            json shareInfo = nullptr;
            if (isOwner) {
                std::string shareQuery =
                    "SELECT share_type, shared_with_id, share_code, "
                    "expire_time, extract_code FROM file_shares "
                    "WHERE file_id = " +
                    std::to_string(fileId);
                MYSQL_RES *shareResult = executeQueryWithResult(shareQuery);

                if (shareResult && mysql_num_rows(shareResult) > 0) {
                    MYSQL_ROW shareRow = mysql_fetch_row(shareResult);
                    std::string shareType = shareRow[0];

                    shareInfo = {{"type", shareType},
                                 {"shareCode", shareRow[2] ? shareRow[2] : ""}};

                    if (shareType == "protected" && shareRow[4]) {
                        shareInfo["extractCode"] = shareRow[4];
                    }

                    if (shareType == "user" && shareRow[1]) {
                        int sharedWithId = std::stoi(shareRow[1]);
                        // 获取共享用户的用户名
                        std::string userQuery =
                            "SELECT username FROM users WHERE id = " +
                            std::to_string(sharedWithId);
                        MYSQL_RES *userResult =
                            executeQueryWithResult(userQuery);
                        if (userResult && mysql_num_rows(userResult) > 0) {
                            MYSQL_ROW userRow = mysql_fetch_row(userResult);
                            shareInfo["sharedWithUsername"] = userRow[0];
                            shareInfo["sharedWithId"] = sharedWithId;
                        }
                        if (userResult) {
                            mysql_free_result(userResult);
                        }
                    }

                    if (shareRow[3]) {
                        shareInfo["expireTime"] = shareRow[3];
                    }
                }
                if (shareResult) {
                    mysql_free_result(shareResult);
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
        response["files"] = files;
    }

    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->setContentType("application/json");
    resp->addHeader("Connection", "close");
    resp->setBody(response.dump());

    // 设置写完成回调以关闭连接
    conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
        LOG_INFO << "List files complete, closing connection";
        connection->shutdown();
        return true;
    });

    return true;
}

bool HttpUploadHandler::handleFileUpload(const TcpConnectionPtr &conn,
                                         HttpRequest &req,
                                         std::shared_ptr<HttpResponse> &resp) {
    auto respPtr = resp;
    // 设置为异步响应
    respPtr->setAsync(true);

    // 验证会话
    std::string sessionId = req.getHeader("X-Session-ID");
    int userId;
    std::string usernameFromSession;

    if (!validateSession(sessionId, userId, usernameFromSession)) {
        sendError(respPtr, "未登录或会话已过期", HttpResponse::k401Unauthorized,
                  conn);
        // 设置回同步
        respPtr->setAsync(false);
        return true;
    }

    // 获取HttpContext
    auto httpContext =
        std::static_pointer_cast<HttpContext>(conn->getContext());
    if (!httpContext) {
        LOG_ERROR << "HttpContext is null";
        sendError(respPtr, "Internal Server Error",
                  HttpResponse::k500InternalServerError, conn);
        // 设置回同步
        respPtr->setAsync(false);
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
            sendError(respPtr, "Content-Type header is missing",
                      HttpResponse::k400BadRequest, conn);
            // 设置回同步
            respPtr->setAsync(false);
            return true;
        }

        std::regex boundaryRegex("boundary=(.+)$");
        std::smatch matches;
        if (!std::regex_search(contentType, matches, boundaryRegex)) {
            sendError(respPtr, "Invalid Content-Type",
                      HttpResponse::k400BadRequest, conn);
            // 设置回同步
            respPtr->setAsync(false);
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
                    sendError(respPtr, "Request body is empty",
                              HttpResponse::k400BadRequest, conn);
                    // 设置回同步
                    respPtr->setAsync(false);
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

            // 选择一个DataNode
            auto datanode = NodeManager::instance().getAliveNode();
            if (!datanode) {
                sendError(respPtr, "No available DataNode",
                          HttpResponse::k500InternalServerError, conn);
                // 设置回同步
                respPtr->setAsync(false);
                return true;
            }

            auto remoteStorage = std::make_shared<RemoteFileStorage>(
                datanode->addr_, conn->getLoop());

            // 开始发起连接请求，remotestorage已经设置了连接回调，发送open命令并补发保存在buffer中的数据
            // 然后才调用用户设置的connection回调。
            remoteStorage->open(filepath);

            // ======原有逻辑修改：创建上传上下文并关联远程存储 ======
            uploadContext = std::make_shared<FileUploadContext>(
                filename, originalFilename, remoteStorage);
            uploadContext->setBoundary(boundary);
            httpContext->setContext(uploadContext);

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
                    // ========== 核心修改：精确去掉结束边界前的 \r\n ==========
                    size_t validEnd = endPos;
                    // 1. 先去掉结束边界前的 \n
                    if (validEnd > pos && body[validEnd - 1] == '\n') {
                        validEnd--;
                        LOG_INFO << "去掉末尾的 \\n,validEnd = " << validEnd;
                    }
                    // 2. 再去掉结束边界前的 \r
                    if (validEnd > pos && body[validEnd - 1] == '\r') {
                        validEnd--;
                        LOG_INFO << "去掉末尾的 \\r,validEnd = " << validEnd;
                    }

                    // 3. 只写入 0 到 validEnd 之间的有效数据
                    if (validEnd > pos) {
                        size_t writeLen = validEnd - pos;
                        uploadContext->writeData(body.data() + pos, writeLen);
                        LOG_INFO << "Wrote " << writeLen
                                 << " bytes (有效数据), total: "
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
            sendError(respPtr, "Failed to create file",
                      HttpResponse::k500InternalServerError, conn);
            // 设置回同步
            respPtr->setAsync(false);
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
                        // ========== 核心修改：精确去掉结束边界前的 \r\n
                        // ==========
                        size_t validEnd = endPos;
                        // 1. 先去掉结束边界前的 \n
                        if (validEnd > 0 && body[validEnd - 1] == '\n') {
                            validEnd--;
                            LOG_INFO << "去掉末尾的 \\n,validEnd = "
                                     << validEnd;
                        }
                        // 2. 再去掉结束边界前的 \r
                        if (validEnd > 0 && body[validEnd - 1] == '\r') {
                            validEnd--;
                            LOG_INFO << "去掉末尾的 \\r,validEnd = "
                                     << validEnd;
                        }
                        // 3. 只写入 0 到 validEnd 之间的有效数据
                        if (validEnd > 0) {
                            size_t writeLen = validEnd;
                            uploadContext->writeData(body.data(), writeLen);
                            LOG_INFO << "Wrote " << writeLen
                                     << " bytes (有效数据), total: "
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
                case FileUploadContext::State::kFailed: {
                    // 上传失败
                    sendError(respPtr,
                              "Upload failed (remote node connect timeout)",
                              HttpResponse::k500InternalServerError, conn);
                    httpContext->setContext(nullptr);
                    respPtr->setAsync(false);
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
            sendError(respPtr, "Failed to process data",
                      HttpResponse::k500InternalServerError, conn);
            // 设置回同步
            respPtr->setAsync(false);
            return true;
        }
    }
    req.setBody(""); // 清空请求体

    // 检查是否还在接收数据
    if (uploadContext->getState() != FileUploadContext::State::kComplete ||
        !httpContext->gotAll()) {
        LOG_INFO << "等待更多上传数据, 当前状态："
                 << static_cast<int>(uploadContext->getState());
        return false;
    }

    auto remoteStorage = uploadContext->getStorage<RemoteFileStorage>();
    if (!remoteStorage) {
        LOG_ERROR << "RemoteFileStorage is null";
        sendError(respPtr, "RemoteFileStorage is null",
                  HttpResponse::k500InternalServerError, conn);
        httpContext->setContext(nullptr); // 清理上下文，释放资源
                                          // 设置回同步
        respPtr->setAsync(false);
        return true;
    }

    auto self = shared_from_this();

    remoteStorage->setCloseCmdSentCallback([self, conn, respPtr,
                                            userId](bool success) {
        // 回调内唯一安全操作：从conn获取上下文
        auto httpContext =
            std::static_pointer_cast<HttpContext>(conn->getContext());
        if (!httpContext) {
            LOG_WARN << "HttpContext 已释放";
            return;
        }

        auto uploadContext = httpContext->getContext<FileUploadContext>();
        if (!uploadContext) {
            LOG_WARN << "FileUploadContext 已释放";
            return;
        }

        // close命令发送失败
        if (!success) {
            LOG_ERROR << "CLOSE 命令发送失败, 文件："
                      << uploadContext->getFilename();
            sendError(respPtr, "上传失败:数据节点连接异常",
                      HttpResponse::k500InternalServerError, conn);
            httpContext->setContext(nullptr);
            // 异步手动发送
            if (conn->connected()) {
                Buffer buf;
                respPtr->appendToBuffer(&buf);
                conn->setWriteCompleteCallback(
                    [](const fn::TcpConnectionPtr &conn) {
                        conn->shutdown(); // 发送完成后再关闭
                    });
                conn->send(&buf);
            }
            return;
        }

        // close发送成功，执行数据库操作
        // 上传完成，准备响应
        std::string serverFilename =
            fs::path(uploadContext->getFilename()).filename().string();
        std::string originalFilename = uploadContext->getOriginalFilename();
        uintmax_t fileSize = uploadContext->getTotalBytes();

        // 检测文件类型
        std::string fileType = self->getFileType(originalFilename);
        uint64_t fileId = 0;
        bool dbSuccess = false;

        // 开始数据库事务
        try {
            if (self->executeQuery("START TRANSACTION;")) {
                // 插入文件基本信息
                std::string fileQuery = "INSERT INTO files (filename, "
                                        "original_filename, file_size, "
                                        "file_type, user_id) VALUES ('" +
                                        self->escapeString(serverFilename) +
                                        "', '" +
                                        self->escapeString(originalFilename) +
                                        "', " + std::to_string(fileSize) +
                                        ", '" + self->escapeString(fileType) +
                                        "', " + std::to_string(userId) + ")";

                if (self->executeInsertQuery(fileQuery, fileId)) {
                    // 插入DataNode存储信息
                    auto addr = uploadContext->getStorage<RemoteFileStorage>()
                                    ->getAddr();
                    std::string datanode_ip = addr.toIp();
                    uint16_t datanode_port = addr.port();
                    std::string nodeQuery =
                        "INSERT INTO file_storage (file_id, "
                        "datanode_ip, datanode_port) VALUES (" +
                        std::to_string(fileId) + ", '" +
                        self->escapeString(datanode_ip) + "', " +
                        std::to_string(datanode_port) + ")";

                    if (self->executeQuery(nodeQuery)) {
                        self->executeQuery("COMMIT;");
                        dbSuccess = true;
                        LOG_INFO << "文件信息入库成功,file_id:" << fileId;
                    } else {
                        LOG_ERROR << "存储节点信息入库失败,回滚事务";
                        self->executeQuery("ROLLBACK;");
                    }
                } else {
                    LOG_ERROR << "文件基本信息入库失败，回滚事务";
                    self->executeQuery("ROLLBACK;");
                }
            } else {
                LOG_ERROR << "开启数据库事务失败";
            }
        } catch (const std::exception &e) {
            LOG_ERROR << "数据库操作异常: " << e.what();
            self->executeQuery("ROLLBACK;");
            httpContext->setContext(nullptr);
        }

        // 处理数据库结果
        if (dbSuccess) {
            // 发送响应
            json response = {{"code", 0},
                             {"message", "上传成功"},
                             {"fileId", fileId},
                             {"filename", serverFilename},
                             {"originalFilename", originalFilename},
                             {"size", fileSize}};

            respPtr->setStatusCode(HttpResponse::k200Ok);
            respPtr->setStatusMessage("OK");
            respPtr->setContentType("application/json");
            respPtr->addHeader("Connection", "close");
            respPtr->setBody(response.dump());

            LOG_DEBUG << "准备发送上传成功响应";

            if (conn->connected()) {
                // 异步响应必须手动send
                Buffer buf;
                respPtr->appendToBuffer(&buf);
                conn->setWriteCompleteCallback(
                    [](const fn::TcpConnectionPtr &conn) {
                        if (conn->connected())
                            conn->shutdown();
                    });
                conn->send(&buf);
                LOG_DEBUG << "已发送上传成功响应";
            }
        } else {
            sendError(respPtr, "上传失败：数据库操作异常",
                      HttpResponse::k500InternalServerError, conn);
        }

        // 清理上下文
        httpContext->setContext(nullptr);
    });

    remoteStorage->close();
    return false;
}

// 用户注册
bool HttpUploadHandler::handleRegister(const TcpConnectionPtr &conn,
                                       HttpRequest &req,
                                       std::shared_ptr<HttpResponse> &resp) {
    LOG_INFO << "Handling register request";
    LOG_INFO << "Request body: " << req.body();
    try {
        json requestData = json::parse(req.body());
        std::string username = requestData["username"];
        std::string password = requestData["password"];
        std::string email = requestData.value("email", "");

        LOG_INFO << "Register attempt for username: " << username;
        // 简单验证
        if (username.empty() || password.empty()) {
            sendError(resp, "用户名和密码不能为空",
                      HttpResponse::k400BadRequest, conn);
            return true;
        }

        // 哈希密码
        // 在实际应用中应使用更安全的哈希算法，这里简化处理
        std::string hashedPassword = sha256(password);

        // 检查用户名是否已存在
        std::string checkQuery = "SELECT id FROM users WHERE username = '" +
                                 escapeString(username) + "'";
        MYSQL_RES *result = executeQueryWithResult(checkQuery);

        if (result && mysql_num_rows(result) > 0) {
            mysql_free_result(result);
            sendError(resp, "用户名已存在", HttpResponse::k400BadRequest, conn);
            return true;
        }

        if (result) {
            mysql_free_result(result);
        }

        // 插入新用户
        std::string insertQuery =
            "INSERT INTO users (username, password, email) VALUES ('" +
            escapeString(username) + "', '" + escapeString(hashedPassword) +
            "', " +
            (email.empty() ? "NULL" : ("'" + escapeString(email) + "'")) + ")";

        if (!executeQuery(insertQuery)) {
            sendError(resp, "注册失败，请稍后重试",
                      HttpResponse::k500InternalServerError, conn);
            return true;
        }

        // 获取新用户ID
        int userId = static_cast<int>(mysql_insert_id(mysql));

        json response = {
            {"code", 0}, {"message", "注册成功"}, {"userId", userId}};

        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("application/json");
        resp->addHeader("Connection", "close");
        resp->setBody(response.dump());

        conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
            connection->shutdown();
            return true;
        });
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR << "用户注册错误: " << e.what();
        sendError(resp, "注册失败: " + std::string(e.what()),
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

// 用户登录
bool HttpUploadHandler::handleLogin(const TcpConnectionPtr &conn,
                                    HttpRequest &req,
                                    std::shared_ptr<HttpResponse> &resp) {
    try {
        json requestData = json::parse(req.body());
        std::string username = requestData["username"];
        std::string password = requestData["password"];

        // 验证参数
        if (username.empty() || password.empty()) {
            sendError(resp, "用户名和密码不能为空",
                      HttpResponse::k400BadRequest, conn);
            return true;
        }

        // 哈希密码
        std::string hashedPassword = sha256(password);

        // 查询用户
        std::string query =
            "SELECT id, username FROM users WHERE username = '" +
            escapeString(username) + "' AND password = '" +
            escapeString(hashedPassword) + "'";

        MYSQL_RES *result = executeQueryWithResult(query);

        if (!result || mysql_num_rows(result) == 0) {
            if (result) {
                mysql_free_result(result);
            }
            sendError(resp, "用户名或密码错误", HttpResponse::k401Unauthorized,
                      conn);
            return true;
        }

        MYSQL_ROW row = mysql_fetch_row(result);
        int userId = std::stoi(row[0]);
        std::string usernameFromDb = row[1];
        mysql_free_result(result);

        // 创建会话
        std::string sessionId = generateSessionId();
        saveSession(sessionId, userId, usernameFromDb);

        json response = {{"code", 0},
                         {"message", "登录成功"},
                         {"sessionId", sessionId},
                         {"userId", userId},
                         {"username", usernameFromDb}};

        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("application/json");
        resp->addHeader("Connection", "close");
        resp->setBody(response.dump());

        conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
            connection->shutdown();
            return true;
        });

        return true;

    } catch (const std::exception &e) {
        LOG_ERROR << "用户登录错误: " << e.what();
        sendError(resp, "登录失败: " + std::string(e.what()),
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool HttpUploadHandler::handleFavicon(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) { // 获取当前文件目录
    std::string currentDir = __FILE__;
    std::string::size_type pos = currentDir.find_last_of("/");
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

bool HttpUploadHandler::handleNotFound(const TcpConnectionPtr &conn,
                                       std::shared_ptr<HttpResponse> &resp) {
    json response = {{"code", 404}, {"message", "Not Found"}};
    resp->setStatusCode(HttpResponse::k404NotFound);
    resp->setContentType("application/json");
    resp->addHeader("Connection", "close");
    resp->setBody(response.dump());

    conn->setWriteCompleteCallback([conn](const TcpConnectionPtr &connection) {
        connection->shutdown();
        return true;
    });
    return true;
}