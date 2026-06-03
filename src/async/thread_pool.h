#pragma once
#include "base/singleton.h"
#include "concurrentqueue.h"
#include "log/log.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gb
{

/// 抢占式线程池 —— 用于执行不应阻塞 Worker 帧循环的重度任务
///（寻路、AI 批量计算、大包序列化、数据库查询等）。
///
/// 与 Worker 线程的关系：
///   - Worker 线程跑游戏逻辑（低延迟、帧率敏感）
///   - ThreadPool 线程跑重度计算（允许数十毫秒）
///   - 任务执行完后通过 Worker::Post 回到 Worker 线程
///
/// 用法示例（Worker 内）：
///   ThreadPool::Instance()->Execute([this]() {
///       // ── 在 ThreadPool 线程上执行 ──
///       auto result = Pathfind(start, end);
///
///       // 不能直接写 Worker 成员，回投到 Worker 线程
///       WorkerManager::Instance()->GetCurWorker()->Post([result]() {
///           // ── 回到 Worker 线程 ──
///           HandlePathResult(result);
///       });
///   });
///
/// 线程安全：是（可被任何线程调用）。
class ThreadPool : public Singleton<ThreadPool>
{
public:
    ThreadPool()  = default;
    ~ThreadPool() { Stop(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// 初始化线程池。
    /// @param thread_count  线程数（0 = 硬件并发数）。
    void Init(size_t thread_count = 0)
    {
        if (!threads_.empty())
            return; // 已初始化

        if (thread_count == 0)
            thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0)
            thread_count = 1;

        running_ = true;
        for (size_t i = 0; i < thread_count; ++i)
        {
            threads_.emplace_back([this, i]() {
                LOG_INFO("ThreadPool worker {} started", i);
                WorkerLoop();
                LOG_INFO("ThreadPool worker {} stopped", i);
            });
        }
        LOG_INFO("ThreadPool initialized with {} threads", thread_count);
    }

    /// 投递一个任务到线程池。
    /// task 会在任意一个线程池线程上异步执行。
    void Execute(std::function<void()> task)
    {
        if (!running_)
        {
            LOG_WARN("ThreadPool not running, discarding task");
            return;
        }
        pending_.fetch_add(1, std::memory_order_release);
        queue_.enqueue(std::move(task));
        WakeOne();
    }

    /// 等待所有剩余任务完成并停止线程池。
    void Stop()
    {
        if (!running_)
            return;
        running_ = false;
        WakeAll();
        for (auto& t : threads_)
        {
            if (t.joinable())
                t.join();
        }
        threads_.clear();
        LOG_INFO("ThreadPool stopped");
    }

    /// 当前待处理任务数。
    size_t PendingCount() const { return pending_.load(std::memory_order_acquire); }

    /// 线程池线程数。
    size_t ThreadCount() const { return threads_.size(); }

private:
    void WorkerLoop()
    {
        while (running_)
        {
            std::function<void()> task;
            if (queue_.try_dequeue(task))
            {
                pending_.fetch_sub(1, std::memory_order_acquire);
                if (task)
                {
                    task();
                }
                continue;
            }
            // 空队列 —— 等待唤醒
            WaitForTask();
        }

        // 关闭后执行剩余任务（pending_ 不计了，直接 drain）
        std::function<void()> task;
        while (queue_.try_dequeue(task))
        {
            if (task)
                task();
        }
    }

    void WaitForTask()
    {
        std::unique_lock<std::mutex> lock(wake_mutex_);
        wake_cv_.wait_for(lock, std::chrono::milliseconds(100),
                          [this]() { return !running_ || pending_.load(std::memory_order_acquire) > 0; });
    }

    void WakeOne()
    {
        wake_cv_.notify_one();
    }

    void WakeAll()
    {
        wake_cv_.notify_all();
    }

    std::atomic<bool>                         running_{false};
    std::atomic<size_t>                       pending_{0};
    std::vector<std::thread>                  threads_;
    moodycamel::ConcurrentQueue<std::function<void()>> queue_;

    std::mutex                                wake_mutex_;
    std::condition_variable                   wake_cv_;
};

} // namespace gb
