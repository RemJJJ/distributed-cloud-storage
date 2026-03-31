#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

class TokenBucketRateLimiter {
  public:
    TokenBucketRateLimiter(uint64_t rateLimitBps, uint64_t capacityBytes)
        : rateLimitBps_(rateLimitBps),
          capacityBytes_(std::max<uint64_t>(capacityBytes, 1)),
          tokens_(static_cast<double>(std::max<uint64_t>(capacityBytes, 1))),
          lastRefillTime_(std::chrono::steady_clock::now()) {}

    // 消耗指定字节数令牌
    void consume(size_t bytes) {
        if (rateLimitBps_ == 0 || bytes == 0) {
            return;
        }

        uint64_t remaining = static_cast<uint64_t>(bytes);
        while (remaining > 0) {
            refill(); // 尝试补充令牌

            uint64_t available = static_cast<uint64_t>(tokens_); // 当前可用令牌
            if (available == 0) {
                sleepForBytes(1);
                continue;
            }

            uint64_t spend = std::min<uint64_t>(
                remaining, std::min<uint64_t>(available, capacityBytes_));
            tokens_ -= static_cast<double>(spend);
            remaining -= spend;

            if (remaining > 0) {
                sleepForBytes(std::min<uint64_t>(remaining, capacityBytes_));
            }
        }
    }

  private:
    // 根据时间差计算并补充生成的令牌
    void refill() {
        auto now = std::chrono::steady_clock::now();
        auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                             now - lastRefillTime_)
                             .count();
        if (elapsedUs <= 0) {
            return;
        }

        // 计算这段时间生成的令牌数
        double generated =
            static_cast<double>(rateLimitBps_) * elapsedUs / 1000000.0;
        tokens_ = std::min<double>(static_cast<double>(capacityBytes_),
                                   tokens_ + generated);
        lastRefillTime_ = now;
    }

    // 当令牌不足时，计算需要等待多久并主动让出CPU;
    void sleepForBytes(uint64_t requiredBytes) {
        refill();
        if (tokens_ >= static_cast<double>(requiredBytes)) {
            return;
        }

        double missingBytes = static_cast<double>(requiredBytes) - tokens_;
        auto sleepUs = static_cast<int64_t>((missingBytes * 1000000.0) /
                                            static_cast<double>(rateLimitBps_));
        if (sleepUs <= 0) {
            sleepUs = 1000; // 最小睡1000微秒, 避免频繁唤醒
        }
        std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
    }

    uint64_t rateLimitBps_;  // 速率限制: 每秒允许通过的字节数
    uint64_t capacityBytes_; // 桶的最大容量: 最多缓存多少字节的突发流量
    double tokens_;          // 当前令牌数
    std::chrono::steady_clock::time_point lastRefillTime_; // 上次补充令牌的时间
};
