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

    DataNodeInfo(const fn::InetAddress &addr) : addr_(addr), isAlive_(true) {
        lastHeartbeat_ = fileserver::Timestamp::now();
    }
};