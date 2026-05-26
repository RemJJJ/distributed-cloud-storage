#pragma once

#include "db/MySQLPool.h"
#include <mutex>
#include <string>

class SceneModeService {
  public:
    static SceneModeService &instance() {
        static SceneModeService instance;
        return instance;
    }

    void ensureSchema();
    std::string getUserSceneTag(db::MySQLPool::ConnectionGuard &mysql,
                                int userId);
    bool updateUserSceneTag(db::MySQLPool::ConnectionGuard &mysql, int userId,
                            const std::string &sceneTag);

  private:
    SceneModeService() = default;

    void ensureUserSceneTagColumn(db::MySQLPool::ConnectionGuard &mysql);
    std::once_flag ensureSchemaOnce_;
};
