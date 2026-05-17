#pragma once
#include "../singleton.h"
#include "worker.h"
#include "../../app/app.h"
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
    private:
        std::vector<WorkerPtr>        workers;
        std::vector<std::thread> worker_threads;
        std::mutex mutex_;
	};


}