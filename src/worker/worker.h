#pragma once
#include "gbnet/common/def.h"
#include "gbnet/common/define.h"
#include <functional>
#include "script/script.h"
#include "base/singleton.h"
#include <gbnet/common/define.h>
#include "concurrentqueue.h"
#include "timer/timer_manager.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <future>
#include <unordered_map>

#include "worker_logic_interface.h"
#include "network/router/service_worker_type.h"

// 前置声明
namespace gb
{
class RpcCall;
}

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
	enum class WorkerType
	{
		NORMAL,
		MAIN
	};

public:
	Worker(WorkerType type = WorkerType::NORMAL);
	virtual ~Worker();
public:
    void Init(uint32_t id, size_t index);
    /// 设置业务 Worker 类型（Normal/AI/Navigation），CreateWorker 时自动设置
    void SetServiceType(ServiceWorkerType st) { service_type_ = st; }
    ServiceWorkerType GetServiceType() const { return service_type_; }
    void SetWorkerLogic(std::shared_ptr<IWorkerLogic> worker_logic);
    void OnStart();
	void Run();
	void ProcessFrame(float elapsed);
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
    uint32_t GetIndex();
	WorkerType GetType() const { return worker_type_; }
	bool IsMainWorker() const { return worker_type_ == WorkerType::MAIN; }

public:
    /// 设置帧率（默认 60fps）
    void SetFrameRate(int fps);

public:
    std::unique_ptr<TimerManager>& GetTimerManager();
    std::shared_ptr<WorkerExecutor> GetExecutor() const;
    async_simple::Executor* getAsyncSimpleExecutor() const;

public:
    /// 进入关闭模式：停止接受新任务，处理待处理任务
    void EnterShutdownMode();
    bool CleanupInWorkerThread(int timeout_ms = -1);
    
    /// 检查worker是否处于关闭模式
    bool IsShuttingDown() const { return shutting_down_.load(); }
    
    /// 获取待处理任务数
    size_t GetPendingTaskCount() const { return events_.size_approx(); }

    /// 尝试从事件队列取一个任务（被 App::ProcessMainThreadEvents 用来 drain main_worker 的事件）
    bool TryDequeueEvent(std::function<void(void)>& task) { return events_.try_dequeue(task); }

public:
    /// 每个worker的RPC序列计数器——无锁，线程本地所有权
    uint32_t AllocRpcSeq();
    void     StorePendingRpc(uint32_t local_seq, std::shared_ptr<RpcCall> call);
    std::shared_ptr<RpcCall> TakePendingRpc(uint32_t local_seq);

private:
    void InitLua();
    bool EnqueueTask(std::function<void(void)>&& handler, bool force);

private:
	ScriptPtr	scriptPtr_;
	WorkerType worker_type_;
    ServiceWorkerType service_type_{SWT_Normal};
    uint32_t index_ = 0;
    uint32_t thread_id_ = 0;
	moodycamel::ConcurrentQueue<std::function<void(void)>> events_;
    std::unique_ptr<TimerManager> timer_manager_;
	std::atomic<bool> runing_ = false;
    std::atomic<bool> shutting_down_ = false;
    std::mutex event_mutex_;
    std::condition_variable event_cv_;

    std::shared_ptr<IWorkerLogic> worker_logic_;
    std::shared_ptr<WorkerExecutor> executor_;
    std::shared_ptr<GbAsyncExecutor> async_executor_;
    std::atomic<uint32_t> rpc_seq_counter_{1};

    /// 帧时长（默认 16ms ≈ 60fps）
    std::chrono::milliseconds frame_duration_{16};

    /// 当前 Worker 的待处理 RPC 调用映射
    /// 从 thread_local 迁移为成员变量，为线程池做准备
    std::unordered_map<uint32_t, std::shared_ptr<RpcCall>> pending_rpcs_;
};

using WorkerPtr = std::shared_ptr<Worker>;
using WorkerWeakPtr = std::weak_ptr<Worker>;
}
