#include "worker_manager.h"
#include "log/log_help.h"

namespace gb
{

namespace
{
// Per-thread pointer to the Worker currently executing on this thread.
// Set when a worker thread enters Run() and cleared when it exits.
thread_local WorkerPtr tl_current_worker;
} // namespace

 WorkerManager::WorkerManager()
{
    main_thread_id_ = std::this_thread::get_id();
}

 WorkerManager::~WorkerManager()
{
    for (auto& worker : workers)
        worker->Stop();
    for (auto& thread : worker_threads)
        if (thread.joinable())
            thread.join();
}

 WorkerPtr WorkerManager::CreateWorker(std::shared_ptr<IWorkerLogic> worker_logic)
{
	 WorkerPtr       work      = std::make_shared<Worker>();
	 work->SetWorkerLogic(worker_logic);
     size_t index = 0;
     {
         std::lock_guard<std::mutex> lock(mutex_);
         index = workers.size();
         workers.push_back(work);
     }
     worker_threads.emplace_back([work, index]() {
         std::thread::id id        = std::this_thread::get_id();
         uint32_t        thread_id = static_cast<uint32_t>(std::hash<std::thread::id>{}(id));
         work->Init(thread_id, index);
         tl_current_worker = work;
         work->OnStart();
         work->Run();
         tl_current_worker.reset();
     });
     return work;
 }

size_t WorkerManager::Size()
{
    return workers.size();
}

gb::WorkerPtr WorkerManager::GetWorker(size_t index) const
{
     if (index < workers.size())
         return workers[index];
	return nullptr;

}

gb::WorkerPtr WorkerManager::GetCurWorker() const
{
    return tl_current_worker;
}

std::vector<std::thread>& WorkerManager::GetThreads()
{
    return worker_threads;
}

std::vector<WorkerPtr>& WorkerManager::GetWorkers()
{
    return workers;
}

bool WorkerManager::PostToWorker(size_t index, const std::function<void()>& handler) const
{
    auto worker = GetWorker(index);
    if (!worker || !handler)
        return false;
    worker->Post(handler);
    return true;
}

bool WorkerManager::PostToWorker(size_t index, std::function<void()>&& handler) const
{
    auto worker = GetWorker(index);
    if (!worker || !handler)
        return false;
    worker->Post(std::move(handler));
    return true;
}

void WorkerManager::PostMain(const std::function<void()>& handler)
{
    if (!handler)
        return;
    main_events_.enqueue(handler);
}

void WorkerManager::PostMain(std::function<void()>&& handler)
{
    if (!handler)
        return;
    main_events_.enqueue(std::move(handler));
}

size_t WorkerManager::DrainMainQueue(size_t max_events)
{
    size_t               processed = 0;
    std::function<void()> fn;
    while (processed < max_events && main_events_.try_dequeue(fn))
    {
        if (fn)
            fn();
        ++processed;
    }
    return processed;
}

bool WorkerManager::IsMainThread() const
{
    return std::this_thread::get_id() == main_thread_id_;
}

void WorkerManager::BroadcastToWorkers(const std::function<void(const WorkerPtr&)>& dispatcher)
{
    if (!dispatcher)
        return;
    std::vector<WorkerPtr> workers_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workers_copy = workers;
    }
    for (auto& worker : workers_copy)
    {
        if (!worker)
            continue;
        worker->Post([dispatcher, worker]() { dispatcher(worker); });
    }
}

}
