#pragma once

#include "Types.h"
#include "copyable.h"
#include <ctime>
#include <string>

namespace fileserver {
class Timestamp : public copyable {
  public:
    Timestamp() : microSecondsSinceEpoch_(0) {}
    /// @brief 从微秒时间戳创建Timestamp
    explicit Timestamp(int64_t microSecondsSinceEpoch)
        : microSecondsSinceEpoch_(microSecondsSinceEpoch) {}

    /// @brief 交换两个Timestamp
    void swap(Timestamp &t) {
        std::swap(microSecondsSinceEpoch_, t.microSecondsSinceEpoch_);
    }

    ~Timestamp() = default;
    std::string toString() const; // 将时间戳转换为字符串
    std::string toFormattedString(
        bool showMicroseconds = true) const; // 将时间戳转换为格式化字符串

    /// @brief 获取微秒时间戳
    int64_t microSecondsSinceEpoch() const { return microSecondsSinceEpoch_; }

    /// @brief 获取秒级时间戳
    time_t secondsSinceEpoch() const {
        return static_cast<time_t>(microSecondsSinceEpoch_ /
                                   kMicroSecondsPerSecond);
    }

    static const int kMicroSecondsPerSecond = 1000 * 1000;

    /// @brief 获取当前时间
    static Timestamp now();

    /// @brief 获取一个无效的时间戳
    static Timestamp invalid() { return Timestamp(); }

    /// @brief 从Unix时间戳创建Timestamp
    static Timestamp fromUnixTime(time_t t) { return fromUnixTime(t, 0); }

    /// @brief 从Unix时间戳和微妙创建Timestamp
    static Timestamp fromUnixTime(time_t t, int microseconds) {
        return Timestamp(static_cast<int64_t>(t) * kMicroSecondsPerSecond +
                         microseconds);
    }

  private:
    int64_t microSecondsSinceEpoch_;
};
inline bool operator<(Timestamp lhs, Timestamp rhs) {
    return lhs.microSecondsSinceEpoch() < rhs.microSecondsSinceEpoch();
}

inline bool operator>(Timestamp lhs, Timestamp rhs) {
    return lhs.microSecondsSinceEpoch() > rhs.microSecondsSinceEpoch();
}

inline bool operator==(Timestamp lhs, Timestamp rhs) {
    return lhs.microSecondsSinceEpoch() == rhs.microSecondsSinceEpoch();
}

/// @brief 计算时间差
/// @param lhs 时间戳1
/// @param rhs 时间戳2
/// @return 相差的秒数
inline double timeDifference(Timestamp lhs, Timestamp rhs) {
    auto diff = lhs.microSecondsSinceEpoch() - rhs.microSecondsSinceEpoch();
    return static_cast<double>(diff) / Timestamp::kMicroSecondsPerSecond;
}

/// @brief 给时间戳加上秒数
inline Timestamp addTime(Timestamp timestamp, double seconds) {
    return Timestamp(
        timestamp.microSecondsSinceEpoch() +
        static_cast<int64_t>(seconds * Timestamp::kMicroSecondsPerSecond));
}

} // namespace fileserver
