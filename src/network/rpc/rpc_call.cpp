#include "rpc_call.h"
#include "base/timer_help.h"
#include "log/log.h"
#include "worker/worker.h"

namespace gb
{
RpcCall::RpcCall() :
    id_(0), worker_index_(0), local_seq_(0), timeout_(std::chrono::milliseconds(kRpcdefaultTimeout)),
    timeout_func_(nullptr), cancel_func_(nullptr), state_(RpcState::Pending),
    session_(nullptr), done_call_bcak_(nullptr), timer_(std::nullopt)
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

    // 浠庣嚎绋嬫湰鍦版寕璧锋槧灏勪腑绉婚櫎锛圕ancel蹇呴』鍦ㄦ墍灞濿orker绾跨▼涓皟鐢級
    Worker::TakePendingRpc(local_seq_);

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
    // 鐩存帴鍦ㄥ綋鍓峎orker绾跨▼涓婅皟鐢ㄥ洖璋冦€?
    // 涓嶉渶瑕丏ispatchCompletion 鈥?Done()濮嬬粓浠庢墍灞濿orker璋冪敤
    // 锛圛O绾跨▼灏嗗搷搴旀姇閫掑埌姝orker锛學orker鍦ㄧ嚎绋嬫湰鍦版槧灏勪笂璋冪敤TakePendingRpc锛?
    //  鐒跺悗鍐呰仈璋冪敤Done()锛夈€?
    //
    // 鍥炶皟锛堜緥濡侻akeRpcHandler鐨刲ambda锛夎皟鐢╯tate->Complete(value)
    // 鎴杝tate->Resume()锛屽悗鑰呰皟鐢∟otifyCompleted() 鈫?executor_.schedule([h](){h.resume();})銆?
    // 鐢变簬executor_鏄粦瀹氬埌鍚屼竴Worker鐨刉orkerExecutor锛屽叾IsCurrent()
    // 杩斿洖true锛屽洜姝.resume()鍐呰仈鎵ц 鈥?闆朵笂涓嬫枃鍒囨崲銆?
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
    if (!session_ || !timer_)
    {
        RpcState expected = RpcState::Pending;
        if (!state_.compare_exchange_strong(expected, RpcState::InvalidRequest, std::memory_order_acq_rel, std::memory_order_acquire))
            return;
        // 娓呯悊锛氫粠鎵€灞濿orker鐨勭嚎绋嬫湰鍦版槧灏勪腑绉婚櫎骞惰皟鐢ㄥ彇娑堝洖璋?
        Worker::TakePendingRpc(local_seq_);
        std::function<void()> cancel_callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            cancel_callback = cancel_func_;
        }
        Finish(std::move(cancel_callback));
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

        // 瓒呮椂鍦↖O绾跨▼涓婅Е鍙?鈥?鏃犳硶鐩存帴璁块棶Worker鐨勭嚎绋嬫湰鍦版槧灏勩€?
        // 鎶曢€掑埌鎵€灞濿orker浠ヤ究瀹夊叏鍦颁粠鍏舵寕璧锋槧灏勪腑绉婚櫎銆?
        auto worker = WorkerManager::Instance()->GetWorker(self->worker_index_);
        if (!worker)
            return;

        worker->Post([self]() {
            // 浠庣嚎绋嬫湰鍦版槧灏勪腑绉婚櫎锛堝湪鎵€灞濿orker绾跨▼涓婂畨鍏ㄦ墽琛岋級
            Worker::TakePendingRpc(self->local_seq_);

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
    if (timer_ && timer_->expiry() > std::chrono::steady_clock::now())
        timer_->cancel();
}

} // namespace gb
