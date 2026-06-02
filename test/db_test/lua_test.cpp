#include "lua_test.h"
#include "report.h"
#include "log/log.h"
#include "worker/worker_manager.h"
#include <sol/sol.hpp>
#include <filesystem>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

// ====================================================================
// Lua Redis / PostgreSQL 绑定测试
//
// 通过 WorkerManager 初始化生产环境的 register_redis、register_postgresql
// 等 C++→Lua 绑定（即 _lua_() 注册的全部内容），然后用绝对路径加载
// script/db_test_lua_redis_pg.lua 并执行其中的 test_* 函数。
//
// 原理：
//   1. WorkerManager::InitMainWorker() 创建 MainWorker、设置
//      thread_local tl_current_worker（GetCurWorker 依赖它）。
//   2. Worker::OnStart() → InitLua() → _lua_(scriptPtr) 注册所有绑定。
//   3. Lua 侧调用 redis.Async* / pg.Async* 时，回调通过 LuaCbBridge
//      → Worker::Post 投递回 MainWorker 队列。
//   4. C++ 侧轮询 Worker::ProcessFrame() 来 drain 队列中的回调，
//      直到 Lua 侧的 .done 状态为 true。
//
//   InitLua() 会尝试加载 LuaSocket 等文件（require("socket.core")），
//   若不可用会抛异常。但此时 runing_ 和 tl_current_worker 已设置、
//   绑定已注册完毕，Worker 仍可正常使用。我们在外部 try-catch 捕获即可。
// ====================================================================

int MenuTestLuaScriptBindings()
{
    TestSection("Lua Redis / PostgreSQL Binding Tests");
    int failures = 0;
    int total    = 0;

    // ── 1. 初始化 WorkerManager ────────────────────────────────────────
    //     创建 MainWorker，设置 tl_current_worker，注册所有 C++→Lua 绑定。
    //     InitLua 中的 require("socket.core") 可能抛异常，但绑定已注册完毕。
    std::cout << "  [setup] Initializing WorkerManager..." << std::endl;
    try
    {
        gb::WorkerManager::Instance()->InitMainWorker();
    }
    catch (const std::exception& e)
    {
        // InitLua 部分失败（如 LuaSocket 不可用），但绑定已经注册、Worker 可用
        LOG_WARN("WorkerManager InitMainWorker partial: {}", e.what());
        std::cout << "  [setup] (non-fatal) " << e.what() << std::endl;
    }

    auto worker = gb::WorkerManager::Instance()->GetMainWorker();
    if (!worker)
    {
        TestResult("WorkerManager init", false, "no main worker created");
        return 1;
    }
    auto scriptPtr = worker->GetScript();
    if (!scriptPtr)
    {
        TestResult("WorkerManager init", false, "worker has no Lua state");
        return 1;
    }

    // ── 2. 以绝对路径加载 Lua 测试脚本 ─────────────────────────────────
    //     __FILE__ = test/db_test/lua_test.cpp → 上三层到项目根 → script/
    namespace fs = std::filesystem;
    fs::path script_path = fs::absolute(
        fs::path(__FILE__).parent_path()  // test/db_test/
            .parent_path()                // test/
            .parent_path()                // <project_root>
        / "script" / "db_test_lua_redis_pg.lua");

    std::cout << "  [setup] Loading Lua script: " << script_path << std::endl;
    scriptPtr->Load(script_path.string());

    // ── 3. 辅助函数 ────────────────────────────────────────────────────

    /// 轮询 ProcessFrame 直到 Lua state_table.done == true 或超时。
    auto wait_for_done = [&](const std::string& tbl, int timeout_ms) -> bool {
        auto start = std::chrono::steady_clock::now();
        while (true)
        {
            worker->ProcessFrame(0.f);

            // sol3: 读取 Lua 全局变量 tbl.done
            sol::object done_obj = (*scriptPtr)[tbl]["done"];
            if (done_obj.is<bool>() && done_obj.as<bool>())
                return true;

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > timeout_ms)
                return false;

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    /// 调用 Lua test_* 函数，等待回调完成，报告结果。
    auto run_test = [&](const std::string& label,
                        const std::string& func_name,
                        const std::string& state_tbl,
                        int timeout_ms = 1000) -> bool {
        ++total;

        auto fn = scriptPtr->get<sol::protected_function>(func_name);
        if (!fn.valid())
        {
            TestResult(label, false, "function '" + func_name + "' not found in Lua state");
            ++failures;
            return false;
        }

        auto result = fn();
        if (!result.valid())
        {
            sol::error err = result;
            TestResult(label, false, std::string("lua call error: ") + err.what());
            ++failures;
            return false;
        }

        if (!wait_for_done(state_tbl, timeout_ms))
        {
            TestResult(label, false, "timeout (" + std::to_string(timeout_ms) + "ms) waiting for callback");
            ++failures;
            return false;
        }

        // 读取结果（sol3 安全访问）
        sol::object state_obj = (*scriptPtr)[state_tbl];
        std::string msg;
        bool        ok    = false;
        if (state_obj.is<sol::table>())
        {
            sol::table state          = state_obj.as<sol::table>();
            bool       ok_default     = false;
            std::string msg_default;
            ok  = state["ok"].get_or(ok_default);
            msg = state["msg"].get_or(msg_default);
        }
        else
        {
            msg = "state table '" + state_tbl + "' not found in Lua";
        }
        TestResult(label, ok, msg.empty() ? (ok ? "ok" : "FAILED") : msg);
        if (!ok) ++failures;
        return ok;
    };

    // ═══════════════════════════════════════════════════════════════════
    // Redis Lua 绑定测试
    // ═══════════════════════════════════════════════════════════════════
    std::cout << "\n  --- Redis Lua binding tests ---" << std::endl;

    // 第一个 async 调用会自动调用 RedisConnectionPool::Init()（默认 127.0.0.1:6379）
    run_test("Redis AsyncPing",             "test_redis_ping",       "redis_state");
    run_test("Redis AsyncSet/AsyncGet",     "test_redis_set_get",    "redis_state");
    run_test("Redis AsyncHSet/AsyncHGet",   "test_redis_hash",       "redis_state");
    run_test("Redis AsyncCall PING",        "test_redis_async_call", "redis_state");
    run_test("Redis AsyncIncr",             "test_redis_incr",       "redis_state");
    run_test("Redis AsyncExpire/AsyncTTL",  "test_redis_expire_ttl", "redis_state");
    run_test("Redis AsyncExists",           "test_redis_exists",     "redis_state");
    run_test("Redis AsyncLPush/AsyncLPop",  "test_redis_list",       "redis_state");

    // ═══════════════════════════════════════════════════════════════════
    // PostgreSQL Lua 绑定测试
    // ═══════════════════════════════════════════════════════════════════
    std::cout << "\n  --- PostgreSQL Lua binding tests ---" << std::endl;

    run_test("PG AsyncConnect/AsyncQuery",  "test_pg_connect_query", "pg_state", 10000);
    run_test("PG AsyncExecute (DDL+DML)",   "test_pg_execute",       "pg_state");
    run_test("PG AsyncExecute (UPDATE)",    "test_pg_update",        "pg_state");
    run_test("PG AsyncExecute (DELETE)",    "test_pg_delete",        "pg_state");
    run_test("PG Cleanup (DROP TABLE)",     "test_pg_cleanup",       "pg_state");

    // ═══════════════════════════════════════════════════════════════════
    // Lua 协程桥接测试（使用 redis.Await / pg.Await）
    // ═══════════════════════════════════════════════════════════════════
    std::cout << "\n  --- Redis Lua coroutine tests ---" << std::endl;

    run_test("Redis coro AsyncPing",             "test_redis_ping_coro",       "redis_state");
    run_test("Redis coro AsyncSet/AsyncGet",     "test_redis_set_get_coro",    "redis_state");
    run_test("Redis coro AsyncHSet/AsyncHGet",   "test_redis_hash_coro",       "redis_state");
    run_test("Redis coro AsyncIncr",             "test_redis_incr_coro",       "redis_state");
    run_test("Redis coro AsyncExpire/AsyncTTL",  "test_redis_expire_ttl_coro", "redis_state");
    run_test("Redis coro AsyncLPush/AsyncLPop",  "test_redis_list_coro",       "redis_state");

    std::cout << "\n  --- PostgreSQL Lua coroutine tests ---" << std::endl;

    run_test("PG coro AsyncConnect/AsyncQuery",   "test_pg_connect_query_coro", "pg_state", 1000);
    run_test("PG coro AsyncExecute (DDL+DML)",    "test_pg_execute_coro",       "pg_state");
    run_test("PG coro AsyncExecute (UPDATE)",     "test_pg_update_coro",        "pg_state");
    run_test("PG coro AsyncExecute (DELETE)",     "test_pg_delete_coro",        "pg_state");
    run_test("PG coro Cleanup (DROP TABLE)",      "test_pg_cleanup_coro",       "pg_state");

    // ═══════════════════════════════════════════════════════════════════
    // 汇总
    // ═══════════════════════════════════════════════════════════════════
    int passed = total - failures;
    TestResult("Lua Redis/PG Binding Tests (total)", failures == 0,
               std::to_string(passed) + "/" + std::to_string(total) + " passed");
    return failures;
}
