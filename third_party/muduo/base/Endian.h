#pragma once

#include <endian.h>
#include <stdint.h>

namespace fileserver {

namespace sockets {
/// @brief 网络字节序（大端）和主机字节序转换函数
/// @brief 64位主机字节序转网络字节序
inline uint64_t hostToNetwork64(uint64_t host64) { return htobe64(host64); }

/// @brief 32位主机字节序转网络字节序
inline uint32_t hostToNetwork32(uint32_t host32) { return htobe32(host32); }

/// @brief 16位主机字节序转网络字节序
inline uint16_t hostToNetwork16(uint16_t host16) { return htobe16(host16); }

/// @brief 64为网络字节序转主机字节序
inline uint64_t networkToHost64(uint64_t net64) { return be64toh(net64); }

/// @brief 32为网络字节序转主机字节序
inline uint32_t networkToHost32(uint32_t net32) { return be32toh(net32); }

/// @brief 16为网络字节序转主机字节序
inline uint16_t networkToHost16(uint16_t net16) { return be16toh(net16); }
} // namespace sockets
} // namespace fileserver