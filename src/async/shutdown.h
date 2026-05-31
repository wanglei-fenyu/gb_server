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
 * 优雅关闭管理器，协调所有线程的有序关闭
 * 顺序：
 *   1. 停止接受新连接（IO线程）
 *   2. 完成当前定时器帧（取消其他）
 *   3. 处理worker中所有待处理任务
 *   4. 按逆序清理资源（worker、IO线程、主线程）
 */
class ShutdownManager
{
public:
    enum class ShutdownPhase
    {
        Normal           = 0, // 正常运行
        StoppingIO       = 1, // IO线程停止接收，完成待处理操作
        CompletingTimers = 2, // 定时器完成当前帧（取消其他）
        ProcessingTasks  = 3, // Worker处理所有待处理任务
        Cleaning         = 4, // 所有线程清理并退出
        Done             = 5  // 关闭完成
    };

    using ShutdownCallback = std::function<void(ShutdownPhase phase)>;

public:
    ShutdownManager()  = default;
    ~ShutdownManager() = default;

    /// 用每个关闭阶段的回调进行初始化
    void Initialize(
        ShutdownCallback on_stop_io,
        ShutdownCallback on_process_tasks,
        ShutdownCallback on_complete_timers,
        ShutdownCallback on_cleanup);

    /// 触发优雅关闭
    void Shutdown();

    /// 检查当前关闭阶段
    ShutdownPhase GetPhase() const;

    /// 检查是否已请求关闭
    bool IsShuttingDown() const;

    /// 检查特定阶段是否激活
    bool IsInPhase(ShutdownPhase phase) const;

    /// 等待关闭完成
    bool WaitForShutdown(int timeout_ms = -1);

    /// 进入下一阶段（通常由关闭处理器调用）
    void NextPhase();

    /// 强制立即关闭（跳过剩余阶段）
    void ForceShutdown();

private:
private:
    mutable std::mutex         phase_mutex_;
    std::condition_variable    phase_cv_;
    std::atomic<ShutdownPhase> current_phase_{ShutdownPhase::Normal};
    std::atomic<bool>          shutdown_requested_{false};
    std::atomic<bool>          is_advancing_{false};
    std::atomic<bool>          pending_advance_{false}; // 新增：是否有待处理的推进请求

    ShutdownCallback on_stop_io_;
    ShutdownCallback on_process_tasks_;
    ShutdownCallback on_complete_timers_;
    ShutdownCallback on_cleanup_;

    void AdvancePhase();
};

using ShutdownManagerPtr = std::shared_ptr<ShutdownManager>;

} // namespace gb