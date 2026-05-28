#include "worker_manager.h"
#include "log/log_help.h"

namespace gb
{
 WorkerManager::WorkerManager()
{
     //for (size_t i = 0; i < worker_num; i++)
     //{
     //    worker_threads.emplace_back([this,i]() {
     //        WorkerPtr work = std::make_shared<Worker>();
     //        std::thread::id id = std::this_thread::get_id();
	    //     uint32_t thread_id = *((uint32_t*)&id);
     //        work->Init(thread_id, i);
     //        {
				 //std::lock_guard<std::mutex> lock(mutex_);
				 //workers.push_back(work);
     //        }
     //        work->OnStart();
     //        work->Run();
     //    });
     //}

}

 WorkerManager::~WorkerManager()
{
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
         uint32_t        thread_id = *((uint32_t*)&id);
         work->Init(thread_id, index);
		work->OnStart();
		work->Run();
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
	 std::thread::id id = std::this_thread::get_id();
	 uint32_t thread_id = *((uint32_t*)&id);
      
     auto it = std::find_if(workers.begin(), workers.end(), [thread_id](const WorkerPtr& work) {
         return work->GetWorkerId() == thread_id;
     });

     if (it != workers.end())
     {
         return *it;

     }
     return nullptr;
}

std::vector<std::thread>& WorkerManager::GetThreads()
{
    return worker_threads;
}

std::vector<WorkerPtr>& WorkerManager::GetWorkers()
{
    return workers;
}

}