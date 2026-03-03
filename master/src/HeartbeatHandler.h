#pragma once
#include "../../third_party/muduo/net/HttpRequest.h"
#include "../../third_party/muduo/net/HttpResponse.h"
#include "../../third_party/muduo/net/TcpConnection.h"
#include "NodeManager.h"

class HeartbeatHandler {
  public:
    bool handleHeartbeat(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                         std::shared_ptr<fn::HttpResponse> &resp);
};