#include "RemoteFileStorage.h"
#include "../../third_party/muduo/base/Logging.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

bool sendAll(int sockfd, const char *data, size_t len) {
    size_t totalSent = 0;
    while (totalSent < len) {
        ssize_t sent = ::send(sockfd, data + totalSent, len - totalSent, 0);
        if (sent <= 0) {
            return false;
        }
        totalSent += sent;
    }
    return true;
}

RemoteFileStorage::RemoteFileStorage(const std::string &ip, int port)
    : ip_(ip), port_(port), sockfd_(-1) {}

bool RemoteFileStorage::open(const std::string &filename) {
    filename_ = filename;
    sockfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) {
        LOG_ERROR << "Failed to create socket";
        return false;
    }
    struct sockaddr_in server_addr;
    ::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = ::htons(port_);
    if (::inet_pton(AF_INET, ip_.c_str(), &server_addr.sin_addr) <= 0) {
        LOG_ERROR << "Invaild IP address: " << ip_;
        if (sockfd_ >= 0) {
            ::close(sockfd_);
            sockfd_ = -1;
        }
        return false;
    }
    if (::connect(sockfd_, reinterpret_cast<sockaddr *>(&server_addr),
                  sizeof(server_addr)) < 0) {
        LOG_ERROR << "Failed to connect to " << ip_ << ": " << port_;
        if (sockfd_ >= 0) {
            ::close(sockfd_);
            sockfd_ = -1;
        }
        return false;
    }
    std::string openCmd = "OPEN " + filename_ + "\n";
    if (!sendAll(sockfd_, openCmd.c_str(), openCmd.size())) {
        LOG_ERROR << "Failed to send open command to " << ip_ << ": " << port_;
        if (sockfd_ >= 0) {
            ::close(sockfd_);
            sockfd_ = -1;
        }
        return false;
    }
    return true;
}

bool RemoteFileStorage::write(const char *data, size_t len) {
    // 协议：
    // OPEN filename\n
    // DATA len\n
    // (binary)
    // CLOSE\n
    std::string header = "DATA " + std::to_string(len) + "\n";

    if (!sendAll(sockfd_, header.c_str(), header.size())) {
        LOG_ERROR << "Failed to send header to " << ip_ << ": " << port_;
        if (sockfd_ >= 0) {
            ::close(sockfd_);
            sockfd_ = -1;
        }
        return false;
    }
    if (!sendAll(sockfd_, data, len)) {
        LOG_ERROR << "Failed to send data to " << ip_ << ": " << port_;
        if (sockfd_ >= 0) {
            ::close(sockfd_);
            sockfd_ = -1;
        }
        return false;
    }
    return true;
}

bool RemoteFileStorage::close() {
    std::string closeCmd = "CLOSE\n";
    if (!sendAll(sockfd_, closeCmd.c_str(), closeCmd.size())) {
        LOG_ERROR << "Failed to send close command to " << ip_ << ": " << port_;
        if (sockfd_ >= 0) {
            ::close(sockfd_);
            sockfd_ = -1;
        }
        return false;
    }
    ::close(sockfd_);
    sockfd_ = -1;
    return true;
}
