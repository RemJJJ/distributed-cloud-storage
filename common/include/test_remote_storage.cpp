#include "RemoteFileStorage.h"
#include "base/Logging.h"
#include "net/EventLoop.h"
#include <chrono>
#include <fstream>
#include <unistd.h>

using namespace fn;

int main() {
    fileserver::Logger::setLogLevel(fileserver::Logger::DEBUG);
    EventLoop loop;
    std::shared_ptr<RemoteFileStorage> storage(
        std::make_shared<RemoteFileStorage>("127.0.0.1", 8888, &loop));

    // 读取本地文件
    std::ifstream localFile("JJJ.txt");
    if (!localFile.is_open()) {
        LOG_ERROR << "Failed to open file JJJ.txt";
    }
    // 读取文件内容
    std::string content((std::istreambuf_iterator<char>(localFile)),
                        std::istreambuf_iterator<char>());

    // // 设置连接完成回调
    // storage->setConnectionCallback([&storage, &loop, &content]() {
    //     // 连接建立后再写数据
    //     storage->write(content.c_str(), content.size());
    //     LOG_INFO << "Content written";

    //     // 数据发送后关闭
    //     storage->close();
    // });

    // // 注册关闭回调
    // storage->setCloseCallback([&loop]() {
    //     LOG_DEBUG << "Close callback triggered: Channel has been removed";
    //     loop.quit();
    // });

    storage->open("uploads/JJJ.txt");

    // // 打开新文件
    // storage.open("uploads/JJJ.txt");
    // LOG_INFO << "uploads/JJJ.txt opened";

    // // 读取本地文件
    // std::ifstream localFile("JJJ.txt");
    // if (!localFile.is_open()) {
    //     LOG_ERROR << "Failed to open JJJ.txt";
    // }
    // // 读取文件内容
    // std::string content((std::istreambuf_iterator<char>(localFile)),
    //                     std::istreambuf_iterator<char>());
    // // 写文件内容
    // storage.write(content.c_str(), content.size());
    // LOG_INFO << "JJJ.txt written";

    // storage.close();
    // LOG_INFO << "uploads/JJJ.txt closed";

    // 启动事件循环
    loop.loop();
    return 0;
}