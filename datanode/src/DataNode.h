#pragma once
#include "base/Logging.h"
#include "net/HttpServer.h"
#include "net/InetAddress.h"
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class DataNodeHttpHandler;
class MasterClient;
namespace fn = fileserver::net;

class DataNode {
  public:
    struct TransferSessionSnapshot {
        int userId = 0;
        std::string username;
        std::string serviceLevel = "normal";
        std::string sceneTag = "general";
        std::string transferType;
        std::string fileName;
        uint64_t currentBps = 0;
        std::string startedAt;
    };

    struct AdminPolicyCache {
        std::string qosMode = "elastic";
        bool manualOverride = false;
        uint64_t globalBandwidthLimitBps = 0;
        uint64_t nodeBandwidthLimitBps = 0;
    };

    DataNode(fn::EventLoop *loop, const fn::InetAddress &listenAddr,
             const fn::InetAddress &masterAddr);

    ~DataNode();

    /// @brief 启动所有服务
    void start();

    //-----------给子组件提供接口------------
    /// @brief 获取MasterClient
    MasterClient *getMasterClient() { return masterClient_.get(); }

    // 原子计数器，记录当前正在处理的上传请求数
    std::atomic<int> activeUploads_{0};
    std::atomic<int> activeDownloads_{0};

    void incActiveUpload() { activeUploads_++; }
    void decActiveUpload() {
        int current = activeUploads_.load();
        while (current > 0 &&
               !activeUploads_.compare_exchange_weak(current, current - 1)) {
        }
    }
    int getActiveUploads() const { return activeUploads_.load(); }

    void incActiveDownload() { activeDownloads_++; }
    void decActiveDownload() {
        int current = activeDownloads_.load();
        while (current > 0 &&
               !activeDownloads_.compare_exchange_weak(current, current - 1)) {
        }
    }
    int getActiveDownloads() const { return activeDownloads_.load(); }
    int getActiveTransfers() const {
        return activeUploads_.load() + activeDownloads_.load();
    }

    void registerTransferSession(const std::string &sessionId, int userId,
                                 const std::string &username,
                                 const std::string &serviceLevel,
                                 const std::string &sceneTag,
                                 const std::string &transferType,
                                 const std::string &fileName,
                                 const std::string &startedAt);
    void recordTransferBytes(const std::string &sessionId, uint64_t bytes);
    void unregisterTransferSession(const std::string &sessionId);
    std::vector<TransferSessionSnapshot> getTransferSessionSnapshots() const;
    uint64_t getCurrentUploadBps() const;
    uint64_t getCurrentDownloadBps() const;
    int getConnectedUsers() const;

    void updateAdminPolicy(const AdminPolicyCache &policy);
    AdminPolicyCache getAdminPolicy() const;
    uint64_t getGlobalBandwidthLimitBps() const;
    uint64_t getNodeBandwidthLimitBps() const;
    uint64_t getEffectiveBandwidthLimitBps() const;
    uint64_t getWeightedRateLimitBps(const std::string &serviceLevel,
                                     const std::string &qosMode) const;

  private:
    struct TransferSessionRuntime {
        int userId = 0;
        std::string username;
        std::string serviceLevel = "normal";
        std::string sceneTag = "general";
        std::string transferType;
        std::string fileName;
        std::string startedAt;
        uint64_t currentBps = 0;
        uint64_t windowBytes = 0;
        int64_t lastRateUpdateUs = 0;
    };

    std::vector<TransferSessionSnapshot> collectTransferSessionsLocked() const;
    uint64_t collectTransferBpsLocked(const std::string &transferType) const;

    fn::EventLoop *loop_;
    std::unique_ptr<MasterClient> masterClient_;     // 持有MasterClient
    std::unique_ptr<fn::HttpServer> datanodeServer_; // 持有DataNodeServer
    std::shared_ptr<DataNodeHttpHandler> handler_;   // 业务处理器
    mutable std::mutex sessionMutex_;
    std::unordered_map<std::string, TransferSessionRuntime> transferSessions_;
    mutable std::mutex adminPolicyMutex_;
    AdminPolicyCache adminPolicy_;
};
