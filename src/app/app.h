#pragma once
#include "types.h"
#include <memory>
#include <chrono>
#include <atomic>
#include <functional>
#include "async/shutdown.h"
#include "async/signal_handler.h"
#include "log/log.h"

namespace gb
{
class Worker;
class IoServicePool;
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

private:
    // 关闭阶段处理器
    void OnPhaseStoppingIO(gb::ShutdownManager::ShutdownPhase phase);
    void OnPhaseProcessingTasks(gb::ShutdownManager::ShutdownPhase phase);
    void OnPhaseCompletingTimers(gb::ShutdownManager::ShutdownPhase phase);
    void OnPhaseCleanup(gb::ShutdownManager::ShutdownPhase phase);
};
