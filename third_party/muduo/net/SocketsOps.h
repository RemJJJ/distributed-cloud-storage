#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>

namespace fileserver {
namespace net {
namespace sockets {
/// @brief 创建一个非阻塞的socket文件描述符
int createNonblockingOrDie(sa_family_t family);

/// @brief 连接socket
/// @return 成功返回0， 失败返回-1
int connect(int sockfd, const struct sockaddr *addr);

/// @brief 绑定socket地址
void bindOrDie(int sockfd, const struct sockaddr *addr);

/// @brief 监听socket
void listenOrDie(int sockfd);

/// @brief 接受新链接
/// @return 成功返回非负的文件描述符，失败返回-1
int accept(int sockfd, struct sockaddr_in6 *addr);

/// @brief 从socket读取数据
ssize_t read(int sockfd, void *buf, size_t count);

/// @brief 从socket读取数据 (分散读)
ssize_t readv(int sockfd, const struct iovec *iov, int iovcnt);

/// @brief 写入数据到socket
ssize_t write(int sockfd, const void *buf, size_t count);

/// @brief 关闭socket
void close(int sockfd);

/// @brief 关闭socket的写入端 (TIME_WAIT)
void shutdownWrite(int sockfd);

/// @brief 将socket地址转换为IP:PORT格式的字符串
void toIpPort(char *buf, size_t size, const struct sockaddr *addr);

/// @brief 将socket地址转换为IP格式的字符串
void toIp(char *buf, size_t size, const struct sockaddr *addr);

/// @brief 将IP和端口转换为IPv4的socket地址
void fromIpPort(const char *ip, uint16_t port, struct sockaddr_in *addr);

/// @brief 将IP和端口转换为IPv6的socket地址
void fromIpPort(const char *ip, uint16_t port, struct sockaddr_in6 *addr);

/// @brief 获取socket错误
int getSocketError(int sockfd);

/// @brief socket地址转换函数
const struct sockaddr *sockaddr_cast(const struct sockaddr_in *addr);
const struct sockaddr *sockaddr_cast(const struct sockaddr_in6 *addr);
struct sockaddr *sockaddr_cast(struct sockaddr_in6 *addr);
const struct sockaddr_in *sockaddr_in_cast(const struct sockaddr *addr);
const struct sockaddr_in6 *sockaddr_in6_cast(const struct sockaddr *addr);

/// @brief 获取socket的本地地址
struct sockaddr_in6 getLocalAddr(int sockfd);

/// @brief 获取socket的对端地址
struct sockaddr_in6 getPeerAddr(int sockfd);

/// @brief 判断是否是自连接
bool isSelfConnect(int sockfd);
} // namespace sockets
} // namespace net
} // namespace fileserver