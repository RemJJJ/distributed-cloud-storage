#include "HandlerUtils.h"
#include "base/Timestamp.h"
#include "net/Buffer.h"
#include "net/Callbacks.h"
#include "net/EventLoop.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include "net/InetAddress.h"
#include "net/TcpClient.h"
#include "net/TcpConnection.h"
#include "net/TimerId.h"
#include <memory>

class AsyncDeleteTask : public std::enable_shared_from_this<AsyncDeleteTask> {
  public:
    AsyncDeleteTask(fileserver::net::EventLoop *loop,
                    const fileserver::net::InetAddress &dnAddr,
                    const std::string &deleteToken, int fileId);

    ~AsyncDeleteTask();
    void start();

  private:
    void onConnection(const fn::TcpConnectionPtr &conn);
    void onMessage(const fn::TcpConnectionPtr &conn, fn::Buffer *buf,
                   fileserver::Timestamp time);

    fn::TcpClient client_;
    std::string deleteToken_;
    int fileId_;

    fileserver::net::TimerId timeoutTimerId_;
    void onTimeout();
};