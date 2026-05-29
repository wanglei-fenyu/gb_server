#pragma once 
#include "gbnet/common/def.h"
#include "gbnet/common/define.h"
#include <functional>
#include "script/script.h"
#include <concurrentqueue.h>
#include <map>
#include <atomic>

namespace gb
{

template <typename F>
concept FUNC = requires () { std::is_function<F>::value; };

class IoWorker : public std::enable_shared_from_this<IoWorker>
{
	using ThreadPtr = std::unique_ptr<std::thread>;
	using ScriptPtr = std::shared_ptr<Script>;
public:
	IoWorker();
	virtual ~IoWorker();
public:
	void OnStart();
	void Run();
	void Stop();
	void GracefulStop();
	IoService& GetIoContext() { return m_ioContext_; }

public:
	uint32_t GetWorkerId();
	bool IsShuttingDown() const { return shutting_down_.load(); }

private:
	IoService			 m_ioContext_;
	IoServiceStrand		 m_ioContextStrand_;
	
	ThreadPtr    m_threadPtr_;
	std::atomic<bool> shutting_down_{false};
};

using IoWorkerPtr = std::shared_ptr<IoWorker>;

class IoServicePool 
{
public:
    IoServicePool(int workerNum = 4);

public:
	size_t GetWorkerCount() { return m_workers.size(); }
	IoWorkerPtr GetWorker(int index);
	IoWorkerPtr GetWorkerById(uint32_t id);
	std::pair<int,IoService&> GetIoService();
    const std::vector<IoWorkerPtr>&    Workers() const;
	void Stop();
	void GracefulStop();
    void Run();

public:
	

private:
	std::vector<IoWorkerPtr> m_workers;
	std::map<uint32_t, IoWorkerPtr> m_worker_map;			//key Ïß³Ìid
    size_t                        _next_service;
};


using IoServicePoolPtr = std::shared_ptr<IoServicePool>;
}
