
#pragma once
#include "BaseHandler.h"
#include "FileUploadContext.h"
#include "NodeManager.h"
#include "UserHandler.h"
#include "base/ThreadPool.h"
#include "db/MySQLPool.h"
#include "db/MySQLStatement.h"
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

  public:
    HttpUploadHandler(int numThreads);

    ~HttpUploadHandler();

    // 加载文件名映射
    void loadFilenameMapping();

    ///@brief 初始化路由，覆盖
    void initRoutes() override;

    ///@brief 链接回调，覆盖
    void onConnection(const TcpConnectionPtr &conn) override;

  private:
    void saveFilenameMapping();

    void saveFilenameMappingInternal();

    void loadFilenameMappingInternal();

    void addFilenameMapping(const std::string &serverFilename,
                            const std::string &originalFilename);

    std::string generateUniqueFilename(const std::string &prefix);

    bool handleFavicon(const TcpConnectionPtr &conn, HttpRequest &req,
                       std::shared_ptr<HttpResponse> &resp);

    bool handleIndex(const TcpConnectionPtr &conn, HttpRequest &req,
                     std::shared_ptr<HttpResponse> &resp);

    bool handleListFiles(const TcpConnectionPtr &conn, HttpRequest &req,
                         std::shared_ptr<HttpResponse> &resp);

    bool handleFileUpload(const TcpConnectionPtr &conn, HttpRequest &req,
                          std::shared_ptr<HttpResponse> &resp);

    bool handleDownload(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                        std::shared_ptr<fn::HttpResponse> &resp);
};