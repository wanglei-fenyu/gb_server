#include "app.h"
#include "worker/worker_manager.h"
#include "async/signal_handler.h"
#include "async/shutdown.h"
#include "network/io/io_service_pool.h"
#include "network/manager/network_manager.h"
#include "timer/timer_manager.h"
#include "async/thread_pool_scheduler.h"
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
    // 主线程系统定时器
    sys_timer_mgr_ = std::make_unique<gb::TimerManager>();

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
    // 初始化抢占式线程池调度器（Worker 可通过 Dispatch / Schedule 使用）
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
    
    // ── 0. 先 Drain ThreadPool —— 确保所有重度任务完成，回调已投递到 Worker 队列 ──
    //     顺序很重要：ThreadPool 先于 Worker，否则 Worker 收不到 ThreadPool 的回调。
    LOG_INFO("Draining ThreadPool (pending: {})", gb::ThreadPoolScheduler::Instance()->PendingCount());
    gb::ThreadPoolScheduler::Instance()->Stop();
    LOG_INFO("ThreadPool drained");

    // 所有 Normal Worker 进入关闭模式
    auto workers = gb::WorkerManager::Instance()->GetWorkers();
    for (auto& worker : workers)
    {
        if (worker)
            worker->EnterShutdownMode();
    }
    
    // 主线程事件队列不再接受新任务（Run 已退出，runding_=false）
    // 不再对 main_worker 调用 EnterShutdownMode，主线程不跑业务帧

    // 处理剩余的主线程事件
    ProcessMainThreadEvents();
    
    // 等待 Normal Worker 待处理任务处理完毕（最长 5 秒）
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
    
    // 主线程系统定时器关闭
    if (sys_timer_mgr_)
        sys_timer_mgr_->EnterShutdownMode();
    
    // 所有 Normal Worker 定时器管理器进入关闭模式
    auto workers = gb::WorkerManager::Instance()->GetWorkers();
    for (auto& worker : workers)
    {
        if (worker && worker->GetTimerManager())
            worker->GetTimerManager()->EnterShutdownMode();
    }
    
    // 主线程执行最后一次定时器 Update + 事件处理
    LOG_INFO("Executing final timer frame");
    if (sys_timer_mgr_)
        sys_timer_mgr_->Update();
    ProcessMainThreadEvents();
    
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

    // 停止抢占式线程池，确保所有重度任务完成
    gb::ThreadPoolScheduler::Instance()->Stop();

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

    // Worker 线程由 WorkerManager 析构时 join
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

   // 不再在 Run() 中调用 main_worker->OnStartup()。
   // Worker 的 OnStartup 由各 Normal Worker 在 InitLua() 后自行调用。
   // 主线程不再充当 MainWorker，只做管理操作。

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

        // ── 1. 系统定时器（续期、健康检查、负载上报等）──
        if (sys_timer_mgr_)
            sys_timer_mgr_->Update();

        // ── 2. 处理主线程事件队列（Bind/Unbind 路由变更等）──
        ProcessMainThreadEvents();

        // ── 2.5 冻结实体路由表 —— Bind/Unbind 变更对 IO/Worker 线程可见 ──
        gb::NetworkManager::Instance()->GetRouter().FreezeEntityRoutes();

        // ── 3. 主线程管理帧（OnUpdate/OnTick 不再包含业务逻辑）──
        if (OnUpdate(elapsed.count()) != 0)
            break;

        // 主线程不再调用 main_worker->ProcessFrame() —— 不在此线程跑业务
        // 主要通过 sys_timer_mgr_ + main_thread_events_ 处理管理操作

	    if (OnTick() != 0)
		    break;

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

void App::ProcessMainThreadEvents()
{
    // 1. Drain App 本身的 main_thread_events_
    std::function<void()> task;
    while (main_thread_events_.try_dequeue(task))
    {
        if (task)
            task();
    }

    // 2. Drain main_worker 的事件队列（兼容 PostToMain 等投递到 main_worker 的请求）
    auto main_worker = gb::WorkerManager::Instance()->GetMainWorker();
    if (main_worker)
    {
        std::function<void()> mtask;
        while (main_worker->TryDequeueEvent(mtask))
        {
            if (mtask)
                mtask();
        }
    }
}

void App::PostToMain(std::function<void()>&& handler)
{
    if (handler)
        main_thread_events_.enqueue(std::move(handler));
}

void App::PostToMain(const std::function<void()>& handler)
{
    if (handler)
    {
        std::function<void()> copy = handler;
        main_thread_events_.enqueue(std::move(copy));
    }
}
