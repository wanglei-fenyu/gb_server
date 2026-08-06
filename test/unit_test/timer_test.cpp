#include <catch2/catch_all.hpp>
#include "worker/worker_manager.h"
#include "base/singleton.h"
#include "log/log.h"

using namespace gb;

// ── TimerManager 直接 C++ 测试 ─────────────────────────────────

// 所有测试都用 shared_ptr 捕获同步对象：
// 回调在 worker 线程异步执行，测试函数返回后栈对象已析构，
// [&] 捕获会留下悬垂引用 → UB（worker 线程可能卡死/崩溃，污染后续测试）。

struct TimerSync
{
    std::atomic<bool> fired{false};
    std::atomic<int>  count{0};
    std::atomic<bool> check_done{false};
    std::mutex        mtx;
    std::condition_variable cv;
};

TEST_CASE("timer: RegisterTimer one-shot fires", "[timer]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto sync = std::make_shared<TimerSync>();

    worker->Post([sync, worker]() {
        auto& timer_mgr = worker->GetTimerManager();
        timer_mgr->RegisterTimer(50, [sync]() {
            sync->fired.store(true);
            std::lock_guard<std::mutex> lk(sync->mtx);
            sync->cv.notify_one();
        });
    });

    {
        std::unique_lock<std::mutex> lk(sync->mtx);
        bool ok = sync->cv.wait_for(lk, std::chrono::seconds(3),
                                    [sync] { return sync->fired.load(); });
        REQUIRE(ok);
    }
    REQUIRE(sync->fired.load());
}

TEST_CASE("timer: RegisterTimer loop fires multiple times", "[timer]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto sync = std::make_shared<TimerSync>();

    worker->Post([sync, worker]() {
        auto& timer_mgr = worker->GetTimerManager();
        // id 用 shared_ptr：回调（worker 线程）里需要取消自己，
        // Post 闭包返回后 id 本体已析构，直接捕获引用会悬垂
        auto id_ptr = std::make_shared<int64_t>(0);
        *id_ptr     = timer_mgr->RegisterTimer(
            30,
            [sync, worker, id_ptr]() {
                int c = ++sync->count;
                if (c >= 3)
                {
                    // 达标立即取消自己，避免测试结束后残留 loop timer
                    // 继续访问已析构的同步对象（悬垂引用 → UB 污染后续测试）
                    worker->GetTimerManager()->UnRegisterTimer(*id_ptr);
                    std::lock_guard<std::mutex> lk(sync->mtx);
                    sync->cv.notify_one();
                }
            },
            true);
    });

    {
        std::unique_lock<std::mutex> lk(sync->mtx);
        bool ok = sync->cv.wait_for(lk, std::chrono::seconds(3),
                                    [sync] { return sync->count.load() >= 3; });
        REQUIRE(ok);
    }
    REQUIRE(sync->count.load() >= 3);
}

TEST_CASE("timer: RegisterSystemTimer fires", "[timer]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto sync = std::make_shared<TimerSync>();

    worker->Post([sync, worker]() {
        auto& timer_mgr = worker->GetTimerManager();
        timer_mgr->RegisterSystemTimer(50, [sync]() {
            sync->fired.store(true);
            std::lock_guard<std::mutex> lk(sync->mtx);
            sync->cv.notify_one();
        });
    });

    {
        std::unique_lock<std::mutex> lk(sync->mtx);
        bool ok = sync->cv.wait_for(lk, std::chrono::seconds(3),
                                    [sync] { return sync->fired.load(); });
        REQUIRE(ok);
    }
    REQUIRE(sync->fired.load());
}

TEST_CASE("timer: UnRegisterTimer prevents firing", "[timer]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto sync = std::make_shared<TimerSync>();

    worker->Post([sync, worker]() {
        auto& timer_mgr = worker->GetTimerManager();
        int64_t id     = timer_mgr->RegisterTimer(50, [sync]() {
            sync->fired.store(true);  // should NOT happen
        });
        timer_mgr->UnRegisterTimer(id);

        // After enough time, signal check_done
        timer_mgr->RegisterTimer(300, [sync]() {
            sync->check_done.store(true);
            std::lock_guard<std::mutex> lk(sync->mtx);
            sync->cv.notify_one();
        });
    });

    {
        std::unique_lock<std::mutex> lk(sync->mtx);
        bool ok = sync->cv.wait_for(lk, std::chrono::seconds(3),
                                    [sync] { return sync->check_done.load(); });
        REQUIRE(ok);
    }
    REQUIRE_FALSE(sync->fired.load());
}

// ── Lua 绑定集成测试 ────────────────────────────────────────────

TEST_CASE("timer: Lua Register callback fires", "[timer][lua]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto script = worker->GetScript();
    REQUIRE(script != nullptr);

    auto sync = std::make_shared<TimerSync>();

    // Register a Lua timer and have it signal C++ when fired
    worker->Post([sync, script]() {
        sol::state_view lua(script->lua_state());
        // Register a C++ callback as a lua global, timer calls it
        lua["__timer_test_cb"] = [sync]() {
            sync->fired.store(true);
            std::lock_guard<std::mutex> lk(sync->mtx);
            sync->cv.notify_one();
        };

        lua.script(R"(
            timer.Register(50, function()
                __timer_test_cb()
            end, false)
        )");
    });

    {
        std::unique_lock<std::mutex> lk(sync->mtx);
        bool ok = sync->cv.wait_for(lk, std::chrono::seconds(3),
                                    [sync] { return sync->fired.load(); });
        REQUIRE(ok);
    }
    REQUIRE(sync->fired.load());
}

TEST_CASE("timer: Lua Register loop & cancel", "[timer][lua]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto script = worker->GetScript();
    REQUIRE(script != nullptr);

    auto sync = std::make_shared<TimerSync>();

    worker->Post([sync, script]() {
        sol::state_view lua(script->lua_state());

        lua["__timer_loop_cb"] = [sync, script]() {
            int c = ++sync->count;
            if (c >= 3)
            {
                // 达标后在 worker 线程内取消 Lua loop timer，防止测试结束后
                // 残留 loop timer 继续访问已析构的同步对象
                sol::state_view lua_view(script->lua_state());
                lua_view["__timer_loop_cancel"]();
                std::lock_guard<std::mutex> lk(sync->mtx);
                sync->cv.notify_one();
            }
        };

        lua.script(R"(
            local loop_id = timer.Register(30, function()
                __timer_loop_cb()
            end, true)

            __timer_loop_cancel = function()
                timer.UnRegister(loop_id)
            end

            -- 兜底：即使 count 未达标，300ms 后也取消
            timer.Register(300, function()
                timer.UnRegister(loop_id)
            end, false)
        )");
    });

    {
        std::unique_lock<std::mutex> lk(sync->mtx);
        bool ok = sync->cv.wait_for(lk, std::chrono::seconds(3),
                                    [sync] { return sync->count.load() >= 3; });
        REQUIRE(ok);
    }
    REQUIRE(sync->count.load() >= 3);
}

TEST_CASE("timer: Lua Register & UnRegister prevents firing", "[timer][lua]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto script = worker->GetScript();
    REQUIRE(script != nullptr);

    auto sync = std::make_shared<TimerSync>();

    worker->Post([sync, script]() {
        sol::state_view lua(script->lua_state());

        lua["__timer_bad_cb"]  = [sync]() { sync->fired.store(true); };
        lua["__timer_done_cb"] = [sync]() {
            sync->check_done.store(true);
            std::lock_guard<std::mutex> lk(sync->mtx);
            sync->cv.notify_one();
        };

        lua.script(R"(
            local id = timer.Register(50, function()
                __timer_bad_cb()
            end, false)
            timer.UnRegister(id)

            -- Signal check after waiting enough time
            timer.Register(300, function()
                __timer_done_cb()
            end, false)
        )");
    });

    {
        std::unique_lock<std::mutex> lk(sync->mtx);
        bool ok = sync->cv.wait_for(lk, std::chrono::seconds(3),
                                    [sync] { return sync->check_done.load(); });
        REQUIRE(ok);
    }
    REQUIRE_FALSE(sync->fired.load());
}
