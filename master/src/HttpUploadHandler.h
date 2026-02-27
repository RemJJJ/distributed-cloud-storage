
#pragma once
#include "../../third_party/muduo/base/Logging.h"
#include "../../third_party/muduo/base/ThreadPool.h"
#include "../../third_party/muduo/net/EventLoop.h"
#include "../../third_party/muduo/net/HttpContext.h"
#include "../../third_party/muduo/net/HttpRequest.h"
#include "../../third_party/muduo/net/HttpResponse.h"
#include "../../third_party/muduo/net/HttpServer.h"
#include "FileUploadContext.h"
#include "NodeManager.h"
#include <chrono>
#include <cstdint>
#include <exception>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <experimental/filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <mysql/mysql.h>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
using namespace fileserver;
using namespace fileserver::net;
using json = nlohmann::json;
namespace fs = std::filesystem;

class HttpUploadHandler
    : public std::enable_shared_from_this<HttpUploadHandler> {
  private:
    ThreadPool threadPool_;                    // 线程池
    std::shared_ptr<NodeManager> nodeManager_; // 节点管理器
    std::string uploadDir_;                    // 上传目录
    std::string mappingFile_;                  // 文件名映射文件
    std::atomic<int> activeRequests_;          // 活跃请求计数
    std::mutex mappingMutex_;                  // 保护文件名映射的互斥锁
    std::map<std::string, std::string>
        filenameMapping_; // <服务器文件名, 原始文件名>

    // MySQL连接
    MYSQL *mysql;
    std::string dbHost;
    std::string dbUser;
    std::string dbPassword;
    std::string dbName;
    unsigned int dbPort;

    // 定义处理函数类型
    using RequestHandler =
        bool (HttpUploadHandler::*)(const TcpConnectionPtr &, HttpRequest &,
                                    std::shared_ptr<HttpResponse> &);

    // 路由模式结构
    struct RoutePattern {
        std::regex pattern;              // 正则表达式模式
        std::vector<std::string> params; // 路径参数名列表
        RequestHandler handler;          // 处理函数
        HttpRequest::Method method;      // HTTP方法

        RoutePattern(const std::string &pattern_str,
                     const std::vector<std::string> &param_names,
                     RequestHandler h, HttpRequest::Method m)
            : pattern(pattern_str), params(param_names), handler(h), method(m) {
        }
    };

    // 路由表
    std::vector<RoutePattern> routes_;

    // 初始化数据库连接
    bool initDatabase();

    // 关闭数据库连接
    void closeDatabase();

    // 执行SQL查询
    bool executeQuery(const std::string &query);

    // 执行SQL并获取自增ID
    bool executeInsertQuery(const std::string &query, uint64_t &lastInsertId) {
        if (!executeQuery(query)) {
            return false;
        }
        lastInsertId = mysql_insert_id(mysql);
        return true;
    }

    // 执行SQL查询并获取结果
    MYSQL_RES *executeQueryWithResult(const std::string &query);

  public:
    HttpUploadHandler(int numThreads, const std::string &dbHost = "localhost",
                      const std::string &dbUser = "RemJJJ",
                      const std::string &dbPassword = "mysql@RemJJJ",
                      const std::string &dbName = "file_manager",
                      unsigned int dbPort = 3307);

    ~HttpUploadHandler();

    // 加载文件名映射
    void loadFilenameMapping();

    void initRoutes();

    void onConnection(const TcpConnectionPtr &conn);

    bool onRequest(const TcpConnectionPtr &conn, HttpRequest &req,
                   std::shared_ptr<HttpResponse> &resp);

  private:
    void addRoute(const std::string &path, HttpRequest::Method method,
                  RequestHandler handler);

    std::string generateSessionId();

    std::string sha256(const std::string &input);

    std::string escapeRegex(const std::string &str);

    void saveSession(const std::string &sessionId, int userId,
                     const std::string &username);

    void saveFilenameMapping();

    void saveFilenameMappingInternal();

    void loadFilenameMappingInternal();

    void addFilenameMapping(const std::string &serverFilename,
                            const std::string &originalFilename);

    bool validateSession(const std::string &sessionId, int &userId,
                         std::string &username);

    static void sendError(const std::shared_ptr<HttpResponse> &resp,
                          const std::string &message,
                          HttpResponse::HttpStatusCode code,
                          const TcpConnectionPtr &conn);

    std::string urlDecode(const std::string &encoded);

    std::string generateUniqueFilename(const std::string &prefix);

    std::string getFileType(const std::string &filename);

    std::string escapeString(const std::string &str);

    bool handleFavicon(const TcpConnectionPtr &conn, HttpRequest &req,
                       std::shared_ptr<HttpResponse> &resp);

    bool handleIndex(const TcpConnectionPtr &conn, HttpRequest &req,
                     std::shared_ptr<HttpResponse> &resp);

    bool handleListFiles(const TcpConnectionPtr &conn, HttpRequest &req,
                         std::shared_ptr<HttpResponse> &resp);

    bool handleRegister(const TcpConnectionPtr &conn, HttpRequest &req,
                        std::shared_ptr<HttpResponse> &resp);

    bool handleLogin(const TcpConnectionPtr &conn, HttpRequest &req,
                     std::shared_ptr<HttpResponse> &resp);

    bool handleFileUpload(const TcpConnectionPtr &conn, HttpRequest &req,
                          std::shared_ptr<HttpResponse> &resp);

    bool handleNotFound(const TcpConnectionPtr &conn,
                        std::shared_ptr<HttpResponse> &resp);
};