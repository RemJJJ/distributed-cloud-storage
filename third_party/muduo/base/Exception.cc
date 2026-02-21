#include "Exception.h"

#include <execinfo.h> // for backtrace
#include <stdlib.h>   // for malloc/free

namespace fileserver {
Exception::Exception(const char *msg) : message_(msg) { fillStackTrace(); }

Exception::Exception(const std::string &msg) : message_(msg) {
    fillStackTrace();
}

void Exception::fillStackTrace() {
    const int len = 200;
    void *buffer[len];

    // backtrace把当前调用栈的函数地址写入buffer，返回实际深度nptrs
    int nptrs = ::backtrace(buffer, len);

    // backtrace_symbols把地址转换为可读的字符串
    char **strings = ::backtrace_symbols(buffer, nptrs);

    // 遍历结果拼接到stack_
    if (strings) {
        for (int i = 0; i < nptrs; i++) {
            // TODO：使用 abi::__cxa_demangle 解构函数名称
            stack_.append(strings[i]);
            stack_.push_back('\n');
        }
        free(strings);
    }
}

} // namespace fileserver
