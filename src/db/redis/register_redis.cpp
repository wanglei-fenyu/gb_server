#include "register_redis.h"
#include "redis_connection.h"
#include "redis_pool.h"
#include "worker/worker_manager.h"
#include "log/log.h"
#include <mutex>

// ═════════════════════════════════════════════════════════════════════════════
// 全局 Redis 连接池（所有 Worker 共享）
//
// ⚠️ 堆分配 + 永不销毁 — 避免静态析构顺序问题。
// 程序退出时由 OS 回收，连接在受控关闭阶段通过 CloseRedisPool() 提前关闭。
// ═════════════════════════════════════════════════════════════════════════════

static RedisConnectionPool& GetRedisPool()
{
    static RedisConnectionPool* pool = new RedisConnectionPool();
    return *pool;
}

void CloseRedisPool()
{
    GetRedisPool().CloseAll();
}

// ═════════════════════════════════════════════════════════════════════════════
// 泛型命令辅助函数 — RESP3 → Lua 类型转换
// ═════════════════════════════════════════════════════════════════════════════

/// 将 RESP3 节点树（前序遍历）转换为 sol::object。
static auto NodesToLua(
    lua_State* L,
    const std::vector<boost::redis::resp3::node>& nodes,
    std::size_t idx)
    -> std::pair<sol::object, std::size_t>
{
    using boost::redis::resp3::type;

    if (idx >= nodes.size())
        return {sol::lua_nil, idx};

    const auto& node = nodes[idx];
    std::size_t next = idx + 1;

    if (boost::redis::resp3::is_aggregate(node.data_type))
    {
        switch (node.data_type)
        {
        case type::array:
        case type::set:
        case type::push:
        {
            sol::table t = sol::state_view(L).create_table();
            for (std::size_t j = 0; j < node.aggregate_size; j++)
            {
                auto pair = NodesToLua(L, nodes, next);
                t[j + 1] = pair.first;
                next = pair.second;
            }
            return {t, next};
        }
        case type::map:
        case type::attribute:
        {
            sol::table t = sol::state_view(L).create_table();
            for (std::size_t j = 0; j < node.aggregate_size; j++)
            {
                auto key_pair = NodesToLua(L, nodes, next);
                auto val_pair = NodesToLua(L, nodes, key_pair.second);
                t[key_pair.first] = val_pair.first;
                next = val_pair.second;
            }
            return {t, next};
        }
        default:
            return {sol::lua_nil, next};
        }
    }
    else
    {
        switch (node.data_type)
        {
        case type::null:
            return {sol::lua_nil, next};
        case type::number:
            try { return {sol::make_object(L, std::stoll(node.value)), next}; }
            catch (...) { return {sol::make_object(L, std::stod(node.value)), next}; }
        case type::boolean:
            return {sol::make_object(L,
                    node.value == "t" || node.value == "true" || node.value == "1"), next};
        case type::doublean:
            return {sol::make_object(L, std::stod(node.value)), next};
        case type::simple_error:
        case type::blob_error:
            LOG_ERROR("Redis error: {}", node.value);
            return {sol::lua_nil, next};
        case type::big_number:
        default:
            return {sol::make_object(L, node.value), next};
        }
    }
}

/// 将 GenericResponse 转换为 sol::object。
static auto GenericRespToLua(lua_State* L,
                             const RedisConnection::GenericResponse& resp)
    -> sol::object
{
    if (!resp.has_value())
    {
        LOG_ERROR("Redis generic response error: type={} diagnostic={}",
                  static_cast<int>(resp.error().data_type),
                  resp.error().diagnostic);
        return sol::lua_nil;
    }

    const auto& nodes = resp.value();
    if (nodes.empty())
        return sol::lua_nil;

    auto obj_pair = NodesToLua(L, nodes, 0);
    return obj_pair.first;
}

static std::once_flag& GetRedisInitFlag()
{
    static std::once_flag flag;
    return flag;
}

/// 从 Lua config table 解析 RedisConfig。
static RedisConfig ParseConfig(const sol::table& cfg)
{
    RedisConfig config;

    auto read = [&](const char* key, auto& val) {
        sol::object obj = cfg.get<sol::object>(key);
        if (obj != sol::lua_nil && obj.is<decltype(val)>())
            val = obj.as<decltype(val)>();
    };

    read("host",       config.host);
    read("port",       config.port);
    read("password",   config.password);
    read("db",         config.db_index);
    read("pool_size",  config.pool_size);
    read("timeout",    config.timeout_ms);

    return config;
}

/// 从 Lua 获取连接（内部辅助）。
static RedisConnection* GetConn()
{
    std::call_once(GetRedisInitFlag(), []() {
        RedisConfig default_cfg;
        LOG_WARN("redis.Connect() not called, using default config {}:{}",
                 default_cfg.host, default_cfg.port);
        GetRedisPool().Init(default_cfg);
    });
    return GetRedisPool().GetConnection();
}

// ═════════════════════════════════════════════════════════════════════════════
// 辅助：将 Worker 线程回调桥接到 Worker 队列
// ═════════════════════════════════════════════════════════════════════════════

/// Lua 异步回调包装。
/// 在 IO 线程上收到回调，通过 Worker::Post 桥接到目标 Worker 线程。
class LuaCbBridge
{
public:
    /// 创建回调桥。shared_ptr 捕获 Worker 和 Lua function。
    ///
    /// ⚠️ 关键：sol::function 在协程上下文创建时会引用协程的 lua_State，
    /// 导致后续 (*cb_)(args) 在协程栈上执行（而非主线程），
    /// 从而与 coroutine.yield/resume 产生死锁。
    /// 这里通过 lua_xmove 强制将函数引用迁移到主 Lua 状态。
    static auto Create(gb::WorkerPtr w, sol::function cb)
        -> std::shared_ptr<LuaCbBridge>
    {
        auto b = std::make_shared<LuaCbBridge>();

        // 获取 Worker 主 Lua 状态（迁移目标）
        lua_State* main_L = w ? w->GetScript()->lua_state() : nullptr;

        if (main_L)
        {
            lua_State* coro_L = cb.lua_state();
            LOG_INFO("[LuaCbBridge::Create] main_L={} coro_L={} same={}",
                     static_cast<void*>(main_L), static_cast<void*>(coro_L),
                     (coro_L == main_L) ? "yes" : "no");
            if (coro_L && coro_L != main_L)
            {
                // 从协程栈复制函数到主栈
                cb.push();
                LOG_INFO("[LuaCbBridge::Create] after push, coro_L stack top={} ivalid={}",
                         lua_gettop(coro_L), cb.valid() ? "valid" : "invalid");
                lua_xmove(coro_L, main_L, 1);
                LOG_INFO("[LuaCbBridge::Create] after xmove, main_L stack top={}",
                         lua_gettop(main_L));
                // 在主状态上创建 sol::function（registry 引用在主状态上）
                sol::function main_cb(main_L, lua_gettop(main_L));
                lua_pop(main_L, 1);
                b->cb_ = std::make_shared<sol::function>(std::move(main_cb));
                LOG_INFO("[LuaCbBridge::Create] xmove path DONE, cb_ valid={}",
                         b->cb_->valid() ? "yes" : "no");
            }
            else
            {
                b->cb_ = std::make_shared<sol::function>(std::move(cb));
                LOG_INFO("[LuaCbBridge::Create] direct path (same state), cb_ valid={}",
                         b->cb_->valid() ? "yes" : "no");
            }
        }
        else
        {
            b->cb_ = std::make_shared<sol::function>(std::move(cb));
            LOG_INFO("[LuaCbBridge::Create] no main_L path");
        }

        b->worker_ = std::move(w);
        return b;
    }

    /// 无值回调: 仅传递 ec。
    void PostCb(boost::system::error_code ec)
    {
        auto w = worker_.lock();
        if (!w) return;
        auto cb_ptr = cb_;
        w->Post([cb_ptr, ec]() {
            if (cb_ptr->valid())
                (*cb_ptr)(ec ? ec.message() : "");
        });
    }

    /// 有值回调: 传递 ec + 值。
    template <typename T>
    void PostCb(boost::system::error_code ec, T val)
    {
        auto w = worker_.lock();
        if (!w) return;
        auto cb_ptr = cb_;
        w->Post([cb_ptr, ec, val = std::move(val)]() mutable {
            if (cb_ptr->valid())
            {
                if (ec)
                    (*cb_ptr)(ec.message(), std::move(val));
                else
                    (*cb_ptr)("", std::move(val));
            }
        });
    }

    /// 字符串回调。
    void PostCbStr(boost::system::error_code ec, std::string val)
    {
        auto w = worker_.lock();
        if (!w) return;
        auto cb_ptr = cb_;
        w->Post([cb_ptr, ec, val = std::move(val)]() mutable {
            if (cb_ptr->valid())
            {
                if (ec) (*cb_ptr)(ec.message(), "");
                else    (*cb_ptr)("", std::move(val));
            }
        });
    }

    /// 布尔回调。
    void PostCbBool(boost::system::error_code ec, bool val)
    {
        auto w = worker_.lock();
        if (!w) { LOG_WARN("[PostCbBool] worker expired"); return; }
        auto cb_ptr = cb_;
        LOG_INFO("[PostCbBool] posting to worker: cb_valid={} ec={} val={}",
                 cb_ptr->valid() ? "yes" : "no",
                 ec ? ec.message() : "none",
                 static_cast<int>(val));
        w->Post([cb_ptr, ec, val]() {
            LOG_INFO("[PostCbBool] lambda ON WORKER: cb_valid={}", cb_ptr->valid() ? "yes" : "no");
            if (cb_ptr->valid())
            {
                sol::protected_function_result result;
                if (ec)
                    result = (*cb_ptr)(ec.message(), "");
                else
                    result = (*cb_ptr)("", true);
                if (!result.valid())
                {
                    sol::error err = result;
                    LOG_ERROR("[PostCbBool] callback LUA ERROR: {}", err.what());
                }
                else
                {
                    LOG_INFO("[PostCbBool] lambda DONE ok");
                }
            }
        });
    }

    /// 向量回调。
    void PostCbStrVec(boost::system::error_code ec, std::vector<std::string> val)
    {
        auto w = worker_.lock();
        if (!w) return;
        auto cb_ptr = cb_;
        w->Post([cb_ptr, ec, val = std::move(val)]() mutable {
            if (cb_ptr->valid())
            {
                if (ec) (*cb_ptr)(ec.message(), sol::nil);
                else    (*cb_ptr)("", sol::as_table(std::move(val)));
            }
        });
    }

    /// 双精度回调。
    void PostCbDouble(boost::system::error_code ec, double val)
    {
        auto w = worker_.lock();
        if (!w) return;
        auto cb_ptr = cb_;
        w->Post([cb_ptr, ec, val]() {
            if (cb_ptr->valid())
            {
                if (ec) (*cb_ptr)(ec.message(), -1.0);
                else    (*cb_ptr)("", val);
            }
        });
    }

    /// 泛型回调（GenericResponse → Lua）。
    void PostCbGeneric(boost::system::error_code ec,
                       RedisConnection::GenericResponse resp)
    {
        auto w = worker_.lock();
        if (!w) return;
        auto cb_ptr = cb_;
        w->Post([cb_ptr, ec, resp = std::move(resp)]() mutable {
            if (!cb_ptr->valid()) return;
            if (ec)
                (*cb_ptr)(ec.message(), sol::lua_nil);
            else
            {
                lua_State* L = cb_ptr->lua_state();
                (*cb_ptr)("", GenericRespToLua(L, resp));
            }
        });
    }

private:
    gb::WorkerWeakPtr                worker_;
    std::shared_ptr<sol::function>   cb_;
};

// ═════════════════════════════════════════════════════════════════════════════
// 注册函数
// ═════════════════════════════════════════════════════════════════════════════

void register_redis(std::shared_ptr<Script>& scriptPtr)
{
    auto redis = scriptPtr->create_table("redis");

    // ── 连接 ──────────────────────────────────────────────────────────────

    /// redis.Connect({host="...", port=6379, password="", db=0,
    ///                 pool_size=4, timeout=5000})
    /// @return boolean 是否连接成功（第一次调用后忽略后续参数）
    redis["Connect"] = [](sol::table cfg) -> bool {
        RedisConfig config = ParseConfig(cfg);
        bool        ok     = false;
        std::call_once(GetRedisInitFlag(), [&]() {
            ok = GetRedisPool().Init(config);
        });
        if (!ok)
            ok = GetRedisPool().IsHealthy();
        return ok;
    };

    /// redis.IsHealthy() → boolean
    redis["IsHealthy"] = []() -> bool {
        return GetRedisPool().IsHealthy();
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 异步回调接口
    //
    // 所有回调签名: callback(err, [val...])
    //   err = "" 表示成功，非空为错误描述
    //   回调在发起调用的 Worker 线程上执行
    // ═══════════════════════════════════════════════════════════════════════

    // ── KV 异步 ──

    /// redis.AsyncSet(key, value, callback(err))
    redis["AsyncSet"] = [](const std::string& key, const std::string& value,
                           sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncSet(key, value, [bridge](boost::system::error_code ec) {
            bridge->PostCb(std::move(ec));
        });
    };

    /// redis.AsyncSetEx(key, value, ttl, callback(err))
    redis["AsyncSetEx"] = [](const std::string& key, const std::string& value,
                             int64_t ttl, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncSetEx(key, value, ttl, [bridge](boost::system::error_code ec) {
            bridge->PostCb(std::move(ec));
        });
    };

    /// redis.AsyncGet(key, callback(err, val))
    redis["AsyncGet"] = [](const std::string& key, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncGet(key, [bridge](boost::system::error_code ec, std::string val) {
            bridge->PostCbStr(std::move(ec), std::move(val));
        });
    };

    /// redis.AsyncDel(key, callback(err, n))
    redis["AsyncDel"] = [](const std::string& key, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncDel(key, [bridge](boost::system::error_code ec, int64_t n) {
            bridge->PostCb(std::move(ec), n);
        });
    };

    /// redis.AsyncExists(key, callback(err, exists))
    redis["AsyncExists"] = [](const std::string& key, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncExists(key, [bridge](boost::system::error_code ec, bool exists) {
            bridge->PostCbBool(std::move(ec), exists);
        });
    };

    /// redis.AsyncIncr(key, callback(err, new_val))
    redis["AsyncIncr"] = [](const std::string& key, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncIncr(key, [bridge](boost::system::error_code ec, int64_t val) {
            bridge->PostCb(std::move(ec), val);
        });
    };

    /// redis.AsyncIncrBy(key, delta, callback(err, new_val))
    redis["AsyncIncrBy"] = [](const std::string& key, int64_t delta, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncIncrBy(key, delta, [bridge](boost::system::error_code ec, int64_t val) {
            bridge->PostCb(std::move(ec), val);
        });
    };

    // ── Hash 异步 ──

    /// redis.AsyncHSet(key, field, value, callback(err, n))
    redis["AsyncHSet"] = [](const std::string& key, const std::string& field,
                            const std::string& value, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncHSet(key, field, value, [bridge](boost::system::error_code ec, int64_t n) {
            bridge->PostCb(std::move(ec), n);
        });
    };

    /// redis.AsyncHGet(key, field, callback(err, val))
    redis["AsyncHGet"] = [](const std::string& key, const std::string& field,
                            sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncHGet(key, field, [bridge](boost::system::error_code ec, std::string val) {
            bridge->PostCbStr(std::move(ec), std::move(val));
        });
    };

    /// redis.AsyncHDel(key, field, callback(err, n))
    redis["AsyncHDel"] = [](const std::string& key, const std::string& field,
                            sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncHDel(key, field, [bridge](boost::system::error_code ec, int64_t n) {
            bridge->PostCb(std::move(ec), n);
        });
    };

    /// redis.AsyncHKeys(key, callback(err, {key, ...}))
    redis["AsyncHKeys"] = [](const std::string& key, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncHKeys(key, [bridge](boost::system::error_code ec, std::vector<std::string> val) {
            bridge->PostCbStrVec(std::move(ec), std::move(val));
        });
    };

    /// redis.AsyncHVals(key, callback(err, {val, ...}))
    redis["AsyncHVals"] = [](const std::string& key, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncHVals(key, [bridge](boost::system::error_code ec, std::vector<std::string> val) {
            bridge->PostCbStrVec(std::move(ec), std::move(val));
        });
    };

    /// redis.AsyncHLen(key, callback(err, n))
    redis["AsyncHLen"] = [](const std::string& key, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncHLen(key, [bridge](boost::system::error_code ec, int64_t n) {
            bridge->PostCb(std::move(ec), n);
        });
    };

    // ── List 异步 ──

    /// redis.AsyncLPush(key, value, callback(err, len))
    redis["AsyncLPush"] = [](const std::string& key, const std::string& value,
                             sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncLPush(key, value, [bridge](boost::system::error_code ec, int64_t n) {
            bridge->PostCb(std::move(ec), n);
        });
    };

    /// redis.AsyncRPush(key, value, callback(err, len))
    redis["AsyncRPush"] = [](const std::string& key, const std::string& value,
                             sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncRPush(key, value, [bridge](boost::system::error_code ec, int64_t n) {
            bridge->PostCb(std::move(ec), n);
        });
    };

    /// redis.AsyncLPop(key, callback(err, val))
    redis["AsyncLPop"] = [](const std::string& key, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncLPop(key, [bridge](boost::system::error_code ec, std::string val) {
            bridge->PostCbStr(std::move(ec), std::move(val));
        });
    };

    /// redis.AsyncRPop(key, callback(err, val))
    redis["AsyncRPop"] = [](const std::string& key, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncRPop(key, [bridge](boost::system::error_code ec, std::string val) {
            bridge->PostCbStr(std::move(ec), std::move(val));
        });
    };

    /// redis.AsyncLLen(key, callback(err, len))
    redis["AsyncLLen"] = [](const std::string& key, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncLLen(key, [bridge](boost::system::error_code ec, int64_t n) {
            bridge->PostCb(std::move(ec), n);
        });
    };

    // ── Sorted Set 异步 ──

    /// redis.AsyncZAdd(key, score, member, callback(err, n))
    redis["AsyncZAdd"] = [](const std::string& key, double score,
                            const std::string& member, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncZAdd(key, score, member, [bridge](boost::system::error_code ec, int64_t n) {
            bridge->PostCb(std::move(ec), n);
        });
    };

    /// redis.AsyncZRange(key, start, stop, with_scores, callback(err, {val, ...}))
    redis["AsyncZRange"] = [](const std::string& key, int64_t start, int64_t stop,
                              bool with_scores, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncZRange(key, start, stop, with_scores,
                       [bridge](boost::system::error_code ec, std::vector<std::string> val) {
                           bridge->PostCbStrVec(std::move(ec), std::move(val));
                       });
    };

    /// redis.AsyncZRevRange(key, start, stop, with_scores, callback(err, {val, ...}))
    redis["AsyncZRevRange"] = [](const std::string& key, int64_t start, int64_t stop,
                                 bool with_scores, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncZRevRange(key, start, stop, with_scores,
                          [bridge](boost::system::error_code ec, std::vector<std::string> val) {
                              bridge->PostCbStrVec(std::move(ec), std::move(val));
                          });
    };

    /// redis.AsyncZCard(key, callback(err, count))
    redis["AsyncZCard"] = [](const std::string& key, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncZCard(key, [bridge](boost::system::error_code ec, int64_t n) {
            bridge->PostCb(std::move(ec), n);
        });
    };

    /// redis.AsyncZRem(key, member, callback(err, n))
    redis["AsyncZRem"] = [](const std::string& key, const std::string& member,
                            sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncZRem(key, member, [bridge](boost::system::error_code ec, int64_t n) {
            bridge->PostCb(std::move(ec), n);
        });
    };

    /// redis.AsyncZScore(key, member, callback(err, score))
    redis["AsyncZScore"] = [](const std::string& key, const std::string& member,
                              sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncZScore(key, member, [bridge](boost::system::error_code ec, double score) {
            bridge->PostCbDouble(std::move(ec), score);
        });
    };

    /// redis.AsyncZRank(key, member, callback(err, rank))
    redis["AsyncZRank"] = [](const std::string& key, const std::string& member,
                             sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncZRank(key, member, [bridge](boost::system::error_code ec, int64_t rank) {
            bridge->PostCb(std::move(ec), rank);
        });
    };

    // ── Key 管理异步 ──

    /// redis.AsyncExpire(key, seconds, callback(err))
    redis["AsyncExpire"] = [](const std::string& key, int64_t seconds, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncExpire(key, seconds, [bridge](boost::system::error_code ec, bool) {
            bridge->PostCb(std::move(ec));
        });
    };

    /// redis.AsyncTTL(key, callback(err, ttl))
    redis["AsyncTTL"] = [](const std::string& key, sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncTTL(key, [bridge](boost::system::error_code ec, int64_t ttl) {
            bridge->PostCb(std::move(ec), ttl);
        });
    };

    /// redis.AsyncPing(callback(err, ok))
    redis["AsyncPing"] = [](sol::function cb) {
        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        LOG_INFO("[AsyncPing] entered: c={} w={}", static_cast<void*>(c), static_cast<void*>(w.get()));
        if (!c || !w) return;
        auto bridge = LuaCbBridge::Create(w, std::move(cb));
        c->AsyncPing([bridge](boost::system::error_code ec, bool ok) {
            LOG_INFO("[AsyncPing] callback fired, ec={} ok={}", ec ? ec.message() : "none", ok);
            bridge->PostCbBool(std::move(ec), ok);
        });
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 泛型异步命令 — redis.AsyncCall / redis.AsyncEval
    //
    // 异步执行任意 Redis 原生命令，自动将 RESP3 响应转换为 Lua 类型。
    // ═══════════════════════════════════════════════════════════════════════

    /// redis.AsyncCall(command, ..., callback(err, result))
    ///
    /// 异步执行任意 Redis 命令。参数自动转为 string。
    /// callback 在发起调用的 Worker 线程上执行。
    ///
    /// @usage
    ///   redis.AsyncCall("SET", "k", "v", function(err, val) end)
    ///   redis.AsyncCall("GET", "k", function(err, val) end)
    redis["AsyncCall"] = [](sol::this_state s, const std::string& cmd,
                            sol::variadic_args args) {
        // 最后一个参数是 callback
        if (args.size() == 0) return;
        sol::function cb = args[args.size() - 1].as<sol::function>();

        std::vector<std::string> str_args;
        str_args.reserve(args.size() - 1);
        for (size_t i = 0; i < args.size() - 1; i++)
            str_args.push_back(args[i].as<std::string>());

        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;

        auto cb_ptr = std::make_shared<sol::function>(std::move(cb));
        c->AsyncCall(cmd, str_args, [w, cb_ptr](boost::system::error_code ec,
                                                 RedisConnection::GenericResponse resp) {
            w->Post([cb_ptr, ec, resp = std::move(resp)]() mutable {
                if (!cb_ptr->valid()) return;
                lua_State* L = cb_ptr->lua_state();
                if (ec)
                    (*cb_ptr)(ec.message(), sol::lua_nil);
                else
                    (*cb_ptr)("", GenericRespToLua(L, resp));
            });
        });
    };

    /// redis.AsyncEval(script, keys, args, callback(err, result))
    ///
    /// 异步在 Redis 服务端执行 Lua 脚本。
    ///
    /// @param script Lua 脚本源码
    /// @param keys   KEYS 表 (table of strings)
    /// @param args   ARGV 表 (table of strings)
    redis["AsyncEval"] = [](sol::this_state s, const std::string& script,
                            sol::table keys, sol::table args, sol::function cb) {
        auto to_str_vec = [](sol::table t) -> std::vector<std::string> {
            std::vector<std::string> v;
            for (auto& [k, val] : t)
            {
                if (val.is<std::string>())
                    v.push_back(val.as<std::string>());
                else if (val.is<const char*>())
                    v.push_back(val.as<const char*>());
            }
            return v;
        };

        auto* c = GetConn();
        auto   w = gb::WorkerManager::Instance()->GetCurWorker();
        if (!c || !w) return;

        auto cb_ptr = std::make_shared<sol::function>(std::move(cb));
        c->AsyncEval(script, to_str_vec(keys), to_str_vec(args),
                     [w, cb_ptr](boost::system::error_code ec,
                                  RedisConnection::GenericResponse resp) {
            w->Post([cb_ptr, ec, resp = std::move(resp)]() mutable {
                if (!cb_ptr->valid()) return;
                lua_State* L = cb_ptr->lua_state();
                if (ec)
                    (*cb_ptr)(ec.message(), sol::lua_nil);
                else
                    (*cb_ptr)("", GenericRespToLua(L, resp));
            });
        });
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Lua 协程桥接 — redis.Await(method, ...)
    //
    // 允许在 Lua 协程中以同步风格写异步代码：
    //
    //   local co = coroutine.create(function()
    //       local err, val = redis.Await("Get", "mykey")
    //       log.Info("Got: " .. val)
    //   end)
    //   coroutine.resume(co)
    //
    // 原理：Await 调用对应的 AsyncXxx 方法，传入一个回调；
    // 回调保存结果并 coroutine.resume 恢复协程；
    // Await 调用 coroutine.yield 挂起等待。
    // ═══════════════════════════════════════════════════════════════════════

    lua_State* L = redis.lua_state();
    luaL_dostring(L, R"(
        if not redis.Await then
            function redis.Await(method, ...)
                local co = coroutine.running()
                if not co then
                    error("redis.Await() must be called from a coroutine")
                end

                local args = { ... }
                local results = nil
                local yielded = false

                local function cb(...)
                    results = { ... }
                    log.Info("[Await] cb called, yielded=" .. tostring(yielded) ..
                             " nresults=" .. #results)
                    if yielded then
                        log.Info("[Await] resuming co=" .. tostring(co))
                        local ok, err = coroutine.resume(co)
                        log.Info("[Await] resume returned: ok=" .. tostring(ok) ..
                                 " err=" .. tostring(err))
                        if not ok then
                            error("[Await] resume FAILED: " .. tostring(err))
                        end
                    end
                end

                args[#args + 1] = cb
                local async_fn = redis["Async" .. method]
                if not async_fn then
                    error("Unknown async method: redis.Async" .. method)
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

    LOG_INFO("Redis Lua API registered (async + await)");
}
