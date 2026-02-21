#pragma once

#include "../base/Logging.h"
#include "../base/Timestamp.h"
#include "../base/copyable.h"
#include "Buffer.h"
#include "HttpRequest.h"
#include <memory>
#include <unordered_map>

namespace fileserver {
namespace net {
class HttpContext : public fileserver::copyable {
  public:
    enum HttpRequestParseState {
        kExpectRequestLine, // 0
        kExpectHeaders,     // 1
        kExpectBody,        // 2
        kGotAll             // 3
    };

    enum ParseResult {
        kError = -1,          // 解析出错
        kNeedMore = 0,        // 需要更多数据
        kHeadersComplete = 1, // 头部解析完成
        kGotRequest = 2,      // 整个请求解析完成
    };

    HttpContext()
        : state_(kExpectRequestLine), contentLength_(0), bodyReceived_(0),
          isChunked_(false) {}

    ~HttpContext() { LOG_INFO << "HttpContext destroyed"; }

    // 默认的拷贝构造函数、析构函数和赋值操作符都可以

    // 返回false标识解析错误，true表示解析成功（包括需要更多数据和完成解析的情况）
    HttpContext::ParseResult parseRequest(Buffer *buf, Timestamp receiveTime);
    bool gotAll() const { return state_ == kGotAll; }

    bool expectBody() const { return state_ == kExpectBody; }

    bool headersComplete() const {
        return state_ == kExpectBody || state_ == kGotAll;
    }

    size_t remainingLength() const { return contentLength_ - bodyReceived_; }

    bool isChunked() const { return isChunked_; }

    void reset() {
        state_ = kExpectRequestLine;
        HttpRequest dummy;
        request_.swap(dummy);
        contentLength_ = 0;
        bodyReceived_ = 0;
        isChunked_ = false;
        customContext_.reset();
    }

    const HttpRequest &request() const { return request_; }

    HttpRequest &request() { return request_; }

    HttpRequestParseState state() const { return state_; }

    template <typename T> std::shared_ptr<T> getContext() const {
        return std::static_pointer_cast<T>(customContext_);
    }

    void setContext(const std::shared_ptr<void> &context) {
        customContext_ = context;
    }

  private:
    bool processRequestLine(const char *begin, const char *end);
    bool processHeaders(Buffer *buf);
    bool processBody(Buffer *buf);

    HttpRequestParseState state_ = HttpRequestParseState::kExpectRequestLine;
    HttpRequest request_;
    size_t contentLength_;                // 存储Content-Length的值
    size_t bodyReceived_;                 // 已接收的body长度
    bool isChunked_;                      // 是否为chunked传输
    std::shared_ptr<void> customContext_; // 自定义上下文存储
};
} // namespace net
} // namespace fileserver