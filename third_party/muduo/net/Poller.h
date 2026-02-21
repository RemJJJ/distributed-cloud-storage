#pragma once

#include "../base/Timestamp.h"
#include <map>
#include <vector>

namespace fileserver {
namespace net {
class Channel;
class EventLoop;

/// @brief IO 多路复用器的基类
/// 这个类不拥有Channel对象
class Poller {
  public:
    using ChannelList = std::vector<Channel *>;

    Poller(EventLoop *loop);
    virtual ~Poller();

    /// @brief 轮询IO事件
    /// @param timeoutMs 超时时间
    /// @param activeChannels 活跃的Channel列表
    /// @return 轮训时间
    virtual Timestamp poll(int timeoutMs, ChannelList *activeChannels) = 0;

    /// @brief 更新Channel
    virtual void updateChannel(Channel *channel) = 0;

    /// @brief 移除Channel
    virtual void removeChannel(Channel *channel) = 0;

    /// @brief 是否有Channel
    virtual bool hasChannel(Channel *channel) const;

    /// @brief 获取默认的Poller
    static Poller *newDefaultPoller(EventLoop *loop);

    void assertInLoopThread() const;

  protected:
    using ChannelMap = std::map<int, Channel *>;
    ChannelMap channels_; // fd到Channel的映射

  private:
    EventLoop *ownerLoop_;
};
} // namespace net
} // namespace fileserver