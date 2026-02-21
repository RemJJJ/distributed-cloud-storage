#pragma once

#include "../base/Thread.h"
#include "../base/noncopyable.h"
#include "EventLoop.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

namespace fileserver {
namespace net {
class EventLoop;

/// @brief 事件循环线程类
/// 用于创建一个新线程，并在该线程中运行事件循环
/// 主要用于IO线程池中
class EventLoopThread : noncopyable {
  public:
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    /// @brief 构造函数
    /// @param cb 线程初始化回调函数
    EventLoopThread(const ThreadInitCallback &cb = ThreadInitCallback(),
                    const std::string &name = std::string());

    /// @brief 析构函数
    /// 如果线程还在运行，会等待线程退出
    ~EventLoopThread();

    /// @brief 启动线程并返回EventLoop对象
    /// 创建新线程并等待EventLoop创建完成
    EventLoop *startLoop();

  private:
    /// @brief 线程函数
    void threadFunc();

    EventLoop *loop_;
    bool exiting_;                 // 是否退出
    Thread thread_;                // 线程对象
    std::mutex mutex_;             // 互斥锁
    std::condition_variable cond_; // 条件变量
    ThreadInitCallback callback_;  // 回调函数
};

} // namespace net
} // namespace fileserver