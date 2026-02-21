#include "FileUploadContext.h"
#include "../../third_party/muduo/base/Logging.h"

FileUploadContext::FileUploadContext(const std::string &filename,
                                     const std::string &originalFilename,
                                     std::unique_ptr<FileStorage> storage)
    : originalFilename_(originalFilename), state_(State::kExpectHeaders),
      boundary_("") {
    // 如果没有传入storage， 默认创建LocalFileStorage
    if (storage) {
        storage_ = std::move(storage);
    } else {
        // 显示构造LocalFileStorage
        auto local_storage = std::make_unique<LocalFileStorage>(filename);
        if (!local_storage->open(filename)) {
            throw std::runtime_error("Failed to open LocalFileStorage for " +
                                     filename);
        }
        storage_ = std::move(local_storage);
    }
    LOG_INFO << "FileUploadContext created: " << filename
             << ", original name: " << originalFilename;
}

FileUploadContext::~FileUploadContext() {
    if (storage_) {
        storage_->close();
        storage_.reset();
    }
}

// 写入数据： 转发给LocalStorage
void FileUploadContext::writeData(const char *data, size_t len) {
    try {
        storage_->write(data, len);
    } catch (const std::exception &e) {
        LOG_ERROR << "FileUploadContext: write failed - " << e.what();
        throw; // 向上层抛出异常
    }
}