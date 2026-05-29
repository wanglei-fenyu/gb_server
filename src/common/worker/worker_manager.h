#pragma once
#include "../singleton.h"
#include "worker.h"
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
        void InitMainWorker();
        WorkerPtr GetMainWorker() const;
        WorkerPtr CreateWorker(std::shared_ptr<IWorkerLogic> worker_logic);

        size_t Size() const;
        WorkerPtr GetWorker(size_t index) const;
        WorkerPtr GetCurWorker() const;
        std::vector<std::thread>& GetThreads();
        std::vector<WorkerPtr>&   GetWorkers();
        bool                      PostToWorker(size_t index, const std::function<void()>& handler) const;
        bool                      PostToWorker(size_t index, std::function<void()>&& handler) const;
        bool                      PostToMain(const std::function<void()>& handler) const;
        bool                      PostToMain(std::function<void()>&& handler) const;
        bool                      IsMainThread() const;
        void                      BroadcastToWorkers(const std::function<void(const WorkerPtr&)>& dispatcher);
    private:
        WorkerPtr                                     main_worker_;
        std::vector<WorkerPtr>                        workers;
        std::vector<std::thread>                      worker_threads;
        std::thread::id                               main_thread_id_;
        std::mutex                                    mutex_;
	};


}
