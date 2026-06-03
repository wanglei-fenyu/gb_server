#include "worker.h"
#include "log/log.h"
#include "script/register_script.h"
#include "base/res_path.h"
#include "network/rpc/executor.h"
#include "network/rpc/rpc_call.h"

namespace gb
{

Worker::Worker(WorkerType type)
	: worker_type_(type)
{
	scriptPtr_ = std::make_shared<Script>();
    timer_manager_ = std::make_unique<TimerManager>();
}

Worker::~Worker()
{
	Stop();
}


void Worker::Init(uint32_t id, size_t index)
{
    thread_id_ = id;
    index_     = index;
    executor_  = std::make_shared<WorkerExecutor>(weak_from_this(), false);
    async_executor_ = std::make_shared<GbAsyncExecutor>(executor_);
}

void Worker::SetWorkerLogic(std::shared_ptr<IWorkerLogic> worker_logic)
{
    worker_logic_ = worker_logic;
}

void Worker::OnStart()
{
    if (worker_type_ == WorkerType::MAIN)
        LOG_INFO("Main worker start");
    else
        LOG_INFO("Worker start index:{}", index_);
    runing_.store(true);
    InitLua();
}   


void Worker::Run()
{
    auto last_frame_time = std::chrono::steady_clock::now();
    while (true)
    {
        if (!runing_.load())
        {
            // 优雅关闭：退出前处理完剩余任务
            std::function<void()> func;
            if (!events_.try_dequeue(func))
                break;
            if (func)
                func();
            continue;
        }

        // 计算自上一帧以来的时间
        auto                        frame_start = std::chrono::steady_clock::now();
        std::chrono::duration<float> elapsed    = frame_start - last_frame_time;
        last_frame_time                        = frame_start;

        ProcessFrame(elapsed.count());

        // 帧率控制：等待剩余帧时间
        // cv notify_one（来自 EnqueueTask）提前唤醒以立即处理任务
        auto frame_time = std::chrono::steady_clock::now() - frame_start;
        auto remaining  = frame_duration_ - frame_time;
        if (remaining > std::chrono::milliseconds::zero())
        {
            std::unique_lock<std::mutex> lk(event_mutex_);
            event_cv_.wait_for(lk, remaining, [this]() {
                return !runing_.load() || events_.size_approx() > 0;
            });
        }
    }
}

void Worker::ProcessFrame(float elapsed)
{
    OnUpdate(elapsed);
    OnTick();
}

void Worker::SetFrameRate(int fps)
{
    if (fps > 0)
        frame_duration_ = std::chrono::milliseconds(1000 / fps);
}

void Worker::Stop()
{
     runing_.store(false);
     event_cv_.notify_all();

}

void Worker::EnterShutdownMode()
{
    shutting_down_.store(true);
    LOG_INFO("Worker {} entering shutdown mode", index_);
}

bool Worker::CleanupInWorkerThread(int timeout_ms)
{
    if (worker_type_ == WorkerType::MAIN)
    {
        OnCleanup();
        return true;
    }

    if (!runing_.load())
    {
        return true;
    }

    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    if (!EnqueueTask([self = shared_from_this(), done]() {
            self->OnCleanup();
            done->set_value();
        }, true))
    {
        return false;
    }

    if (timeout_ms < 0)
    {
        future.wait();
        return true;
    }

    return future.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready;
}

int Worker::OnStartup()
{

    if (worker_logic_)
		worker_logic_->OnStartup();
    return 0;
}

int Worker::OnUpdate(float elapsed)
{
    if (timer_manager_)
        timer_manager_->Update();

    std::function<void()> func;
    while (events_.try_dequeue(func))
    {
        if (func)
            func();
    }

    if (worker_logic_)
        worker_logic_->OnUpdate(elapsed);
    return 0;
}

int Worker::OnTick()
{
    if (worker_logic_)
		worker_logic_->OnTick();
    return 0;
}

int Worker::OnCleanup()
{
    if (worker_logic_)
		worker_logic_->OnCleanup();
    Stop();
	return 0;
}

void Worker::Post(const std::function<void(void)>& handler)
{
    std::function<void(void)> task = handler;
    EnqueueTask(std::move(task), false);
}

void Worker::Post(std::function<void(void)>&& handler)
{
    EnqueueTask(std::move(handler), false);
}

uint32_t Worker::GetWorkerId()
{
	return thread_id_;
}


uint32_t Worker::GetIndex()
{
    return index_;
}

uint32_t Worker::AllocRpcSeq()
{
    return rpc_seq_counter_.fetch_add(1, std::memory_order_relaxed);
}

void Worker::StorePendingRpc(uint32_t local_seq, RpcCallPtr call)
{
    pending_rpcs_[local_seq] = std::move(call);
}

RpcCallPtr Worker::TakePendingRpc(uint32_t local_seq)
{
    auto it = pending_rpcs_.find(local_seq);
    if (it == pending_rpcs_.end())
        return nullptr;
    auto call = std::move(it->second);
    pending_rpcs_.erase(it);
    return call;
}

std::unique_ptr<TimerManager>& Worker::GetTimerManager()
{
    return timer_manager_;
}

std::shared_ptr<WorkerExecutor> Worker::GetExecutor() const
{
    return executor_;
}

async_simple::Executor* Worker::getAsyncSimpleExecutor() const
{
    return async_executor_.get();
}

void Worker::InitLua()
{
	using sol::lib;
	if (!scriptPtr_)
		return;
	scriptPtr_->open_libraries(lib::base, lib::package, lib::string, lib::table, lib::os, lib::bit32, lib::coroutine, lib::count, lib::debug, lib::ffi, lib::io, lib::jit, lib::math, lib::utf8);
	_lua_(scriptPtr_);
	sol::function require = (*scriptPtr_)["require"];
#ifdef MY_DEBUG_MODE
	std::string _lua_socket = ResPath::Instance()->GetCurrentExeDirectory();
#else
	std::string _lua_socket = ResPath::Instance()->FindResPath("../Release/bin/");
#endif

#if ENGINE_PLATFORM != PLATFORM_WIN32
	_lua_socket += "?.so";
#endif
	std::string package_cpath = (*scriptPtr_)["package"]["cpath"].get<std::string>();
	(*scriptPtr_)["package"]["cpath"] = package_cpath + ";" + _lua_socket;
	require("socket.core");

	std::string script_path = ResPath::Instance()->FindResPath("../script");
	std::string package_path = (*scriptPtr_)["package"]["path"];
	package_path += ";" + script_path + "/?.lua";
	(*scriptPtr_)["package"]["path"] = package_path;
	auto result = scriptPtr_->safe_script_file(script_path + "/start_debug.lua");
	if (!result.valid())
	{
		sol::error err = result;
		LOG_ERROR("Start Lua Debug Fail {}", err.what());
	}

	std::string scriptRootPath = ResPath::Instance()->FindResPath("../script/main.lua");
	scriptPtr_->Load(scriptRootPath);
}

bool Worker::EnqueueTask(std::function<void(void)>&& handler, bool force)
{
    if (!handler)
        return false;

    if (!force && shutting_down_.load())
        return false;

    if (!runing_.load())
        return false;

    events_.enqueue(std::move(handler));
    event_cv_.notify_one();
    return true;
}

}
