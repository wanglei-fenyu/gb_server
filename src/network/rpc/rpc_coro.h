#pragma once
#include "async_simple/coro/Lazy.h"
#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

NAMESPACE_BEGIN(gb)

namespace detail
{

template <typename T>
class RpcResumeState
{
public:
    RpcResumeState(std::coroutine_handle<> handle, T& result) :
        handle_(handle), result_(result)
    {
    }

    void Complete(T value)
    {
        result_ = std::move(value);
        Resume();
    }

    void Resume()
    {
        if (resumed_.exchange(true, std::memory_order_acq_rel))
            return;
        handle_.resume();
    }

private:
    std::coroutine_handle<> handle_;
    T&                      result_;
    std::atomic<bool>       resumed_{false};
};

template <>
class RpcResumeState<void>
{
public:
    explicit RpcResumeState(std::coroutine_handle<> handle) :
        handle_(handle)
    {
    }

    void Resume()
    {
        if (resumed_.exchange(true, std::memory_order_acq_rel))
            return;
        handle_.resume();
    }

private:
    std::coroutine_handle<> handle_;
    std::atomic<bool>       resumed_{false};
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
    using binder_type = std::function<void(std::coroutine_handle<>, T&)>;

public:
    explicit RpcCallAwaiter(binder_type binder) :
        binder_(std::move(binder))
    {
    }

public:
    bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle)
    {
        binder_(handle, result_);
    }

    T await_resume() { return std::move(result_); }

private:
    T           result_{};
    binder_type binder_;
};

template <>
struct RpcCallAwaiter<void>
{
public:
    using binder_type = std::function<void(std::coroutine_handle<>)>;

public:
    explicit RpcCallAwaiter(binder_type binder) :
        binder_(std::move(binder))
    {
    }

public:
    bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle)
    {
        binder_(handle);
    }

    void await_resume() { return; }

private:
    binder_type binder_;
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
                co_return co_await RpcCallAwaiter<void>{[call = std::move(call), method = std::move(method), args_tuple = std::move(args_tuple)](std::coroutine_handle<> handle) mutable {
                    auto resume_state = std::make_shared<detail::RpcResumeState<void>>(handle);
                    call->SetCallBack([resume_state]() { resume_state->Resume(); });
                    call->SetTimeout([resume_state]() { resume_state->Resume(); });
                    call->SetCancel([resume_state]() { resume_state->Resume(); });
                    detail::InvokeRpcCall(call, method, std::move(args_tuple));
                }};
            }
            else
            {
                co_return co_await RpcCallAwaiter<ResultType>{[call = std::move(call), method = std::move(method), args_tuple = std::move(args_tuple)](std::coroutine_handle<> handle, ResultType& result) mutable {
                    auto resume_state = std::make_shared<detail::RpcResumeState<ResultType>>(handle, result);
                    call->SetCallBack([resume_state](ResultType value) { resume_state->Complete(std::move(value)); });
                    call->SetTimeout([resume_state]() { resume_state->Resume(); });
                    call->SetCancel([resume_state]() { resume_state->Resume(); });
                    detail::InvokeRpcCall(call, method, std::move(args_tuple));
                }};
            }
        }
        else
        {
            co_return co_await RpcCallAwaiter<ResultType>{[call = std::move(call), method = std::move(method), args_tuple = std::move(args_tuple)](std::coroutine_handle<> handle, ResultType& result) mutable {
                auto resume_state = std::make_shared<detail::RpcResumeState<ResultType>>(handle, result);
                call->SetCallBack([resume_state](T1 value, Rets... rest) {
                    resume_state->Complete(std::make_tuple(std::move(value), std::move(rest)...));
                });
                call->SetTimeout([resume_state]() { resume_state->Resume(); });
                call->SetCancel([resume_state]() { resume_state->Resume(); });
                detail::InvokeRpcCall(call, method, std::move(args_tuple));
            }};
        }
    }
};

NAMESPACE_END
