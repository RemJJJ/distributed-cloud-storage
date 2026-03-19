
#pragma once
#include "BaseHandler.h"
#include "FileUploadContext.h"
#include "HeartbeatHandler.h"
#include "NodeManager.h"
#include "RegisterNodeHandler.h"
#include "base/ThreadPool.h"
#include <atomic>
#include <cstdio>
#include <experimental/filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <mysql/mysql.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
namespace fs = std::filesystem;

class HttpUploadHandler : public BaseHandler {
  private:
    ThreadPool threadPool_;           // 线程池
    std::string uploadDir_;           // 上传目录
    std::string mappingFile_;         // 文件名映射文件
    std::atomic<int> activeRequests_; // 活跃请求计数
    std::mutex mappingMutex_;         // 保护文件名映射的互斥锁
    std::map<std::string, std::string>
        filenameMapping_; // <服务器文件名, 原始文件名>

    // MySQL连接
    MYSQL *mysql;
    std::string dbHost;
    std::string dbUser;
    std::string dbPassword;
    std::string dbName;
    unsigned int dbPort;

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

    ///@brief 初始化路由，覆盖
    void initRoutes() override;

    ///@brief 链接回调，覆盖
    void onConnection(const TcpConnectionPtr &conn) override;

  private:
    std::string generateSessionId();

    std::string sha256(const std::string &input);

    void saveSession(const std::string &sessionId, int userId,
                     const std::string &username);

    void saveFilenameMapping();

    void saveFilenameMappingInternal();

    void loadFilenameMappingInternal();

    void addFilenameMapping(const std::string &serverFilename,
                            const std::string &originalFilename);

    bool validateSession(const std::string &sessionId, int &userId,
                         std::string &username);

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
};