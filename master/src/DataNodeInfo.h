#pragma once

#include "base/Timestamp.h"
#include "net/InetAddress.h"
#include <atomic>
#include <cstdint>
#include <jwt-cpp/jwt.h>
#include <string>
#include <vector>
#include <uuid/uuid.h>

namespace fn = fileserver::net;

struct ActiveUserSessionInfo {
    std::string nodeId_;
    int userId_ = 0;
    std::string username_;
    std::string serviceLevel_ = "normal";
    std::string sceneTag_ = "general";
    std::string transferType_;
    std::string fileName_;
    uint64_t currentBps_ = 0;
    std::string startedAt_;
};

struct DataNodeInfo {
    fn::InetAddress addr_;
    std::string publicUrl_;               // 给前端访问用的公网地址
    std::string id_;                      // 节点唯一ID
    fileserver::Timestamp lastHeartbeat_; // 最后一次心跳时间
    bool isAlive_;                        // 是否存活
    const int kTimeoutSeconds = 10;       // 超时阈值（10秒）
    uint64_t diskTotalMb_ = 0;
    uint64_t diskFreeMb_ = 0;
    int activeUploads_ = 0;
    int activeDownloads_ = 0;
    int activeTransfers_ = 0;
    bool isManuallyDisabled_ = false;
    uint64_t nodeBandwidthLimitBps_ = 0;
    uint64_t uploadBps_ = 0;
    uint64_t downloadBps_ = 0;
    int connectedUsers_ = 0;
    std::vector<ActiveUserSessionInfo> activeUserSessions_;
    double currentScore_ = 0.0;

    DataNodeInfo(const fn::InetAddress &addr) : addr_(addr), isAlive_(true) {
        lastHeartbeat_ = fileserver::Timestamp::now();
        id_ = generateUUID();
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

    std::string generateUUID() {
        uuid_t uuid;
        uuid_generate(uuid);
        char str[37];
        uuid_unparse_lower(uuid, str);
        return std::string(str);
    }
};
