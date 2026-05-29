#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <chrono>

namespace gb
{

/**
 * Graceful shutdown manager to coordinate orderly shutdown of all threads
 * Sequence:
 *   1. Stop accepting new connections (IO threads)
 *   2. Process all pending tasks in workers
 *   3. Complete current timer frame (cancel others)
 *   4. Cleanup resources in reverse order (workers, IO threads, main thread)
 */
class ShutdownManager
{
public:
    enum class ShutdownPhase
    {
        Normal = 0,           // Normal operation
        StoppingIO = 1,       // IO threads stop accepting, complete pending operations
        ProcessingTasks = 2,  // Workers process all pending tasks
        CompletingTimers = 3, // Timers complete current frame (cancel others)
        Cleaning = 4,         // All threads cleanup and exit
        Done = 5              // Shutdown complete
    };

    using ShutdownCallback = std::function<void(ShutdownPhase phase)>;

public:
    ShutdownManager() = default;
    ~ShutdownManager() = default;

    /// Initialize with callbacks for each shutdown phase
    void Initialize(
        ShutdownCallback on_stop_io,
        ShutdownCallback on_process_tasks,
        ShutdownCallback on_complete_timers,
        ShutdownCallback on_cleanup
    );

    /// Trigger graceful shutdown
    void Shutdown();

    /// Check current shutdown phase
    ShutdownPhase GetPhase() const;

    /// Check if shutdown is requested
    bool IsShuttingDown() const;

    /// Check if specific phase is active
    bool IsInPhase(ShutdownPhase phase) const;

    /// Wait for shutdown to complete
    bool WaitForShutdown(int timeout_ms = -1);

    /// Proceed to next phase (typically called by shutdown handlers)
    void NextPhase();

    /// Force immediate shutdown (skip remaining phases)
    void ForceShutdown();

private:
    mutable std::mutex phase_mutex_;
    std::condition_variable phase_cv_;
    std::atomic<ShutdownPhase> current_phase_{ShutdownPhase::Normal};
    std::atomic<bool> shutdown_requested_{false};

    ShutdownCallback on_stop_io_;
    ShutdownCallback on_process_tasks_;
    ShutdownCallback on_complete_timers_;
    ShutdownCallback on_cleanup_;

    void AdvancePhase();
};

using ShutdownManagerPtr = std::shared_ptr<ShutdownManager>;

} // namespace gb
