#pragma once

#include "db/MySQLPool.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class StorageFeatureService {
  public:
    // 文件夹信息
    struct FolderInfo {
        int id = 0;
        int parentId = 0;
        std::string name;
        std::string fullPath;
        std::string createdAt;
    };

    // 存储摘要
    struct StorageSummary {
        std::string serviceLevel = "normal";
        uint64_t quotaBytes = 0;
        uint64_t usedBytes = 0;
        uint64_t remainingBytes = 0;
        bool isVip = false;
    };

    static StorageFeatureService &instance() {
        static StorageFeatureService instance;
        return instance;
    }

    // 数据库初始化
    void ensureSchema();

    // 查询用户等级
    std::string getUserServiceLevel(db::MySQLPool::ConnectionGuard &mysql,
                                    int userId);

    // 查询存储摘要
    StorageSummary getStorageSummary(db::MySQLPool::ConnectionGuard &mysql,
                                     int userId);

    // 检查是否可以上传文件(是否超额)
    bool canUploadFile(db::MySQLPool::ConnectionGuard &mysql, int userId,
                       uint64_t incomingBytes,
                       StorageSummary *summary = nullptr);

    // 保证路径存在(不存在创建)
    int ensureFolderPath(db::MySQLPool::ConnectionGuard &mysql, int userId,
                         int baseFolderId,
                         const std::vector<std::string> &segments);

    int ensureFolderPathFromString(db::MySQLPool::ConnectionGuard &mysql,
                                   int userId, int baseFolderId,
                                   const std::string &folderPath);

    // 列出某个目录下的子目录
    std::vector<FolderInfo> listFolders(db::MySQLPool::ConnectionGuard &mysql,
                                        int userId, int parentFolderId);

    // 查询单个目录的面包屑 /a/b/c 变成 a->b->c
    std::vector<FolderInfo>
    getFolderBreadcrumbs(db::MySQLPool::ConnectionGuard &mysql, int userId,
                         int folderId);

    // 查单个目录
    FolderInfo getFolderInfo(db::MySQLPool::ConnectionGuard &mysql, int userId,
                             int folderId);

    // 校验目录是否合法
    bool folderExists(db::MySQLPool::ConnectionGuard &mysql, int userId,
                      int folderId);

    uint64_t getQuotaBytesByServiceLevel(const std::string &serviceLevel) const;
    static std::string normalizeServiceLevel(const std::string &serviceLevel);

  private:
    StorageFeatureService() = default;

    void ensureFoldersTable(db::MySQLPool::ConnectionGuard &mysql);
    void ensureFilesFolderColumns(db::MySQLPool::ConnectionGuard &mysql);
    void ensureUsersServiceLevelColumn(db::MySQLPool::ConnectionGuard &mysql);

    // pareng + "/" + child
    std::string buildChildPath(const std::string &parentPath,
                               const std::string &folderName) const;

    // 对 full_path 计算稳定哈希，避免 utf8mb4 下超长唯一索引问题
    std::string computePathHash(const std::string &path) const;

    // 清洗目录名，去掉/ \，去掉首位空格，禁止"", ".", ".."
    std::string sanitizeFolderSegment(const std::string &folderName) const;

    // 统计已用空间
    uint64_t queryUserUsedBytes(db::MySQLPool::ConnectionGuard &mysql,
                                int userId);

    std::once_flag ensureSchemaOnce_;
};
