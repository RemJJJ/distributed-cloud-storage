#include "DataNodeServer.h"
#include "../../common/include/LocalFileStorage.h"
#include "../../third_party/muduo/base/Logging.h"
#include <sstream>

ConnectionContext::ConnectionContext(const std::string &filename,
                                     std::unique_ptr<FileStorage> storage)
    : filename_(filename), storage_(std::move(storage)),
      state_(State::WAIT_COMMAND), contentLength_(0) {}

ConnectionContext::ConnectionContext()
    : filename_(""), storage_(nullptr), state_(State::WAIT_COMMAND),
      contentLength_(0) {}

ConnectionContext::~ConnectionContext() {
    if (storage_ && storage_->isOpen()) {
        storage_->close();
        storage_.reset();
    }
}

bool ConnectionContext::hasMoreDataToProcess() const {
    // 如果是WAIT_DATA状态，且还有未读数据，返回true
    return (state_ == State::WAIT_DATA && contentLength_ > 0) ||
           (state_ == State::WAIT_COMMAND && contentLength_ == 0);
}

bool ConnectionContext::parseRequest(fn::Buffer *buf,
                                     fileserver::Timestamp receiveTime) {
    bool hasMore = true;
    // 记录本次DATA的总长度
    uintmax_t currentDataTotal = 0;
    while (hasMore) {
        if (state_ == State::WAIT_COMMAND) {
            const char *crlf = buf->findCRLF();
            if (!crlf) { // 不够一行，等待下一次
                hasMore = false;
                break;
            }
            // 取出一行
            size_t lineLen = crlf - buf->peek();
            std::string line = buf->retrieveAsString(lineLen);
            buf->retrieve(2); // 去掉\r\n
            LOG_INFO << "Received line: " << line;

            // 解析命令
            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;
            if (cmd == "OPEN") {
                std::string filename;
                iss >> filename;
                filename_ = filename;
                storage_ = std::make_unique<LocalFileStorage>();
                if (!storage_->open(filename_)) {
                    LOG_ERROR << "Open file failed: " << filename;
                    return false;
                }
                LOG_INFO << "OPEN " << filename;
            } else if (cmd == "DATA") {
                size_t len = 0;
                iss >> len;
                contentLength_ = len;
                currentDataTotal = len; // 记录本次DATA总长度
                state_ = State::WAIT_DATA;
            } else if (cmd == "CLOSE") {
                if (storage_ && storage_->isOpen()) {
                    storage_->close();
                    storage_.reset();
                    LOG_INFO << "CLOSE " << filename_;
                }
            }
        } else if (state_ == State::WAIT_DATA) {
            size_t readable = buf->readableBytes();
            if (readable == 0) {
                hasMore = false;
                break;
            }
            // 一次能写多少
            size_t writeLen = std::min(readable, contentLength_);
            if (storage_ && storage_->isOpen()) {
                writeData(buf->peek(), writeLen);
                buf->retrieve(writeLen);
                // 修复：递减剩余长度，判断是否写完
                contentLength_ -= writeLen;
                if (contentLength_ == 0) {
                    state_ = State::WAIT_COMMAND;
                    LOG_INFO
                        << "DATA write complete, total: " << currentDataTotal
                        << " bytes";
                }
            } else {
                LOG_ERROR << "DATA before OPEN";
                return false;
            }
        }
    }
    return true;
}

void ConnectionContext::writeData(const char *data, size_t len) {
    if (!storage_ || !storage_->isOpen()) {
        throw std::runtime_error("File storage is not initialized");
    }

    if (!storage_->write(data, len)) {
        throw std::runtime_error("Failed to write data to file: " + filename_);
    }
}

DataNodeServer::DataNodeServer(fn::EventLoop *loop,
                               const fileserver::net::InetAddress &listenAddr,
                               const std::string &name)
    : server_(loop, listenAddr, name) {
    server_.setThreadNum(4);
    server_.setConnectionCallback(
        std::bind(&DataNodeServer::onConnection, this, std::placeholders::_1));
    server_.setMessageCallback(
        std::bind(&DataNodeServer::onMessage, this, std::placeholders::_1,
                  std::placeholders::_2, std::placeholders::_3));
}

void DataNodeServer::start() { server_.start(); }

void DataNodeServer::onConnection(const fn::TcpConnectionPtr &conn) {
    if (conn->connected()) {
        auto context = std::make_shared<ConnectionContext>();
        conn->setContext(context);
        LOG_INFO << "New connection: " << conn->name();
    } else {
        LOG_INFO << "Connection closed: " << conn->name();
    }
}

void DataNodeServer::onMessage(const fn::TcpConnectionPtr &conn,
                               fn::Buffer *buf,
                               fileserver::Timestamp receiveTime) {
    auto context =
        std::static_pointer_cast<ConnectionContext>(conn->getContext());

    if (!context) {
        LOG_ERROR << "context is null";
        conn->shutdown();
        return;
    }

    // 循环处理缓冲区所有数据(解决粘包/半包问题)
    while (buf->readableBytes() > 0) {
        bool result = context->parseRequest(buf, receiveTime);
        LOG_INFO << "result = " << result;
        if (!result) {
            LOG_ERROR << "parseRequest failed";
            conn->shutdown();
            return;
        } else {
            LOG_INFO << "parseRequest success, remaining bytes in buf: "
                     << buf->readableBytes();
        }

        // 如果parseRequest处理后仍有数据，继续循环；否则退出
        if (!context->hasMoreDataToProcess()) {
            break;
        }
    }
    return;
}