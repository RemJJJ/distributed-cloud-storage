#pragma once

#include <cstdint>

namespace fileserver {
namespace CurrentThread {
// 内部使用的线程局部存储变量
extern __thread int t_cachedTid;          // 缓存的线程ID
extern __thread char t_tidString[32];     // 线程ID的字符串表示
extern __thread int t_tidStringLength;    // 线程ID字符串长度
extern __thread const char *t_threadName; // 线程名称

/// @brief 缓存线程ID
/// @details 通过系统调用获取线程ID并缓存，避免频繁使用
void cacheTid();

/// @brief 获取线程ID
/// @details 如果还未缓存，则调用cacheTid()进行缓存
/// @return 返回当前线程ID
inline int tid() {
    // __builtin_expect是GCC内置的分支优化函数
    // 第二个参数为0表示预测包表达式很可能为假
    if (__builtin_expect(t_cachedTid == 0, 0)) {
        cacheTid();
    }
    return t_cachedTid;
}

/// @brief 设置线程名称
/// @param name 线程名称
inline void setName(const char *name) { t_threadName = name; }

/// @brief 判断是否是主线程
/// @return 如果当前线程是主线程返回true
bool isMainThread();

/// @brief 获取线程ID的字符串表示
/// @return 返回线程ID的字符串
inline const char *tidString() { return t_tidString; }

/// @brief 获取线程ID字符串长度
/// @return 返回线程ID字符串长度
inline int tidStringLength() { return t_tidStringLength; }

/// @brief 获取线程名称
/// @return 返回当前线程的名称，如果未设置则返回"unknown"
inline const char *name() { return t_threadName; }

/// @brief 休眠制定的微秒数
/// @param usec 要休眠的微秒数
void sleepUsec(int64_t usec);

} // namespace CurrentThread
} // namespace fileserver