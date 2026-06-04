#include "rpc_call.h"
#include "rpc_timer_pool.h"
#include "base/timer_help.h"
#include "log/log.h"
#include "worker/worker.h"

namespace gb
{

RpcCall::RpcCall()
    : id_(0)
    , worker_index_(0)
    , local_seq_(0)
    , timeout_(std::chrono::milliseconds(kRpcdefaultTimeout))
    , timeout_func_(nullptr)
    , cancel_func_(nullptr)
    , state_(RpcState::Pending)
    , session_(nullptr)
    , done_call_bcak_(nullptr)
{
}

RpcCall::~RpcCall()
{
    StopTimer();
}

void RpcCall::SetTimeout(std::function<void()> timeout_fun, int64_t timeout)
{
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        timeout_func_ = std::move(timeout_fun);
    }
    SetTimeout(timeout);
};

void RpcCall::SetCancel(std::function<void()> cancel_fun)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    cancel_func_ = std::move(cancel_fun);
}

void RpcCall::SetTimeout(int64_t timeout)
{
    timeout_ = std::chrono::milliseconds(timeout);
    if (!timer_handle_ && HasSession())
    {
        timer_handle_ = GetCurrentThreadTimerPool()->Acquire(session_->ioservice());
    }
}

void RpcCall::SetSession(const std::shared_ptr<Session>& session)
{
    session_ = session;
    if (!timer_handle_ && session_)
    {
        timer_handle_ = GetCurrentThreadTimerPool()->Acquire(session_->ioservice());
    }
}

void RpcCall::BindCurrentExecutor()
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_executor_ = WorkerExecutor::Current(false);
}

void RpcCall::Call(Meta& meta, const ReadBufferPtr buffer /*= nullptr*/)
{
    state_.store(RpcState::Pending, std::memory_order_release);
    StartTimer();
    if (session_)
    {
        if (buffer)
            session_->Send(&meta, buffer);
        else
            session_->Send(&meta);
    }
}



void RpcCall::Cancel()
{
    RpcState expected = RpcState::Pending;
    if (!state_.compare_exchange_strong(expected, RpcState::Cancelled, std::memory_order_acq_rel, std::memory_order_acquire))
        return;

    // 从所属Worker的挂起映射中移除（Cancel必须在所属Worker线程中调用）
    auto _worker = WorkerManager::Instance()->GetWorker(worker_index_);
    if (_worker) _worker->TakePendingRpc(local_seq_);

    std::function<void()> cancel_callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cancel_callback = cancel_func_;
    }
    Finish(std::move(cancel_callback));
}

bool RpcCall::HasCallBack() const
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    return done_call_bcak_ != nullptr;
}

bool RpcCall::HasSession()
{
    if (!session_)
        return false;
    if (session_->is_closed() || !session_->is_connected())
        return false;
    return true;
}

void RpcCall::Done(const SessionPtr& session, const ReadBufferPtr& buffer, Meta& meta, int meta_size, int64_t data_size)
{
    RpcState expected = RpcState::Pending;
    if (!state_.compare_exchange_strong(expected, RpcState::Completed, std::memory_order_acq_rel, std::memory_order_acquire))
        return;

    StopTimer();
    rpc_done_call callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = done_call_bcak_;
    }
    // 直接在当前Worker线程上调用回调。
    // 不需要DispatchCompletion — Done()始终从所属Worker调用
    // （IO线程将响应投递到此Worker，Worker在线程本地映射上调用TakePendingRpc，
    //  然后内联调用Done()）。
    //
    // 回调（例如MakeRpcHandler的lambda）调用state->Complete(value)
    // 或state->Resume()，后者调用NotifyCompleted() → executor_.schedule([h](){h.resume();})。
    // 由于executor_是绑定到同一Worker的WorkerExecutor，其IsCurrent()
    // 返回true，因此h.resume()内联执行 — 零上下文切换。
    if (callback)
        callback(session, buffer, meta, meta_size, data_size);
}

bool RpcCall::IsError()
{
    return ErrorCode() != RpcErrorCode::None;
}

RpcErrorCode RpcCall::ErrorCode()
{
    switch (state_.load(std::memory_order_acquire))
    {
    case RpcState::Timeout:        return RpcErrorCode::Timeout;
    case RpcState::Cancelled:      return RpcErrorCode::Cancel;
    case RpcState::InvalidRequest: return RpcErrorCode::InvalidRequest;
    default:                       return RpcErrorCode::None;
    }
}

void RpcCall::StartTimer()
{
    if (!session_ || !timer_handle_)
    {
        RpcState expected = RpcState::Pending;
        if (!state_.compare_exchange_strong(expected, RpcState::InvalidRequest, std::memory_order_acq_rel, std::memory_order_acquire))
            return;
        // 清理：从所属Worker的挂起映射中移除并调用取消回调
        auto _w = WorkerManager::Instance()->GetWorker(worker_index_);
        if (_w) _w->TakePendingRpc(local_seq_);
        std::function<void()> cancel_callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            cancel_callback = cancel_func_;
        }
        Finish(std::move(cancel_callback));
        return;
    }

    timer_handle_->timer.expires_after(timeout_);
    timer_handle_->timer.async_wait([self = shared_from_this()](const Error_code& error) {
        if (error == Asio::error::operation_aborted)
            return;

        RpcState expected = RpcState::Pending;
        if (!self->state_.compare_exchange_strong(expected, RpcState::Timeout, std::memory_order_acq_rel, std::memory_order_acquire))
            return;

        LOG_WARN("RPC timeout {}", self->id_);

        // 超时在IO线程上触发 — 无法直接访问Worker的线程本地映射。
        // 投递到所属Worker以便安全地从其挂起映射中移除。
        auto worker = WorkerManager::Instance()->GetWorker(self->worker_index_);
        if (!worker)
            return;

        worker->Post([self, worker]() {
            // 从所属Worker的挂起映射中移除（在所属Worker线程上安全执行）
            worker->TakePendingRpc(self->local_seq_);

            std::function<void()> timeout_callback;
            {
                std::lock_guard<std::mutex> lock(self->callback_mutex_);
                timeout_callback = self->timeout_func_;
            }
            if (timeout_callback)
                timeout_callback();
        });
    });
}

void RpcCall::DispatchCompletion(std::function<void()> cb) const
{
    if (!cb)
        return;
    WorkerExecutor executor;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        executor = callback_executor_;
    }
    executor.Dispatch(std::move(cb));
}

void RpcCall::Finish(std::function<void()> completion)
{
    StopTimer();
    DispatchCompletion(std::move(completion));
}

void RpcCall::StopTimer()
{
    if (timer_handle_ && timer_handle_->timer.expiry() > std::chrono::steady_clock::now())
    {
        timer_handle_->timer.cancel();
    }
    // 归还 timer 到池
    if (timer_handle_)
    {
        GetCurrentThreadTimerPool()->Release(timer_handle_);
        timer_handle_.reset();
    }
}

} // namespace gb
