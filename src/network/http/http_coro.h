#pragma once
#include "async_simple/coro/Lazy.h"
#include "worker/worker_manager.h"
#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

NAMESPACE_BEGIN(gb)

namespace http
{

namespace detail
{

template <typename T>
class HttpAwaitState
{
public:
    void Bind(std::coroutine_handle<> handle, WorkerExecutor exec)
    {
        handle_   = handle;
        executor_ = std::move(exec);
    }

    void Complete(std::shared_ptr<T> response, const std::error_code &ec)
    {
        response_ = std::move(response);
        ec_       = ec;
        NotifyCompleted();
    }

    bool FinishSuspend()
    {
        suspend_finished_.store(true, std::memory_order_release);
        return completed_.load(std::memory_order_acquire);
    }

    std::shared_ptr<T> TakeResponse()
    {
        return std::move(response_);
    }

    std::error_code TakeError()
    {
        return ec_;
    }

private:
    void NotifyCompleted()
    {
        if (completed_.exchange(true, std::memory_order_acq_rel))
            return;
        if (suspend_finished_.load(std::memory_order_acquire))
        {
            auto h = handle_;
            executor_.schedule([h]() { h.resume(); });
        }
    }

private:
    std::shared_ptr<T>  response_{};
    std::error_code          ec_{};
    std::coroutine_handle<> handle_{};
    WorkerExecutor      executor_;
    std::atomic<bool>   suspend_finished_{false};
    std::atomic<bool>   completed_{false};
};

template <typename T>
struct HttpCallAwaiter
{
public:
    using state_type = detail::HttpAwaitState<T>;
    using binder_type = std::function<void(const std::shared_ptr<state_type> &)>;

public:
    explicit HttpCallAwaiter(binder_type binder) :
        state_(std::make_shared<state_type>()), binder_(std::move(binder))
    {
    }

public:
    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        state_->Bind(handle, WorkerExecutor::Current(true));
        binder_(state_);
        return !state_->FinishSuspend();
    }

    std::shared_ptr<T> await_resume() { return state_->TakeResponse(); }

    std::error_code await_error() { return state_->TakeError(); }

private:
    std::shared_ptr<state_type> state_;
    binder_type                 binder_;
};

} // namespace detail

} // namespace http

NAMESPACE_END
