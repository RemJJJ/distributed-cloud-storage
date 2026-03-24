#include "MySQLPool.h"
#include "Config.h"
#include "base/Logging.h"
#include "mysql.h"
#include "mysql_com.h"
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace db {
// ============ PoolConfig 实现 ============
PoolConfig PoolConfig::fromConfig() {
    PoolConfig config;

    auto &cfg = Config::instance();

    config.host = cfg.getString("mysql.host", "localhost");
    config.user = cfg.getString("mysql.user", "root");
    config.password = cfg.getString("mysql.password", "");
    config.database = cfg.getString("mysql.database", "");
    config.port = static_cast<uint16_t>(cfg.getInt("mysql.port", 3306));
    config.maxConnections =
        static_cast<size_t>(cfg.getInt("mysql.maxConnections", 10));
    config.minConnections =
        static_cast<size_t>(cfg.getInt("mysql.minConnections", 2));
    config.maxIdleTime =
        static_cast<size_t>(cfg.getInt("mysql.maxIdleTime", 300));
    config.connectionTimeout =
        static_cast<size_t>(cfg.getInt("mysql.connectionTimeout", 10));

    return config;
}

// ============ ConnectionGuard 实现 ============
MySQLPool::ConnectionGuard::ConnectionGuard(MySQLPool &pool,
                                            MySQLConnectionPtr conn)
    : pool_(pool), conn_(std::move(conn)) {}

MySQLPool::ConnectionGuard::~ConnectionGuard() {
    if (conn_) {
        if (isBad_) {
            // 通知池子销毁连接并减少计数
            pool_.destroyConnection(std::move(conn_));
        } else {
            pool_.returnConnection(std::move(conn_));
        }
    }
}

// 【修复】实现移动构造函数
MySQLPool::ConnectionGuard::ConnectionGuard(ConnectionGuard &&other) noexcept
    : pool_(other.pool_), conn_(std::move(other.conn_)), isBad_(other.isBad_) {
    other.isBad_ = true; // 防止原对象析构时归还
}

MySQLPool::ConnectionGuard &
MySQLPool::ConnectionGuard::operator=(ConnectionGuard &&other) noexcept {
    if (this != &other) {
        // 先处理当前持有的连接
        if (conn_) {
            if (isBad_) {
                pool_.destroyConnection(std::move(conn_));
            } else {
                pool_.returnConnection(std::move(conn_));
            }
        }
        // 转移所有权
        conn_ = std::move(other.conn_);
        isBad_ = other.isBad_;
        other.isBad_ = true;
    }
    return *this;
}

void MySQLPool::ConnectionGuard::markBad() { isBad_ = true; }

// ============ MySQLPool 实现 ============
MySQLPool &MySQLPool::instance() {
    static MySQLPool instance;
    return instance;
}

MySQLPool::~MySQLPool() { shutdown(); }

bool MySQLPool::initialize(const PoolConfig &config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (totalConnections_ > 0) {
        LOG_WARN << "MySQLPool already initialized";
        return true;
    }

    config_ = config;

    // 初始化MYSQL库
    mysql_library_init(0, nullptr, nullptr);

    // 创建最小连接数
    for (size_t i = 0; i < config_.minConnections; i++) {
        auto conn = createConnection();
        if (conn) {
            idleConnections_.push(std::move(conn));
            ++totalConnections_;
        } else {
            LOG_ERROR << "Failed to create initial connection " << i;
        }
    }

    if (idleConnections_.empty()) {
        LOG_ERROR << "Failed to initialize MySQLPool: no connections created";
        mysql_library_end();
        return false;
    }

    // 启动维护线程
    shutdown_ = false;
    maintenanceThread_ = std::thread(&MySQLPool::maintenanceThread, this);

    LOG_INFO << "MySQLPool initialized with " << idleConnections_.size()
             << " connections (max: " << config_.maxConnections << ")";

    return true;
}

bool MySQLPool::initialize() { return initialize(PoolConfig::fromConfig()); }

MySQLConnectionPtr MySQLPool::createConnection() {
    MYSQL *conn = mysql_init(nullptr);
    if (!conn) {
        LOG_ERROR << "mysql_init failed: out of memory";
        return nullptr;
    }

    // 设置连接选项 (必须在 connect 之前)
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &config_.connectionTimeout);
    mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    // 连接数据库
    if (!mysql_real_connect(conn, config_.host.c_str(), config_.user.c_str(),
                            config_.password.c_str(), config_.database.c_str(),
                            config_.port, nullptr, CLIENT_FOUND_ROWS)) {
        LOG_ERROR << "mysql_real_connect failed: " << mysql_error(conn);
        mysql_close(conn);
        return nullptr;
    }

    LOG_DEBUG << "MySQL connection created: " << config_.host << ":"
              << config_.port;

    return MySQLConnectionPtr(conn);
}

bool MySQLPool::pingConnection(MYSQL *conn) { return mysql_ping(conn) == 0; }

// 【修复】支持动态扩容的核心逻辑
std::unique_ptr<MySQLPool::ConnectionGuard> MySQLPool::getConnection() {
    std::unique_lock<std::mutex> lock(mutex_);

    // 等待逻辑：如果没关闭，且没空闲连接，且已达最大连接数 -> 等待
    while (!shutdown_ && idleConnections_.empty() &&
           totalConnections_ >= config_.maxConnections) {
        condition_.wait(lock);
    }

    if (shutdown_) {
        LOG_ERROR << "MySQLPool is shutting down";
        return nullptr;
    }

    MySQLConnectionPtr conn;

    // 1. 优先取空闲连接
    if (!idleConnections_.empty()) {
        conn = std::move(idleConnections_.front());
        idleConnections_.pop();
    }
    // 2. 没有空闲但还没到上限，创建新连接
    else if (totalConnections_ < config_.maxConnections) {
        conn = createConnection();
        if (conn) {
            totalConnections_++;
        } else {
            LOG_ERROR << "Failed to create new connection in getConnection";
            return nullptr;
        }
    }

    // 验证链接 (简单的 ping)
    if (conn && !pingConnection(conn.get())) {
        LOG_WARN << "Connection invalid, trying to replace";
        // 旧连接销毁，计数减1
        conn.reset();
        totalConnections_--;

        // 尝试创建新连接
        conn = createConnection();
        if (conn) {
            totalConnections_++;
        } else {
            LOG_ERROR << "Failed to create replacement connection";
            return nullptr;
        }
    }

    return std::make_unique<ConnectionGuard>(*this, std::move(conn));
}

void MySQLPool::returnConnection(MySQLConnectionPtr conn) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (shutdown_) {
        // 连接池已关闭, 直接销毁链接
        conn.reset();
        totalConnections_--;
        return;
    }

    // 验证连接
    if (pingConnection(conn.get())) {
        idleConnections_.push(std::move(conn));
        condition_.notify_one();
    } else {
        LOG_WARN << "Returning invalid connection, discarding";
        conn.reset();
        totalConnections_--;

        // 创建新连接补充 (保持最小连接数)
        if (totalConnections_ < config_.minConnections) {
            lock.unlock();
            auto newConn = createConnection();
            lock.lock();
            if (newConn) {
                idleConnections_.push(std::move(newConn));
                totalConnections_++;
            }
        }
    }
}

// 【新增】内部用于销毁坏连接的函数
void MySQLPool::destroyConnection(MySQLConnectionPtr conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    // conn 在这里会自动调用 deleter 关闭
    conn.reset();
    if (totalConnections_ > 0) {
        totalConnections_--;
    }
    LOG_DEBUG << "Connection destroyed (marked bad)";
}

size_t MySQLPool::getActiveConnections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalConnections_ - idleConnections_.size();
}

size_t MySQLPool::getTotalConnections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalConnections_;
}

size_t MySQLPool::getIdleConnections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return idleConnections_.size();
}

void MySQLPool::maintenanceThread() {
    LOG_INFO << "MySQLPool maintenance thread started";

    while (!shutdown_) {
        std::this_thread::sleep_for(
            std::chrono::seconds(config_.maxIdleTime / 2));

        std::lock_guard<std::mutex> lock(mutex_);

        // 简单的维护：清理队列头部失效的连接
        // 注意：这里没有实现基于时间的淘汰，只做了保活检查
        // 如果需要基于时间的淘汰，需要将 queue 改为 list 并存储时间戳
        while (idleConnections_.size() > config_.minConnections) {
            auto &conn = idleConnections_.front();
            if (!pingConnection(conn.get())) {
                conn.reset();
                totalConnections_--;
                idleConnections_.pop();
                LOG_DEBUG << "Removed invalid idle connection";
            } else {
                break; // 只检查头部，避免长时间持有锁
            }
        }
    }
    LOG_INFO << "MySQLPool maintenance thread stopped";
}

void MySQLPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
    }

    condition_.notify_all();

    // 等待维护线程
    if (maintenanceThread_.joinable()) {
        maintenanceThread_.join();
    }

    // 关闭所有连接
    std::lock_guard<std::mutex> lock(mutex_);
    while (!idleConnections_.empty()) {
        idleConnections_.pop(); // unique_ptr 自动释放
        totalConnections_--;
    }

    mysql_library_end();

    LOG_INFO << "MySQLPool shutdown complete";
}

} // namespace db