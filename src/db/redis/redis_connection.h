#pragma once
#include "redis_config.h"
#include "log/log.h"
#include <boost/redis/connection.hpp>
#include <boost/redis/resp3/node.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <thread>
#include <memory>
#include <atomic>
#include <string>
#include <vector>
#include <functional>
#include <optional>

#include "async_simple/Future.h"
#include "async_simple/Promise.h"
#include "async_simple/coro/Lazy.h"

/// Redis 连接封装。
///
/// 每个 RedisConnection 拥有独立的 io_context + 后台线程。
/// 所有 API 都是异步的——回调 API（AsyncXxx）或协程 API（CoXxx）。
///
/// 回调签名约定：
///   所有回调在 Redis IO 线程上执行。若需回到 Worker 线程，请使用 Worker::Post。
///   cb(ec)         — 无返回值的操作
///   cb(ec, value)  — 有返回值的操作
///
/// 协程约定：
///   协程方法返回 async_simple::coro::Lazy<T>，可在 Worker 线程 co_await。
///   内部自动桥接 IO 线程，恢复时回到原调用线程的执行器。
///
class RedisConnection
{
public:
    RedisConnection();
    ~RedisConnection();

    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    /// 连接 Redis 服务器。返回 true 表示连接成功。
    bool Connect(const RedisConfig& cfg);

    /// 断开连接。
    void Disconnect();

    /// 是否已连接。
    bool IsConnected() const { return connected_; }

    const RedisConfig& GetConfig() const { return config_; }

    /// 获取 io_context（用于投递异步任务）。
    boost::asio::io_context& GetIoContext() { return io_context_; }

    // ════════════════════════════════════════════════════════════════════════
    // 异步回调接口
    // 所有回调在 Redis IO 线程上执行
    // 回调约定: cb(boost::system::error_code, [value])
    // ════════════════════════════════════════════════════════════════════════

    // ── 类型别名 ──
    using AsyncCb       = std::function<void(boost::system::error_code)>;
    using AsyncCbStr    = std::function<void(boost::system::error_code, std::string)>;
    using AsyncCbInt    = std::function<void(boost::system::error_code, int64_t)>;
    using AsyncCbDouble = std::function<void(boost::system::error_code, double)>;
    using AsyncCbBool   = std::function<void(boost::system::error_code, bool)>;
    using AsyncCbStrVec = std::function<void(boost::system::error_code, std::vector<std::string>)>;
    using AsyncCbPairs  = std::function<void(boost::system::error_code, std::vector<std::pair<std::string, double>>)>;

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
    using GenericResponse = boost::redis::adapter::result<std::vector<boost::redis::resp3::node>>;
    using AsyncCbGeneric = std::function<void(boost::system::error_code, GenericResponse)>;

    void AsyncCall(const std::string& cmd, const std::vector<std::string>& args, AsyncCbGeneric cb);
    void AsyncEval(const std::string& script,
                   const std::vector<std::string>& keys,
                   const std::vector<std::string>& args,
                   AsyncCbGeneric cb);

    // ════════════════════════════════════════════════════════════════════════
    // 协程接口
    // 返回 async_simple::coro::Lazy<T>，可在 Worker 线程上 co_await
    // ════════════════════════════════════════════════════════════════════════

    // ── KV ──
    async_simple::coro::Lazy<bool>       CoSet(std::string key, std::string value);
    async_simple::coro::Lazy<bool>       CoSetEx(std::string key, std::string value, int64_t ttl_seconds);
    async_simple::coro::Lazy<std::string> CoGet(std::string key);
    async_simple::coro::Lazy<int64_t>    CoDel(std::string key);
    async_simple::coro::Lazy<bool>       CoExists(std::string key);
    async_simple::coro::Lazy<int64_t>    CoIncr(std::string key);
    async_simple::coro::Lazy<int64_t>    CoIncrBy(std::string key, int64_t delta);

    // ── Hash ──
    async_simple::coro::Lazy<int64_t>              CoHSet(std::string key, std::string field, std::string value);
    async_simple::coro::Lazy<std::string>           CoHGet(std::string key, std::string field);
    async_simple::coro::Lazy<int64_t>              CoHDel(std::string key, std::string field);
    async_simple::coro::Lazy<std::vector<std::string>> CoHKeys(std::string key);
    async_simple::coro::Lazy<std::vector<std::string>> CoHVals(std::string key);
    async_simple::coro::Lazy<int64_t>              CoHLen(std::string key);

    // ── List ──
    async_simple::coro::Lazy<int64_t>              CoLPush(std::string key, std::string value);
    async_simple::coro::Lazy<int64_t>              CoRPush(std::string key, std::string value);
    async_simple::coro::Lazy<std::string>           CoLPop(std::string key);
    async_simple::coro::Lazy<std::string>           CoRPop(std::string key);
    async_simple::coro::Lazy<int64_t>              CoLLen(std::string key);

    // ── Sorted Set ──
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

    // ── Key 管理 ──
    async_simple::coro::Lazy<bool>    CoExpire(std::string key, int64_t seconds);
    async_simple::coro::Lazy<int64_t> CoTTL(std::string key);
    async_simple::coro::Lazy<bool>    CoPing();

    // ════════════════════════════════════════════════════════════════════════
    // 底层异步执行（模板，不能放在 .cpp 中）
    // ════════════════════════════════════════════════════════════════════════

    /// 异步执行 Redis 命令（非阻塞）。
    /// callback 在 Redis IO 线程上执行。
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
    // ── 协程辅助：将 AsyncCb 转为 Lazy<bool> ──
    async_simple::coro::Lazy<bool> CbToLazyBool(std::function<void(AsyncCb)> invoker);

    // ── 协程辅助：将 AsyncCbStr 转为 Lazy<std::string> ──
    async_simple::coro::Lazy<std::string> CbToLazyStr(std::function<void(AsyncCbStr)> invoker);

    // ── 协程辅助：将 AsyncCbInt 转为 Lazy<int64_t> ──
    async_simple::coro::Lazy<int64_t> CbToLazyInt(std::function<void(AsyncCbInt)> invoker);

    // ── 协程辅助：将 AsyncCbDouble 转为 Lazy<double> ──
    async_simple::coro::Lazy<double> CbToLazyDouble(std::function<void(AsyncCbDouble)> invoker);

    // ── 协程辅助：将 AsyncCbBool 转为 Lazy<bool> ──
    async_simple::coro::Lazy<bool> CbToLazyBoolCb(std::function<void(AsyncCbBool)> invoker);

    // ── 协程辅助：将 AsyncCbStrVec 转为 Lazy<vector<string>> ──
    async_simple::coro::Lazy<std::vector<std::string>> CbToLazyStrVec(std::function<void(AsyncCbStrVec)> invoker);

    // ── 协程辅助：将 AsyncCbPairs 转为 Lazy<vector<pair<string, double>>> ──
    async_simple::coro::Lazy<std::vector<std::pair<std::string, double>>> CbToLazyPairs(std::function<void(AsyncCbPairs)> invoker);

private:
    boost::asio::io_context                                           io_context_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    std::thread                                                       io_thread_;
    std::unique_ptr<boost::redis::connection>        conn_;
    std::atomic<bool>                                connected_{false};
    RedisConfig                                      config_;
};
