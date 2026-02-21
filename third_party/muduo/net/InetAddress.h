#pragma once

#include "../base/StringPiece.h"
#include "../base/copyable.h"

#include <cstdint>
#include <netinet/in.h>
#include <string>

namespace fileserver {
namespace net {
namespace sockets {
const struct sockaddr *sockaddr_cast(const struct sockaddr_in6 *addr);
}

// 用于字符串参数的包装类
typedef const std::string &StringArg;

/// 用于ip地址的包装类
class InetAddress : public copyable {
  public:
    /// 构造一个基于给定端口的端点
    /// 大多是被用在TcpServer监听
    explicit InetAddress(uint16_t port = 0, bool loopbackOnly = false);

    /// 构造基于给定ip地址和端口的端点
    /// @c ip should be "1.2.3.4"
    InetAddress(StringArg ip, uint16_t port);
    InetAddress(StringArg ip, uint16_t prort, bool ipv6);
    /// 构造一个基于给定struct @c sockaddr_in的端点
    /// 大多数被用于接受新链接
    explicit InetAddress(const struct sockaddr_in &addr) : addr_(addr) {}

    InetAddress(const struct sockaddr_in6 &addr6) : addr6_(addr6) {}

    sa_family_t family() const { return addr_.sin_family; }
    std::string toIp() const;
    std::string toIpPort() const;
    uint16_t port() const;

    const struct sockaddr *getSockAddr() const {
        return reinterpret_cast<const struct sockaddr *>(&addr_);
    }

    void setSockAddr(const struct sockaddr_in &addr) { addr_ = addr; }
    const struct sockaddr *getSockAddrInet() const {
        return sockets::sockaddr_cast(&addr6_);
    }

    const struct sockaddr *getSockAddrInet6() const {
        return reinterpret_cast<const struct sockaddr *>(&addr6_);
    }
    void setSockAddrInet6(const struct sockaddr_in6 &addr6) { addr6_ = addr6; }
    uint32_t ipv4NetEndian() const;
    uint16_t portNetEndian() const { return addr_.sin_port; }

    // 解决域名转换IP地址问题，不改变端口或sin_family
    // 线程安全
    static bool resolve(StringArg hostname, InetAddress *result);

  private:
    union {
        struct sockaddr_in addr_;
        struct sockaddr_in6 addr6_;
    };
};

} // namespace net
} // namespace fileserver