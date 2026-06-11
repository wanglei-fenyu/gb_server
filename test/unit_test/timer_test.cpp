#include <catch2/catch_all.hpp>
#include "worker/worker_manager.h"
#include "base/singleton.h"
#include "log/log.h"

using namespace gb;

// ── TimerManager 直接 C++ 测试 ─────────────────────────────────

TEST_CASE("timer: RegisterTimer one-shot fires", "[timer]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto& timer_mgr = worker->GetTimerManager();
    REQUIRE(timer_mgr != nullptr);

    std::atomic<bool> fired{false};
    std::mutex        mtx;
    std::condition_variable cv;

    worker->Post([&]() {
        timer_mgr->RegisterTimer(50, [&]() {
            fired.store(true);
            std::lock_guard<std::mutex> lk(mtx);
            cv.notify_one();
        });
    });

    {
        std::unique_lock<std::mutex> lk(mtx);
        bool ok = cv.wait_for(lk, std::chrono::seconds(3),
                              [&fired] { return fired.load(); });
        REQUIRE(ok);
    }
    REQUIRE(fired.load());
}

TEST_CASE("timer: RegisterTimer loop fires multiple times", "[timer]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto& timer_mgr = worker->GetTimerManager();
    REQUIRE(timer_mgr != nullptr);

    std::atomic<int>  count{0};
    std::mutex        mtx;
    std::condition_variable cv;

    worker->Post([&]() {
        int64_t id = timer_mgr->RegisterTimer(30, [&]() {
            int c = ++count;
            if (c >= 3)
            {
                std::lock_guard<std::mutex> lk(mtx);
                cv.notify_one();
            }
        }, true);

        // Cancel after 5 ticks via a one-shot timer
        timer_mgr->RegisterTimer(500, [&, id]() {
            timer_mgr->UnRegisterTimer(id);
        });
    });

    {
        std::unique_lock<std::mutex> lk(mtx);
        bool ok = cv.wait_for(lk, std::chrono::seconds(3),
                              [&count] { return count.load() >= 3; });
        REQUIRE(ok);
    }
    REQUIRE(count.load() >= 3);
}

TEST_CASE("timer: RegisterSystemTimer fires", "[timer]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto& timer_mgr = worker->GetTimerManager();
    REQUIRE(timer_mgr != nullptr);

    std::atomic<bool> fired{false};
    std::mutex        mtx;
    std::condition_variable cv;

    worker->Post([&]() {
        timer_mgr->RegisterSystemTimer(50, [&]() {
            fired.store(true);
            std::lock_guard<std::mutex> lk(mtx);
            cv.notify_one();
        });
    });

    {
        std::unique_lock<std::mutex> lk(mtx);
        bool ok = cv.wait_for(lk, std::chrono::seconds(3),
                              [&fired] { return fired.load(); });
        REQUIRE(ok);
    }
    REQUIRE(fired.load());
}

TEST_CASE("timer: UnRegisterTimer prevents firing", "[timer]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto& timer_mgr = worker->GetTimerManager();
    REQUIRE(timer_mgr != nullptr);

    std::atomic<bool> fired{false};
    std::atomic<bool> check_done{false};
    std::mutex        mtx;
    std::condition_variable cv;

    worker->Post([&]() {
        int64_t id = timer_mgr->RegisterTimer(50, [&]() {
            fired.store(true);  // should NOT happen
        });
        timer_mgr->UnRegisterTimer(id);

        // After enough time, signal check_done
        timer_mgr->RegisterTimer(300, [&]() {
            check_done.store(true);
            std::lock_guard<std::mutex> lk(mtx);
            cv.notify_one();
        });
    });

    {
        std::unique_lock<std::mutex> lk(mtx);
        bool ok = cv.wait_for(lk, std::chrono::seconds(3),
                              [&check_done] { return check_done.load(); });
        REQUIRE(ok);
    }
    REQUIRE_FALSE(fired.load());
}

// ── Lua 绑定集成测试 ────────────────────────────────────────────

TEST_CASE("timer: Lua Register callback fires", "[timer][lua]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto script = worker->GetScript();
    REQUIRE(script != nullptr);

    std::atomic<bool> fired{false};
    std::mutex        mtx;
    std::condition_variable cv;

    // Register a Lua timer and have it signal C++ when fired
    worker->Post([&]() {
        sol::state_view lua(script->lua_state());
        // Register a C++ callback as a lua global, timer calls it
        lua["__timer_test_cb"] = [&]() {
            fired.store(true);
            std::lock_guard<std::mutex> lk(mtx);
            cv.notify_one();
        };

        lua.script(R"(
            timer.Register(50, function()
                __timer_test_cb()
            end, false)
        )");
    });

    {
        std::unique_lock<std::mutex> lk(mtx);
        bool ok = cv.wait_for(lk, std::chrono::seconds(3),
                              [&fired] { return fired.load(); });
        REQUIRE(ok);
    }
    REQUIRE(fired.load());
}

TEST_CASE("timer: Lua Register loop & cancel", "[timer][lua]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto script = worker->GetScript();
    REQUIRE(script != nullptr);

    std::atomic<int>  count{0};
    std::mutex        mtx;
    std::condition_variable cv;

    worker->Post([&]() {
        sol::state_view lua(script->lua_state());

        lua["__timer_loop_cb"] = [&]() {
            int c = ++count;
            if (c >= 3)
            {
                std::lock_guard<std::mutex> lk(mtx);
                cv.notify_one();
            }
        };

        lua.script(R"(
            local id = timer.Register(30, function()
                __timer_loop_cb()
            end, true)

            -- Cancel after enough time
            timer.Register(500, function()
                timer.UnRegister(id)
            end, false)
        )");
    });

    {
        std::unique_lock<std::mutex> lk(mtx);
        bool ok = cv.wait_for(lk, std::chrono::seconds(3),
                              [&count] { return count.load() >= 3; });
        REQUIRE(ok);
    }
    REQUIRE(count.load() >= 3);
}

TEST_CASE("timer: Lua Register & UnRegister prevents firing", "[timer][lua]")
{
    auto worker = WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);

    auto script = worker->GetScript();
    REQUIRE(script != nullptr);

    std::atomic<bool> bad_fired{false};
    std::atomic<bool> check_done{false};
    std::mutex        mtx;
    std::condition_variable cv;

    worker->Post([&]() {
        sol::state_view lua(script->lua_state());

        lua["__timer_bad_cb"]  = [&]() { bad_fired.store(true); };
        lua["__timer_done_cb"] = [&]() {
            check_done.store(true);
            std::lock_guard<std::mutex> lk(mtx);
            cv.notify_one();
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
        std::unique_lock<std::mutex> lk(mtx);
        bool ok = cv.wait_for(lk, std::chrono::seconds(3),
                              [&check_done] { return check_done.load(); });
        REQUIRE(ok);
    }
    REQUIRE_FALSE(bad_fired.load());
}
