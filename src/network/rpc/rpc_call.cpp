#include "rpc_call.h"
#include "../../common/timer_help.h"
#include "log/log_help.h"
#include "../net_manager/network_manager.h"
namespace gb
{
RpcCall::RpcCall() :
    id_(0), timeout_(std::chrono::milliseconds(kRpcdefaultTimeout)), timeout_func_(nullptr), cancel_func_(nullptr), state_(RpcState::Pending), session_(nullptr), done_call_bcak_(nullptr), timer_(std::nullopt)
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
    if (!timer_ && HasSession()) {
         timer_ = Asio::steady_timer(GetSession()->ioservice());
    }
}

void RpcCall::SetSession(const std::shared_ptr<Session>& session)
{
    session_ = session;
    if (!timer_ && session_) {
         timer_ = Asio::steady_timer(GetSession()->ioservice());
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

    std::function<void()> cancel_callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cancel_callback = cancel_func_;
    }
    Finish(std::move(cancel_callback), true);
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

    rpc_done_call callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = done_call_bcak_;
    }
    Finish(callback ? std::function<void()>([callback, session, buffer, meta = std::move(meta), meta_size, data_size]() mutable {
               callback(session, buffer, meta, meta_size, data_size);
           })
                    : std::function<void()>{},
           false);
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
    if (!session_ || !timer_)
    {
        RpcState expected = RpcState::Pending;
        if (!state_.compare_exchange_strong(expected, RpcState::InvalidRequest, std::memory_order_acq_rel, std::memory_order_acquire))
            return;
        std::function<void()> cancel_callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            cancel_callback = cancel_func_;
        }
        Finish(std::move(cancel_callback), true);
        return;
    }
    timer_->expires_after(timeout_);
    timer_->async_wait([self = shared_from_this()](const Error_code& error) {
        if (error == Asio::error::operation_aborted)
            return;

        RpcState expected = RpcState::Pending;
        if (!self->state_.compare_exchange_strong(expected, RpcState::Timeout, std::memory_order_acq_rel, std::memory_order_acquire))
            return;

        LOG_WARN("RPC timeout {}", self->id_);
        if (!error)
        {
            std::function<void()> timeout_callback;
            {
                std::lock_guard<std::mutex> lock(self->callback_mutex_);
                timeout_callback = self->timeout_func_;
            }
            self->Finish(std::move(timeout_callback), true);
        }
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

void RpcCall::Finish(std::function<void()> completion, bool remove_call)
{
    StopTimer();
    if (remove_call)
        gb::NetworkManager::Instance()->RpcCancel(GetId());
    DispatchCompletion(std::move(completion));
}

void RpcCall::StopTimer()
{
    if (timer_ && timer_->expiry() > std::chrono::steady_clock::now())
        timer_->cancel();
}

} // namespace gb
