#pragma once
#include "Config.h"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <mysql/mysql.h>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace db {

// ============ MySQL 连接智能指针 ============
struct MySQLDeleter {
    void operator()(MYSQL *conn) const {
        if (conn) {
            mysql_close(conn);
        }
    }
};

using MySQLConnectionPtr = std::unique_ptr<MYSQL, MySQLDeleter>;

// ============ 连接池配置 ============
struct PoolConfig {
    std::string host;
    std::string user;
    std::string password;
    std::string database;
    uint16_t port;
    size_t maxConnections;
    size_t minConnections;
    size_t maxIdleTime;       // 秒
    size_t connectionTimeout; // 秒

    static PoolConfig fromConfig(); // 从Config类加载
};

// ============ MySQL 连接池 ============
class MySQLPool {
  public:
    // 获取单例
    static MySQLPool &instance();

    bool initialize(const PoolConfig &config);
    bool initialize(); // 从Config自动加载

    // 获取连接(RAII)
    class ConnectionGuard {
      public:
        ConnectionGuard(MySQLPool &pool, MySQLConnectionPtr conn);
        ~ConnectionGuard();

        // 禁止拷贝
        ConnectionGuard(const ConnectionGuard &) = delete;
        ConnectionGuard &operator=(const ConnectionGuard &) = delete;

        // 允许移动
        ConnectionGuard(ConnectionGuard &&other) noexcept;
        ConnectionGuard &operator=(ConnectionGuard &&other) noexcept;

        MYSQL *get() const { return conn_.get(); }
        operator MYSQL *() const { return conn_.get(); }

        // 标记连接为需要关闭(发生错误时)
        void markBad();
        bool isBad() const { return isBad_; }

      private:
        MySQLPool &pool_;
        MySQLConnectionPtr conn_;
        bool isBad_ = false;
    };

    std::unique_ptr<ConnectionGuard> getConnection();

    // 统计信息
    size_t getTotalConnections() const;
    size_t getActiveConnections() const;
    size_t getIdleConnections() const;

    // 关闭连接池
    void shutdown();

  private:
    MySQLPool() = default;
    ~MySQLPool();

    // 禁止拷贝
    MySQLPool(const MySQLPool &) = delete;
    MySQLPool &operator=(const MySQLPool &) = delete;

    MySQLConnectionPtr createConnection();

    bool pingConnection(MYSQL *conn);

    // 回收连接到池
    void returnConnection(MySQLConnectionPtr conn);

    // 销毁连接并减少计数 (内部使用)
    void destroyConnection(MySQLConnectionPtr conn);

    // 连接池维护线程
    void maintenanceThread();

    // 成员变量
    PoolConfig config_;
    std::queue<MySQLConnectionPtr> idleConnections_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    size_t totalConnections_ = 0;
    bool shutdown_ = false;
    std::thread maintenanceThread_;
};
} // namespace db