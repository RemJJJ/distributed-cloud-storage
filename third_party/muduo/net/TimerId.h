#pragma once

#include "../base/Types.h"
#include "../base/copyable.h"

namespace fileserver {
namespace net {
class Timer;

/// @brief 一个不透明的标识符，用于取消定时器。
class TimerId : public copyable {
  public:
    TimerId() : timer_(nullptr), sequence_(0) {}

    TimerId(Timer *timer, int64_t seq) : timer_(timer), sequence_(seq) {}

    // 默认的拷贝构造函数、析构函数和赋值操作符是可以的

    friend class TimerQueue;

  private:
    Timer *timer_;     // 定时器指针
    int64_t sequence_; // 定时器序号
};

} // namespace net
} // namespace fileserver