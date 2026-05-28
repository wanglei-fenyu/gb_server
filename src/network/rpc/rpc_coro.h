#pragma once
#include "async_simple/coro/Lazy.h"
#include "network/scheduler/executor.h"
#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

NAMESPACE_BEGIN(gb)

namespace detail
{

template <typename T>
class RpcAwaitState
{
public:
    void Bind(std::coroutine_handle<> handle, WorkerExecutor exec)
    {
        handle_   = handle;
        executor_ = std::move(exec);
    }

    void Complete(T value)
    {
        result_.emplace(std::move(value));
        NotifyCompleted();
    }

    void Resume()
    {
        NotifyCompleted();
    }

    bool FinishSuspend()
    {
        suspend_finished_.store(true, std::memory_order_release);
        return completed_.load(std::memory_order_acquire);
    }

    T TakeResult()
    {
        if (result_)
            return std::move(*result_);
        return {};
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
    std::optional<T>        result_{};
    std::coroutine_handle<> handle_{};
    WorkerExecutor                executor_;
    std::atomic<bool>       suspend_finished_{false};
    std::atomic<bool>       completed_{false};
};

template <>
class RpcAwaitState<void>
{
public:
    void Bind(std::coroutine_handle<> handle, WorkerExecutor exec)
    {
        handle_   = handle;
        executor_ = std::move(exec);
    }

    void Resume()
    {
        NotifyCompleted();
    }

    bool FinishSuspend()
    {
        suspend_finished_.store(true, std::memory_order_release);
        return completed_.load(std::memory_order_acquire);
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
    std::coroutine_handle<> handle_{};
    WorkerExecutor                executor_;
    std::atomic<bool>       suspend_finished_{false};
    std::atomic<bool>       completed_{false};
};

template <typename Tuple>
void InvokeRpcCall(const RpcCallPtr& call, const std::string& method, Tuple&& args)
{
    std::apply(
        [&](auto&&... values) {
            gb::NetworkManager::Instance()->Call(call, method, std::forward<decltype(values)>(values)...);
        },
        std::forward<Tuple>(args));
}

} // namespace detail

template <typename T>
struct RpcCallAwaiter
{
public:
    using state_type  = detail::RpcAwaitState<T>;
    using binder_type = std::function<void(const std::shared_ptr<state_type>&)>;

public:
    explicit RpcCallAwaiter(binder_type binder) :
        state_(std::make_shared<state_type>()), binder_(std::move(binder))
    {
    }

public:
    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        // Capture the current executor before invoking the binder so that any
        // synchronous completion still finds a valid executor reference.
        state_->Bind(handle, WorkerExecutor::Current(true));
        binder_(state_);
        return !state_->FinishSuspend();
    }

    T await_resume() { return state_->TakeResult(); }

private:
    std::shared_ptr<state_type> state_;
    binder_type                 binder_;
};

template <>
struct RpcCallAwaiter<void>
{
public:
    using state_type  = detail::RpcAwaitState<void>;
    using binder_type = std::function<void(const std::shared_ptr<state_type>&)>;

public:
    explicit RpcCallAwaiter(binder_type binder) :
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

    void await_resume() { return; }

private:
    std::shared_ptr<state_type> state_;
    binder_type                 binder_;
};

template <typename T1 = void, typename... Rets>
struct CoRpc
{
    using ResultType = std::conditional_t<sizeof...(Rets) == 0, T1, std::tuple<T1, Rets...>>;

    template <typename... Args>
    static async_simple::coro::Lazy<ResultType> execute(RpcCallPtr call, std::string method, Args... args) noexcept
    {
        auto args_tuple = std::make_tuple(std::forward<Args>(args)...);
        if constexpr (sizeof...(Rets) == 0)
        {
            if constexpr (std::is_void_v<ResultType>)
            {
                co_return co_await RpcCallAwaiter<void>{[call = std::move(call), method = std::move(method), args_tuple = std::move(args_tuple)](const std::shared_ptr<detail::RpcAwaitState<void>>& state) mutable {
                    call->SetCallBack([state]() { state->Resume(); });
                    call->SetTimeout([state]() { state->Resume(); });
                    call->SetCancel([state]() { state->Resume(); });
                    detail::InvokeRpcCall(call, method, std::move(args_tuple));
                }};
            }
            else
            {
                co_return co_await RpcCallAwaiter<ResultType>{[call = std::move(call), method = std::move(method), args_tuple = std::move(args_tuple)](const std::shared_ptr<detail::RpcAwaitState<ResultType>>& state) mutable {
                    call->SetCallBack([state](ResultType value) { state->Complete(std::move(value)); });
                    call->SetTimeout([state]() { state->Resume(); });
                    call->SetCancel([state]() { state->Resume(); });
                    detail::InvokeRpcCall(call, method, std::move(args_tuple));
                }};
            }
        }
        else
        {
            co_return co_await RpcCallAwaiter<ResultType>{[call = std::move(call), method = std::move(method), args_tuple = std::move(args_tuple)](const std::shared_ptr<detail::RpcAwaitState<ResultType>>& state) mutable {
                call->SetCallBack([state](T1 value, Rets... rest) {
                    state->Complete(std::make_tuple(std::move(value), std::move(rest)...));
                });
                call->SetTimeout([state]() { state->Resume(); });
                call->SetCancel([state]() { state->Resume(); });
                detail::InvokeRpcCall(call, method, std::move(args_tuple));
            }};
        }
    }
};

NAMESPACE_END
