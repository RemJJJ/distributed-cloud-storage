
#pragma once
#include "BaseHandler.h"
#include "FileUploadContext.h"
#include "NodeManager.h"
#include "UserHandler.h"
#include "base/ThreadPool.h"
#include "db/MySQLPool.h"
#include "db/MySQLStatement.h"
#include "net/EventLoop.h"
#include "net/TimerId.h"
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

class MasterHttpHandler : public BaseHandler {
  private:
    ThreadPool threadPool_; // 线程池
    std::string uploadDir_; // 上传目录

  public:
    MasterHttpHandler(int numThreads = 4);

    ~MasterHttpHandler();

    ///@brief 初始化路由，覆盖
    void initRoutes() override;

    ///@brief 链接回调，覆盖
    void onConnection(const TcpConnectionPtr &conn) override;

    void startGC(EventLoop *loop) {
        loop->runEvery(60.0,
                       std::bind(&MasterHttpHandler::processGarbageCollection,
                                 this, loop));
        loop->runEvery(60.0,
                       std::bind(&MasterHttpHandler::processStaleUploadingFiles,
                                 this, loop));
        loop->runEvery(60.0,
                       std::bind(&MasterHttpHandler::processExpiredRecycleBin,
                                 this, loop));
        loop->runEvery(10.0, std::bind(&MasterHttpHandler::processHotCacheTasks,
                                       this, loop));
        LOG_INFO << "Async garbage collection timer start";
    }

  private:
    void addFilenameMapping(const std::string &serverFilename,
                            const std::string &originalFilename);

    bool handleFavicon(const TcpConnectionPtr &conn, HttpRequest &req,
                       std::shared_ptr<HttpResponse> &resp);

    bool handleIndex(const TcpConnectionPtr &conn, HttpRequest &req,
                     std::shared_ptr<HttpResponse> &resp);

    void processGarbageCollection(EventLoop *loop);
    void processStaleUploadingFiles(EventLoop *loop);
    void processExpiredRecycleBin(EventLoop *loop);
    void processHotCacheTasks(EventLoop *loop);
};
