module;

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <optional>
#include <thread>

// Forward declarations needed before module purview
namespace boost::redis { class connection; }

#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>
#include <boost/redis/resp3/node.hpp>
#include <boost/redis/adapter/result.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>
#include <async_simple/coro/Lazy.h>
#include <async_simple/Future.h>
#include <async_simple/Promise.h>
#include "script/script.h"

export module db.redis;

// ══════════════════════════════════════════════════════════════════════════
// RedisConfig
// ══════════════════════════════════════════════════════════════════════════

export struct RedisConfig {
    std::string host         = "127.0.0.1";
    uint16_t    port         = 6379;
    std::string password;
    int         db_index     = 0;
    int         pool_size    = 4;
    int         timeout_ms   = 5000;
};

// ══════════════════════════════════════════════════════════════════════════
// RedisConnection
// ══════════════════════════════════════════════════════════════════════════

export class RedisConnection
{
public:
    RedisConnection();
    ~RedisConnection();

    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    bool Connect(const RedisConfig& cfg);
    void Disconnect();
    bool IsConnected() const { return connected_; }
    const RedisConfig& GetConfig() const { return config_; }
    boost::asio::io_context& GetIoContext() { return io_context_; }

    // ── 回调类型 ──
    using AsyncCb       = std::function<void(boost::system::error_code)>;
    using AsyncCbStr    = std::function<void(boost::system::error_code, std::string)>;
    using AsyncCbInt    = std::function<void(boost::system::error_code, int64_t)>;
    using AsyncCbDouble = std::function<void(boost::system::error_code, double)>;
    using AsyncCbBool   = std::function<void(boost::system::error_code, bool)>;
    using AsyncCbStrVec = std::function<void(boost::system::error_code, std::vector<std::string>)>;
    using AsyncCbPairs  = std::function<void(boost::system::error_code, std::vector<std::pair<std::string, double>>)>;
    using GenericResponse = boost::redis::adapter::result<std::vector<boost::redis::resp3::node>>;
    using AsyncCbGeneric = std::function<void(boost::system::error_code, GenericResponse)>;

    // ── KV ──
    void AsyncSet(std::string key, std::string value, AsyncCb cb);
    void AsyncSetEx(std::string key, std::string value, int64_t ttl_seconds, AsyncCb cb);
    void AsyncGet(std::string key, AsyncCbStr cb);
    void AsyncDel(std::string key, AsyncCbInt cb);
    void AsyncExists(std::string key, AsyncCbBool cb);
    void AsyncIncr(std::string key, AsyncCbInt cb);
    void AsyncIncrBy(std::string key, int64_t delta, AsyncCbInt cb);

    // ── Hash ──
    void AsyncHSet(std::string key, std::string field, std::string value, AsyncCbInt cb);
    void AsyncHGet(std::string key, std::string field, AsyncCbStr cb);
    void AsyncHDel(std::string key, std::string field, AsyncCbInt cb);
    void AsyncHKeys(std::string key, AsyncCbStrVec cb);
    void AsyncHVals(std::string key, AsyncCbStrVec cb);
    void AsyncHLen(std::string key, AsyncCbInt cb);

    // ── List ──
    void AsyncLPush(std::string key, std::string value, AsyncCbInt cb);
    void AsyncRPush(std::string key, std::string value, AsyncCbInt cb);
    void AsyncLPop(std::string key, AsyncCbStr cb);
    void AsyncRPop(std::string key, AsyncCbStr cb);
    void AsyncLLen(std::string key, AsyncCbInt cb);

    // ── Sorted Set ──
    void AsyncZAdd(std::string key, double score, std::string member, AsyncCbInt cb);
    void AsyncZRange(std::string key, int64_t start, int64_t stop, bool with_scores, AsyncCbStrVec cb);
    void AsyncZRevRange(std::string key, int64_t start, int64_t stop, bool with_scores, AsyncCbStrVec cb);
    void AsyncZCard(std::string key, AsyncCbInt cb);
    void AsyncZRem(std::string key, std::string member, AsyncCbInt cb);
    void AsyncZScore(std::string key, std::string member, AsyncCbDouble cb);
    void AsyncZRank(std::string key, std::string member, AsyncCbInt cb);
    void AsyncZRevRank(std::string key, std::string member, AsyncCbInt cb);
    void AsyncZCount(std::string key, double min, double max, AsyncCbInt cb);
    void AsyncZIncrBy(std::string key, std::string member, double delta, AsyncCbDouble cb);
    void AsyncZRangeByScore(std::string key, double min, double max, bool with_scores, AsyncCbStrVec cb);
    void AsyncZRevRangeByScore(std::string key, double min, double max, bool with_scores, AsyncCbStrVec cb);
    void AsyncZRemRangeByRank(std::string key, int64_t start, int64_t stop, AsyncCbInt cb);
    void AsyncZRemRangeByScore(std::string key, double min, double max, AsyncCbInt cb);
    void AsyncZRangeWithScores(std::string key, int64_t start, int64_t stop, AsyncCbPairs cb);
    void AsyncZRevRangeWithScores(std::string key, int64_t start, int64_t stop, AsyncCbPairs cb);

    // ── Key 管理 ──
    void AsyncExpire(std::string key, int64_t seconds, AsyncCbBool cb);
    void AsyncTTL(std::string key, AsyncCbInt cb);
    void AsyncPing(AsyncCbBool cb);

    // ── 泛型命令 ──
    void AsyncCall(const std::string& cmd, const std::vector<std::string>& args, AsyncCbGeneric cb);
    void AsyncEval(const std::string& script,
                   const std::vector<std::string>& keys,
                   const std::vector<std::string>& args,
                   AsyncCbGeneric cb);

    // ── 协程接口（KV） ──
    async_simple::coro::Lazy<bool>       CoSet(std::string key, std::string value);
    async_simple::coro::Lazy<bool>       CoSetEx(std::string key, std::string value, int64_t ttl_seconds);
    async_simple::coro::Lazy<std::string> CoGet(std::string key);
    async_simple::coro::Lazy<int64_t>    CoDel(std::string key);
    async_simple::coro::Lazy<bool>       CoExists(std::string key);
    async_simple::coro::Lazy<int64_t>    CoIncr(std::string key);
    async_simple::coro::Lazy<int64_t>    CoIncrBy(std::string key, int64_t delta);

    // ── 协程接口（Hash） ──
    async_simple::coro::Lazy<int64_t>              CoHSet(std::string key, std::string field, std::string value);
    async_simple::coro::Lazy<std::string>           CoHGet(std::string key, std::string field);
    async_simple::coro::Lazy<int64_t>              CoHDel(std::string key, std::string field);
    async_simple::coro::Lazy<std::vector<std::string>> CoHKeys(std::string key);
    async_simple::coro::Lazy<std::vector<std::string>> CoHVals(std::string key);
    async_simple::coro::Lazy<int64_t>              CoHLen(std::string key);

    // ── 协程接口（List） ──
    async_simple::coro::Lazy<int64_t>              CoLPush(std::string key, std::string value);
    async_simple::coro::Lazy<int64_t>              CoRPush(std::string key, std::string value);
    async_simple::coro::Lazy<std::string>           CoLPop(std::string key);
    async_simple::coro::Lazy<std::string>           CoRPop(std::string key);
    async_simple::coro::Lazy<int64_t>              CoLLen(std::string key);

    // ── 协程接口（Sorted Set） ──
    async_simple::coro::Lazy<int64_t>              CoZAdd(std::string key, double score, std::string member);
    async_simple::coro::Lazy<std::vector<std::string>> CoZRange(std::string key, int64_t start, int64_t stop, bool with_scores);
    async_simple::coro::Lazy<std::vector<std::string>> CoZRevRange(std::string key, int64_t start, int64_t stop, bool with_scores);
    async_simple::coro::Lazy<int64_t>              CoZCard(std::string key);
    async_simple::coro::Lazy<int64_t>              CoZRem(std::string key, std::string member);
    async_simple::coro::Lazy<double>               CoZScore(std::string key, std::string member);
    async_simple::coro::Lazy<int64_t>              CoZRank(std::string key, std::string member);
    async_simple::coro::Lazy<int64_t>              CoZRevRank(std::string key, std::string member);
    async_simple::coro::Lazy<int64_t>              CoZCount(std::string key, double min, double max);
    async_simple::coro::Lazy<double>               CoZIncrBy(std::string key, std::string member, double delta);
    async_simple::coro::Lazy<std::vector<std::string>> CoZRangeByScore(std::string key, double min, double max, bool with_scores);
    async_simple::coro::Lazy<std::vector<std::string>> CoZRevRangeByScore(std::string key, double min, double max, bool with_scores);
    async_simple::coro::Lazy<int64_t>              CoZRemRangeByRank(std::string key, int64_t start, int64_t stop);
    async_simple::coro::Lazy<int64_t>              CoZRemRangeByScore(std::string key, double min, double max);
    async_simple::coro::Lazy<std::vector<std::pair<std::string, double>>> CoZRangeWithScores(std::string key, int64_t start, int64_t stop);
    async_simple::coro::Lazy<std::vector<std::pair<std::string, double>>> CoZRevRangeWithScores(std::string key, int64_t start, int64_t stop);

    // ── 协程接口（Key 管理） ──
    async_simple::coro::Lazy<bool>    CoExpire(std::string key, int64_t seconds);
    async_simple::coro::Lazy<int64_t> CoTTL(std::string key);
    async_simple::coro::Lazy<bool>    CoPing();

    // ── 底层异步执行 ──
    template <typename F, typename... Ts>
    void AsyncExec(std::shared_ptr<boost::redis::request> req,
                   std::shared_ptr<boost::redis::response<Ts...>> resp,
                   F&& callback)
    {
        boost::asio::post(io_context_,
            [this, req = std::move(req), resp, cb = std::forward<F>(callback)]() mutable {
                if (!conn_)
                {
                    boost::system::error_code ec = boost::asio::error::not_connected;
                    cb(std::move(ec), 0);
                    return;
                }
                conn_->async_exec(*req, *resp,
                    [cb = std::move(cb), req, resp](boost::system::error_code ec, std::size_t) mutable {
                        cb(std::move(ec), 0);
                    });
            });
    }

private:
    // ── 协程辅助 ──
    async_simple::coro::Lazy<bool>       CbToLazyBool(std::function<void(AsyncCb)> invoker);
    async_simple::coro::Lazy<std::string> CbToLazyStr(std::function<void(AsyncCbStr)> invoker);
    async_simple::coro::Lazy<int64_t>    CbToLazyInt(std::function<void(AsyncCbInt)> invoker);
    async_simple::coro::Lazy<double>     CbToLazyDouble(std::function<void(AsyncCbDouble)> invoker);
    async_simple::coro::Lazy<bool>       CbToLazyBoolCb(std::function<void(AsyncCbBool)> invoker);
    async_simple::coro::Lazy<std::vector<std::string>> CbToLazyStrVec(std::function<void(AsyncCbStrVec)> invoker);
    async_simple::coro::Lazy<std::vector<std::pair<std::string, double>>> CbToLazyPairs(std::function<void(AsyncCbPairs)> invoker);

private:
    boost::asio::io_context                                           io_context_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    std::thread                                                       io_thread_;
    std::unique_ptr<boost::redis::connection>        conn_;
    std::atomic<bool>                                connected_{false};
    RedisConfig                                      config_;
};

// ══════════════════════════════════════════════════════════════════════════
// RedisConnectionPool
// ══════════════════════════════════════════════════════════════════════════

export class RedisConnectionPool
{
public:
    RedisConnectionPool()  = default;
    ~RedisConnectionPool();

    RedisConnectionPool(const RedisConnectionPool&) = delete;
    RedisConnectionPool& operator=(const RedisConnectionPool&) = delete;

    bool Init(const RedisConfig& cfg);
    void CloseAll();
    RedisConnection* GetConnection();
    bool IsHealthy() const;
    int  Size() const { return static_cast<int>(connections_.size()); }
    int  CountHealthy() const;

private:
    std::vector<std::unique_ptr<RedisConnection>> connections_;
    std::atomic<size_t>                           next_index_{0};
    RedisConfig                                   config_;
};

// ══════════════════════════════════════════════════════════════════════════
// 注册函数
// ══════════════════════════════════════════════════════════════════════════

export void register_redis(std::shared_ptr<Script>& scriptPtr);
export void CloseRedisPool();
