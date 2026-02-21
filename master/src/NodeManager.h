#pragma once
#include "StorageNode.h"
#include <vector>

class NodeManager {
  public:
    NodeManager();

    StorageNode selectNode(); // 现在固定返回本地

  private:
    std::vector<StorageNode> nodes_;
};