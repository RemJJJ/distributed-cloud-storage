#pragma once

#include "base/ThreadPool.h"
#include "db/MySQLPool.h"
#include <cstdint>
#include <mutex>
#include <string>

class SceneProfileService {
  public:
    struct SceneStats {
        int totalFiles = 0;
        int learningFiles = 0;
        int devFiles = 0;
        double avgSize = 0.0;
        int64_t totalDownloads = 0;
    };

    static SceneProfileService &instance() {
        static SceneProfileService instance;
        return instance;
    }

    void ensureSchema();
    std::string calculateUserScene(int userId);
    void refreshUserSceneAsync(int userId, fileserver::ThreadPool *threadPool);
    std::string getUserSceneTag(db::MySQLPool::ConnectionGuard &mysql,
                                int userId);
    void incrementDownloadCount(db::MySQLPool::ConnectionGuard &mysql,
                                int fileId);

  private:
    SceneProfileService() = default;

    void ensureUserSceneTagColumn(db::MySQLPool::ConnectionGuard &mysql);
    void ensureFileDownloadCountColumn(db::MySQLPool::ConnectionGuard &mysql);

    // 更新用户画像
    void updateUserSceneTag(db::MySQLPool::ConnectionGuard &mysql, int userId,
                            const std::string &sceneTag);

    // 查询用户统计
    SceneStats queryUserStats(db::MySQLPool::ConnectionGuard &mysql,
                              int userId);

    std::once_flag ensureSchemaOnce_;
};
