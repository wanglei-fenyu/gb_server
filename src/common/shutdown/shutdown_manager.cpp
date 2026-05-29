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
    on_stop_io_ = std::move(on_stop_io);
    on_process_tasks_ = std::move(on_process_tasks);
    on_complete_timers_ = std::move(on_complete_timers);
    on_cleanup_ = std::move(on_cleanup);
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
    {
        std::unique_lock<std::mutex> lock(phase_mutex_);
        AdvancePhase();
    }
    phase_cv_.notify_all();
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
    ShutdownPhase current = current_phase_.load();
    ShutdownPhase next = current;

    switch (current)
    {
        case ShutdownPhase::Normal:
            next = ShutdownPhase::StoppingIO;
            LOG_INFO("Entering Phase 1: StoppingIO - stopping IO threads from accepting new messages");
            if (on_stop_io_)
                on_stop_io_(next);
            break;

        case ShutdownPhase::StoppingIO:
            next = ShutdownPhase::ProcessingTasks;
            LOG_INFO("Entering Phase 2: ProcessingTasks - processing all pending tasks");
            if (on_process_tasks_)
                on_process_tasks_(next);
            break;

        case ShutdownPhase::ProcessingTasks:
            next = ShutdownPhase::CompletingTimers;
            LOG_INFO("Entering Phase 3: CompletingTimers - completing current timer frame (cancelling others)");
            if (on_complete_timers_)
                on_complete_timers_(next);
            break;

        case ShutdownPhase::CompletingTimers:
            next = ShutdownPhase::Cleaning;
            LOG_INFO("Entering Phase 4: Cleaning - cleaning up resources");
            if (on_cleanup_)
                on_cleanup_(next);
            break;

        case ShutdownPhase::Cleaning:
            next = ShutdownPhase::Done;
            LOG_INFO("Shutdown complete");
            break;

        case ShutdownPhase::Done:
            // Already done, do nothing
            break;
    }

    current_phase_.store(next);
}

} // namespace gb
