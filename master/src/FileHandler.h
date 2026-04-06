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
#include <memory>

class FileHandler : public handlerUtils {
  public:
    FileHandler();

    bool handleFileUpload(const fileserver::net::TcpConnectionPtr &conn,
                          fileserver::net::HttpRequest &req,
                          std::shared_ptr<fileserver::net::HttpResponse> &resp);

    bool handleListFiles(const fileserver::net::TcpConnectionPtr &conn,
                         fileserver::net::HttpRequest &req,
                         std::shared_ptr<fileserver::net::HttpResponse> &resp);

    bool
    handleFileDownload(const fileserver::net::TcpConnectionPtr &conn,
                       fileserver::net::HttpRequest &req,
                       std::shared_ptr<fileserver::net::HttpResponse> &resp);

    // 回收站
    bool handleDeleteFile(const fileserver::net::TcpConnectionPtr &conn,
                          fileserver::net::HttpRequest &req,
                          std::shared_ptr<fileserver::net::HttpResponse> &resp);

    bool
    handleListRecycleBin(const fileserver::net::TcpConnectionPtr &conn,
                         fileserver::net::HttpRequest &req,
                         std::shared_ptr<fileserver::net::HttpResponse> &resp);

    bool
    handleRestoreFile(const fileserver::net::TcpConnectionPtr &conn,
                      fileserver::net::HttpRequest &req,
                      std::shared_ptr<fileserver::net::HttpResponse> &resp);

    bool handleHardDelete(const fileserver::net::TcpConnectionPtr &conn,
                          fileserver::net::HttpRequest &req,
                          std::shared_ptr<fileserver::net::HttpResponse> &resp);

    bool handleCreateFolder(
        const fileserver::net::TcpConnectionPtr &conn,
        fileserver::net::HttpRequest &req,
        std::shared_ptr<fileserver::net::HttpResponse> &resp);

    bool handleListFolders(const fileserver::net::TcpConnectionPtr &conn,
                           fileserver::net::HttpRequest &req,
                           std::shared_ptr<fileserver::net::HttpResponse> &resp);

  private:
    std::string generateUniqueFilename(const std::string &prefix);
};
