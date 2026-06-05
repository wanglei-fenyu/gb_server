#include "app.h"
#include "worker/worker_manager.h"
#include "async/signal_handler.h"
#include "async/shutdown.h"
#include "network/io/io_service_pool.h"
#include "network/manager/network_manager.h"
#include "async/thread_pool_scheduler.h"
#include <thread>
#include "base/res_path.h"
#include "db/redis/register_redis.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


App::App(int argc, char* argv[])  : runding_(false), frame_duration_(std::chrono::milliseconds(16))
{
}

App::~App()
{
    log.UnInit();
}

void App::SetFrameRate(int fps)
{
    if (fps > 0)
    {
        frame_duration_ = std::chrono::milliseconds(1000 / fps);
    }
}


int App::Init()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
	log.Init(ResPath::Instance()->FindResPath("log4/test.log").c_str(), 1024 * 1024 * 1000, 10,
             GbLog::ASYNC, GbLog::CONSOLE_AND_FILE, GbLog::LEVEL_INFO);

    gb::WorkerManager::Instance()->InitMainWorker();

    shutdown_manager_ = std::make_shared<gb::ShutdownManager>();
    shutdown_manager_->Initialize(
        [this](gb::ShutdownManager::ShutdownPhase phase) { OnPhaseStoppingIO(phase); },
        [this](gb::ShutdownManager::ShutdownPhase phase) { OnPhaseProcessingTasks(phase); },
        [this](gb::ShutdownManager::ShutdownPhase phase) { OnPhaseCompletingTimers(phase); },
        [this](gb::ShutdownManager::ShutdownPhase phase) { OnPhaseCleanup(phase); }
    );

    signal_handler_ = std::make_unique<gb::SignalHandler>();
    if (!signal_handler_->Initialize([this](gb::SignalHandler::SignalType) {
        Stop();
        if (shutdown_manager_)
            shutdown_manager_->Shutdown();
    }))
        return -1;
    gb::ThreadPoolScheduler::Instance()->Init();

    if (OnInit() != 0) return -1;
    runding_ = true;
    return 0;
}

void App::Stop()
{
    runding_ = false;
}

void App::OnPhaseStoppingIO(gb::ShutdownManager::ShutdownPhase phase)
{
    LOG_INFO("=== Phase 1: Stopping IO threads ===");
    if (io_service_pool_)
    {
        io_service_pool_->GracefulStop();
    }
    LOG_INFO("IO threads stopped, proceeding to next phase");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (shutdown_manager_)
        shutdown_manager_->NextPhase();
}

void App::OnPhaseProcessingTasks(gb::ShutdownManager::ShutdownPhase phase)
{
    LOG_INFO("=== Phase 3: Processing pending tasks ===");

    LOG_INFO("Draining ThreadPool (pending: {})", gb::ThreadPoolScheduler::Instance()->PendingCount());
    gb::ThreadPoolScheduler::Instance()->Stop();
    LOG_INFO("ThreadPool drained");

    // 所有 Worker 进入关闭模式
    auto main_worker = gb::WorkerManager::Instance()->GetMainWorker();
    if (main_worker)
        main_worker->EnterShutdownMode();
    auto workers = gb::WorkerManager::Instance()->GetWorkers();
    for (auto& worker : workers)
    {
        if (worker)
            worker->EnterShutdownMode();
    }

    // 等待所有 Worker 待处理任务完毕（最长 5 秒）
    auto start_time = std::chrono::steady_clock::now();
    bool all_empty = false;

    while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(5))
    {
        all_empty = true;
        if (main_worker && main_worker->GetPendingTaskCount() > 0)
            all_empty = false;
        for (auto& worker : workers)
        {
            if (worker && worker->GetPendingTaskCount() > 0)
            {
                all_empty = false;
                break;
            }
        }

        if (all_empty)
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (all_empty)
        LOG_INFO("All pending tasks processed");
    else
        LOG_WARN("Task processing timeout, moving to next phase");

    if (shutdown_manager_)
        shutdown_manager_->NextPhase();
}

void App::OnPhaseCompletingTimers(gb::ShutdownManager::ShutdownPhase phase)
{
    LOG_INFO("=== Phase 2: Completing current timer frame ===");

    auto main_worker = gb::WorkerManager::Instance()->GetMainWorker();

    // 所有 Worker 定时器进入关闭模式
    if (main_worker && main_worker->GetTimerManager())
        main_worker->GetTimerManager()->EnterShutdownMode();
    auto workers = gb::WorkerManager::Instance()->GetWorkers();
    for (auto& worker : workers)
    {
        if (worker && worker->GetTimerManager())
            worker->GetTimerManager()->EnterShutdownMode();
    }

    // 所有 Worker 执行最后一次 ProcessFrame（定时器回调 + 剩余任务）
    LOG_INFO("Executing final timer frame");
    if (main_worker)
        main_worker->ProcessFrame(0.0f);
    for (auto& worker : workers)
    {
        if (worker)
            worker->ProcessFrame(0.0f);
    }
    LOG_INFO("Current timer frame completed");

    if (shutdown_manager_)
        shutdown_manager_->NextPhase();
}

void App::OnPhaseCleanup(gb::ShutdownManager::ShutdownPhase phase)
{
    LOG_INFO("=== Phase 4: Cleaning up resources ===");

    if (0 != OnCleanup())
    {
        LOG_ERROR("OnCleanup failed");
    }

    gb::ThreadPoolScheduler::Instance()->Stop();
    CloseRedisPool();

    auto main_worker = gb::WorkerManager::Instance()->GetMainWorker();
    LOG_INFO("Cleaning up worker threads");
    auto workers = gb::WorkerManager::Instance()->GetWorkers();
    for (auto& worker : workers)
    {
        if (!worker)
            continue;
        if (!worker->CleanupInWorkerThread(5000))
        {
            LOG_WARN("Worker {} cleanup timeout, forcing stop", worker->GetIndex());
            worker->Stop();
        }
    }

    // 清理 main_worker（由 WorkerManager 析构时 join）
    if (main_worker)
        main_worker->OnCleanup();

    OnUnInit();

    if (signal_handler_)
        signal_handler_->Cleanup();

    LOG_INFO("All resources cleaned up, shutdown complete");

    if (shutdown_manager_)
        shutdown_manager_->NextPhase();
}

void App::Run()
{
    auto main_worker = gb::WorkerManager::Instance()->GetMainWorker();
    if (!main_worker)
        return;

    if (0 != OnStartup())
    {
        return;
    }

    gb::NetworkManager::Instance()->Freeze();

    auto last_time = std::chrono::steady_clock::now();
    while (runding_)
    {
        if (gb::SignalHandler::IsSignalReceived())
            break;
        auto                         current_time = std::chrono::steady_clock::now();
        std::chrono::duration<float> elapsed      = current_time - last_time;
        last_time                                 = current_time;

        // ── 1. 冻结实体路由表 ──
        gb::NetworkManager::Instance()->GetRouter().FreezeEntityRoutes();

        // ── 2. 主线程管理帧（OnUpdate/OnTick 由子类实现）──
        if (OnUpdate(elapsed.count()) != 0)
            break;
        if (OnTick() != 0)
            break;

        // ── 3. 主线程 Worker 帧循环（处理 entity_id=0 的系统消息 + 定时器）──
        if (main_worker)
            main_worker->ProcessFrame(elapsed.count());

        // ── 4. 帧率控制 ──
        auto frame_end_time = std::chrono::steady_clock::now();
        auto frame_time     = frame_end_time - current_time;
        if (frame_time < frame_duration_)
            std::this_thread::sleep_for(frame_duration_ - frame_time);
    }

    LOG_INFO("Main loop exited, starting graceful shutdown");

    if (shutdown_manager_)
    {
        if (!shutdown_manager_->IsShuttingDown())
            shutdown_manager_->Shutdown();
        shutdown_manager_->WaitForShutdown();
    }
}
