//
// scheduler_test.cpp — 调度器模块单元测试
//
// 覆盖：
//   - ThreadPool：抢占式线程池的初始化、执行、状态查询、生命周期
//   - ThreadPoolScheduler：统一调度抽象，Execute/schedule/currentThreadInExecutor
//   - ThreadPoolScheduler::Dispatch/Post 无 Worker 时的回退路径
//   - WorkerExecutor：mock dispatch/in_executor 模式
//
// 以上测试不依赖 App/Worker/Lua 初始化，可在 UNIT_TEST_HEADLESS 子进程中独立运行。
// 需要真实 Worker 上下文的集成测试见 app_test.cpp 的调度器子菜单。
//

#include <catch2/catch_test_macros.hpp>

// Catch2 的 CHECK 与项目 log.h 的 CHECK 冲突，取消 Catch2 版本
#ifdef CHECK
#undef CHECK
#endif

#include "async/thread_pool_scheduler.h"
#include "async/thread_pool.h"
#include "network/rpc/executor.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace gb;

// ── 辅助：等待 ThreadPool 处理完所有待执行任务 ──
static void Drain(ThreadPool& tp)
{
    while (tp.PendingCount() > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

// ============================================================
// ThreadPool — 抢占式线程池
// ============================================================

TEST_CASE("scheduler: ThreadPool Init/Stop", "[scheduler][threadpool]")
{
    ThreadPool tp;
    tp.Init(2);
    REQUIRE(tp.ThreadCount() == 2);
    REQUIRE(tp.PendingCount() == 0);
    tp.Stop();
    // Stop 后线程应全部 join
    REQUIRE(tp.ThreadCount() == 0);
}

TEST_CASE("scheduler: ThreadPool Init(0) 使用硬件并发数", "[scheduler][threadpool]")
{
    ThreadPool tp;
    tp.Init(0);
    REQUIRE(tp.ThreadCount() > 0);
    REQUIRE(tp.ThreadCount() <= std::thread::hardware_concurrency());
    tp.Stop();
}

TEST_CASE("scheduler: ThreadPool 重复 Init 被忽略", "[scheduler][threadpool]")
{
    ThreadPool tp;
    tp.Init(2);
    size_t first = tp.ThreadCount();
    tp.Init(100);  // 第二次调用，threads_ 非空 → 忽略
    REQUIRE(tp.ThreadCount() == first);
    tp.Stop();
}

TEST_CASE("scheduler: ThreadPool 单任务执行", "[scheduler][threadpool]")
{
    ThreadPool tp;
    tp.Init(2);

    std::atomic<int64_t> val{0};
    tp.Execute([&]() { val.store(42); });
    Drain(tp);
    tp.Stop();

    REQUIRE(val.load() == 42);
}

TEST_CASE("scheduler: ThreadPool 多任务并行", "[scheduler][threadpool]")
{
    ThreadPool tp;
    tp.Init(4);

    std::atomic<int64_t> counter{0};
    constexpr int kTasks = 200;
    for (int i = 0; i < kTasks; i++)
        tp.Execute([&]() { counter.fetch_add(1); });
    Drain(tp);
    tp.Stop();

    REQUIRE(counter.load() == kTasks);
}

TEST_CASE("scheduler: ThreadPool PendingCount 跟踪", "[scheduler][threadpool]")
{
    ThreadPool tp;
    tp.Init(1);

    std::promise<void> task_started;
    std::promise<void> let_task_finish;

    auto started_future = task_started.get_future();
    auto finish_future  = let_task_finish.get_future();

    tp.Execute([&]() {
        task_started.set_value();
        finish_future.wait();
    });

    // 等待任务开始执行（这期间 pending 可能为 0 因为已出队）
    started_future.wait();

    // 此时任务正在执行，pending 应为 0（已出队）
    REQUIRE(tp.PendingCount() == 0);

    let_task_finish.set_value();
    Drain(tp);
    tp.Stop();
}

TEST_CASE("scheduler: ThreadPool IsThreadPoolThread", "[scheduler][threadpool]")
{
    ThreadPool tp;
    tp.Init(2);

    // 测试线程不在 TP 中
    REQUIRE_FALSE(tp.IsThreadPoolThread());

    // 在 TP 线程中检查
    std::atomic<bool> in_tp{false};
    tp.Execute([&]() { in_tp.store(tp.IsThreadPoolThread()); });
    Drain(tp);
    tp.Stop();

    REQUIRE(in_tp.load());
}

TEST_CASE("scheduler: ThreadPool Stop 时 drain 剩余任务", "[scheduler][threadpool]")
{
    ThreadPool tp;
    tp.Init(2);

    std::atomic<int> executed{0};
    // 投递大量任务，然后立即 Stop（不等待）
    for (int i = 0; i < 50; i++)
        tp.Execute([&]() { executed.fetch_add(1); });

    tp.Stop();  // Stop 内部 join 线程并 drain 队列

    // Stop 后所有已投递任务应至少执行一次（可能更多，但不会更少）
    REQUIRE(executed.load() >= 50);
}

TEST_CASE("scheduler: ThreadPool 嵌套 Execute（TP 内投递 TP）", "[scheduler][threadpool]")
{
    ThreadPool tp;
    tp.Init(2);

    std::atomic<int> outer{0};
    std::atomic<int> inner{0};

    tp.Execute([&]() {
        outer.store(1);
        // 在 TP 线程内再次投递
        tp.Execute([&]() { inner.store(1); });
    });

    Drain(tp);
    tp.Stop();

    REQUIRE(outer.load() == 1);
    REQUIRE(inner.load() == 1);
}

// ============================================================
// ThreadPoolScheduler — 统一调度抽象（无 Worker 路径）
// ============================================================

TEST_CASE("scheduler: ThreadPoolScheduler Init/Stop", "[scheduler][scheduler]")
{
    ThreadPoolScheduler sched;
    sched.Init(2);
    REQUIRE(sched.PendingCount() == 0);
    sched.Stop();
}

TEST_CASE("scheduler: ThreadPoolScheduler Execute 裸执行", "[scheduler][scheduler]")
{
    ThreadPoolScheduler sched;
    sched.Init(2);

    std::atomic<int> val{0};
    sched.Execute([&]() { val.store(1); });
    while (sched.PendingCount() > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.Stop();

    REQUIRE(val.load() == 1);
}

TEST_CASE("scheduler: ThreadPoolScheduler schedule (Executor 接口)", "[scheduler][scheduler]")
{
    ThreadPoolScheduler sched;
    sched.Init(2);

    std::atomic<bool> executed{false};
    sched.schedule([&]() { executed.store(true); });
    while (sched.PendingCount() > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.Stop();

    REQUIRE(executed.load());
}

TEST_CASE("scheduler: ThreadPoolScheduler currentThreadInExecutor", "[scheduler][scheduler]")
{
    ThreadPoolScheduler sched;
    sched.Init(2);

    // 测试线程不在 TP 中
    REQUIRE_FALSE(sched.currentThreadInExecutor());

    // 在 TP 线程中检查
    std::atomic<bool> in_tp{false};
    sched.Execute([&]() { in_tp.store(sched.currentThreadInExecutor()); });
    while (sched.PendingCount() > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.Stop();

    REQUIRE(in_tp.load());
}

TEST_CASE("scheduler: ThreadPoolScheduler stat() 返回待处理数", "[scheduler][scheduler]")
{
    ThreadPoolScheduler sched;
    sched.Init(1);

    sched.Execute([]() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
    sched.Execute([]() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });

    // 应观察到至少有一个待处理任务
    auto st = sched.stat();
    REQUIRE(st.pendingTaskCount > 0);

    while (sched.PendingCount() > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.Stop();
}

TEST_CASE("scheduler: Dispatch<T> 无 Worker 时回退 inline 执行", "[scheduler][scheduler][noworker]")
{
    ThreadPoolScheduler sched;
    sched.Init(2);

    // 测试线程不在 Worker 上下文中，无 Worker → 走回退路径（同步执行）
    std::atomic<int> result{0};
    sched.Dispatch<int>(
        []() { return 99; },
        [&](int r) { result.store(r); }
    );

    // 回退路径是同步的
    REQUIRE(result.load() == 99);
    sched.Stop();
}

TEST_CASE("scheduler: Dispatch<void> 无 Worker 时回退 inline", "[scheduler][scheduler][noworker]")
{
    ThreadPoolScheduler sched;
    sched.Init(2);

    std::atomic<bool> called{false};
    sched.Dispatch(
        []() { /* do nothing */ },
        [&]() { called.store(true); }
    );

    REQUIRE(called.load());
    sched.Stop();
}

TEST_CASE("scheduler: Post 无 Worker 时在 TP 上执行", "[scheduler][scheduler]")
{
    ThreadPoolScheduler sched;
    sched.Init(2);

    std::atomic<bool> executed{false};
    sched.Post([&]() { executed.store(true); });

    while (sched.PendingCount() > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.Stop();

    REQUIRE(executed.load());
}

// ============================================================
// WorkerExecutor — mock dispatch/in_executor 模式
// ============================================================

TEST_CASE("scheduler: WorkerExecutor 默认构造 inline_fallback 执行", "[scheduler][executor]")
{
    WorkerExecutor exec;
    REQUIRE_FALSE(exec.HasWorker());
    REQUIRE_FALSE(exec.IsCurrent());

    bool called = false;
    bool ok = exec.Dispatch([&]() { called = true; });
    REQUIRE(ok);
    REQUIRE(called);  // inline_fallback 直接执行
}

TEST_CASE("scheduler: WorkerExecutor inline_fallback=false 返回 false", "[scheduler][executor]")
{
    WorkerExecutor exec(WorkerWeakPtr{}, false);
    REQUIRE_FALSE(exec.HasWorker());

    bool called = false;
    bool ok = exec.Dispatch([&]() { called = true; });
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(called);
}

TEST_CASE("scheduler: WorkerExecutor 自定义 dispatch 函数", "[scheduler][executor]")
{
    int dispatch_count = 0;
    auto dispatch_fn = [&](std::function<void()> fn) {
        dispatch_count++;
        fn();
    };
    WorkerExecutor exec(dispatch_fn, []() { return false; });

    bool called = false;
    bool ok = exec.Dispatch([&]() { called = true; });
    REQUIRE(ok);
    REQUIRE(called);
    REQUIRE(dispatch_count == 1);
}

TEST_CASE("scheduler: WorkerExecutor 自定义 in_executor 函数", "[scheduler][executor]")
{
    WorkerExecutor exec(
        [](auto) {},
        []() { return true; }  // 模拟在 worker 线程中
    );
    REQUIRE(exec.IsCurrent());
}

TEST_CASE("scheduler: WorkerExecutor IsCurrent 时 inline 执行", "[scheduler][executor]")
{
    // dispatch 函数不应该被调用（因为 IsCurrent 会 inline 执行）
    int dispatch_called = 0;
    auto dispatch_fn = [&](std::function<void()> fn) {
        dispatch_called++;
        fn();
    };
    WorkerExecutor exec(dispatch_fn, []() { return true; });

    bool called = false;
    bool ok = exec.Dispatch([&]() { called = true; });
    REQUIRE(ok);
    REQUIRE(called);
    // dispatch 函数未被调用 → 说明 inline 执行了
    REQUIRE(dispatch_called == 0);
}

TEST_CASE("scheduler: WorkerExecutor HasWorker 检查", "[scheduler][executor]")
{
    // 使用空的 shared_ptr（模拟已销毁的 worker）
    auto empty = std::make_shared<Worker>();
    WorkerExecutor exec(empty);
    REQUIRE(exec.HasWorker());

    empty.reset();
    REQUIRE_FALSE(exec.HasWorker());  // worker 已 expire
}

TEST_CASE("scheduler: WorkerExecutor 拷贝构造", "[scheduler][executor]")
{
    int dc = 0;
    auto dfn = [&](auto fn) { dc++; fn(); };
    WorkerExecutor exec(dfn, []() { return true; });

    WorkerExecutor copy(exec);
    REQUIRE(copy.IsCurrent());
    REQUIRE_FALSE(copy.HasWorker());

    // 拷贝后的 executor 应能正常调用
    bool called = false;
    copy.Dispatch([&]() { called = true; });
    REQUIRE(called);
    // IsCurrent → inline 执行，不调用 dispatch_
    REQUIRE(dc == 0);
}

TEST_CASE("scheduler: WorkerExecutor 移动构造", "[scheduler][executor]")
{
    int dc = 0;
    auto dfn = [&](auto fn) { dc++; fn(); };
    WorkerExecutor exec(dfn, []() { return false; });

    WorkerExecutor moved(std::move(exec));
    REQUIRE_FALSE(moved.HasWorker());
    REQUIRE_FALSE(moved.IsCurrent());

    bool called = false;
    moved.Dispatch([&]() { called = true; });
    REQUIRE(called);
    REQUIRE(dc == 1);
}

TEST_CASE("scheduler: WorkerExecutor schedule 转发到 Dispatch", "[scheduler][executor]")
{
    // schedule 是 async_simple Executor 接口，内部调用 Dispatch
    auto dispatch_fn = [](std::function<void()> fn) { fn(); };
    WorkerExecutor exec(dispatch_fn, []() { return false; });

    bool called = false;
    bool ok = exec.schedule([&]() { called = true; });
    REQUIRE(ok);
    REQUIRE(called);
}

TEST_CASE("scheduler: WorkerExecutor Dispatch 空函数返回 false", "[scheduler][executor]")
{
    WorkerExecutor exec;
    bool ok = exec.Dispatch(nullptr);
    REQUIRE_FALSE(ok);
}

TEST_CASE("scheduler: WorkerExecutor Main() 无 Worker 时回退到 dispatch", "[scheduler][executor]")
{
    // 在 headless 模式下 GetMainWorker() 返回 nullptr
    // → 构造 fallback dispatch 版本
    auto exec = WorkerExecutor::Main();
    // 此时没有 main worker，HasWorker() 为 false

    // Dispatch 通过 dispatch_ 调用 PostToMain（main_worker_ 为空则返回 false）
    // inline_fallback=true 会最后兜底
    bool called = false;
    bool ok = exec.Dispatch([&]() { called = true; });
    REQUIRE(ok);
    REQUIRE(called);  // inline_fallback 兜底
}

TEST_CASE("scheduler: WorkerExecutor Current() 无 Worker 时为空", "[scheduler][executor][noworker]")
{
    // 在 headless 模式下 GetCurWorker() 返回 nullptr
    auto exec = WorkerExecutor::Current();
    REQUIRE_FALSE(exec.HasWorker());

    // inline_fallback 兜底
    bool called = false;
    bool ok = exec.Dispatch([&]() { called = true; });
    REQUIRE(ok);
    REQUIRE(called);
}

TEST_CASE("scheduler: WorkerExecutor Worker() 静态工厂", "[scheduler][executor]")
{
    auto empty = WorkerPtr{};
    auto exec = WorkerExecutor::Worker(empty);
    REQUIRE_FALSE(exec.HasWorker());

    // inline_fallback 兜底
    bool called = false;
    bool ok = exec.Dispatch([&]() { called = true; });
    REQUIRE(ok);
    REQUIRE(called);
}

TEST_CASE("scheduler: WorkerExecutor 多次 Dispatch 全部执行", "[scheduler][executor]")
{
    auto dispatch_fn = [](std::function<void()> fn) { fn(); };
    WorkerExecutor exec(dispatch_fn, []() { return false; });

    std::atomic<int> counter{0};
    for (int i = 0; i < 10; i++)
        exec.Dispatch([&]() { counter.fetch_add(1); });
    REQUIRE(counter.load() == 10);
}
