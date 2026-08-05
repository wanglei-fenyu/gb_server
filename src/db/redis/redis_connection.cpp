#include "redis_connection.h"
#include "redis_value.h"
#include "log/log.h"
#include "async_simple/coro/FutureAwaiter.h"
#include <boost/asio/post.hpp>
#include <chrono>
#include <cstdlib>
#include <memory>

// ════════════════════════════════════════════════════════════════════════
// 匿名辅助：安全数值转换（避免异常逃逸到 hiredis 回调）
// ════════════════════════════════════════════════════════════════════════

namespace
{

double SafeStod(const std::string& s)
{
    if (s.empty())
        return 0.0;
    char* end = nullptr;
    double v  = std::strtod(s.c_str(), &end);
    if (end == s.c_str())
    {
        LOG_ERROR("Redis parse double failed: \"{}\"", s);
        return 0.0;
    }
    return v;
}

int64_t SafeStoll(const std::string& s)
{
    if (s.empty())
        return 0;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str())
    {
        LOG_ERROR("Redis parse int failed: \"{}\"", s);
        return 0;
    }
    return static_cast<int64_t>(v);
}

/// 将 RedisValue 转为 string 向量。兼容 RESP2（扁平数组）和 RESP3（嵌套数组）。
std::vector<std::string> ToStrArray(const RedisValue& v)
{
    std::vector<std::string> out;
    if (v.is_nil() || !v.is_array() || !v.array_val)
        return out;
    for (const auto& item : *v.array_val)
    {
        if (item.is_array())
        {
            if (item.array_val)
            {
                for (const auto& sub : *item.array_val)
                    out.push_back(sub.is_nil() ? std::string{} : sub.as_str());
            }
        }
        else
        {
            out.push_back(item.is_nil() ? std::string{} : item.as_str());
        }
    }
    return out;
}

/// 将 RedisValue 转为 (member, score) pairs。兼容 RESP2（扁平）和 RESP3（嵌套）。
std::vector<std::pair<std::string, double>> ToPairs(const RedisValue& v)
{
    std::vector<std::pair<std::string, double>> out;
    if (v.is_nil() || !v.is_array() || !v.array_val)
        return out;
    const auto& arr = *v.array_val;
    if (!arr.empty() && arr[0].is_array())
    {
        // RESP3: [[member, score], ...]
        for (const auto& item : arr)
        {
            if (item.is_array() && item.array_val && item.array_val->size() >= 2)
                out.emplace_back((*item.array_val)[0].as_str(), (*item.array_val)[1].as_double());
        }
    }
    else
    {
        // RESP2: [member, score, member, score, ...]
        out.reserve(arr.size() / 2);
        for (size_t i = 0; i + 1 < arr.size(); i += 2)
            out.emplace_back(arr[i].as_str(), SafeStod(arr[i + 1].as_str()));
    }
    return out;
}

/// 将 RedisValue 转为 int64_t。处理 nil（返回 0）、Int、Bool、Double、Str。
int64_t ToInt(const RedisValue& v)
{
    if (v.is_nil())
        return 0;
    if (v.is_int())
        return v.int_val;
    if (v.is_bool())
        return v.bool_val ? 1 : 0;
    if (v.is_double())
        return static_cast<int64_t>(v.dbl_val);
    if (v.is_str())
        return SafeStoll(v.str_val);
    return 0;
}

/// 将 RedisValue 转为 double。处理 nil（返回 0.0）、Int、Bool、Double、Str。
double ToDouble(const RedisValue& v)
{
    if (v.is_nil())
        return 0.0;
    if (v.is_double())
        return v.dbl_val;
    if (v.is_int())
        return static_cast<double>(v.int_val);
    if (v.is_bool())
        return v.bool_val ? 1.0 : 0.0;
    if (v.is_str())
        return SafeStod(v.str_val);
    return 0.0;
}

/// 将 RedisValue 转为 string。处理 nil（返回空串）、Str、Int、Double、Bool。
std::string ToString(const RedisValue& v)
{
    if (v.is_nil())
        return {};
    if (v.is_str())
        return v.str_val;
    if (v.is_int())
        return std::to_string(v.int_val);
    if (v.is_double())
        return std::to_string(v.dbl_val);
    if (v.is_bool())
        return v.bool_val ? "1" : "0";
    return {};
}

} // anonymous namespace

// ════════════════════════════════════════════════════════════════════════
// 生命周期
// ════════════════════════════════════════════════════════════════════════

RedisConnection::RedisConnection(boost::asio::io_context& io_ctx)
    : io_ctx_(io_ctx)
    , read_descriptor_(io_ctx_)
    , write_descriptor_(io_ctx_)
{
}

RedisConnection::~RedisConnection()
{
    // 强制释放 hiredis 上下文（若仍存活）。
    // 注意：我们不拥有 io_context，不应在析构中 join 线程或重置 work_guard。
    if (ctx_alive_ || async_ctx_ != nullptr)
    {
        boost::asio::post(io_ctx_, [this]() {
            ctx_alive_ = false;
            auto* ac  = async_ctx_;
            async_ctx_ = nullptr;
            if (ac)
                redisAsyncFree(ac);
        });
    }
}

struct RedisConnection::ConnectChain
{
    std::promise<bool> promise;
    std::atomic<bool> done{false};
    int step{0};
};

bool RedisConnection::Connect(const RedisConfig& cfg)
{
    config_ = cfg;
    connected_ = false;

    if (ctx_alive_)
    {
        LOG_WARN("RedisConnection::Connect called while a context is alive; disconnecting first");
        Disconnect();
    }

    auto chain = std::make_shared<ConnectChain>();
    connect_state_ = chain;
    auto fut = chain->promise.get_future();

    boost::asio::post(io_ctx_, [this, chain]() { StartConnectChain(chain); });

    int timeout_ms = cfg.timeout_ms + 2000;
    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready)
    {
        bool ok = fut.get();
        connected_ = ok;
        return ok;
    }

    // 超时 — FailConnect 会 post 到 IO 线程清理上下文
    FailConnect();
    return false;
}

void RedisConnection::Disconnect()
{
    bool was_connected = connected_.exchange(false);
    boost::asio::post(io_ctx_, [this]() { DisconnectInternal(); });
    if (was_connected)
        LOG_INFO("Redis disconnected: {}:{}", config_.host, config_.port);
}

// ════════════════════════════════════════════════════════════════════════
// KV — 异步回调
// ════════════════════════════════════════════════════════════════════════

void RedisConnection::AsyncSet(std::string key, std::string value, AsyncCb cb)
{
    PostCommand({"SET", std::move(key), std::move(value)},
                [cb = std::move(cb)](RedisValue v) {
                    cb(v.is_error() ? v.err() : RedisError{});
                });
}

void RedisConnection::AsyncSetEx(std::string key, std::string value,
                                      int64_t ttl_seconds, AsyncCb cb)
{
    PostCommand({"SETEX", std::move(key), std::to_string(ttl_seconds), std::move(value)},
                [cb = std::move(cb)](RedisValue v) {
                    cb(v.is_error() ? v.err() : RedisError{});
                });
}

void RedisConnection::AsyncGet(std::string key, AsyncCbStr cb)
{
    PostCommand({"GET", std::move(key)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), {}); return; }
                    cb(RedisError{}, ToString(v));
                });
}

void RedisConnection::AsyncDel(std::string key, AsyncCbInt cb)
{
    PostCommand({"DEL", std::move(key)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncExists(std::string key, AsyncCbBool cb)
{
    PostCommand({"EXISTS", std::move(key)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), false); return; }
                    cb(RedisError{}, ToInt(v) > 0);
                });
}

void RedisConnection::AsyncIncr(std::string key, AsyncCbInt cb)
{
    PostCommand({"INCR", std::move(key)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncIncrBy(std::string key, int64_t delta, AsyncCbInt cb)
{
    PostCommand({"INCRBY", std::move(key), std::to_string(delta)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

// ════════════════════════════════════════════════════════════════════════
// Hash — 异步回调
// ════════════════════════════════════════════════════════════════════════

void RedisConnection::AsyncHSet(std::string key, std::string field,
                                    std::string value, AsyncCbInt cb)
{
    PostCommand({"HSET", std::move(key), std::move(field), std::move(value)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncHGet(std::string key, std::string field, AsyncCbStr cb)
{
    PostCommand({"HGET", std::move(key), std::move(field)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), {}); return; }
                    cb(RedisError{}, ToString(v));
                });
}

void RedisConnection::AsyncHDel(std::string key, std::string field, AsyncCbInt cb)
{
    PostCommand({"HDEL", std::move(key), std::move(field)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncHKeys(std::string key, AsyncCbStrVec cb)
{
    PostCommand({"HKEYS", std::move(key)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), {}); return; }
                    cb(RedisError{}, ToStrArray(v));
                });
}

void RedisConnection::AsyncHVals(std::string key, AsyncCbStrVec cb)
{
    PostCommand({"HVALS", std::move(key)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), {}); return; }
                    cb(RedisError{}, ToStrArray(v));
                });
}

void RedisConnection::AsyncHLen(std::string key, AsyncCbInt cb)
{
    PostCommand({"HLEN", std::move(key)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

// ════════════════════════════════════════════════════════════════════════
// List — 异步回调
// ════════════════════════════════════════════════════════════════════════

void RedisConnection::AsyncLPush(std::string key, std::string value, AsyncCbInt cb)
{
    PostCommand({"LPUSH", std::move(key), std::move(value)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncRPush(std::string key, std::string value, AsyncCbInt cb)
{
    PostCommand({"RPUSH", std::move(key), std::move(value)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncLPop(std::string key, AsyncCbStr cb)
{
    PostCommand({"LPOP", std::move(key)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), {}); return; }
                    cb(RedisError{}, ToString(v));
                });
}

void RedisConnection::AsyncRPop(std::string key, AsyncCbStr cb)
{
    PostCommand({"RPOP", std::move(key)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), {}); return; }
                    cb(RedisError{}, ToString(v));
                });
}

void RedisConnection::AsyncLLen(std::string key, AsyncCbInt cb)
{
    PostCommand({"LLEN", std::move(key)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

// ════════════════════════════════════════════════════════════════════════
// Sorted Set — 异步回调
// ════════════════════════════════════════════════════════════════════════

void RedisConnection::AsyncZAdd(std::string key, double score,
                                    std::string member, AsyncCbInt cb)
{
    PostCommand({"ZADD", std::move(key), std::to_string(score), std::move(member)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncZRange(std::string key, int64_t start, int64_t stop,
                                      bool with_scores, AsyncCbStrVec cb)
{
    if (with_scores)
        PostCommand({"ZRANGE", std::move(key), std::to_string(start), std::to_string(stop), "WITHSCORES"},
                    [cb = std::move(cb)](RedisValue v) {
                        if (v.is_error()) { cb(v.err(), {}); return; }
                        cb(RedisError{}, ToStrArray(v));
                    });
    else
        PostCommand({"ZRANGE", std::move(key), std::to_string(start), std::to_string(stop)},
                    [cb = std::move(cb)](RedisValue v) {
                        if (v.is_error()) { cb(v.err(), {}); return; }
                        cb(RedisError{}, ToStrArray(v));
                    });
}

void RedisConnection::AsyncZRevRange(std::string key, int64_t start, int64_t stop,
                                         bool with_scores, AsyncCbStrVec cb)
{
    if (with_scores)
        PostCommand({"ZREVRANGE", std::move(key), std::to_string(start), std::to_string(stop), "WITHSCORES"},
                    [cb = std::move(cb)](RedisValue v) {
                        if (v.is_error()) { cb(v.err(), {}); return; }
                        cb(RedisError{}, ToStrArray(v));
                    });
    else
        PostCommand({"ZREVRANGE", std::move(key), std::to_string(start), std::to_string(stop)},
                    [cb = std::move(cb)](RedisValue v) {
                        if (v.is_error()) { cb(v.err(), {}); return; }
                        cb(RedisError{}, ToStrArray(v));
                    });
}

void RedisConnection::AsyncZCard(std::string key, AsyncCbInt cb)
{
    PostCommand({"ZCARD", std::move(key)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncZRem(std::string key, std::string member, AsyncCbInt cb)
{
    PostCommand({"ZREM", std::move(key), std::move(member)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncZScore(std::string key, std::string member, AsyncCbDouble cb)
{
    PostCommand({"ZSCORE", std::move(key), std::move(member)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), -1.0); return; }
                    cb(RedisError{}, ToDouble(v));
                });
}

void RedisConnection::AsyncZRank(std::string key, std::string member, AsyncCbInt cb)
{
    PostCommand({"ZRANK", std::move(key), std::move(member)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), -1); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncZRevRank(std::string key, std::string member, AsyncCbInt cb)
{
    PostCommand({"ZREVRANK", std::move(key), std::move(member)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), -1); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncZCount(std::string key, double min, double max, AsyncCbInt cb)
{
    PostCommand({"ZCOUNT", std::move(key), std::to_string(min), std::to_string(max)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncZIncrBy(std::string key, std::string member, double delta, AsyncCbDouble cb)
{
    PostCommand({"ZINCRBY", std::move(key), std::to_string(delta), std::move(member)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0.0); return; }
                    cb(RedisError{}, ToDouble(v));
                });
}

void RedisConnection::AsyncZRangeByScore(std::string key, double min, double max,
                                             bool with_scores, AsyncCbStrVec cb)
{
    if (with_scores)
        PostCommand({"ZRANGEBYSCORE", std::move(key), std::to_string(min), std::to_string(max), "WITHSCORES"},
                    [cb = std::move(cb)](RedisValue v) {
                        if (v.is_error()) { cb(v.err(), {}); return; }
                        cb(RedisError{}, ToStrArray(v));
                    });
    else
        PostCommand({"ZRANGEBYSCORE", std::move(key), std::to_string(min), std::to_string(max)},
                    [cb = std::move(cb)](RedisValue v) {
                        if (v.is_error()) { cb(v.err(), {}); return; }
                        cb(RedisError{}, ToStrArray(v));
                    });
}

void RedisConnection::AsyncZRevRangeByScore(std::string key, double min, double max,
                                                bool with_scores, AsyncCbStrVec cb)
{
    if (with_scores)
        PostCommand({"ZREVRANGEBYSCORE", std::move(key), std::to_string(max), std::to_string(min), "WITHSCORES"},
                    [cb = std::move(cb)](RedisValue v) {
                        if (v.is_error()) { cb(v.err(), {}); return; }
                        cb(RedisError{}, ToStrArray(v));
                    });
    else
        PostCommand({"ZREVRANGEBYSCORE", std::move(key), std::to_string(max), std::to_string(min)},
                    [cb = std::move(cb)](RedisValue v) {
                        if (v.is_error()) { cb(v.err(), {}); return; }
                        cb(RedisError{}, ToStrArray(v));
                    });
}

void RedisConnection::AsyncZRemRangeByRank(std::string key, int64_t start, int64_t stop, AsyncCbInt cb)
{
    PostCommand({"ZREMRANGEBYRANK", std::move(key), std::to_string(start), std::to_string(stop)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncZRemRangeByScore(std::string key, double min, double max, AsyncCbInt cb)
{
    PostCommand({"ZREMRANGEBYSCORE", std::move(key), std::to_string(min), std::to_string(max)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), 0); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncZRangeWithScores(std::string key, int64_t start, int64_t stop, AsyncCbPairs cb)
{
    PostCommand({"ZRANGE", std::move(key), std::to_string(start), std::to_string(stop), "WITHSCORES"},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), {}); return; }
                    cb(RedisError{}, ToPairs(v));
                });
}

void RedisConnection::AsyncZRevRangeWithScores(std::string key, int64_t start, int64_t stop, AsyncCbPairs cb)
{
    PostCommand({"ZREVRANGE", std::move(key), std::to_string(start), std::to_string(stop), "WITHSCORES"},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), {}); return; }
                    cb(RedisError{}, ToPairs(v));
                });
}

// ════════════════════════════════════════════════════════════════════════
// Key 管理 — 异步回调
// ════════════════════════════════════════════════════════════════════════

void RedisConnection::AsyncExpire(std::string key, int64_t seconds, AsyncCbBool cb)
{
    PostCommand({"EXPIRE", std::move(key), std::to_string(seconds)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), false); return; }
                    cb(RedisError{}, ToInt(v) > 0);
                });
}

void RedisConnection::AsyncTTL(std::string key, AsyncCbInt cb)
{
    PostCommand({"TTL", std::move(key)},
                [cb = std::move(cb)](RedisValue v) {
                    if (v.is_error()) { cb(v.err(), -2); return; }
                    cb(RedisError{}, ToInt(v));
                });
}

void RedisConnection::AsyncPing(AsyncCbBool cb)
{
    PostCommand({"PING"},
                [cb = std::move(cb)](RedisValue v) {
                    cb(v.is_error() ? v.err() : RedisError{}, !v.is_error());
                });
}

// ════════════════════════════════════════════════════════════════════════
// 泛型命令 — 异步回调
// ════════════════════════════════════════════════════════════════════════

void RedisConnection::AsyncCall(const std::string& cmd,
                                    const std::vector<std::string>& args,
                                    AsyncCbGeneric cb)
{
    std::vector<std::string> argv;
    argv.reserve(1 + args.size());
    argv.push_back(cmd);
    for (const auto& a : args)
        argv.push_back(a);
    PostCommand(std::move(argv),
                [cb = std::move(cb)](RedisValue v) {
                    cb(v.is_error() ? v.err() : RedisError{}, std::move(v));
                });
}

void RedisConnection::AsyncEval(const std::string& script,
                                    const std::vector<std::string>& keys,
                                    const std::vector<std::string>& args,
                                    AsyncCbGeneric cb)
{
    std::vector<std::string> eval_args;
    eval_args.reserve(2 + keys.size() + args.size());
    eval_args.push_back(script);
    eval_args.push_back(std::to_string(keys.size()));
    for (const auto& k : keys)
        eval_args.push_back(k);
    for (const auto& a : args)
        eval_args.push_back(a);
    AsyncCall("EVAL", eval_args, std::move(cb));
}

// ════════════════════════════════════════════════════════════════════════
// 协程辅助
// ════════════════════════════════════════════════════════════════════════

async_simple::coro::Lazy<bool> RedisConnection::CbToLazyBool(
    std::function<void(AsyncCb)> invoker)
{
    async_simple::Promise<bool> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](RedisError err) mutable {
        promise.setValue(!err);
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<std::string> RedisConnection::CbToLazyStr(
    std::function<void(AsyncCbStr)> invoker)
{
    async_simple::Promise<std::string> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](RedisError err, std::string val) mutable {
        promise.setValue(std::move(val));
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<int64_t> RedisConnection::CbToLazyInt(
    std::function<void(AsyncCbInt)> invoker)
{
    async_simple::Promise<int64_t> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](RedisError err, int64_t val) mutable {
        promise.setValue(val);
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<double> RedisConnection::CbToLazyDouble(
    std::function<void(AsyncCbDouble)> invoker)
{
    async_simple::Promise<double> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](RedisError err, double val) mutable {
        promise.setValue(val);
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<bool> RedisConnection::CbToLazyBoolCb(
    std::function<void(AsyncCbBool)> invoker)
{
    async_simple::Promise<bool> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](RedisError err, bool val) mutable {
        promise.setValue(val);
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<std::vector<std::string>> RedisConnection::CbToLazyStrVec(
    std::function<void(AsyncCbStrVec)> invoker)
{
    async_simple::Promise<std::vector<std::string>> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](RedisError err, std::vector<std::string> val) mutable {
        promise.setValue(std::move(val));
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<std::vector<std::pair<std::string, double>>> RedisConnection::CbToLazyPairs(
    std::function<void(AsyncCbPairs)> invoker)
{
    async_simple::Promise<std::vector<std::pair<std::string, double>>> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](RedisError err, std::vector<std::pair<std::string, double>> val) mutable {
        promise.setValue(std::move(val));
    });
    co_return co_await std::move(future);
}

// ════════════════════════════════════════════════════════════════════════
// KV — 协程接口
// ════════════════════════════════════════════════════════════════════════

async_simple::coro::Lazy<bool> RedisConnection::CoSet(std::string key, std::string value)
{
    co_return co_await CbToLazyBool([this, key = std::move(key), value = std::move(value)](AsyncCb cb) mutable {
        AsyncSet(std::move(key), std::move(value), std::move(cb));
    });
}

async_simple::coro::Lazy<bool> RedisConnection::CoSetEx(std::string key, std::string value, int64_t ttl_seconds)
{
    co_return co_await CbToLazyBool([this, key = std::move(key), value = std::move(value), ttl_seconds](AsyncCb cb) mutable {
        AsyncSetEx(std::move(key), std::move(value), ttl_seconds, std::move(cb));
    });
}

async_simple::coro::Lazy<std::string> RedisConnection::CoGet(std::string key)
{
    co_return co_await CbToLazyStr([this, key = std::move(key)](AsyncCbStr cb) mutable {
        AsyncGet(std::move(key), std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoDel(std::string key)
{
    co_return co_await CbToLazyInt([this, key = std::move(key)](AsyncCbInt cb) mutable {
        AsyncDel(std::move(key), std::move(cb));
    });
}

async_simple::coro::Lazy<bool> RedisConnection::CoExists(std::string key)
{
    co_return co_await CbToLazyBoolCb([this, key = std::move(key)](AsyncCbBool cb) mutable {
        AsyncExists(std::move(key), std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoIncr(std::string key)
{
    co_return co_await CbToLazyInt([this, key = std::move(key)](AsyncCbInt cb) mutable {
        AsyncIncr(std::move(key), std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoIncrBy(std::string key, int64_t delta)
{
    co_return co_await CbToLazyInt([this, key = std::move(key), delta](AsyncCbInt cb) mutable {
        AsyncIncrBy(std::move(key), delta, std::move(cb));
    });
}

// ════════════════════════════════════════════════════════════════════════
// Hash — 协程接口
// ════════════════════════════════════════════════════════════════════════

async_simple::coro::Lazy<int64_t> RedisConnection::CoHSet(std::string key, std::string field, std::string value)
{
    co_return co_await CbToLazyInt([this, key = std::move(key), field = std::move(field), value = std::move(value)](AsyncCbInt cb) mutable {
        AsyncHSet(std::move(key), std::move(field), std::move(value), std::move(cb));
    });
}

async_simple::coro::Lazy<std::string> RedisConnection::CoHGet(std::string key, std::string field)
{
    co_return co_await CbToLazyStr([this, key = std::move(key), field = std::move(field)](AsyncCbStr cb) mutable {
        AsyncHGet(std::move(key), std::move(field), std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoHDel(std::string key, std::string field)
{
    co_return co_await CbToLazyInt([this, key = std::move(key), field = std::move(field)](AsyncCbInt cb) mutable {
        AsyncHDel(std::move(key), std::move(field), std::move(cb));
    });
}

async_simple::coro::Lazy<std::vector<std::string>> RedisConnection::CoHKeys(std::string key)
{
    co_return co_await CbToLazyStrVec([this, key = std::move(key)](AsyncCbStrVec cb) mutable {
        AsyncHKeys(std::move(key), std::move(cb));
    });
}

async_simple::coro::Lazy<std::vector<std::string>> RedisConnection::CoHVals(std::string key)
{
    co_return co_await CbToLazyStrVec([this, key = std::move(key)](AsyncCbStrVec cb) mutable {
        AsyncHVals(std::move(key), std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoHLen(std::string key)
{
    co_return co_await CbToLazyInt([this, key = std::move(key)](AsyncCbInt cb) mutable {
        AsyncHLen(std::move(key), std::move(cb));
    });
}

// ════════════════════════════════════════════════════════════════════════
// List — 协程接口
// ════════════════════════════════════════════════════════════════════════

async_simple::coro::Lazy<int64_t> RedisConnection::CoLPush(std::string key, std::string value)
{
    co_return co_await CbToLazyInt([this, key = std::move(key), value = std::move(value)](AsyncCbInt cb) mutable {
        AsyncLPush(std::move(key), std::move(value), std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoRPush(std::string key, std::string value)
{
    co_return co_await CbToLazyInt([this, key = std::move(key), value = std::move(value)](AsyncCbInt cb) mutable {
        AsyncRPush(std::move(key), std::move(value), std::move(cb));
    });
}

async_simple::coro::Lazy<std::string> RedisConnection::CoLPop(std::string key)
{
    co_return co_await CbToLazyStr([this, key = std::move(key)](AsyncCbStr cb) mutable {
        AsyncLPop(std::move(key), std::move(cb));
    });
}

async_simple::coro::Lazy<std::string> RedisConnection::CoRPop(std::string key)
{
    co_return co_await CbToLazyStr([this, key = std::move(key)](AsyncCbStr cb) mutable {
        AsyncRPop(std::move(key), std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoLLen(std::string key)
{
    co_return co_await CbToLazyInt([this, key = std::move(key)](AsyncCbInt cb) mutable {
        AsyncLLen(std::move(key), std::move(cb));
    });
}

// ════════════════════════════════════════════════════════════════════════
// Sorted Set — 协程接口
// ════════════════════════════════════════════════════════════════════════

async_simple::coro::Lazy<int64_t> RedisConnection::CoZAdd(std::string key, double score, std::string member)
{
    co_return co_await CbToLazyInt([this, key = std::move(key), score, member = std::move(member)](AsyncCbInt cb) mutable {
        AsyncZAdd(std::move(key), score, std::move(member), std::move(cb));
    });
}

async_simple::coro::Lazy<std::vector<std::string>> RedisConnection::CoZRange(
    std::string key, int64_t start, int64_t stop, bool with_scores)
{
    co_return co_await CbToLazyStrVec([this, key = std::move(key), start, stop, with_scores](AsyncCbStrVec cb) mutable {
        AsyncZRange(std::move(key), start, stop, with_scores, std::move(cb));
    });
}

async_simple::coro::Lazy<std::vector<std::string>> RedisConnection::CoZRevRange(
    std::string key, int64_t start, int64_t stop, bool with_scores)
{
    co_return co_await CbToLazyStrVec([this, key = std::move(key), start, stop, with_scores](AsyncCbStrVec cb) mutable {
        AsyncZRevRange(std::move(key), start, stop, with_scores, std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoZCard(std::string key)
{
    co_return co_await CbToLazyInt([this, key = std::move(key)](AsyncCbInt cb) mutable {
        AsyncZCard(std::move(key), std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoZRem(std::string key, std::string member)
{
    co_return co_await CbToLazyInt([this, key = std::move(key), member = std::move(member)](AsyncCbInt cb) mutable {
        AsyncZRem(std::move(key), std::move(member), std::move(cb));
    });
}

async_simple::coro::Lazy<double> RedisConnection::CoZScore(std::string key, std::string member)
{
    co_return co_await CbToLazyDouble([this, key = std::move(key), member = std::move(member)](AsyncCbDouble cb) mutable {
        AsyncZScore(std::move(key), std::move(member), std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoZRank(std::string key, std::string member)
{
    co_return co_await CbToLazyInt([this, key = std::move(key), member = std::move(member)](AsyncCbInt cb) mutable {
        AsyncZRank(std::move(key), std::move(member), std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoZRevRank(std::string key, std::string member)
{
    co_return co_await CbToLazyInt([this, key = std::move(key), member = std::move(member)](AsyncCbInt cb) mutable {
        AsyncZRevRank(std::move(key), std::move(member), std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoZCount(std::string key, double min, double max)
{
    co_return co_await CbToLazyInt([this, key = std::move(key), min, max](AsyncCbInt cb) mutable {
        AsyncZCount(std::move(key), min, max, std::move(cb));
    });
}

async_simple::coro::Lazy<double> RedisConnection::CoZIncrBy(std::string key, std::string member, double delta)
{
    co_return co_await CbToLazyDouble([this, key = std::move(key), member = std::move(member), delta](AsyncCbDouble cb) mutable {
        AsyncZIncrBy(std::move(key), std::move(member), delta, std::move(cb));
    });
}

async_simple::coro::Lazy<std::vector<std::string>> RedisConnection::CoZRangeByScore(
    std::string key, double min, double max, bool with_scores)
{
    co_return co_await CbToLazyStrVec([this, key = std::move(key), min, max, with_scores](AsyncCbStrVec cb) mutable {
        AsyncZRangeByScore(std::move(key), min, max, with_scores, std::move(cb));
    });
}

async_simple::coro::Lazy<std::vector<std::string>> RedisConnection::CoZRevRangeByScore(
    std::string key, double min, double max, bool with_scores)
{
    co_return co_await CbToLazyStrVec([this, key = std::move(key), min, max, with_scores](AsyncCbStrVec cb) mutable {
        AsyncZRevRangeByScore(std::move(key), min, max, with_scores, std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoZRemRangeByRank(std::string key, int64_t start, int64_t stop)
{
    co_return co_await CbToLazyInt([this, key = std::move(key), start, stop](AsyncCbInt cb) mutable {
        AsyncZRemRangeByRank(std::move(key), start, stop, std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoZRemRangeByScore(std::string key, double min, double max)
{
    co_return co_await CbToLazyInt([this, key = std::move(key), min, max](AsyncCbInt cb) mutable {
        AsyncZRemRangeByScore(std::move(key), min, max, std::move(cb));
    });
}

async_simple::coro::Lazy<std::vector<std::pair<std::string, double>>> RedisConnection::CoZRangeWithScores(
    std::string key, int64_t start, int64_t stop)
{
    co_return co_await CbToLazyPairs([this, key = std::move(key), start, stop](AsyncCbPairs cb) mutable {
        AsyncZRangeWithScores(std::move(key), start, stop, std::move(cb));
    });
}

async_simple::coro::Lazy<std::vector<std::pair<std::string, double>>> RedisConnection::CoZRevRangeWithScores(
    std::string key, int64_t start, int64_t stop)
{
    co_return co_await CbToLazyPairs([this, key = std::move(key), start, stop](AsyncCbPairs cb) mutable {
        AsyncZRevRangeWithScores(std::move(key), start, stop, std::move(cb));
    });
}

// ════════════════════════════════════════════════════════════════════════
// Key 管理 — 协程接口
// ════════════════════════════════════════════════════════════════════════

async_simple::coro::Lazy<bool> RedisConnection::CoExpire(std::string key, int64_t seconds)
{
    co_return co_await CbToLazyBoolCb([this, key = std::move(key), seconds](AsyncCbBool cb) mutable {
        AsyncExpire(std::move(key), seconds, std::move(cb));
    });
}

async_simple::coro::Lazy<int64_t> RedisConnection::CoTTL(std::string key)
{
    co_return co_await CbToLazyInt([this, key = std::move(key)](AsyncCbInt cb) mutable {
        AsyncTTL(std::move(key), std::move(cb));
    });
}

async_simple::coro::Lazy<bool> RedisConnection::CoPing()
{
    co_return co_await CbToLazyBoolCb([this](AsyncCbBool cb) mutable {
        AsyncPing(std::move(cb));
    });
}

// ════════════════════════════════════════════════════════════════════════
// 底层基础设施 — hiredis 异步 API + Boost.Asio reactor 适配
// ════════════════════════════════════════════════════════════════════════

void RedisConnection::PostCommand(std::vector<std::string> argv,
                                   std::function<void(RedisValue)> on_reply)
{
    if (!async_ctx_ || !connected_)
    {
        on_reply(RedisValue::MakeError("not connected"));
        return;
    }

    pending_cb_ = std::move(on_reply);

    std::vector<const char*> cargv;
    std::vector<size_t>      argvlen;
    cargv.reserve(argv.size());
    argvlen.reserve(argv.size());
    for (const auto& arg : argv)
    {
        cargv.push_back(arg.c_str());
        argvlen.push_back(arg.size());
    }

    int rc = redisAsyncCommandArgv(async_ctx_,
                                      CommandCallback,
                                      this,
                                      static_cast<int>(cargv.size()),
                                      cargv.data(),
                                      argvlen.data());
    if (rc != REDIS_OK)
    {
        LOG_ERROR("redisAsyncCommandArgv failed: {}", async_ctx_->errstr);
        pending_cb_ = nullptr;
    }
}

void RedisConnection::CommandCallback(redisAsyncContext* ac, void* r, void* privdata)
{
    auto* self = static_cast<RedisConnection*>(privdata);
    if (!self)
        return;

    if (!r)
    {
        self->pending_cb_(RedisValue::MakeError("connection closed"));
        self->pending_cb_ = nullptr;
        return;
    }

    auto* reply = static_cast<redisReply*>(r);
    RedisValue val = RedisValue::FromReply(reply);
    self->pending_cb_(std::move(val));
    self->pending_cb_ = nullptr;
}

void RedisConnection::StartConnectChain(const std::shared_ptr<ConnectChain>& chain)
{
    async_ctx_ = redisAsyncConnect(config_.host.c_str(), config_.port);
    if (!async_ctx_ || async_ctx_->err)
    {
        LOG_ERROR("Redis async connect failed: {}",
                  async_ctx_ ? async_ctx_->errstr : "unknown error");
        chain->promise.set_value(false);
        return;
    }

    async_ctx_->data = this;
    redisAsyncSetConnectCallback(async_ctx_, EvConnect);
    redisAsyncSetDisconnectCallback(async_ctx_, EvDisconnect);

    async_ctx_->ev.addRead  = EvAddRead;
    async_ctx_->ev.delRead  = EvDelRead;
    async_ctx_->ev.addWrite = EvAddWrite;
    async_ctx_->ev.delWrite = EvDelWrite;
    async_ctx_->ev.cleanup  = EvCleanup;
    async_ctx_->ev.data     = this;

    int fd = async_ctx_->c.fd;
    if (fd < 0)
    {
        LOG_ERROR("Redis async get fd failed");
        redisAsyncFree(async_ctx_);
        async_ctx_ = nullptr;
        chain->promise.set_value(false);
        return;
    }

    ctx_alive_ = true;
    read_descriptor_.assign(fd);
    write_descriptor_.assign(fd);
    OnAddRead();
}

void RedisConnection::FailConnect()
{
    boost::asio::post(io_ctx_, [this]() {
        if (async_ctx_)
        {
            redisAsyncFree(async_ctx_);
            async_ctx_ = nullptr;
        }
        ctx_alive_ = false;
        connect_state_.reset();
    });
}

void RedisConnection::DisconnectInternal()
{
    if (async_ctx_)
    {
        redisAsyncFree(async_ctx_);
        async_ctx_ = nullptr;
    }
    ctx_alive_ = false;
    connect_state_.reset();
    connected_ = false;
    read_descriptor_.cancel();
    write_descriptor_.cancel();
}

// ── hiredis 事件回调（静态 thunk）──

void RedisConnection::EvAddRead(void* privdata)
{
    auto* self = static_cast<RedisConnection*>(privdata);
    if (self)
        self->OnAddRead();
}

void RedisConnection::EvDelRead(void* privdata)
{
    auto* self = static_cast<RedisConnection*>(privdata);
    if (self)
        self->OnDelRead();
}

void RedisConnection::EvAddWrite(void* privdata)
{
    auto* self = static_cast<RedisConnection*>(privdata);
    if (self)
        self->OnAddWrite();
}

void RedisConnection::EvDelWrite(void* privdata)
{
    auto* self = static_cast<RedisConnection*>(privdata);
    if (self)
        self->OnDelWrite();
}

void RedisConnection::EvCleanup(void* privdata)
{
    auto* self = static_cast<RedisConnection*>(privdata);
    if (self)
        self->OnCleanupEv();
}

void RedisConnection::EvConnect(const redisAsyncContext* ac, int status)
{
    auto* self = static_cast<RedisConnection*>(ac->data);
    if (self)
        self->OnConnectStatus(ac, status);
}

void RedisConnection::EvDisconnect(const redisAsyncContext* ac, int status)
{
    auto* self = static_cast<RedisConnection*>(ac->data);
    if (self)
        self->OnDisconnectStatus(ac, status);
}

// ── Asio IO 处理器 ──

void RedisConnection::OnAddRead()
{
    if (!async_ctx_ || !ctx_alive_)
        return;
    read_descriptor_.async_wait(boost::asio::posix::stream_descriptor::wait_read,
        [this](const boost::system::error_code& ec) {
            if (!ec)
            {
                redisAsyncHandleRead(async_ctx_);
                OnAddRead();
            }
        });
}

void RedisConnection::OnDelRead()
{
    read_descriptor_.cancel();
}

void RedisConnection::OnAddWrite()
{
    if (!async_ctx_ || !ctx_alive_)
        return;
    write_descriptor_.async_wait(boost::asio::posix::stream_descriptor::wait_write,
        [this](const boost::system::error_code& ec) {
            if (!ec)
            {
                redisAsyncHandleWrite(async_ctx_);
                OnAddWrite();
            }
        });
}

void RedisConnection::OnDelWrite()
{
    write_descriptor_.cancel();
}

void RedisConnection::OnCleanupEv()
{
    read_descriptor_.cancel();
    write_descriptor_.cancel();
}

void RedisConnection::OnConnectStatus(const redisAsyncContext* ac, int status)
{
    if (status != REDIS_OK)
    {
        LOG_ERROR("Redis connect failed: {}", ac->errstr);
        if (connect_state_)
            connect_state_->promise.set_value(false);
        return;
    }

    LOG_INFO("Redis connected: {}:{}", config_.host, config_.port);

    if (connect_state_)
        OnConnectStep(connect_state_, RedisValue::MakeNil());
}

void RedisConnection::OnDisconnectStatus(const redisAsyncContext* ac, int status)
{
    LOG_WARN("Redis disconnected: {}:{}", config_.host, config_.port);
    connected_ = false;
    ctx_alive_ = false;
    if (connect_state_ && !connect_state_->done.load())
    {
        connect_state_->promise.set_value(false);
        connect_state_->done = true;
    }
}

void RedisConnection::OnConnectStep(const std::shared_ptr<ConnectChain>& chain,
                                     RedisValue v)
{
    if (chain->done.load())
        return;

    switch (chain->step)
    {
    case 0:
        PostCommand({"PING"}, [chain, this](RedisValue r) {
            if (r.is_error())
            {
                chain->promise.set_value(false);
                chain->done = true;
                return;
            }
            chain->step = 1;
            OnConnectStep(chain, std::move(r));
        });
        break;

    case 1:
        PostCommand({"HELLO", "3"}, [chain, this](RedisValue r) {
            if (r.is_error())
            {
                chain->step = 2;
                OnConnectStep(chain, std::move(r));
                return;
            }
            chain->step = 2;
            OnConnectStep(chain, std::move(r));
        });
        break;

    case 2:
        if (config_.db_index > 0)
        {
            PostCommand({"SELECT", std::to_string(config_.db_index)},
                        [chain, this](RedisValue r) {
                            if (r.is_error())
                            {
                                chain->promise.set_value(false);
                                chain->done = true;
                                return;
                            }
                            chain->promise.set_value(true);
                            chain->done = true;
                            connected_ = true;
                        });
        }
        else
        {
            chain->promise.set_value(true);
            chain->done = true;
            connected_ = true;
        }
        break;

    default:
        chain->promise.set_value(true);
        chain->done = true;
        connected_ = true;
        break;
    }
}
