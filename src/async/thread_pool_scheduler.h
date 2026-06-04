#pragma once
#include "async/thread_pool.h"
#include "async_simple/coro/Lazy.h"
#include "worker/worker_manager.h"
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>

namespace gb
{

template <typename T>
class ThreadPoolAwaiter;

/// ThreadPoolScheduler —— 抢占式线程池的统一调度抽象。
///
/// 继承自 async_simple::Executor，可原生与 async_simple 协程库配合使用。
///
/// ┌──────────────────────────────────────────────┐
/// │              ThreadPoolScheduler             │
/// │  (Singleton + async_simple::Executor)        │
/// │                                              │
/// │  + schedule(func)      ← Executor 接口       │
/// │  + Execute(task)       ← 裸执行，不回投      │
/// │  + Dispatch(task,cb)   ← 回调风格            │
/// │  + Post(task)          ← 回投但不关心返回值  │
/// │  + Schedule<T>(task)   ← 协程风格 (Lazy<T>)  │
/// │                                              │
/// │  ┌────────────────────────────────┐          │
/// │  │  ThreadPool (裸执行引擎)       │          │
/// │  │  - N 个专用线程                │          │
/// │  │  - ConcurrentQueue 任务队列    │          │
/// │  └────────────────────────────────┘          │
/// └──────────────────────────────────────────────┘
///
/// 自动回投：
///   Dispatch / Post / Schedule 都会在执行完毕后
///   通过 Worker::Post 把回调/协程恢复投递到发起
///   调用的 Worker 线程。
///
class ThreadPoolScheduler : public Singleton<ThreadPoolScheduler>,
                            public async_simple::Executor
{
public:
    /// 初始化线程池。
    /// @param thread_count  线程数（0 = 硬件并发数）。
    void Init(uint32_t thread_count = 0)
    {
        thread_pool_.Init(thread_count);
    }

    /// 关闭线程池（等待所有已投递任务完成）。
    void Stop()
    {
        thread_pool_.Stop();
    }

    /// 当前待处理任务数。
    size_t PendingCount() const { return thread_pool_.PendingCount(); }

    // ── 裸执行（不回投）────────────────────────────────

    /// 在 ThreadPool 上执行 task，不保证回投到哪个线程。
    /// 调用者需自行处理线程安全。
    void Execute(std::function<void()> task)
    {
        thread_pool_.Execute(std::move(task));
    }

    // ── 回调风格 ───────────────────────────────────────


    /// 在 ThreadPool 上执行重度任务，执行完后自动回投
    /// 到发起调用的 Worker 线程调用 callback。
    ///
    /// @tparam T         任务返回值类型
    /// @param  task      在 ThreadPool 线程上执行的函数
    /// @param  callback  回到 Worker 线程后调用（接收 task 的返回值）
    ///
    /// 用法（在 Worker 线程上调用）：
    ///   ThreadPoolScheduler::Instance()->Dispatch(
    ///       []() -> PathResult { return Pathfind(start, end); },
    ///       [](PathResult r) { HandleResult(r); }
    ///   );
      // 通用模板版本（仅当 T 非 void 时参与重载）
    template <typename T, typename = std::enable_if_t<!std::is_void_v<T>>>
    void Dispatch(std::function<T()> task, std::function<void(T)> callback) {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        if (!worker) {
            LOG_WARN("ThreadPoolScheduler::Dispatch called from non-Worker thread");
            callback(task());
            return;
        }
        thread_pool_.Execute([
            worker = std::move(worker),
            task   = std::move(task),
            cb     = std::move(callback)
        ]() mutable {
            auto result = task();
            worker->Post([result = std::move(result), cb = std::move(cb)]() mutable {
                cb(std::move(result));
            });
        });
    }

    // void 版本：普通成员函数重载（非模板）
    void Dispatch(std::function<void()> task, std::function<void()> callback) {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        if (!worker) {
            LOG_WARN("ThreadPoolScheduler::Dispatch called from non-Worker thread");
            task();
            if (callback) callback();
            return;
        }
        thread_pool_.Execute([
            worker = std::move(worker),
            task   = std::move(task),
            cb     = std::move(callback)
        ]() mutable {
            task();
            worker->Post([cb = std::move(cb)]() mutable {
                if (cb) cb();
            });
        });
    }
    /// 在 ThreadPool 上执行重度任务，执行完后自动回投
    /// 到发起 Worker 线程（不关心返回值）。
    ///
    /// 等价于 Dispatch<void>(task, [](){})。
    void Post(std::function<void()> task)
    {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        if (!worker)
        {
            thread_pool_.Execute(std::move(task));
            return;
        }
        thread_pool_.Execute([
            worker = std::move(worker),
            task   = std::move(task)
        ]() mutable {
            task();
            // 即使没有返回值，也回投一个空回调以确保在 Worker
            // 线程上产生一次帧循环触碰（便于清除线程局部状态）。
            worker->Post([]() {});
        });
    }

    // ── 协程风格 ───────────────────────────────────────

    /// 返回一个可在 Worker 协程中 co_await 的 Awaiter。
    /// 用法：
    ///   auto result = co_await ThreadPoolScheduler::Instance()->Schedule<int>([]() {
    ///       return HeavyComputation();
    ///   });
    ///   // 回到 Worker 线程
    template <typename T>
    [[nodiscard]] ThreadPoolAwaiter<T> Schedule(std::function<T()> task);

    // ── async_simple::Executor 接口 ─────────────────────

    ThreadPoolScheduler() : async_simple::Executor("thread_pool") {}

    /// 在 ThreadPool 上调度函数执行（Executor 核心接口）。
    /// 语义：在 TP 线程上异步执行 func，由调用者管理协程上下文。
    bool schedule(Func func) override
    {
        thread_pool_.Execute(std::move(func));
        return true;
    }

    /// 当前线程是否为 ThreadPool 线程。
    bool currentThreadInExecutor() const override
    {
        return thread_pool_.IsThreadPoolThread();
    }

    /// 返回统计信息（待处理任务数）。
    async_simple::ExecutorStat stat() const override
    {
        async_simple::ExecutorStat s;
        s.pendingTaskCount = thread_pool_.PendingCount();
        return s;
    }

    /// ThreadPool 不提供 IOExecutor。
    async_simple::IOExecutor* getIOExecutor() override
    {
        return nullptr;
    }

private:
    ThreadPool thread_pool_;
};

// ────────────────────────────────────────────────────────
//  协程 Awaiter 实现
// ────────────────────────────────────────────────────────

/// ThreadPool 协程的共享状态。
/// 原子双重校验模式：提交任务后先记录 suspend 状态，TP 完成时
/// 若已 suspend 则回投恢复，若尚未 suspend 则标记完成，
/// 由 await_suspend 检查后决定是否挂起。
template <typename T>
class ThreadPoolAwaitState
{
public:
    using TaskFunc = std::function<T()>;

    explicit ThreadPoolAwaitState(TaskFunc task) :
        task_(std::move(task))
    {
    }

    void Bind(std::coroutine_handle<> handle, WorkerPtr worker)
    {
        handle_ = handle;
        worker_ = std::move(worker);
    }

    /// 在 ThreadPool 上执行，执行完后回投到发起 Worker。
    void ExecuteOnPool()
    {
        if constexpr (std::is_void_v<T>)
        {
            task_();
        }
        else
        {
            try
            {
                result_.emplace(task_());
            }
            catch (...)
            {
                exception_ = std::current_exception();
            }
        }
        NotifyCompleted();
    }

    /// 在 await_suspend 中调用。返回 true 表示协程需要挂起，
    /// false 表示任务已经完成不需要挂起。
    bool FinishSuspend()
    {
        suspend_finished_.store(true, std::memory_order_release);
        return !completed_.load(std::memory_order_acquire);
    }

    T TakeResult()
    {
        if (exception_)
            std::rethrow_exception(exception_);
        if constexpr (!std::is_void_v<T>)
        {
            return std::move(*result_);
        }
    }

private:
    void NotifyCompleted()
    {
        if (completed_.exchange(true, std::memory_order_acq_rel))
            return; // 已处理过

        if (suspend_finished_.load(std::memory_order_acquire))
        {
            auto h = handle_;
            auto w = std::move(worker_);
            w->Post([h]() { h.resume(); });
        }
        // 否则: FinishSuspend() 中的 completed_ 检查会发现并返回 false
    }

private:
    TaskFunc                     task_;
    std::optional<T>             result_{};
    std::exception_ptr           exception_{};
    std::coroutine_handle<>      handle_{};
    WorkerPtr                    worker_;
    std::atomic<bool>            suspend_finished_{false};
    std::atomic<bool>            completed_{false};
};

/// ThreadPool 协程可等待对象。
/// 用法：co_await ThreadPoolAwaiter<T>(lambda)
template <typename T>
struct ThreadPoolAwaiter
{
    using state_type = ThreadPoolAwaitState<T>;

    explicit ThreadPoolAwaiter(typename state_type::TaskFunc task) :
        state_(std::make_shared<state_type>(std::move(task)))
    {
    }

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        if (!worker)
        {
            LOG_ERROR("ThreadPoolAwaiter used outside of Worker thread, running inline");
            state_->Bind(handle, WorkerPtr{});
            state_->ExecuteOnPool();
            return false;
        }
        state_->Bind(handle, worker);
        ThreadPoolScheduler::Instance()->Execute([state = state_]() {
            state->ExecuteOnPool();
        });
        return state_->FinishSuspend();
    }

    T await_resume() { return state_->TakeResult(); }

private:
    std::shared_ptr<state_type> state_;
};

// ── Schedule 模板方法实现（必须跟在 Awaiter 定义之后）──

template <typename T>
inline ThreadPoolAwaiter<T> ThreadPoolScheduler::Schedule(std::function<T()> task)
{
    return ThreadPoolAwaiter<T>{std::move(task)};
}

/// 自由函数风格的 ScheduleOnThreadPool（向下兼容）。
/// 内部通过 ThreadPoolScheduler 调度。
template <typename T>
inline async_simple::coro::Lazy<T> ScheduleOnThreadPool(std::function<T()> task)
{
    co_return co_await ThreadPoolAwaiter<T>{std::move(task)};
}

} // namespace gb
