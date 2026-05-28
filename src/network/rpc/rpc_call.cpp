#include "rpc_call.h"
#include "../../common/timer_help.h"
#include "log/log_help.h"
#include "../net_manager/network_manager.h"
namespace gb
{
RpcCall::RpcCall() :
    id_(0), timeout_(std::chrono::milliseconds(kRpcdefaultTimeout)), timeout_func_(nullptr), cancel_func_(nullptr), is_cancel_(false), finished_(false), session_(nullptr), done_call_bcak_(nullptr), timer_(std::nullopt), error_code_(RpcErrorCode::None)
{
}

RpcCall::~RpcCall()
{
    StopTimer();
}

void RpcCall::SetTimeout(std::function<void()> timeout_fun, int64_t timeout)
{
    timeout_func_ = timeout_fun;
    SetTimeout(timeout);
};

void RpcCall::SetCancel(std::function<void()> cancel_fun)
{
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
    callback_executor_ = Executor::Current();
}

void RpcCall::Call(Meta& meta, const ReadBufferPtr buffer /*= nullptr*/)
{
	error_code_.store(RpcErrorCode::None, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    is_cancel_.store(false, std::memory_order_release);
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
    Finish(RpcErrorCode::Cancel, cancel_func_, true);
}

bool RpcCall::HasCallBack() const
{
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
    if (finished_.exchange(true, std::memory_order_acq_rel))
        return;

    StopTimer();
    is_cancel_.store(false, std::memory_order_release);
    error_code_.store(RpcErrorCode::None, std::memory_order_release);
    if (!HasCallBack())
        return;
    auto callback = done_call_bcak_;
    DispatchCompletion([callback, session, buffer, meta = std::move(meta), meta_size, data_size]() mutable {
        callback(session, buffer, meta, meta_size, data_size);
    });
}

bool RpcCall::IsError()
{
    return error_code_.load(std::memory_order_acquire) != RpcErrorCode::None;
}

RpcErrorCode RpcCall::ErrorCode() 
{
    return error_code_.load(std::memory_order_acquire);
}

void RpcCall::StartTimer()
{
    if (!session_ || !timer_)
    {
        Finish(RpcErrorCode::InvalidRequest, cancel_func_, true);
        return;
    }
    is_cancel_.store(false, std::memory_order_release);
    timer_->expires_after(timeout_);
	timer_->async_wait([self = shared_from_this()](const Error_code& error) {
        if (self->is_cancel_.load(std::memory_order_acquire) || error == Asio::error::operation_aborted) {
            return;
        }
        if (self->finished_.load(std::memory_order_acquire))
            return;
		LOG_WARN("RPC timeout {}", self->id_);
		if (!error)
            self->Finish(RpcErrorCode::Timeout, self->timeout_func_, true);
	});
}

void RpcCall::DispatchCompletion(std::function<void()> cb) const
{
    if (!cb)
        return;
    Executor executor;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        executor = callback_executor_;
    }
    executor.Dispatch(std::move(cb));
}

void RpcCall::Finish(RpcErrorCode error_code, std::function<void()> completion, bool remove_call)
{
    if (finished_.exchange(true, std::memory_order_acq_rel))
        return;

    error_code_.store(error_code, std::memory_order_release);
    is_cancel_.store(error_code != RpcErrorCode::None, std::memory_order_release);
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
