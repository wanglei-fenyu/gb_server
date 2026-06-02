#pragma once
#include "db/db_connection.h"
#include "db/db_config.h"
#include "db/db_result.h"
#include "async_simple/Future.h"
#include "async_simple/Promise.h"
#include <libpq-fe.h>
#include <boost/asio/io_context.hpp>
#include <functional>
#include <memory>
#include <string>

namespace gb
{

/// PostgreSQL 异步连接（libpq 非阻塞模式 + Boost.Asio reactor）。
///
/// 线程安全说明：
///   - 所有异步方法（Connect/Query/Execute）必须在调用者协程上执行，
///     内部会切换到 IO 线程执行 libpq 操作。
///   - 析构函数线程安全。
///
class PgConnection final : public DbConnection
{
public:
    explicit PgConnection(boost::asio::io_context& io_ctx);
    ~PgConnection() override;

    // 禁用拷贝
    PgConnection(const PgConnection&) = delete;
    PgConnection& operator=(const PgConnection&) = delete;

    // ── DbConnection 接口 ─────────────────────────────────────────────────

    DbType type() const noexcept override { return DbType::POSTGRESQL; }

    async_simple::coro::Lazy<bool>          Connect(const DbConfig& cfg) override;
    async_simple::coro::Lazy<void>          Close() override;
    bool                                    IsConnected() const override;
    async_simple::coro::Lazy<bool>          Reset() override;

    // ── 同步接口（供连接池在 IO 线程上使用） ──────────────────────────────

    /// 异步连接（回调在 PG IO 线程执行）。
    void AsyncConnect(const DbConfig& cfg, std::function<void(bool)> cb);

    /// 异步查询（无参数，回调在 PG IO 线程执行）。
    void AsyncQuery(const std::string& sql, std::function<void(DbResult)> cb);

    /// 异步参数化查询（回调在 PG IO 线程执行）。
    void AsyncQuery(const std::string& sql, const std::vector<DbValue>& params,
                    std::function<void(DbResult)> cb);

    /// 异步 DML（回调返回影响行数，回调在 PG IO 线程执行）。
    void AsyncExecute(const std::string& sql, std::function<void(uint64_t)> cb);

    /// 异步事务命令（回调在 PG IO 线程执行）。
    void AsyncBegin(std::function<void(bool)> cb);
    void AsyncCommit(std::function<void(bool)> cb);
    void AsyncRollback(std::function<void(bool)> cb);

    /// 异步关闭（回调在 PG IO 线程执行）。
    void AsyncClose(std::function<void()> cb);

    /// 同步关闭（线程安全）。
    void CloseSync();

    /// 同步连接（在 IO 线程上直接阻塞调用，不经过 Promise/Future）。
    bool ConnectSync(const DbConfig& cfg);

    async_simple::coro::Lazy<DbResult>      Query(std::string_view sql) override;
    async_simple::coro::Lazy<DbResult>      Query(
        std::string_view sql, const std::vector<DbValue>& params) override;
    async_simple::coro::Lazy<uint64_t>      Execute(std::string_view sql) override;

    async_simple::coro::Lazy<void>          Begin() override;
    async_simple::coro::Lazy<void>          Commit() override;
    async_simple::coro::Lazy<void>          Rollback() override;

private:
    // ── 同步操作（在 IO 线程上执行） ──────────────────────────────────────

    /// 同步查询。
    DbResult SyncQuery(const char* sql);

    /// 参数化同步查询。
    DbResult SyncExecParams(const char* sql, const std::vector<DbValue>& params);

    /// 同步命令（BEGIN/COMMIT/ROLLBACK）。
    bool SyncCommand(const char* sql);

    /// 构造 libpq 连接串。
    static std::string BuildConnString(const DbConfig& cfg);

    /// 将 PGresult 解析为 DbResult。
    static DbResult ParseResult(PGresult* res);
    /// 将 libpq 文本值转为 DbValue。
    static DbValue ToDbValue(const char* value, Oid type_oid);

private:
    boost::asio::io_context&           io_ctx_;
    PGconn*                            conn_        = nullptr;
    DbConfig                           config_;
    bool                               connected_   = false;
};

} // namespace gb
