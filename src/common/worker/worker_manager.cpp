#include "worker_manager.h"
#include "log/log_help.h"

namespace gb
{

namespace
{
thread_local WorkerPtr tl_current_worker;
}

WorkerManager::WorkerManager()
	: main_worker_(nullptr)
{
	main_thread_id_ = std::this_thread::get_id();
}

WorkerManager::~WorkerManager()
{
	if (main_worker_)
		main_worker_->Stop();

	for (auto& worker : workers)
		worker->Stop();

	for (auto& thread : worker_threads)
		if (thread.joinable())
			thread.join();
}

void WorkerManager::InitMainWorker()
{
	if (main_worker_)
	{
		LOG_WARNING("Main worker already initialized");
		return;
	}

	main_worker_ = std::make_shared<Worker>(Worker::WorkerType::MAIN);
	uint32_t thread_id = static_cast<uint32_t>(std::hash<std::thread::id>{}(main_thread_id_));
	main_worker_->Init(thread_id, 0);
	tl_current_worker = main_worker_;
	main_worker_->OnStart();

	LOG_INFO("Main worker initialized");
}

WorkerPtr WorkerManager::CreateWorker(std::shared_ptr<IWorkerLogic> worker_logic)
{
	WorkerPtr work = std::make_shared<Worker>(Worker::WorkerType::NORMAL);
	work->SetWorkerLogic(worker_logic);

	size_t index = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		index = workers.size() + 1;  // +1 是因为主线程占用了 index 0
		workers.push_back(work);
	}

	worker_threads.emplace_back([work, index]() {
		std::thread::id id = std::this_thread::get_id();
		uint32_t thread_id = static_cast<uint32_t>(std::hash<std::thread::id>{}(id));
		work->Init(thread_id, index);
		tl_current_worker = work;
		work->OnStart();
		work->Run();
		tl_current_worker.reset();
	});

	return work;
}

size_t WorkerManager::Size() const
{
	return workers.size() + (main_worker_ ? 1 : 0);
}

gb::WorkerPtr WorkerManager::GetWorker(size_t index) const
{
	if (index == 0 && main_worker_)
		return main_worker_;

	if (index > 0 && index - 1 < workers.size())
		return workers[index - 1];

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

bool WorkerManager::PostToMain(const std::function<void()>& handler) const
{
	if (!handler || !main_worker_)
		return false;
	main_worker_->Post(handler);
	return true;
}

bool WorkerManager::PostToMain(std::function<void()>&& handler) const
{
	if (!main_worker_)
		return false;
	main_worker_->Post(std::move(handler));
	return true;
}

size_t WorkerManager::DrainMainQueue(size_t max_events)
{
	size_t processed = 0;
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
		WorkerWeakPtr weak_worker = worker;
		worker->Post([dispatcher, weak_worker]() {
			auto target = weak_worker.lock();
			if (target)
				dispatcher(target);
		});
	}
}

}
