#include "CurrentThread.h"

#include <stdio.h>
#include <sys/syscall.h> // 获取线程ID
#include <sys/time.h>    // struct timeval
#include <sys/types.h>   // 获取进程ID
#include <unistd.h>

namespace fileserver {
namespace CurrentThread {
// 初始化线程局部存储变量
__thread int t_cachedTid = 0;
__thread char t_tidString[32];
__thread int t_tidStringLength = 6;
__thread const char *t_threadName = "unknown";

/// @brief 获取线程的真实ID
/// @details 在Linux系统中，通过系统调用SYS_gettid获取
pid_t gettid() { return static_cast<pid_t>(::syscall(SYS_gettid)); }

void cacheTid() {
    if (t_cachedTid == 0) {
        // 获取真实的线程ID
        t_cachedTid = gettid();
        // 将线程ID转换为字符串形式
        t_tidStringLength =
            snprintf(t_tidString, sizeof(t_tidString), "%5d ", t_cachedTid);
    }
}

bool isMainThread() {
    // 主线程ID等于进程ID
    return tid() == ::getpid();
}

void sleepUsec(int64_t usec) {
    struct timeval tv = {0, 0};
    tv.tv_sec = static_cast<time_t>(usec / 1000000);
    tv.tv_usec = static_cast<time_t>(usec % 1000000);

    // 不用监听fd，仅做延时。用作高精度sleep
    ::select(0, NULL, NULL, NULL, &tv);
}

} // namespace CurrentThread
} // namespace fileserver
