#pragma once

#include "BlockingQueue.h"
#include "BoundedBlockingQueue.h"
#include "CountDownLatch.h"
#include "LogStream.h"
#include "Mutex.h"
#include "Thread.h"

#include <atomic>
#include <vector>

namespace fileserver {
/**
 * @brief 异步日志类
 *
 * 特点：
 * 1. 双缓冲设计
 * 2. 异步写入磁盘
 * 3. 支持多线程
 * 4. 自动滚动日志文件
 */
class AsyncLogging : noncopyable {
  public:
    /**
     * @brief 构造函数
     * @param basename 日志文件基本名
     * @param rollSize 滚动大小
     * @param flushInterval 刷新间隔，秒
     */
    AsyncLogging(const string &basename, off_t rollSize, int flushInterval = 3);
    ~AsyncLogging() {
        if (running_) {
            stop();
        }
    }

    /**
     * @brief 前端调用接口，写入日志
     */
    void append(const char *logiline, int line);

    /**
     * @brief 启动日志线程
     */
    void start() {
        running_ = true;
        thread_.start();
        latch_.wait();
    }

    /**
     * @brief 停止日志线程
     */
    void stop() {
        running_ = false;
        cond_.notify();
        thread_.join();
    }

  private:
    /// @brief 后端线程函数
    void threadFunc();
    typedef fileserver::detail::FixedBuffer<fileserver::detail::kLargeBuffer>
        Buffer;
    typedef std::vector<std::unique_ptr<Buffer>> BufferVector;
    typedef BufferVector::value_type BufferPtr;

    const int flushInterval_;          // 刷新间隔
    std::atomic<bool> running_;        // 运行标志
    const string basename_;            // 日志文件及本命
    const off_t rollSize_;             // 滚动大小
    fileserver::Thread thread_;        // 后端线程
    fileserver::CountDownLatch latch_; // 用于等待线程启动
    fileserver::MutexLock mutex_;      // 互斥锁
    fileserver::Condition cond_;       // 条件变量
    BufferPtr currentBuffer_;          // 当前缓冲区
    BufferPtr nextBuffer_;             // 下一个缓冲区
    BufferVector buffers_;             // 待写入文件的缓冲区列表
};
} // namespace fileserver