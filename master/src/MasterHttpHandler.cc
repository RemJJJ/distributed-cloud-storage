#include "MasterHttpHandler.h"
#include "AsyncDeleteTask.h"
#include "DataNodeHandler.h"
#include "FileHandler.h"
#include "NodeManager.h"
#include "TokenManager.h"
#include "db/MySQLPool.h"
#include "db/MySQLStatement.h"
#include "net/EventLoop.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include <memory>

MasterHttpHandler::MasterHttpHandler(int numThreads)
    : threadPool_("UploadHandler"), uploadDir_("uploads") {
    threadPool_.start(numThreads);

    // 创建上传目录
    if (!fs::exists(uploadDir_)) {
        fs::create_directory(uploadDir_);
    }

    // 初始化路由表
    initRoutes();
}

MasterHttpHandler::~MasterHttpHandler() { threadPool_.stop(); }

void MasterHttpHandler::processGarbageCollection(EventLoop *loop) {
    auto mysql = db::MySQLPool::instance().getConnection();
    if (!mysql)
        return;

    // 查出所有等待物理删除的文件(is_deleted = 2)
    // 每次最多处理50个，防止瞬间发起太多TCP连接耗尽资源
    std::string sql =
        "SELECT id, node_id, filename FROM files WHERE is_deleted = 2 LIMIT 50";
    db::MySQLStatement stmt(*mysql, sql);
    if (!stmt.execute()) {
        LOG_ERROR << "GC scan database failed: " << stmt.getError();
        return;
    }

    auto rs = stmt.getResultSet();
    int taskCount = 0;

    while (rs->next()) {
        int fileId = rs->getInt(0);
        std::string nodeId = rs->getString(1);
        std::string serverFilename = rs->getString(2);

        // 检查文件所在datanode是否存活
        auto nodeInfo = NodeManager::instance().getNodeInfo(nodeId);
        if (nodeInfo && nodeInfo->isAlive_) {
            // 签发删除Token
            std::string deleteToken =
                TokenManager::instance().generateDeleteToken(serverFilename);

            // 创建并启动异步删除任务
            auto task = std::make_shared<AsyncDeleteTask>(loop, nodeInfo->addr_,
                                                          deleteToken, fileId);

            task->start(); // 异步执行
            taskCount++;
        } else {
            LOG_DEBUG << "GC escape: DataNode " << nodeId
                      << " not online, FileID: " << fileId;
        }
    }

    if (taskCount > 0) {
        LOG_INFO << "This round GC start " << taskCount
                 << " async physial delete task(s)";
    }
}

void MasterHttpHandler::onConnection(const TcpConnectionPtr &conn) {
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

void MasterHttpHandler::initRoutes() {
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
    addRoute("/s/([^/]+)", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 // 读取本地的 share.html 并返回给浏览器
                 return handleIndex(conn, req, resp);
             },
             {"share_id"}); // 捕获 share_id

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
    addRoute("/logout", fileserver::net::HttpRequest::kPost,
             [userHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return userHandler->handleLogout(conn, req, resp);
             });
    addRoute("/users/search", fileserver::net::HttpRequest::kGet,
             [userHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return userHandler->handleSearchUsers(conn, req, resp);
             });
    addRoute("/share", fileserver::net::HttpRequest::kPost,
             [userHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return userHandler->handleShareFile(conn, req, resp);
             });
    addRoute("/share/cancel", HttpRequest::kPost,
             [userHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return userHandler->handleCancelShare(conn, req, resp);
             });
    addRoute("/share/info", HttpRequest::kGet,
             [userHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return userHandler->handleShareInfo(conn, req, resp);
             });
    addRoute("/share/verify", HttpRequest::kPost,
             [userHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return userHandler->handleShareVerify(conn, req, resp);
             });
    addRoute("/share/received", HttpRequest::kGet,
             [userHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return userHandler->handleListSharedWithMe(conn, req, resp);
             });

    // FileHandler
    auto fileHandler = std::make_shared<FileHandler>();
    addRoute("/upload", HttpRequest::kPost,
             [fileHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return fileHandler->handleFileUpload(conn, req, resp);
             });
    addRoute("/files", HttpRequest::kGet,
             [fileHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return fileHandler->handleListFiles(conn, req, resp);
             });
    addRoute("/download/([^/]+)", fileserver::net::HttpRequest::kGet,
             [fileHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return fileHandler->handleFileDownload(conn, req, resp);
             },
             {"filename"});
    addRoute("/delete", fileserver::net::HttpRequest::kDelete,
             [fileHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return fileHandler->handleDeleteFile(conn, req, resp);
             });
    addRoute("/recycle_bin", fileserver::net::HttpRequest::kGet,
             [fileHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return fileHandler->handleListRecycleBin(conn, req, resp);
             });
    addRoute("/restore", fileserver::net::HttpRequest::kPost,
             [fileHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return fileHandler->handleRestoreFile(conn, req, resp);
             });
    addRoute("/hard_delete", fileserver::net::HttpRequest::kPost,
             [fileHandler](const TcpConnectionPtr &conn, HttpRequest &req,
                           std::shared_ptr<HttpResponse> &resp) {
                 return fileHandler->handleHardDelete(conn, req, resp);
             });

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

// 🌟 重构后的 handleIndex：通用的静态 HTML 页面处理器
bool MasterHttpHandler::handleIndex(const TcpConnectionPtr &conn,
                                    HttpRequest &req,
                                    std::shared_ptr<HttpResponse> &resp) {
    // 1. 确定请求的物理文件名
    std::string targetFile;
    const std::string &path = req.path();
    LOG_DEBUG << path;

    if (path == "/" || path == "/index.html") {
        targetFile = "index.html";
    } else if (path == "/register.html") {
        targetFile = "register.html";
    } else if (path.find("/s/") == 0) { // 匹配 /s/aB3x9Y 这种短链
        targetFile = "share.html";
    } else {
        targetFile = "index.html"; // 默认兜底
    }
    LOG_DEBUG << targetFile;

    // 2. 确定 Web 根目录
    // 假设你在 build 目录下执行 ./master/master，那么相对路径就是 ./html/
    // 建议在生产环境中，把这个路径写进 Config.h 里
    std::string webRoot = "./html/";
    std::string filePath = webRoot + targetFile;

    LOG_INFO << "Serving static file: " << filePath << " for path: " << path;

    // 3. 读取文件内容
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR << "Failed to open " << filePath;
        // 如果找不到文件，返回 404 而不是 500
        sendError(resp, "404 Not Found: " + targetFile,
                  HttpResponse::k404NotFound, conn);
        return true;
    }

    // 高效读取整个文件到 string
    std::string html((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    file.close();

    // 4. 组装响应
    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->setContentType("text/html; charset=utf-8");
    resp->setBody(html);
    resp->addHeader("Content-Length", std::to_string(html.size()));
    resp->addHeader("Connection", "close");

    conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
        connection->shutdown();
        return true;
    });

    return true;
}

bool MasterHttpHandler::handleFavicon(
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