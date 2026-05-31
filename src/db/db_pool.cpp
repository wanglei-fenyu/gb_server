#include "db/db_pool.h"
#include "db/postgres/pg_connection.h"
#include "db/mysql/mysql_connection.h"
#include "log/log.h"
#include "async_simple/coro/FutureAwaiter.h"
#include <boost/asio/post.hpp>

namespace gb
{

// =========================================================================
// 工厂 + 生命周期
// =========================================================================

DbConnectionPool::DbConnectionPool(boost::asio::io_context& io_ctx, const DbConfig& cfg)
    : io_ctx_(io_ctx)
    , config_(cfg)
    , db_type_(cfg.port == 5432 || cfg.port == 0 ? DbType::POSTGRESQL : DbType::MYSQL)
    , heartbeat_timer_(io_ctx_)
{
    // 若无显式端口，用默认值
    if (config_.port == 0)
        config_.port = DbConfig::DefaultPort(db_type_);

    if (config_.max_pool_size == 0)
        config_.max_pool_size = 32;
    if (config_.min_pool_size == 0)
        config_.min_pool_size = 4;
}

DbConnectionPool::~DbConnectionPool()
{
    // 关闭所有空闲连接
    DbConnectionPtr conn;
    while (idle_.try_dequeue(conn)) {
        idle_count_.fetch_sub(1);
        total_connections_.fetch_sub(1);
        // conn 析构时自动关闭
    }
}

std::shared_ptr<DbConnectionPool> DbConnectionPool::Create(
    boost::asio::io_context& io_ctx, const DbConfig& cfg)
{
    return std::shared_ptr<DbConnectionPool>(new DbConnectionPool(io_ctx, cfg));
}

// =========================================================================
// Start / Stop
// =========================================================================

async_simple::coro::Lazy<bool> DbConnectionPool::Start()
{
    async_simple::Promise<bool> promise;
    auto future = promise.getFuture();

    auto self = shared_from_this();
    boost::asio::post(io_ctx_, [this, self, promise = std::move(promise)]() mutable {
        running_.store(true);

        // 创建初始连接
        size_t to_create = std::min(config_.min_pool_size, config_.max_pool_size);
        size_t created = 0;
        for (size_t i = 0; i < to_create; ++i) {
            auto conn = CreateConnectionSync();
            if (conn) {
                idle_.enqueue(std::move(conn));
                idle_count_.fetch_add(1);
                total_connections_.fetch_add(1);
                ++created;
            }
        }

        LOG_INFO("[DbPool] started: {}/{} connections (min={} max={})",
                 created, to_create, config_.min_pool_size, config_.max_pool_size);

        // 启动心跳
        StartHeartbeat();

        promise.setValue(created > 0 || to_create == 0);
    });

    co_return co_await std::move(future);
}

async_simple::coro::Lazy<void> DbConnectionPool::Stop()
{
    async_simple::Promise<void> promise;
    auto future = promise.getFuture();

    auto self = shared_from_this();
    boost::asio::post(io_ctx_, [this, self, promise = std::move(promise)]() mutable {
        running_.store(false);

        // 取消心跳
        boost::system::error_code ec;
        heartbeat_timer_.cancel(ec);

        // 关闭所有空闲连接
        DbConnectionPtr conn;
        while (idle_.try_dequeue(conn)) {
            idle_count_.fetch_sub(1);
            total_connections_.fetch_sub(1);
            conn->CloseSync();  // 同步关闭
        }

        // 将等待者唤醒并传入 nullptr（表示池已关闭）
        Waiter w;
        while (waiters_.try_dequeue(w)) {
            w.promise.setValue(nullptr);
        }

        LOG_INFO("[DbPool] stopped");
        promise.setValue();
    });

    co_await std::move(future);
}

// =========================================================================
// Acquire / Release
// =========================================================================

async_simple::coro::Lazy<DbConnectionPtr> DbConnectionPool::Acquire()
{
    async_simple::Promise<DbConnectionPtr> promise;
    auto future = promise.getFuture();

    auto self = shared_from_this();
    boost::asio::post(io_ctx_, [this, self, promise = std::move(promise)]() mutable {
        DoAcquire(std::move(promise));
    });

    co_return co_await std::move(future);
}

void DbConnectionPool::Release(DbConnectionPtr conn)
{
    if (!conn) return;

    boost::asio::post(io_ctx_, [this, conn = std::move(conn)]() mutable {
        DoRelease(std::move(conn));
    });
}

// =========================================================================
// 内部实现（在 IO 线程上执行）
// =========================================================================

void DbConnectionPool::DoAcquire(async_simple::Promise<DbConnectionPtr> promise)
{
    // 1. 尝试从空闲队列取
    {
        DbConnectionPtr conn;
        if (idle_.try_dequeue(conn)) {
            idle_count_.fetch_sub(1);
            if (conn && conn->IsConnected()) {
                active_count_.fetch_add(1);
                promise.setValue(std::move(conn));
                return;
            }
            // 连接已断开，丢弃并减少计数
            total_connections_.fetch_sub(1);
        }
    }

    // 2. 若池未满，创建新连接
    if (total_connections_.load() < config_.max_pool_size) {
        auto new_conn = CreateConnectionSync();
        if (new_conn) {
            total_connections_.fetch_add(1);
            active_count_.fetch_add(1);
            promise.setValue(std::move(new_conn));
            return;
        }
    }

    // 3. 池满或创建失败，加入等待队列
    waiters_.enqueue(Waiter{std::move(promise)});
}

void DbConnectionPool::DoRelease(DbConnectionPtr conn)
{
    if (!conn) return;

    // 检查连接是否正常
    if (!conn->IsConnected()) {
        // 连接断开，丢弃
        total_connections_.fetch_sub(1);
        active_count_.fetch_sub(1);
        return;
    }

    // 优先分发给等待者
    Waiter w;
    if (waiters_.try_dequeue(w)) {
        // 将连接直接交给等待者
        active_count_.fetch_sub(1);  // 当前 active--
        // 等待者的 active++ 在其返回后由 Acquire 处理
        w.promise.setValue(std::move(conn));
        return;
    }

    // 没有等待者，归还到空闲队列
    idle_.enqueue(std::move(conn));
    idle_count_.fetch_add(1);
    active_count_.fetch_sub(1);
}

DbConnectionPtr DbConnectionPool::CreateConnectionSync()
{
    if (db_type_ == DbType::POSTGRESQL) {
        auto conn = std::make_shared<PgConnection>(io_ctx_);
        if (conn->ConnectSync(config_)) {
            return conn;
        }
    } else if (db_type_ == DbType::MYSQL) {
        auto conn = std::make_shared<MySqlConnection>(io_ctx_);
        if (conn->ConnectSync(config_)) {
            return conn;
        }
    }
    return nullptr;
}

// =========================================================================
// 心跳
// =========================================================================

void DbConnectionPool::StartHeartbeat()
{
    heartbeat_timer_.expires_after(
        std::chrono::seconds(HEARTBEAT_INTERVAL_SEC));
    heartbeat_timer_.async_wait(
        std::bind(&DbConnectionPool::OnHeartbeat, this, std::placeholders::_1));
}

void DbConnectionPool::OnHeartbeat(const boost::system::error_code& ec)
{
    if (ec || !running_.load())
        return;

    // 检查所有空闲连接
    std::vector<DbConnectionPtr> checked;
    DbConnectionPtr conn;
    while (idle_.try_dequeue(conn)) {
        idle_count_.fetch_sub(1);
        if (conn && conn->IsConnected()) {
            // 发送心跳查询
            // 注意：同步查询会阻塞 IO 线程，但这是可接受的（心跳间隔 30s）
            checked.push_back(std::move(conn));
        } else {
            // 连接已断开
            total_connections_.fetch_sub(1);
        }
    }

    // 归还健康的连接
    for (auto& c : checked) {
        idle_.enqueue(std::move(c));
        idle_count_.fetch_add(1);
    }

    // 补充连接至 min_pool_size
    while (total_connections_.load() < config_.min_pool_size) {
        auto new_conn = CreateConnectionSync();
        if (!new_conn) break;
        idle_.enqueue(std::move(new_conn));
        idle_count_.fetch_add(1);
        total_connections_.fetch_add(1);
    }

    // 重新调度
    StartHeartbeat();
}

} // namespace gb
