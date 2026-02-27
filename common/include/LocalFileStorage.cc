#include "LocalFileStorage.h"
#include "../../third_party/muduo/base/Logging.h"

// LocalFileStorage::LocalFileStorage(const std::string &filename)
//     : filename_(filename), total_bytes_(0) {
//     // 确保目录存在
//     fs::path filePath(filename);
//     fs::path dir = filePath.parent_path();
//     if (!dir.empty() && !fs::exists(dir)) {
//         if (!fs::create_directories(dir)) {
//             LOG_ERROR << "Failed to create directory: " << dir.string();
//             throw std::runtime_error("Failed to create directory: " +
//                                      dir.string());
//         }
//     }

//     // 以二进制写入模式打开文件。
//     file_.open(filename, std::ios::binary | std::ios::out);
//     if (!file_.is_open()) {
//         LOG_ERROR << "Failed to open file: " << filename_;
//         throw std::runtime_error("Failed to open file: " + filename_);
//     }
//     LOG_INFO << "LocalFileStorage: file open - " << filename_;
// }

LocalFileStorage::LocalFileStorage() : filename_(""), total_bytes_(0) {}

LocalFileStorage::LocalFileStorage(const std::string &filename)
    : filename_(filename), total_bytes_(0) {
    if (!open(filename)) {
        throw std::runtime_error("Failed to init LocalFileStorage for " +
                                 filename);
    }
}

LocalFileStorage::~LocalFileStorage() {
    if (file_.is_open()) {
        try {
            file_.flush();
            file_.close();
            LOG_INFO << "LocalFileStorage: file closed - " << filename_
                     << ", total bytes written: " << total_bytes_;
        } catch (const std::exception &e) {
            LOG_ERROR << "LocalFileStorage: error when closing file - "
                      << filename_ << ". Message: " << e.what();
        }
    }
}

bool LocalFileStorage::open(const std::string &filename) {
    // 先关闭已打开的文件
    if (file_.is_open()) {
        close();
    }

    filename_ = filename;
    // 确保目录存在
    fs::path filePath(filename);
    fs::path dir = filePath.parent_path();
    if (!dir.empty() && !fs::exists(dir)) {
        if (!fs::create_directories(dir)) {
            LOG_ERROR << "Failed to create directory: " << dir.string();
            return false; // 返回 false，不再 throw（由调用方决定是否 throw）
        }
    }

    // 打开文件（二进制写入）
    file_.open(filename, std::ios::binary | std::ios::out);
    if (!file_.is_open()) {
        LOG_ERROR << "Failed to open file: " << filename_;
        return false;
    }

    LOG_INFO << "LocalFileStorage: file open - " << filename_;
    return true;
}

bool LocalFileStorage::write(const char *data, size_t len) {
    // 前置检查：返回 false，不 throw
    if (!file_.is_open()) {
        LOG_ERROR << "LocalFileStorage: write failed - file not open: "
                  << filename_;
        return false;
    }
    if (data == nullptr || len == 0) {
        LOG_WARN << "LocalFileStorage: invalid data (null or len=0)";
        return false;
    }

    // 写入数据
    file_.write(data, len);
    if (file_.fail()) {
        LOG_ERROR << "LocalStorage: write failed - " << filename_
                  << ", len: " << len;
        return false;
    }

    // 刷盘 + 累计字节数
    file_.flush();
    total_bytes_ += len;
    return true;
}

bool LocalFileStorage::close() {
    if (file_.is_open()) {
        try {
            file_.flush();
            file_.close();
            LOG_INFO << "LocalFileStorage: file closed - " << filename_
                     << ", total bytes written: " << total_bytes_;
        } catch (const std::exception &e) {
            LOG_ERROR << "LocalFileStorage: error when closing file - "
                      << filename_ << ". Message: " << e.what();
        }
    }
    return true;
}
