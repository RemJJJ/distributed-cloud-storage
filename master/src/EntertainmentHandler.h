#pragma once

#include "HandlerUtils.h"
#include "db/MySQLPool.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include "net/TcpConnection.h"
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

class EntertainmentHandler : public handlerUtils {
  public:
    EntertainmentHandler();

    bool handlePlaylists(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                         std::shared_ptr<fn::HttpResponse> &resp);
    bool handleDanmaku(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                       std::shared_ptr<fn::HttpResponse> &resp);
    bool handleRooms(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                     std::shared_ptr<fn::HttpResponse> &resp);

  private:
    struct UserContext {
        int userId = -1;
        std::string serviceLevel = "normal";
    };

    void ensureSchema();
    bool verifyUser(const fn::TcpConnectionPtr &conn, fn::HttpRequest &req,
                    const std::shared_ptr<fn::HttpResponse> &resp,
                    UserContext &ctx);
    bool isVipLike(const std::string &serviceLevel) const;
    bool isSvip(const std::string &serviceLevel) const;
    bool fileAccessible(db::MySQLPool::ConnectionGuard &mysql, int userId,
                        int fileId, nlohmann::json *fileInfo = nullptr,
                        const std::string &roomCode = "");
    bool playlistOwned(db::MySQLPool::ConnectionGuard &mysql, int userId,
                       int playlistId);
    nlohmann::json listPlaylists(db::MySQLPool::ConnectionGuard &mysql,
                                 int userId);
    nlohmann::json buildRoomStatus(db::MySQLPool::ConnectionGuard &mysql,
                                   const std::string &roomCode);
    std::string generateRoomCode();
    void sendJson(const std::shared_ptr<fn::HttpResponse> &resp,
                  const nlohmann::json &payload,
                  fn::HttpResponse::HttpStatusCode status =
                      fn::HttpResponse::k200Ok);
};
