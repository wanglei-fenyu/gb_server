#pragma once
#include <memory>
#include <vector>
#include <functional>
#include <atomic>
#include "gbnet/common/def.h"
#include "gbnet/common/define.h"

namespace gb
{

// 每个 IO 线程维护自己的 TimerPool，timer 与 io_context 绑定
// Acquire/Release 必须在同一个 IO 线程上调用
class RpcTimerPool : public std::enable_shared_from_this<RpcTimerPool>
{
public:
    RpcTimerPool() = default;
    ~RpcTimerPool() = default;

    struct TimerHandle : public std::enable_shared_from_this<TimerHandle>
    {
        IoService* ios{nullptr};
        mutable boost::asio::steady_timer timer;
        size_t id{0};

        explicit TimerHandle(IoService& ioservice, size_t i)
            : ios(&ioservice), timer(ioservice), id(i)
        {
        }
    };

    using TimerHandlePtr = std::shared_ptr<TimerHandle>;

    // 从池中获取 timer（必须在正确的 IO 线程上调用）
    TimerHandlePtr Acquire(IoService& ios);

    // 归还 timer 到池中
    void Release(TimerHandlePtr handle);

    // 预热池
    void PreWarm(IoService& ios, size_t count);

    size_t GetPoolSize() const { return pool_size_.load(std::memory_order_relaxed); }
    size_t GetInUseCount() const { return in_use_.load(std::memory_order_relaxed); }

private:
    std::vector<TimerHandlePtr> available_;
    std::atomic<size_t>        pool_size_{0};
    std::atomic<size_t>        in_use_{0};
    size_t                     next_id_{0};
};

using RpcTimerPoolPtr = std::shared_ptr<RpcTimerPool>;

// 获取当前 IO 线程的 thread_local timer pool
RpcTimerPoolPtr& GetCurrentThreadTimerPool();

} // namespace gb
