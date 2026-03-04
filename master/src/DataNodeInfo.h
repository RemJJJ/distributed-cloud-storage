#pragma once

#include "../../third_party/muduo/base/Timestamp.h"
#include "../../third_party/muduo/net/InetAddress.h"

#include <atomic>
#include <string>

namespace fn = fileserver::net;

struct DataNodeInfo {
    fn::InetAddress addr_;
    fileserver::Timestamp lastHeartbeat_; // 最后一次心跳时间
    bool isAlive_;                        // 是否存活
    const int kTimeoutSeconds = 10;       // 超时阈值（10秒）

    DataNodeInfo(const fn::InetAddress &addr) : addr_(addr), isAlive_(true) {
        lastHeartbeat_ = fileserver::Timestamp::now();
    }

    // 判断是否超时
    bool isTimeout() const {
        // 计算当前时间 - 最后心跳时间 > 10秒
        int64_t elapsed =
            (fileserver::Timestamp::now().microSecondsSinceEpoch() -
             lastHeartbeat_.microSecondsSinceEpoch()) /
            1000000; // 转成秒
        return elapsed > kTimeoutSeconds;
    }
};