#include "../../common/include/FileStorage.h"
#include <string>

class RemoteFileStorage : public FileStorage {
  public:
    RemoteFileStorage(const std::string &ip, int port);

    bool open(const std::string &filename) override;

    bool write(const char *data, size_t len) override;

    bool close() override;

    uintmax_t totalBytes() const override;

    std::string &filename() const override;

  private:
    std::string ip_;
    int port_;
    int sockfd_;
    std::string filename_;
};