#pragma once
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
#include <chrono>

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
	// Worker 类型枚举
	enum class WorkerType
	{
		NORMAL,    // 普通工作线程
		MAIN       // 主线程
	};

public:
	Worker(WorkerType type = WorkerType::NORMAL);
	virtual ~Worker();

public:
	// 初始化方法
	void Init(uint32_t id, size_t index);
	void SetWorkerLogic(std::shared_ptr<IWorkerLogic> worker_logic);
	void OnStart();
	
	// 执行方法
	void Run();                    // 用于工作线程的运行循环
	void ProcessMainFrame();       // 用于主线程的帧处理（从 App::Run 调用）
	void Stop();

public:
	// 生命周期钩子
	virtual int OnStartup();
	virtual int OnUpdate(float elapsed);
	virtual int OnTick();
	virtual int OnCleanup();

public:
	// 任务投递接口
	void Post(const std::function<void(void)>& handler);
	void Post(std::function<void(void)>&& handler);

public:
	// 获取 Worker 信息
	ScriptPtr GetScript() { return scriptPtr_; }
	uint32_t GetWorkerId();
	uint32_t GetIndex();
	WorkerType GetWorkerType() const { return worker_type_; }
	bool IsMainThread() const { return worker_type_ == WorkerType::MAIN; }

public:
	// 管理方法
	std::unique_ptr<TimerManager>& GetTimerManager();
	std::shared_ptr<WorkerExecutor> GetExecutor() const;
	async_simple::Executor* getAsyncSimpleExecutor() const;

private:
	void InitLua();

private:
	ScriptPtr scriptPtr_;
	WorkerType worker_type_;
	uint32_t index_;
	uint32_t thread_id_;
	moodycamel::ConcurrentQueue<std::function<void(void)>> events_;
	std::unique_ptr<TimerManager> timer_manager_;
	std::atomic<bool> runing_ = false;
	std::mutex event_mutex_;
	std::condition_variable event_cv_;

	std::shared_ptr<IWorkerLogic> worker_logic_;
	std::shared_ptr<WorkerExecutor> executor_;
	std::shared_ptr<GbAsyncExecutor> async_executor_;

	// 用于主线程的帧计时
	std::chrono::steady_clock::time_point last_frame_time_;
};

using WorkerPtr = std::shared_ptr<Worker>;
using WorkerWeakPtr = std::weak_ptr<Worker>;
}
