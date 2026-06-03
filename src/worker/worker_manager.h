#pragma once
#include "base/singleton.h"
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
        /// 创建 Normal Worker。
        /// @param worker_logic  工作逻辑
        /// @param service_type  业务类型（SWT_Normal / SWT_AI / SWT_Navigation 等）
        WorkerPtr CreateWorker(std::shared_ptr<IWorkerLogic> worker_logic,
                               ServiceWorkerType service_type = SWT_Normal);

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
