#pragma once
#include "../singleton.h"
#include "worker.h"
#include "concurrentqueue.h"
#include <functional>
#include <limits>
#include <thread>
namespace gb
{
	class WorkerManager :public Singleton<WorkerManager>
	{
    public:
		WorkerManager();
		~WorkerManager();
        WorkerPtr CreateWorker(std::shared_ptr<IWorkerLogic> worker_logic);

        size_t Size();
        WorkerPtr GetWorker(size_t index) const;
        WorkerPtr GetCurWorker() const;
        std::vector<std::thread>& GetThreads();
        std::vector<WorkerPtr>&   GetWorkers();
        bool                      PostToWorker(size_t index, const std::function<void()>& handler) const;
        bool                      PostToWorker(size_t index, std::function<void()>&& handler) const;
        void                      PostMain(const std::function<void()>& handler);
        void                      PostMain(std::function<void()>&& handler);
        size_t                    DrainMainQueue(size_t max_events = std::numeric_limits<size_t>::max());
        bool                      IsMainThread() const;
        void                      BroadcastToWorkers(const std::function<void(const WorkerPtr&)>& dispatcher);
    private:
        std::vector<WorkerPtr>                         workers;
        std::vector<std::thread>                       worker_threads;
        moodycamel::ConcurrentQueue<std::function<void()>> main_events_;
        std::thread::id                                main_thread_id_;
        std::mutex                                     mutex_;
	};


}