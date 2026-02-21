#include "NodeManager.h"

NodeManager::NodeManager() {
    nodes_.push_back({"node1", "127.0.0.1", 9000, true});
}

StorageNode NodeManager::selectNode() { return nodes_[0]; }