#include "Connector.h"

#include <errno.h>
#include <string.h>

#include "../base/Logging.h"
#include "Channel.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "SocketsOps.h"

namespace fileserver {
namespace net {
const int Connector::kMaxRetryDelayMs;

Connector::Connector(EventLoop *loop, const InetAddress &serverAddr)
    : loop_(loop), serverAddr_(serverAddr), connect_(false),
      state_(kDisconnected), retryDelatMs_(kInitRetryDelayMs) {
    LOG_DEBUG << "ctor[" << this << "]";
}

Connector::~Connector() {
    LOG_DEBUG << "dtor[" << this << "]";
    assert(!channel_);
}

void Connector::start() {
    connect_ = true;
    loop_->runInLoop(std::bind(&Connector::startInLoop, this));
}

// 在loop线程中启动连接
void Connector::startInLoop() {
    loop_->assertInLoopThread();
    assert(state_ == kDisconnected);
    if (connect_) {
        connect();
    } else {
        LOG_DEBUG << "do not connect";
    }
}

void Connector::stop() {
    connect_ = false;
    loop_->queueInLoop(std::bind(&Connector::stopInLoop, this));
}

void Connector::stopInLoop() {
    loop_->assertInLoopThread();
    if (state_ == kConnecting) {
        setState(kDisconnected);
        int sockfd = removeAndResetChannel();
        retry(sockfd);
    }
}

void Connector::restart() {
    loop_->assertInLoopThread();
    setState(kDisconnected);
    retryDelatMs_ = kInitRetryDelayMs;
    connect_ = true;
    startInLoop();
}

void Connector::connect() {
    int sockfd = sockets::createNonblockingOrDie(serverAddr_.family());
    int ret = sockets::connect(
        sockfd, reinterpret_cast<const sockaddr *>(serverAddr_.getSockAddr()));
    int savedErrno = (ret == 0) ? 0 : errno;
    switch (savedErrno) {
    case 0:
    case EINPROGRESS:       // 连接正在进行
    case EINTR:             // 被信号中断
    case EISCONN:           // 已经连接
        connecting(sockfd); // 处理连接
        break;

    case EAGAIN:        // 临时端口不足
    case EADDRINUSE:    // 地址已被使用
    case EADDRNOTAVAIL: // 地址不可用
    case ECONNREFUSED:  // 连接被拒绝
    case ENETUNREACH:   // 网络不可达
        retry(sockfd);  // 重试
        break;

    case EACCES:       // 权限不足
    case EPERM:        // 操作不允许
    case EAFNOSUPPORT: // 地址族不支持
    case EALREADY:     // 连接已经在进行
    case EBADF:        // 无效的文件描述符
    case EFAULT:       // 地址无效
    case ENOTSOCK:     // 不是socket
        LOG_SYSERR << "connect error in Connector::connect";
        sockets::close(sockfd); // 关闭socket
        break;

    default:
        LOG_SYSERR << "Unexpected error in Connector::connect " << savedErrno;
        sockets::close(sockfd);
        break;
    }
}

void Connector::connecting(int sockfd) {
    setState(kConnecting);
    assert(!channel_);
    // 创建Channel观察socket上的事件
    channel_.reset(new Channel(loop_, sockfd));
    // 设置可写事件回调
    channel_->setWriteCallback(std::bind(&Connector::handleWrite, this));
    // 设置错误事件回调
    channel_->setErrorCallback(std::bind(&Connector::handleError, this));

    // 关注可写事件
    // 连接建立成功时socket可写
    channel_->enableWriting();
}

int Connector::removeAndResetChannel() {
    channel_->disableAll();
    channel_->remove();
    int sockfd = channel_->fd();
    // 不能再handleWrite/handleError等会调函数中删除channel
    // 因为正在调用channel的回调函数
    loop_->queueInLoop(std::bind(&Connector::resetChannel, this));
    return sockfd;
}

void Connector::resetChannel() { channel_.reset(); }

void Connector::handleWrite() {
    LOG_TRACE << "Connector::handleWrite " << state_;

    if (state_ == kConnecting) {
        int sockfd = removeAndResetChannel();
        int err = sockets::getSocketError(sockfd);
        if (err) {
            LOG_WARN << "Connector::handleWrite - SO_ERROR = " << err << " "
                     << strerror_tl(err);
            retry(sockfd);                           // 重试
        } else if (sockets::isSelfConnect(sockfd)) { // 自连接
            LOG_WARN << "Connector::handleWrite - Self connect";
            retry(sockfd);
        } else {
            setState(KConnected);
            if (connect_) {
                newConnectionCallback_(sockfd); // 调用连接成功的回调函数
            } else {
                sockets::close(sockfd); // 关闭socket
            }
        }
    } else {
        assert(state_ == kDisconnected);
    }
}

void Connector::handleError() {
    LOG_ERROR << "Connector::handleError state=" << state_;
    if (state_ == kConnecting) {
        int sockfd = removeAndResetChannel();
        int err = sockets::getSocketError(sockfd);
        LOG_TRACE << "SO_ERROR = " << err << " " << strerror_tl(err);
        retry(sockfd);
    }
}

void Connector::retry(int sockfd) {
    sockets::close(sockfd);
    setState(kDisconnected);
    if (connect_) {
        LOG_INFO << "cONNECTOR::retry - Retry connecting to "
                 << serverAddr_.toIpPort() << " in " << retryDelatMs_
                 << " milliseconds.";
        // 创建定时器，延迟重试
        timerId_ = loop_->runAfter(
            retryDelatMs_ / 1000.0,
            std::bind(&Connector::startInLoop, shared_from_this()));
        // 增加重试延迟
        retryDelatMs_ = std::min(retryDelatMs_ * 2, kMaxRetryDelayMs);
    } else {
        LOG_DEBUG << "do not connect";
    }
}

} // namespace net
} // namespace fileserver