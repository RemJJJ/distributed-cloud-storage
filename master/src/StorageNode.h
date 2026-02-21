#pragma once
#include <string>

struct StorageNode {
    std::string id;
    std::string ip;
    int port;
    bool isLocal;
};