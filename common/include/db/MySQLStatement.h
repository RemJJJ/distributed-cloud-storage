#pragma once

#include "MySQLPool.h"
#include <cstdint>
#include <list>
#include <memory>
#include <mysql/mysql.h>
#include <string>
#include <vector>

namespace db {

// ============ 预处理语句封装 ============
class MySQLStatement {
  public:
    MySQLStatement(MySQLPool::ConnectionGuard &conn, const std::string &sql);
    ~MySQLStatement();

    MySQLStatement(const MySQLStatement &) = delete;
    MySQLStatement &operator=(const MySQLStatement &) = delete;

    // 绑定参数（链式调用）
    MySQLStatement &bindInt(int value);
    MySQLStatement &bindInt64(int64_t value);
    MySQLStatement &bindDouble(double value);
    MySQLStatement &bindString(const std::string &value);
    MySQLStatement &bindNull();

    // 执行
    bool execute();

    // 获取结果
    uint64_t affectedRows() const;
    uint64_t insertId() const;

    // 获取结果集
    class ResultSet {
      public:
        ResultSet(MYSQL_STMT *stmt);
        ~ResultSet();

        ResultSet(const ResultSet &) = delete;
        ResultSet &operator=(const ResultSet &) = delete;

        bool next();
        size_t columnCount() const;
        std::string getFieldName(size_t index) const;

        bool isNull(size_t index) const;
        int getInt(size_t index, int defaultValue = 0) const;
        int64_t getInt64(size_t index, int64_t defaultValue = 0) const;
        double getDouble(size_t index, double defaultValue = 0.0) const;
        std::string getString(size_t index,
                              const std::string &defaultValue = "") const;

      private:
        void fetchRow();
        void bindResults();

        MYSQL_STMT *stmt_;
        std::vector<MYSQL_BIND> bindBuffer_;
        std::vector<std::vector<char>> stringBuffers_;
        std::vector<char> isNull_;
        std::vector<unsigned long> lengths_;
        size_t columnCount_ = 0;
        bool hasMoreRows_ = false;
    };

    std::unique_ptr<ResultSet> getResultSet();

    // 错误信息
    bool hasError() const { return hasError_; }
    const std::string &getError() const { return error_; }

  private:
    void prepare(const std::string &sql);
    void bindParameters();

    MYSQL_STMT *stmt_;
    MySQLPool::ConnectionGuard &conn_;
    std::vector<MYSQL_BIND> paramBuffer_;

    // 【这里定义了！】用于存储参数数据的缓冲区
    std::list<std::string> stringParams_;
    std::list<int> intParams_;
    std::list<int64_t> int64Params_;
    std::list<double> doubleParams_;

    std::vector<char> paramIsNull_; // 注意：char 不是 bool
    std::vector<unsigned long> paramLengths_;
    size_t paramCount_ = 0;
    size_t bindIndex_ = 0;
    bool prepared_ = false;
    bool hasError_ = false;
    std::string error_;
};

// 查询多行
class QueryResult {
  public:
    using Row = std::vector<std::string>;
    using Rows = std::vector<Row>;

    Rows rows;
    std::vector<std::string> columns;
    bool success = false;
    std::string error;

    bool empty() const { return rows.empty(); }
    size_t size() const { return rows.size(); }
};

QueryResult query(MySQLPool::ConnectionGuard &conn, const std::string &sql);

} // namespace db