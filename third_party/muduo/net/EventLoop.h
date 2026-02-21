#pragma once

#include "../base/CurrentThread.h"
#include "../base/Timestamp.h"
#include "../base/noncopyable.h"
#include "Callbacks.h"
#include "Channel.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace fileserver {
namespace net {
class Channel;
class Poller;
class TimerId;
class TimerQueue;

/// @brief 事件循环类
/// 每个线程只能有一个EventLoop对象
/// 负责IO和定时器事件的分发
class EventLoop : noncopyable {
  public:
    using Functor = std::function<void()>;
    EventLoop();
    ~EventLoop();

    /// @brief 开启事件循环
    void loop();

    /// @brief 退出事件循环
    void quit();

    /// @brief 获取poll返回时间
    Timestamp pollReturnTime() const { return pollReturnTime_; }

    /// @brief 在当前loop中执行回调
    void runInLoop(Functor cb);

    /// @brief 把回调放入队列，唤醒loop所在线程回调
    void queueInLoop(Functor cb);

    /// @brief 唤醒loop所在线程
    void wakeup();

    /// @brief 更新Channel
    void updateChannel(Channel *channel);

    /// @brief 移除Channel
    void removeChannel(Channel *channel);

    /// @brief 是否有Channel
    bool hasChannel(Channel *channel);

    /// @brief 断言在EventLoop线程中
    void assertInLoopThread() {
        if (!isInLoopThread()) {
            abortNotInLoopThread();
        }
    }

    /// @brief 是否在EventLoop线程中
    bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }

    /// @brief 获取当前线程的EventLoop对象
    static EventLoop *getEventLoopOfCurrentThread();

    // timers
    TimerId runAt(Timestamp time, TimerCallback cb);
    TimerId runAfter(double delay, TimerCallback cb);
    TimerId runEvery(double interval, TimerCallback cb);
    void cancel(TimerId timerId);

  private:
    void abortNotInLoopThread();
    void handleRead(); // 唤醒
    void doPendingFunctors();

    using ChannelList = std::vector<Channel *>;

    // 按照构造函数初始化顺序声明成员变量
    std::atomic<bool> looping_;
    std::atomic<bool> quit_;
    std::atomic<bool> eventHandling_;
    std::atomic<bool> callingPendingFunctors_;
    const pid_t threadId_;                   // 当前对象所属线程ID
    Timestamp pollReturnTime_;               // poll返回时间
    std::unique_ptr<Poller> poller_;         // IO多路复用器
    std::unique_ptr<TimerQueue> timerQueue_; // 定时器队列
    int wakeupFd_;                           // 唤醒loop所属线程
    std::unique_ptr<Channel> wakeupChannel_; // 处理wakeupFd_上的事件
    Channel *currentActiveChannel_;          // 当前正在处理的活动
    ChannelList activeChannels_;             // Poller返回的活动通道
    std::mutex mutex_;                       // 互斥锁，用于保护pendingFunctors_
    std::vector<Functor> pendingFunctors_;   // 待处理的回调函数
};
} // namespace net
} // namespace fileserver