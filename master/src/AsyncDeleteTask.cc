#include "AsyncDeleteTask.h"
#include "base/Timestamp.h"
#include "db/MySQLPool.h"
#include "db/MySQLStatement.h"
#include "net/Buffer.h"
#include "net/Callbacks.h"
#include "net/TcpClient.h"
#include <string>

AsyncDeleteTask::AsyncDeleteTask(fileserver::net::EventLoop *loop,
                                 const fileserver::net::InetAddress &dnAddr,
                                 const std::string &deleteToken, int fileId)
    : client_(loop, dnAddr, "AsyncDeleteTask_" + std::to_string(fileId)),
      deleteToken_(deleteToken), fileId_(fileId) {}

AsyncDeleteTask::~AsyncDeleteTask() {
    LOG_DEBUG << "AsyncDeleteTask destory, FileID: " << fileId_;
}

void AsyncDeleteTask::start() {
    // 在这里绑定回调，并使用 shared_from_this()
    // 不直接调用 conn->shutdown()，调用 TcpClient 的 disconnect()
    // TcpClient 会走完整的清理流程，把 Channel 从 EventLoop 中安全移除
    client_.setConnectionCallback(std::bind(&AsyncDeleteTask::onConnection,
                                            shared_from_this(),
                                            std::placeholders::_1));

    client_.setMessageCallback(std::bind(
        &AsyncDeleteTask::onMessage, shared_from_this(), std::placeholders::_1,
        std::placeholders::_2, std::placeholders::_3));

    LOG_INFO << "Start async physial delete task, FileID: " << fileId_;
    client_.connect();

    // 如果5秒内DataNode没连上，强制结束任务
    timeoutTimerId_ = client_.getLoop()->runAfter(
        5.0, std::bind(&AsyncDeleteTask::onTimeout, shared_from_this()));
}

void AsyncDeleteTask::onTimeout() {
    LOG_WARN << "Async physical delete task timeout, FileID: " << fileId_;
    client_.disconnect();
}

void AsyncDeleteTask::onConnection(const fn::TcpConnectionPtr &conn) {
    if (conn->connected()) {
        LOG_DEBUG << "Connected to datanode send delete request, FileID: "
                  << fileId_;
        std::string request = "POST /api/datanode/delete HTTP/1.1\r\n"
                              "Host: " +
                              conn->peerAddress().toIp() +
                              "\r\n"
                              "Authorization: Bearer " +
                              deleteToken_ +
                              "\r\n"
                              "Content-Lentth: 0\r\n"
                              "Connection: close\r\n\r\n";
        conn->send(request);
    } else {
        // 连接断开
        LOG_DEBUG << "Connection to datanode disconnect, FileID: " << fileId_;
        client_.getLoop()->cancel(timeoutTimerId_);
    }
}

void AsyncDeleteTask::onMessage(const fn::TcpConnectionPtr &conn,
                                fn::Buffer *buf, fileserver::Timestamp time) {
    client_.getLoop()->cancel(timeoutTimerId_);
    std::string response = buf->retrieveAllAsString();

    if (response.find("HTTP/1.1 200") != std::string::npos) {
        LOG_INFO << "DataNode physial delete success, ready to clean database, "
                    "FileID: "
                 << fileId_;

        // 执行数据库清理
        auto mysql = db::MySQLPool::instance().getConnection();
        if (mysql) {
            std::string delShareSql =
                "DELETE FROM file_shares WHERE file_id = ?";
            db::MySQLStatement delShareStmt(*mysql, delShareSql);
            delShareStmt.bindInt(fileId_);
            if (!delShareStmt.execute()) {
                LOG_ERROR << "Sharefiles database delete error: "
                          << delShareStmt.getError();
            }

            std::string delFileSql = "DELETE FROM files WHERE id = ?";
            db::MySQLStatement delFileStmt(*mysql, delFileSql);
            delFileStmt.bindInt(fileId_);
            if (delFileStmt.execute()) {
                LOG_INFO << "Database delete complete, FileID: " << fileId_;
            } else {
                LOG_ERROR << "Files database delete error: "
                          << delFileStmt.getError();
            }
        } else {
            LOG_ERROR << "Get database connection failed, FileID: " << fileId_;
        }
    } else {
        // 如果 DataNode 返回 404 (文件本来就不存在)，其实也可以认为是删除成功了
        if (response.find("HTTP/1.1 404") != std::string::npos) {
            LOG_WARN << "DataNode report file not exists, FileID: " << fileId_;
            // 执行数据库清理
            auto mysql = db::MySQLPool::instance().getConnection();
            if (mysql) {
                std::string delShareSql =
                    "DELETE FROM file_shares WHERE file_id = ?";
                db::MySQLStatement delShareStmt(*mysql, delShareSql);
                delShareStmt.bindInt(fileId_);
                if (!delShareStmt.execute()) {
                    LOG_ERROR << "Sharefiles database delete error: "
                              << delShareStmt.getError();
                }

                std::string delFileSql = "DELETE FROM files WHERE id = ?";
                db::MySQLStatement delFileStmt(*mysql, delFileSql);
                delFileStmt.bindInt(fileId_);
                if (delFileStmt.execute()) {
                    LOG_INFO << "Database delete complete, FileID: " << fileId_;
                } else {
                    LOG_ERROR << "Files database delete error: "
                              << delFileStmt.getError();
                }
            } else {
                LOG_ERROR << "Get database connection failed, FileID: "
                          << fileId_;
            }
            // 这里你可以选择复用上面的数据库清理逻辑
        } else {
            LOG_ERROR << "DataNode physical delete failed, 响应: "
                      << response.substr(0, 100) << "...";
            // 失败了什么都不做，数据库里的 is_deleted 依然是 2，等待下一轮 GC
            // 定时器重试
        }
    }

    client_.disconnect();
}