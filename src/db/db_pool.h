#pragma once
#include "db/db_connection.h"
#include "db/db_config.h"
#include "async_simple/coro/Lazy.h"
#include "async_simple/Future.h"
#include "async_simple/Promise.h"
#include <concurrentqueue.h>
#include <boost/asio/steady_timer.hpp>
#include <memory>
#include <vector>

namespace gb
{

/// 异步数据库连接池。
///
/// 用法：
///   auto pool = DbConnectionPool::Create(io_ctx, cfg);
///   co_await pool->Start();
///   auto conn = co_await pool->Acquire();
///   auto res  = co_await conn->Query("SELECT 1");
///   pool->Release(std::move(conn));
///   // ── 程序退出 ──
///   co_await pool->Stop();
///
/// 连接池运行在自己的 IO 线程上，所有 libpq/MySQL 操作在该线程同步执行，
/// 通过 async_simple::Promise/Future 将结果桥接回调用者协程。
///
class DbConnectionPool : public std::enable_shared_from_this<DbConnectionPool>
{
public:
    /// 工厂方法。
    /// @param io_ctx  连接池使用的 IO 上下文（应独占一个线程）。
    /// @param cfg     连接配置（含连接池大小参数）。
    static std::shared_ptr<DbConnectionPool> Create(
        boost::asio::io_context& io_ctx, const DbConfig& cfg);

    ~DbConnectionPool();

    // 禁用拷贝/移动
    DbConnectionPool(const DbConnectionPool&) = delete;
    DbConnectionPool& operator=(const DbConnectionPool&) = delete;

    // ── 生命周期 ───────────────────────────────────────────────────────────

    /// 启动连接池，创建初始连接。
    async_simple::coro::Lazy<bool> Start();

    /// 停止连接池，关闭所有连接。
    async_simple::coro::Lazy<void> Stop();

    // ── 连接获取/归还 ──────────────────────────────────────────────────────

    /// 从池中获取一个连接。若池为空则等待。
    async_simple::coro::Lazy<DbConnectionPtr> Acquire();

    /// 归还连接。连接如果已断开会被丢弃。
    void Release(DbConnectionPtr conn);

    // ── 状态 ───────────────────────────────────────────────────────────────

    size_t IdleCount()      const noexcept { return idle_count_.load(); }
    size_t ActiveCount()    const noexcept { return active_count_.load(); }
    size_t TotalCount()     const noexcept { return total_connections_.load(); }
    bool   IsRunning()      const noexcept { return running_.load(); }

private:
    DbConnectionPool(boost::asio::io_context& io_ctx, const DbConfig& cfg);

    // ── 内部（所有方法在 IO 线程上串行执行） ──────────────────────────────

    /// 获取连接的原子操作（在 IO 线程上执行）。
    void DoAcquire(async_simple::Promise<DbConnectionPtr> promise);

    /// 归还连接的原子操作（在 IO 线程上执行）。
    void DoRelease(DbConnectionPtr conn);

    /// 创建新连接（同步，在 IO 线程上阻塞）。
    DbConnectionPtr CreateConnectionSync();

    /// 心跳检查。
    void StartHeartbeat();
    void OnHeartbeat(const boost::system::error_code& ec);

private:
    boost::asio::io_context&     io_ctx_;

    DbConfig                     config_;
    DbType                       db_type_;

    // ── 连接池状态 ──
    moodycamel::ConcurrentQueue<DbConnectionPtr> idle_;
    std::atomic<size_t>          total_connections_{0};
    std::atomic<size_t>          idle_count_{0};
    std::atomic<size_t>          active_count_{0};
    std::atomic<bool>            running_{false};

    // ── 等待者队列 ──
    struct Waiter {
        async_simple::Promise<DbConnectionPtr> promise;
    };
    moodycamel::ConcurrentQueue<Waiter> waiters_;

    // ── 心跳 ──
    boost::asio::steady_timer    heartbeat_timer_;
    static constexpr int         HEARTBEAT_INTERVAL_SEC = 30;
};

/// DbConnectionPool 智能指针。
using DbConnectionPoolPtr = std::shared_ptr<DbConnectionPool>;

} // namespace gb
