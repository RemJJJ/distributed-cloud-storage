#pragma once
#include "NodeManager.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include "net/TcpConnection.h"

namespace fn = fileserver::net;

class RegisterNodeHandler {
  public:
    bool handleRegisterNode(const fn::TcpConnectionPtr &conn,
                            fn::HttpRequest &req,
                            std::shared_ptr<fn::HttpResponse> &resp);
};