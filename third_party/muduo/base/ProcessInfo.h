#pragma once

#include "StringPiece.h"
#include "Timestamp.h"
#include "Types.h"
#include <sys/types.h>
#include <vector>

namespace fileserver {
namespace ProcessInfo {
pid_t pid();               // 获取进程pid
string pidString();        // 获取pid的字符串形式
uid_t uid();               // 当前用户id
string username();         // 当前用户名
uid_t euid();              // 有效用户id
Timestamp startTime();     // 进程启动时间
int clockTicksPerSecond(); // 系统时钟频率
int pageSize();            // 内存页大小
bool isDebugBuild();       // constexpr 编译器开关，判断你是否是debug版本

string hostname(); // 主机名
string procname(); // 进程名
StringPiece
procname(const string &stat); // 从/proc/[pid]/stat 的一行字符串里解析出进程名

/// read /proc/self/status,进程的详细状态
string procStatus();

/// read /proc/self/stat, 进程的cpu/调度相关信息
string procStat();

/// read /proc/self/task/tid/stat,某个线程的stat信息
string threadStat();

/// readlink /proc/self/exe, 读取符号链接，得到程序的真是路径
string exePath();

/// 当前进程打开的文件数
int opendFiles();

/// 系统允许的最大文件数
int maxOpenFiles();

struct CpuTime {
    double userSeconds;   // 用户态cpu时间
    double systemSeconds; // 内核态cpu时间

    CpuTime() : userSeconds(0.0), systemSeconds(0.0) {}

    double total() const { return userSeconds + systemSeconds; }
};
CpuTime cpuTime();

int numThreads();             // 当前进程线程数
std::vector<pid_t> threads(); // 当前进程的所有线程id

} // namespace ProcessInfo
} // namespace fileserver