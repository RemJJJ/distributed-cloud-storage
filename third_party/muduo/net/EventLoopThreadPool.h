#pragma once

#include "../base/Types.h"
#include "../base/noncopyable.h"
#include "EventLoop.h"
#include "EventLoopThread.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fileserver {
namespace net {
/**
 * @brief 事件循环线程池类
 *
 * 该类管理一组EventLoopThread，用于多线程事件处理。
 * 通常用于TCP服务器，其中mainLoop接受连接，
 * 而IO线程池中的线程负责处理已建立连接的IO事件。
 *
 * 特点：
 * 1. 支持动态调整线程数量
 * 2. Round-robin方式分配连接
 * 3. 支持线程初始化回调
 * 4. 支持优雅关闭
 * 5. 线程安全的设计
 */
class EventLoopThreadPool : noncopyable {
  public:
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    /// @brief 构造函数
    /// @param baseLoop 主事件循环
    /// @param name 线程名称
    EventLoopThreadPool(EventLoop *baseLoop, const string &nameArg);

    /// @brief 析构函数
    /// 等待所有线程安全退出
    ~EventLoopThreadPool();

    /// @brief 设置线程数量
    void setThreadNum(int numThreads) { numThreads_ = numThreads; }

    /// @brief 启动线程池
    void start(const ThreadInitCallback &cb = ThreadInitCallback());

    /// @brief 优雅关闭线程池
    void stop();

    /// @brief 获取下一个事件循环
    /// 用于新链接的分发，采用round-robin方式分配
    EventLoop *getNextLoop();

    /// @brief 获取所有事件循环
    std::vector<EventLoop *> getAllLoops();

    /// @brief 是否已启动
    bool started() const { return started_; }

    /// @brief 获取线程名称
    const string &name() const { return name_; }

    /// @brief 获取线程数量
    int numThreads() const { return numThreads_; }

    /// @brief 获取活跃事件循环数量
    size_t size() const { return loops_.size(); }

  private:
    EventLoop *baseLoop_;            // 主事件循环，通常是Acceptor
    string name_;                    // 线程名称
    bool started_;                   // 是否启动
    int numThreads_;                 // 线程数量
    std::atomic<int> next_;          // 下一个要被分配的线程索引
    std::vector<EventLoop *> loops_; // EventLoop列表
    std::vector<std::unique_ptr<EventLoopThread>> threads_; // IO线程类表
};
} // namespace net
} // namespace fileserver