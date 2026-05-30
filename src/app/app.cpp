#include "app.h"
#include "common/worker/worker_manager.h"
#include "common/signal/signal_handler.h"
#include "common/shutdown/shutdown_manager.h"
#include "network/io_service_pool/io_service_pool.h"
#include <thread>
#include "common/res_path.h"

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
    
    // Initialize shutdown manager
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
    // Stop IO service pool from accepting new messages
    if (io_service_pool_)
    {
        io_service_pool_->GracefulStop();
    }
    LOG_INFO("IO threads stopped, proceeding to next phase");
    // Move to next phase after brief delay
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (shutdown_manager_)
        shutdown_manager_->NextPhase();
}

void App::OnPhaseProcessingTasks(gb::ShutdownManager::ShutdownPhase phase)
{
    LOG_INFO("=== Phase 2: Processing pending tasks ===");
    
    // Enter shutdown mode for all workers
    auto workers = gb::WorkerManager::Instance()->GetWorkers();
    for (auto& worker : workers)
    {
        if (worker)
            worker->EnterShutdownMode();
    }
    
    auto main_worker = gb::WorkerManager::Instance()->GetMainWorker();
    if (main_worker)
        main_worker->EnterShutdownMode();
    
    // Wait for pending tasks to be processed (max 5 seconds)
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
    LOG_INFO("=== Phase 3: Completing current timer frame ===");
    
    // Enter shutdown mode for all timer managers (cancel future timers, complete current frame)
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
    
    // Execute one more frame to complete current timers
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

    log.UnInit();
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
