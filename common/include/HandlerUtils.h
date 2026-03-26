#pragma once
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include "net/TcpConnection.h"

namespace fn = fileserver::net;
class handlerUtils {
  public:
    // 【通用】统一错误响应
    static void sendError(const std::shared_ptr<fn::HttpResponse> &resp,
                          const std::string &message,
                          fn::HttpResponse::HttpStatusCode code,
                          const fn::TcpConnectionPtr &conn);

    // 【通用】URL解码
    static std::string urlDecode(const std::string &encoded);

    // 【通用】获取文件类型
    static std::string getFileType(const std::string &filename);
};