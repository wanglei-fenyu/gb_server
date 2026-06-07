#pragma once
#include "async_simple/coro/Lazy.h"
#include "network/rpc/executor.h"
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

template <typename Tuple>
void InvokeRpcCall(const RpcCallPtr& call, const std::string& method, uint64_t id, Tuple&& args)
{
    std::apply(
        [&](auto&&... values) {
            gb::NetworkManager::Instance()->Call(call, method, id, std::forward<decltype(values)>(values)...);
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
        state_->Bind(handle, WorkerExecutor::Current(true));
        binder_(state_);
        return !state_->FinishSuspend();
    }

    T await_resume() { return state_->TakeResult(); }

private:
    std::shared_ptr<state_type> state_;
    binder_type                 binder_;
};

template <typename T1 = void, typename... Rets>
struct CoRpc
{
    using ResultType = std::conditional_t<sizeof...(Rets) == 0, T1, std::tuple<T1, Rets...>>;
    using ReturnType = RpcResult<ResultType>;

    template <typename... Args>
    static async_simple::coro::Lazy<ReturnType> execute(RpcCallPtr call, std::string method, uint64_t id, Args... args) noexcept
    {
        auto args_tuple = std::make_tuple(std::forward<Args>(args)...);
        if constexpr (sizeof...(Rets) == 0)
        {
            if constexpr (std::is_void_v<T1>)
            {
                co_return co_await RpcCallAwaiter<ReturnType>{[call = std::move(call), method = std::move(method), id, args_tuple = std::move(args_tuple)](const std::shared_ptr<detail::RpcAwaitState<ReturnType>>& state) mutable {
                    call->SetCallBack([state](RpcErrorCode err) {
                        state->Complete(ReturnType{err});
                    });
                    detail::InvokeRpcCall(call, method, id, std::move(args_tuple));
                }};
            }
            else
            {
                co_return co_await RpcCallAwaiter<ReturnType>{[call = std::move(call), method = std::move(method), id, args_tuple = std::move(args_tuple)](const std::shared_ptr<detail::RpcAwaitState<ReturnType>>& state) mutable {
                    call->SetCallBack([state](RpcErrorCode err, T1 value) {
                        state->Complete(ReturnType{err, std::move(value)});
                    });
                    detail::InvokeRpcCall(call, method, id, std::move(args_tuple));
                }};
            }
        }
        else
        {
            co_return co_await RpcCallAwaiter<ReturnType>{[call = std::move(call), method = std::move(method), id, args_tuple = std::move(args_tuple)](const std::shared_ptr<detail::RpcAwaitState<ReturnType>>& state) mutable {
                call->SetCallBack([state](RpcErrorCode err, T1 value, Rets... rest) {
                    state->Complete(ReturnType{err, std::make_tuple(std::move(value), std::move(rest)...)});
                });
                detail::InvokeRpcCall(call, method, id, std::move(args_tuple));
            }};
        }
    }
};

NAMESPACE_END
