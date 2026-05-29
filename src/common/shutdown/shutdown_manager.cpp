#include "shutdown_manager.h"
#include "log/log_help.h"

namespace gb
{

void ShutdownManager::Initialize(
    ShutdownCallback on_stop_io,
    ShutdownCallback on_process_tasks,
    ShutdownCallback on_complete_timers,
    ShutdownCallback on_cleanup)
{
    on_stop_io_         = std::move(on_stop_io);
    on_process_tasks_   = std::move(on_process_tasks);
    on_complete_timers_ = std::move(on_complete_timers);
    on_cleanup_         = std::move(on_cleanup);
}

void ShutdownManager::Shutdown()
{
    bool expected = false;
    if (!shutdown_requested_.compare_exchange_strong(expected, true))
    {
        return; // Already shutting down
    }
    LOG_INFO("Graceful shutdown initiated");
    AdvancePhase();
}

ShutdownManager::ShutdownPhase ShutdownManager::GetPhase() const
{
    return current_phase_.load();
}

bool ShutdownManager::IsShuttingDown() const
{
    return shutdown_requested_.load();
}

bool ShutdownManager::IsInPhase(ShutdownPhase phase) const
{
    return current_phase_.load() == phase;
}

bool ShutdownManager::WaitForShutdown(int timeout_ms)
{
    std::unique_lock<std::mutex> lock(phase_mutex_);

    if (timeout_ms < 0)
    {
        // Wait indefinitely
        phase_cv_.wait(lock, [this]() { return current_phase_.load() == ShutdownPhase::Done; });
        return true;
    }
    else
    {
        // Wait with timeout
        return phase_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [this]() { return current_phase_.load() == ShutdownPhase::Done; });
    }
}

void ShutdownManager::NextPhase()
{
    AdvancePhase();
}

void ShutdownManager::ForceShutdown()
{
    LOG_WARN("Force shutdown triggered");
    {
        std::unique_lock<std::mutex> lock(phase_mutex_);
        current_phase_.store(ShutdownPhase::Done);
    }
    phase_cv_.notify_all();
}

void ShutdownManager::AdvancePhase()
{
    // 防止递归调用，但记录需要再次推进
    if (is_advancing_.exchange(true))
    {
        // 已经在 AdvancePhase 中，标记需要再次推进
        pending_advance_.store(true);
        LOG_DEBUG("AdvancePhase already in progress, will retry later");
        return;
    }

    // RAII 自动重置标志，并处理待处理的推进
    struct ResetGuard
    {
        std::atomic<bool>& flag;
        std::atomic<bool>& pending;
        ResetGuard(std::atomic<bool>& f, std::atomic<bool>& p) :
            flag(f), pending(p) {}
        ~ResetGuard()
        {
            flag.store(false);
            // 如果有待处理的推进请求，在退出前执行
            if (pending.exchange(false))
            {
                // 注意：这里不能直接调用 AdvancePhase，需要让外部循环来处理
                // 所以只是清除标志，由外层循环处理
            }
        }
    } guard{is_advancing_, pending_advance_};

    // 循环执行，直到没有待处理的推进
    bool has_pending = false;
    do {
        has_pending = false;

        ShutdownPhase current = current_phase_.load();
        ShutdownPhase next    = current;

        switch (current)
        {
            case ShutdownPhase::Normal:
                next = ShutdownPhase::StoppingIO;
                LOG_INFO("Entering Phase 1: StoppingIO - stopping IO threads from accepting new messages");
                break;
            case ShutdownPhase::StoppingIO:
                next = ShutdownPhase::ProcessingTasks;
                LOG_INFO("Entering Phase 2: ProcessingTasks - processing all pending tasks");
                break;
            case ShutdownPhase::ProcessingTasks:
                next = ShutdownPhase::CompletingTimers;
                LOG_INFO("Entering Phase 3: CompletingTimers - completing current timer frame (cancelling others)");
                break;
            case ShutdownPhase::CompletingTimers:
                next = ShutdownPhase::Cleaning;
                LOG_INFO("Entering Phase 4: Cleaning - cleaning up resources");
                break;
            case ShutdownPhase::Cleaning:
                next = ShutdownPhase::Done;
                LOG_INFO("Shutdown complete");
                break;
            case ShutdownPhase::Done:
                return;
        }

        // 先更新状态
        current_phase_.store(next);

        // 通知等待者
        phase_cv_.notify_all();

        // 调用回调（回调中可能会再次调用 NextPhase，但会被上面的递归检查拦截）
        switch (current)
        {
            case ShutdownPhase::Normal:
                if (on_stop_io_)
                    on_stop_io_(next);
                break;
            case ShutdownPhase::StoppingIO:
                if (on_process_tasks_)
                    on_process_tasks_(next);
                break;
            case ShutdownPhase::ProcessingTasks:
                if (on_complete_timers_)
                    on_complete_timers_(next);
                break;
            case ShutdownPhase::CompletingTimers:
                if (on_cleanup_)
                    on_cleanup_(next);
                break;
            default:
                break;
        }

        // 检查在回调执行期间是否有新的推进请求
        has_pending = pending_advance_.exchange(false);

    } while (has_pending); // 如果有待处理的推进，继续循环
}

} // namespace gb