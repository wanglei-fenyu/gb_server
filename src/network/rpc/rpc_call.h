#pragma once 
#include "network/io/session.h"
#include "rpc_function.hpp"
#include "network/rpc/function.hpp"
#include "network/rpc/executor.h"
#include <atomic>
#include <mutex>
#include <type_traits>


namespace gb
{
class Worker;

constexpr int64_t kRpcdefaultTimeout = 1000 * 5; // 5秒

typedef rpc_listen_fun rpc_done_call;


enum class RpcErrorCode {
    None = 0,
    Timeout = 1,
    Cancel = 2,
    InvalidRequest = 3,
};

// 内部生命周期状态 — 替代原来的三个独立原子变量
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


class RpcCall : public std::enable_shared_from_this<RpcCall>
{
public:
    RpcCall();
    ~RpcCall();
    void                      SetId(uint64_t id) { id_ = id; }
    uint64_t                  GetId() { return id_; }
    void                      SetTimeout(std::function<void()> timeout_fun, int64_t timeout = kRpcdefaultTimeout);
    void                      SetCancel(std::function<void()> cancel_fun);
    void                      SetTimeout(int64_t timeout);
    void                      SetSession(const std::shared_ptr<Session>& session);
    void                      BindCurrentExecutor();
    std::shared_ptr<Session>& GetSession() { return session_; }
    void                      Call(Meta& meta,const ReadBufferPtr buffer = nullptr);
    void                      Cancel();
    void                      SetWorkerInfo(uint32_t worker_index, uint32_t local_seq) { worker_index_ = worker_index; local_seq_ = local_seq; }
    bool                      HasCallBack() const;
    bool                      HasSession();
    void                      Done(const SessionPtr& session, const ReadBufferPtr& buffer, Meta& meta, int meta_size, int64_t data_size);
    bool                      IsError();
    RpcErrorCode              ErrorCode();
    template <class F>
    void SetCallBack(F f);

private:
    void StartTimer();
    void Finish(std::function<void()> completion);
    void StopTimer();
    void DispatchCompletion(std::function<void()> cb) const;

private:
    uint64_t                  id_;
    uint32_t                  worker_index_{0};
    uint32_t                  local_seq_{0};
    mutable std::optional<Asio::steady_timer> timer_;
    std::chrono::steady_clock::duration timeout_;
    std::function<void()>     timeout_func_;
    std::function<void()>     cancel_func_;
    std::atomic<RpcState>     state_;
    WorkerExecutor                  callback_executor_;
    mutable std::mutex        callback_mutex_;
    std::shared_ptr<Session>  session_;
    rpc_done_call             done_call_bcak_;
};

template <class F>
inline void RpcCall::SetCallBack(F f)
{
    rpc_listen_fun func;
    if constexpr (std::is_same_v<std::decay_t<F>, sol::function>)
    {
        auto            lua_state = f.lua_state();
        sol::state_view lua_view(lua_state);
        sol::state*     state = (sol::state*)&lua_view;
        func                  = RpcFunctionaTraits<sol::function>::make(state, f);
    }
    else
    {
        func = MakeRpcHandler(std::move(f));
    }
    std::lock_guard<std::mutex> lock(callback_mutex_);
    done_call_bcak_ = std::move(func);
}


using RpcCallPtr = std::shared_ptr<RpcCall>;
} // namespace gb
