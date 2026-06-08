#pragma once 
#include "network/io/session.h"
#include "rpc_function.hpp"
#include "network/rpc/function.hpp"
#include "network/rpc/executor.h"
#include "network/rpc/rpc_timer_pool.h"
#include "network/rpc/register_rpc.h"
#include <atomic>
#include <type_traits>
#include <memory>
#include <functional>


namespace gb
{
class Worker;

constexpr int64_t kRpcdefaultTimeout = 1000 * 5; // 5秒

// ============================================================================
// RpcErrorCode — 所有 RPC 调用结果状态码
// ============================================================================
enum class RpcErrorCode {
    None = 0,
    Timeout = 1,
    Cancel = 2,
    InvalidRequest = 3,
};

// ============================================================================
// RpcResult<T> — 统一 RPC 返回值，类似 NatsResult
// ============================================================================
template <typename T>
struct RpcResult {
    RpcErrorCode error_code{RpcErrorCode::None};
    T value{};
    explicit operator bool() const { return error_code == RpcErrorCode::None; }
};

template <>
struct RpcResult<void> {
    RpcErrorCode error_code{RpcErrorCode::None};
    explicit operator bool() const { return error_code == RpcErrorCode::None; }
};

// 内部生命周期状态
enum class RpcState : uint8_t {
    Pending = 0,
    Completed,
    Timeout,
    Cancelled,
    InvalidRequest,
};

union SequenceId
{
    struct
    {
        uint64_t index : 32;
        uint64_t seq : 32;
    };
    uint64_t value;
};

// 统一 RPC 结果回调 — 代替分散的 done/timeout/cancel 三个回调
using rpc_result_callback = std::function<void(RpcErrorCode, const SessionPtr&, const ReadBufferPtr&, Meta&, int, int64_t)>;


class RpcCall : public std::enable_shared_from_this<RpcCall>
{
public:
    RpcCall();
    ~RpcCall();
    void                      SetId(uint64_t id) { id_ = id; }
    uint64_t                  GetId() { return id_; }
    void                      SetTimeout(int64_t timeout);
    void                      SetSession(const std::shared_ptr<Session>& session);
    void                      BindCurrentExecutor();
    std::shared_ptr<Session>& GetSession() { return session_; }
    void                      Call(Meta& meta,const ReadBufferPtr buffer = nullptr);
    void                      Cancel();
    void                      SetWorkerInfo(uint32_t worker_index, uint32_t local_seq) { worker_index_ = worker_index; local_seq_ = local_seq; }
    bool                      HasSession();
    void                      Done(const SessionPtr& session, const ReadBufferPtr& buffer, Meta& meta, int meta_size, int64_t data_size);
    bool                      IsError();
    RpcErrorCode              ErrorCode();
    template <class F>
    void SetCallBack(F f);

private:
    void StartTimer();
    void StopTimer();

private:
    uint64_t                  id_;
    uint32_t                  worker_index_{0};
    uint32_t                  local_seq_{0};
    RpcTimerPool::TimerHandlePtr timer_handle_;
    std::chrono::steady_clock::duration timeout_;
    std::atomic<RpcState>     state_;
    std::shared_ptr<Session>  session_;
    rpc_result_callback       result_callback_;
};

template <class F>
inline void RpcCall::SetCallBack(F f)
{
    using DecayF = std::decay_t<F>;

    if constexpr (std::is_same_v<DecayF, sol::function>)
    {
        sol::function main_func = BridgeCallback(std::move(f));
        lua_State*    lstate    = main_func.lua_state();
        result_callback_ = [lstate, func = std::move(main_func)](
            RpcErrorCode err,
            const SessionPtr& session,
            const ReadBufferPtr& buffer,
            Meta& meta,
            int meta_size,
            int64_t data_size) mutable
        {
            int top = lua_gettop(lstate);
            std::string s = "";
            GetMsgData(meta, buffer, meta_size, data_size, s);
            RpcReply reply(std::move(meta), session);
            sol::state_view lua_view(lstate);

            sol::protected_function_result result;
            if (err == RpcErrorCode::None && data_size > 0)
            {
                sol::variadic_args arg = gb::msgpack::unpack(
                    lua_view, reinterpret_cast<const uint8_t*>(s.data()), s.size());
                result = func(reply, static_cast<int>(err), arg);
            }
            else
            {
                result = func(reply, static_cast<int>(err));
            }
            if (!result.valid())
                OnScriptError(result);
            lua_settop(lstate, top);
        };
    }
    else
    {
        // ── C++ 路径：handler(RpcErrorCode, value_types...) ──
        using Tuple = typename function_traits<DecayF>::args_tuple;
        static_assert(std::tuple_size_v<Tuple> >= 1 &&
                      std::is_same_v<std::decay_t<std::tuple_element_t<0, Tuple>>, RpcErrorCode>,
                      "First parameter of RpcCall::SetCallBack callback must be RpcErrorCode");
        using ValueTypes = tuple_tail_t<1, Tuple>;

        result_callback_ = [f = std::move(f)](
            RpcErrorCode err,
            const SessionPtr& session,
            const ReadBufferPtr& buffer,
            Meta& meta,
            int meta_size,
            int64_t data_size) mutable
        {
            if constexpr (std::tuple_size_v<ValueTypes> == 0)
            {
                f(err);
            }
            else
            {
                if (err == RpcErrorCode::None)
                {
                    std::string s;
                    GetMsgData(meta, buffer, meta_size, data_size, s);
                    ValueTypes values{};
                    if (!msgpack_unpack_tuple(values, reinterpret_cast<const uint8_t*>(s.data()), s.size()))
                    {
                        std::apply([&](auto&&... args) {
                            f(err, std::forward<decltype(args)>(args)...);
                        }, ValueTypes{});
                        return;
                    }
                    std::apply([&](auto&&... args) {
                        f(RpcErrorCode::None, std::forward<decltype(args)>(args)...);
                    }, std::move(values));
                }
                else
                {
                    std::apply([err, &f](auto&&... args) {
                        f(err, std::forward<decltype(args)>(args)...);
                    }, ValueTypes{});
                }
            }
        };
    }
}


using RpcCallPtr = std::shared_ptr<RpcCall>;
} // namespace gb
