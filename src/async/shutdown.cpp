#include "shutdown.h"
#include "log/log.h"

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
        return; // 宸插浜庡叧闂姸鎬?
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
        // 鏃犻檺绛夊緟
        phase_cv_.wait(lock, [this]() { return current_phase_.load() == ShutdownPhase::Done; });
        return true;
    }
    else
    {
        // 甯﹁秴鏃剁瓑寰?
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
    // 闃叉閫掑綊璋冪敤锛屼絾璁板綍闇€瑕佸啀娆℃帹杩?
    if (is_advancing_.exchange(true))
    {
        // 宸茬粡鍦?AdvancePhase 涓紝鏍囪闇€瑕佸啀娆℃帹杩?
        pending_advance_.store(true);
        LOG_DEBUG("AdvancePhase already in progress, will retry later");
        return;
    }

    // RAII 鑷姩閲嶇疆鏍囧織锛屽苟澶勭悊寰呭鐞嗙殑鎺ㄨ繘
    struct ResetGuard
    {
        std::atomic<bool>& flag;
        std::atomic<bool>& pending;
        ResetGuard(std::atomic<bool>& f, std::atomic<bool>& p) :
            flag(f), pending(p) {}
        ~ResetGuard()
        {
            flag.store(false);
            // 濡傛灉鏈夊緟澶勭悊鐨勬帹杩涜姹傦紝鍦ㄩ€€鍑哄墠鎵ц
            if (pending.exchange(false))
            {
                // 娉ㄦ剰锛氳繖閲屼笉鑳界洿鎺ヨ皟鐢?AdvancePhase锛岄渶瑕佽澶栭儴寰幆鏉ュ鐞?
                // 鎵€浠ュ彧鏄竻闄ゆ爣蹇楋紝鐢卞灞傚惊鐜鐞?
            }
        }
    } guard{is_advancing_, pending_advance_};

    // 寰幆鎵ц锛岀洿鍒版病鏈夊緟澶勭悊鐨勬帹杩?
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
                next = ShutdownPhase::CompletingTimers;
                LOG_INFO("Entering Phase 2: CompletingTimers - completing current timer frame (cancelling others)");
                break;
            case ShutdownPhase::CompletingTimers:
                next = ShutdownPhase::ProcessingTasks;
                LOG_INFO("Entering Phase 3: ProcessingTasks - processing all pending tasks");
                break;
            case ShutdownPhase::ProcessingTasks:
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

        // 鍏堟洿鏂扮姸鎬?
        current_phase_.store(next);

        // 閫氱煡绛夊緟鑰?
        phase_cv_.notify_all();

        // 璋冪敤鍥炶皟锛堝洖璋冧腑鍙兘浼氬啀娆¤皟鐢?NextPhase锛屼絾浼氳涓婇潰鐨勯€掑綊妫€鏌ユ嫤鎴級
        switch (current)
        {
            case ShutdownPhase::Normal:
                if (on_stop_io_)
                    on_stop_io_(next);
                break;
            case ShutdownPhase::StoppingIO:
                if (on_complete_timers_)
                    on_complete_timers_(next);
                break;
            case ShutdownPhase::CompletingTimers:
                if (on_process_tasks_)
                    on_process_tasks_(next);
                break;
            case ShutdownPhase::ProcessingTasks:
                if (on_cleanup_)
                    on_cleanup_(next);
                break;
            default:
                break;
        }

        // 妫€鏌ュ湪鍥炶皟鎵ц鏈熼棿鏄惁鏈夋柊鐨勬帹杩涜姹?
        has_pending = pending_advance_.exchange(false);

    } while (has_pending); // 濡傛灉鏈夊緟澶勭悊鐨勬帹杩涳紝缁х画寰幆
}

} // namespace gb