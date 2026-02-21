#pragma once

#include "../base/Timestamp.h"
#include "Buffer.h"
#include <functional>
#include <memory>

namespace fileserver {
using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;

template <typename T> inline T *get_pointer(const std::shared_ptr<T> &ptr) {
    return ptr.get();
}

template <typename T> inline T *get_pointer(const std::unique_ptr<T> &ptr) {
    return ptr.get();
}

namespace net {
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

// 所有的客户端可见的回调函数
using TimerCallback = std::function<void()>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr &)>;
using CloseCallback = std::function<void(const TcpConnectionPtr &)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr &)>;
using HighWaterMarkCallback =
    std::function<void(const TcpConnectionPtr &, size_t)>;
using MessageCallback =
    std::function<void(const TcpConnectionPtr &, Buffer *, Timestamp)>;

} // namespace net
} // namespace fileserver