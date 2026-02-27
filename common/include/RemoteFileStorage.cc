#include "RemoteFileStorage.h"
#include "../../third_party/muduo/base/Logging.h"
#include <arpa/inet.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

void RemoteFileStorage::onConnection(const fn::TcpConnectionPtr &conn) {
    if (conn->connected()) {
        LOG_INFO << "Connected to DataNode: " << ip_ << ":" << port_;
        conn_ = conn;
        std::string openCmd = "OPEN " + filename_ + "\r\n";
        conn_->send(openCmd);

        // 补发连接建立前缓存的数据
        if (sendBuffer_.readableBytes() > 0) {
            conn_->send(sendBuffer_.peek(), sendBuffer_.readableBytes());
            sendBuffer_.retrieveAll();
        }

        std::weak_ptr<RemoteFileStorage> weakThis = shared_from_this();
        conn_->setCloseCallback([weakThis](const fn::TcpConnectionPtr &conn) {
            LOG_DEBUG << "TcpConnection closed";
            auto self = weakThis.lock();
            if (!self)
                return;
            if (self->closeCallback_) {
                self->closeCallback_();
            }
        });

        // 触发连接成功回调
        if (connectionCallback_) {
            connectionCallback_();
        }
    } else {
        LOG_INFO << "Disconnected from DataNode: " << ip_ << ":" << port_;
    }
}

void RemoteFileStorage::setConnectionCallback(const std::function<void()> &cb) {
    connectionCallback_ = cb;
}

void RemoteFileStorage::setCloseCallback(const std::function<void()> &cb) {
    closeCallback_ = cb;
}

RemoteFileStorage::RemoteFileStorage(const std::string &ip, int port,
                                     fn::EventLoop *loop)
    : ip_(ip), port_(port),
      client_(std::make_unique<fn::TcpClient>(
          loop, fn::InetAddress(ip_, port_), "RemoteFileStorage - TcpClient")) {
    client_->setConnectionCallback(std::bind(&RemoteFileStorage::onConnection,
                                             this, std::placeholders::_1));
}

RemoteFileStorage::~RemoteFileStorage() {}

bool RemoteFileStorage::isConnected() const {
    return conn_ != nullptr && conn_->connected();
}

bool RemoteFileStorage::open(const std::string &filename) {
    if (filename.empty()) {
        LOG_ERROR << "Filename is empty";
        return false;
    }
    filename_ = filename;
    LOG_DEBUG << filename_;
    client_->connect();
    return true;
}

bool RemoteFileStorage::write(const char *data, size_t len) {
    // 协议：
    // OPEN filename\n
    // DATA len\n
    // (binary)
    // CLOSE\n
    if (len == 0 || data == nullptr) {
        LOG_WARN << "Write empty data";
        return true;
    }
    std::string header = "DATA " + std::to_string(len) + "\r\n";

    fn::Buffer buf;
    buf.append(header);
    buf.append(data, len);
    if (!conn_) {
        sendBuffer_.append(header);
        sendBuffer_.append(data, len);
        LOG_DEBUG << "Cache data to sendBuffer, size: "
                  << sendBuffer_.readableBytes();
    } else {
        // 合并，防止粘包
        conn_->send(buf.peek(), buf.readableBytes());
        buf.retrieveAll();
        LOG_DEBUG << "Send data to DataNode, len: " << len;
    }
    totalBytes_ += len;
    return true;
}

bool RemoteFileStorage::close() {
    std::string closeCmd = "CLOSE\r\n";

    // 已连接，发送CLOSE
    if (conn_ && conn_->connected()) {
        auto self = shared_from_this();
        // 确保写完后再shutdown
        conn_->setWriteCompleteCallback(
            [self](const fn::TcpConnectionPtr &conn) {
                LOG_INFO << "CLOSE cmd sent, shutdown connection to "
                         << self->ip_ << ":" << self->port_;
                conn->shutdown(); // 发送完成后再关闭
                // 清空写完成回调，避免重复触发
                conn->setWriteCompleteCallback(nullptr);

                // 新增：触发业务用的CLOSE发送完成回调
                if (self->closeCmdSentCallback_) {
                    self->closeCmdSentCallback_(true);
                }
            });
        conn_->send(closeCmd);
        LOG_INFO << "Send CLOSE cmd and shutdown connection to " << ip_ << ":"
                 << port_;
    } else if (sendBuffer_.readableBytes() > 0) { // 未连接但有缓存，清空缓存
        sendBuffer_.retrieveAll();
        LOG_WARN << "Connection not established, clear sendBuffer";
        // 新增：触发回调，标记失败
        if (closeCmdSentCallback_) {
            closeCmdSentCallback_(false);
        }
    } else {
        if (closeCmdSentCallback_) {
            closeCmdSentCallback_(false);
        }
    }

    // 重置状态
    filename_.clear();
    return true;
}

uintmax_t RemoteFileStorage::totalBytes() const { return totalBytes_; }

bool RemoteFileStorage::isOpen() const {
    return !filename_.empty() && conn_ != nullptr && conn_->connected();
}

const std::string &RemoteFileStorage::filename() const { return filename_; }