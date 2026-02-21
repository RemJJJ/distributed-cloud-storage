#include "Acceptor.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "../base/Logging.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "SocketsOps.h"

using namespace fileserver;
using namespace fileserver::net;

Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr,
                   bool reuseport)
    : loop_(loop), acceptSocket_(sockets::createNonblockingOrDie(
                       listenAddr.family())), // 创建非阻塞socket
      acceptChannel_(loop, acceptSocket_.fd()), listening_(false),
      idleFd_(::open(
          "/dev/null",
          O_RDONLY | O_CLOEXEC)) // 打开空闲文件描述符，用于防止文件描述符耗尽
{
    assert(idleFd_ >= 0);

    // 设置socket选项
    acceptSocket_.setReuseAddr(true); // 设置地址重用，防止服务器重启时bind失败
    acceptSocket_.setReusePort(
        reuseport); // 设置端口重用，支持多进程监听同一端口
    acceptSocket_.bindAddress(listenAddr); // 绑定地址

    // 当监听socket可读时，调用handleRead
    acceptChannel_.setReadCallback(std::bind(&Acceptor::handleRead, this));
}

Acceptor::~Acceptor() {
    acceptChannel_.disableAll(); // 禁用所有事件
    acceptChannel_.remove();     // 从事件循环中移除
    ::close(idleFd_);            // 关闭空闲文件描述符
}

void Acceptor::listen() {
    loop_->assertInLoopThread(); // 确保在IO线程中调用
    listening_ = true;
    acceptSocket_.listen();         // 监听socket
    acceptChannel_.enableReading(); // 启用读事件，开始接受连接
}

void Acceptor::handleRead() {
    loop_->assertInLoopThread();
    InetAddress peerAddr;                         // 对端地址
    int connfd = acceptSocket_.accept(&peerAddr); // 接受新链接
    if (connfd >= 0) {
        if (newConnectionCallback_) { // 如果设置了新连接回调函数
            newConnectionCallback_(connfd, peerAddr); // 调用回调函数处理新连接
        } else {
            sockets::close(connfd); // 如果没有设置回调函数，直接关闭
        }
    } else {
        LOG_SYSERR << "in Acceptor::handleRead";
        if (errno == EMFILE) { // 文件描述符耗尽
            ::close(idleFd_);  // 关闭空闲描述符
            idleFd_ = ::accept(acceptSocket_.fd(), NULL, NULL);
            ::close(idleFd_);
            idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        }
    }
}