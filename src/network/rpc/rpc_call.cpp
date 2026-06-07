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
    , state_(RpcState::Pending)
    , session_(nullptr)
{
}

RpcCall::~RpcCall()
{
    StopTimer();
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
    // callback_executor_ no longer stored; result_callback_ runs inline on
    // the Worker thread and refers to the captured WorkerExecutor inside the
    // coroutine awaiter if needed.
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

    // 从所属Worker的挂起映射中移除
    auto _worker = WorkerManager::Instance()->GetWorker(worker_index_);
    if (_worker) _worker->TakePendingRpc(local_seq_);

    StopTimer();
    Meta dummy_meta{};
    if (result_callback_)
        result_callback_(RpcErrorCode::Cancel, nullptr, nullptr, dummy_meta, 0, 0);
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
    if (result_callback_)
        result_callback_(RpcErrorCode::None, session, buffer, meta, meta_size, data_size);
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
        // 清理：从所属Worker的挂起映射中移除
        auto _w = WorkerManager::Instance()->GetWorker(worker_index_);
        if (_w) _w->TakePendingRpc(local_seq_);
        StopTimer();
        Meta dummy_meta{};
        if (result_callback_)
            result_callback_(RpcErrorCode::InvalidRequest, nullptr, nullptr, dummy_meta, 0, 0);
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

        // 超时在IO线程上触发 — 投递到所属Worker
        auto worker = WorkerManager::Instance()->GetWorker(self->worker_index_);
        if (!worker)
            return;

        worker->Post([self]() {
            // 从所属Worker的挂起映射中移除
            auto w = WorkerManager::Instance()->GetWorker(self->worker_index_);
            if (w) w->TakePendingRpc(self->local_seq_);

            self->StopTimer();
            Meta dummy_meta{};
            if (self->result_callback_)
                self->result_callback_(RpcErrorCode::Timeout, nullptr, nullptr, dummy_meta, 0, 0);
        });
    });
}

void RpcCall::StopTimer()
{
    if (timer_handle_ && timer_handle_->timer.expiry() > std::chrono::steady_clock::now())
    {
        timer_handle_->timer.cancel();
    }
    if (timer_handle_)
    {
        GetCurrentThreadTimerPool()->Release(timer_handle_);
        timer_handle_.reset();
    }
}

} // namespace gb
