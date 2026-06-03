#include "app.h"
#include "worker/worker_manager.h"
#include "async/signal_handler.h"
#include "async/shutdown.h"
#include "network/io/io_service_pool.h"
#include "network/manager/network_manager.h"
#include <thread>
#include "base/res_path.h"
#include "db/redis/register_redis.h"


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
	log.Init(ResPath::Instance()->FindResPath("log4/test.log").c_str(), 1024 * 1024 * 1000, 10,
             GbLog::ASYNC, GbLog::CONSOLE_AND_FILE, GbLog::LEVEL_INFO);

    gb::WorkerManager::Instance()->InitMainWorker();
    
    // 初始化关闭管理器
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
    // 停止 IO 服务池，不再接受新消息
    if (io_service_pool_)
    {
        io_service_pool_->GracefulStop();
    }
    LOG_INFO("IO threads stopped, proceeding to next phase");
    // 短暂延迟后进入下一阶段
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (shutdown_manager_)
        shutdown_manager_->NextPhase();
}

void App::OnPhaseProcessingTasks(gb::ShutdownManager::ShutdownPhase phase)
{
    LOG_INFO("=== Phase 3: Processing pending tasks ===");
    
    // 所有 Worker 进入关闭模式
    auto workers = gb::WorkerManager::Instance()->GetWorkers();
    for (auto& worker : workers)
    {
        if (worker)
            worker->EnterShutdownMode();
    }
    
    auto main_worker = gb::WorkerManager::Instance()->GetMainWorker();
    if (main_worker)
        main_worker->EnterShutdownMode();
    
    // 等待待处理任务处理完毕（最长 5 秒）
    auto start_time = std::chrono::steady_clock::now();
    bool all_empty = false;
    
    while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(5))
    {
        all_empty = true;
        for (auto& worker : workers)
        {
            if (worker && worker->GetPendingTaskCount() > 0)
            {
                all_empty = false;
                break;
            }
        }
        
        if (main_worker && main_worker->GetPendingTaskCount() > 0)
            all_empty = false;
        
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
    
    // 所有定时器管理器进入关闭模式（取消未来定时器，完成当前帧）
    auto workers = gb::WorkerManager::Instance()->GetWorkers();
    for (auto& worker : workers)
    {
        if (worker && worker->GetTimerManager())
        {
            worker->GetTimerManager()->EnterShutdownMode();
        }
    }
    
    auto main_worker = gb::WorkerManager::Instance()->GetMainWorker();
    if (main_worker && main_worker->GetTimerManager())
    {
        main_worker->GetTimerManager()->EnterShutdownMode();
    }
    
    // 再执行一帧以完成当前定时器
    LOG_INFO("Executing final timer frame");
    auto last_time = std::chrono::steady_clock::now();
    auto current_time = std::chrono::steady_clock::now();
    std::chrono::duration<float> elapsed = current_time - last_time;
    
    if (main_worker)
    {
        main_worker->ProcessFrame(elapsed.count());
    }
    
    LOG_INFO("Current timer frame completed");
    
    if (shutdown_manager_)
        shutdown_manager_->NextPhase();
}

void App::OnPhaseCleanup(gb::ShutdownManager::ShutdownPhase phase)
{
    LOG_INFO("=== Phase 4: Cleaning up resources ===");
    
    auto main_worker = gb::WorkerManager::Instance()->GetMainWorker();
    if (0 != OnCleanup())
    {
        LOG_ERROR("OnCleanup failed");
    }

    // 在 Worker 线程 join 前关闭 Redis 连接池（释放 IO 线程）
    CloseRedisPool();

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

    auto& worker_threads = gb::WorkerManager::Instance()->GetThreads();
    for (auto& thread : worker_threads)
    {
        if (thread.joinable())
            thread.join();
    }

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

   main_worker->OnStartup();

   // 将 NetworkManager 的处理器映射冻结为只读原子指针。
   // 此后，FindListenFunction 和 FindRpcFunction 变为无锁访问。
   // 每个 worker 的 thread_local RPC 待处理映射处理所有响应路由。
   // 必须在所有 InitLua() 和 Register/Listen 调用完成后调用。
   gb::NetworkManager::Instance()->Freeze();

    auto last_time = std::chrono::steady_clock::now();
    while (runding_)
    {
        if (gb::SignalHandler::IsSignalReceived())
            break;
        auto                         current_time = std::chrono::steady_clock::now();
        std::chrono::duration<float> elapsed      = current_time - last_time;
        last_time                                 = current_time;
        if (OnUpdate(elapsed.count()) != 0)
        {
            break;
        }

        main_worker->ProcessFrame(elapsed.count());

	    if (OnTick() != 0)
	    {
		   break;
	    }

        auto frame_end_time = std::chrono::steady_clock::now();
        auto frame_time     = frame_end_time - current_time;

        if (frame_time < frame_duration_)
        {
            std::this_thread::sleep_for(frame_duration_ - frame_time);
        }
    }

    LOG_INFO("Main loop exited, starting graceful shutdown");
    
    if (shutdown_manager_)
    {
        if (!shutdown_manager_->IsShuttingDown())
            shutdown_manager_->Shutdown();
        shutdown_manager_->WaitForShutdown();
    }
}
