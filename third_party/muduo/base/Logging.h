#pragma once

#include "LogStream.h"
#include "Timestamp.h"

namespace fileserver {
class Logger {
  public:
    enum LogLevel {
        TRACE,
        DEBUG,
        INFO,
        WARN,
        ERROR,
        FATAL,
        NUM_LOG_LEVELS,
    };

    // 编译时计算原文件名
    class SourceFile {
      public:
        template <int N>
        SourceFile(const char (&arr)[N]) : data_(arr), size_(N - 1) {
            const char *slash = strrchr(data_, '/');
            if (slash) {
                data_ = slash + 1;
                size_ -= static_cast<int>(data_ - arr);
            }
        }

        explicit SourceFile(const char *filename) : data_(filename) {
            const char *slash = strrchr(filename, '/');
            if (slash) {
                data_ = slash + 1;
            }
            size_ = static_cast<int>(strlen(data_));
        }

        const char *data_;
        int size_;
    };

    Logger(SourceFile file, int line);
    Logger(SourceFile file, int line, LogLevel level);
    Logger(SourceFile file, int line, LogLevel LEVEL, const char *func);
    Logger(SourceFile file, int line, bool toAbort);
    ~Logger();

    LogStream &stream() { return impl_.stream_; }

    static LogLevel logLevel();
    static void setLogLevel(LogLevel level);

    typedef void (*OutputFunc)(const char *msg, int len);
    typedef void (*FlushFunc)();
    static void setOutput(OutputFunc);
    static void setFlush(FlushFunc);

  private:
    class Impl {
      public:
        typedef Logger::LogLevel LogLevel;
        Impl(LogLevel level, int old_errno, const SourceFile &file, int line);
        void formatTime();
        void finish();

        Timestamp time_;
        LogStream stream_;
        LogLevel level_;
        int line_;
        SourceFile basename_;
    };
    Impl impl_;
};

extern Logger::LogLevel g_logLevel;

#define LOG_TRACE                                                              \
    if (fileserver::Logger::logLevel() <= fileserver::Logger::TRACE)           \
    fileserver::Logger(__FILE__, __LINE__, fileserver::Logger::TRACE,          \
                       __func__)                                               \
        .stream()
#define LOG_DEBUG                                                              \
    if (fileserver::Logger::logLevel() <= fileserver::Logger::DEBUG)           \
    fileserver::Logger(__FILE__, __LINE__, fileserver::Logger::DEBUG,          \
                       __func__)                                               \
        .stream()
#define LOG_INFO                                                               \
    if (fileserver::Logger::logLevel() <= fileserver::Logger::INFO)            \
    fileserver::Logger(__FILE__, __LINE__).stream()
#define LOG_WARN                                                               \
    fileserver::Logger(__FILE__, __LINE__, fileserver::Logger::WARN).stream()
#define LOG_ERROR                                                              \
    fileserver::Logger(__FILE__, __LINE__, fileserver::Logger::ERROR).stream()
#define LOG_FATAL                                                              \
    fileserver::Logger(__FILE__, __LINE__, fileserver::Logger::FATAL).stream()
#define LOG_SYSERR fileserver::Logger(__FILE__, __LINE__, false).stream()
#define LOG_SYSFATAL fileserver::Logger(__FILE__, __LINE__, true).stream()

inline Logger::LogLevel Logger::logLevel() { return g_logLevel; }

const char *strerror_tl(int savedErrno);
} // namespace fileserver