#pragma  once
#include "gbnet/common/def.h"
#include "gbnet/common/define.h"
#include <functional>
#include "../../script/script.h"
#include "../singleton.h"
#include <gbnet/common/define.h>
#include "concurrentqueue.h"
#include "../timer/timer_manager.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

#include "worker_logic_interface.h"

namespace async_simple
{
class Executor;
}

namespace gb
{
class WorkerExecutor;
class GbAsyncExecutor;

class Worker : public std::enable_shared_from_this<Worker>
{
	using ScriptPtr = std::shared_ptr<Script>;
public:
	Worker();
	virtual ~Worker();
public:
    void Init(uint32_t id, size_t index);
    void SetWorkerLogic(std::shared_ptr<IWorkerLogic> worker_logic);
    void OnStart();
	void Run();
	void Stop();

public:
    virtual int OnStartup();
    virtual int OnUpdate(float elapsed);
    virtual int OnTick();
    virtual int OnCleanup();
public:
	void Post(const std::function<void(void)>& handler);
    void Post(std::function<void(void)>&& handler);
public:
	ScriptPtr GetScript() { return scriptPtr_; }
	uint32_t GetWorkerId();
    uint32_t  GetIndex();

public:
    std::unique_ptr<TimerManager>& GetTimerManager();
    std::shared_ptr<WorkerExecutor>      GetExecutor() const;
    async_simple::Executor*        getAsyncSimpleExecutor() const;

private:
    void InitLua();

private:
	ScriptPtr	scriptPtr_;
    uint32_t      index_;
    uint32_t	thread_id_;	 
	moodycamel::ConcurrentQueue<std::function<void(void)>> events_;
    std::unique_ptr<TimerManager>   timer_manager_;
	std::atomic<bool> runing_ = false;
    std::mutex event_mutex_;
    std::condition_variable event_cv_;

    std::shared_ptr<IWorkerLogic>    worker_logic_;
    std::shared_ptr<WorkerExecutor>        executor_;
    std::shared_ptr<GbAsyncExecutor> async_executor_;
};

using WorkerPtr = std::shared_ptr<Worker>;
using WorkerWeakPtr = std::weak_ptr<Worker>;
}