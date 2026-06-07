#include "rpc_timer_pool.h"
#include "log/log.h"

namespace gb
{

RpcTimerPoolPtr& GetCurrentThreadTimerPool()
{
    // 每个 IO 线程有独立的 timer pool
    thread_local RpcTimerPoolPtr pool = std::make_shared<RpcTimerPool>();
    return pool;
}

RpcTimerPool::TimerHandlePtr RpcTimerPool::Acquire(IoService& ios)
{
    TimerHandlePtr handle;

    if (!available_.empty())
    {
        handle = std::move(available_.back());
        available_.pop_back();
    }

    if (!handle)
    {
        // 池中没有可用的 timer，创建新的
        handle = std::make_shared<TimerHandle>(ios, next_id_++);
        pool_size_.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        // 重置 timer 到新的 io_service（使用宏获取真实类型名）
#if USE_STANDALONE_ASIO
        handle->timer.~basic_waitable_timer<std::chrono::steady_clock>();
#else
        handle->timer.~basic_waitable_timer<boost::asio::steady_timer::clock_type>();
#endif
        new (&handle->timer) boost::asio::steady_timer(ios);
        handle->ios = &ios;
        handle->id = next_id_++;
    }

    in_use_.fetch_add(1, std::memory_order_relaxed);
    return handle;
}

void RpcTimerPool::Release(TimerHandlePtr handle)
{
    if (!handle)
        return;

    // 取消 timer
    handle->timer.cancel();

    available_.push_back(std::move(handle));

    in_use_.fetch_sub(1, std::memory_order_relaxed);
}

void RpcTimerPool::PreWarm(IoService& ios, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        auto handle = std::make_shared<TimerHandle>(ios, next_id_++);
        available_.push_back(std::move(handle));
    }
    pool_size_.fetch_add(count, std::memory_order_relaxed);
}

} // namespace gb
