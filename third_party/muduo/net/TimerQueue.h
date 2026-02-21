#pragma once

#include <set>
#include <vector>

#include "../base/Mutex.h"
#include "../base/Timestamp.h"
#include "Callbacks.h"
#include "Channel.h"

namespace fileserver {
namespace net {
class EventLoop;
class Timer;
class TimerId;

/// @brief 一个尽力而为的定时器队列
/// 不保证回调函数按时进行
class TimerQueue : noncopyable {
  public:
    explicit TimerQueue(EventLoop *loop);
    ~TimerQueue();

    /// 在指定时间调度回调函数
    /// 如果 @c interval >0.0 则重复执行
    /// 必须是线程安全的，通常从其他线程调用。
    TimerId addTimer(TimerCallback cb, Timestamp when, double interval);

    void cancel(TimerId timerId);

  private:
    // 定时器条目,包含到期时间和定时器指针
    using Entry = std::pair<Timestamp, Timer *>;

    // 定时器集合，按到期时间排序
    using TimerList = std::set<Entry>;

    // 激活的定时器集合
    using ActiveTimer = std::pair<Timer *, int64_t>;
    using ActiveTimerSet = std::set<ActiveTimer>;

    void addTimerInLoop(Timer *timer);
    void cancelInLoop(TimerId timerId);

    // 当 timerfd 发出警报时被调用
    void handleRead();

    // 移出所有超时的定时器
    std::vector<Entry> getExpired(Timestamp now);
    void reset(const std::vector<Entry> &expired, Timestamp now);

    bool insert(Timer *timer);

    EventLoop *loop_;        // 所属的事件循环
    const int timerfd_;      // 定时器文件描述符
    Channel timerfdChannel_; // 定时器通道
    TimerList timers_;       // 定时器集合

    // 用于取消
    ActiveTimerSet activeTimers_;    // 按对象地址排序的定时器集合
    bool callingExpiredTimers_;      // 是否正在处理到期定时器
    ActiveTimerSet cancelingTimers_; // 保存被取消的定时器
};
} // namespace net
} // namespace fileserver