#pragma once
#include "../singleton.h"
#include "worker.h"
#include "concurrentqueue.h"
#include <functional>
#include <limits>
#include <thread>

namespace gb
{
class WorkerManager : public Singleton<WorkerManager>
{
public:
	WorkerManager();
	~WorkerManager();

	// 初始化主线程 Worker（在 App::Init 中调用）
	void InitMainWorker();

	// 创建普通工作线程
	WorkerPtr CreateWorker(std::shared_ptr<IWorkerLogic> worker_logic);

	// 获取主线程 Worker
	WorkerPtr GetMainWorker() const { return main_worker_; }

	// Worker 管理接口
	size_t Size() const;
	WorkerPtr GetWorker(size_t index) const;
	WorkerPtr GetCurWorker() const;
	std::vector<std::thread>& GetThreads();
	std::vector<WorkerPtr>& GetWorkers();

	// 任务投递接口
	bool PostToWorker(size_t index, const std::function<void()>& handler) const;
	bool PostToWorker(size_t index, std::function<void()>&& handler) const;
	
	// 投递到主线程
	bool PostToMain(const std::function<void()>& handler) const;
	bool PostToMain(std::function<void()>&& handler) const;

	// 旧接口保留（向后兼容）
	void PostMain(const std::function<void()>& handler) { PostToMain(handler); }
	void PostMain(std::function<void()>&& handler) { PostToMain(std::move(handler)); }
	size_t DrainMainQueue(size_t max_events = std::numeric_limits<size_t>::max());

	bool IsMainThread() const;
	void BroadcastToWorkers(const std::function<void(const WorkerPtr&)>& dispatcher);

private:
	WorkerPtr main_worker_;                                              // 主线程 Worker
	std::vector<WorkerPtr> workers;                                     // 所有工作线程
	std::vector<std::thread> worker_threads;                            // 工作线程容器
	moodycamel::ConcurrentQueue<std::function<void()>> main_events_;   // 兼容旧的队列
	std::thread::id main_thread_id_;
	std::mutex mutex_;
};

}
