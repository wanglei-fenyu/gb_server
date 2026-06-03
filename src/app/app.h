#pragma once
#include "types.h"
#include <memory>
#include <chrono>
#include <atomic>
#include <functional>
#include "async/shutdown.h"
#include "async/signal_handler.h"
#include "log/log.h"
#include "concurrentqueue.h"

namespace gb
{
class Worker;
class IoServicePool;
class TimerManager;
}

class App{
public:
    App(int argc, char* argv[]);
          virtual ~App();
	APP_TYPE GetAppType() { return appType_; }
    void SetFrameRate(int fps);
    int Init();
    void Stop();
    void Run();

    /// 投递任务到主线程事件队列（Bind/Unbind 等管理操作）
    void PostToMain(std::function<void()>&& handler);
    void PostToMain(const std::function<void()>& handler);

protected:
	virtual int OnInit() = 0;
    virtual int OnStartup() = 0;
    virtual int OnUpdate(float) = 0;
    virtual int OnTick() = 0;
    virtual int OnCleanup() = 0;
    virtual int OnUnInit() = 0;

protected:
	APP_TYPE appType_;
private:
    std::atomic<bool> runding_;
    std::chrono::milliseconds frame_duration_;
    std::unique_ptr<gb::SignalHandler> signal_handler_;
    std::shared_ptr<gb::ShutdownManager> shutdown_manager_;
    std::shared_ptr<gb::IoServicePool> io_service_pool_;
    GbLog                log;

    /// 主线程系统定时器（续期、健康检查、负载上报等，不走 Worker 的 TimerManager）
    std::unique_ptr<gb::TimerManager> sys_timer_mgr_;

    /// 主线程事件队列 —— 接收外部的 Bind/Unbind 等管理请求
    moodycamel::ConcurrentQueue<std::function<void()>> main_thread_events_;

private:
    // 关闭阶段处理器
    void OnPhaseStoppingIO(gb::ShutdownManager::ShutdownPhase phase);
    void OnPhaseProcessingTasks(gb::ShutdownManager::ShutdownPhase phase);
    void OnPhaseCompletingTimers(gb::ShutdownManager::ShutdownPhase phase);
    void OnPhaseCleanup(gb::ShutdownManager::ShutdownPhase phase);

    /// 处理主线程事件队列中的任务，每帧调用
    void ProcessMainThreadEvents();
};
