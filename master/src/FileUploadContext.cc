#include "FileUploadContext.h"
#include "../../third_party/muduo/base/Logging.h"

FileUploadContext::FileUploadContext(const std::string &filename,
                                     const std::string &originalFilename,
                                     std::shared_ptr<FileStorage> storage)
    : filename_(filename), originalFilename_(originalFilename), totalBytes_(0),
      state_(State::kExpectHeaders), boundary_(""), uploadComplete_(false) {
    // 如果没有传入storage， 默认创建LocalFileStorage
    if (storage) {
        storage_ = storage;
    }
    LOG_INFO << "FileUploadContext created: " << filename
             << ", original name: " << originalFilename;
}

FileUploadContext::~FileUploadContext() {}

// 写入数据： 转发给LocalStorage
void FileUploadContext::writeData(const char *data, size_t len) {
    if (!storage_) {
        LOG_ERROR << "RemoteFileStorage not initialized";
        throw std::runtime_error("RemoteFileStorage not initialized");
    }
    try {
        storage_->write(data, len);
        totalBytes_ += len;
        LOG_INFO << "Wrote " << len
                 << "bytes to DataNode, total: " << totalBytes_;
    } catch (const std::exception &e) {
        LOG_ERROR << "FileUploadContext: write failed - " << e.what();
        throw; // 向上层抛出异常
    }
}