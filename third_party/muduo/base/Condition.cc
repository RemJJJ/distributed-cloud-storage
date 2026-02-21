#include "Condition.h"

#include <cstdint>
#include <ctime>
#include <errno.h>
#include <time.h>

namespace fileserver {
// 返回是否超时
bool Condition::waitForSeconds(double seconds) {
    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    const int64_t kNanoSecondsPerSecond = 1000000000;
    int64_t nanoseconds = static_cast<int64_t>(seconds * kNanoSecondsPerSecond);

    abstime.tv_sec += static_cast<time_t>((abstime.tv_nsec + nanoseconds) /
                                          kNanoSecondsPerSecond);
    abstime.tv_nsec = static_cast<long>((abstime.tv_nsec + nanoseconds) %
                                        kNanoSecondsPerSecond);

    // pthread_cond_timedwait 等待条件变量pcond_，超时时间为abstime
    return ETIMEDOUT ==
           pthread_cond_timedwait(&pcond_, mutex_.getPthreadMutex(), &abstime);
}
} // namespace fileserver
