#include "redis_connection.h"
#include "async_simple/coro/FutureAwaiter.h"
#include <boost/redis/src.hpp>
#include <chrono>
#include <optional>
#include <memory>

// ═════════════════════════════════════════════════════════════════════════════
// 内部辅助：从 adapter::result<T> 中安全取值
// ═════════════════════════════════════════════════════════════════════════════

namespace {

template <typename T>
static bool ExtractValue(const boost::redis::adapter::result<T>& result, T& out)
{
    if (!result.has_value())
    {
        LOG_ERROR("Redis result error: type={} diagnostic={}",
                  static_cast<int>(result.error().data_type),
                  result.error().diagnostic);
        return false;
    }
    out = result.value();
    return true;
}

template <typename T>
static bool ExtractOptionalValue(
    const boost::redis::adapter::result<std::optional<T>>& result, T& out)
{
    if (!result.has_value())
    {
        LOG_ERROR("Redis result error: type={} diagnostic={}",
                  static_cast<int>(result.error().data_type),
                  result.error().diagnostic);
        return false;
    }
    auto& opt = result.value();
    if (!opt.has_value())
        return false;
    out = opt.value();
    return true;
}

} // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════════
// 生命周期
// ═════════════════════════════════════════════════════════════════════════════

RedisConnection::RedisConnection()
    : work_guard_(boost::asio::make_work_guard(io_context_))
{
    conn_ = std::make_unique<boost::redis::connection>(io_context_, boost::redis::logger{});
    io_thread_ = std::thread([this]() { io_context_.run(); });
    connected_ = false;
}

RedisConnection::~RedisConnection()
{
    connected_ = false;

    // Post cancel to the io_context — the IO thread (which is running inside
    // io_context::run()) will process it synchronously, driving the cancellation
    // chain (run_op → parallel_group → reader/writer → timer → socket I/O)
    // to completion before run() returns.
    if (conn_) {
        boost::asio::post(io_context_, [this]() {
            conn_->cancel(boost::redis::operation::all);
        });
    }

    // Drop the work guard so io_context::run() can exit naturally once all
    // handlers (including the cancellation chain) have been processed.
    work_guard_.reset();

    // Wait for the IO thread to finish — run() will process the cancel lambda
    // and the entire cancellation chain, ensuring all socket I/O is cancelled
    // before the connection is destroyed.
    if (io_thread_.joinable())
        io_thread_.join();

    // conn_ destroyed implicitly — socket has no pending I/O.
    // io_context_ destroyed last (original declaration order).
}

bool RedisConnection::Connect(const RedisConfig& cfg)
{
    config_ = cfg;

    boost::redis::config redis_cfg;
    redis_cfg.addr.host = cfg.host;
    redis_cfg.addr.port = std::to_string(cfg.port);
    if (!cfg.password.empty())
        redis_cfg.password = cfg.password;
    redis_cfg.health_check_interval = std::chrono::seconds::zero();

    std::promise<bool> conn_promise;
    auto               conn_fut = conn_promise.get_future();

    boost::asio::post(io_context_, [this, redis_cfg, &conn_promise]() {
        conn_->async_run(redis_cfg,
                         [](boost::system::error_code ec) {
                             if (ec)
                                 LOG_ERROR("Redis async_run stopped: {}", ec.message());
                         });

        // PING 验证连接 — 用 shared_ptr 确保对象活到回调完成
        auto ping_req  = std::make_shared<boost::redis::request>();
        auto ping_resp = std::make_shared<boost::redis::response<std::string>>();
        ping_req->push("PING");
        conn_->async_exec(*ping_req, *ping_resp,
                          [&conn_promise, ping_req, ping_resp](boost::system::error_code ec, auto) {
                              conn_promise.set_value(!ec);
                          });
    });

    bool ok = conn_fut.get();
    connected_ = ok;

    if (ok)
    {
        if (cfg.db_index >= 0)
        {
            // SELECT 也用异步方式做 — shared_ptr 确保对象活到回调完成
            std::promise<void> select_promise;
            auto               select_fut = select_promise.get_future();
            auto select_req  = std::make_shared<boost::redis::request>();
            auto select_resp = std::make_shared<boost::redis::response<std::string>>();
            select_req->push("SELECT", cfg.db_index);
            boost::asio::post(io_context_, [this, select_req, select_resp, &select_promise]() {
                conn_->async_exec(*select_req, *select_resp,
                                  [&select_promise, select_req, select_resp](boost::system::error_code, auto) {
                                      select_promise.set_value();
                                  });
            });
            select_fut.wait();
        }
        LOG_INFO("Redis connected: {}:{}/{}", cfg.host, cfg.port, cfg.db_index);
    }
    else
    {
        LOG_ERROR("Redis connect failed: {}:{}", cfg.host, cfg.port);
    }

    return ok;
}

void RedisConnection::Disconnect()
{
    bool was_connected = connected_.exchange(false);
    boost::asio::post(io_context_, [this]() { conn_->cancel(); });
    if (was_connected)
        LOG_INFO("Redis disconnected: {}:{}", config_.host, config_.port);
}

// ═════════════════════════════════════════════════════════════════════════════
// 协程辅助：将回调 API 桥接为协程 API
// ═════════════════════════════════════════════════════════════════════════════

async_simple::coro::Lazy<bool> RedisConnection::CbToLazyBool(
    std::function<void(AsyncCb)> invoker)
{
    async_simple::Promise<bool> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](boost::system::error_code ec) mutable {
        promise.setValue(!ec);
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<std::string> RedisConnection::CbToLazyStr(
    std::function<void(AsyncCbStr)> invoker)
{
    async_simple::Promise<std::string> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](boost::system::error_code ec, std::string val) mutable {
        promise.setValue(std::move(val));
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<int64_t> RedisConnection::CbToLazyInt(
    std::function<void(AsyncCbInt)> invoker)
{
    async_simple::Promise<int64_t> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](boost::system::error_code ec, int64_t val) mutable {
        promise.setValue(val);
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<double> RedisConnection::CbToLazyDouble(
    std::function<void(AsyncCbDouble)> invoker)
{
    async_simple::Promise<double> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](boost::system::error_code ec, double val) mutable {
        promise.setValue(val);
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<bool> RedisConnection::CbToLazyBoolCb(
    std::function<void(AsyncCbBool)> invoker)
{
    async_simple::Promise<bool> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](boost::system::error_code ec, bool val) mutable {
        promise.setValue(val);
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<std::vector<std::string>> RedisConnection::CbToLazyStrVec(
    std::function<void(AsyncCbStrVec)> invoker)
{
    async_simple::Promise<std::vector<std::string>> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](boost::system::error_code ec, std::vector<std::string> val) mutable {
        promise.setValue(std::move(val));
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<std::vector<std::pair<std::string, double>>> RedisConnection::CbToLazyPairs(
    std::function<void(AsyncCbPairs)> invoker)
{
    async_simple::Promise<std::vector<std::pair<std::string, double>>> promise;
    auto future = promise.getFuture();
    invoker([promise = std::move(promise)](boost::system::error_code ec, std::vector<std::pair<std::string, double>> val) mutable {
        promise.setValue(std::move(val));
    });
    co_return co_await std::move(future);
}

// ═════════════════════════════════════════════════════════════════════════════
// KV — 异步回调
// ═════════════════════════════════════════════════════════════════════════════

void RedisConnection::AsyncSet(std::string key, std::string value, AsyncCb cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::string>>();
    req->push("SET", std::move(key), std::move(value));
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb)](boost::system::error_code ec, auto) { cb(std::move(ec)); });
}

void RedisConnection::AsyncSetEx(std::string key, std::string value,
                                 int64_t ttl_seconds, AsyncCb cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::string>>();
    req->push("SETEX", std::move(key), std::to_string(ttl_seconds), std::move(value));
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb)](boost::system::error_code ec, auto) { cb(std::move(ec)); });
}

void RedisConnection::AsyncGet(std::string key, AsyncCbStr cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::optional<std::string>>>();
    req->push("GET", std::move(key));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, {}); return; }
                  std::string val;
                  ExtractOptionalValue(std::get<0>(*resp_ptr), val);
                  cb(ec, std::move(val));
              });
}

void RedisConnection::AsyncDel(std::string key, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("DEL", std::move(key));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncExists(std::string key, AsyncCbBool cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("EXISTS", std::move(key));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, false); return; }
                  long long val{};
                  cb(ec, ExtractValue(std::get<0>(*resp_ptr), val) && val > 0);
              });
}

void RedisConnection::AsyncIncr(std::string key, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("INCR", std::move(key));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncIncrBy(std::string key, int64_t delta, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("INCRBY", std::move(key), std::to_string(delta));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

// ═════════════════════════════════════════════════════════════════════════════
// Hash — 异步回调
// ═════════════════════════════════════════════════════════════════════════════

void RedisConnection::AsyncHSet(std::string key, std::string field,
                                std::string value, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("HSET", std::move(key), std::move(field), std::move(value));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncHGet(std::string key, std::string field, AsyncCbStr cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::optional<std::string>>>();
    req->push("HGET", std::move(key), std::move(field));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, {}); return; }
                  std::string val;
                  ExtractOptionalValue(std::get<0>(*resp_ptr), val);
                  cb(ec, std::move(val));
              });
}

void RedisConnection::AsyncHDel(std::string key, std::string field, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("HDEL", std::move(key), std::move(field));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncHKeys(std::string key, AsyncCbStrVec cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::vector<std::string>>>();
    req->push("HKEYS", std::move(key));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, {}); return; }
                  std::vector<std::string> val;
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, std::move(val));
              });
}

void RedisConnection::AsyncHVals(std::string key, AsyncCbStrVec cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::vector<std::string>>>();
    req->push("HVALS", std::move(key));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, {}); return; }
                  std::vector<std::string> val;
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, std::move(val));
              });
}

void RedisConnection::AsyncHLen(std::string key, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("HLEN", std::move(key));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

// ═════════════════════════════════════════════════════════════════════════════
// List — 异步回调
// ═════════════════════════════════════════════════════════════════════════════

void RedisConnection::AsyncLPush(std::string key, std::string value, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("LPUSH", std::move(key), std::move(value));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncRPush(std::string key, std::string value, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("RPUSH", std::move(key), std::move(value));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncLPop(std::string key, AsyncCbStr cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::optional<std::string>>>();
    req->push("LPOP", std::move(key));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, {}); return; }
                  std::string val;
                  ExtractOptionalValue(std::get<0>(*resp_ptr), val);
                  cb(ec, std::move(val));
              });
}

void RedisConnection::AsyncRPop(std::string key, AsyncCbStr cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::optional<std::string>>>();
    req->push("RPOP", std::move(key));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, {}); return; }
                  std::string val;
                  ExtractOptionalValue(std::get<0>(*resp_ptr), val);
                  cb(ec, std::move(val));
              });
}

void RedisConnection::AsyncLLen(std::string key, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("LLEN", std::move(key));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

// ═════════════════════════════════════════════════════════════════════════════
// Sorted Set — 异步回调
// ═════════════════════════════════════════════════════════════════════════════

void RedisConnection::AsyncZAdd(std::string key, double score,
                                std::string member, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("ZADD", std::move(key), std::to_string(score), std::move(member));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncZRange(std::string key, int64_t start, int64_t stop,
                                  bool with_scores, AsyncCbStrVec cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::vector<std::string>>>();
    if (with_scores)
        req->push("ZRANGE", std::move(key), std::to_string(start), std::to_string(stop), "WITHSCORES");
    else
        req->push("ZRANGE", std::move(key), std::to_string(start), std::to_string(stop));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, {}); return; }
                  std::vector<std::string> val;
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, std::move(val));
              });
}

void RedisConnection::AsyncZRevRange(std::string key, int64_t start, int64_t stop,
                                     bool with_scores, AsyncCbStrVec cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::vector<std::string>>>();
    if (with_scores)
        req->push("ZREVRANGE", std::move(key), std::to_string(start), std::to_string(stop), "WITHSCORES");
    else
        req->push("ZREVRANGE", std::move(key), std::to_string(start), std::to_string(stop));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, {}); return; }
                  std::vector<std::string> val;
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, std::move(val));
              });
}

void RedisConnection::AsyncZCard(std::string key, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("ZCARD", std::move(key));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncZRem(std::string key, std::string member, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("ZREM", std::move(key), std::move(member));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncZScore(std::string key, std::string member, AsyncCbDouble cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::optional<std::string>>>();
    req->push("ZSCORE", std::move(key), std::move(member));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, -1.0); return; }
                  std::string val;
                  if (ExtractOptionalValue(std::get<0>(*resp_ptr), val))
                      cb(ec, std::stod(val));
                  else
                      cb(ec, -1.0);
              });
}

void RedisConnection::AsyncZRank(std::string key, std::string member, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::optional<std::string>>>();
    req->push("ZRANK", std::move(key), std::move(member));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, -1); return; }
                  std::string val;
                  if (ExtractOptionalValue(std::get<0>(*resp_ptr), val))
                      cb(ec, static_cast<int64_t>(std::stoll(val)));
                  else
                      cb(ec, -1);
              });
}

void RedisConnection::AsyncZRevRank(std::string key, std::string member, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::optional<std::string>>>();
    req->push("ZREVRANK", std::move(key), std::move(member));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, -1); return; }
                  std::string val;
                  if (ExtractOptionalValue(std::get<0>(*resp_ptr), val))
                      cb(ec, static_cast<int64_t>(std::stoll(val)));
                  else
                      cb(ec, -1);
              });
}

void RedisConnection::AsyncZCount(std::string key, double min, double max, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("ZCOUNT", std::move(key), std::to_string(min), std::to_string(max));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncZIncrBy(std::string key, std::string member, double delta, AsyncCbDouble cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::string>>();
    req->push("ZINCRBY", std::move(key), std::to_string(delta), std::move(member));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0.0); return; }
                  std::string val;
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, std::stod(val));
              });
}

void RedisConnection::AsyncZRangeByScore(std::string key, double min, double max,
                                         bool with_scores, AsyncCbStrVec cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::vector<std::string>>>();
    if (with_scores)
        req->push("ZRANGEBYSCORE", std::move(key), std::to_string(min), std::to_string(max), "WITHSCORES");
    else
        req->push("ZRANGEBYSCORE", std::move(key), std::to_string(min), std::to_string(max));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, {}); return; }
                  std::vector<std::string> val;
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, std::move(val));
              });
}

void RedisConnection::AsyncZRevRangeByScore(std::string key, double min, double max,
                                            bool with_scores, AsyncCbStrVec cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::vector<std::string>>>();
    if (with_scores)
        req->push("ZREVRANGEBYSCORE", std::move(key), std::to_string(max), std::to_string(min), "WITHSCORES");
    else
        req->push("ZREVRANGEBYSCORE", std::move(key), std::to_string(max), std::to_string(min));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, {}); return; }
                  std::vector<std::string> val;
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, std::move(val));
              });
}

void RedisConnection::AsyncZRemRangeByRank(std::string key, int64_t start, int64_t stop, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("ZREMRANGEBYRANK", std::move(key), std::to_string(start), std::to_string(stop));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncZRemRangeByScore(std::string key, double min, double max, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("ZREMRANGEBYSCORE", std::move(key), std::to_string(min), std::to_string(max));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, 0); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncZRangeWithScores(std::string key, int64_t start, int64_t stop,
                                            AsyncCbPairs cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::vector<std::string>>>();
    req->push("ZRANGE", std::move(key), std::to_string(start), std::to_string(stop), "WITHSCORES");

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, {}); return; }
                  std::vector<std::string> flat;
                  ExtractValue(std::get<0>(*resp_ptr), flat);
                  std::vector<std::pair<std::string, double>> pairs;
                  pairs.reserve(flat.size() / 2);
                  for (size_t i = 0; i + 1 < flat.size(); i += 2)
                      pairs.emplace_back(std::move(flat[i]), std::stod(flat[i + 1]));
                  cb(ec, std::move(pairs));
              });
}

void RedisConnection::AsyncZRevRangeWithScores(std::string key, int64_t start, int64_t stop,
                                               AsyncCbPairs cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::vector<std::string>>>();
    req->push("ZREVRANGE", std::move(key), std::to_string(start), std::to_string(stop), "WITHSCORES");

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, {}); return; }
                  std::vector<std::string> flat;
                  ExtractValue(std::get<0>(*resp_ptr), flat);
                  std::vector<std::pair<std::string, double>> pairs;
                  pairs.reserve(flat.size() / 2);
                  for (size_t i = 0; i + 1 < flat.size(); i += 2)
                      pairs.emplace_back(std::move(flat[i]), std::stod(flat[i + 1]));
                  cb(ec, std::move(pairs));
              });
}

// ═════════════════════════════════════════════════════════════════════════════
// Key 管理 — 异步回调
// ═════════════════════════════════════════════════════════════════════════════

void RedisConnection::AsyncExpire(std::string key, int64_t seconds, AsyncCbBool cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("EXPIRE", std::move(key), std::to_string(seconds));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, false); return; }
                  long long val{};
                  cb(ec, ExtractValue(std::get<0>(*resp_ptr), val) && val > 0);
              });
}

void RedisConnection::AsyncTTL(std::string key, AsyncCbInt cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<long long>>();
    req->push("TTL", std::move(key));

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec) { cb(ec, -2); return; }
                  long long val{};
                  ExtractValue(std::get<0>(*resp_ptr), val);
                  cb(ec, static_cast<int64_t>(val));
              });
}

void RedisConnection::AsyncPing(AsyncCbBool cb)
{
    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<std::string>>();
    req->push("PING");
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb)](boost::system::error_code ec, auto) {
                  cb(ec, !ec);
              });
}

// ═════════════════════════════════════════════════════════════════════════════
// 泛型命令 — 异步回调
// ═════════════════════════════════════════════════════════════════════════════

void RedisConnection::AsyncCall(const std::string& cmd,
                                const std::vector<std::string>& args,
                                AsyncCbGeneric cb)
{
    using VecResp = std::vector<boost::redis::resp3::node>;

    auto req  = std::make_shared<boost::redis::request>();
    auto resp = std::make_shared<boost::redis::response<VecResp>>();
    req->push_range(cmd, args);

    auto resp_ptr = resp;
    AsyncExec(std::move(req), std::move(resp),
              [cb = std::move(cb), resp_ptr](boost::system::error_code ec, auto) {
                  if (ec)
                  {
                      cb(ec, {});
                      return;
                  }
                  cb(ec, std::get<0>(*resp_ptr));
              });
}

void RedisConnection::AsyncEval(const std::string& script,
                                const std::vector<std::string>& keys,
                                const std::vector<std::string>& args,
                                AsyncCbGeneric cb)
{
    std::vector<std::string> eval_args;
    eval_args.reserve(1 + 1 + keys.size() + args.size());
    eval_args.push_back(script);
    eval_args.push_back(std::to_string(keys.size()));
    eval_args.insert(eval_args.end(), keys.begin(), keys.end());
    eval_args.insert(eval_args.end(), args.begin(), args.end());
    AsyncCall("EVAL", eval_args, std::move(cb));
}

// ═════════════════════════════════════════════════════════════════════════════
// KV — 协程接口
// ═════════════════════════════════════════════════════════════════════════════

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

// ═════════════════════════════════════════════════════════════════════════════
// Hash — 协程接口
// ═════════════════════════════════════════════════════════════════════════════

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

// ═════════════════════════════════════════════════════════════════════════════
// List — 协程接口
// ═════════════════════════════════════════════════════════════════════════════

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

// ═════════════════════════════════════════════════════════════════════════════
// Sorted Set — 协程接口
// ═════════════════════════════════════════════════════════════════════════════

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

// ═════════════════════════════════════════════════════════════════════════════
// Key 管理 — 协程接口
// ═════════════════════════════════════════════════════════════════════════════

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
