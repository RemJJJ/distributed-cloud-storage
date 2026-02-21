#include "Socket.h"
#include "../base/Logging.h"
#include "InetAddress.h"
#include "SocketsOps.h"

#include <asm-generic/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>

using namespace fileserver;
using namespace fileserver::net;

Socket::Socket(int sockfd) : sockfd_(sockfd) {
    // 设置为非阻塞模式
    int flags = ::fcntl(sockfd_, F_GETFL, 0);
    flags |= O_NONBLOCK;
    int ret = ::fcntl(sockfd_, F_SETFL, flags);
    if (ret < 0) {
        LOG_ERROR << "Socket::Socket set nonblocking failed";
    }
}

Socket::~Socket() { sockets::close(sockfd_); }

void Socket::bindAddress(const InetAddress &addr) {
    sockets::bindOrDie(sockfd_, addr.getSockAddrInet6());
}

void Socket::listen() { sockets::listenOrDie(sockfd_); }

int Socket::accept(InetAddress *peeraddr) {
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    int connfd = sockets::accept(sockfd_, &addr);
    if (connfd >= 0) {
        peeraddr->setSockAddrInet6(addr);
    }
    return connfd;
}

void Socket::shutdownWrite() { sockets::shutdownWrite(sockfd_); }

void Socket::setTcpNoDelay(bool on) {
    int optval = on ? 1 : 0;
    int ret = ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval,
                           sizeof(optval));
    if (ret < 0 && on) {
        LOG_ERROR << "Socket::setTcpNoDelay failed";
    }
}

void Socket::setReuseAddr(bool on) {
    int optval = on ? 1 : 0;
    int ret = ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval,
                           sizeof(optval));
    if (ret < 0 && on) {
        LOG_ERROR << "Socket::setReuseAddr failed";
    }
}

void Socket::setReusePort(bool on) {
#ifdef SO_REUSEPORT
    int optval = on ? 1 : 0;
    int ret = ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval,
                           sizeof(optval));
    if (ret < 0 && on) {
        LOG_ERROR << "Socket::setReusePort failed";
    }
#else
    if (on) {
        LOG_ERROR << "SO_REUSEPORT is not supported";
    }
#endif
}

void Socket::setKeepAlive(bool on) {
    int optval = on ? 1 : 0;
    int ret = setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval,
                         static_cast<socklen_t>(sizeof(optval)));
    if (ret < 0 && on) {
        LOG_ERROR << "Socket::setKeepAlive";
    }
}
