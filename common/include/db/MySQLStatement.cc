#include "MySQLStatement.h"
#include "base/Logging.h"
#include <cstring>

namespace db {

MySQLStatement::MySQLStatement(MySQLPool::ConnectionGuard &conn,
                               const std::string &sql)
    : stmt_(mysql_stmt_init(conn.get())), conn_(conn) {
    if (!stmt_) {
        hasError_ = true;
        error_ = "mysql_stmt_init failed";
        return;
    }
    prepare(sql);
}

MySQLStatement::~MySQLStatement() {
    if (stmt_)
        mysql_stmt_close(stmt_);
}

void MySQLStatement::prepare(const std::string &sql) {
    if (mysql_stmt_prepare(stmt_, sql.c_str(), sql.length()) != 0) {
        hasError_ = true;
        error_ = "mysql_stmt_prepare failed: " +
                 std::string(mysql_stmt_error(stmt_));
        return;
    }
    paramCount_ = mysql_stmt_param_count(stmt_);
    paramBuffer_.resize(paramCount_);
    paramIsNull_.resize(paramCount_, 0);
    paramLengths_.resize(paramCount_, 0);
    prepared_ = true;
}

void MySQLStatement::bindParameters() {
    if (mysql_stmt_bind_param(stmt_, paramBuffer_.data()) != 0) {
        hasError_ = true;
        error_ = "mysql_stmt_bind_param failed: " +
                 std::string(mysql_stmt_error(stmt_));
    }
}

MySQLStatement &MySQLStatement::bindInt(int value) {
    if (bindIndex_ >= paramCount_ || !prepared_)
        return *this;
    intParams_.push_back(value);

    auto &bind = paramBuffer_[bindIndex_];
    memset(&bind, 0, sizeof(bind));
    bind.buffer_type = MYSQL_TYPE_LONG;
    bind.buffer = &intParams_.back();
    bind.is_null = reinterpret_cast<bool *>(&paramIsNull_[bindIndex_]);
    bind.length = &paramLengths_[bindIndex_];
    paramLengths_[bindIndex_] = sizeof(int);

    bindIndex_++;
    return *this;
}

MySQLStatement &MySQLStatement::bindInt64(int64_t value) {
    if (bindIndex_ >= paramCount_ || !prepared_)
        return *this;
    int64Params_.push_back(value);

    auto &bind = paramBuffer_[bindIndex_];
    memset(&bind, 0, sizeof(bind));
    bind.buffer_type = MYSQL_TYPE_LONGLONG;
    bind.buffer = &int64Params_.back();
    bind.is_null = reinterpret_cast<bool *>(&paramIsNull_[bindIndex_]);
    bind.length = &paramLengths_[bindIndex_];
    paramLengths_[bindIndex_] = sizeof(int64_t);

    bindIndex_++;
    return *this;
}

MySQLStatement &MySQLStatement::bindDouble(double value) {
    if (bindIndex_ >= paramCount_ || !prepared_)
        return *this;
    doubleParams_.push_back(value);

    auto &bind = paramBuffer_[bindIndex_];
    memset(&bind, 0, sizeof(bind));
    bind.buffer_type = MYSQL_TYPE_DOUBLE;
    bind.buffer = &doubleParams_.back();
    bind.is_null = reinterpret_cast<bool *>(&paramIsNull_[bindIndex_]);
    bind.length = &paramLengths_[bindIndex_];
    paramLengths_[bindIndex_] = sizeof(double);

    bindIndex_++;
    return *this;
}

MySQLStatement &MySQLStatement::bindString(const std::string &value) {
    if (bindIndex_ >= paramCount_ || !prepared_)
        return *this;
    stringParams_.push_back(value);
    const std::string &str = stringParams_.back();

    auto &bind = paramBuffer_[bindIndex_];
    memset(&bind, 0, sizeof(bind));

    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char *>(str.c_str());
    bind.buffer_length = static_cast<unsigned long>(str.length() + 1);

    bind.is_null = reinterpret_cast<bool *>(&paramIsNull_[bindIndex_]);
    bind.length = &paramLengths_[bindIndex_];
    paramLengths_[bindIndex_] = static_cast<unsigned long>(str.length());

    bindIndex_++;
    return *this;
}

MySQLStatement &MySQLStatement::bindNull() {
    if (bindIndex_ >= paramCount_ || !prepared_)
        return *this;

    auto &bind = paramBuffer_[bindIndex_];
    memset(&bind, 0, sizeof(bind));
    bind.buffer_type = MYSQL_TYPE_NULL;
    bind.is_null = reinterpret_cast<bool *>(&paramIsNull_[bindIndex_]);
    paramIsNull_[bindIndex_] = 1; // 1 代表 true

    bindIndex_++;
    return *this;
}

bool MySQLStatement::execute() {
    if (!prepared_ || hasError_)
        return false;
    bindParameters();
    if (mysql_stmt_execute(stmt_) != 0) {
        hasError_ = true;
        error_ = "mysql_stmt_execute failed: " +
                 std::string(mysql_stmt_error(stmt_));
        return false;
    }
    return true;
}

uint64_t MySQLStatement::affectedRows() const {
    return mysql_stmt_affected_rows(stmt_);
}
uint64_t MySQLStatement::insertId() const {
    return mysql_stmt_insert_id(stmt_);
}
std::unique_ptr<MySQLStatement::ResultSet> MySQLStatement::getResultSet() {
    return std::make_unique<ResultSet>(stmt_);
}

// --- ResultSet ---
MySQLStatement::ResultSet::ResultSet(MYSQL_STMT *stmt) : stmt_(stmt) {
    if (mysql_stmt_store_result(stmt_) != 0)
        return;
    columnCount_ = mysql_stmt_field_count(stmt_);
    bindBuffer_.resize(columnCount_);
    stringBuffers_.resize(columnCount_);
    isNull_.resize(columnCount_, 0);
    lengths_.resize(columnCount_, 0);
    bindResults();
    // fetchRow();
}

MySQLStatement::ResultSet::~ResultSet() { mysql_stmt_free_result(stmt_); }

void MySQLStatement::ResultSet::bindResults() {
    MYSQL_RES *metadata = mysql_stmt_result_metadata(stmt_);
    if (!metadata)
        return;
    MYSQL_FIELD *fields = mysql_fetch_fields(metadata);

    for (size_t i = 0; i < columnCount_; ++i) {
        auto &bind = bindBuffer_[i];
        memset(&bind, 0, sizeof(bind));
        bind.buffer_type = MYSQL_TYPE_STRING;
        bind.is_null = reinterpret_cast<bool *>(&isNull_[i]);
        bind.length = &lengths_[i];

        stringBuffers_[i].resize(1024);
        bind.buffer = stringBuffers_[i].data();
        bind.buffer_length =
            static_cast<unsigned long>(stringBuffers_[i].size());
    }
    mysql_stmt_bind_result(stmt_, bindBuffer_.data());
    mysql_free_result(metadata);
}

void MySQLStatement::ResultSet::fetchRow() {
    int ret = mysql_stmt_fetch(stmt_);
    if (ret == 0 || ret == MYSQL_DATA_TRUNCATED) {
        hasMoreRows_ = true;
        // 处理截断
        for (size_t i = 0; i < columnCount_; ++i) {
            if (lengths_[i] > stringBuffers_[i].size()) {
                stringBuffers_[i].resize(lengths_[i] + 1);
                bindBuffer_[i].buffer = stringBuffers_[i].data();
                bindBuffer_[i].buffer_length =
                    static_cast<unsigned long>(stringBuffers_[i].size());
            }
        }
        if (ret == MYSQL_DATA_TRUNCATED) {
            mysql_stmt_bind_result(stmt_, bindBuffer_.data());
            mysql_stmt_fetch(stmt_); // 重取
        }
    } else if (ret == MYSQL_NO_DATA) {
        hasMoreRows_ = false;
    } else {
        hasMoreRows_ = false;
    }
}

bool MySQLStatement::ResultSet::next() {
    fetchRow();
    return hasMoreRows_;
}

size_t MySQLStatement::ResultSet::columnCount() const { return columnCount_; }

std::string MySQLStatement::ResultSet::getFieldName(size_t index) const {
    if (index >= columnCount_)
        return "";
    MYSQL_RES *metadata = mysql_stmt_result_metadata(stmt_);
    if (!metadata)
        return "";
    MYSQL_FIELD *fields = mysql_fetch_fields(metadata);
    std::string name = fields[index].name;
    mysql_free_result(metadata);
    return name;
}

bool MySQLStatement::ResultSet::isNull(size_t index) const {
    if (index >= columnCount_)
        return true;
    return isNull_[index] != 0;
}

int MySQLStatement::ResultSet::getInt(size_t index, int defaultValue) const {
    if (index >= columnCount_ || isNull_[index])
        return defaultValue;
    try {
        return std::stoi(
            std::string(stringBuffers_[index].data(), lengths_[index]));
    } catch (...) {
        return defaultValue;
    }
}

int64_t MySQLStatement::ResultSet::getInt64(size_t index,
                                            int64_t defaultValue) const {
    if (index >= columnCount_ || isNull_[index])
        return defaultValue;
    try {
        return std::stoll(
            std::string(stringBuffers_[index].data(), lengths_[index]));
    } catch (...) {
        return defaultValue;
    }
}

double MySQLStatement::ResultSet::getDouble(size_t index,
                                            double defaultValue) const {
    if (index >= columnCount_ || isNull_[index])
        return defaultValue;
    try {
        return std::stod(
            std::string(stringBuffers_[index].data(), lengths_[index]));
    } catch (...) {
        return defaultValue;
    }
}

std::string
MySQLStatement::ResultSet::getString(size_t index,
                                     const std::string &defaultValue) const {
    if (index >= columnCount_ || isNull_[index])
        return defaultValue;
    return std::string(stringBuffers_[index].data(), lengths_[index]);
}

// --- Query ---
QueryResult query(MySQLPool::ConnectionGuard &conn, const std::string &sql) {
    QueryResult result;
    MySQLStatement stmt(conn, sql);
    if (!stmt.execute() || stmt.hasError()) {
        result.success = false;
        result.error = stmt.getError();
        return result;
    }
    auto rs = stmt.getResultSet();
    if (!rs) {
        result.success = false;
        result.error = "Get result set failed";
        return result;
    }
    for (size_t i = 0; i < rs->columnCount(); ++i) {
        result.columns.push_back(rs->getFieldName(i));
    }
    while (rs->next()) {
        QueryResult::Row row;
        for (size_t i = 0; i < rs->columnCount(); ++i) {
            row.push_back(rs->getString(i));
        }
        result.rows.push_back(row);
    }
    result.success = true;
    return result;
}

} // namespace db