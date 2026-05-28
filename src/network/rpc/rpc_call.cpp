#include "rpc_call.h"
#include "../../common/timer_help.h"
#include "../../common/worker/worker_manager.h"
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
    if (timer_ && timer_->expiry() > std::chrono::steady_clock::now())
    {
        timer_->cancel();
    }
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

void RpcCall::BindCurrentWorker()
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_worker_ = WorkerManager::Instance()->GetCurWorker();
}

void RpcCall::Call(Meta& meta, const ReadBufferPtr buffer /*= nullptr*/)
{
	error_code_ = RpcErrorCode::None;
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
    if (finished_.exchange(true, std::memory_order_acq_rel))
        return;
    is_cancel_.store(true, std::memory_order_release);
    if (error_code_ == RpcErrorCode::None)
        error_code_ = RpcErrorCode::Cancel;
    if (timer_ && timer_->expiry() > std::chrono::steady_clock::now())
    {
        timer_->cancel();
    }
    gb::NetworkManager::Instance()->RpcCancel(GetId());
    DispatchCompletion(cancel_func_);
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
    // 取消定时器
    if (timer_ && timer_->expiry() > std::chrono::steady_clock::now())
    {
        timer_->cancel();
    }


    if (is_cancel_.load(std::memory_order_acquire) || !HasCallBack())
        return;
    auto callback = done_call_bcak_;
    DispatchCompletion([callback, session, buffer, meta = std::move(meta), meta_size, data_size]() mutable {
        callback(session, buffer, meta, meta_size, data_size);
    });
}

bool RpcCall::IsError()
{
    return error_code_ != RpcErrorCode::None;
}

RpcErrorCode RpcCall::ErrorCode() 
{
    return error_code_;
}

void RpcCall::StartTimer()
{
    if (!session_ || !timer_)
    {
        error_code_ = RpcErrorCode::InvalidRequest;
        Cancel();
        return;
    }
    is_cancel_.store(false, std::memory_order_release);
    timer_->expires_after(timeout_);
	timer_->async_wait([self = shared_from_this()](const Error_code& error) {
        if (self->is_cancel_.load(std::memory_order_acquire) || error == Asio::error::operation_aborted) {
            return;
        }
        if (self->finished_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        self->is_cancel_.store(true, std::memory_order_release);
        gb::NetworkManager::Instance()->RpcCancel(self->GetId());

		LOG_WARN("RPC timeout {}", self->id_);
		if (!error && self->timeout_func_) {
				self->DispatchCompletion(self->timeout_func_);
				self->error_code_ = RpcErrorCode::Timeout;
		}
	});
}

void RpcCall::DispatchCompletion(std::function<void()> cb) const
{
    if (!cb)
        return;
    WorkerPtr worker;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        worker = callback_worker_.lock();
    }
    if (worker)
    {
        auto cur_worker = WorkerManager::Instance()->GetCurWorker();
        if (!cur_worker || cur_worker->GetIndex() != worker->GetIndex())
        {
            worker->Post(std::move(cb));
            return;
        }
    }
    cb();
}

} // namespace gb
