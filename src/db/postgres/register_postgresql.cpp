module;

#include "worker/worker_manager.h"
#include "script/script.h"
#include "log/log.h"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <mutex>
#include <thread>

module db.postgres;

// ═════════════════════════════════════════════════════════════════════════════
// 全局状态
// ═════════════════════════════════════════════════════════════════════════════

namespace {

/// 全局 PG IO 上下文（供 Lua 全局连接使用）。
boost::asio::io_context g_pg_io_ctx;
/// work_guard 防止 io_context::run() 在无待处理事件时退出。
/// 若无此 guard，g_pg_io_thread 启动后立即退出 → AsyncConnect 等 post 的事件永不执行。
using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
std::unique_ptr<WorkGuard> g_pg_work_guard;
std::thread             g_pg_io_thread;
bool                    g_pg_thread_started = false;

/// 保护 g_pg_conn 的互斥锁。
std::mutex              g_pg_mutex;

/// 全局 PG 连接（通过 AsyncConnect 设置）。
std::shared_ptr<gb::PgConnection> g_pg_conn;

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// 辅助函数 — DbValue / DbResult ↔ Lua 类型转换
// ═════════════════════════════════════════════════════════════════════════════

/// 将 Lua 值转为 DbValue。
static gb::DbValue LuaValueToDbValue(sol::object obj)
{
    if (obj == sol::lua_nil)
        return gb::DbValue(nullptr);

    if (obj.is<bool>())
        return gb::DbValue(obj.as<bool>());

    if (obj.is<int64_t>())
        return gb::DbValue(obj.as<int64_t>());

    if (obj.is<double>())
        return gb::DbValue(obj.as<double>());

    if (obj.is<std::string>())
        return gb::DbValue(obj.as<std::string>());

    if (obj.is<const char*>())
        return gb::DbValue(obj.as<const char*>());

    // fallback: tostring
    return gb::DbValue(
        sol::state_view(obj.lua_state())["tostring"](obj).get<std::string>());
}

/// 将 DbValue 转为 sol::object（用于结果行）。
static sol::object DbValueToLua(lua_State* L, const gb::DbValue& val)
{
    if (val.is_null())
        return sol::lua_nil;

    switch (val.type())
    {
    case gb::DbFieldType::BOOL:
        return sol::make_object(L, val.as_bool());
    case gb::DbFieldType::INT8:
    case gb::DbFieldType::INT16:
    case gb::DbFieldType::INT32:
    case gb::DbFieldType::INT64:
        return sol::make_object(L, val.as_int64());
    case gb::DbFieldType::UINT64:
        return sol::make_object(L, static_cast<double>(val.as_uint64()));
    case gb::DbFieldType::FLOAT:
        return sol::make_object(L, val.as_float());
    case gb::DbFieldType::DOUBLE:
        return sol::make_object(L, val.as_double());
    // NUMERIC/DATE/TIME/TIMESTAMP/UUID/JSON/INET/GEOMETRY/XML/ARRAY/BLOB…
    // 统一作为字符串返回
    default:
        return sol::make_object(L, std::string(val.as_string()));
    }
}

/// 将 DbResult（行结果集）转换为 Lua table array。
/// 每个元素是一个 field_name → value 的 table。
static sol::object DbResultToLua(lua_State* L, const gb::DbResult& result_ref)
{
    // 注意：next() 需要修改游标，因此需要一份可变拷贝。
    // 拷贝开销相对于 SQL 查询可忽略（仅复制 vector<DbRow>）。
    gb::DbResult r = result_ref;

    if (r.is_error())
    {
        LOG_ERROR("[PostgreSQL] query error: {}", r.error().message);
        return sol::lua_nil;
    }

    if (r.rows_count() == 0)
    {
        // 空结果集：返回空 table
        return sol::make_object(L, sol::state_view(L).create_table());
    }

    sol::table result = sol::state_view(L).create_table();
    for (size_t i = 0; i < r.rows_count(); i++)
    {
        gb::DbRow db_row = r.next();   // 推进游标
        sol::table row   = sol::state_view(L).create_table();

        for (size_t j = 0; j < db_row.field_count(); j++)
        {
            const gb::DbField& field = db_row[j];
            row[field.name()] = DbValueToLua(L, field.value());
        }

        result[i + 1] = row;   // Lua 数组从 1 开始
    }
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// 协程回调桥接 — 将 sol::function 从协程 lua_State 迁移到主 Lua 状态
// ═════════════════════════════════════════════════════════════════════════════

/// 将 sol::function 迁移到 Worker 主线程的 lua_State。
///
/// ⚠️ 关键：sol::function 在协程上下文创建时会引用协程的 lua_State，
/// 导致后续 (*cb_ptr)(args) 在协程栈上执行（而非主线程），
/// 从而与 coroutine.yield/resume 产生冲突（尝试 resume 一个 running 的协程）。
/// 这里通过 lua_xmove 强制将函数引用迁移到主 Lua 状态。
///
/// 参考 Redis 的 LuaCbBridge::Create（register_redis.cpp）。
static auto BridgeCallback(gb::WorkerPtr w, sol::function cb)
    -> std::shared_ptr<sol::function>
{
    lua_State* main_L = w ? w->GetScript()->lua_state() : nullptr;
    lua_State* cb_L   = cb.lua_state();

    if (main_L && cb_L && cb_L != main_L)
    {
        // 从协程栈复制函数到主栈
        cb.push();
        lua_xmove(cb_L, main_L, 1);
        // 在主状态上创建 sol::function（registry 引用在主状态上）
        sol::function main_cb(main_L, lua_gettop(main_L));
        lua_pop(main_L, 1);
        return std::make_shared<sol::function>(std::move(main_cb));
    }

    return std::make_shared<sol::function>(std::move(cb));
}

// ═════════════════════════════════════════════════════════════════════════════
// 注册函数
// ═════════════════════════════════════════════════════════════════════════════

void register_postgresql(std::shared_ptr<Script>& scriptPtr)
{
    // ── 启动全局 PG IO 线程 ──────────────────────────────────────────────
    if (!g_pg_thread_started)
    {
        g_pg_thread_started = true;
        g_pg_work_guard = std::make_unique<WorkGuard>(
            boost::asio::make_work_guard(g_pg_io_ctx));
        g_pg_io_thread = std::thread([]() { g_pg_io_ctx.run(); });
        g_pg_io_thread.detach();
    }

    auto pg = scriptPtr->create_table("pg");

    // ═══════════════════════════════════════════════════════════════════════
    // 异步 API — 所有操作不阻塞 Worker 线程
    //
    // 所有 async 方法使用 PgConnection 的内置异步回调 API，
    // 在 PG IO 线程上执行完毕后通过 w->Post() 将结果回调
    // 送回发起调用的 Worker 线程。
    //
    // 回调约定: callback(err, ...)
    //   err = "" 表示成功，非空为错误描述
    // ═══════════════════════════════════════════════════════════════════════

    // ── 异步连接 ──────────────────────────────────────────────────────────

    /// pg.AsyncConnect(config, callback(err, ok))
    ///
    /// 异步连接 PostgreSQL。连接建立后保存为全局连接，供后续 Async* 调用使用。
    ///
    /// @usage
    ///   pg.AsyncConnect({host="127.0.0.1", port=5432, database="test",
    ///                    user="postgres", password=""}, function(err, ok)
    ///       if ok then log.Info("PG connected!") end
    ///   end)
    pg["AsyncConnect"] = [](sol::table cfg, sol::function cb) {
        auto w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!w) return;

        // 解析 Lua 配置表 → DbConfig
        gb::DbConfig config;
        auto read = [&](const char* key, auto& val) {
            sol::object obj = cfg.get<sol::object>(key);
            if (obj != sol::lua_nil && obj.is<decltype(val)>())
                val = obj.as<decltype(val)>();
        };
        read("host",             config.host);
        read("port",             config.port);
        read("user",             config.user);
        read("password",         config.password);
        read("database",         config.database);
        read("use_ssl",          config.use_ssl);
        read("connect_timeout",  config.connect_timeout_sec);

        auto cb_ptr   = BridgeCallback(w, std::move(cb));
        auto weak_w   = std::weak_ptr<gb::Worker>(w);
        auto conn_ptr = std::make_shared<std::shared_ptr<gb::PgConnection>>(
            std::make_shared<gb::PgConnection>(g_pg_io_ctx));

        // 异步连接
        (*conn_ptr)->AsyncConnect(config, [cb_ptr, weak_w, conn_ptr](bool ok) {
            auto w = weak_w.lock();
            if (!w || !cb_ptr->valid()) return;

            w->Post([cb_ptr, ok, conn_ptr]() {
                if (!cb_ptr->valid()) {
                    LOG_ERROR("[PostgreSQL] cb_ptr invalid before call");
                    return;
                }
                if (ok)
                {
                    // 设置全局连接
                    {
                        std::lock_guard<std::mutex> lock(g_pg_mutex);
                        g_pg_conn = *conn_ptr;
                    }
                    try {
                        (*cb_ptr)("", true);
                    } catch (const sol::error& e) {
                        LOG_ERROR("[PostgreSQL] callback sol::error: {}", e.what());
                    } catch (const std::exception& e) {
                        LOG_ERROR("[PostgreSQL] callback std::exception: {}", e.what());
                    } catch (...) {
                        LOG_ERROR("[PostgreSQL] callback unknown exception");
                    }
                }
                else
                {
                    LOG_ERROR("[PostgreSQL] async connect failed");
                    try {
                        (*cb_ptr)("AsyncConnect failed", false);
                    } catch (const sol::error& e) {
                        LOG_ERROR("[PostgreSQL] error callback sol::error: {}", e.what());
                    } catch (const std::exception& e) {
                        LOG_ERROR("[PostgreSQL] error callback std::exception: {}", e.what());
                    } catch (...) {
                        LOG_ERROR("[PostgreSQL] error callback unknown exception");
                    }
                }
            });
        });
    };

    // ── 异步关闭 ──────────────────────────────────────────────────────────

    /// pg.AsyncClose(callback(err))
    pg["AsyncClose"] = [](sol::function cb) {
        auto w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!w) return;
        auto cb_ptr = BridgeCallback(w, std::move(cb));
        auto weak_w = std::weak_ptr<gb::Worker>(w);

        // 取出全局连接并清空
        std::shared_ptr<gb::PgConnection> conn;
        {
            std::lock_guard<std::mutex> lock(g_pg_mutex);
            conn = std::move(g_pg_conn);
        }

        if (conn)
        {
            conn->AsyncClose([cb_ptr, weak_w]() {
                auto w = weak_w.lock();
                if (!w || !cb_ptr->valid()) return;
                w->Post([cb_ptr]() {
                    if (cb_ptr->valid())
                        (*cb_ptr)("");
                });
            });
        }
    };

    // ── 异步查询 ──────────────────────────────────────────────────────────

    /// pg.AsyncQuery(sql, ..., callback(err, rows))
    ///
    /// 异步查询。callback 在发起调用的 Worker 线程上执行。
    /// 最后一个参数是 callback function，之前的参数（若有）为 SQL 参数。
    ///
    /// @usage
    ///   pg.AsyncQuery("SELECT * FROM users", function(err, rows)
    ///       if err ~= "" then log.Error(err); return end
    ///       for _, row in ipairs(rows) do log.Info(row.name) end
    ///   end)
    ///   pg.AsyncQuery("SELECT * FROM users WHERE id = $1", 1, function(err, rows)
    ///       -- ...
    ///   end)
    pg["AsyncQuery"] = [](const std::string& sql, sol::variadic_args args) {
        auto w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!w) return;

        // 最后一个参数是 callback
        if (args.size() == 0) return;
        sol::function cb = args[args.size() - 1].as<sol::function>();

        // 前面的参数是 SQL 参数
        bool has_params = (args.size() > 1);
        auto params = std::make_shared<std::vector<gb::DbValue>>();
        if (has_params)
        {
            params->reserve(args.size() - 1);
            for (size_t i = 0; i < args.size() - 1; i++)
                params->push_back(LuaValueToDbValue(args[i]));
        }

        // 获取全局连接（快照，保持 shared_ptr 使连接在使用期间不析构）
        std::shared_ptr<gb::PgConnection> conn;
        {
            std::lock_guard<std::mutex> lock(g_pg_mutex);
            conn = g_pg_conn;
        }
        if (!conn) return;

        auto cb_ptr = BridgeCallback(w, std::move(cb));
        auto weak_w = std::weak_ptr<gb::Worker>(w);

        if (!has_params)
        {
            conn->AsyncQuery(sql,
                [cb_ptr, weak_w](gb::DbResult res) {
                    auto w = weak_w.lock();
                    if (!w || !cb_ptr->valid()) return;
                    w->Post([cb_ptr, res = std::move(res)]() {
                        if (!cb_ptr->valid()) return;
                        lua_State* L = cb_ptr->lua_state();
                        if (res.is_error())
                            (*cb_ptr)(res.error().message, sol::lua_nil);
                        else
                            (*cb_ptr)("", DbResultToLua(L, res));
                    });
                });
        }
        else
        {
            conn->AsyncQuery(sql, *params,
                [cb_ptr, weak_w](gb::DbResult res) {
                    auto w = weak_w.lock();
                    if (!w || !cb_ptr->valid()) return;
                    w->Post([cb_ptr, res = std::move(res)]() {
                        if (!cb_ptr->valid()) return;
                        lua_State* L = cb_ptr->lua_state();
                        if (res.is_error())
                            (*cb_ptr)(res.error().message, sol::lua_nil);
                        else
                            (*cb_ptr)("", DbResultToLua(L, res));
                    });
                });
        }
    };

    // ── 异步执行 ──────────────────────────────────────────────────────────

    /// pg.AsyncExecute(sql, ..., callback(err, n))
    ///
    /// 异步 DML。callback(n) 返回影响行数。
    ///
    /// @usage
    ///   pg.AsyncExecute("DELETE FROM users WHERE id = $1", 1, function(err, n)
    ///       if err ~= "" then log.Error(err); return end
    ///       log.Info("deleted " .. n .. " rows")
    ///   end)
    pg["AsyncExecute"] = [](const std::string& sql, sol::variadic_args args) {
        auto w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!w) {
            LOG_ERROR("[AsyncExecute] no worker");
            return;
        }

        if (args.size() == 0) {
            LOG_ERROR("[AsyncExecute] no callback arg");
            return;
        }
        sol::function cb = args[args.size() - 1].as<sol::function>();

        bool has_params = (args.size() > 1);
        auto params = std::make_shared<std::vector<gb::DbValue>>();
        if (has_params)
        {
            params->reserve(args.size() - 1);
            for (size_t i = 0; i < args.size() - 1; i++)
                params->push_back(LuaValueToDbValue(args[i]));
        }

        std::shared_ptr<gb::PgConnection> conn;
        {
            std::lock_guard<std::mutex> lock(g_pg_mutex);
            conn = g_pg_conn;
        }
        if (!conn) {
            LOG_ERROR("[AsyncExecute] no connection");
            return;
        }

        auto cb_ptr = BridgeCallback(w, std::move(cb));
        auto weak_w = std::weak_ptr<gb::Worker>(w);

        if (!has_params)
        {
            // 简单 SQL → AsyncExecute
            conn->AsyncExecute(sql,
                [cb_ptr, weak_w](uint64_t n) {
                    auto w = weak_w.lock();
                    if (!w || !cb_ptr->valid()) return;
                    w->Post([cb_ptr, n]() {
                        if (cb_ptr->valid())
                            (*cb_ptr)("", n);
                    });
                });
        }
        else
        {
            // 参数化 → 走 AsyncQuery + 取 affected_rows
            // PgConnection 的 Execute 只支持无参数版本，参数化查询使用 Query
            conn->AsyncQuery(sql, *params,
                [cb_ptr, weak_w](gb::DbResult res) {
                    auto w = weak_w.lock();
                    if (!w || !cb_ptr->valid()) return;
                    w->Post([cb_ptr, res = std::move(res)]() {
                        if (!cb_ptr->valid()) return;
                        if (res.is_error())
                            (*cb_ptr)(res.error().message, 0);
                        else
                            (*cb_ptr)("", static_cast<uint64_t>(res.affected_rows()));
                    });
                });
        }
    };

    // ── 异步事务 ──────────────────────────────────────────────────────────

    /// pg.AsyncBegin(callback(err, ok))
    pg["AsyncBegin"] = [](sol::function cb) {
        auto w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!w) return;

        std::shared_ptr<gb::PgConnection> conn;
        {
            std::lock_guard<std::mutex> lock(g_pg_mutex);
            conn = g_pg_conn;
        }
        if (!conn) return;

        auto cb_ptr = BridgeCallback(w, std::move(cb));
        auto weak_w = std::weak_ptr<gb::Worker>(w);

        conn->AsyncBegin([cb_ptr, weak_w](bool ok) {
            auto w = weak_w.lock();
            if (!w || !cb_ptr->valid()) return;
            w->Post([cb_ptr, ok]() {
                if (cb_ptr->valid())
                    (*cb_ptr)(ok ? "" : "BEGIN failed", ok);
            });
        });
    };

    /// pg.AsyncCommit(callback(err, ok))
    pg["AsyncCommit"] = [](sol::function cb) {
        auto w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!w) return;

        std::shared_ptr<gb::PgConnection> conn;
        {
            std::lock_guard<std::mutex> lock(g_pg_mutex);
            conn = g_pg_conn;
        }
        if (!conn) return;

        auto cb_ptr = BridgeCallback(w, std::move(cb));
        auto weak_w = std::weak_ptr<gb::Worker>(w);

        conn->AsyncCommit([cb_ptr, weak_w](bool ok) {
            auto w = weak_w.lock();
            if (!w || !cb_ptr->valid()) return;
            w->Post([cb_ptr, ok]() {
                if (cb_ptr->valid())
                    (*cb_ptr)(ok ? "" : "COMMIT failed", ok);
            });
        });
    };

    /// pg.AsyncRollback(callback(err, ok))
    pg["AsyncRollback"] = [](sol::function cb) {
        auto w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!w) return;

        std::shared_ptr<gb::PgConnection> conn;
        {
            std::lock_guard<std::mutex> lock(g_pg_mutex);
            conn = g_pg_conn;
        }
        if (!conn) return;

        auto cb_ptr = BridgeCallback(w, std::move(cb));
        auto weak_w = std::weak_ptr<gb::Worker>(w);

        conn->AsyncRollback([cb_ptr, weak_w](bool ok) {
            auto w = weak_w.lock();
            if (!w || !cb_ptr->valid()) return;
            w->Post([cb_ptr, ok]() {
                if (cb_ptr->valid())
                    (*cb_ptr)(ok ? "" : "ROLLBACK failed", ok);
            });
        });
    };

    // ── 同步查询（仅连接状态） ────────────────────────────────────────────

    /// pg.IsConnected() → boolean
    pg["IsConnected"] = []() -> bool {
        std::lock_guard<std::mutex> lock(g_pg_mutex);
        return g_pg_conn && g_pg_conn->IsConnected();
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Lua 协程桥接 — pg.Await(method, ...)
    //
    // 允许在 Lua 协程中以同步风格写异步代码：
    //
    //   local co = coroutine.create(function()
    //       local err, rows = pg.Await("Query", "SELECT * FROM users")
    //       for _, row in ipairs(rows) do log.Info(row.name) end
    //   end)
    //   coroutine.resume(co)
    //
    // 原理：Await 调用对应的 AsyncXxx 方法，传入一个回调；
    // 回调保存结果并 coroutine.resume 恢复协程；
    // Await 调用 coroutine.yield 挂起等待。
    // ═══════════════════════════════════════════════════════════════════════

    lua_State* L = pg.lua_state();
    luaL_dostring(L, R"(
        if not pg.Await then
            function pg.Await(method, ...)
                local co = coroutine.running()
                if not co then
                    error("pg.Await() must be called from a coroutine")
                end

                local args = { ... }
                local results = nil
                local yielded = false

                local function cb(...)
                    results = { ... }
                    if yielded then
                        coroutine.resume(co)
                    end
                end

                args[#args + 1] = cb
                local async_fn = pg["Async" .. method]
                if not async_fn then
                    error("Unknown async method: pg.Async" .. method)
                end
                async_fn(table.unpack(args))

                if results == nil then
                    yielded = true
                    coroutine.yield()
                    yielded = false
                end

                return table.unpack(results)
            end
        end
    )");

    LOG_INFO("PostgreSQL Lua API registered (async + await)");
}
